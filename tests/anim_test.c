/* The animation engine (kit/intern/anim): every ease against reference values, springs and
 * inertia against a numerical integration, tweens and timelines on a stepped clock (GSAP's
 * position parameter, labels, calls, repeat and yoyo, reverse, from, relative values,
 * overwrite with velocity handed over, stagger), no allocation per frame, and every MEM
 * block back after BK_anim_exit. Exits 77 (skipped) when the build has no animation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "BUI_anim.h"
#include "BUI_kit.h"
#include "MEM_guardedalloc.h"

static int g_fail = 0;
#define CHECK(c) \
  do { \
    if (!(c)) { \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      g_fail = 1; \
    } \
  } while (0)
#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))

/* -------------------------------------------------------------------- */
/** \name A target: named numbers
 * \{ */

typedef struct Obj {
  float x, y, opacity, color[4];
  int writes;
} Obj;

static int obj_get(void *o, int prop, float v[4], int *unit)
{
  Obj *b = (Obj *)o;
  *unit = BK_UNIT_PX;
  switch (prop) {
    case BK_PROP_X:
      v[0] = b->x;
      return 1;
    case BK_PROP_Y:
      v[0] = b->y;
      return 1;
    case BK_PROP_OPACITY:
      v[0] = b->opacity;
      *unit = BK_UNIT_NONE;
      return 1;
    case BK_PROP_COLOR:
      memcpy(v, b->color, sizeof(b->color));
      *unit = BK_UNIT_COLOR;
      return 4;
  }
  return 0;
}

static void obj_set(void *o, int prop, const float *v, int n, int unit)
{
  (void)n;
  (void)unit;
  Obj *b = (Obj *)o;
  b->writes++;
  switch (prop) {
    case BK_PROP_X:
      b->x = v[0];
      break;
    case BK_PROP_Y:
      b->y = v[0];
      break;
    case BK_PROP_OPACITY:
      b->opacity = v[0];
      break;
    case BK_PROP_COLOR:
      memcpy(b->color, v, sizeof(b->color));
      break;
  }
}

static const BKAnimClass OBJ = {obj_get, obj_set, NULL, NULL};

/* a tween of one property to one value */
static BKMotion *to(Obj *o, int prop, float value, BKMotionVars v)
{
  const BKTrack t = {prop, 1, 1, &value, NULL, NULL, NULL, NULL, BK_UNIT_PX, 0};
  return BK_tween(&OBJ, o, &t, 1, &v);
}

static double g_now = 100.0; /* the clock starts anywhere */
static double test_clock(void)
{
  return g_now;
}
static void step(double seconds)
{
  g_now += seconds;
  BK_anim_tick(g_now);
}

static BKMotionVars linear(double duration)
{
  BKMotionVars v = BK_motion_vars_default();
  v.duration = duration;
  BK_ease_parse("none", &v.ease);
  return v;
}

/** \} */

static float ease(const char *name, float t)
{
  BKEase e;
  if (!BK_ease_parse(name, &e)) {
    printf("FAIL: unknown ease %s\n", name);
    g_fail = 1;
  }
  return BK_ease_eval(&e, t);
}

static void test_easing(void)
{
  /* GSAP */
  CHECK(NEAR(ease("power1.out", 0.5f), 0.75, 1e-6));
  CHECK(NEAR(ease("power1", 0.5f), 0.75, 1e-6)); /* alone: .out */
  CHECK(NEAR(ease("power2.inOut", 0.25f), 0.0625, 1e-6));
  CHECK(NEAR(ease("Power2.easeOut", 0.5f), ease("power2.out", 0.5f), 1e-7)); /* GSAP 2 */
  CHECK(NEAR(ease("power3.in", 0.5f), 0.0625, 1e-6));
  CHECK(NEAR(ease("power4.in", 0.5f), 0.03125, 1e-6));
  CHECK(NEAR(ease("sine.in", 0.5f), 1.0 - cos(3.14159265 / 4), 1e-6));
  CHECK(NEAR(ease("circ.in", 0.6f), 1.0 - sqrt(1 - 0.36), 1e-6));
  CHECK(NEAR(ease("expo.in", 1.0f), 1.0, 1e-6) && NEAR(ease("expo.in", 0.0f), 0.0, 1e-6));
  CHECK(NEAR(ease("back.out", 0.5f), 1.0877, 1e-3)); /* overshoots */
  CHECK(NEAR(ease("back.out(3)", 0.5f), 1.0 - 0.125 * 4 + 0.25 * 3, 1e-5) || ease("back.out(3)", 0.5f) > 1.1);
  CHECK(NEAR(ease("bounce.out", 0.5f), 0.765625, 1e-6));
  CHECK(ease("elastic.out", 0.2f) > 1.0f && NEAR(ease("elastic.out(1, 0.3)", 1.0f), 1.0, 1e-6));
  CHECK(NEAR(ease("steps(4)", 0.5f), 0.5, 1e-6) && NEAR(ease("steps(4)", 0.99f), 0.75, 1e-6));
  CHECK(NEAR(ease("none", 0.3f), 0.3, 1e-7) && NEAR(ease("linear", 0.3f), 0.3, 1e-7));
  CHECK(NEAR(ease("slow(0.7, 0.7)", 0.5f), 0.5, 1e-6));
  CHECK(NEAR(ease("expoScale(1, 2)", 0.5f), sqrt(2.0) - 1.0, 1e-5));
  CHECK(fabsf(ease("wiggle(6)", 1.0f)) < 1e-3f && fabsf(ease("wiggle(6)", 0.04f)) > 0.3f);
  CHECK(NEAR(ease("customBounce(0.7)", 1.0f), 1.0, 1e-3) && ease("customBounce(0.7)", 0.3f) > 0.0f);
  CHECK(ease("rough({strength: 1, points: 20})", 1.0f) == 1.0f);
  /* Motion */
  CHECK(NEAR(ease("easeInOut", 0.5f), 0.5, 1e-4));
  CHECK(ease("easeIn", 0.5f) < 0.5f && ease("easeOut", 0.5f) > 0.5f);
  CHECK(NEAR(ease("circInOut", 0.5f), 0.5, 1e-6));
  CHECK(ease("backIn", 0.2f) < 0.0f && ease("backOut", 0.8f) > 1.0f);
  CHECK(NEAR(ease("backInOut", 0.5f), 0.5, 1e-3));
  CHECK(ease("anticipate", 0.2f) < 0.0f && NEAR(ease("anticipate", 1.0f), 1.0, 1e-3));
  /* CSS */
  CHECK(NEAR(ease("ease", 0.5f), 0.8024034, 1e-4));
  CHECK(NEAR(ease("cubic-bezier(0.42, 0, 0.58, 1)", 0.5f), 0.5, 1e-4));
  CHECK(NEAR(ease("steps(4, jump-start)", 0.5f), 0.75, 1e-6));
  CHECK(NEAR(ease("step-end", 0.99f), 0.0, 1e-6) && NEAR(ease("step-start", 0.01f), 1.0, 1e-6));
  CHECK(NEAR(ease("linear(0, 0.25 75%, 1)", 0.75f), 0.25, 1e-6));
  CHECK(NEAR(ease("linear(0, 0.25 75%, 1)", 0.5f), 0.25 * 0.5 / 0.75, 1e-5));
  /* Blender */
  CHECK(NEAR(ease("constant", 0.99f), 0.0, 1e-6));
  CHECK(NEAR(ease("bezier", 0.5f), 0.5, 1e-4) && ease("bezier", 0.2f) < 0.2f);
  CHECK(NEAR(ease("quad.inOut", 0.25f), ease("power1.inOut", 0.25f), 1e-7));
  CHECK(NEAR(ease("quadInOut", 0.25f), ease("power1.inOut", 0.25f), 1e-7));
  /* custom curves, registered names */
  BKEase path;
  CHECK(BK_ease_path("M0,0 L1,1", &path) && NEAR(BK_ease_eval(&path, 0.3f), 0.3, 1e-6));
  CHECK(BK_ease_path("M0,0 C0.5,0 0.5,1 1,1", &path) && BK_ease_eval(&path, 0.25f) < 0.25f);
  CHECK(BK_ease_register("myEase", &path) && NEAR(ease("myEase", 0.25f), BK_ease_eval(&path, 0.25f), 1e-7));
  BKEase unknown;
  CHECK(!BK_ease_parse("nonsense.out", &unknown) && !BK_ease_parse("power2.sideways", &unknown));
  printf("easing: every family by its GSAP, Motion, CSS and Blender names\n");
}

