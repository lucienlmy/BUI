//! The imperative API: GSAP's `gsap.to / from / fromTo / set / timeline` and
//! Motion's `animate`, on [`MotionValue`]s and on elements by selector
//! (`"#id"`, `".class"`, `"tag"`, comma-separated), with [`Tween`] /
//! [`Timeline`] handles that control them.

use std::borrow::Cow;
use std::ffi::CString;
use std::ptr::NonNull;

use smallvec::SmallVec;

use blenderui_sys as sys;

use super::MotionValue;
use super::pending::{Pending, Selector, queue};
use super::value::Value;
use super::vars::{Flavor, Transition, Vars};

/// A running (or paused) tween or timeline. Dropping the handle lets it run
/// on; [`kill`](Self::kill) stops it.
#[derive(Debug)]
pub struct Tween(NonNull<sys::BKMotion>);

impl Drop for Tween {
    fn drop(&mut self) {
        // SAFETY: the handle this owns.
        unsafe { sys::BK_motion_release(self.0.as_ptr()) };
    }
}

impl Clone for Tween {
    fn clone(&self) -> Tween {
        // SAFETY: one more handle.
        unsafe { sys::BK_motion_retain(self.0.as_ptr()) };
        Tween(self.0)
    }
}

macro_rules! controls {
    ($($(#[$doc:meta])* $name:ident => $f:ident;)*) => {
        $( $(#[$doc])* pub fn $name(&self) -> &Self {
            // SAFETY: a live motion.
            unsafe { sys::$f(self.0.as_ptr()) };
            crate::hooks::wake_all();
            self
        } )*
    };
}

macro_rules! queries {
    ($($(#[$doc:meta])* $name:ident: $ty:ty => $f:ident;)*) => {
        $( $(#[$doc])* #[must_use] pub fn $name(&self) -> $ty {
            // SAFETY: a live motion.
            unsafe { sys::$f(self.0.as_ptr()) }
        } )*
    };
}

macro_rules! flags {
    ($($(#[$doc:meta])* $name:ident => $f:ident;)*) => {
        $( $(#[$doc])* #[must_use] pub fn $name(&self) -> bool {
            // SAFETY: a live motion.
            unsafe { sys::$f(self.0.as_ptr()) != 0 }
        } )*
    };
}

impl Tween {
    /// Take one handle of `m` (a motion just made; a null one is an empty
    /// timeline, so the handle is never dangling).
    pub(crate) fn from_raw(m: *mut sys::BKMotion) -> Tween {
        match NonNull::new(m) {
            Some(m) => Tween(m),
            // the engine is off: an empty, finished timeline stands in
            None => Tween(NonNull::new(unsafe { sys::BK_timeline(std::ptr::null()) }).unwrap_or(NonNull::dangling())),
        }
    }

    pub(crate) fn raw(&self) -> *mut sys::BKMotion {
        self.0.as_ptr()
    }

    controls! {
        /// Play forward from where it is.
        play => BK_motion_play;
        pause => BK_motion_pause;
        resume => BK_motion_resume;
        /// Turn around (and play).
        reverse => BK_motion_reverse;
        /// From the start.
        restart => BK_motion_restart;
        /// To its end now (Motion's `complete()`).
        complete => BK_motion_complete;
        /// Stop it where it is.
        kill => BK_motion_kill;
    }

    flags! {
        is_active => BK_motion_is_active;
        paused => BK_motion_paused;
        reversed => BK_motion_reversed;
    }

    queries! {
        /// Seconds into the current iteration.
        time: f64 => BK_motion_time;
        total_time: f64 => BK_motion_total_time;
        duration: f64 => BK_motion_duration;
        total_duration: f64 => BK_motion_total_duration;
        progress: f64 => BK_motion_progress;
        time_scale: f64 => BK_motion_time_scale;
    }

    /// Jump to `time` seconds.
    pub fn seek(&self, time: f64) -> &Self {
        // SAFETY: a live motion.
        unsafe { sys::BK_motion_seek(self.raw(), time) };
        self
    }

    /// Jump to `p` (0..1) of an iteration.
    pub fn set_progress(&self, p: f64) -> &Self {
        // SAFETY: a live motion.
        unsafe { sys::BK_motion_set_progress(self.raw(), p) };
        self
    }

    /// Jump to `p` (0..1) of the whole (with repeats).
    pub fn set_total_progress(&self, p: f64) -> &Self {
        // SAFETY: a live motion.
        unsafe { sys::BK_motion_set_total_progress(self.raw(), p) };
        self
    }

    pub fn set_time_scale(&self, s: f64) -> &Self {
        // SAFETY: a live motion.
        unsafe { sys::BK_motion_set_time_scale(self.raw(), s) };
        self
    }
}

/// What an imperative motion animates.
#[derive(Debug)]
pub enum Targets {
    Values(SmallVec<[MotionValue; 4]>),
    /// Elements, found at the next layout: `"#id"`, `".class"`, `"tag"`, a
    /// comma-separated list.
    Selector(Cow<'static, str>),
}

impl From<MotionValue> for Targets {
    fn from(v: MotionValue) -> Targets {
        Targets::Values(smallvec::smallvec![v])
    }
}

impl From<&[MotionValue]> for Targets {
    fn from(v: &[MotionValue]) -> Targets {
        Targets::Values(v.into())
    }
}

impl<const N: usize> From<[MotionValue; N]> for Targets {
    fn from(v: [MotionValue; N]) -> Targets {
        Targets::Values(v.into_iter().collect())
    }
}

impl From<&'static str> for Targets {
    fn from(s: &'static str) -> Targets {
        Targets::Selector(Cow::Borrowed(s))
    }
}

impl From<String> for Targets {
    fn from(s: String) -> Targets {
        Targets::Selector(Cow::Owned(s))
    }
}

/// The start offsets of `n` targets (GSAP's stagger `each`).
fn offsets(t: &Transition, n: usize) -> SmallVec<[f64; 8]> {
    (0..n).map(|i| t.stagger.unwrap_or(0.0) * i as f64).collect()
}

/// A position for `BK_timeline_add` (seconds, as text).
pub(crate) fn at(seconds: f64) -> CString {
    CString::new(format!("{seconds}")).unwrap_or_default()
}

/// Start `vars` on `targets`; `from`: GSAP's from(); `flavor`: whose defaults.
fn start(targets: Targets, vars: Vars, from: bool, flavor: Flavor) -> Tween {
    let Vars { props, transition, events, .. } = vars;
    match targets {
        // one value: one tween (of its first property)
        Targets::Values(v) if v.len() == 1 => {
            let keys = props.first().map_or(&[][..], |(_, x)| x.keys());
            v[0].drive(&[(sys::BK_PROP_VALUE, keys)], &transition, flavor, from, events)
        }
        // several (or elements later): a timeline, one tween per target at its offset
        targets => {
            let mut tv = Transition { delay: transition.delay, ..Transition::default() }.engine(flavor, 0, 1, 0.0);
            if let Some(e) = events {
                e.attach(&mut tv);
            }
            super::sync();
            // SAFETY: options read during the call.
            let tl = Tween::from_raw(unsafe { sys::BK_timeline(&raw const tv) });
            let each = Transition { delay: None, ..transition };
            match targets {
                Targets::Values(v) => {
                    let offs = offsets(&each, v.len());
                    let keys = props.first().map_or(&[][..], |(_, x)| x.keys());
                    for (mv, off) in v.iter().zip(offs) {
                        let m = mv.drive(&[(sys::BK_PROP_VALUE, keys)], &each, flavor, from, None);
                        // SAFETY: both alive; the timeline takes the child.
                        unsafe { sys::BK_timeline_add(tl.raw(), m.raw(), at(off).as_ptr()) };
                    }
                }
                Targets::Selector(sel) => queue(Pending {
                    selector: Selector::parse(&sel),
                    props: props.into_owned(),
                    transition: each,
                    from,
                    flavor,
                    hold: hold(&tl),
                    timeline: tl.clone(),
                    matched: 0,
                }),
            }
            crate::hooks::wake_all();
            tl
        }
    }
}

/// A selector's elements are found at the next layout: until then its
/// timeline holds this (nothing, for ever) so it doesn't end empty.
fn hold(tl: &Tween) -> Tween {
    static NOTHING: sys::BKAnimClass = sys::BKAnimClass { get: None, set: None, retain: None, release: None };
    // SAFETY: plain values; a tween of no tracks never reads its object.
    let h = unsafe {
        let mut v = sys::BK_motion_vars_default();
        v.duration = 1e9;
        Tween::from_raw(sys::BK_tween(&raw const NOTHING, std::ptr::null_mut(), std::ptr::null(), 0, &raw const v))
    };
    // SAFETY: both alive; the timeline takes it.
    unsafe { sys::BK_timeline_add(tl.raw(), h.raw(), c"0".as_ptr()) };
    h
}

/// GSAP's API: `gsap::to("#box", vars!{ x: 100, duration: 1 })`.
pub mod gsap {
    use super::{Targets, Tween, Value, Vars, start};
    use crate::motion::Flavor;

    /// Animate `targets` from where they are to `vars`.
    pub fn to(targets: impl Into<Targets>, vars: Vars) -> Tween {
        start(targets.into(), vars, false, Flavor::Gsap)
    }

    /// Animate `targets` from `vars` to where they are.
    pub fn from(targets: impl Into<Targets>, mut vars: Vars) -> Tween {
        vars.transition.immediate.get_or_insert(true);
        start(targets.into(), vars, true, Flavor::Gsap)
    }

    /// Animate `targets` from `from` to `to` (the timing is `to`'s).
    pub fn from_to(targets: impl Into<Targets>, from: Vars, mut to: Vars) -> Tween {
        // two keyframes per property: from's value, then to's
        let props = to.props.iter().map(|(k, v)| {
            let start = from.props.iter().find(|(f, _)| f.id() == k.id()).map_or(Value::Current, |(_, x)| x.clone());
            (*k, Value::Keys(vec![start, v.clone()].into()))
        }).collect::<Vec<_>>();
        to.props = props.into();
        to.transition.immediate.get_or_insert(true);
        start(targets.into(), to, false, Flavor::Gsap)
    }

    /// Set `vars` now (a zero-length tween).
    pub fn set(targets: impl Into<Targets>, mut vars: Vars) -> Tween {
        vars.transition.duration = Some(0.0);
        start(targets.into(), vars, false, Flavor::Gsap)
    }

    /// A timeline: place tweens on it with [`Timeline::to`](super::Timeline::to) ...
    pub fn timeline(vars: Vars) -> super::Timeline {
        super::Timeline::new(&vars)
    }
}

/// Motion's `animate(target, keyframes, options)`: springs for transforms,
/// eased tweens for the rest, unless `vars` says otherwise.
pub fn animate(targets: impl Into<Targets>, vars: Vars) -> Tween {
    start(targets.into(), vars, false, Flavor::Motion)
}

/// A GSAP timeline: tweens placed in time (GSAP's position parameter:
/// `"<"`, `">-0.2"`, `"+=1"`, `"label"`, `"label+=0.5"`, `"50%"`, `""` = the end).
#[derive(Clone, Debug)]
pub struct Timeline(pub Tween);

impl std::ops::Deref for Timeline {
    type Target = Tween;
    fn deref(&self) -> &Tween {
        &self.0
    }
}

impl Timeline {
    fn new(vars: &Vars) -> Timeline {
        let v = vars.transition.engine(Flavor::Gsap, 0, 1, 0.0);
        super::sync();
        // SAFETY: options read during the call.
        Timeline(Tween::from_raw(unsafe { sys::BK_timeline(&raw const v) }))
    }

    /// Do `op` now, or (this timeline waits for elements) after the layout
    /// that finds them, in order.
    fn then(&self, op: Deferred, waits: bool) -> &Self {
        if waits || DEFERRED.with(|d| with_deferred(d, |v| v.iter().any(|(tl, _)| tl.raw() == self.0.raw()))) {
            DEFERRED.with(|d| with_deferred(d, |v| v.push((self.0.clone(), op))));
        } else {
            op.run(&self.0);
        }
        self
    }

    fn place(&self, t: &Tween, position: &str) -> &Self {
        // a selector's elements are found at the next layout: until then it has no length
        self.then(Deferred::Place(t.clone(), CString::new(position).unwrap_or_default()), super::pending::waits(t.raw()))
    }

    /// `tl.to(targets, vars, position)`.
    pub fn to(&self, targets: impl Into<Targets>, vars: Vars, position: &str) -> &Self {
        self.place(&gsap::to(targets, vars), position)
    }

    pub fn from(&self, targets: impl Into<Targets>, vars: Vars, position: &str) -> &Self {
        self.place(&gsap::from(targets, vars), position)
    }

    pub fn from_to(&self, targets: impl Into<Targets>, from: Vars, to: Vars, position: &str) -> &Self {
        self.place(&gsap::from_to(targets, from, to), position)
    }

    pub fn set(&self, targets: impl Into<Targets>, vars: Vars, position: &str) -> &Self {
        self.place(&gsap::set(targets, vars), position)
    }

    /// Place a tween (or timeline) made elsewhere.
    pub fn add(&self, t: &Tween, position: &str) -> &Self {
        self.place(t, position)
    }

    pub fn add_label(&self, name: &str, position: &str) -> &Self {
        let (n, p) = (CString::new(name).unwrap_or_default(), CString::new(position).unwrap_or_default());
        self.then(Deferred::Label(n, p), false)
    }

    /// Run `f` when the playhead passes `position` (GSAP's `call()`).
    pub fn call(&self, f: impl FnMut() + 'static, position: &str) -> &Self {
        self.then(Deferred::Call(Box::new(Box::new(f)), CString::new(position).unwrap_or_default()), false)
    }

    /// When label `name` is (seconds), if it exists (once the elements of
    /// selectors placed before it are found).
    #[must_use]
    pub fn label_time(&self, name: &str) -> Option<f64> {
        let n = CString::new(name).unwrap_or_default();
        // SAFETY: a live timeline.
        let t = unsafe { sys::BK_timeline_label_time(self.0.raw(), n.as_ptr()) };
        (t >= 0.0).then_some(t)
    }
}

/// A timeline's placement, label or call.
enum Deferred {
    Place(Tween, CString),
    Label(CString, CString),
    Call(Box<Box<dyn FnMut()>>, CString),
}

impl Deferred {
    fn run(self, tl: &Tween) {
        unsafe extern "C" fn call(_m: *mut sys::BKMotion, _ev: i32, data: *mut std::ffi::c_void) {
            // SAFETY: the box handed over below, alive while the call is.
            let f = unsafe { &mut *data.cast::<Box<dyn FnMut()>>() };
            f();
        }
        unsafe extern "C" fn free(data: *mut std::ffi::c_void) {
            // SAFETY: the box handed over below.
            drop(unsafe { Box::from_raw(data.cast::<Box<dyn FnMut()>>()) });
        }
        // SAFETY (all three): a live timeline (and child); the timeline takes the closure (and frees it).
        match self {
            Deferred::Place(t, p) => unsafe { sys::BK_timeline_add(tl.raw(), t.raw(), p.as_ptr()); },
            Deferred::Label(n, p) => unsafe { sys::BK_timeline_label(tl.raw(), n.as_ptr(), p.as_ptr()); },
            Deferred::Call(f, p) => unsafe {
                sys::BK_timeline_call(tl.raw(), Some(call), Box::into_raw(f).cast(), Some(free), p.as_ptr());
            },
        }
    }
}

thread_local! {
    /// Timelines' operations waiting for a layout to find their selectors' elements.
    static DEFERRED: std::cell::Cell<Vec<(Tween, Deferred)>> = const { std::cell::Cell::new(Vec::new()) };
}

fn with_deferred<R>(c: &std::cell::Cell<Vec<(Tween, Deferred)>>, f: impl FnOnce(&mut Vec<(Tween, Deferred)>) -> R) -> R {
    let mut v = c.take();
    let r = f(&mut v);
    c.set(v);
    r
}

/// After a layout found the selectors' elements: the timelines' waiting
/// operations, in the order they were made.
pub(crate) fn run_deferred() {
    for (tl, op) in DEFERRED.with(std::cell::Cell::take) {
        op.run(&tl);
    }
}

