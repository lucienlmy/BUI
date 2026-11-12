//! Motion's `drag`: the pointer moves an element's `x` / `y` (within its
//! constraints, past them with some give), and when let go it carries on
//! (inertia, stopping at the constraints with a spring), springs back to its
//! constraints, or to where it started.
//!
//! A drag writes the values itself (no motion runs while the pointer holds
//! it), so its frames are redraws: no render, no layout.

use blenderui_sys as sys;

use super::element::{Elements, Slot, start};
use super::spec::Drag;
use super::value::{Read, Value};
use super::vars::{Kind, Transition};

/// The drag in progress.
pub(super) struct Dragging {
    pub(super) key: u64,
    pub(super) slot: u32,
    spec: Drag,
    /// the values and the pointer when it was pressed
    from: (f32, f32),
    at: (f32, f32),
    /// the last pointer positions (time, x, y), for the velocity when let go
    samples: [(f64, f32, f32); 4],
    n: usize,
}

/// `v` past `lo .. hi` by `elastic` of the way (0: stopped there).
fn elastic(v: f32, lo: f32, hi: f32, e: f32) -> f32 {
    if v < lo {
        lo - (lo - v) * e
    } else if v > hi {
        hi + (v - hi) * e
    } else {
        v
    }
}

impl Elements {
    /// The pointer pressed on `path` (root to deepest): the deepest element
    /// that drags takes it. True when one did.
    pub(crate) fn pointer_down(&mut self, path: &[u64], x: f32, y: f32) -> bool {
        for &key in path.iter().rev() {
            let Some(&i) = self.els.keys.get(&key) else { continue };
            let s = self.els.at(i);
            let Some(spec) = s.gest.as_ref().and_then(|g| g.drag).filter(|d| d.x || d.y) else { continue };
            let value = |id: i32| s.props.iter().find(|p| p.id == id).and_then(|p| p.cur).map_or(0.0, |r| r.v[0]);
            let from = (value(sys::BK_PROP_X), value(sys::BK_PROP_Y));
            let obj: *mut std::ffi::c_void = (&raw mut *s).cast();
            // SAFETY: a live slot; the pointer holds it now, not its motions
            unsafe {
                sys::BK_anim_kill_target(obj, sys::BK_PROP_X);
                sys::BK_anim_kill_target(obj, sys::BK_PROP_Y);
            }
            let t = super::now();
            self.drag = Some(Dragging { key, slot: i, spec, from, at: (x, y), samples: [(t, x, y); 4], n: 1 });
            return true;
        }
        false
    }

    /// The pointer moved: a drag follows it. True when one did (redraw).
    pub(crate) fn pointer_move(&mut self, x: f32, y: f32) -> bool {
        let Some(d) = &mut self.drag else { return false };
        d.samples[d.n % 4] = (super::now(), x, y);
        d.n += 1;
        let c = d.spec.constraints.unwrap_or([f32::NEG_INFINITY, f32::INFINITY, f32::NEG_INFINITY, f32::INFINITY]);
        let nx = elastic(d.from.0 + x - d.at.0, c[0], c[1], d.spec.elastic);
        let ny = elastic(d.from.1 + y - d.at.1, c[2], c[3], d.spec.elastic);
        let (spec, slot) = (d.spec, d.slot);
        let s = self.els.at(slot);
        for (on, id, v) in [(spec.x, sys::BK_PROP_X, nx), (spec.y, sys::BK_PROP_Y, ny)] {
            if let Some(p) = s.props.iter_mut().filter(|_| on).find(|p| p.id == id) {
                p.cur = Some(Read { v: [v, 0.0, 0.0, 0.0], n: 1, unit: sys::BK_UNIT_PX, ..Read::default() });
            }
        }
        true
    }

    /// The pointer let go: the drag ends in inertia, or a spring back to its
    /// constraints or its origin. True when a drag ended.
    pub(crate) fn pointer_up(&mut self) -> bool {
        let Some(d) = self.drag.take() else { return false };
        // velocity: from the oldest sample kept to the newest
        let newest = d.samples[(d.n - 1) % 4];
        let oldest = d.samples[d.n.saturating_sub(4) % 4];
        let dt = (newest.0 - oldest.0) as f32;
        let vel = if dt > 1e-4 { ((newest.1 - oldest.1) / dt, (newest.2 - oldest.2) / dt) } else { (0.0, 0.0) };
        let c = d.spec.constraints;
        let slot: *mut Slot = self.els.at(d.slot);
        for (on, id, v, lo, hi) in [(d.spec.x, sys::BK_PROP_X, vel.0, c.map(|c| c[0]), c.map(|c| c[1])),
                                    (d.spec.y, sys::BK_PROP_Y, vel.1, c.map(|c| c[2]), c.map(|c| c[3]))] {
            if !on {
                continue;
            }
            // SAFETY: a live slot, not borrowed elsewhere
            let now = unsafe { &*slot }.props.iter().find(|p| p.id == id).and_then(|p| p.cur).map_or(0.0, |r| r.v[0]);
            let (lo, hi) = (lo.unwrap_or(f32::NEG_INFINITY), hi.unwrap_or(f32::INFINITY));
            let spring = Transition { kind: Kind::Spring, velocity: Some(v), stiffness: Some(500.0),
                                      damping: Some(40.0), ..Transition::default() };
            let (to, t) = if d.spec.snap_to_origin {
                (0.0, spring)
            } else if d.spec.momentum {
                let t = Transition { kind: Kind::Inertia, velocity: Some(v), min: lo.is_finite().then_some(lo),
                                     max: hi.is_finite().then_some(hi), ..Transition::default() };
                (now, t)
            } else if now < lo || now > hi {
                (now.clamp(lo, hi), spring)
            } else {
                continue; // stays where it was let go
            };
            let m = start(slot, id, &[Value::Num(to)], &t);
            // SAFETY: our handle (the global timeline holds it while it runs)
            unsafe { sys::BK_motion_release(m) };
        }
        true
    }
}
