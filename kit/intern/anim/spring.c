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

/* The undamped frequency and the damping ratio: a mass, stiffness or damping of 0 (or less)
 * is the smallest the formulas allow, so a degenerate spring is slow, not a division by 0. */
static void natural(const BKSpring *s, double *r_w0, double *r_zeta)
{
  const double m = s->mass > 0.0f ? s->mass : 1.0, k = BK_MAX(1e-6, (double)s->stiffness);
  const double c = BK_MAX(0.0, (double)s->damping);
  *r_w0 = sqrt(k / m);
  *r_zeta = c / (2.0 * sqrt(k * m));
}

/* Displacement from `to` and its velocity at `t`: x(t) = to + d(t). */
void BK_spring_eval(const BKSpring *s, float from, float to, double t, float *r_pos, float *r_vel)
{
  double w0, zeta;
  natural(s, &w0, &zeta);
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

/* -------------------------------------------------------------------- */
/** \name Settling
 *
 * Motion steps the spring at 120 Hz until it stays within its rest thresholds (up to 20 s).
 * The displacement and the velocity are both f(t) = e^{-a t} (p c(t) + q s(t)), with (c, s) =
 * (cos, sin) of the damped frequency (under), (1, t) (critical) or (cosh, sinh) (over), so the
 * last time |f| is above a threshold is found in a few evaluations instead of 2400 per
 * property: under-damped, from the peaks of the oscillation (their size is known); the
 * others, from the pieces between f's zero and its extremum, where |f| only falls or rises.
 * \{ */

#define SETTLE_MAX 20.0

enum { RESP_UNDER, RESP_CRIT, RESP_OVER };

typedef struct Response {
  int kind;
  double a, w, p, q;
} Response;

static double response_at(const Response *f, double t)
{
  switch (f->kind) {
    case RESP_UNDER:
      return exp(-f->a * t) * (f->p * cos(f->w * t) + f->q * sin(f->w * t));
    case RESP_OVER: /* the two exponentials, which never overflow (w < a) */
      return 0.5 * (f->p + f->q) * exp((f->w - f->a) * t) + 0.5 * (f->p - f->q) * exp(-(f->a + f->w) * t);
    default:
      return exp(-f->a * t) * (f->p + f->q * t);
  }
}

static Response response_derivative(const Response *f)
{
  Response d = *f;
  switch (f->kind) {
    case RESP_UNDER:
      d.p = -f->a * f->p + f->q * f->w;
      d.q = -f->a * f->q - f->p * f->w;
      break;
    case RESP_OVER:
      d.p = -f->a * f->p + f->q * f->w;
      d.q = -f->a * f->q + f->p * f->w;
      break;
    default:
      d.p = -f->a * f->p + f->q;
      d.q = -f->a * f->q;
      break;
  }
  return d;
}

/* the displacement from `to` as a Response */
static Response spring_response(const BKSpring *s, float from, float to)
{
  double w0, zeta;
  natural(s, &w0, &zeta);
  const double d0 = (double)from - (double)to, a = zeta * w0;
  Response f = {RESP_CRIT, a, 0.0, d0, (double)s->velocity + a * d0};
  const double w = w0 * sqrt(fabs(1.0 - zeta * zeta));
  if (w > 1e-9 * w0) {
    f.kind = zeta < 1.0 ? RESP_UNDER : RESP_OVER;
    f.w = w;
    f.q /= w;
  }
  return f;
}

/* where |f| falls to `th` in (lo, hi], when it is above it at lo and not above it at hi
 * (|f| going one way: a bisection) */
static double response_fall(const Response *f, double th, double lo, double hi)
{
  for (int i = 0; i < 40; i++) {
    const double mid = 0.5 * (lo + hi);
    if (fabs(response_at(f, mid)) > th) {
      lo = mid;
    }
    else {
      hi = mid;
    }
  }
  return hi;
}

/* the last time an under-damped |f| is above `th` */
static double exit_wave(const Response *f, double th)
{
  const double amp = hypot(f->p, f->q);
  if (amp <= th) {
    return 0.0;
  }
  if (f->a < 1e-9) {
    return SETTLE_MAX; /* it never decays */
  }
  /* f = amp e^{-a t} cos(w t - phi): its extrema k are at (phi - delta + k pi) / w, as large as
   * amp cos(delta) e^{-a t} */
  const double phi = atan2(f->q, f->p), delta = atan2(f->a, f->w);
  const double t_lim = log(amp * cos(delta) / th) / f->a; /* extrema after it are within th */
  const double k0 = floor((delta - phi) / PI_D) + 1.0;    /* the first after 0 */
  const double k1 = t_lim > 0.0 ? ceil((f->w * t_lim - phi + delta) / PI_D) - 1.0 : k0 - 1.0;
  double lo = 0.0, hi;
  if (k1 >= k0) { /* the last extremum above th, and the next (within it) */
    lo = (phi - delta + k1 * PI_D) / f->w;
    hi = lo + PI_D / f->w;
  }
  else if (fabs(f->p) <= th) {
    return 0.0; /* it starts within, and every extremum is */
  }
  else {
    hi = (phi - delta + k0 * PI_D) / f->w;
  }
  return lo >= SETTLE_MAX ? SETTLE_MAX : response_fall(f, th, lo, BK_MIN(hi, SETTLE_MAX));
}

/* the time f (critical or over-damped) is 0, or -1 */
static double response_zero(const Response *f)
{
  if (f->q == 0.0) {
    return -1.0;
  }
  if (f->kind == RESP_CRIT) {
    return -f->p / f->q;
  }
  const double r = -f->p / f->q; /* tanh(w t) = r */
  return fabs(r) < 1.0 ? atanh(r) / f->w : -1.0;
}

/* the last time a critically or over-damped |f| is above `th` */
static double exit_decay(const Response *f, double th)
{
  /* |f| <= k e^{-rate t} */
  const bool crit = f->kind == RESP_CRIT;
  const double k = crit ? fabs(f->p) + 2.0 * fabs(f->q) / (2.718281828459045 * f->a) : fabs(f->p) + fabs(f->q);
  const double rate = crit ? 0.5 * f->a : f->a - f->w;
  if (k <= th) {
    return 0.0;
  }
  if (rate < 1e-9) {
    return SETTLE_MAX;
  }
  /* the pieces where |f| is monotone: split at its zero and its extremum (f' = 0) */
  const double up = BK_MIN(log(k / th) / rate, SETTLE_MAX);
  const Response d = response_derivative(f);
  double bp[4] = {0.0, response_zero(f), response_zero(&d), up};
  int n = 1;
  for (int i = 1; i < 3; i++) {
    if (bp[i] > 0.0 && bp[i] < up) {
      bp[n++] = bp[i];
    }
  }
  if (n == 3 && bp[1] > bp[2]) {
    const double swap = bp[1];
    bp[1] = bp[2];
    bp[2] = swap;
  }
  bp[n++] = up;
  for (int i = n - 1; i > 0; i--) {
    if (fabs(response_at(f, bp[i - 1])) > th) {
      return response_fall(f, th, bp[i - 1], bp[i]);
    }
  }
  return 0.0;
}

static double response_exit(const Response *f, double th)
{
  return f->kind == RESP_UNDER ? exit_wave(f, th) : exit_decay(f, th);
}

double BK_spring_settle(const BKSpring *s, float from, float to)
{
  if (!isfinite(s->stiffness) || !isfinite(s->damping) || !isfinite(s->mass) || !isfinite(s->velocity) ||
      !isfinite(from) || !isfinite(to))
  {
    return 0.0;
  }
  double rd, rs;
  rest(s, from, to, &rd, &rs);
  const Response pos = spring_response(s, from, to), vel = response_derivative(&pos);
  return BK_MAX(response_exit(&pos, rd), response_exit(&vel, rs));
}

/** \} */

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
