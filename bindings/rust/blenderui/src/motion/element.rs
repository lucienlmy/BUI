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
use crate::anim::{Field, copy_field, field_numbers, fields_of, set_field_numbers};
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
    fn vars(&self, w: When) -> Option<&Vars> {
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
    /// its element's key, and where it is in the live list
    key: u64,
    live: u32,
    /// After the last full frame: the element's box, its style before the
    /// moving values (they go over it again on redraws), whether a moving
    /// value changes layout (then a redraw can't do), and the MotionValues
    /// its `style` binds.
    node: usize,
    base: Option<Box<Computed>>,
    layout: bool,
    bound: SmallVec<[(i32, super::MotionValue); 2]>,
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

const CHUNK: usize = 64;

/// The slots: in chunks of 64 (a slot never moves: the engine holds its
/// address), the live ones listed densely (a frame walks them in memory
/// order; one that goes is swapped with the last), freed ones reused.
#[derive(Default)]
struct Arena {
    chunks: Vec<Box<[Slot; CHUNK]>>,
    keys: KeyMap<u64, u32>,
    live: Vec<u32>,
    free: Vec<u32>,
}

impl Arena {
    fn at(&mut self, i: u32) -> &mut Slot {
        &mut self.chunks[i as usize / CHUNK][i as usize % CHUNK]
    }

    fn alloc(&mut self, key: u64) -> u32 {
        let i = self.free.pop().unwrap_or_else(|| {
            let n = (self.chunks.len() * CHUNK) as u32;
            self.chunks.push(Box::new(std::array::from_fn(|_| Slot::default())));
            self.free.extend((n + 1..n + CHUNK as u32).rev());
            n
        });
        let live = self.live.len() as u32;
        self.live.push(i);
        self.keys.insert(key, i);
        let s = self.at(i);
        s.key = key;
        s.live = live;
        i
    }

    /// The engine stops writing to slot `i`; it's free.
    fn release(&mut self, i: u32) {
        let s = self.at(i);
        let (key, live) = (s.key, s.live);
        let obj: *mut c_void = (&raw mut *s).cast();
        for p in &s.props {
            // SAFETY: the slot is alive; per property is a hash lookup (all of an
            // object's motions would be a walk over every motion)
            unsafe { sys::BK_anim_kill_target(obj, p.id) };
        }
        *self.at(i) = Slot::default();
        self.keys.remove(&key);
        self.live.swap_remove(live as usize);
        if let Some(&moved) = self.live.get(live as usize) {
            self.at(moved).live = live;
        }
        self.free.push(i);
    }
}

/// Every motion element of a window, by element key.
#[derive(Default)]
pub(crate) struct Elements {
    els: Arena,
    frame: u64,
    /// scratch: each box's move on a redraw
    shift: Vec<(f32, f32)>,
}

impl Drop for Elements {
    fn drop(&mut self) {
        while let Some(&i) = self.els.live.last() {
            self.els.release(i);
        }
    }
}

impl Elements {
    pub(crate) fn begin(&mut self) {
        self.frame += 1;
    }

    /// Forget elements gone this frame (their motions stop).
    pub(crate) fn end(&mut self) {
        let frame = self.frame;
        let mut k = 0;
        while k < self.els.live.len() {
            let i = self.els.live[k];
            if self.els.at(i).seen == frame {
                k += 1;
            } else {
                self.els.release(i); /* the last takes its place */
            }
        }
    }

    pub(crate) fn has(&self, key: u64) -> bool {
        self.els.keys.contains_key(&key)
    }

    /// Element `el` (`key`) with motion `spec` and style `base` this frame:
    /// start the motions its targets (and imperative motions for it) need, and
    /// lay the moving values over `out`.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn apply(&mut self, key: u64, node: usize, el: &ElData, spec: Option<&Spec>, base: &Computed, g: Gestures, u: &Units,
                        out: &mut Option<Computed>) {
        let frame = self.frame;
        let found = self.els.keys.get(&key).copied();
        let first = found.is_none();
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
        let i = found.unwrap_or_else(|| self.els.alloc(key));
        let slot: *mut Slot = self.els.at(i);
        // targets, later layers over earlier ones (Motion's priority)
        let mut targets: SmallVec<[(&Key, &Value, &Vars); 8]> = SmallVec::new();
        let layers = [Some(When::Animate), g.focus.then_some(When::WhileFocus), g.hover.then_some(When::WhileHover),
                      g.tap.then_some(When::WhileTap)];
        for w in layers.into_iter().flatten() {
            let Some(vars) = spec.vars(w) else { continue };
            for (k, v) in vars.props.iter() {
                match targets.iter_mut().find(|(o, _, _)| o.id() == k.id()) {
                    Some(slot) => *slot = (*k, v, vars),
                    None => targets.push((*k, v, vars)),
                }
            }
        }
        let none = Transition::default();
        // the element's transition for a property (its own, else the element's)
        let element_t = |id: i32| spec.transition().map_or_else(Transition::default, |v| v.transition_for(id, &none));
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
            if let Some(style) = spec.style() {
                for (k, v) in style.props.iter() {
                    ensure(s, k.id(), base, u);
                    if let (Some(p), Some(r)) = (s.find(k.id()), v.read().filter(|r| !r.current)) {
                        p.cur = Some(r);
                    }
                }
            }
            if first {
                // where it starts: `initial`, or (initial={false}) where it goes
                match spec.target(When::Initial) {
                    Some(Target::Off) => {
                        for (k, v, _) in &targets {
                            if let (Some(p), Some(r)) = (s.find(k.id()), v.read()) {
                                p.cur = Some(r);
                                p.last = Some((*v).clone());
                            }
                        }
                    }
                    _ => {
                        if let Some(init) = spec.vars(When::Initial) {
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
            let changed = unsafe { &mut *slot }.find(k.id()).is_some_and(|p| !p.last.as_ref().is_some_and(|l| l.same(v)));
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
        let s = unsafe { &mut *slot };
        for p in &s.props {
            if let Some(r) = p.cur {
                write(p, &r, out.get_or_insert_with(|| base.clone()), u);
            }
        }
        // what a redraw needs (a frame with nothing but moving values)
        s.node = node;
        s.layout = s.props.iter().any(|p| p.cur.is_some() && !paint_only(p));
        s.bound.clear();
        if let Some(style) = spec.style() {
            s.bound.extend(style.props.iter().filter_map(|(k, v)| match v {
                Value::Motion(m) => Some((k.id(), *m)),
                _ => None,
            }));
        }
        // an element with animated properties owns its style (redraws change
        // it in place), even before its first value arrives
        if s.props.is_empty() {
            s.base = None;
        } else {
            out.get_or_insert_with(|| base.clone());
            match &mut s.base {
                Some(b) => (**b).clone_from(base),
                None => s.base = Some(Box::new(base.clone())),
            }
        }
    }

    /// A frame where only moving values changed (no render, cascade or
    /// layout): each element's values go over its style again, and boxes
    /// moved by `x` / `y` move with their contents. False when that can't do
    /// (a moving value changes layout, or the frame isn't the one the slots
    /// know): the window then renders as usual.
    pub(crate) fn redraw(&mut self, frame: &mut crate::layout::Frame, styles: &mut crate::layout::StyleStore,
                         viewport: (f32, f32)) -> bool {
        let Arena { chunks, live, .. } = &mut self.els;
        let at = |i: u32| (i as usize / CHUNK, i as usize % CHUNK);
        if live.iter().any(|&i| {
            let (c, o) = at(i);
            chunks[c][o].layout && chunks[c][o].base.is_some()
        }) {
            return false;
        }
        let nodes = &mut frame.nodes;
        let mut moved = false;
        let mut faded = false;
        for &i in live.iter() {
            let (c, o) = at(i);
            let s = &mut chunks[c][o];
            let Some(base) = &s.base else { continue };
            let Some(n) = nodes.get(s.node).filter(|n| n.key == s.key) else { return false };
            for &(id, mv) in &s.bound {
                if let Some(p) = s.props.iter_mut().find(|p| p.id == id) {
                    p.cur = Some(mv.read());
                }
            }
            let st = styles.private_mut(n.style);
            let u = Units { font_size: base.font_size, viewport };
            let (w, h) = (n.bounds.size.width, n.bounds.size.height);
            let before = (st.translate.0.resolve(&u, w), st.translate.1.resolve(&u, h));
            // back to the base where the values go (not the whole style: a few fields)
            let mut transforms = false;
            for p in &s.props {
                transforms |= restore(p, base, st);
            }
            if transforms {
                restore_transforms(base, st);
            }
            for p in &s.props {
                if let Some(r) = p.cur {
                    write(p, &r, st, &u);
                    faded |= p.id == sys::BK_PROP_OPACITY || p.fields.iter().any(|f| matches!(f, Field::Visibility));
                }
            }
            let after = (st.translate.0.resolve(&u, w), st.translate.1.resolve(&u, h));
            if before != after {
                self.shift.resize(nodes.len(), (0.0, 0.0));
                self.shift[s.node] = (after.0 - before.0, after.1 - before.1);
                moved = true;
            }
        }
        if faded {
            crate::layout::inherit(nodes, styles); // boxes keep their opacity times their parents'
        }
        if moved {
            // a box moves with every box under it (parents come before children)
            for j in 0..nodes.len() {
                let own = self.shift[j];
                let up = nodes[j].parent.map_or((0.0, 0.0), |p| self.shift[p]);
                let d = (own.0 + up.0, own.1 + up.1);
                self.shift[j] = d;
                if d != (0.0, 0.0) {
                    let b = &mut nodes[j].bounds;
                    b.origin.x += d.0;
                    b.origin.y += d.1;
                    // a clip made inside the moved subtree moves with it
                    let inner = nodes[j].parent.is_some_and(|p| self.shift[p] == d && own == (0.0, 0.0));
                    let parent_clip = nodes[j].parent.and_then(|p| nodes[p].clip);
                    if let (true, Some(c)) = (inner, &mut nodes[j].clip) {
                        if parent_clip != Some(*c) {
                            c.origin.x += d.0;
                            c.origin.y += d.1;
                        }
                    }
                }
            }
            self.shift.clear();
        }
        true
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

/// Whether property `p` only changes how things look, not where they are
/// (transforms, opacity, colors, shadows, filters): a redraw can do it.
fn paint_only(p: &PropState) -> bool {
    use Field::{Accent, Backdrop, Background, BorderColor, BoxShadow, Color, DecorationColor, Filter, Opacity,
                OutlineColor, Rotate, Scale, TextShadow, Transform, Translate, Visibility};
    match p.id {
        sys::BK_PROP_X | sys::BK_PROP_Y | sys::BK_PROP_Z | sys::BK_PROP_SCALE | sys::BK_PROP_SCALE_X
        | sys::BK_PROP_SCALE_Y | sys::BK_PROP_ROTATE | sys::BK_PROP_ROTATE_X | sys::BK_PROP_ROTATE_Y
        | sys::BK_PROP_SKEW_X | sys::BK_PROP_SKEW_Y => true,
        _ => p.fields.iter().all(|f| matches!(f, Opacity | Color | Background | BorderColor(_) | OutlineColor
            | DecorationColor | Accent | BoxShadow | TextShadow | Filter | Backdrop | Rotate | Scale | Transform
            | Translate | Visibility)),
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

/// Put back in `st` what property `p` writes, from `base`; true for the
/// transform functions (restored together by [`restore_transforms`]).
fn restore(p: &PropState, base: &Computed, st: &mut Computed) -> bool {
    match p.id {
        sys::BK_PROP_X => st.translate.0 = base.translate.0,
        sys::BK_PROP_Y => st.translate.1 = base.translate.1,
        sys::BK_PROP_OPACITY => st.opacity = base.opacity,
        sys::BK_PROP_COLOR => st.color = base.color,
        sys::BK_PROP_BACKGROUND_COLOR => st.background = base.background,
        sys::BK_PROP_SCALE | sys::BK_PROP_SCALE_X | sys::BK_PROP_SCALE_Y | sys::BK_PROP_ROTATE | sys::BK_PROP_SKEW_X
        | sys::BK_PROP_SKEW_Y => return true,
        _ => {
            for &f in p.fields {
                copy_field(f, base, st);
            }
        }
    }
    false
}

/// The transform parts Motion writes (scale, rotate, skews), as in `base`.
fn restore_transforms(base: &Computed, st: &mut Computed) {
    match (&base.effects, &mut st.effects) {
        (Some(b), Some(s)) => {
            s.scale2 = b.scale2;
            s.rotate = b.rotate;
            s.transform.clone_from(&b.transform);
        }
        (None, Some(s)) => {
            s.scale2 = None;
            s.rotate = 0.0;
            s.transform.clear();
        }
        _ => {}
    }
}

fn rgba(v: &[f32; 4]) -> crate::color::Rgba {
    let c = |x: f32| x.clamp(0.0, 1.0);
    crate::color::Rgba { r: c(v[0]), g: c(v[1]), b: c(v[2]), a: c(v[3]) }
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
        // the common ones straight into their field (the generic way builds a value first)
        sys::BK_PROP_OPACITY => c.opacity = v.clamp(0.0, 1.0),
        sys::BK_PROP_COLOR if r.n == 4 => c.color = rgba(&r.v),
        sys::BK_PROP_BACKGROUND_COLOR if r.n == 4 => c.background = rgba(&r.v),
        _ => {
            let pct = r.unit == sys::BK_UNIT_PERCENT;
            for &f in p.fields {
                set_field_numbers(f, c, &r.v[..r.n as usize], pct);
            }
        }
    }
}
