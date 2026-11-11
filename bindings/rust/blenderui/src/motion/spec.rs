//! An element's Motion props ([`Spec`]): `initial`, `animate`, `exit`, the
//! `while*` targets, `transition`, `variants`, `style`, as `Div` builder
//! methods. Only the props given are stored, in place.

use std::borrow::Cow;

use smallvec::SmallVec;

use super::vars::{Transition, Vars};
use crate::element::Div;

/// What an element animates to in one state: values, a variant's name, or
/// nothing (`initial={false}`: start at the `animate` values, no animation).
#[derive(Debug)]
pub enum Target {
    Vars(Vars),
    Variant(Cow<'static, str>),
    Off,
}

impl From<Vars> for Target {
    fn from(v: Vars) -> Target {
        Target::Vars(v)
    }
}

impl From<&'static str> for Target {
    fn from(s: &'static str) -> Target {
        Target::Variant(Cow::Borrowed(s))
    }
}

impl From<String> for Target {
    fn from(s: String) -> Target {
        Target::Variant(Cow::Owned(s))
    }
}

impl From<bool> for Target {
    /// `false`: no initial animation (`true`: the default).
    fn from(on: bool) -> Target {
        if on { Target::Variant(Cow::Borrowed("")) } else { Target::Off }
    }
}

/// Named targets (Motion's `variants`): usually a `const` (no allocation).
#[derive(Debug)]
pub enum Variants {
    Static(&'static [(&'static str, Vars)]),
    Owned(Vec<(&'static str, Vars)>),
}

impl std::ops::Deref for Variants {
    type Target = [(&'static str, Vars)];
    fn deref(&self) -> &Self::Target {
        match self {
            Variants::Static(v) => v,
            Variants::Owned(v) => v,
        }
    }
}

impl From<&'static [(&'static str, Vars)]> for Variants {
    fn from(v: &'static [(&'static str, Vars)]) -> Variants {
        Variants::Static(v)
    }
}

impl<const N: usize> From<&'static [(&'static str, Vars); N]> for Variants {
    fn from(v: &'static [(&'static str, Vars); N]) -> Variants {
        Variants::Static(v)
    }
}

impl From<Vec<(&'static str, Vars)>> for Variants {
    fn from(v: Vec<(&'static str, Vars)>) -> Variants {
        Variants::Owned(v)
    }
}

impl<const N: usize> From<[(&'static str, Vars); N]> for Variants {
    fn from(v: [(&'static str, Vars); N]) -> Variants {
        Variants::Owned(v.into())
    }
}

/// Which of Motion's target props.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum When {
    Initial,
    Animate,
    Exit,
    WhileHover,
    WhileTap,
    WhileFocus,
}

#[derive(Debug)]
enum Entry {
    Target(When, Target),
    Transition(Vars),
    Style(Vars),
    Variants(Variants),
}

impl Entry {
    /// Entries of one kind replace each other.
    fn same(&self, o: &Entry) -> bool {
        match (self, o) {
            (Entry::Target(a, _), Entry::Target(b, _)) => a == b,
            _ => std::mem::discriminant(self) == std::mem::discriminant(o),
        }
    }
}

/// An element's motion (Motion's props): only the ones given, in place (an
/// element with `animate` alone stores just that, with no allocation of its own).
#[derive(Debug, Default)]
pub struct Spec(SmallVec<[Entry; 1]>);

impl Spec {
    fn set(&mut self, e: Entry) {
        match self.0.iter_mut().find(|x| x.same(&e)) {
            Some(x) => *x = e,
            None => self.0.push(e),
        }
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// The target given for `w`.
    #[must_use]
    pub fn target(&self, w: When) -> Option<&Target> {
        self.0.iter().find_map(|e| match e {
            Entry::Target(x, t) if *x == w => Some(t),
            _ => None,
        })
    }

    /// The element's transition (with Motion's per-property ones).
    #[must_use]
    pub fn transition(&self) -> Option<&Vars> {
        self.0.iter().find_map(|e| if let Entry::Transition(v) = e { Some(v) } else { None })
    }

    /// `style={{ x: mv }}`: values read every frame.
    #[must_use]
    pub fn style(&self) -> Option<&Vars> {
        self.0.iter().find_map(|e| if let Entry::Style(v) = e { Some(v) } else { None })
    }

    fn variants(&self) -> Option<&Variants> {
        self.0.iter().find_map(|e| if let Entry::Variants(v) = e { Some(v) } else { None })
    }

    /// The values of target `w` (a variant's, by name).
    pub(super) fn vars(&self, w: When) -> Option<&Vars> {
        match self.target(w)? {
            Target::Vars(v) => Some(v),
            Target::Variant(name) => self.variants()?.iter().find(|(n, _)| *n == name).map(|(_, v)| v),
            Target::Off => None,
        }
    }
}

macro_rules! spec_setters {
    ($($(#[$doc:meta])* $method:ident => $when:ident),*) => {
        impl Div {$(
            $(#[$doc])*
            #[must_use]
            pub fn $method(mut self, target: impl Into<Target>) -> Div {
                self.motion_mut().set(Entry::Target(When::$when, target.into()));
                self
            }
        )*}
    };
}
spec_setters!(
    /// Where it starts when it appears (`false`: at its `animate` values).
    initial => Initial,
    /// What it animates to; changing it animates again.
    animate => Animate,
    /// What it animates to when it goes away (in an `AnimatePresence`).
    exit => Exit,
    /// On top of `animate` while the pointer is over it.
    while_hover => WhileHover,
    /// On top while it is pressed.
    while_tap => WhileTap,
    /// On top while it has focus.
    while_focus => WhileFocus
);

impl Div {
    fn motion_mut(&mut self) -> &mut Spec {
        &mut self.rare_mut().motion
    }

    /// The default transition of its motions (Motion's `transition`:
    /// `{{ type: "spring", x: { duration: 1 } }}`).
    #[must_use]
    pub fn transition(mut self, t: impl Into<Vars>) -> Div {
        self.motion_mut().set(Entry::Transition(t.into()));
        self
    }

    /// Values set every frame, usually [`MotionValue`](super::MotionValue)s
    /// (Motion's `style={{ x: mv }}`).
    #[must_use]
    pub fn motion_style(mut self, v: Vars) -> Div {
        self.motion_mut().set(Entry::Style(v));
        self
    }

    /// Named targets: `animate="visible"` picks one.
    #[must_use]
    pub fn variants(mut self, v: impl Into<Variants>) -> Div {
        self.motion_mut().set(Entry::Variants(v.into()));
        self
    }
}

impl From<Transition> for Vars {
    fn from(t: Transition) -> Vars {
        Vars { transition: t, ..Vars::default() }
    }
}
