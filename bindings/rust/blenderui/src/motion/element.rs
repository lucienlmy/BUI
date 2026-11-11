//! Motion on elements: `<motion.div initial={{..}} animate={{..}} whileHover={{..}}>`.
//!
//! An element's [`Spec`] says what it animates to; the window keeps, per
//! element key, a [`Slot`] the engine writes the moving values into (the
//! slot is the engine's target object). Each layout compares the element's
//! targets (animate, then whileFocus, whileHover, whileTap on top) with the
//! last ones and starts a motion for each property that changed, from where
//! it is (a spring takes the velocity it had). The values are then laid over
//! the element's computed style, like CSS animations (`anim.rs`).

use std::borrow::Cow;
use std::ffi::c_void;

use smallvec::SmallVec;

use blenderui_sys as sys;

use super::value::{Key, Read, Value};
use super::vars::{Flavor, Transition, Vars, tween};
use crate::anim::{Field, field_numbers, fields_of, set_field_numbers};
use crate::element::Div;
use crate::layout::ElData;
use crate::geometry::{Length, Units};
use crate::style::{Computed, KeyMap};

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

/// An element's motion (Motion's props).
#[derive(Debug, Default)]
pub struct Spec {
    pub initial: Option<Target>,
    pub animate: Option<Target>,
    pub exit: Option<Target>,
    pub while_hover: Option<Target>,
    pub while_tap: Option<Target>,
    pub while_focus: Option<Target>,
    /// the element's transition, with Motion's per-property ones
    pub transition: Option<Vars>,
    pub variants: Option<Variants>,
    /// `style={{ x: mv }}`: values (MotionValues) read every frame
    pub style: Option<Vars>,
}

impl Spec {
    fn vars<'s>(&'s self, t: Option<&'s Target>) -> Option<&'s Vars> {
        match t? {
            Target::Vars(v) => Some(v),
            Target::Variant(name) => self.variants.as_deref()?.iter().find(|(n, _)| *n == name).map(|(_, v)| v),
            Target::Off => None,
        }
    }
}