/* x'' = -k (x - to) - c x' (semi-implicit Euler, fine steps) */
static float integrate(const BKSpring *s, float from, float to, double t)
{
  double x = from, v = s->velocity;
  const double dt = 1e-5;
  for (double u = 0.0; u < t; u += dt) {
    v += (-s->stiffness * (x - to) - s->damping * v) / s->mass * dt;
    x += v * dt;
  }
  return (float)x;
}

static void test_springs(void)
{
  const BKSpring cases[3] = {{100, 10, 1, 0, 0, 0}, {100, 20, 1, 3, 0, 0}, {100, 40, 1, -2, 0, 0}};
  for (int i = 0; i < 3; i++) { /* under-, critically and over-damped */
    for (double t = 0.1; t < 1.0; t += 0.3) {
      float pos;
      BK_spring_eval(&cases[i], 0.0f, 1.0f, t, &pos, NULL);
      CHECK(NEAR(pos, integrate(&cases[i], 0.0f, 1.0f, t), 2e-3));
    }
  }
  const double settle = BK_spring_settle(&cases[0], 0.0f, 100.0f);
  CHECK(settle > 0.3 && settle < 3.0);
  const BKSpring crit = BK_spring_bounce(0.0f, 0.5f);
  CHECK(NEAR(crit.damping / (2.0 * sqrt(crit.stiffness)), 1.0, 1e-4)); /* bounce 0: critically damped */
  const BKSpring timed = BK_spring_duration(0.8f, 0.25f, 0.0f, 1.0f);
  CHECK(timed.stiffness > 1.0f && timed.damping > 0.0f);
  printf("springs: closed form = integration (under, critical, over); settles in %.2f s\n", settle);
}

static void test_tweens(void)
{
  Obj o = {0};
  /* to: linear, 1 s */
  BKMotion *m = to(&o, BK_PROP_X, 100.0f, linear(1.0));
  step(0.25);
  CHECK(NEAR(o.x, 25.0, 1e-3));
  step(0.25);
  CHECK(NEAR(o.x, 50.0, 1e-3));
  /* pause holds, resume goes on */
  BK_motion_pause(m);
  step(0.5);
  CHECK(NEAR(o.x, 50.0, 1e-3));
  BK_motion_resume(m);
  step(0.25);
  CHECK(NEAR(o.x, 75.0, 1e-3));
  /* reverse: back from where it is */
  BK_motion_reverse(m);
  step(0.25);
  CHECK(NEAR(o.x, 50.0, 1e-3));
  /* time scale */
  BK_motion_set_time_scale(m, 2.0);
  step(0.125);
  CHECK(NEAR(o.x, 25.0, 1e-3));
  /* seek, progress */
  BK_motion_seek(m, 0.9);
  CHECK(NEAR(o.x, 90.0, 1e-3));
  BK_motion_set_progress(m, 0.1);
  CHECK(NEAR(o.x, 10.0, 1e-3));
  BK_motion_kill(m);
  BK_motion_release(m);

  /* from: starts at the value given, ends where it was */
  o.x = 30.0f;
  BKMotionVars v = linear(1.0);
  const float from_value = 100.0f;
  const BKTrack fromt = {BK_PROP_X, 1, 1, &from_value, NULL, NULL, NULL, NULL, BK_UNIT_PX, 1};
  v.immediate = 1;
  m = BK_tween(&OBJ, &o, &fromt, 1, &v);
  CHECK(NEAR(o.x, 100.0, 1e-3)); /* rendered at once (immediateRender) */
  step(1.0);
  CHECK(NEAR(o.x, 30.0, 1e-3));
  BK_motion_release(m);

  /* relative: "+=50" */
  o.x = 10.0f;
  const float by = 50.0f;
  const int8_t plus = 1;
  const BKTrack rel = {BK_PROP_X, 1, 1, &by, NULL, &plus, NULL, NULL, BK_UNIT_PX, 0};
  BKMotionVars lv = linear(1.0);
  m = BK_tween(&OBJ, &o, &rel, 1, &lv);
  step(1.0);
  CHECK(NEAR(o.x, 60.0, 1e-3));
  BK_motion_release(m);

  /* keyframes with times and per-segment eases (Motion's) */
  o.x = 0.0f;
  const float keys[3] = {0.0f, 100.0f, 0.0f};
  const float times[3] = {0.0f, 0.2f, 1.0f};
  BKEase eases[2];
  BK_ease_parse("none", &eases[0]);
  BK_ease_parse("none", &eases[1]);
  const BKTrack kt = {BK_PROP_X, 1, 3, keys, NULL, NULL, times, eases, BK_UNIT_PX, 0};
  m = BK_tween(&OBJ, &o, &kt, 1, &lv);
  step(0.1);
  CHECK(NEAR(o.x, 50.0, 1e-2));
  step(0.1);
  CHECK(NEAR(o.x, 100.0, 1e-2));
  step(0.4);
  CHECK(NEAR(o.x, 50.0, 1e-2));
  BK_motion_release(m);
  step(1.0);

  /* colors: four numbers */
  float red[4] = {1, 0, 0, 1};
  memcpy(o.color, (float[4]){0, 0, 1, 1}, sizeof(o.color));
  float parsed[4];
  int unit, r;
  CHECK(BK_anim_parse_value("#ff0000", parsed, &unit, &r) == 4 && unit == BK_UNIT_COLOR && parsed[0] == 1.0f);
  CHECK(BK_anim_parse_value("rgb(0 128 255 / 50%)", parsed, &unit, &r) == 4 && NEAR(parsed[3], 0.5, 1e-6));
  CHECK(BK_anim_parse_value("rebeccapurple", parsed, &unit, &r) == 4);
  CHECK(BK_anim_parse_value("+=45deg", parsed, &unit, &r) == 1 && r == 1 && unit == BK_UNIT_DEG && parsed[0] == 45.0f);
  CHECK(BK_anim_parse_value("12abc", parsed, &unit, &r) == 0);
  const BKTrack ct = {BK_PROP_COLOR, 4, 1, red, NULL, NULL, NULL, NULL, BK_UNIT_COLOR, 0};
  m = BK_tween(&OBJ, &o, &ct, 1, &lv);
  step(0.5);
  CHECK(NEAR(o.color[0], 0.5, 1e-3) && NEAR(o.color[2], 0.5, 1e-3));
  BK_motion_release(m);
  step(1.0);

  /* property names: one id per spelling */
  CHECK(BK_anim_prop("scaleX") == BK_PROP_SCALE_X && BK_anim_prop("scale-x") == BK_PROP_SCALE_X &&
        BK_anim_prop("scale_x") == BK_PROP_SCALE_X && BK_anim_prop("rotation") == BK_PROP_ROTATE &&
        BK_anim_prop("autoAlpha") == BK_PROP_OPACITY);
  const int custom = BK_anim_prop("border-radius");
  CHECK(custom >= BK_PROP__FIXED && BK_anim_prop("borderRadius") == custom &&
        strcmp(BK_anim_prop_name(custom), "borderRadius") == 0);
  printf("tweens: to, from, relative, keyframes, colors, pause, reverse, speed, seek\n");
}

