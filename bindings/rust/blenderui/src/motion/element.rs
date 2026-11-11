//! Motion on elements: `<motion.div initial={{..}} animate={{..}} whileHover={{..}}>`.
//!
//! An element's [`Spec`] says what it animates to; the window keeps, per
//! element key, a [`Slot`] the engine writes the moving values into (the
//! slot is the engine's target object). Each layout compares the element's
//! targets (animate, then whileFocus, whileHover, whileTap on top) with the
//! last ones and starts a motion for each property that changed, from where
//! it is (a spring takes the velocity it had). The values are then laid over
//! the element's computed style, like CSS animations (`anim.rs`).


use std::ffi::c_void;

use smallvec::SmallVec;

use blenderui_sys as sys;

use super::pending::{PENDING, with_pending};
use super::spec::{Spec, Target, When};
use super::styling::{base_of, fields_for, paint_only, restore, restore_transforms, write};
use super::value::{Key, Read, Value};
use super::vars::{Flavor, Transition, Vars, tween};
use crate::anim::Field;
use crate::layout::ElData;
use crate::geometry::Units;
use crate::style::{Computed, KeyMap};

/// The interaction states `while*` targets follow.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct Gestures {
    pub hover: bool,
    pub tap: bool,
    pub focus: bool,
    pub drag: bool,
}

/// One animated property of an element.
pub(super) struct PropState {
    pub(super) id: i32,
    /// the style fields it writes (non-transform properties)
    pub(super) fields: &'static [Field],
    /// its value now (written by the engine), when it has one
    pub(super) cur: Option<Read>,
    /// its value in the element's style (what it goes back to)
    pub(super) base: Read,
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
    /// what its values touch (a redraw skips the rest): x / y, opacity / visibility
    moves: bool,
    fades: bool,
    bound: SmallVec<[(i32, super::MotionValue); 2]>,
    /// what few elements use (in view, exiting): out of the way of the rest
    gest: Option<Box<Gest>>,
}

/// A slot's rarer state.
#[derive(Default)]
struct Gest {
    /// `whileInView`: wanted, when it counts, and whether it is (last layout)
    view: Option<super::spec::InView>,
    in_view: bool,
    /// the presence taking it away (0: none), its exit motions (one handle
    /// each, to see them end), and whether their end was told
    exiting: u64,
    exit_motions: SmallVec<[*mut sys::BKMotion; 2]>,
    exit_told: bool,
}

impl Gest {
    /// Let go of the exit motions' handles (the element stays, or goes).
    fn end_exit(&mut self) {
        for m in self.exit_motions.drain(..) {
            // SAFETY: the handle taken when it started
            unsafe { sys::BK_motion_release(m) };
        }
        self.exiting = 0;
        self.exit_told = false;
    }
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
        if let Some(g) = &mut self.at(i).gest {
            g.end_exit();
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
    /// the boxes a redraw changed (its elements and everything under them)
    dirty: Vec<bool>,
    /// a slot may be exiting (frames look for exits that ended)
    exits: bool,
}

impl Drop for Elements {
    fn drop(&mut self) {
        while let Some(&i) = self.els.live.last() {
            self.els.release(i);
        }
    }
}

impl Elements {
    pub(crate) fn take_dirty(&mut self) -> Vec<bool> {
        std::mem::take(&mut self.dirty)
    }

    pub(crate) fn give_dirty(&mut self, v: Vec<bool>) {
        self.dirty = v;
    }

    pub(crate) fn begin(&mut self) {
        self.frame += 1;
    }

    /// Exits that ended (every element a presence takes away has finished
    /// its exit motions): the presence lets them go.
    fn check_exits(&mut self) {
        if !self.exits {
            return; // nothing is exiting: no walk over the slots
        }
        let mut ids: SmallVec<[(u64, bool); 4]> = SmallVec::new();
        for &i in &self.els.live {
            let Some(g) = &self.els.chunks[i as usize / CHUNK][i as usize % CHUNK].gest else { continue };
            if g.exiting == 0 || g.exit_told {
                continue;
            }
            // SAFETY: handles held by the slot
            let ended = g.exit_motions.iter().all(|&m| unsafe { sys::BK_motion_is_active(m) } == 0);
            match ids.iter_mut().find(|(id, _)| *id == g.exiting) {
                Some((_, all)) => *all &= ended,
                None => ids.push((g.exiting, ended)),
            }
        }
        self.exits = !ids.is_empty();
        for (id, all) in ids {
            if all {
                for k in 0..self.els.live.len() {
                    let i = self.els.live[k];
                    if let Some(g) = self.els.at(i).gest.as_mut().filter(|g| g.exiting == id) {
                        g.exit_told = true;
                    }
                }
                super::presence::done(id);
            }
        }
    }

