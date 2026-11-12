//! [`MotionValue`]: a value the engine animates without re-rendering anything
//! that doesn't read it (Motion's `useMotionValue`; GSAP tweens of a plain
//! object). `Copy` handles into a per-thread slab, like signals.
//!
//! Elements read them through `style={{ x: mv }}` each frame; `use_spring`,
//! `use_transform` and `use_velocity` derive one from another.

use std::cell::Cell;
use std::ffi::c_void;

use blenderui_sys as sys;

use super::value::{Read, Value};
use super::vars::{Flavor, Transition, Vars, tween};
use super::Tween;

struct Slot {
    v: Read,
    generation: u32,
    live: bool,
    /// the motion driving it (one handle held), for its velocity
    driver: *mut sys::BKMotion,
}

thread_local! {
    static SLOTS: Cell<Vec<Slot>> = const { Cell::new(Vec::new()) };
    static FREE: Cell<Vec<u32>> = const { Cell::new(Vec::new()) };
}

/// The slab, moved out of its cell while `f` runs (no engine call or user
/// code runs inside).
fn with<R>(f: impl FnOnce(&mut Vec<Slot>) -> R) -> R {
    SLOTS.with(|c| {
        let mut s = c.take();
        let r = f(&mut s);
        c.set(s);
        r
    })
}

/// A value the engine animates.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct MotionValue {
    i: u32,
    generation: u32,
}

/// Engine object: the slot index + 1.
fn obj(i: u32) -> *mut c_void {
    (i as usize + 1) as *mut c_void
}

unsafe extern "C" fn mv_get(o: *mut c_void, _prop: i32, r_value: *mut f32, r_unit: *mut i32) -> i32 {
    let i = (o as usize - 1) as u32;
    let r = with(|s| s.get(i as usize).filter(|s| s.live).map(|s| s.v));
    let Some(r) = r else { return 0 };
    // SAFETY: the engine passes room for 4 floats and an int.
    unsafe {
        std::ptr::copy_nonoverlapping(r.v.as_ptr(), r_value, 4);
        *r_unit = r.unit;
    }
    i32::from(r.n)
}

unsafe extern "C" fn mv_set(o: *mut c_void, _prop: i32, value: *const f32, n: i32, unit: i32) {
    let i = (o as usize - 1) as u32;
    let mut r = Read { n: n.clamp(1, 4) as u8, unit, ..Read::default() };
    // SAFETY: `value` holds `n` floats.
    unsafe { std::ptr::copy_nonoverlapping(value, r.v.as_mut_ptr(), r.n as usize) };
    with(|s| {
        if let Some(s) = s.get_mut(i as usize).filter(|s| s.live) {
            s.v = r;
        }
    });
}

pub(crate) static MV_CLASS: sys::BKAnimClass = sys::BKAnimClass { get: Some(mv_get), set: Some(mv_set), retain: None,
                                                                  release: None };

impl MotionValue {
    /// A new value (freed with [`dispose`](Self::dispose); [`use_motion_value`]
    /// frees its own when the component goes).
    pub fn new(init: impl Into<Value>) -> Self {
        let v = init.into().read().unwrap_or_default();
        let i = FREE.with(|f| {
            let mut free = f.take();
            let i = free.pop();
            f.set(free);
            i
        });
        with(|s| match i {
            Some(i) => {
                let slot = &mut s[i as usize];
                slot.v = v;
                slot.live = true;
                Self { i, generation: slot.generation }
            }
            None => {
                s.push(Slot { v, generation: 0, live: true, driver: std::ptr::null_mut() });
                Self { i: (s.len() - 1) as u32, generation: 0 }
            }
        })
    }

    /// Set it from the window (a scroll position): no wake, no motion
    /// stopped. True when it changed.
    pub(crate) fn set_quiet(self, r: Read) -> bool {
        with(|s| match s.get_mut(self.i as usize).filter(|s| s.live && s.generation == self.generation) {
            Some(slot) if slot.v != r => {
                slot.v = r;
                true
            }
            _ => false,
        })
    }

    pub(crate) fn alive(self) -> bool {
        with(|s| s.get(self.i as usize).is_some_and(|s| s.live && s.generation == self.generation))
    }

    /// Its value (the first number; colors: [`get4`](Self::get4)).
    #[must_use]
    pub fn get(self) -> f32 {
        self.get4()[0]
    }

    /// Its value as up to 4 numbers.
    #[must_use]
    pub fn get4(self) -> [f32; 4] {
        with(|s| s.get(self.i as usize).filter(|s| s.generation == self.generation).map_or([0.0; 4], |s| s.v.v))
    }

    pub(crate) fn read(self) -> Read {
        with(|s| s.get(self.i as usize).filter(|s| s.generation == self.generation).map_or(Read::default(), |s| s.v))
    }

    /// Set it now (stopping what animates it); windows draw it.
    pub fn set(self, v: impl Into<Value>) {
        if !self.alive() {
            return;
        }
        self.stop();
        let r = v.into().read();
        with(|s| {
            if let Some(r) = r {
                s[self.i as usize].v = r;
            }
        });
        crate::hooks::wake_all();
    }

    /// Stop what animates it, where it is.
    pub fn stop(self) {
        if self.alive() {
            // SAFETY: the engine object of a live value.
            unsafe { sys::BK_anim_kill_target(obj(self.i), -1) };
        }
    }