static int g_events[64];
static void on_event(BKMotion *m, int ev, void *data)
{
  (void)m;
  (void)data;
  for (int i = 0; i < 6; i++) {
    if (ev == (1 << i)) {
      g_events[i]++;
    }
  }
}

static int g_calls;
static void on_call(BKMotion *m, int ev, void *data)
{
  (void)m;
  (void)ev;
  (void)data;
  g_calls++;
}

static void test_timelines(void)
{
  Obj a = {0}, b = {0}, c = {0};
  BKMotion *tl = BK_timeline(&(BKMotionVars){.time_scale = 1.0});
  BKMotionVars lv = linear(1.0);
  lv.paused = 0;
  BKMotion *ta = to(&a, BK_PROP_X, 100.0f, lv), *tb = to(&b, BK_PROP_X, 100.0f, lv),
           *tc = to(&c, BK_PROP_X, 100.0f, lv);
  CHECK(NEAR(BK_timeline_add(tl, ta, ""), 0.0, 1e-9));     /* at the end: 0 */
  CHECK(NEAR(BK_timeline_add(tl, tb, "<0.5"), 0.5, 1e-9)); /* half a second after a starts */
  BK_timeline_label(tl, "mid", ">");                       /* where b ends: 1.5 */
  CHECK(NEAR(BK_timeline_label_time(tl, "mid"), 1.5, 1e-9));
  CHECK(NEAR(BK_timeline_add(tl, tc, "mid+=0.25"), 1.75, 1e-9));
  BK_timeline_call(tl, on_call, NULL, NULL, "mid");
  CHECK(NEAR(BK_motion_duration(tl), 2.75, 1e-9));
  BK_motion_release(ta);
  BK_motion_release(tb);
  BK_motion_release(tc);
  step(0.75);
  CHECK(NEAR(a.x, 75.0, 1e-3) && NEAR(b.x, 25.0, 1e-3) && c.x == 0.0f);
  step(1.0); /* 1.75: past the call */
  CHECK(NEAR(a.x, 100.0, 1e-3) && NEAR(b.x, 100.0, 1e-3) && NEAR(c.x, 0.0, 1e-3) && g_calls == 1);
  BK_motion_seek(tl, 0.25); /* a jump back: every child where it belongs */
  CHECK(NEAR(a.x, 25.0, 1e-3) && NEAR(b.x, 0.0, 1e-3) && NEAR(c.x, 0.0, 1e-3));
  BK_motion_seek(tl, 2.75);
  CHECK(NEAR(c.x, 100.0, 1e-3));
  BK_motion_release(tl);

  /* a timeline with a child that repeats for ever plays until it is killed (it had no length, and so no time to run) */
  {
    Obj loop = {0}, loop2 = {0};
    BKMotionVars fv = linear(1.0);
    fv.repeat = -1;
    BKMotion *ltl = BK_timeline(&(BKMotionVars){.time_scale = 1.0});
    BKMotion *lt = to(&loop, BK_PROP_X, 100.0f, fv);
    BK_timeline_add(ltl, lt, "0");
    BK_motion_release(lt);
    step(0.5);
    CHECK(NEAR(loop.x, 50.0, 1e-3));
    step(1.0); /* 1.5: the second iteration */
    CHECK(NEAR(loop.x, 50.0, 1e-3) && NEAR(BK_motion_total_duration(ltl), 1e300, 1e290));
    step(20.0);
    CHECK(NEAR(loop.x, 50.0, 1e-3) && BK_motion_is_active(ltl));
    /* a yoyo one in a timeline inside a timeline */
    fv.yoyo = 1;
    BKMotion *outer = BK_timeline(&(BKMotionVars){.time_scale = 1.0}), *inner = BK_timeline(&(BKMotionVars){.time_scale = 1.0});
    BKMotion *lt2 = to(&loop2, BK_PROP_X, 100.0f, fv);
    BK_timeline_add(inner, lt2, "0");
    BK_motion_release(lt2);
    BK_timeline_add(outer, inner, "0");
    BK_motion_release(inner);
    step(1.5);
    CHECK(NEAR(loop2.x, 50.0, 1e-3));
    step(1.0); /* 2.5 into it: back down */
    CHECK(NEAR(loop2.x, 50.0, 1e-3));
    /* a loop of timelines (a in b in a, or a in a) is refused, not recursed into */
    {
      BKMotion *ca = BK_timeline(&(BKMotionVars){.time_scale = 1.0}), *cb = BK_timeline(&(BKMotionVars){.time_scale = 1.0});
      BK_timeline_add(ca, cb, "0");
      BK_timeline_add(cb, ca, "0");
      BK_timeline_add(ca, ca, "0");
      CHECK(BK_motion_duration(ca) == 0.0 && BK_motion_duration(cb) == 0.0);
      BK_motion_kill(ca);
      BK_motion_kill(cb);
      BK_motion_release(ca);
      BK_motion_release(cb);
    }
    BK_motion_kill(ltl);
    BK_motion_kill(outer);
    BK_motion_release(ltl);
    BK_motion_release(outer);
  }

  /* motions starting and ending on many targets over time: the tables of targets and drivers stay small */
  {
    Obj pool[64];
    memset(pool, 0, sizeof(pool));
    for (int round = 0; round < 4000; round++) {
      Obj *o = &pool[(round * 7) % 64];
      BKMotion *m = to(o, BK_PROP_X, 10.0f, linear(0.01));
      BK_motion_release(m);
      step(0.02);
      CHECK(NEAR(o->x, 10.0, 1e-3));
      o->x = 0.0f;
    }
  }

  /* repeat and yoyo, with the events */
  memset(g_events, 0, sizeof(g_events));
  Obj o = {0};
  BKMotionVars rv = linear(1.0);
  rv.repeat = 1;
  rv.yoyo = 1;
  rv.on = on_event;
  rv.events = BK_EV_MOTION_START | BK_EV_MOTION_COMPLETE | BK_EV_MOTION_REPEAT;
  BKMotion *m = to(&o, BK_PROP_X, 100.0f, rv);
  CHECK(NEAR(BK_motion_total_duration(m), 2.0, 1e-9));
  step(0.5);
  step(1.0); /* 1.5: going back */
  CHECK(NEAR(o.x, 50.0, 1e-3));
  step(1.0);
  CHECK(NEAR(o.x, 0.0, 1e-3));
  CHECK(g_events[0] == 1 && g_events[2] == 1 && g_events[3] == 1); /* start, complete, repeat once each */
  BK_motion_release(m);

  /* stagger: GSAP's distribute */
  double offs[5];
  BKStagger s = {0};
  s.each = 0.1;
  s.from = "center";
  BK_stagger(&s, 5, offs);
  CHECK(NEAR(offs[0], 0.2, 1e-9) && NEAR(offs[2], 0.0, 1e-9) && NEAR(offs[4], 0.2, 1e-9));
  s.from = "start";
  s.amount = 1.0;
  BK_stagger(&s, 5, offs);
  CHECK(NEAR(offs[4], 1.0, 1e-9) && NEAR(offs[1], 0.25, 1e-9));
  BKStagger g = {0};
  g.each = 0.1;
  g.grid_rows = 3;
  g.grid_cols = 3;
  g.from = "center";
  double grid[9];
  BK_stagger(&g, 9, grid);
  CHECK(NEAR(grid[4], 0.0, 1e-9) && NEAR(grid[1], 0.1, 1e-9) && grid[0] > grid[1]);
  /* with a clock set (frames stop when idle, as the kit's do), a motion started after an idle
   * gap starts then, not when frames last ran */
  {
    step(1.0);
    BK_anim_set_clock(test_clock);
    g_now += 0.3; /* idle: no frames for 0.3 s */
    Obj gap = {0};
    BKMotion *g = to(&gap, BK_PROP_X, 100.0f, linear(1.0));
    step(0.1);
    CHECK(NEAR(gap.x, 10.0, 1e-3));
    BK_anim_set_clock(NULL);
    BK_motion_release(g);
    step(1.0);
  }
  /* a motion started after an idle stretch (the global timeline made anew) is active at once */
  step(2.0);
  Obj idle = {0};
  BKMotion *fresh = to(&idle, BK_PROP_X, 100.0f, linear(1.0));
  CHECK(BK_motion_is_active(fresh));
  BK_motion_release(fresh);
  /* a timeline that finished empty plays on with what is added later (GSAP) */
  Obj late = {0};
  BKMotion *empty = BK_timeline(&(BKMotionVars){.time_scale = 1.0});
  step(0.5); /* empty: done, out of the global timeline */
  BKMotion *lt = to(&late, BK_PROP_X, 100.0f, linear(1.0));
  BK_timeline_add(empty, lt, "");
  BK_motion_release(lt);
  step(0.5);
  CHECK(NEAR(late.x, 50.0, 1e-3));
  BK_motion_release(empty);
  printf("timelines: positions, labels, calls, seeking, repeat, yoyo, events, stagger\n");
}

