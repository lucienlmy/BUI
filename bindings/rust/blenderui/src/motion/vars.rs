//! [`Vars`]: what to animate and how (GSAP's vars object, Motion's target +
//! `transition`), and how it becomes the engine's tracks.
//!
//! A `vars!{}` / rsx `{{ }}` object of literals is a `static`: no allocation,
//! its property ids and eases resolved once per call site.

use std::borrow::Cow;
use std::ffi::c_void;

use smallvec::SmallVec;

use blenderui_sys as sys;

use super::ease::Ease;
use super::value::{Key, Read, Value};

/// How a value moves.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Kind {
    /// The flavour's default: GSAP tweens; Motion springs for transforms and
    /// tweens for the rest.
    #[default]
    Auto,
    /// Duration and ease (`type: "tween"` / `"keyframes"`).
    Tween,
    /// A physical spring (`type: "spring"`).
    Spring,
    /// Decay from a velocity (`type: "inertia"` / `"decay"`).
    Inertia,
}

/// Whose defaults fill what a transition leaves out.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Flavor {
    /// 0.5 s, `power1.out`, one tween for every property.
    Gsap,
    /// Springs for x / y / scale / rotate, 0.3 s eased tweens for the rest,
    /// 0.8 s for keyframes; one motion per property.
    Motion,
}

/// Timing options (GSAP's vars, Motion's `transition`): `None` / unset take
/// the defaults (or an enclosing transition's).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Transition {
    pub kind: Kind,
    pub duration: Option<f64>,
    pub delay: Option<f64>,
    pub ease: Option<Ease>,
    /// per keyframe segment (Motion's `ease: [..]`)
    pub eases: Option<Cow<'static, [Ease]>>,
    /// keyframe offsets, 0..1 (Motion's `times`)
    pub times: Option<Cow<'static, [f32]>>,
    pub repeat: Option<i32>,
    pub repeat_delay: Option<f64>,
    pub yoyo: Option<bool>,
    pub time_scale: Option<f64>,
    pub paused: Option<bool>,
    pub reversed: Option<bool>,
    pub immediate: Option<bool>,
    /// GSAP's overwrite (`true` / `"auto"`: the default; `false`: none)
    pub overwrite: Option<bool>,
    /// GSAP's stagger `each` (seconds between targets)
    pub stagger: Option<f64>,
    // springs
    pub stiffness: Option<f32>,
    pub damping: Option<f32>,
    pub mass: Option<f32>,
    pub bounce: Option<f32>,
    pub visual_duration: Option<f32>,
    pub velocity: Option<f32>,
    pub rest_delta: Option<f32>,
    pub rest_speed: Option<f32>,
    // inertia
    pub power: Option<f32>,
    pub time_constant: Option<f32>,
    pub min: Option<f32>,
    pub max: Option<f32>,
    pub bounce_stiffness: Option<f32>,
    pub bounce_damping: Option<f32>,
}

macro_rules! or_fields {
    ($a:ident, $b:ident; $($f:ident),*) => {
        Transition { kind: if $a.kind == Kind::Auto { $b.kind } else { $a.kind }, $($f: $a.$f.clone().or_else(|| $b.$f.clone()),)* }
    };
}

impl Transition {
    /// These options, with `base`'s where these have none.
    #[must_use]
    pub fn or(&self, base: &Transition) -> Transition {
        let a = self;
        let b = base;
        or_fields!(a, b; duration, delay, ease, eases, times, repeat, repeat_delay, yoyo, time_scale, paused, reversed,
                   immediate, overwrite, stagger, stiffness, damping, mass, bounce, visual_duration, velocity,
                   rest_delta, rest_speed, power, time_constant, min, max, bounce_stiffness, bounce_damping)
    }