macro_rules! spec_setters {
    ($($(#[$doc:meta])* $method:ident => $field:ident),*) => {
        impl Div {$(
            $(#[$doc])*
            #[must_use]
            pub fn $method(mut self, target: impl Into<Target>) -> Div {
                self.motion_mut().$field = Some(target.into());
                self
            }
        )*}
    };
}
spec_setters!(
    /// Where it starts when it appears (`false`: at its `animate` values).
    initial => initial,
    /// What it animates to; changing it animates again.
    animate => animate,
    /// What it animates to when it goes away (in an `AnimatePresence`).
    exit => exit,
    /// On top of `animate` while the pointer is over it.
    while_hover => while_hover,
    /// On top while it is pressed.
    while_tap => while_tap,
    /// On top while it has focus.
    while_focus => while_focus
);

impl Div {
    fn motion_mut(&mut self) -> &mut Spec {
        self.rare_mut().motion.get_or_insert_with(Box::default)
    }

    /// The default transition of its motions (Motion's `transition`:
    /// `{{ type: "spring", x: { duration: 1 } }}`).
    #[must_use]
    pub fn transition(mut self, t: impl Into<Vars>) -> Div {
        self.motion_mut().transition = Some(t.into());
        self
    }

    /// Values set every frame, usually [`MotionValue`](super::MotionValue)s
    /// (Motion's `style={{ x: mv }}`).
    #[must_use]
    pub fn motion_style(mut self, v: Vars) -> Div {
        self.motion_mut().style = Some(v);
        self
    }

    /// Named targets: `animate="visible"` picks one.
    #[must_use]
    pub fn variants(mut self, v: impl Into<Variants>) -> Div {
        self.motion_mut().variants = Some(v.into());
        self
    }
}

impl From<Transition> for Vars {
    fn from(t: Transition) -> Vars {
        Vars { transition: t, ..Vars::default() }
    }
}

/// The interaction states `while*` targets follow.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct Gestures {
    pub hover: bool,
    pub tap: bool,
    pub focus: bool,
}

/// One animated property of an element.
struct PropState {
    id: i32,
    /// the style fields it writes (non-transform properties)
    fields: &'static [Field],
    /// its value now (written by the engine), when it has one
    cur: Option<Read>,
    /// its value in the element's style (what it goes back to)
    base: Read,
    /// the target it last started a motion to
    last: Option<Value>,
}

/// An element's animated values: the engine's target object.
#[derive(Default)]
pub(crate) struct Slot {
    props: SmallVec<[PropState; 4]>,
    seen: u64,
}

impl Slot {
    fn find(&mut self, id: i32) -> Option<&mut PropState> {
        self.props.iter_mut().find(|p| p.id == id)
    }
}

unsafe extern "C" fn slot_get(obj: *mut c_void, prop: i32, r_value: *mut f32, r_unit: *mut i32) -> i32 {
    // SAFETY: `obj` is a live boxed Slot (the engine forgets it before it's freed), and
    // nothing else borrows it during engine calls; the out pointers hold 4 floats / 1 int.
    let slot = unsafe { &mut *obj.cast::<Slot>() };
    let Some(p) = slot.find(prop) else { return 0 };
    let r = p.cur.unwrap_or(p.base);
    unsafe {
        std::ptr::copy_nonoverlapping(r.v.as_ptr(), r_value, 4);
        *r_unit = r.unit;
    }
    i32::from(r.n)
}

unsafe extern "C" fn slot_set(obj: *mut c_void, prop: i32, value: *const f32, n: i32, unit: i32) {
    // SAFETY: as in `slot_get`; `value` holds `n` (<= 4) floats.
    let slot = unsafe { &mut *obj.cast::<Slot>() };
    let Some(p) = slot.find(prop) else { return };
    let mut r = Read { n: n.clamp(1, 4) as u8, unit, ..Read::default() };
    unsafe { std::ptr::copy_nonoverlapping(value, r.v.as_mut_ptr(), r.n as usize) };
    p.cur = Some(r);
}

static SLOT_CLASS: sys::BKAnimClass = sys::BKAnimClass { get: Some(slot_get), set: Some(slot_set), retain: None,
                                                         release: None };

/// Every motion element of a window, by element key.
#[derive(Default)]
pub(crate) struct Elements {
    els: KeyMap<u64, Box<Slot>>,
    frame: u64,
}

impl Drop for Elements {
    fn drop(&mut self) {
        for (_, s) in self.els.drain() {
            forget(s);
        }
    }
}

/// The engine stops writing to a slot going away.
fn forget(mut slot: Box<Slot>) {
    // SAFETY: the slot is still alive here.
    unsafe { sys::BK_anim_kill_target((&raw mut *slot).cast(), -1) };
}

impl Elements {
    pub(crate) fn begin(&mut self) {
        self.frame += 1;
    }

    /// Forget elements gone this frame (their motions stop).
    pub(crate) fn end(&mut self) {
        let frame = self.frame;
        let gone: SmallVec<[u64; 4]> = self.els.iter().filter(|(_, s)| s.seen != frame).map(|(k, _)| *k).collect();
        for k in gone {
            if let Some(s) = self.els.remove(&k) {
                forget(s);
            }
        }
    }

    pub(crate) fn has(&self, key: u64) -> bool {
        self.els.contains_key(&key)
    }

    /// Element `el` (`key`) with motion `spec` and style `base` this frame:
    /// start the motions its targets (and imperative motions for it) need, and
    /// lay the moving values over `out`.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn apply(&mut self, key: u64, el: &ElData, spec: Option<&Spec>, base: &Computed, g: Gestures, u: &Units,
                        out: &mut Option<Computed>) {
        let frame = self.frame;
        let first = !self.els.contains_key(&key);
        if first && spec.is_none() && !PENDING.with(|p| with_pending(p, |ps| ps.iter().any(|p| p.selector.matches(el)))) {
            return; // nothing animates it
        }
        let empty;
        let spec = match spec {
            Some(s) => s,
            None => {
                empty = Spec::default();
                &empty
            }
        };
        let slot: *mut Slot = &raw mut **self.els.entry(key).or_default();
        // targets, later layers over earlier ones (Motion's priority)
        let mut targets: SmallVec<[(&Key, &Value, &Vars); 8]> = SmallVec::new();
        let layers = [Some(spec.animate.as_ref()), g.focus.then_some(spec.while_focus.as_ref()),
                      g.hover.then_some(spec.while_hover.as_ref()), g.tap.then_some(spec.while_tap.as_ref())];
        for t in layers.into_iter().flatten() {
            let Some(vars) = spec.vars(t) else { continue };
            for (k, v) in vars.props.iter() {
                match targets.iter_mut().find(|(o, _, _)| o.id() == k.id()) {
                    Some(slot) => *slot = (*k, v, vars),
                    None => targets.push((*k, v, vars)),
                }
            }
        }
        let none = Transition::default();
        // the element's transition for a property (its own, else the element's)
        let element_t = |id: i32| spec.transition.as_ref().map_or_else(Transition::default, |v| v.transition_for(id, &none));
        {
            // SAFETY: no engine call runs while this borrow lives.
            let s = unsafe { &mut *slot };
            s.seen = frame;
            for (k, _, _) in &targets {
                ensure(s, k.id(), base, u);
            }
            for p in &mut s.props {
                p.base = base_of(p, base, u);
            }
            // `style={{ x: mv }}`: values read every frame
            if let Some(style) = &spec.style {
                for (k, v) in style.props.iter() {
                    ensure(s, k.id(), base, u);
                    if let (Some(p), Some(r)) = (s.find(k.id()), v.read().filter(|r| !r.current)) {
                        p.cur = Some(r);
                    }
                }
            }
            if first {
                // where it starts: `initial`, or (initial={false}) where it goes
                match &spec.initial {
                    Some(Target::Off) => {
                        for (k, v, _) in &targets {
                            if let (Some(p), Some(r)) = (s.find(k.id()), v.read()) {
                                p.cur = Some(r);
                                p.last = Some((*v).clone());
                            }
                        }
                    }
                    t => {
                        if let Some(init) = spec.vars(t.as_ref()) {
                            for (k, v) in init.props.iter() {
                                ensure(s, k.id(), base, u);
                                if let (Some(p), Some(r)) = (s.find(k.id()), v.read()) {
                                    p.cur = Some(r);
                                }
                            }
                        }
                    }
                }
            }
        }
        // start a motion for each target that changed
        for (k, v, vars) in &targets {
            // SAFETY: as above, for this check only.
            let changed = unsafe { &mut *slot }.find(k.id()).is_some_and(|p| p.last.as_ref() != Some(*v));
            if changed {
                let t = vars.transition_for(k.id(), &element_t(k.id()));
                start(slot, k.id(), v.keys(), &t);
                if let Some(p) = unsafe { &mut *slot }.find(k.id()) {
                    p.last = Some((*v).clone());
                }
            }
        }
        // properties no longer targeted go back to their style's value
        let n = unsafe { &*slot }.props.len();
        for i in 0..n {
            let (id, back) = {
                let p = &unsafe { &*slot }.props[i];
                let targeted = targets.iter().any(|(k, _, _)| k.id() == p.id);
                (p.id, (!targeted && p.last.is_some() && p.cur.is_some()).then_some(p.base))
            };
            if let Some(b) = back {
                let to = [Value::Num(b.v[0])];
                let keys: &[Value] = if b.n == 4 {
                    &[Value::Color(crate::color::Rgba { r: b.v[0], g: b.v[1], b: b.v[2], a: b.v[3] })]
                } else {
                    &to
                };
                start(slot, id, keys, &element_t(id));
                unsafe { &mut *slot }.props[i].last = None;
            }
        }
        // imperative motions waiting for this element (gsap.to("#id", ..))
        PENDING.with(|p| with_pending(p, |ps| {
            for p in ps.iter_mut().filter(|p| p.selector.matches(el)) {
                {
                    // SAFETY: no engine call while borrowed.
                    let s = unsafe { &mut *slot };
                    for (k, _) in &p.props {
                        ensure(s, k.id(), base, u);
                    }
                }
                let tracks: SmallVec<[(i32, &[Value]); 4]> = p.props.iter().map(|(k, v)| (k.id(), v.keys())).collect();
                let m = tween(&SLOT_CLASS, slot.cast(), &tracks, &p.transition, p.flavor, p.from, None);
                let off = super::tween::at(p.transition.stagger.unwrap_or(0.0) * p.matched as f64);
                // SAFETY: both alive; the timeline takes it, then our handle goes.
                unsafe {
                    sys::BK_timeline_add(p.timeline.raw(), m, off.as_ptr());
                    sys::BK_motion_release(m);
                }
                p.matched += 1;
            }
        }));
        // lay the values over the style
        let s = unsafe { &*slot };
        for p in &s.props {
            if let Some(r) = p.cur {
                write(p, &r, out.get_or_insert_with(|| base.clone()), u);
            }
        }
    }
}

/// One compound selector: tag, `#id`, `.class`es, all matching.
#[derive(Debug, Default)]
struct Compound {
    tag: Option<Box<str>>,
    id: Option<Box<str>>,
    classes: SmallVec<[Box<str>; 2]>,
}

/// The elements of an imperative motion: `"#id"`, `".a.b"`, `"div.card"`, a
/// comma-separated list (any matching).
#[derive(Debug, Default)]
pub(crate) struct Selector(SmallVec<[Compound; 1]>);

impl Selector {
    pub(crate) fn parse(s: &str) -> Selector {
        Selector(s.split(',').map(|part| {
            let mut c = Compound::default();
            let mut rest = part.trim();
            let tag_end = rest.find(['#', '.']).unwrap_or(rest.len());
            if tag_end > 0 && &rest[..tag_end] != "*" {
                c.tag = Some(rest[..tag_end].into());
            }
            rest = &rest[tag_end..];
            while let Some(kind) = rest.chars().next() {
                let end = rest[1..].find(['#', '.']).map_or(rest.len(), |i| i + 1);
                let name: Box<str> = rest[1..end].into();
                if kind == '#' {
                    c.id = Some(name);
                } else {
                    c.classes.push(name);
                }
                rest = &rest[end..];
            }
            c
        }).collect())
    }

    pub(crate) fn matches(&self, el: &ElData) -> bool {
        let attr = |n: &str| el.attrs.iter().find(|(k, _)| &**k == n).map(|(_, v)| &**v);
        self.0.iter().any(|c| {
            c.tag.as_deref().is_none_or(|t| t == el.tag)
                && c.id.as_deref().is_none_or(|id| attr("id") == Some(id))
                && c.classes.iter().all(|cl| attr("class").is_some_and(|v| v.split_whitespace().any(|x| x == &**cl)))
        })
    }
}

/// An imperative motion waiting for its elements (they're found at the
/// next layout, in document order: the stagger order).
pub(crate) struct Pending {
    pub selector: Selector,
    pub props: Vec<(&'static Key, Value)>,
    pub transition: Transition,
    pub from: bool,
    pub flavor: Flavor,
    pub timeline: super::Tween,
    pub matched: usize,
}

thread_local! {
    static PENDING: std::cell::Cell<Vec<Pending>> = const { std::cell::Cell::new(Vec::new()) };
}

fn with_pending<R>(c: &std::cell::Cell<Vec<Pending>>, f: impl FnOnce(&mut Vec<Pending>) -> R) -> R {
    let mut v = c.take();
    let r = f(&mut v);
    c.set(v);
    r
}

pub(crate) fn queue(p: Pending) {
    PENDING.with(|c| with_pending(c, |v| v.push(p)));
}

/// Whether imperative motions wait for elements (every element is checked).
pub(crate) fn pending() -> bool {
    PENDING.with(|c| with_pending(c, |v| !v.is_empty()))
}

/// After a layout: the waiting motions found their elements (or none).
pub(crate) fn end_pending() {
    let done = PENDING.with(std::cell::Cell::take);
    drop(done);
}

/// A motion of property `id` of the slot to `keys` with transition `t`.
fn start(slot: *mut Slot, id: i32, keys: &[Value], t: &Transition) {
    let m = tween(&SLOT_CLASS, slot.cast(), &[(id, keys)], t, Flavor::Motion, false, None);
    // SAFETY: our handle (the global timeline holds it while it runs).
    unsafe { sys::BK_motion_release(m) };
}

fn ensure(s: &mut Slot, id: i32, base: &Computed, u: &Units) {
    if s.find(id).is_none() {
        let mut p = PropState { id, fields: fields_for(id), cur: None, base: Read::default(), last: None };
        p.base = base_of(&p, base, u);
        s.props.push(p);
    }
}

/// The style fields a non-transform property writes (by its CSS name).
fn fields_for(id: i32) -> &'static [Field] {
    match id {
        sys::BK_PROP_OPACITY => &[Field::Opacity],
        sys::BK_PROP_WIDTH => &[Field::Width],
        sys::BK_PROP_HEIGHT => &[Field::Height],
        sys::BK_PROP_COLOR => &[Field::Color],
        sys::BK_PROP_BACKGROUND_COLOR => &[Field::Background],
        id if id >= sys::BK_PROP__FIXED => {
            // SAFETY: the engine's name for an id it gave (or NULL).
            let name = unsafe { sys::BK_anim_prop_name(id) };
            if name.is_null() {
                return &[];
            }
            let camel = unsafe { std::ffi::CStr::from_ptr(name) }.to_string_lossy();
            let mut kebab = String::with_capacity(camel.len() + 4);
            for c in camel.chars() {
                if c.is_ascii_uppercase() {
                    kebab.push('-');
                    kebab.push(c.to_ascii_lowercase());
                } else {
                    kebab.push(c);
                }
            }
            fields_of(crate::css::Prop::lookup(&kebab))
        }
        _ => &[],
    }
}

/// What property `p` is in style `c` (transforms: none, Motion's x / scale /
/// rotate are on top of the style's own).
fn base_of(p: &PropState, c: &Computed, u: &Units) -> Read {
    let one = |v: f32| Read { v: [v, 0.0, 0.0, 0.0], n: 1, ..Read::default() };
    match p.id {
        sys::BK_PROP_SCALE | sys::BK_PROP_SCALE_X | sys::BK_PROP_SCALE_Y => one(1.0),
        sys::BK_PROP_X | sys::BK_PROP_Y | sys::BK_PROP_Z | sys::BK_PROP_ROTATE | sys::BK_PROP_ROTATE_X
        | sys::BK_PROP_ROTATE_Y | sys::BK_PROP_SKEW_X | sys::BK_PROP_SKEW_Y => one(0.0),
        _ => p.fields.first().and_then(|&f| field_numbers(f, c, u)).map_or(one(0.0), |(v, n, pct)| Read {
            v,
            n: n as u8,
            unit: if n == 4 { sys::BK_UNIT_COLOR } else if pct { sys::BK_UNIT_PERCENT } else { sys::BK_UNIT_PX },
            ..Read::default()
        }),
    }
}

fn length(v: f32, unit: i32) -> Length {
    match unit {
        sys::BK_UNIT_PERCENT => Length::Percent(v),
        sys::BK_UNIT_EM => Length::Em(v),
        sys::BK_UNIT_REM => Length::Rem(v),
        sys::BK_UNIT_VW => Length::Vw(v),
        sys::BK_UNIT_VH => Length::Vh(v),
        _ => Length::Px(v),
    }
}

/// `a` moved by `v` in `unit` (pixels add; another unit replaces a zero `a`).
fn offset(a: Length, v: f32, unit: i32, u: &Units) -> Length {
    match (a, length(v, unit)) {
        (a, Length::Px(d)) => a.to_px(u).map_or(Length::Px(d), |x| Length::Px(x + d)),
        (a, l) if a.to_px(u) == Some(0.0) => l,
        (a, _) => a,
    }
}

fn radians(v: f32, unit: i32) -> f32 {
    match unit {
        sys::BK_UNIT_RAD => v,
        sys::BK_UNIT_TURN => v * std::f32::consts::TAU,
        _ => v.to_radians(),
    }
}

/// Lay value `r` of property `p` over style `c`.
fn write(p: &PropState, r: &Read, c: &mut Computed, u: &Units) {
    let v = r.v[0];
    match p.id {
        sys::BK_PROP_X => c.translate.0 = offset(c.translate.0, v, r.unit, u),
        sys::BK_PROP_Y => c.translate.1 = offset(c.translate.1, v, r.unit, u),
        sys::BK_PROP_SCALE | sys::BK_PROP_SCALE_X | sys::BK_PROP_SCALE_Y => {
            let e = c.effects_mut();
            let (sx, sy) = e.scale2.unwrap_or((1.0, 1.0));
            e.scale2 = Some(match p.id {
                sys::BK_PROP_SCALE_X => (sx * v, sy),
                sys::BK_PROP_SCALE_Y => (sx, sy * v),
                _ => (sx * v, sy * v),
            });
        }
        sys::BK_PROP_ROTATE => c.effects_mut().rotate += radians(v, r.unit),
        sys::BK_PROP_SKEW_X => c.effects_mut().transform.push(crate::effects::TransformFn::Skew(radians(v, r.unit), 0.0)),
        sys::BK_PROP_SKEW_Y => c.effects_mut().transform.push(crate::effects::TransformFn::Skew(0.0, radians(v, r.unit))),
        _ => {
            let pct = r.unit == sys::BK_UNIT_PERCENT;
            for &f in p.fields {
                set_field_numbers(f, c, &r.v[..r.n as usize], pct);
            }
        }
    }
}