static void test_overwrite_and_springs(void)
{
  Obj o = {0};
  BKMotionVars lv = linear(1.0);
  BKMotion *first = to(&o, BK_PROP_X, 100.0f, lv);
  step(0.5);
  CHECK(NEAR(o.x, 50.0, 1e-3));
  CHECK(NEAR(BK_motion_velocity(first, BK_PROP_X), 100.0, 1.0)); /* 100 px a second */
  /* a spring on the same property takes over, with the velocity it had */
  BKMotionVars sv = BK_motion_vars_default();
  sv.kind = BK_MOTION_SPRING_KIND;
  BKMotion *spring = to(&o, BK_PROP_X, 0.0f, sv);
  step(1.0 / 60.0);
  CHECK(o.x > 50.0f); /* still moving the way it went */
  step(0.5);
  CHECK(!BK_motion_is_active(first) || BK_motion_progress(first) <= 0.51);
  for (int i = 0; i < 300; i++) {
    step(1.0 / 60.0);
  }
  CHECK(NEAR(o.x, 0.0, 0.6)); /* settled where the spring goes */
  BK_motion_release(first);
  BK_motion_release(spring);

  /* inertia: decays to origin + power * velocity */
  Obj p = {0};
  BKMotionVars iv = BK_motion_vars_default();
  iv.kind = BK_MOTION_INERTIA_KIND;
  iv.inertia = BK_inertia_default(1000.0f);
  BKMotion *in = to(&p, BK_PROP_X, 0.0f, iv);
  for (int i = 0; i < 240; i++) {
    step(1.0 / 60.0);
  }
  CHECK(NEAR(p.x, 800.0, 1.0));
  BK_motion_release(in);
  printf("overwrite: a spring takes over with the velocity; inertia ends at %.1f\n", p.x);
}

static void test_lag_smoothing(void)
{
  Obj o = {0};
  BK_anim_set_lag_smoothing(0.5, 0.033); /* GSAP's default */
  BKMotion *m = to(&o, BK_PROP_X, 100.0f, linear(1.0));
  step(5.0); /* a stall: the clock moves 33 ms, not 5 s */
  CHECK(NEAR(o.x, 3.3, 1e-2));
  step(0.25); /* under the threshold: as is */
  CHECK(NEAR(o.x, 28.3, 1e-2));
  BK_anim_set_lag_smoothing(0.0, 0.0);
  step(5.0);
  CHECK(NEAR(o.x, 100.0, 1e-3));
  BK_motion_release(m);
  printf("lag smoothing: a 5 s stall moves the clock 33 ms\n");
}

static int g_wakes;
static void on_wake(void *data)
{
  (void)data;
  g_wakes++;
}

static void test_kit_nodes(void)
{
  BKNode *slider = BK_node_new("slider");
  BK_set_num(slider, "max", 100.0);
  BK_set_num(slider, "value", 10.0);
  BK_anim_set_wake(on_wake, NULL);
  const float to_value = 90.0f;
  const BKTrack t = {BK_anim_prop("value"), 1, 1, &to_value, NULL, NULL, NULL, NULL, BK_UNIT_NONE, 0};
  BKMotionVars lv = linear(1.0);
  BKMotion *m = BK_tween(&BK_anim_node, slider, &t, 1, &lv);
  CHECK(g_wakes == 1); /* the host is asked for frames */
  step(0.5);
  CHECK(NEAR(BK_get_num(slider, "value"), 50.0, 1e-3));
  BKNode *frame = BK_node_new("frame");
  const float bg[4] = {1, 1, 1, 1};
  const BKTrack ct = {BK_PROP_BACKGROUND_COLOR, 4, 1, bg, NULL, NULL, NULL, NULL, BK_UNIT_COLOR, 0};
  BKMotion *c = BK_tween(&BK_anim_node, frame, &ct, 1, &lv);
  step(0.25);
  float got[4];
  CHECK(BK_get_color(frame, "bg", got) && got[0] > 0.0f && got[0] < 1.0f);
  CHECK(BK_anim_node_has(slider, BK_anim_prop("value"), 1) && BK_anim_node_has(frame, BK_PROP_BACKGROUND_COLOR, 4));
  CHECK(!BK_anim_node_has(slider, BK_PROP_OPACITY, 1) && !BK_anim_node_has(slider, BK_PROP_X, 1));
  BK_node_free(slider); /* its motion stops: nothing writes to freed memory */
  BK_node_free(frame);
  CHECK(!BK_motion_is_active(m) && !BK_motion_is_active(c));
  step(0.5);
  BK_motion_release(m);
  BK_motion_release(c);
  BK_anim_set_wake(NULL, NULL);
  printf("kit nodes: a slider's value, a frame's bg; freeing a node stops its motions\n");
}

static void test_kill(void)
{
  /* a target going away stops a motion that has not started (a delay): it never writes it */
  Obj gone = {0};
  BKMotion *running = to(&gone, BK_PROP_X, 100.0f, linear(1.0)); /* drives x: the drivers table knows it */
  step(0.25);
  BKMotionVars dv = linear(1.0);
  dv.delay = 0.5;
  BKMotion *delayed = to(&gone, BK_PROP_X, 100.0f, dv); /* on x too, but not started: not in it */
  BKMotion *other = to(&gone, BK_PROP_Y, 100.0f, linear(1.0));
  step(0.125);
  BK_anim_kill_target(&gone, BK_PROP_X); /* every motion of x: the running one and the one waiting */
  CHECK(!BK_motion_is_active(running) && BK_motion_is_active(other));
  const float x_then = gone.x;
  step(2.0); /* the waiting motion's start comes and goes: it never writes x */
  CHECK(gone.x == x_then);
  BK_anim_kill_target(&gone, -1);
  CHECK(!BK_motion_is_active(other));
  BK_motion_release(running);
  BK_motion_release(delayed);
  BK_motion_release(other);

  /* a motion without overwrite is stopped too (it isn't in the drivers table) */
  Obj free_obj = {0};
  BKMotionVars nv = linear(1.0);
  nv.overwrite = BK_OVERWRITE_NONE;
  BKMotion *plain = to(&free_obj, BK_PROP_X, 100.0f, nv);
  step(0.25);
  BK_anim_kill_target(&free_obj, BK_PROP_X);
  CHECK(!BK_motion_is_active(plain));
  BK_motion_release(plain);

  /* killing a timeline stops every child, also when the caller let go of them */
  Obj a = {0}, b = {0}, c = {0};
  BKMotion *tl = BK_timeline(&(BKMotionVars){.time_scale = 1.0});
  BKMotion *ta = to(&a, BK_PROP_X, 100.0f, linear(1.0)), *tb = to(&b, BK_PROP_X, 100.0f, linear(1.0)),
           *tc = to(&c, BK_PROP_X, 100.0f, linear(1.0));
  BK_timeline_add(tl, ta, "0");
  BK_timeline_add(tl, tb, "0");
  BK_timeline_add(tl, tc, "0");
  BK_motion_release(ta); /* the timeline holds ta alone: killing it frees it */
  step(0.25);
  CHECK(BK_motion_is_active(tb) && BK_motion_is_active(tc));
  BK_motion_kill(tl);
  CHECK(!BK_motion_is_active(tb) && !BK_motion_is_active(tc)); /* every child, not only the first */
  step(1.0);
  CHECK(NEAR(a.x, 25.0, 1e-3) && NEAR(b.x, 25.0, 1e-3) && NEAR(c.x, 25.0, 1e-3)); /* none went on */
  BK_motion_release(tb);
  BK_motion_release(tc);
  BK_motion_release(tl);
  printf("kill: a target's motions, started or not; a timeline's children\n");
}