    /// The engine's options for a motion of these keyframes (`first`: the
    /// property, `to`: its last value), with `flavor`'s defaults.
    pub(crate) fn engine(&self, flavor: Flavor, prop: i32, nkeys: usize, to: f32) -> sys::BKMotionVars {
        // SAFETY: plain values.
        let mut v = unsafe { sys::BK_motion_vars_default() };
        let physical = matches!(prop, sys::BK_PROP_X | sys::BK_PROP_Y | sys::BK_PROP_Z | sys::BK_PROP_ROTATE
            | sys::BK_PROP_ROTATE_X | sys::BK_PROP_ROTATE_Y | sys::BK_PROP_SCALE | sys::BK_PROP_SCALE_X
            | sys::BK_PROP_SCALE_Y);
        let kind = match (self.kind, flavor) {
            (Kind::Auto, Flavor::Motion) if nkeys <= 1 && physical && self.duration.is_none()
                && self.ease.is_none() => Kind::Spring,
            (Kind::Auto, _) => Kind::Tween,
            (k, _) => k,
        };
        if flavor == Flavor::Motion {
            // Motion's defaults (getDefaultTransition, and its keyframes generator)
            v.duration = if nkeys > 1 { 0.8 } else { 0.3 };
            v.ease = if self.kind == Kind::Auto && self.duration.is_none() && nkeys <= 1 {
                Ease::bezier(0.25, 0.1, 0.35, 1.0).0
            } else {
                super::ease::ease(c"easeInOut").0
            };
        }
        if let Some(d) = self.duration {
            v.duration = d;
        }
        if let Some(e) = self.ease {
            v.ease = e.0;
        }
        v.delay = self.delay.unwrap_or(0.0);
        v.repeat = self.repeat.unwrap_or(0);
        v.repeat_delay = self.repeat_delay.unwrap_or(0.0);
        v.yoyo = i32::from(self.yoyo.unwrap_or(false));
        v.time_scale = self.time_scale.unwrap_or(1.0);
        v.paused = i32::from(self.paused.unwrap_or(false));
        v.reversed = i32::from(self.reversed.unwrap_or(false));
        v.immediate = i32::from(self.immediate.unwrap_or(false));
        if self.overwrite == Some(false) {
            v.overwrite = sys::BK_OVERWRITE_NONE;
        }
        match kind {
            Kind::Spring => {
                v.kind = sys::BK_MOTION_SPRING_KIND;
                v.spring = self.spring(flavor, prop, to);
            }
            Kind::Inertia => {
                v.kind = sys::BK_MOTION_INERTIA_KIND;
                // SAFETY: plain values.
                let mut i = unsafe { sys::BK_inertia_default(self.velocity.unwrap_or(0.0)) };
                macro_rules! set { ($($f:ident),*) => {$( if let Some(x) = self.$f { i.$f = x; } )*}; }
                set!(power, time_constant, min, max, bounce_stiffness, bounce_damping, rest_delta);
                v.inertia = i;
            }
            Kind::Tween | Kind::Auto => v.kind = sys::BK_MOTION_TWEEN_KIND,
        }
        v
    }

    /// A spring from these options: `bounce` / `visualDuration`, `duration`,
    /// or stiffness / damping / mass (Motion's three forms).
    fn spring(&self, flavor: Flavor, prop: i32, to: f32) -> sys::BKSpring {
        // SAFETY (all three): plain values.
        let mut s = if let Some(vd) = self.visual_duration {
            unsafe { sys::BK_spring_bounce(self.bounce.unwrap_or(0.25), vd) }
        } else if let Some(d) = self.duration.filter(|_| self.stiffness.is_none()) {
            unsafe { sys::BK_spring_duration(d as f32, self.bounce.unwrap_or(0.25), self.velocity.unwrap_or(0.0),
                                             self.mass.unwrap_or(1.0)) }
        } else {
            let mut s = unsafe { sys::BK_spring_default() };
            if flavor == Flavor::Motion && self.stiffness.is_none() && self.damping.is_none() {
                // Motion's default springs: under-damped for moves, critically for scales
                let scale = matches!(prop, sys::BK_PROP_SCALE | sys::BK_PROP_SCALE_X | sys::BK_PROP_SCALE_Y);
                s.stiffness = if scale { 550.0 } else { 500.0 };
                s.damping = if !scale { 25.0 } else if to == 0.0 { 2.0 * 550f32.sqrt() } else { 30.0 };
                s.rest_speed = 10.0;
            }
            s
        };
        macro_rules! set { ($($f:ident),*) => {$( if let Some(x) = self.$f { s.$f = x; } )*}; }
        set!(stiffness, damping, mass, velocity, rest_delta, rest_speed);
        s
    }
}

/// Callbacks of an imperative motion (GSAP's `onComplete` ..., Motion's
/// `onComplete` / `onUpdate`). They run on the UI thread during a frame.
#[derive(Default)]
pub struct Events {
    pub on_start: Option<Box<dyn FnMut()>>,
    pub on_update: Option<Box<dyn FnMut()>>,
    pub on_complete: Option<Box<dyn FnMut()>>,
    pub on_repeat: Option<Box<dyn FnMut()>>,
    pub on_reverse_complete: Option<Box<dyn FnMut()>>,
    pub on_interrupt: Option<Box<dyn FnMut()>>,
}

impl std::fmt::Debug for Events {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Events")
    }
}

impl Events {
    fn mask(&self) -> i32 {
        let mut m = 0;
        for (on, bit) in [(self.on_start.is_some(), sys::BK_EV_MOTION_START),
                          (self.on_update.is_some(), sys::BK_EV_MOTION_UPDATE),
                          (self.on_complete.is_some(), sys::BK_EV_MOTION_COMPLETE),
                          (self.on_repeat.is_some(), sys::BK_EV_MOTION_REPEAT),
                          (self.on_reverse_complete.is_some(), sys::BK_EV_MOTION_REVERSE_COMPLETE),
                          (self.on_interrupt.is_some(), sys::BK_EV_MOTION_INTERRUPT)] {
            if on {
                m |= bit;
            }
        }
        m
    }

