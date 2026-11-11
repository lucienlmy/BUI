/* Springs and inertia, as Motion defines them, solved in closed form: a spring's position and
 * velocity at any time are formulas (under-, critically or over-damped), not a simulation, so
 * a frame costs a few operations and any time can be asked for (seeking, timelines).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <math.h>

#include "anim_intern.h"

#define PI_D 3.14159265358979323846

BKSpring BK_spring_default(void)
{
  return (BKSpring){100.0f, 10.0f, 1.0f, 0.0f, 0.0f, 0.0f};
}

BKSpring BK_spring_bounce(float bounce, float visual_duration)
{
  /* Motion: root = 2 pi / (visualDuration * 1.2), stiffness = root^2,
   * damping = 2 * clamp(0.05, 1, 1 - bounce) * sqrt(stiffness) */
  BKSpring s = BK_spring_default();
  const double root = 2.0 * PI_D / (BK_MAX(0.01, (double)visual_duration) * 1.2);
  s.stiffness = (float)(root * root);
  s.damping = (float)(2.0 * BK_CLAMP(1.0 - (double)bounce, 0.05, 1.0) * sqrt((double)s.stiffness));
  return s;
}

static double angular(double f, double ratio)
{
  return f * sqrt(1.0 - ratio * ratio);
}

BKSpring BK_spring_duration(float duration, float bounce, float velocity, float mass)
{
  /* Motion's findSpring: the undamped frequency whose envelope has decayed to `safe_min` at
   * `duration`, by Newton's method from 5 / duration */
  const double safe_min = 0.001, v = velocity;
  const double ratio = BK_CLAMP(1.0 - (double)bounce, 0.05, 1.0);
  const double d = BK_CLAMP((double)duration, 0.01, 10.0);
  double f = 5.0 / d;
  for (int i = 0; i < 12; i++) {
    double env, der;
    if (ratio < 1.0) {
      const double decay = f * ratio, delta = decay * d;
      env = safe_min - (decay - v) / angular(f, ratio) * exp(-delta);
      const double dd = delta * v + v, e = ratio * ratio * f * f * d, g = angular(f * f, ratio);
      const double factor = -env + safe_min > 0.0 ? -1.0 : 1.0;
      der = factor * ((dd - e) * exp(-delta)) / g;
    }
    else {
      env = -safe_min + exp(-f * d) * ((f - v) * d + 1.0);
      der = exp(-f * d) * ((v - f) * d * d);
    }
    if (fabs(der) < 1e-12) {
      break;
    }
    f -= env / der;
  }
  BKSpring s = BK_spring_default();
  const double m = mass > 0.0f ? mass : 1.0;
  if (isfinite(f) && f > 0.0) {
    s.stiffness = (float)(f * f * m);
    s.damping = (float)(ratio * 2.0 * sqrt(m * (double)s.stiffness));
  }
  s.mass = (float)m;
  s.velocity = velocity;
  return s;
}

/* Displacement from `to` and its velocity at `t`: x(t) = to + d(t). */
void BK_spring_eval(const BKSpring *s, float from, float to, double t, float *r_pos, float *r_vel)
{
  const double m = s->mass > 0.0f ? s->mass : 1.0, k = BK_MAX(1e-6, (double)s->stiffness);
  const double c = BK_MAX(0.0, (double)s->damping);
  const double w0 = sqrt(k / m), zeta = c / (2.0 * sqrt(k * m));
  const double d0 = (double)from - (double)to, v0 = s->velocity;
  double d, v;
  if (zeta < 1.0) { /* underdamped: it oscillates */
    const double wd = w0 * sqrt(1.0 - zeta * zeta), e = exp(-zeta * w0 * t);
    const double b = (v0 + zeta * w0 * d0) / wd, cs = cos(wd * t), sn = sin(wd * t);
    d = e * (d0 * cs + b * sn);
    v = e * ((b * wd - zeta * w0 * d0) * cs - (d0 * wd + zeta * w0 * b) * sn);
  }
  else if (zeta == 1.0) { /* critically damped */
    const double e = exp(-w0 * t), b = v0 + w0 * d0;
    d = e * (d0 + b * t);
    v = e * (b - w0 * (d0 + b * t));
  }
  else { /* overdamped */
    const double r = w0 * sqrt(zeta * zeta - 1.0), r1 = -zeta * w0 + r, r2 = -zeta * w0 - r;
    const double c1 = (v0 - r2 * d0) / (r1 - r2), c2 = d0 - c1;
    const double e1 = exp(r1 * t), e2 = exp(r2 * t);
    d = c1 * e1 + c2 * e2;
    v = c1 * r1 * e1 + c2 * r2 * e2;
  }
  if (r_pos) {
    *r_pos = (float)((double)to + d);
  }
  if (r_vel) {
    *r_vel = (float)v;
  }
}

