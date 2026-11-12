//! An element's Motion props ([`Spec`]): `initial`, `animate`, `exit`, the
//! `while*` targets, `transition`, `variants`, `style`, as `Div` builder
//! methods. Only the props given are stored, in place.

use std::borrow::Cow;

use smallvec::SmallVec;

use super::value::Value;
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
    fn from(v: Vars) -> Self {
        Self::Vars(v)
    }
}

impl From<&'static str> for Target {
    fn from(s: &'static str) -> Self {
        Self::Variant(Cow::Borrowed(s))
    }
}

impl From<String> for Target {
    fn from(s: String) -> Self {
        Self::Variant(Cow::Owned(s))
    }
}

impl From<bool> for Target {
    /// `false`: no initial animation (`true`: the default).
    fn from(on: bool) -> Self {
        if on { Self::Variant(Cow::Borrowed("")) } else { Self::Off }
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
            Self::Static(v) => v,
            Self::Owned(v) => v,
        }
    }
}

impl From<&'static [(&'static str, Vars)]> for Variants {
    fn from(v: &'static [(&'static str, Vars)]) -> Self {
        Self::Static(v)
    }
}

impl<const N: usize> From<&'static [(&'static str, Vars); N]> for Variants {
    fn from(v: &'static [(&'static str, Vars); N]) -> Self {
        Self::Static(v)
    }
}