    /// After a layout: which elements are in view now (`whileInView`); true
    /// when one changed (its targets change: render again).
    pub(crate) fn after_layout(&mut self, frame: &crate::layout::Frame, viewport: (f32, f32)) -> bool {
        let mut changed = false;
        for k in 0..self.els.live.len() {
            let i = self.els.live[k];
            let s = self.els.at(i);
            let (node, key) = (s.node, s.key);
            let Some(g) = &mut s.gest else { continue };
            let Some(v) = g.view else { continue };
            let Some(n) = frame.nodes.get(node).filter(|n| n.key == key) else { continue };
            let b = n.bounds;
            let area = (b.size.width * b.size.height).max(1e-6);
            let w = (b.origin.x + b.size.width).min(viewport.0) - b.origin.x.max(0.0);
            let h = (b.origin.y + b.size.height).min(viewport.1) - b.origin.y.max(0.0);
            let shown = if w > 0.0 && h > 0.0 { w * h / area } else { 0.0 };
            let inside = if v.amount <= 0.0 { shown > 0.0 } else { shown >= v.amount - 1e-4 };
            let now = inside || (v.once && g.in_view);
            if now != g.in_view {
                g.in_view = now;
                changed = true;
            }
        }
        self.check_exits();
        changed
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
        // SAFETY (this and the short borrows below): no engine call runs while they live.
        let (in_view, exiting) = {
            let s = unsafe { &mut *slot };
            let view = spec.target(When::WhileInView).map(|_| spec.viewport());
            let exiting = spec.exiting().unwrap_or(0);
            if view.is_some() || exiting != 0 || s.gest.is_some() {
                let g = s.gest.get_or_insert_with(Box::default);
                g.view = view;
                if g.exiting != exiting {
                    g.end_exit(); // a new exit, or none: shown again midway
                    g.exiting = exiting;
                }
                self.exits |= exiting != 0;
                (view.is_some() && g.in_view, exiting)
            } else {
                (false, 0)
            }
        };
        // targets, later layers over earlier ones (Motion's priority); taken
        // away, `exit` alone
        let mut targets: SmallVec<[(&Key, &Value, &Vars); 8]> = SmallVec::new();
        let layers = if exiting != 0 {
            [Some(When::Exit), None, None, None, None, None]
        } else {
            [Some(When::Animate), in_view.then_some(When::WhileInView), g.focus.then_some(When::WhileFocus),
             g.hover.then_some(When::WhileHover), g.tap.then_some(When::WhileTap),
             g.drag.then_some(When::WhileDrag)]
        };
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
                let m = start(slot, k.id(), v.keys(), &t);
                if exiting != 0 {
                    // its end lets the element go
                    unsafe { &mut *slot }.gest.get_or_insert_with(Box::default).exit_motions.push(m);
                } else {
                    // SAFETY: our handle (the global timeline holds it while it runs).
                    unsafe { sys::BK_motion_release(m) };
                }
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
                // what an exit leaves out stays where it is
                let targeted = exiting != 0 || targets.iter().any(|(k, _, _)| k.id() == p.id);
                (p.id, (!targeted && p.last.is_some() && p.cur.is_some()).then_some(p.base))
            };
            if let Some(b) = back {
                let to = [Value::Num(b.v[0])];
                let keys: &[Value] = if b.n == 4 {
                    &[Value::Color(crate::color::Rgba { r: b.v[0], g: b.v[1], b: b.v[2], a: b.v[3] })]
                } else {
                    &to
                };
                // SAFETY: our handle (the global timeline holds it while it runs).
                unsafe { sys::BK_motion_release(start(slot, id, keys, &element_t(id))) };
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
        s.moves = s.props.iter().any(|p| matches!(p.id, sys::BK_PROP_X | sys::BK_PROP_Y));
        s.fades = s.props.iter().any(|p| p.id == sys::BK_PROP_OPACITY || p.fields.contains(&Field::Visibility));
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
        self.dirty.clear();
        self.dirty.resize(nodes.len(), false);
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
            self.dirty[s.node] = true;
            let st = styles.private_mut(n.style);
            let u = Units { font_size: base.font_size, viewport };
            let (w, h) = (n.bounds.size.width, n.bounds.size.height);
            let at = |st: &Computed| (st.translate.0.resolve(&u, w), st.translate.1.resolve(&u, h));
            let before = if s.moves { at(st) } else { (0.0, 0.0) };
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
                }
            }
            faded |= s.fades;
            let after = if s.moves { at(st) } else { (0.0, 0.0) };
            if before != after {
                self.shift.resize(nodes.len(), (0.0, 0.0));
                self.shift[s.node] = (after.0 - before.0, after.1 - before.1);
                moved = true;
            }
        }
        if faded {
            crate::layout::inherit(nodes, styles); // boxes keep their opacity times their parents'
        }
        self.check_exits();
        // what's under a changed box changes with it (opacity, position)
        for j in 0..nodes.len() {
            if let Some(p) = nodes[j].parent {
                self.dirty[j] |= self.dirty[p];
            }
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

/// A motion of property `id` of the slot to `keys` with transition `t`.
/// A motion of property `id` of the slot to `keys` with transition `t`: the
/// handle is the caller's.
fn start(slot: *mut Slot, id: i32, keys: &[Value], t: &Transition) -> *mut sys::BKMotion {
    tween(&SLOT_CLASS, slot.cast(), &[(id, keys)], t, Flavor::Motion, false, None)
}

fn ensure(s: &mut Slot, id: i32, base: &Computed, u: &Units) {
    if s.find(id).is_none() {
        let mut p = PropState { id, fields: fields_for(id), cur: None, base: Read::default(), last: None };
        p.base = base_of(&p, base, u);
        s.props.push(p);
    }
}
