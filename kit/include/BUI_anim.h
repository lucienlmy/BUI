/* BUI animation: the one engine behind GSAP-style and Motion-style animation in every
 * language (Rust, JavaScript, Python, Lua, C). Feature `anim` (BUI_ANIM); without it the
 * functions are there and do nothing (BK_anim_available() is 0).
 *
 *   easing     every GSAP, Motion, CSS and Blender ease, parsed once into a BKEase
 *   springs    Motion's physical springs and inertia, solved in closed form
 *   motions    tweens (one target's properties over time) and timelines (motions placed
 *              in time), with GSAP's controls; one clock, ticked once per frame
 *
 * Everything is parsed and precomputed when an animation is made; a frame is one pass over
 * the running tracks, with no allocation. UI-thread API.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when this build has the animation engine (feature `anim`). */
int BK_anim_available(void);

/* -------------------------------------------------------------------- */
/** \name Easing
 * \{ */

enum {
  BK_EASE_LINEAR = 0,
  BK_EASE_POWER,     /* p[0]: exponent (2 quad .. 5 quint) */
  BK_EASE_SINE,
  BK_EASE_EXPO,
  BK_EASE_CIRC,
  BK_EASE_BACK,      /* p[0]: overshoot */
  BK_EASE_ELASTIC,   /* p[0]: amplitude, p[1]: period */
  BK_EASE_BOUNCE,
  BK_EASE_BEZIER,    /* cubic-bezier(p[0], p[1], p[2], p[3]) */
  BK_EASE_STEPS,     /* p[0]: steps, p[1]: BK_STEPS_* */
  BK_EASE_TABLE,     /* linear() points and sampled curves: `table` in the ease pool */
  BK_EASE_SLOW,      /* GSAP's SlowMo: p[0] linear ratio, p[1] power, p[2] yoyo */
  BK_EASE_EXPO_SCALE, /* GSAP's ExpoScaleEase: p[0] start, p[1] end scale (of an inner ease: `table`) */
  BK_EASE_ANTICIPATE, /* Motion's anticipate */
};
/* How the base curve (an "in" curve) is used. Curves given as such (bezier, steps, tables)
 * are used as they are. */
enum { BK_EASE_IN = 0, BK_EASE_OUT, BK_EASE_IN_OUT, BK_EASE_AS_IS };
enum { BK_STEPS_END = 0, BK_STEPS_START, BK_STEPS_NONE, BK_STEPS_BOTH };

typedef struct BKEase {
  uint8_t kind; /* BK_EASE_* */
  uint8_t mode; /* BK_EASE_IN .. AS_IS */
  uint16_t table; /* a sampled curve or points in the ease pool (0: none) */
  float p[4];
} BKEase;

/* Every ease by name (GSAP, Motion, CSS and Blender spellings; case doesn't matter):
 *   GSAP:    none, power0..4, quad, cubic, quart, quint, strong, sine, expo, circ,
 *            back(overshoot), elastic(amplitude, period), bounce, with .in / .out / .inOut
 *            (alone: .out), the old Power2.easeOut and .config(...) forms, steps(n),
 *            slow(ratio, power, yoyo), rough({...}), expoScale(start, end, ease),
 *            wiggle(n, type) (CustomWiggle), customBounce(strength), registered names
 *   Motion:  linear, easeIn, easeOut, easeInOut, circIn/Out/InOut, backIn/Out/InOut,
 *            anticipate
 *   CSS:     ease, ease-in, ease-out, ease-in-out, step-start, step-end,
 *            steps(n, jump-start|jump-end|jump-none|jump-both|start|end),
 *            cubic-bezier(x1, y1, x2, y2), linear(0, 0.25 75%, 1)
 *   Blender: constant, linear, bezier (its default keyframe ease), and its easing types
 *            (sine .. elastic) with .in / .out / .inOut
 * Returns 1, or 0 for a name it doesn't know (`r_ease` then linear). */
int BK_ease_parse(const char *name, BKEase *r_ease);
/* The eased value of `t` (0 .. 1; outside it, the curve goes on). */
float BK_ease_eval(const BKEase *e, float t);
/* Motion's array form, and CSS's cubic-bezier(). */
BKEase BK_ease_bezier(float x1, float y1, float x2, float y2);
/* A curve through (x, y) points, x ascending from 0 to 1 (a Blender curve widget, Motion's
 * keyframe arrays): kept in the ease pool. */