    unsafe extern "C" fn dispatch(_m: *mut sys::BKMotion, ev: i32, data: *mut c_void) {
        // SAFETY: `data` is the `Events` this motion owns (freed with it).
        let e = unsafe { &mut *data.cast::<Events>() };
        let f = match ev {
            sys::BK_EV_MOTION_START => &mut e.on_start,
            sys::BK_EV_MOTION_UPDATE => &mut e.on_update,
            sys::BK_EV_MOTION_COMPLETE => &mut e.on_complete,
            sys::BK_EV_MOTION_REPEAT => &mut e.on_repeat,
            sys::BK_EV_MOTION_REVERSE_COMPLETE => &mut e.on_reverse_complete,
            sys::BK_EV_MOTION_INTERRUPT => &mut e.on_interrupt,
            _ => return,
        };
        if let Some(f) = f {
            f();
        }
    }

    unsafe extern "C" fn free(data: *mut c_void) {
        // SAFETY: the box handed over in `attach`.
        drop(unsafe { Box::from_raw(data.cast::<Events>()) });
    }

    /// Hand these callbacks to a motion's options (the motion frees them).
    pub(crate) fn attach(self: Box<Self>, v: &mut sys::BKMotionVars) {
        v.events = self.mask();
        v.on = Some(Events::dispatch);
        v.free_data = Some(Events::free);
        v.data = Box::into_raw(self).cast();
    }
}

/// What to animate and how: properties to values (or keyframes) and the
/// [`Transition`]. Made by `vars!{ x: 100, opacity: 0.5, duration: 1 }`, rsx's
/// `{{ .. }}`, or the builder ([`Vars::new`]).
#[derive(Debug, Default)]
pub struct Vars {
    pub props: Cow<'static, [(&'static Key, Value)]>,
    pub transition: Transition,
    /// Motion's per-property transitions (`transition: { x: { .. } }`).
    pub per: Cow<'static, [(&'static Key, Transition)]>,
    /// Imperative motions' callbacks.
    pub events: Option<Box<Events>>,
}

impl Vars {
    #[must_use]
    pub fn new() -> Vars {
        Vars::default()
    }

    /// Animate `key` to `value` (a number, `"50%"`, `"#f80"`, `[keyframes]` ...).
    #[must_use]
    pub fn set(mut self, key: &'static Key, value: impl Into<Value>) -> Vars {
        self.props.to_mut().push((key, value.into()));
        self
    }

    /// Change the timing options.
    #[must_use]
    pub fn with(mut self, f: impl FnOnce(&mut Transition)) -> Vars {
        f(&mut self.transition);
        self
    }

    /// The transition of property `id`: its own, else these vars', else `base`.
    pub(crate) fn transition_for(&self, id: i32, base: &Transition) -> Transition {
        let own = self.per.iter().find(|(k, _)| k.id() == id).map(|(_, t)| t);
        match own {
            Some(t) => t.or(&self.transition).or(base),
            None => self.transition.or(base),
        }
    }
}

macro_rules! shorthands {
    ($($method:ident => $key:ident),*) => {
        impl Vars {$(
            #[doc = concat!("Animate `", stringify!($method), "`.")]
            #[must_use]
            pub fn $method(self, value: impl Into<Value>) -> Vars {
                self.set(&super::value::keys::$key, value)
            }
        )*}
    };
}
shorthands!(x => X, y => Y, scale => SCALE, scale_x => SCALE_X, scale_y => SCALE_Y, rotate => ROTATE,
            skew_x => SKEW_X, skew_y => SKEW_Y, opacity => OPACITY, width => WIDTH, height => HEIGHT,
            color => COLOR, background_color => BACKGROUND_COLOR);

macro_rules! options {
    ($($method:ident: $ty:ty => $field:ident),*) => {
        impl Vars {$(
            #[doc = concat!("The transition's `", stringify!($field), "`.")]
            #[must_use]
            pub fn $method(mut self, v: $ty) -> Vars {
                self.transition.$field = Some(v.into());
                self
            }
        )*}
    };
}
options!(duration: f64 => duration, delay: f64 => delay, ease: Ease => ease, repeat: i32 => repeat,
         repeat_delay: f64 => repeat_delay, yoyo: bool => yoyo, time_scale: f64 => time_scale,
         stagger: f64 => stagger, stiffness: f32 => stiffness, damping: f32 => damping, mass: f32 => mass,
         bounce: f32 => bounce, visual_duration: f32 => visual_duration, velocity: f32 => velocity);

impl Vars {
    /// `type: "spring"`.
    #[must_use]
    pub fn spring(self) -> Vars {
        self.with(|t| t.kind = Kind::Spring)
    }