static void test_no_allocation_per_frame(void)
{
  Obj objs[64] = {{0}};
  BKMotion *ms[64];
  BKMotionVars lv = linear(10.0);
  for (int i = 0; i < 64; i++) {
    ms[i] = to(&objs[i], BK_PROP_X, 100.0f, lv);
  }
  step(1.0 / 60.0);
  const size_t allocs = MEM_get_allocations_total();
  for (int i = 0; i < 120; i++) {
    step(1.0 / 60.0);
  }
  CHECK(MEM_get_allocations_total() == allocs);
  CHECK(objs[63].x > 0.0f);
  for (int i = 0; i < 64; i++) {
    BK_motion_release(ms[i]);
  }
  printf("frames: 64 tweens for 120 frames, %zu allocations\n", MEM_get_allocations_total() - allocs);
}

/* -------------------------------------------------------------------- */
/** \name Robustness: callbacks that change what is rendering, bad numbers, bad strings
 * \{ */

typedef struct Hook {
  BKMotion *victim;   /* killed by the first event */
  Obj *spawn_obj;     /* a tween of its x starts from the first event */
  BKMotion *spawned;
  int calls;
} Hook;

static void hook_fn(BKMotion *m, int ev, void *data)
{
  (void)m;
  (void)ev;
  Hook *h = (Hook *)data;
  h->calls++;
  if (h->victim) {
    BKMotion *v = h->victim;
    h->victim = NULL;
    BK_motion_kill(v);
  }
  if (h->spawn_obj && !h->spawned) {
    h->spawned = to(h->spawn_obj, BK_PROP_X, 50.0f, linear(1.0));
  }
}

static BKMotionVars with_hook(double duration, int events, Hook *h)
{
  BKMotionVars v = linear(duration);
  v.on = hook_fn;
  v.events = events;
  v.data = h;
  return v;
}

static void test_callbacks_that_change_the_tree(void)
{
  BK_anim_set_clock(test_clock); /* a motion started by a callback must not tick the clock again */
  /* a child's onComplete kills its timeline: the rest of the pass ends, nothing is touched */
  Obj a = {0}, b = {0};
  BKMotion *tl = BK_timeline(&(BKMotionVars){.time_scale = 1.0});
  Hook h1 = {tl, NULL, NULL, 0};
  BKMotion *ma = to(&a, BK_PROP_X, 100.0f, with_hook(0.5, BK_EV_MOTION_COMPLETE, &h1));
  BKMotion *mb = to(&b, BK_PROP_X, 100.0f, linear(1.0));
  BK_timeline_add(tl, ma, "0");
  BK_timeline_add(tl, mb, "0");
  step(0.25);
  step(0.5); /* a completes, kills the timeline, b is not rendered after it */
  CHECK(h1.calls == 1 && !BK_motion_is_active(mb));
  const float bx = b.x;
  step(1.0);
  CHECK(b.x == bx);
  BK_motion_release(ma);
  BK_motion_release(mb);
  BK_motion_release(tl);

  /* a child's onUpdate kills a sibling not yet rendered, and the one after still renders */
  Obj p = {0}, q = {0}, r = {0};
  BKMotion *tl2 = BK_timeline(&(BKMotionVars){.time_scale = 1.0});
  Hook h2 = {NULL, NULL, NULL, 0};
  BKMotion *mp = to(&p, BK_PROP_X, 100.0f, with_hook(1.0, BK_EV_MOTION_UPDATE, &h2));
  BKMotion *mq = to(&q, BK_PROP_X, 100.0f, linear(1.0));
  BKMotion *mr = to(&r, BK_PROP_X, 100.0f, linear(1.0));
  h2.victim = mq;
  BK_timeline_add(tl2, mp, "0");
  BK_timeline_add(tl2, mq, "0");
  BK_timeline_add(tl2, mr, "0");
  step(0.5);
  CHECK(h2.calls >= 1 && q.x == 0.0f && p.x > 0.0f && r.x > 0.0f);
  step(1.0);
  CHECK(q.x == 0.0f && NEAR(r.x, 100.0, 1e-3));
  BK_motion_release(mp);
  BK_motion_release(mq);
  BK_motion_release(mr);
  BK_motion_release(tl2);

  /* a global motion's onUpdate starts another, and its onComplete kills one */
  Obj s = {0}, t = {0};
  Hook h3 = {NULL, &t, NULL, 0};
  BKMotion *ms = to(&s, BK_PROP_X, 100.0f, with_hook(1.0, BK_EV_MOTION_UPDATE, &h3));
  step(0.25);
  CHECK(h3.spawned != NULL);
  step(0.5);
  CHECK(t.x > 0.0f);
  BKMotion *dead = to(&s, BK_PROP_Y, 100.0f, linear(2.0));
  Hook h4 = {dead, NULL, NULL, 0};
  BKMotion *killer = to(&s, BK_PROP_OPACITY, 1.0f, with_hook(0.25, BK_EV_MOTION_COMPLETE, &h4));
  step(0.5);
  CHECK(h4.calls == 1 && !BK_motion_is_active(dead));
  BK_motion_release(ms);
  BK_motion_release(h3.spawned);
  BK_motion_release(dead);
  BK_motion_release(killer);
  step(5.0);
  CHECK(!BK_anim_active());
  BK_anim_set_clock(NULL);
  printf("callbacks: kill a sibling or the timeline, start a motion, mid-render\n");
}

static void test_killed_stays_killed(void)
{
  Obj k = {0};
  BKMotion *m = to(&k, BK_PROP_X, 100.0f, linear(1.0));
  step(0.25);
  BK_motion_kill(m);
  const float x = k.x;
  BK_motion_play(m);
  BK_motion_resume(m);
  BK_motion_reverse(m);
  BK_motion_seek(m, 0.5);
  BK_motion_set_progress(m, 0.9);
  BK_motion_restart(m);
  BK_motion_complete(m);
  BKMotion *tl = BK_timeline(&(BKMotionVars){.time_scale = 1.0});
  CHECK(BK_timeline_add(tl, m, "0") == 0.0); /* nor can a timeline take it back */
  step(1.0);
  CHECK(k.x == x && !BK_motion_is_active(m) && !BK_anim_active());
  BK_motion_release(m);
  BK_motion_release(tl);
  printf("killed: play, resume, reverse, seek and add don't revive it\n");
}

