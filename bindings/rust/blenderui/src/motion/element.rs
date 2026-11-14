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
    /// a `layout` offset (1 x, 2 y, 3 scale x, 4 scale y), or 0
    pub(super) layout: u8,
    /// its value came from the element's `style` last frame (when that goes, so does the value)
    styled: bool,
}

/// An element's animated values: the engine's target object.
#[derive(Default)]
pub(crate) struct Slot {
    pub(super) props: SmallVec<[PropState; 4]>,
    seen: u64,
    /// its element's key, and where it is in the live list
    pub(super) key: u64,
    live: u32,
    /// After the last full frame: the element's box, its style before the
    /// moving values (they go over it again on redraws), whether a moving
    /// value changes layout (then a redraw can't do), and the MotionValues
    /// its `style` binds.
    pub(super) node: usize,
    base: Option<Box<Computed>>,
    layout: bool,
    /// what its values touch (a redraw skips the rest): x / y, opacity / visibility
    moves: bool,
    fades: bool,
    bound: SmallVec<[(i32, super::MotionValue); 2]>,
    /// what few elements use (in view, exiting, drag): out of the way of the rest
    pub(super) gest: Option<Box<Gest>>,
}

/// A slot's rarer state.
#[derive(Default)]
pub(super) struct Gest {
    /// `drag`: how it drags
    pub(super) drag: Option<super::spec::Drag>,
    /// `layout`: what animates, its box last layout (without its offsets), and the transition
    pub(super) layout: Option<super::spec::Layout>,
    pub(super) last_box: Option<crate::geometry::Bounds<f32>>,
    pub(super) layout_t: Transition,
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
    // SAFETY: the engine passes room for 4 floats and an int.
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
    // SAFETY: `value` holds `n` (clamped to 4) floats, `r.v` has room for 4.
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
pub(super) struct Arena {
    chunks: Vec<Box<[Slot; CHUNK]>>,
    pub(super) keys: KeyMap<u64, u32>,
    pub(super) live: Vec<u32>,
    free: Vec<u32>,
}

impl Arena {
    pub(super) fn at(&mut self, i: u32) -> &mut Slot {
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
        // SAFETY: the slot is alive. Every motion of it, started or not: the engine finds them
        // by their target, so a motion still waiting for its delay can't write a freed slot.
        unsafe { sys::BK_anim_kill_target(obj, -1) };
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
    pub(super) els: Arena,
    /// the drag in progress
    pub(super) drag: Option<super::drag::Dragging>,
    frame: u64,
    /// scratch: each box's move on a redraw
    shift: Vec<(f32, f32)>,
    /// the boxes a redraw changed (its elements and everything under them)
    dirty: Vec<bool>,
    /// a slot may be exiting (frames look for exits that ended)
    exits: bool,
    /// the boxes a redraw walks (see `build_region`), and scratch for building them
    region: Vec<u32>,
    mark: Vec<bool>,
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

    /// After a layout: which elements are in view now (`whileInView`), and
    /// the `layout` elements that moved. (render again: an in-view state
    /// changed; correct this frame: a layout animation started.)
    pub(crate) fn after_layout(&mut self, frame: &crate::layout::Frame, styles: &crate::layout::StyleStore,
                               viewport: (f32, f32)) -> (bool, bool) {
        let flipped = self.flip(frame, styles, viewport);
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
            // what the window shows of it, and what the scroll containers around it show
            let (mut x0, mut y0) = (b.origin.x.max(0.0), b.origin.y.max(0.0));
            let (mut x1, mut y1) = ((b.origin.x + b.size.width).min(viewport.0), (b.origin.y + b.size.height).min(viewport.1));
            if let Some(c) = n.clip {
                x0 = x0.max(c.origin.x);
                y0 = y0.max(c.origin.y);
                x1 = x1.min(c.origin.x + c.size.width);
                y1 = y1.min(c.origin.y + c.size.height);
            }
            let (w, h) = (x1 - x0, y1 - y0);
            let shown = if w > 0.0 && h > 0.0 { w * h / area } else { 0.0 };
            let inside = if v.amount <= 0.0 { shown > 0.0 } else { shown >= v.amount - 1e-4 };
            let now = inside || (v.once && g.in_view);
            if now != g.in_view {
                g.in_view = now;
                changed = true;
            }
        }
        self.check_exits();
        self.build_region(&frame.nodes);
        (changed, flipped)
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
                if self.drag.as_ref().is_some_and(|d| d.slot == i) {
                    self.drag = None; // the dragged element went
                }
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
        let none = Transition::default();
        // SAFETY (this and the short borrows below): no engine call runs while they live.
        let (in_view, exiting) = {
            let s = unsafe { &mut *slot };
            let view = spec.target(When::WhileInView).map(|_| spec.viewport());
            let exiting = spec.exiting().unwrap_or(0);
            let drag = spec.drag();
            let flip = spec.layout();
            if flip.is_some() {
                for k in [&super::flip::LAYOUT_X, &super::flip::LAYOUT_Y, &super::flip::LAYOUT_SX, &super::flip::LAYOUT_SY] {
                    ensure(s, k.id(), base, u);
                }
            }
            if let Some(d) = drag {
                // the pointer moves these: they're there from the start (a redraw can write them)
                for (on, id) in [(d.x, sys::BK_PROP_X), (d.y, sys::BK_PROP_Y)] {
                    if on {
                        ensure(s, id, base, u);
                    }
                }
            }
            if view.is_some() || exiting != 0 || drag.is_some() || flip.is_some() || s.gest.is_some() {
                let g = s.gest.get_or_insert_with(Box::default);
                g.view = view;
                g.drag = drag;
                g.layout = flip;
                if flip.is_some() {
                    // copied only when it changed: a frame reads it, never clones it
                    let wanted = spec.transition().map_or(&none, |v| &v.transition);
                    if g.layout_t != *wanted {
                        g.layout_t.clone_from(wanted);
                    }
                }
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
             self.drag.as_ref().is_some_and(|d| d.key == key).then_some(When::WhileDrag)]
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
            // `style={{ x: mv }}`: values read every frame; one the style no longer has (and no target
            // moves) stops holding the style's last value over the element's own
            let mut bound: SmallVec<[i32; 4]> = SmallVec::new();
            if let Some(style) = spec.style() {
                for (k, v) in style.props.iter() {
                    ensure(s, k.id(), base, u);
                    if let (Some(p), Some(r)) = (s.find(k.id()), v.read().filter(|r| !r.current)) {
                        p.cur = Some(r);
                        bound.push(k.id());
                    }
                }
            }
            for p in &mut s.props {
                let now = bound.contains(&p.id);
                if p.styled && !now && p.last.is_none() {
                    p.cur = None;
                }
                p.styled = now;
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
        // start a motion for each target that changed (the short `unsafe` borrows of the slot below: no engine call runs while one lives, and the engine has dropped none of its own)
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
        s.moves = s.props.iter().any(|p| matches!(p.id, sys::BK_PROP_X | sys::BK_PROP_Y) || matches!(p.layout, 1 | 2));
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

    /// After a full frame: the boxes a redraw can change, in order (parents
    /// first): the animated elements' boxes and everything under them. A
    /// redraw walks these alone, not every box of the frame.
    fn build_region(&mut self, nodes: &[crate::layout::BoxNode]) {
        self.region.clear();
        self.dirty.clear(); // the old marks aren't the new region's
        if self.els.live.is_empty() {
            return;
        }
        let mark = &mut self.mark;
        mark.clear();
        mark.resize(nodes.len(), false);
        let mut any = false;
        for &i in &self.els.live {
            let s = &self.els.chunks[i as usize / CHUNK][i as usize % CHUNK];
            if s.base.is_some() && nodes.get(s.node).is_some_and(|n| n.key == s.key) {
                mark[s.node] = true;
                any = true;
            }
        }
        if !any {
            return;
        }
        for (j, n) in nodes.iter().enumerate() {
            mark[j] |= n.parent.is_some_and(|p| mark[p]);
            if mark[j] {
                self.region.push(j as u32);
            }
        }
    }

    /// Zero the moves a redraw left (what the next one starts from).
    fn clear_shift(&mut self) {
        for &j in &self.region {
            if let Some(s) = self.shift.get_mut(j as usize) {
                *s = (0.0, 0.0);
            }
        }
    }

    /// A frame where only moving values changed (no render, cascade or
    /// layout): each element's values go over its style again, and boxes
    /// moved by `x` / `y` move with their contents. False when that can't do
    /// (a moving value changes layout, or the frame isn't the one the slots
    /// know): the window then renders as usual. It touches the animated
    /// elements and what is under them, not the rest of the frame.
    pub(crate) fn redraw(&mut self, frame: &mut crate::layout::Frame, styles: &mut crate::layout::StyleStore,
                         viewport: (f32, f32)) -> bool {
        let Arena { chunks, live, .. } = &mut self.els;
        let at = |i: u32| (i as usize / CHUNK, i as usize % CHUNK);
        // a layout value moving now (also one whose first value came after the last full frame)
        if live.iter().any(|&i| {
            let (c, o) = at(i);
            let s = &chunks[c][o];
            s.base.is_some() && (s.layout || s.props.iter().any(|p| p.cur.is_some() && !paint_only(p)))
        }) {
            return false;
        }
        let nodes = &mut frame.nodes;
        let mut moved = false;
        let mut faded = false;
        if self.dirty.len() == nodes.len() {
            for &j in &self.region {
                self.dirty[j as usize] = false;
            }
        } else {
            self.dirty.clear();
            self.dirty.resize(nodes.len(), false);
        }
        work(live.len() + self.region.len());
        for &i in live.iter() {
            let (c, o) = at(i);
            let s = &mut chunks[c][o];
            let Some(base) = &s.base else { continue };
            let Some(n) = nodes.get(s.node).filter(|n| n.key == s.key) else {
                self.clear_shift();
                return false;
            };
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
                if self.shift.len() < nodes.len() {
                    self.shift.resize(nodes.len(), (0.0, 0.0));
                }
                self.shift[s.node] = (after.0 - before.0, after.1 - before.1);
                moved = true;
            }
        }
        if faded {
            // boxes keep their opacity times their parents'
            crate::layout::inherit_at(nodes, styles, self.region.iter().map(|&j| j as usize));
        }
        self.check_exits();
        // what's under a changed box changes with it (opacity, position)
        for &j in &self.region {
            let j = j as usize;
            if let Some(p) = nodes[j].parent {
                self.dirty[j] |= self.dirty[p];
            }
        }
        if moved {
            // a box moves with every box under it (parents come before children)
            for &j in &self.region {
                let j = j as usize;
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
                    if let (true, Some(c)) = (inner, &mut nodes[j].clip)
                        && parent_clip != Some(*c) {
                            c.origin.x += d.0;
                            c.origin.y += d.1;
                        }
                }
            }
            self.clear_shift();
        }
        true
    }
}


/// A motion of property `id` of the slot to `keys` with transition `t`: the
/// handle is the caller's.
pub(super) fn start(slot: *mut Slot, id: i32, keys: &[Value], t: &Transition) -> *mut sys::BKMotion {
    tween(&SLOT_CLASS, slot.cast(), &[(id, keys)], t, Flavor::Motion, false, None)
}

thread_local! {
    /// Slots and boxes the redraws on this thread visited (see [`redraw_work`](super::redraw_work)).
    static WORK: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
}

/// A redraw visits `n` slots and boxes.
fn work(n: usize) {
    WORK.set(WORK.get() + n as u64);
}

/// What the redraws visited so far on this thread.
pub(super) fn redraw_work() -> u64 {
    WORK.get()
}

fn ensure(s: &mut Slot, id: i32, base: &Computed, u: &Units) {
    if s.find(id).is_none() {
        let mut p = PropState { id, fields: fields_for(id), cur: None, base: Read::default(), last: None,
                                layout: super::flip::role(id), styled: false };
        p.base = base_of(&p, base, u);
        s.props.push(p);
    }
}
