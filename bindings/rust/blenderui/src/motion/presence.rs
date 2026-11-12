//! Motion's `AnimatePresence`: an element animates out (its `exit`) before it
//! goes.
//!
//! ```ignore
//! {...presence(open, || rsx! { <motion.div initial={{ opacity: 0 }} animate={{ opacity: 1 }}
//!                                        exit={{ opacity: 0 }}>"Hi"</motion.div> })}
//! ```
//!
//! Rust doesn't keep an element once a render stops making it, so the
//! presence keeps calling `build` while the element exits: the elements built
//! then with an `exit` take it as their only target, and when their exit
//! motions end the presence lets the element go (at once if none has an
//! `exit`). Shown again midway, it animates back from where it is.

use std::cell::Cell;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::element::{AnyElement, IntoElement};

thread_local! {
    /// The presence whose exiting element is being built (0: none), and
    /// whether something in it has an `exit`.
    static BUILDING: Cell<(u64, bool)> = const { Cell::new((0, false)) };
    /// Presences whose exits ended (their elements go at the next render).
    static DONE: Cell<Vec<u64>> = const { Cell::new(Vec::new()) };
}

static NEXT: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Copy, PartialEq, Eq)]
enum Phase {
    Gone,
    Present,
    Exiting,
}

/// The presence an `exit` being set belongs to (it marks the element exiting).
pub(super) fn exiting() -> Option<u64> {
    let (id, _) = BUILDING.get();
    (id != 0).then(|| {
        BUILDING.set((id, true));
        id
    })
}

/// A presence's exits ended: the element goes (windows render again).
pub(super) fn done(id: u64) {
    DONE.with(|d| {
        let mut v = d.take();
        if !v.contains(&id) {
            v.push(id);
        }
        d.set(v);
    });
    crate::hooks::wake_all();
}

/// A new presence (for a host that keeps elements in a tree of its own).
#[must_use]
pub fn presence_id() -> u64 {
    NEXT.fetch_add(1, Ordering::Relaxed)
}

/// Run `build` as presence `id` taking its elements away: the ones it builds with an `exit` take it as their
/// only target. Also whether any of them has an `exit` (if none, nothing animates out).
pub fn exiting_scope<R>(id: u64, build: impl FnOnce() -> R) -> (R, bool) {
    BUILDING.set((id, false));
    let r = build();
    let (_, any_exit) = BUILDING.replace((0, false));
    (r, any_exit)
}

/// Whether presence `id`'s exits ended (once: the host lets its elements go).
#[must_use]
pub fn exit_done(id: u64) -> bool {
    take_done(id)
}

fn take_done(id: u64) -> bool {
    DONE.with(|d| {
        let mut v = d.take();
        let found = v.iter().position(|&x| x == id).map(|i| v.swap_remove(i)).is_some();
        d.set(v);
        found
    })
}

/// The element `build` makes while `show` is true, and while it animates
/// out after (Motion's `AnimatePresence` around one child).
#[track_caller]
pub fn presence<E: IntoElement>(show: bool, build: impl FnOnce() -> E) -> Option<AnyElement> {
    let state = crate::hooks::use_signal_with(|| (presence_id(), Phase::Gone));
    let (id, was) = state.with(|s| *s);
    let phase = match (show, was) {
        (true, _) => {
            take_done(id); // shown again before it went: it stays
            Phase::Present
        }
        (false, Phase::Present) => Phase::Exiting,
        (false, Phase::Exiting) if take_done(id) => Phase::Gone,
        (false, p) => p,
    };
    let el = match phase {
        Phase::Gone => None,
        Phase::Present => Some(build().into_any_element()),
        Phase::Exiting => {
            let (el, any_exit) = exiting_scope(id, || build().into_any_element());
            any_exit.then_some(el) // nothing animates out: it goes now
        }
    };
    let phase = if phase == Phase::Exiting && el.is_none() { Phase::Gone } else { phase };
    if phase != was {
        state.update_quiet(|s| s.1 = phase);
    }
    el
}