int BK_ease_points(const float (*xy)[2], int n, BKEase *r_ease);
/* GSAP's CustomEase: an SVG path ("M0,0 C0.2,0.8 0.4,1 1,1"), normalized to 0..1. */
int BK_ease_path(const char *svg_path, BKEase *r_ease);
/* A name for an ease (CustomEase.create, gsap.registerEase): BK_ease_parse finds it. */
int BK_ease_register(const char *name, const BKEase *e);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Springs and inertia (Motion's, in closed form)
 * \{ */

typedef struct BKSpring {
  float stiffness; /* 100 */
  float damping;   /* 10 */
  float mass;      /* 1 */
  float velocity;  /* units per second at the start */
  float rest_delta, rest_speed; /* settled within these (0: Motion's defaults for the distance) */
} BKSpring;

BKSpring BK_spring_default(void);
/* Motion's newer form: how bouncy (0 none .. 1) and the time it visually takes. */
BKSpring BK_spring_bounce(float bounce, float visual_duration);
/* Motion's older form: the stiffness and damping that settle in `duration` with `bounce`. */
BKSpring BK_spring_duration(float duration, float bounce, float velocity, float mass);
/* Position and velocity `t` seconds into a spring from `from` to `to`. */
void BK_spring_eval(const BKSpring *s, float from, float to, double t, float *r_pos, float *r_vel);
/* When it settles (seconds): a spring's duration in a timeline. */
double BK_spring_settle(const BKSpring *s, float from, float to);

typedef struct BKInertia {
  float velocity;       /* units per second */
  float power;          /* 0.8 */
  float time_constant;  /* 0.325 s */
  float min, max;       /* bounds (min > max: none) */
  float bounce_stiffness, bounce_damping; /* the spring at a bound: 500, 10 */
  float rest_delta;     /* 0.5 */
} BKInertia;

BKInertia BK_inertia_default(float velocity);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Properties and values
 * \{ */

/* What gets animated: an object of a class that reads and writes properties by id. */
typedef struct BKAnimClass {
  /* The current value (up to 4 numbers: a length 1, a color 4); 0 when it has no such
   * property. `unit` is the unit it reads in (BK_UNIT_*). */
  int (*get)(void *obj, int prop, float r_value[4], int *r_unit);
  /* Write the animated value (`n` numbers in `unit`). */
  void (*set)(void *obj, int prop, const float *value, int n, int unit);
  void (*retain)(void *obj); /* optional: held while animated */
  void (*release)(void *obj);
} BKAnimClass;

enum {
  BK_UNIT_NONE = 0, BK_UNIT_PX, BK_UNIT_PERCENT, BK_UNIT_EM, BK_UNIT_REM, BK_UNIT_VW, BK_UNIT_VH,
  BK_UNIT_DEG, BK_UNIT_RAD, BK_UNIT_TURN, BK_UNIT_COLOR,
};

/* A property's id by name (interned once; "scaleX", "scale-x" and "scale_x" are one id).
 * The transform shorthands and the common ones have fixed ids (BK_PROP_*). */
int BK_anim_prop(const char *name);
const char *BK_anim_prop_name(int prop);
enum {
  BK_PROP_X = 1, BK_PROP_Y, BK_PROP_Z, BK_PROP_SCALE, BK_PROP_SCALE_X, BK_PROP_SCALE_Y,
  BK_PROP_ROTATE, BK_PROP_ROTATE_X, BK_PROP_ROTATE_Y, BK_PROP_SKEW_X, BK_PROP_SKEW_Y,
  BK_PROP_OPACITY, BK_PROP_WIDTH, BK_PROP_HEIGHT, BK_PROP_COLOR, BK_PROP_BACKGROUND_COLOR,
  BK_PROP_VALUE, BK_PROP__FIXED, /* the first interned id */
};

/* A value as text: "100", "100px", "50%", "2rem", "45deg", "0.5turn", "#f80", "#ff8800cc",
 * "rgb(255 128 0 / 50%)", "hsl(...)", a CSS color name; "+=50" / "-=50" (relative: `r_rel`
 * +1 / -1). Returns the numbers read (1, or 4 for a color), 0 when it isn't a value. */
int BK_anim_parse_value(const char *text, float r_value[4], int *r_unit, int *r_rel);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Motions: tweens and timelines
 * \{ */

typedef struct BKMotion BKMotion;

/* One property's keyframes in a tween. */
typedef struct BKTrack {
  int prop;              /* BK_anim_prop */
  int n;                 /* numbers per value: 1, or 4 for colors */
  int nkeys;             /* values: 1 (to), 2 (from, to) or more (keyframes) */
  const float *values;   /* nkeys * n */
  const uint8_t *current; /* per key: 1 = the value it has when the tween starts (NULL: none) */
  const int8_t *rel;     /* per key: +1 / -1 relative to the start value (NULL: none) */
  const float *times;    /* per key, 0 .. 1 (NULL: evenly spaced) */
  const BKEase *eases;   /* per segment (nkeys - 1) (NULL: the tween's ease) */
  int unit;              /* BK_UNIT_* of the values */
  int from;              /* 1: values[0..] are where it starts and it goes to the current value (gsap.from) */
} BKTrack;

enum { BK_MOTION_TWEEN_KIND = 0, BK_MOTION_SPRING_KIND, BK_MOTION_INERTIA_KIND };
enum { BK_OVERWRITE_NONE = 0, BK_OVERWRITE_AUTO, BK_OVERWRITE_ALL };

/* What a motion tells (BKMotionVars.on). */
enum {
  BK_EV_MOTION_START = 1 << 0,
  BK_EV_MOTION_UPDATE = 1 << 1,
  BK_EV_MOTION_COMPLETE = 1 << 2,
  BK_EV_MOTION_REPEAT = 1 << 3,
  BK_EV_MOTION_REVERSE_COMPLETE = 1 << 4,
  BK_EV_MOTION_INTERRUPT = 1 << 5, /* another motion took over its properties */
};
typedef void (*BKMotionFn)(BKMotion *m, int event, void *data);

typedef struct BKMotionVars {
  double duration;      /* seconds (0.5); springs and inertia compute theirs */
  double delay;         /* seconds before it starts (negative: it starts that far in) */
  int repeat;           /* -1 forever */
  double repeat_delay;
  int yoyo;
  double time_scale;    /* 1 */
  int paused;           /* made paused */
  int reversed;
  BKEase ease;          /* the default: power1.out (GSAP's) */
  int kind;             /* BK_MOTION_TWEEN_KIND / SPRING_KIND / INERTIA_KIND */
  BKSpring spring;
  BKInertia inertia;
  int overwrite;        /* BK_OVERWRITE_* (motions on the same properties of the same target) */
  int immediate;        /* render at once (gsap.from / fromTo / set) */
  BKMotionFn on;        /* events in `events` */
  int events;
  void *data;
  void (*free_data)(void *data); /* optional: frees `data` with the motion (bindings' closures) */
} BKMotionVars;

BKMotionVars BK_motion_vars_default(void);

/* A tween of `obj`'s properties. Starts (in the global timeline) unless paused or added to
 * a timeline. The handle is yours (BK_motion_release). */
BKMotion *BK_tween(const BKAnimClass *cls, void *obj, const BKTrack *tracks, int ntracks,
                   const BKMotionVars *vars);
/* A timeline: motions placed at times (BK_timeline_add). */
BKMotion *BK_timeline(const BKMotionVars *vars);
/* Place `child` (taken from wherever it was) at `position`, GSAP's position parameter:
 * "1.5" absolute, "+=0.5" / "-=0.5" after / before the end, "<" / ">" the start / end of
 * the last added, "<0.2" / ">-0.1", "label", "label+=1", "50%" of the timeline, "" = the
 * end. Returns the time it starts at. */
double BK_timeline_add(BKMotion *tl, BKMotion *child, const char *position);
/* A label at `position`; and a callback there (gsap's call()). */
double BK_timeline_label(BKMotion *tl, const char *name, const char *position);
double BK_timeline_call(BKMotion *tl, BKMotionFn fn, void *data, void (*free_data)(void *data),
                        const char *position);
double BK_timeline_label_time(const BKMotion *tl, const char *name); /* < 0: none */

/* GSAP's stagger: the start offsets of `count` targets (seconds). `from`: "start", "center",
 * "end", "edges", "random" or an index; `grid` rows x columns (0: a line, -1: auto) with
 * `axis` "x" / "y" / NULL; `amount` total (else `each`); `ease` shapes the spread. */
typedef struct BKStagger {
  double each, amount;
  const char *from;
  int from_index;
  int grid_rows, grid_cols;
  const char *axis;
  BKEase ease;
} BKStagger;
void BK_stagger(const BKStagger *s, int count, double *r_offsets);

/* Controls (GSAP's). Time is the motion's own (seconds, without repeats unless "total"). */
void BK_motion_play(BKMotion *m);
void BK_motion_pause(BKMotion *m);
void BK_motion_resume(BKMotion *m);
void BK_motion_reverse(BKMotion *m);
void BK_motion_restart(BKMotion *m);
void BK_motion_seek(BKMotion *m, double time);
void BK_motion_set_progress(BKMotion *m, double progress);   /* of one iteration */
void BK_motion_set_total_progress(BKMotion *m, double progress);
void BK_motion_set_time_scale(BKMotion *m, double scale);
void BK_motion_complete(BKMotion *m);  /* to its end now (Motion's complete) */
void BK_motion_kill(BKMotion *m);      /* stopped where it is, out of its timeline */
int BK_motion_is_active(const BKMotion *m);
int BK_motion_paused(const BKMotion *m);
int BK_motion_reversed(const BKMotion *m);
double BK_motion_time(const BKMotion *m);
double BK_motion_total_time(const BKMotion *m);
double BK_motion_duration(const BKMotion *m);        /* one iteration */
double BK_motion_total_duration(const BKMotion *m);  /* with repeats (-1 repeat: huge) */
double BK_motion_progress(const BKMotion *m);
double BK_motion_time_scale(const BKMotion *m);
/* A property's velocity now (units per second): Motion hands it to the next motion. */
float BK_motion_velocity(const BKMotion *m, int prop);
void BK_motion_retain(BKMotion *m);
void BK_motion_release(BKMotion *m);

/* The clock: every motion advances to `now` (seconds, any origin, increasing). Returns 1 while
 * something is still animating. Hosts call it once per frame before drawing; a second call
 * with the same `now` does nothing. */
int BK_anim_tick(double now);
int BK_anim_active(void);
/* Who ticks the clock: the kit's frames (BK_render, with the kit's clock; the default), or
 * (on) a host with a clock of its own that calls BK_anim_tick itself (Rust's windows, whose
 * headless clock is virtual): two clocks would fight. */
void BK_anim_set_host_clock(int on);
int BK_anim_host_clock(void);
/* Reduced motion (Motion's reducedMotion "always", the OS setting): transforms and layout
 * jump to their end; opacity and colors still animate. */
void BK_anim_set_reduced_motion(int on);
int BK_anim_reduced_motion(void);
/* Every motion's speed (GSAP's globalTimeline.timeScale). */
void BK_anim_set_time_scale(double scale);
/* GSAP's lagSmoothing: a frame later than `threshold` seconds counts as `adjusted` (0.5,
 * 0.033), so a stall doesn't jump animations to their end; threshold 0 turns it off. */
void BK_anim_set_lag_smoothing(double threshold, double adjusted);
/* Stop and forget every motion of `obj` (it is going away), or with prop >= 0 just that one
 * (found at once: per property is the fast way to forget a target). */
void BK_anim_kill_target(void *obj, int prop);
/* Called when a motion starts or resumes, so the host draws frames again (the kit sets it
 * to request a frame from every context; a host with its own loop sets its own). */
void BK_anim_set_wake(void (*wake)(void *data), void *data);
/* Kit nodes (BKNode *) as targets: "value" (a number, or a color's four), "color",
 * "backgroundColor" (a frame's "bg") and any number BK_set_num takes, by its name. A node
 * being freed stops its motions. */
extern const BKAnimClass BK_anim_node;
/* Free every motion and the pools (tests and exit; motions you hold become invalid). */
void BK_anim_exit(void);

/** \} */

#ifdef __cplusplus
}
#endif