static void test_numbers(void)
{
  /* the one clamp of a repeat */
  CHECK(BK_motion_clamp_repeat(-1.0) == -1 && BK_motion_clamp_repeat(-7.0) == -1);
  CHECK(BK_motion_clamp_repeat(INFINITY) == -1 && BK_motion_clamp_repeat(NAN) == 0);
  CHECK(BK_motion_clamp_repeat(3.0) == 3 && BK_motion_clamp_repeat(4294967296.0) == BK_MOTION_MAX_REPEAT);
  CHECK(BK_motion_clamp_repeat(1e30) == BK_MOTION_MAX_REPEAT);

  /* huge repeats and durations: totals in doubles, nothing overflows */
  Obj o = {0};
  BKMotionVars hv = linear(0.5);
  hv.repeat = 2147483647;
  hv.repeat_delay = 1e300;
  BKMotion *m = to(&o, BK_PROP_X, 100.0f, hv);
  CHECK(isfinite(BK_motion_total_duration(m)) && BK_motion_total_duration(m) > 1e9);
  step(1.0);
  CHECK(isfinite(o.x));
  BK_motion_kill(m); /* these run for ever: a handle let go of doesn't stop them */
  BK_motion_release(m);
  BKMotionVars big = linear(1e300);
  big.repeat = -1;
  m = to(&o, BK_PROP_Y, 100.0f, big);
  CHECK(BK_motion_duration(m) <= BK_MOTION_MAX_TIME);
  BKMotionVars tiny = linear(1e-300);
  tiny.repeat = 1000000;
  BKMotion *mt = to(&o, BK_PROP_OPACITY, 1.0f, tiny);
  step(1.0);
  CHECK(isfinite(o.y) && isfinite(o.opacity));
  BK_motion_kill(m);
  BK_motion_release(m);
  BK_motion_kill(mt);
  BK_motion_release(mt);

  /* NaN and infinity in the options: refused by the check, and read as 0 by the engine */
  BKMotionVars nv = linear(0.5);
  nv.duration = NAN;
  CHECK(!BK_motion_vars_check(&nv) && nv.duration == 0.0);
  nv = linear(0.5);
  nv.delay = INFINITY;
  CHECK(!BK_motion_vars_check(&nv));
  nv = linear(0.5);
  nv.repeat_delay = NAN;
  CHECK(!BK_motion_vars_check(&nv));
  nv = linear(0.5);
  nv.time_scale = NAN;
  CHECK(!BK_motion_vars_check(&nv) && nv.time_scale == 1.0);
  nv = linear(-3.0);
  nv.delay = -2.0;
  CHECK(BK_motion_vars_check(&nv) && nv.duration == 0.0 && nv.delay == -2.0); /* negative: clamped */
  nv = linear(NAN);
  nv.delay = NAN;
  nv.repeat_delay = INFINITY;
  Obj z = {0};
  m = to(&z, BK_PROP_X, 100.0f, nv); /* straight to the engine */
  step(0.5);
  CHECK(isfinite(z.x) && isfinite(BK_motion_total_duration(m)));
  CHECK(!BK_motion_seek(m, NAN) && !BK_motion_seek(m, INFINITY) && !BK_motion_seek(m, -INFINITY));
  CHECK(!BK_motion_set_progress(m, NAN) && !BK_motion_set_total_progress(m, INFINITY));
  CHECK(!BK_motion_set_time_scale(m, NAN) && BK_motion_time_scale(m) == 1.0);
  CHECK(BK_motion_seek(m, -5.0) && BK_motion_time(m) == 0.0); /* negative: the start */
  CHECK(BK_motion_set_time_scale(m, 0.0) && BK_motion_set_time_scale(m, -2.0) && BK_motion_time_scale(m) == 1.0);
  BK_motion_reverse(m);
  BK_motion_seek(m, 0.0);
  step(0.25);
  CHECK(isfinite(z.x));
  BK_motion_release(m);

  /* a repeat for ever of nothing: one evaluation, then it's done and the host stops drawing */
  Obj f = {0};
  BKMotionVars zero = linear(0.0);
  zero.repeat = -1;
  m = to(&f, BK_PROP_X, 7.0f, zero);
  step(0.1);
  CHECK(f.x == 7.0f && !BK_anim_active());
  BK_motion_release(m);
  BKMotion *empty = BK_timeline(&(BKMotionVars){.time_scale = 1.0, .repeat = -1});
  step(0.1);
  CHECK(!BK_anim_active());
  BK_motion_release(empty);
  printf("numbers: repeat clamp, huge totals, NaN and infinity, zero and negative, repeat -1 of nothing\n");
}

static void test_positions(void)
{
  Obj o = {0};
  BKMotion *tl = BK_timeline(&(BKMotionVars){.time_scale = 1.0});
  BKMotion *c = to(&o, BK_PROP_X, 100.0f, linear(2.0));
  CHECK(BK_timeline_add(tl, c, "") == 0.0); /* the end of an empty one */
  /* the last added runs 0 .. 2 in a timeline of 2 */
  CHECK(NEAR(BK_timeline_label(tl, "a", "0.5"), 0.5, 1e-12));
  CHECK(NEAR(BK_timeline_label(tl, "b", "<"), 0.0, 1e-12) && NEAR(BK_timeline_label(tl, "b", ">"), 2.0, 1e-12));
  CHECK(NEAR(BK_timeline_label(tl, "b", "<0.5"), 0.5, 1e-12) && NEAR(BK_timeline_label(tl, "b", ">-0.5"), 1.5, 1e-12));
  CHECK(NEAR(BK_timeline_label(tl, "b", "+=1"), 3.0, 1e-12) && NEAR(BK_timeline_label(tl, "b", "-=1"), 1.0, 1e-12));
  CHECK(NEAR(BK_timeline_label(tl, "b", "50%"), 1.0, 1e-12) && NEAR(BK_timeline_label(tl, "b", "<25%"), 0.5, 1e-12));
  CHECK(NEAR(BK_timeline_label(tl, "b", "+=50%"), 3.0, 1e-12) && NEAR(BK_timeline_label(tl, "b", "1e0"), 1.0, 1e-12));
  CHECK(NEAR(BK_timeline_label(tl, "b", "a"), 0.5, 1e-12) && NEAR(BK_timeline_label(tl, "b", "a+=1"), 1.5, 1e-12));
  CHECK(NEAR(BK_timeline_label(tl, "b", "a-=1"), 0.0, 1e-12) && NEAR(BK_timeline_label(tl, "b", "-3"), 0.0, 1e-12));
  /* everything else is refused, and nothing is placed or made */
  static const char *const bad[] = {"nan", "inf", "-inf", "0x10", "1e999", "<=1", "abc", "+=", "-=x", "<+=", "<x",
                                    "%", "<%", "a+=", "a+=x", "a++1", "1 2", "1x", "+", ".", "e5", "<<", ">>1",
                                    "+=nan", "<inf", "1e400%"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    CHECK(BK_timeline_label(tl, "bad", bad[i]) == -1.0);
    BKMotion *x = to(&o, BK_PROP_Y, 1.0f, linear(1.0));
    CHECK(BK_timeline_add(tl, x, bad[i]) == -1.0);
    BK_motion_kill(x);
    BK_motion_release(x);
  }
  CHECK(BK_timeline_label_time(tl, "bad") < 0.0 && BK_timeline_label_time(tl, "abc") < 0.0);
  /* GSAP: a label that doesn't exist yet is made at the end of the timeline, by any use of it... */
  const double end0 = BK_motion_total_duration(tl);
  CHECK(NEAR(BK_timeline_label(tl, "b2", "later"), end0, 1e-12) && NEAR(BK_timeline_label_time(tl, "later"), end0, 1e-12));
  CHECK(NEAR(BK_timeline_label(tl, "b3", "later+=1"), end0 + 1.0, 1e-12));
  CHECK(NEAR(BK_timeline_label(tl, "b4", "fresh-=0.5"), end0 - 0.5, 1e-12) && NEAR(BK_timeline_label_time(tl, "fresh"), end0, 1e-12));
  /* ...and a refused position makes nothing */
  CHECK(BK_timeline_label(tl, "b5", "ghost+=x") == -1.0 && BK_timeline_label_time(tl, "ghost") < 0.0);
  CHECK(BK_timeline_call(tl, hook_fn, NULL, NULL, "<=1") == -1.0);
  CHECK(BK_motion_total_duration(tl) == 2.0);
  BK_motion_release(c);
  BK_motion_release(tl);
  printf("positions: GSAP's grammar and nothing else\n");
}