    /// Its velocity now (units per second).
    #[must_use]
    pub fn velocity(self) -> f32 {
        let d = with(|s| s.get(self.i as usize).filter(|s| s.generation == self.generation).map_or(std::ptr::null_mut(), |s| s.driver));
        // SAFETY: a motion we hold a handle to.
        if d.is_null() { 0.0 } else { unsafe { sys::BK_motion_velocity(d, sys::BK_PROP_VALUE) } }
    }

    /// Whether something animates it.
    #[must_use]
    pub fn is_animating(self) -> bool {
        let d = with(|s| s.get(self.i as usize).filter(|s| s.generation == self.generation).map_or(std::ptr::null_mut(), |s| s.driver));
        // SAFETY: a motion we hold a handle to.
        !d.is_null() && unsafe { sys::BK_motion_is_active(d) != 0 }
    }

    /// Animate it to `to` (Motion's `animate(value, to, transition)`): a
    /// spring when `t` says so, else a tween.
    pub fn animate(self, to: impl Into<Value>, t: impl Into<Vars>) -> Tween {
        let v: Vars = t.into();
        let to = to.into();
        self.drive(&[(sys::BK_PROP_VALUE, to.keys())], &v.transition, Flavor::Motion, false, v.events)
    }

    /// Start a motion of this value (the newest drives its velocity).
    pub(crate) fn drive(self, tracks: &[(i32, &[Value])], t: &Transition, flavor: Flavor, from: bool,
                        events: Option<Box<super::Events>>) -> Tween {
        let m = tween(&MV_CLASS, obj(self.i), tracks, t, flavor, from, events);
        // SAFETY: a motion just made; one more handle for the velocity.
        unsafe { sys::BK_motion_retain(m) };
        let old = with(|s| std::mem::replace(&mut s[self.i as usize].driver, m));
        if !old.is_null() {
            // SAFETY: the handle held for the last driver.
            unsafe { sys::BK_motion_release(old) };
        }
        crate::hooks::wake_all();
        Tween::from_raw(m)
    }

    /// Free it (its motions stop).
    pub fn dispose(self) {
        if !self.alive() {
            return;
        }
        self.stop();
        let d = with(|s| {
            let slot = &mut s[self.i as usize];
            slot.live = false;
            slot.generation = slot.generation.wrapping_add(1);
            std::mem::replace(&mut slot.driver, std::ptr::null_mut())
        });
        if !d.is_null() {
            // SAFETY: the handle held for the driver.
            unsafe { sys::BK_motion_release(d) };
        }
        FREE.with(|f| {
            let mut free = f.take();
            free.push(self.i);
            f.set(free);
        });
    }
}

impl From<MotionValue> for Value {
    fn from(mv: MotionValue) -> Self {
        Self::Motion(mv)
    }
}

/// A component's [`MotionValue`], freed when the component goes.
struct Owned(MotionValue);

impl Drop for Owned {
    fn drop(&mut self) {
        // at thread exit the slab may be gone before the component's state
        if SLOTS.try_with(|_| ()).is_ok() && FREE.try_with(|_| ()).is_ok() {
            self.0.dispose();
        }
    }
}

/// Motion's `useMotionValue(init)`: the same value on every render.
#[track_caller]
pub fn use_motion_value(init: impl Into<Value>) -> MotionValue {
    let mut init = Some(init);
    crate::hooks::use_signal_with(|| Owned(MotionValue::new(init.take().expect("init")))).with(|o| o.0)
}

/// Motion's `useSpring(source, options)`: follows `source` with a spring
/// (a number, or another value it springs after).
#[track_caller]
pub fn use_spring(source: impl Into<Value>, t: impl Into<Vars>) -> MotionValue {
    let source: Value = source.into();
    let start = match &source {
        Value::Motion(m) => Value::Num(m.get()),
        v => v.clone(),
    };
    let mv = use_motion_value(start);
    let target = match &source {
        Value::Motion(m) => m.get(),
        v => v.read().map_or(0.0, |r| r.v[0]),
    };
    // a new target (or a moving source): spring to it from where it is
    let last = crate::hooks::use_signal(f32::NAN);
    if last.get().to_bits() != target.to_bits() {
        last.update_quiet(|l| *l = target);
        let mut v: Vars = t.into();
        v.transition.kind = super::Kind::Spring;
        drop(mv.animate(target, v));
    }
    mv
}

/// Motion's `useTransform(value, input, output)`: `value` mapped through
/// the ranges (clamped), recomputed every render.
#[track_caller]
pub fn use_transform(source: MotionValue, input: &[f32], output: &[f32]) -> MotionValue {
    let mv = use_motion_value(0.0);
    let x = source.get();
    let n = input.len().min(output.len());
    let y = if n == 0 {
        x
    } else if x <= input[0] {
        output[0]
    } else if x >= input[n - 1] {
        output[n - 1]
    } else {
        let k = input.windows(2).position(|w| x >= w[0] && x <= w[1]).unwrap_or(0);
        let t = (x - input[k]) / (input[k + 1] - input[k]).max(f32::EPSILON);
        output[k] + (output[k + 1] - output[k]) * t
    };
    with(|s| s[mv.i as usize].v = Read { v: [y, 0.0, 0.0, 0.0], n: 1, ..Read::default() });
    mv
}

/// Motion's `useVelocity(value)`: its velocity, every render.
#[track_caller]
pub fn use_velocity(source: MotionValue) -> MotionValue {
    let mv = use_motion_value(0.0);
    let v = source.velocity();
    with(|s| s[mv.i as usize].v = Read { v: [v, 0.0, 0.0, 0.0], n: 1, ..Read::default() });
    mv
}
