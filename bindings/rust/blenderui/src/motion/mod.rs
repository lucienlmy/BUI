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
mod value;
mod vars;

pub use ease::{Ease, ease};
pub use element::{Spec, Target, Variants};
pub(crate) use element::{Elements, Gestures};
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

/// A motion is about to start on this thread: from now on the windows'
/// clocks drive the engine (not the kit's frames).
pub(crate) fn used() {
    if !USED.replace(true) {
        // SAFETY: plain value.
        unsafe { sys::BK_anim_set_host_clock(1) };
    }
}

/// Advance every motion to `now` (seconds): once per frame, before layout.
pub(crate) fn tick(now: f64) {
    if USED.get() {
        // SAFETY: UI thread; the targets the engine writes are alive (they're
        // forgotten before they're freed).
        unsafe { sys::BK_anim_tick(now) };
    }
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