static void count_free(void *data)
{
  (*(int *)data)++;
}

static void test_tween_boundary(void)
{
  Obj o = {0};
  const float value = 5.0f, nan = NAN;
  int freed = 0;
  BKMotionVars v = linear(1.0);
  v.data = &freed;
  v.free_data = count_free; /* a refused tween frees what it was given */
  const BKTrack nokeys = {BK_PROP_X, 1, 0, &value, NULL, NULL, NULL, NULL, BK_UNIT_PX, 0};
  const BKTrack novalues = {BK_PROP_X, 1, 1, NULL, NULL, NULL, NULL, NULL, BK_UNIT_PX, 0};
  const BKTrack badn = {BK_PROP_X, 9, 1, &value, NULL, NULL, NULL, NULL, BK_UNIT_PX, 0};
  const BKTrack nanv = {BK_PROP_X, 1, 1, &nan, NULL, NULL, NULL, NULL, BK_UNIT_PX, 0};
  const BKTrack good = {BK_PROP_X, 1, 1, &value, NULL, NULL, NULL, NULL, BK_UNIT_PX, 0};
  CHECK(BK_tween(&OBJ, &o, &nokeys, 1, &v) == NULL);
  CHECK(BK_tween(&OBJ, &o, &novalues, 1, &v) == NULL);
  CHECK(BK_tween(&OBJ, &o, &badn, 1, &v) == NULL);
  CHECK(BK_tween(&OBJ, &o, &nanv, 1, &v) == NULL);
  CHECK(BK_tween(&OBJ, &o, NULL, 1, &v) == NULL);
  const BKTrack mixed[2] = {good, nokeys};
  CHECK(BK_tween(&OBJ, &o, mixed, 2, &v) == NULL);
  CHECK(freed == 6);
  BKMotionVars plain = linear(1.0);
  BKMotion *none = BK_tween(&OBJ, &o, NULL, 0, &plain); /* no tracks: a motion that does nothing */
  CHECK(none != NULL);
  BK_motion_release(none);
  step(2.0);
  CHECK(o.x == 0.0f && o.writes == 0 && !BK_anim_active());
  printf("tween: tracks without keys, values or a list are refused\n");
}

static void test_degenerate_springs(void)
{
  const BKSpring def = BK_spring_default();
  BKSpring zero_mass = def, zero_k = def, zero_c = def, zeros = {0, 0, 0, 0, 0, 0}, nan_s = def;
  zero_mass.mass = 0.0f;
  zero_k.stiffness = 0.0f;
  zero_c.damping = 0.0f;
  nan_s.stiffness = NAN;
  const BKSpring all[] = {zero_mass, zero_k, zero_c, zeros, nan_s};
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    const double settle = BK_spring_settle(&all[i], 0.0f, 100.0f);
    float p, v;
    BK_spring_eval(&all[i], 0.0f, 100.0f, 0.5, &p, &v);
    CHECK(isfinite(settle) && settle >= 0.0 && settle <= 20.0 + 1e-9);
    if (all[i].stiffness == all[i].stiffness) {
      CHECK(isfinite(p) && isfinite(v));
    }
    Obj o = {0};
    BKMotionVars sv = BK_motion_vars_default();
    sv.kind = BK_MOTION_SPRING_KIND;
    sv.spring = all[i];
    BKMotion *m = to(&o, BK_PROP_X, 100.0f, sv);
    step(0.5);
    CHECK(isfinite(o.x) && isfinite(BK_motion_duration(m)));
    BK_motion_kill(m);
    BK_motion_release(m);
  }
  printf("springs: zero mass, stiffness, damping and NaN stay finite\n");
}

/* Motion's stepping, as the engine did it before the closed form (120 Hz, up to 20 s). With
 * `hold` it gave up at the first 0.1 s spent within the thresholds; without, it finds the last
 * step outside them (what "it stays" means, and what the closed form computes). */
static double settle_stepped(const BKSpring *s, float from, float to, int hold)
{
  const int granular = fabsf(to - from) < 5.0f;
  const double rd = s->rest_delta > 0.0f ? s->rest_delta : (granular ? 0.005 : 0.5);
  const double rs = s->rest_speed > 0.0f ? s->rest_speed : (granular ? 0.01 : 2.0);
  double last_out = 0.0;
  for (double t = 0.0; t <= 20.0; t += 1.0 / 120.0) {
    float p, v;
    BK_spring_eval(s, from, to, t, &p, &v);
    if (fabsf(p - to) > rd || fabsf(v) > rs) {
      last_out = t + 1.0 / 120.0;
    }
    else if (hold && t - last_out > 0.1) {
      break;
    }
  }
  return last_out;
}

static void test_settle_closed_form(void)
{
  /* the closed-form settle time against the stepping over a grid (under, critical and over
   * damped): within 2 % and the stepping's own 1/120 s resolution (twice) */
  static const float stiffness[] = {20.0f, 100.0f, 400.0f, 1000.0f};
  static const float ratio[] = {0.05f, 0.2f, 0.5f, 0.9f, 1.0f, 1.3f, 3.0f, 10.0f};
  static const float mass[] = {0.5f, 1.0f, 3.0f};
  static const float velocity[] = {0.0f, 800.0f, -300.0f};
  static const float to[] = {100.0f, 1.0f, -40.0f};
  int cases = 0;
  double worst = 1.0;
  for (size_t a = 0; a < 4; a++) {
    for (size_t b = 0; b < 8; b++) {
      for (size_t c = 0; c < 3; c++) {
        for (size_t d = 0; d < 3; d++) {
          for (size_t e = 0; e < 3; e++) {
            BKSpring s = BK_spring_default();
            s.stiffness = stiffness[a];
            s.mass = mass[c];
            s.damping = ratio[b] * 2.0f * sqrtf(stiffness[a] * mass[c]);
            s.velocity = velocity[d];
            const double fast = BK_spring_settle(&s, 0.0f, to[e]);
            const double slow = settle_stepped(&s, 0.0f, to[e], 0), held = settle_stepped(&s, 0.0f, to[e], 1);
            const double err = fabs(fast - slow) - (0.02 * slow + 2.0 / 120.0);
            cases++;
            if (err > 0.0 || fast < held - 2.0 / 120.0) { /* and never earlier than the old early stop */
              printf("settle mismatch k=%g r=%g m=%g v=%g to=%g: closed %.4f stepped %.4f (held %.4f)\n",
                     s.stiffness, ratio[b], s.mass, s.velocity, to[e], fast, slow, held);
              g_fail = 1;
            }
            if (held > 0.0 && fast / held > worst) {
              worst = fast / held;
            }
          }
        }
      }
    }
  }
  printf("settle: closed form = stepping over %d springs (within 2%% and 2/120 s); %.2fx the old early stop at most\n",
         cases, worst);
}

