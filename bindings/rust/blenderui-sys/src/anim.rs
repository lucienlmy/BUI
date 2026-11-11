//! `kit/include/BUI_anim.h`: one engine for GSAP- and Motion-style animation.
//! Without the `anim` feature the functions exist and do nothing
//! (`BK_anim_available()` is 0).

use std::os::raw::{c_char, c_double, c_float, c_int, c_void};

pub const BK_EASE_LINEAR: u8 = 0;

/// An ease, parsed once (`BK_ease_parse`).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BKEase {
    pub kind: u8,
    pub mode: u8,
    pub table: u16,
    pub p: [c_float; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BKSpring {
    pub stiffness: c_float,
    pub damping: c_float,
    pub mass: c_float,
    pub velocity: c_float,
    pub rest_delta: c_float,
    pub rest_speed: c_float,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BKInertia {
    pub velocity: c_float,
    pub power: c_float,
    pub time_constant: c_float,
    pub min: c_float,
    pub max: c_float,
    pub bounce_stiffness: c_float,
    pub bounce_damping: c_float,
    pub rest_delta: c_float,
}

pub type BKAnimGet = unsafe extern "C" fn(obj: *mut c_void, prop: c_int, r_value: *mut c_float, r_unit: *mut c_int) -> c_int;
pub type BKAnimSet = unsafe extern "C" fn(obj: *mut c_void, prop: c_int, value: *const c_float, n: c_int, unit: c_int);

/// What gets animated: reads and writes properties of `obj` by id.
#[repr(C)]
pub struct BKAnimClass {
    pub get: Option<BKAnimGet>,
    pub set: Option<BKAnimSet>,
    pub retain: Option<unsafe extern "C" fn(obj: *mut c_void)>,
    pub release: Option<unsafe extern "C" fn(obj: *mut c_void)>,
}

pub const BK_UNIT_NONE: c_int = 0;
pub const BK_UNIT_PX: c_int = 1;
pub const BK_UNIT_PERCENT: c_int = 2;
pub const BK_UNIT_EM: c_int = 3;
pub const BK_UNIT_REM: c_int = 4;
pub const BK_UNIT_VW: c_int = 5;
pub const BK_UNIT_VH: c_int = 6;
pub const BK_UNIT_DEG: c_int = 7;
pub const BK_UNIT_RAD: c_int = 8;
pub const BK_UNIT_TURN: c_int = 9;
pub const BK_UNIT_COLOR: c_int = 10;

pub const BK_PROP_X: c_int = 1;
pub const BK_PROP_Y: c_int = 2;
pub const BK_PROP_Z: c_int = 3;
pub const BK_PROP_SCALE: c_int = 4;
pub const BK_PROP_SCALE_X: c_int = 5;
pub const BK_PROP_SCALE_Y: c_int = 6;
pub const BK_PROP_ROTATE: c_int = 7;
pub const BK_PROP_ROTATE_X: c_int = 8;
pub const BK_PROP_ROTATE_Y: c_int = 9;
pub const BK_PROP_SKEW_X: c_int = 10;
pub const BK_PROP_SKEW_Y: c_int = 11;
pub const BK_PROP_OPACITY: c_int = 12;
pub const BK_PROP_WIDTH: c_int = 13;
pub const BK_PROP_HEIGHT: c_int = 14;
pub const BK_PROP_COLOR: c_int = 15;
pub const BK_PROP_BACKGROUND_COLOR: c_int = 16;
pub const BK_PROP_VALUE: c_int = 17;
pub const BK_PROP__FIXED: c_int = 18;

/// A tween or timeline.
#[repr(C)]
pub struct BKMotion {
    _private: [u8; 0],
}

/// One property's keyframes.
#[repr(C)]
pub struct BKTrack {
    pub prop: c_int,
    pub n: c_int,
    pub nkeys: c_int,
    pub values: *const c_float,
    pub current: *const u8,
    pub rel: *const i8,
    pub times: *const c_float,
    pub eases: *const BKEase,
    pub unit: c_int,
    pub from: c_int,
}

pub const BK_MOTION_TWEEN_KIND: c_int = 0;
pub const BK_MOTION_SPRING_KIND: c_int = 1;
pub const BK_MOTION_INERTIA_KIND: c_int = 2;
pub const BK_OVERWRITE_NONE: c_int = 0;
pub const BK_OVERWRITE_AUTO: c_int = 1;
pub const BK_OVERWRITE_ALL: c_int = 2;

pub const BK_EV_MOTION_START: c_int = 1 << 0;
pub const BK_EV_MOTION_UPDATE: c_int = 1 << 1;
pub const BK_EV_MOTION_COMPLETE: c_int = 1 << 2;
pub const BK_EV_MOTION_REPEAT: c_int = 1 << 3;
pub const BK_EV_MOTION_REVERSE_COMPLETE: c_int = 1 << 4;
pub const BK_EV_MOTION_INTERRUPT: c_int = 1 << 5;

pub type BKMotionFn = Option<unsafe extern "C" fn(m: *mut BKMotion, event: c_int, data: *mut c_void)>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct BKMotionVars {
    pub duration: c_double,
    pub delay: c_double,
    pub repeat: c_int,
    pub repeat_delay: c_double,
    pub yoyo: c_int,
    pub time_scale: c_double,
    pub paused: c_int,
    pub reversed: c_int,
    pub ease: BKEase,
    pub kind: c_int,
    pub spring: BKSpring,
    pub inertia: BKInertia,
    pub overwrite: c_int,
    pub immediate: c_int,
    pub on: BKMotionFn,
    pub events: c_int,
    pub data: *mut c_void,
    pub free_data: Option<unsafe extern "C" fn(data: *mut c_void)>,
}

#[repr(C)]
pub struct BKStagger {
    pub each: c_double,
    pub amount: c_double,
    pub from: *const c_char,
    pub from_index: c_int,
    pub grid_rows: c_int,
    pub grid_cols: c_int,
    pub axis: *const c_char,
    pub ease: BKEase,
}

unsafe extern "C" {
    pub fn BK_anim_available() -> c_int;
    pub fn BK_ease_parse(name: *const c_char, r_ease: *mut BKEase) -> c_int;
    pub fn BK_ease_eval(e: *const BKEase, t: c_float) -> c_float;
    pub fn BK_ease_bezier(x1: c_float, y1: c_float, x2: c_float, y2: c_float) -> BKEase;
    pub fn BK_ease_points(xy: *const [c_float; 2], n: c_int, r_ease: *mut BKEase) -> c_int;
    pub fn BK_ease_path(svg_path: *const c_char, r_ease: *mut BKEase) -> c_int;
    pub fn BK_ease_register(name: *const c_char, e: *const BKEase) -> c_int;
    pub fn BK_spring_default() -> BKSpring;
    pub fn BK_spring_bounce(bounce: c_float, visual_duration: c_float) -> BKSpring;
    pub fn BK_spring_duration(duration: c_float, bounce: c_float, velocity: c_float, mass: c_float) -> BKSpring;
    pub fn BK_spring_eval(s: *const BKSpring, from: c_float, to: c_float, t: c_double, r_pos: *mut c_float,
                          r_vel: *mut c_float);
    pub fn BK_spring_settle(s: *const BKSpring, from: c_float, to: c_float) -> c_double;
    pub fn BK_inertia_default(velocity: c_float) -> BKInertia;
    pub fn BK_anim_prop(name: *const c_char) -> c_int;
    pub fn BK_anim_prop_name(prop: c_int) -> *const c_char;
    pub fn BK_anim_parse_value(text: *const c_char, r_value: *mut c_float, r_unit: *mut c_int,
                               r_rel: *mut c_int) -> c_int;
    pub fn BK_motion_vars_default() -> BKMotionVars;
    pub fn BK_tween(cls: *const BKAnimClass, obj: *mut c_void, tracks: *const BKTrack, ntracks: c_int,
                    vars: *const BKMotionVars) -> *mut BKMotion;
    pub fn BK_timeline(vars: *const BKMotionVars) -> *mut BKMotion;
    pub fn BK_timeline_add(tl: *mut BKMotion, child: *mut BKMotion, position: *const c_char) -> c_double;
    pub fn BK_timeline_label(tl: *mut BKMotion, name: *const c_char, position: *const c_char) -> c_double;
    pub fn BK_timeline_call(tl: *mut BKMotion, f: BKMotionFn, data: *mut c_void,
                            free_data: Option<unsafe extern "C" fn(data: *mut c_void)>, position: *const c_char)
                            -> c_double;
    pub fn BK_timeline_label_time(tl: *const BKMotion, name: *const c_char) -> c_double;
    pub fn BK_stagger(s: *const BKStagger, count: c_int, r_offsets: *mut c_double);
    pub fn BK_motion_play(m: *mut BKMotion);
    pub fn BK_motion_pause(m: *mut BKMotion);
    pub fn BK_motion_resume(m: *mut BKMotion);
    pub fn BK_motion_reverse(m: *mut BKMotion);
    pub fn BK_motion_restart(m: *mut BKMotion);
    pub fn BK_motion_seek(m: *mut BKMotion, time: c_double);
    pub fn BK_motion_set_progress(m: *mut BKMotion, progress: c_double);
    pub fn BK_motion_set_total_progress(m: *mut BKMotion, progress: c_double);
    pub fn BK_motion_set_time_scale(m: *mut BKMotion, scale: c_double);
    pub fn BK_motion_complete(m: *mut BKMotion);
    pub fn BK_motion_kill(m: *mut BKMotion);
    pub fn BK_motion_is_active(m: *const BKMotion) -> c_int;
    pub fn BK_motion_paused(m: *const BKMotion) -> c_int;
    pub fn BK_motion_reversed(m: *const BKMotion) -> c_int;
    pub fn BK_motion_time(m: *const BKMotion) -> c_double;
    pub fn BK_motion_total_time(m: *const BKMotion) -> c_double;
    pub fn BK_motion_duration(m: *const BKMotion) -> c_double;
    pub fn BK_motion_total_duration(m: *const BKMotion) -> c_double;
    pub fn BK_motion_progress(m: *const BKMotion) -> c_double;
    pub fn BK_motion_time_scale(m: *const BKMotion) -> c_double;
    pub fn BK_motion_velocity(m: *const BKMotion, prop: c_int) -> c_float;
    pub fn BK_motion_retain(m: *mut BKMotion);
    pub fn BK_motion_release(m: *mut BKMotion);
    pub fn BK_anim_tick(now: c_double) -> c_int;
    pub fn BK_anim_active() -> c_int;
    pub fn BK_anim_set_host_clock(on: c_int);
    pub fn BK_anim_host_clock() -> c_int;
    pub fn BK_anim_set_reduced_motion(on: c_int);
    pub fn BK_anim_reduced_motion() -> c_int;
    pub fn BK_anim_set_time_scale(scale: c_double);
    pub fn BK_anim_set_lag_smoothing(threshold: c_double, adjusted: c_double);
    pub fn BK_anim_kill_target(obj: *mut c_void, prop: c_int);
    pub fn BK_anim_set_wake(wake: Option<unsafe extern "C" fn(data: *mut c_void)>, data: *mut c_void);
    pub fn BK_anim_exit();
    pub static BK_anim_node: BKAnimClass;
}
