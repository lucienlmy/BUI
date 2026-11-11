//! Animation: GSAP and Motion (Framer Motion) on one engine (`kit/intern/anim`,
//! feature `anim`).
//!
//! ```ignore
//! rsx! {
//!     <motion.div
//!         initial={{ opacity: 0, y: 20 }}
//!         animate={{ opacity: 1, y: 0 }}
//!         whileHover={{ scale: 1.05 }}
//!         whileTap={{ scale: 0.95 }}
//!         transition={{ type: "spring", stiffness: 300, damping: 20 }}
//!     >
//!         "Hello"
//!     </motion.div>
//! }
//! ```
//!
//! Every ease of GSAP, Motion, CSS and Blender is there by name; springs and
//! inertia are solved in closed form. Objects of literals (`vars!{}`, rsx's
//! `{{ }}`) are `static`s: their property ids and eases are resolved once per
//! call site, and a frame allocates nothing.

mod ease;
mod element;
mod motion_value;
mod tween;
mod value;
mod vars;

pub use ease::{Ease, ease};
pub use element::{Spec, Target, Variants};
pub(crate) use element::{Elements, Gestures, end_pending, pending};
pub use motion_value::{MotionValue, use_motion_value, use_spring, use_transform, use_velocity};
pub use tween::{Targets, Timeline, Tween, animate, gsap};
pub use value::{Key, Value, keys};
pub use vars::{Events, Flavor, Kind, Transition, Vars};

use blenderui_sys as sys;

/// Whether this build animates (feature `anim`); without it values jump to
/// their targets.
#[must_use]
pub fn available() -> bool {
    // SAFETY: no arguments.
    unsafe { sys::BK_anim_available() != 0 }
}

thread_local! {
    /// This (UI) thread started a motion: the engine needs its frames. Apps
    /// that never animate never call it.
    static USED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

thread_local! {
    /// The engine's clock on this thread: real time (from when the app
    /// started), plus what `App::advance_clock` added (all of it, headless).
    static CLOCK: std::cell::Cell<(Option<std::time::Instant>, f64)> = const { std::cell::Cell::new((None, 0.0)) };
}

/// An app starts on this thread: its clock is real time, or (headless)
/// virtual, moved only by [`advance`].
pub(crate) fn start_clock(real: bool) {
    CLOCK.set((real.then(std::time::Instant::now), 0.0));
}

/// `App::advance_clock`.
pub(crate) fn advance(seconds: f64) {
    let (origin, offset) = CLOCK.get();
    CLOCK.set((origin, offset + seconds));
}

fn now() -> f64 {
    let (origin, offset) = CLOCK.get();
    origin.map_or(0.0, |o| o.elapsed().as_secs_f64()) + offset
}

/// Advance every motion to now: once per frame, before layout (a second
/// call at the same time does nothing).
pub(crate) fn tick() {
    if USED.get() {
        // SAFETY: UI thread; the targets the engine writes are alive (they're
        // forgotten before they're freed).
        unsafe { sys::BK_anim_tick(now()) };
    }
}

/// A motion is about to start: the engine runs on this thread's clock (not
/// the kit's frames) and catches up with it, so the motion starts now even
/// after an idle stretch without frames.
pub(crate) fn sync() {
    if !USED.replace(true) {
        // SAFETY: plain value.
        unsafe { sys::BK_anim_set_host_clock(1) };
    }
    tick();
}

/// Whether anything is animating (windows keep rendering frames).
pub(crate) fn active() -> bool {
    // SAFETY: no arguments.
    USED.get() && unsafe { sys::BK_anim_active() != 0 }
}

/// Reduced motion (Motion's `reducedMotion="always"`, the OS setting):
/// transforms and layout jump to their ends; opacity and colors still fade.
pub fn set_reduced_motion(on: bool) {
    // SAFETY: plain value.
    unsafe { sys::BK_anim_set_reduced_motion(i32::from(on)) };
}

/// Every motion's speed (`gsap.globalTimeline.timeScale()`).
pub fn set_time_scale(scale: f64) {
    // SAFETY: plain value.
    unsafe { sys::BK_anim_set_time_scale(scale) };
}

/// GSAP's `lagSmoothing(threshold, adjusted)`; `threshold` 0 turns it off.
pub fn lag_smoothing(threshold: f64, adjusted: f64) {
    // SAFETY: plain values.
    unsafe { sys::BK_anim_set_lag_smoothing(threshold, adjusted) };
}