static void test_ease_strings(void)
{
  /* strings made to break a parser: every one is parsed to something safe, or refused */
  char *longs = (char *)MEM_mallocN(20001, "long ease");
  memset(longs, 'a', 20000);
  longs[20000] = '\0';
  char *deep = (char *)MEM_mallocN(8000, "deep ease");
  deep[0] = '\0';
  for (int i = 0; i < 300; i++) {
    strcat(deep, "exposcale(1,2,");
  }
  static const char *const fuzz[] = {
      "", " ", "(", ")", "linear(", "linear((((", "linear(,,,,)", "linear(nan, inf)", "linear(0 50%, 1 20%, 0 10%)",
      "linear(0, 1 200%)", "steps(", "steps(nan)", "steps(1e30, jump-both)", "steps(-5, nope)", "rough(nan, 1e30)",
      "rough({strength: 1e30, points: -9, taper: x, clamp: true})", "cubic-bezier(nan, 1, 2, inf)",
      "cubic-bezier(1,2,3)", "back.out(nan)", "elastic.out(0, 0)", "elastic.out(1e30, -1e30)", "power9999", "power2.in.out",
      "expo.", ".out", "slow(nan, nan, nan)", "wiggle(nan)", "wiggle(1e30)", "customBounce(1e30)", "exposcale(0, 5)",
      "exposcale(-1, 5)", "exposcale(nan, nan)", "exposcale(1, 2, nope)", "\xff\xfe", "power2.out(((((",
  };
  const float ts[] = {-1.0f, 0.0f, 0.3f, 0.999f, 1.0f, 2.0f, NAN, INFINITY};
  for (size_t i = 0; i < sizeof(fuzz) / sizeof(fuzz[0]) + 2; i++) {
    const char *s = i < sizeof(fuzz) / sizeof(fuzz[0]) ? fuzz[i] : i == sizeof(fuzz) / sizeof(fuzz[0]) ? longs : deep;
    BKEase e;
    const int ok = BK_ease_parse(s, &e);
    for (size_t k = 0; k < sizeof(ts) / sizeof(ts[0]); k++) {
      const float v = BK_ease_eval(&e, ts[k]); /* no crash; finite in 0 .. 1 when it parsed */
      if (ok && ts[k] >= 0.0f && ts[k] <= 1.0f && !isfinite(v)) {
        printf("FAIL: ease '%.40s' at %g is %g\n", s, ts[k], v);
        g_fail = 1;
      }
    }
  }
  CHECK(!BK_ease_parse(longs, &(BKEase){0}));
  MEM_freeN(longs);
  MEM_freeN(deep);

  /* a curve parsed again is the same table */
  BKEase first, again;
  CHECK(BK_ease_parse("linear(0, 0.123, 1)", &first) && BK_ease_parse("linear(0, 0.123, 1)", &again));
  CHECK(first.table == again.table && first.table != 0);
  CHECK(!BK_ease_points(NULL, 5, &again) && !BK_ease_points((const float(*)[2])&first, 1, &again));
  const float bad[2][2] = {{0.0f, NAN}, {1.0f, 1.0f}};
  CHECK(!BK_ease_points(bad, 2, &again) && again.kind == BK_EASE_LINEAR);
  printf("ease strings: junk, very long, deeply nested, NaN: parsed safe or refused; one table per curve\n");
}

/* BK_anim_exit with live motions (a killed one played again too) ends, and starts afresh */
static void test_exit_with_live_motions(void)
{
  Obj o[4] = {{0}};
  BKMotion *m[4];
  for (int i = 0; i < 4; i++) {
    m[i] = to(&o[i], BK_PROP_X, 100.0f, linear(1.0 + i));
  }
  BKMotion *tl = BK_timeline(&(BKMotionVars){.time_scale = 1.0});
  BK_timeline_add(tl, m[3], "0");
  BK_motion_kill(m[0]);
  BK_motion_play(m[0]);
  BK_motion_seek(m[0], 0.5);
  step(0.25);
  BK_anim_exit(); /* must return, with every motion out of the global timeline */
  CHECK(!BK_anim_active());
  for (int i = 0; i < 4; i++) {
    BK_motion_release(m[i]);
  }
  BK_motion_release(tl);
  BK_anim_tick(g_now); /* afresh: the clock starts again */
  Obj again = {0};
  BKMotion *n = to(&again, BK_PROP_X, 10.0f, linear(1.0));
  step(0.5);
  CHECK(NEAR(again.x, 5.0, 1e-3));
  BK_motion_release(n);
  step(1.0);
  printf("exit: live and killed motions leave, the engine starts again\n");
}

static void test_ease_pool(void)
{
  /* an ease from before BK_anim_exit is no curve now, not the one that took its table id */
  BKEase old, fresh;
  CHECK(BK_ease_parse("linear(0, 0.1, 1)", &old));
  CHECK(NEAR(BK_ease_eval(&old, 0.5f), 0.1, 1e-6));
  BKEase old_scale;
  CHECK(BK_ease_parse("exposcale(1, 2, power2.in)", &old_scale));
  BK_anim_exit();
  CHECK(NEAR(BK_ease_eval(&old, 0.5f), 0.5, 1e-6)); /* none: as it is */
  CHECK(isfinite(BK_ease_eval(&old_scale, 0.5f)));
  CHECK(BK_ease_parse("linear(0, 0.9, 1)", &fresh));
  CHECK(NEAR(BK_ease_eval(&fresh, 0.5f), 0.9, 1e-6) && NEAR(BK_ease_eval(&old, 0.5f), 0.5, 1e-6));
  /* a bezier too: its table is checked against the curve */
  BKEase bez = BK_ease_bezier(0.1f, 0.9f, 0.2f, 1.0f);
  const float before = BK_ease_eval(&bez, 0.5f);
  BK_anim_exit();
  BKEase other = BK_ease_bezier(0.9f, 0.0f, 0.9f, 0.0f);
  (void)other;
  CHECK(NEAR(BK_ease_eval(&bez, 0.5f), before, 1e-3));

  /* the same string again makes no table; distinct ones stop at the pool's size, and parse as an error */
  char name[64];
  int made = 0, refused = 0;
  for (int i = 0; i < 70000; i++) {
    CHECK(BK_ease_parse("linear(0, 2.5, 1)", &fresh));
  }
  for (int i = 0; i < 70000; i++) {
    snprintf(name, sizeof(name), "linear(0, %.9g, 1)", (double)i / 70000.0);
    BKEase e;
    if (BK_ease_parse(name, &e)) {
      made++;
      CHECK(e.table != 0 && isfinite(BK_ease_eval(&e, 0.5f)));
    }
    else {
      refused++;
      CHECK(e.kind == BK_EASE_LINEAR);
    }
  }
  CHECK(made + 1 == BK_EASE_POOL_MAX && refused == 70000 - made); /* the 2.5 curve is one of them */
  BKEase last;
  CHECK(!BK_ease_parse("exposcale(1, 2, none)", &last)); /* full: a refusal, not a wrapped id */
  CHECK(BK_ease_parse("linear(0, 2.5, 1)", &fresh) && fresh.table != 0); /* still found: no new table */
  printf("ease pool: stale ids, one table per curve, %d made, %d refused\n", made, refused);
}

/** \} */

int main(void)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  if (!BK_anim_available()) {
    printf("anim_test: no animation in this build, skipped\n");
    return 77;
  }
  const unsigned blocks0 = MEM_get_memory_blocks_in_use();
  BK_anim_set_lag_smoothing(0.0, 0.0); /* the tests step whole seconds */
  BK_anim_tick(g_now);
  test_easing();
  test_springs();
  test_tweens();
  test_timelines();
  test_overwrite_and_springs();
  test_lag_smoothing();
  test_kill();
  test_kit_nodes();
  test_no_allocation_per_frame();
  test_callbacks_that_change_the_tree();
  test_killed_stays_killed();
  test_numbers();
  test_positions();
  test_tween_boundary();
  test_degenerate_springs();
  test_settle_closed_form();
  test_ease_strings();
  test_exit_with_live_motions();
  test_ease_pool();
  BK_anim_exit();
  printf("memory: %u blocks before, %u after\n", blocks0, MEM_get_memory_blocks_in_use());
  CHECK(MEM_get_memory_blocks_in_use() == blocks0);
  printf(g_fail ? "anim_test: FAILED\n" : "anim_test: ok\n");
  return g_fail;
}