    /// `type: "inertia"`.
    #[must_use]
    pub fn inertia(self) -> Vars {
        self.with(|t| t.kind = Kind::Inertia)
    }

    /// Run `f` when the motion completes.
    #[must_use]
    pub fn on_complete(mut self, f: impl FnMut() + 'static) -> Vars {
        self.events.get_or_insert_with(Box::default).on_complete = Some(Box::new(f));
        self
    }

    /// Run `f` every frame the motion renders.
    #[must_use]
    pub fn on_update(mut self, f: impl FnMut() + 'static) -> Vars {
        self.events.get_or_insert_with(Box::default).on_update = Some(Box::new(f));
        self
    }

    /// Run `f` when the motion starts.
    #[must_use]
    pub fn on_start(mut self, f: impl FnMut() + 'static) -> Vars {
        self.events.get_or_insert_with(Box::default).on_start = Some(Box::new(f));
        self
    }
}

/// A motion of `obj` (of class `cls`): property `id` of each of `tracks` to its
/// keyframes, timed by `t` with `flavor`'s defaults; `from`: the keyframes
/// are where it starts and it goes to its current values (gsap.from). Unread
/// values are skipped. The motion runs in the global timeline (or `paused`);
/// the handle is the caller's.
pub(crate) fn tween(cls: &'static sys::BKAnimClass, obj: *mut c_void, tracks: &[(i32, &[Value])], t: &Transition,
                    flavor: Flavor, from: bool, events: Option<Box<Events>>) -> *mut sys::BKMotion {
    super::sync();
    let bufs: SmallVec<[(i32, TrackBuf); 4]> =
        tracks.iter().filter_map(|(id, keys)| Some((*id, TrackBuf::read(keys)?))).collect();
    let engine: SmallVec<[sys::BKTrack; 4]> = bufs.iter().map(|(id, b)| b.track(*id, t, from)).collect();
    let (first, nkeys, to) = bufs.first().map_or((0, 1, 0.0), |(id, b)| (*id, tracks[0].1.len(), b.last));
    let mut vars = t.engine(flavor, first, nkeys, to);
    if let Some(e) = events {
        e.attach(&mut vars);
    }
    // SAFETY: the tracks' buffers live through the call (the engine copies them); `obj`
    // stays valid while the engine may write it (its owner forgets it before freeing).
    unsafe { sys::BK_tween(cls, obj, engine.as_ptr(), engine.len() as i32, &raw const vars) }
}

/// The engine's track for keyframes `keys` (read once; the buffers live on
/// the stack for the call).
pub(crate) struct TrackBuf {
    values: SmallVec<[f32; 16]>,
    current: SmallVec<[u8; 4]>,
    rel: SmallVec<[i8; 4]>,
    pub n: usize,
    pub unit: i32,
    pub last: f32,
}

impl TrackBuf {
    /// `None` when a keyframe isn't a value this engine reads.
    pub(crate) fn read(keys: &[Value]) -> Option<TrackBuf> {
        let mut reads: SmallVec<[Read; 4]> = SmallVec::new();
        for k in keys {
            reads.push(k.read()?);
        }
        let n = reads.iter().filter(|r| !r.current).map(|r| r.n as usize).max().unwrap_or(1);
        let unit = reads.iter().rev().find(|r| !r.current).map_or(sys::BK_UNIT_NONE, |r| r.unit);
        let mut b = TrackBuf { values: SmallVec::new(), current: SmallVec::new(), rel: SmallVec::new(), n, unit,
                               last: 0.0 };
        for r in &reads {
            b.values.extend_from_slice(&r.v[..n]);
            b.current.push(u8::from(r.current));
            b.rel.push(r.rel);
        }
        b.last = reads.last().map_or(0.0, |r| r.v[0]);
        Some(b)
    }

    /// The track (borrowing this buffer: use it before the buffer goes).
    pub(crate) fn track(&self, prop: i32, t: &Transition, from: bool) -> sys::BKTrack {
        let nkeys = self.current.len();
        let times = t.times.as_deref().filter(|x| x.len() == nkeys);
        let eases = t.eases.as_deref().filter(|e| e.len() + 1 == nkeys);
        sys::BKTrack {
            prop,
            n: self.n as i32,
            nkeys: nkeys as i32,
            values: self.values.as_ptr(),
            current: if self.current.iter().any(|&c| c != 0) { self.current.as_ptr() } else { std::ptr::null() },
            rel: if self.rel.iter().any(|&r| r != 0) { self.rel.as_ptr() } else { std::ptr::null() },
            times: times.map_or(std::ptr::null(), <[f32]>::as_ptr),
            eases: eases.map_or(std::ptr::null(), |e| e.as_ptr().cast()),
            unit: self.unit,
            from: i32::from(from),
        }
    }
}