impl From<Vec<(&'static str, Vars)>> for Variants {
    fn from(v: Vec<(&'static str, Vars)>) -> Self {
        Self::Owned(v)
    }
}

impl<const N: usize> From<[(&'static str, Vars); N]> for Variants {
    fn from(v: [(&'static str, Vars); N]) -> Self {
        Self::Owned(v.into())
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
    WhileInView,
    WhileDrag,
}

/// `viewport={{ once: true, amount: 0.5 }}`: when an element counts as in
/// view (`amount`: the part of it showing, 0 = any, 1 = all; `once`: it stays
/// in view after the first time).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct InView {
    pub once: bool,
    pub amount: f32,
}

impl From<Vars> for InView {
    fn from(v: Vars) -> Self {
        let mut r = Self::default();
        for (k, val) in v.props.iter() {
            match k.name().to_bytes() {
                b"once" => r.once = val.read().is_some_and(|x| x.v[0] != 0.0),
                b"amount" => {
                    r.amount = match val {
                        Value::Text(t) if t.to_bytes() == b"all" => 1.0,
                        Value::Text(t) if t.to_bytes() == b"some" => 0.0,
                        v => v.read().map_or(0.0, |x| x.v[0].clamp(0.0, 1.0)),
                    };
                }
                _ => {}
            }
        }
        r
    }
}

/// Motion's `drag`: on which axes, within what (pixels from where it
/// started: left, right, top, bottom), how far past it with what give
/// (`elastic`, 0 .. 1), and whether it carries on when let go (`momentum`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Drag {
    pub x: bool,
    pub y: bool,
    pub constraints: Option<[f32; 4]>,
    pub elastic: f32,
    pub momentum: bool,
    pub snap_to_origin: bool,
}

impl Default for Drag {
    fn default() -> Self {
        Self { x: true, y: true, constraints: None, elastic: 0.5, momentum: true, snap_to_origin: false }
    }
}

/// `drag` / `drag="x"` / `drag="y"`.
impl From<bool> for Drag {
    fn from(on: bool) -> Self {
        Self { x: on, y: on, ..Self::default() }
    }
}

impl From<&str> for Drag {
    fn from(axis: &str) -> Self {
        Self { x: axis != "y", y: axis != "x", ..Self::default() }
    }
}

/// A drag's bounds: left, right, top, bottom (pixels; a side left out: none).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DragConstraints(pub [f32; 4]);

impl From<[f32; 4]> for DragConstraints {
    fn from(c: [f32; 4]) -> Self {
        Self(c)
    }
}

impl From<Vars> for DragConstraints {
    fn from(v: Vars) -> Self {
        let mut c = [f32::NEG_INFINITY, f32::INFINITY, f32::NEG_INFINITY, f32::INFINITY];
        for (k, val) in v.props.iter() {
            let i = match k.name().to_bytes() {
                b"left" => 0,
                b"right" => 1,
                b"top" => 2,
                b"bottom" => 3,
                _ => continue,
            };
            if let Some(r) = val.read() {
                c[i] = r.v[0];
            }
        }
        Self(c)
    }
}

/// Motion's `layout`: what of a change of its box animates (`true`: where
/// and how big; `"position"`; `"size"`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Layout {
    Both,
    Position,
    Size,
}

impl From<bool> for Layout {
    fn from(_: bool) -> Self {
        Self::Both
    }
}

impl From<&str> for Layout {
    fn from(s: &str) -> Self {
        match s {
            "position" => Self::Position,
            "size" => Self::Size,
            _ => Self::Both,
        }
    }
}

#[derive(Debug)]
enum Entry {
    Target(When, Target),
    Transition(Vars),
    Style(Vars),
    Variants(Variants),
    Viewport(InView),
    Drag(Drag),
    Layout(Layout),
    /// built while its `AnimatePresence` takes it away: `exit` is the target
    Exiting(u64),
}

impl Entry {
    /// Entries of one kind replace each other.
    fn same(&self, o: &Self) -> bool {
        match (self, o) {
            (Self::Target(a, _), Self::Target(b, _)) => a == b,
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

    /// When it counts as in view (`whileInView`).
    #[must_use]
    pub fn viewport(&self) -> InView {
        self.0.iter().find_map(|e| if let Entry::Viewport(v) = e { Some(*v) } else { None }).unwrap_or_default()
    }

    /// How it drags, if it does.
    #[must_use]
    pub fn drag(&self) -> Option<Drag> {
        self.0.iter().find_map(|e| if let Entry::Drag(d) = e { Some(*d) } else { None })
    }

    /// Whether changes of its box animate.
    #[must_use]
    pub fn layout(&self) -> Option<Layout> {
        self.0.iter().find_map(|e| if let Entry::Layout(l) = e { Some(*l) } else { None })
    }

    /// The `AnimatePresence` taking it away, if one is.
    pub(super) fn exiting(&self) -> Option<u64> {
        self.0.iter().find_map(|e| if let Entry::Exiting(id) = e { Some(*id) } else { None })
    }

    fn drag_mut(&mut self) -> &mut Drag {
        if self.drag().is_none() {
            self.0.push(Entry::Drag(Drag::default()));
        }
        self.0.iter_mut().find_map(|e| if let Entry::Drag(d) = e { Some(d) } else { None }).expect("drag")
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
    /// On top of `animate` while the pointer is over it.
    while_hover => WhileHover,
    /// On top while it is pressed.
    while_tap => WhileTap,
    /// On top while it has focus.
    while_focus => WhileFocus,
    /// On top while it is in view (see [`viewport`](Div::viewport)).
    while_in_view => WhileInView,
    /// On top while it is dragged.
    while_drag => WhileDrag
);

impl Div {
    fn motion_mut(&mut self) -> &mut Spec {
        &mut self.rare_mut().motion
    }

    /// The default transition of its motions (Motion's `transition`:
    /// `{{ type: "spring", x: { duration: 1 } }}`).
    #[must_use]
    pub fn transition(mut self, t: impl Into<Vars>) -> Self {
        self.motion_mut().set(Entry::Transition(t.into()));
        self
    }

    /// Values set every frame, usually [`MotionValue`](super::MotionValue)s
    /// (Motion's `style={{ x: mv }}`).
    #[must_use]
    pub fn motion_style(mut self, v: Vars) -> Self {
        self.motion_mut().set(Entry::Style(v));
        self
    }

    /// What it animates to when it goes away (in a [`presence`](super::presence)):
    /// while a presence takes it away, this is its only target.
    #[must_use]
    pub fn exit(mut self, target: impl Into<Target>) -> Self {
        let m = self.motion_mut();
        m.set(Entry::Target(When::Exit, target.into()));
        if let Some(id) = super::presence::exiting() {
            m.set(Entry::Exiting(id));
        }
        self
    }

    /// When it counts as in view: `{{ once: true, amount: 0.5 }}`.
    #[must_use]
    pub fn viewport(mut self, v: impl Into<InView>) -> Self {
        self.motion_mut().set(Entry::Viewport(v.into()));
        self
    }

    /// Drag it with the pointer: `true`, `"x"` or `"y"`.
    #[must_use]
    pub fn drag(mut self, d: impl Into<Drag>) -> Self {
        let d = d.into();
        let m = self.motion_mut().drag_mut();
        (m.x, m.y) = (d.x, d.y);
        self
    }

    /// Where a drag may go, in pixels from where it started: `{{ left: -100,
    /// right: 100, top: 0, bottom: 0 }}` or `[left, right, top, bottom]`
    /// (past them, `drag_elastic` gives).
    #[must_use]
    pub fn drag_constraints(mut self, c: impl Into<DragConstraints>) -> Self {
        self.motion_mut().drag_mut().constraints = Some(c.into().0);
        self
    }

    /// How far past its constraints a drag goes (0: none, 1: freely; 0.5).
    #[must_use]
    pub fn drag_elastic(mut self, e: f32) -> Self {
        self.motion_mut().drag_mut().elastic = e.clamp(0.0, 1.0);
        self
    }

    /// Whether it carries on (inertia) when let go (true).
    #[must_use]
    pub fn drag_momentum(mut self, on: bool) -> Self {
        self.motion_mut().drag_mut().momentum = on;
        self
    }

    /// Whether it springs back to where it started when let go.
    #[must_use]
    pub fn drag_snap_to_origin(mut self, on: bool) -> Self {
        self.motion_mut().drag_mut().snap_to_origin = on;
        self
    }

    /// Animate changes of its box (Motion's `layout`): `true`, `"position"`, `"size"`.
    #[must_use]
    pub fn layout(mut self, l: impl Into<Layout>) -> Self {
        self.motion_mut().set(Entry::Layout(l.into()));
        self
    }

    /// Named targets: `animate="visible"` picks one.
    #[must_use]
    pub fn variants(mut self, v: impl Into<Variants>) -> Self {
        self.motion_mut().set(Entry::Variants(v.into()));
        self
    }
}

impl From<Transition> for Vars {
    fn from(t: Transition) -> Self {
        Self { transition: t, ..Self::default() }
    }
}