/* Motion's rest thresholds: finer for small distances (scales, opacity) */
static void rest(const BKSpring *s, float from, float to, double *r_delta, double *r_speed)
{
  const bool granular = fabsf(to - from) < 5.0f;
  *r_delta = s->rest_delta > 0.0f ? s->rest_delta : (granular ? 0.005 : 0.5);
  *r_speed = s->rest_speed > 0.0f ? s->rest_speed : (granular ? 0.01 : 2.0);
}

double BK_spring_settle(const BKSpring *s, float from, float to)
{
  double rd, rs;
  rest(s, from, to, &rd, &rs);
  /* the first time it stays within both thresholds: stepped at 120 Hz once (made with the
   * tween, never per frame), up to Motion's 20 s */
  double last_out = 0.0;
  for (double t = 0.0; t <= 20.0; t += 1.0 / 120.0) {
    float p, v;
    BK_spring_eval(s, from, to, t, &p, &v);
    if (fabsf(p - to) > rd || fabsf(v) > rs) {
      last_out = t + 1.0 / 120.0;
    }
    else if (t - last_out > 0.1) {
      break; /* settled for a tenth of a second: it stays */
    }
  }
  return last_out;
}

BKInertia BK_inertia_default(float velocity)
{
  return (BKInertia){velocity, 0.8f, 0.325f, 1.0f, -1.0f, 500.0f, 10.0f, 0.5f};
}

/* -------------------------------------------------------------------- */
/** \name One value driven by physics (motion.c's spring and inertia tracks)
 * \{ */

void bk_physics_init(BKPhysics *ph, int kind, const BKSpring *s, const BKInertia *in, float from, float to)
{
  ph->kind = kind;
  ph->from = from;
  ph->to = to;
  ph->switch_t = -1.0;
  if (kind == BK_MOTION_SPRING_KIND) {
    ph->spring = *s;
    ph->duration = BK_spring_settle(s, from, to);
    return;
  }
  /* inertia: decays toward origin + power * velocity; at a bound, a spring to it */
  ph->inertia = *in;
  ph->amplitude = in->power * in->velocity;
  ph->to = from + ph->amplitude;
  const bool bounded = in->min <= in->max;
  const float tc = BK_MAX(1e-3f, in->time_constant);
  /* when the decay crosses a bound: target + delta(t) = bound */
  if (bounded && (ph->to < in->min || ph->to > in->max)) {
    const float bound = ph->to < in->min ? in->min : in->max;
    const float frac = (bound - ph->to) / -ph->amplitude; /* exp(-t / tc) */
    ph->switch_t = frac > 0.0f && frac <= 1.0f ? -tc * logf(frac) : 0.0;
    ph->bound = bound;
    float pos, vel;
    bk_physics_eval(ph, ph->switch_t - 1e-6, &pos, &vel);
    ph->bspring = (BKSpring){in->bounce_stiffness, in->bounce_damping, 1.0f, vel, in->rest_delta, 0.0f};
    ph->duration = ph->switch_t + BK_spring_settle(&ph->bspring, bound, bound);
    return;
  }
  /* settled when the decay is within rest_delta */
  const float a = fabsf(ph->amplitude), rd = BK_MAX(1e-4f, in->rest_delta);
  ph->duration = a > rd ? tc * logf(a / rd) : 0.0;
}

void bk_physics_eval(const BKPhysics *ph, double t, float *r_pos, float *r_vel)
{
  if (ph->kind == BK_MOTION_SPRING_KIND) {
    BK_spring_eval(&ph->spring, ph->from, ph->to, t, r_pos, r_vel);
    return;
  }
  if (ph->switch_t >= 0.0 && t >= ph->switch_t) {
    BK_spring_eval(&ph->bspring, ph->bound, ph->bound, t - ph->switch_t, r_pos, r_vel);
    return;
  }
  const double tc = BK_MAX(1e-3, (double)ph->inertia.time_constant), e = exp(-t / tc);
  *r_pos = (float)((double)ph->to - (double)ph->amplitude * e);
  if (r_vel) {
    *r_vel = (float)((double)ph->amplitude / tc * e);
  }
}

/** \} */
