/* Easing: every family is one "in" curve (Blender's easing.cc, Penner's equations, as GSAP
 * and Motion use them); out is its reverse and in-out its mirror, so each is written once.
 * Curves given as such (cubic-bezier, steps, linear() and sampled curves) are used as they
 * are. Names are parsed once into a BKEase; evaluating one is a switch and a few operations.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "anim_intern.h"

#define PI_F 3.14159265358979f

/* -------------------------------------------------------------------- */
/** \name The pool: point tables and inner eases (kept for the program's life)
 * \{ */

typedef struct Table {
  int n;
  float *xy;     /* n (x, y) points, x ascending */
  BKEase inner;  /* expoScale: the ease it scales */
} Table;

static struct {
  Table *t;
  int n, cap;
  struct Named { char *name; BKEase ease; } *names;
  int nnames, cap_names;
} P;

/* Bezier lookup tables: y at LUT_N + 1 evenly spaced x, made once per distinct curve (found
 * again by its control points: Motion makes the same few curves for every motion). A frame
 * then reads two samples instead of solving the cubic (error about 1e-5). */
#define LUT_N 256
static struct {
  struct Lut { float p[4]; float *y; } *l;
  int n, cap;
} L;

static float lut_eval(const float *y, float t);

/* cos(t pi / 2) for t in 0 .. 1 without a library call (sine eases, every frame): its series
 * in t^2 to the 10th power (error below 1e-6). */
static inline float cos_quarter(float t)
{
  const float x = t * t;
  return 1.0f + x * (-1.2337005501f + x * (0.2536695079f + x * (-0.0208634807f + x * (0.0009192602f +
                                                                                        x * -0.0000252020f))));
}

/* 2^x without a library call (expo eases, every frame): the power of two in the exponent
 * bits, 2^fraction from a polynomial (relative error about 2e-7). x in -126 .. 126. */
static inline float fast_exp2(float x)
{
  x = BK_CLAMP(x, -126.0f, 126.0f);
  const float fi = (float)((int)(x + 126.0f) - 126), f = x - fi; /* floor, without a call */
  const float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
  union { uint32_t u; float f; } b = {(uint32_t)((int)fi + 127) << 23};
  return b.f * p;
}

static int pool_add(float *xy, int n, const BKEase *inner)
{
  if (P.n == P.cap) {
    P.cap = P.cap ? P.cap * 2 : 16;
    P.t = (Table *)MEM_reallocN(P.t, sizeof(Table) * (size_t)P.cap);
  }
  P.t[P.n] = (Table){n, xy, inner ? *inner : (BKEase){0}};
  return ++P.n; /* 0 is "none" */
}

void bk_ease_exit(void)
{
  for (int i = 0; i < P.n; i++) {
    MEM_SAFE_FREE(P.t[i].xy);
  }
  for (int i = 0; i < P.nnames; i++) {
    MEM_freeN(P.names[i].name);
  }
  MEM_SAFE_FREE(P.t);
  MEM_SAFE_FREE(P.names);
  memset(&P, 0, sizeof(P));
  for (int i = 0; i < L.n; i++) {
    MEM_freeN(L.l[i].y);
  }
  MEM_SAFE_FREE(L.l);
  memset(&L, 0, sizeof(L));
}

static float table_eval(const Table *tb, float x)
{
  const float *p = tb->xy;
  if (x <= p[0]) {
    return p[1];
  }
  if (x >= p[2 * (tb->n - 1)]) {
    return p[2 * (tb->n - 1) + 1];
  }
  int lo = 0, hi = tb->n - 1; /* the segment holding x */
  while (hi - lo > 1) {
    const int mid = (lo + hi) / 2;
    if (p[2 * mid] <= x) {
      lo = mid;
    }
    else {
      hi = mid;
    }
  }
  const float x0 = p[2 * lo], x1 = p[2 * hi], y0 = p[2 * lo + 1], y1 = p[2 * hi + 1];
  return x1 > x0 ? y0 + (y1 - y0) * (x - x0) / (x1 - x0) : y1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The curves
 * \{ */

static float bounce_out(float t)
{
  const float n1 = 7.5625f, d1 = 2.75f;
  if (t < 1.0f / d1) {
    return n1 * t * t;
  }
  if (t < 2.0f / d1) {
    t -= 1.5f / d1;
    return n1 * t * t + 0.75f;
  }
  if (t < 2.5f / d1) {
    t -= 2.25f / d1;
    return n1 * t * t + 0.9375f;
  }
  t -= 2.625f / d1;
  return n1 * t * t + 0.984375f;
}

/* cubic-bezier(x1, y1, x2, y2): x(s) solved for s (Newton, then bisection), y(s) returned */
static float bezier(const float *p, float x)
{
  const float cx = 3.0f * p[0], bx = 3.0f * (p[2] - p[0]) - cx, ax = 1.0f - cx - bx;
  const float cy = 3.0f * p[1], by = 3.0f * (p[3] - p[1]) - cy, ay = 1.0f - cy - by;
  float s = x;
  for (int i = 0; i < 8; i++) {
    const float fx = ((ax * s + bx) * s + cx) * s - x;
    const float d = (3.0f * ax * s + 2.0f * bx) * s + cx;
    if (fabsf(fx) < 1e-6f) {
      return ((ay * s + by) * s + cy) * s;
    }
    if (fabsf(d) < 1e-6f) {
      break;
    }
    s -= fx / d;
  }
  float lo = 0.0f, hi = 1.0f;
  s = x;
  for (int i = 0; i < 32 && lo < hi; i++) {
    const float fx = ((ax * s + bx) * s + cx) * s;
    if (fabsf(fx - x) < 1e-6f) {
      break;
    }
    if (fx < x) {
      lo = s;
    }
    else {
      hi = s;
    }
    s = 0.5f * (lo + hi);
  }
  return ((ay * s + by) * s + cy) * s;
}

static float steps(const float *p, float t)
{
  const int n = BK_MAX(1, (int)p[0]), pos = (int)p[1];
  if (t >= 1.0f) {
    return 1.0f;
  }
  if (t < 0.0f) {
    return 0.0f;
  }
  int step = (int)floorf(t * (float)n);
  if (pos == BK_STEPS_START || pos == BK_STEPS_BOTH) {
    step++;
  }
  const int jumps = pos == BK_STEPS_NONE ? n - 1 : pos == BK_STEPS_BOTH ? n + 1 : n;
  return jumps > 0 ? BK_CLAMP((float)step / (float)jumps, 0.0f, 1.0f) : 1.0f;
}

/* GSAP's SlowMo: ease in, linear through the middle, ease out */
static float slow(const float *p, float t)
{
  const float ratio = BK_MIN(1.0f, p[0]), power = p[1], p1 = (1.0f - ratio) * 0.5f, p3 = p1 + ratio;
  const bool yoyo = p[2] != 0.0f;
  const float r = t + (0.5f - t) * power;
  if (t < p1) {
    const float u = 1.0f - t / p1;
    return yoyo ? 1.0f - u * u : r - u * u * u * u * r;
  }
  if (t > p3) {
    const float u = (t - p3) / p1;
    return yoyo ? (t >= 1.0f ? 0.0f : 1.0f - u * u) : r + (t - r) * u * u * u * u;
  }
  return yoyo ? 1.0f : r;
}

/* Motion's backIn: the reverse of backOut = cubic-bezier(0.33, 1.53, 0.69, 0.99) */
static const float MOTION_BACK_OUT[4] = {0.33f, 1.53f, 0.69f, 0.99f};
static float motion_back_in(float t)
{
  return 1.0f - bezier(MOTION_BACK_OUT, 1.0f - t);
}

/* The base ("in") curve of the parametric families. */
static float base_in(const BKEase *e, float t)
{
  switch (e->kind) {
    case BK_EASE_POWER: { /* GSAP's are whole powers (quad .. quint): multiplied, not powf */
      const int k = (int)e->p[0];
      if ((float)k != e->p[0] || k < 1 || k > 8) {
        return powf(t, e->p[0]);
      }
      float r = t;
      for (int i = 1; i < k; i++) {
        r *= t;
      }
      return r;
    }
    case BK_EASE_SINE:
      return 1.0f - (t >= 0.0f && t <= 1.0f ? cos_quarter(t) : cosf(t * PI_F * 0.5f));
    case BK_EASE_EXPO:
      return t <= 0.0f ? 0.0f : fast_exp2(10.0f * (t - 1.0f));
    case BK_EASE_CIRC:
      return 1.0f - sqrtf(BK_MAX(0.0f, 1.0f - t * t));
    case BK_EASE_BACK: {
      const float s = e->p[0];
      return t * t * ((s + 1.0f) * t - s);
    }
    case BK_EASE_ELASTIC: { /* in(t) = 1 - out(1 - t), out as GSAP and Penner */
      const float a = BK_MAX(1.0f, e->p[0]), period = e->p[1] / BK_MIN(1.0f, BK_MAX(e->p[0], 1e-3f));
      const float s = period / (2.0f * PI_F) * asinf(1.0f / a);
      const float u = 1.0f - t;
      const float out = u >= 1.0f ? 1.0f :
                                    a * fast_exp2(-10.0f * u) * sinf((u - s) * 2.0f * PI_F / period) + 1.0f;
      return 1.0f - out;
    }
    case BK_EASE_BOUNCE:
      return 1.0f - bounce_out(1.0f - t);
    default:
      return t;
  }
}

float BK_ease_eval(const BKEase *e, float t)
{
  switch (e->kind) {
    case BK_EASE_LINEAR:
      return t;
    case BK_EASE_BEZIER:
      if (t <= 0.0f || t >= 1.0f) {
        return t;
      }
      /* a table from before BK_anim_exit, or none: solve */
      return e->table && e->table <= L.n ? lut_eval(L.l[e->table - 1].y, t) : bezier(e->p, t);
    case BK_EASE_STEPS:
      return steps(e->p, t);
    case BK_EASE_TABLE:
      return e->table > 0 && e->table <= P.n ? table_eval(&P.t[e->table - 1], t) : t;
    case BK_EASE_SLOW:
      return slow(e->p, BK_CLAMP(t, 0.0f, 1.0f));
    case BK_EASE_EXPO_SCALE: {
      const float a = e->p[0], b = e->p[1];
      const float inner = e->table ? BK_ease_eval(&P.t[e->table - 1].inner, t) : t;
      return fabsf(b - a) < 1e-9f ? t : (a * powf(b / a, inner) - a) / (b - a);
    }
    case BK_EASE_ANTICIPATE: {
      const float u = t * 2.0f;
      return u < 1.0f ? 0.5f * motion_back_in(u) : 0.5f * (2.0f - powf(2.0f, -10.0f * (u - 1.0f)));
    }
    default:
      break;
  }
  if (t <= 0.0f || t >= 1.0f) {
    return t <= 0.0f ? 0.0f : 1.0f;
  }
  switch (e->mode) {
    case BK_EASE_OUT:
      return 1.0f - base_in(e, 1.0f - t);
    case BK_EASE_IN_OUT:
      return t < 0.5f ? 0.5f * base_in(e, 2.0f * t) : 1.0f - 0.5f * base_in(e, 2.0f - 2.0f * t);
    default:
      return base_in(e, t);
  }
}

static int bezier_lut(const float p[4])
{
  for (int i = 0; i < L.n; i++) {
    if (memcmp(L.l[i].p, p, sizeof(float[4])) == 0) {
      return i + 1;
    }
  }
  if (L.n >= 0xffff) {
    return 0; /* solve instead */
  }
  if (L.n == L.cap) {
    L.cap = L.cap ? L.cap * 2 : 8;
    L.l = (struct Lut *)MEM_reallocN(L.l, sizeof(*L.l) * (size_t)L.cap);
  }
  struct Lut *lut = &L.l[L.n++];
  memcpy(lut->p, p, sizeof(float[4]));
  lut->y = (float *)MEM_mallocN(sizeof(float) * (LUT_N + 1), "bezier lut");
  for (int i = 0; i <= LUT_N; i++) {
    const float x = (float)i / LUT_N;
    lut->y[i] = i == 0 ? 0.0f : i == LUT_N ? 1.0f : bezier(p, x);
  }
  return L.n;
}

static float lut_eval(const float *y, float t)
{
  const float f = t * LUT_N;
  const int i = (int)f; /* t in (0, 1): 0 .. LUT_N - 1 */
  return y[i] + (y[i + 1] - y[i]) * (f - (float)i);
}

BKEase BK_ease_bezier(float x1, float y1, float x2, float y2)
{
  BKEase e = {BK_EASE_BEZIER, BK_EASE_AS_IS, 0, {BK_CLAMP(x1, 0.0f, 1.0f), y1, BK_CLAMP(x2, 0.0f, 1.0f), y2}};
  e.table = (uint16_t)bezier_lut(e.p);
  return e;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tables: points, paths and generated curves
 * \{ */

int BK_ease_points(const float (*xy)[2], int n, BKEase *r_ease)
{
  if (!xy || n < 2) {
    return 0;
  }
  float *copy = (float *)MEM_mallocN(sizeof(float) * 2 * (size_t)n, "ease points");
  memcpy(copy, xy, sizeof(float) * 2 * (size_t)n);
  for (int i = 1; i < n; i++) { /* x never goes back */
    copy[2 * i] = BK_MAX(copy[2 * i], copy[2 * (i - 1)]);
  }
  *r_ease = (BKEase){BK_EASE_TABLE, BK_EASE_AS_IS, (uint16_t)pool_add(copy, n, NULL), {0}};
  return 1;
}

/* A curve sampled from a function at `n` x's (generated eases). */
static int sampled(float (*fn)(const float *p, float t), const float *p, int n, BKEase *r_ease)
{
  float(*xy)[2] = (float(*)[2])MEM_mallocN(sizeof(float) * 2 * (size_t)n, "ease samples");
  for (int i = 0; i < n; i++) {
    const float x = (float)i / (float)(n - 1);
    xy[i][0] = x;
    xy[i][1] = fn(p, x);
  }
  const int ok = BK_ease_points((const float(*)[2])xy, n, r_ease);
  MEM_freeN(xy);
  return ok;
}

/* a deterministic random (rough, wiggle "random"): the same curve every run */
static float hash01(uint32_t i)
{
  i ^= i >> 16;
  i *= 0x7feb352dU;
  i ^= i >> 15;
  i *= 0x846ca68bU;
  i ^= i >> 16;
  return (float)(i & 0xffffff) / (float)0x1000000;
}

/* GSAP's CustomWiggle: oscillates `p[0]` times around 0; p[1] the type (0 easeOut,
 * 1 easeInOut, 2 anticipate, 3 uniform, 4 random) */
static float wiggle_fn(const float *p, float t)
{
  const float n = BK_MAX(1.0f, p[0]);
  const int type = (int)p[1];
  float amp = 1.0f - t;
  float phase = t;
  if (type == 1) {
    amp = sinf(PI_F * t);
  }
  else if (type == 2) {
    amp = t < 0.25f ? t * 4.0f : (1.0f - t) / 0.75f;
  }
  else if (type == 3) {
    amp = t < 0.02f || t > 0.98f ? 0.0f : 1.0f;
  }
  else if (type == 4) {
    const float k = floorf(t * n * 2.0f);
    return (hash01((uint32_t)k + 7u) * 2.0f - 1.0f) * (1.0f - t);
  }
  return sinf(2.0f * PI_F * n * phase) * amp;
}

/* GSAP's CustomBounce: a drop, then bounces whose heights shrink by `strength` */
static float custom_bounce_fn(const float *p, float t)
{
  const float k = BK_CLAMP(p[0], 0.0f, 0.999f); /* each bounce's height, of the last */
  /* times: the drop takes 1, a bounce of height h takes 2 * sqrt(h) (a parabola) */
  float total = 1.0f, h = 1.0f;
  for (int i = 0; i < 12 && h > 1e-3f; i++) {
    h *= k;
    total += 2.0f * sqrtf(h);
  }
  float x = t * total;
  if (x < 1.0f) {
    return x * x;
  }
  x -= 1.0f;
  h = 1.0f;
  for (int i = 0; i < 12 && h > 1e-3f; i++) {
    h *= k;
    const float w = 2.0f * sqrtf(h);
    if (x < w) {
      const float u = x / w * 2.0f - 1.0f; /* -1 .. 1 across the bounce */
      return 1.0f - h * (1.0f - u * u);
    }
    x -= w;
  }
  return 1.0f;
}

/* GSAP's RoughEase: a template ease with seeded noise. p[0] strength, p[1] points,
 * p[2] taper (0 none, 1 in, 2 out, 3 both), p[3] clamp */
static int rough(const float *p, const BKEase *tmpl, BKEase *r_ease)
{
  const int n = BK_CLAMP((int)p[1], 2, 200) + 2;
  float(*xy)[2] = (float(*)[2])MEM_mallocN(sizeof(float) * 2 * (size_t)n, "rough ease");
  xy[0][0] = 0.0f;
  xy[0][1] = 0.0f;
  for (int i = 1; i < n - 1; i++) {
    const float x = (float)i / (float)(n - 1);
    float taper = 1.0f;
    const int tp = (int)p[2];
    if (tp == 1 || tp == 3) {
      taper *= x;
    }
    if (tp == 2 || tp == 3) {
      taper *= 1.0f - x;
    }
    float y = BK_ease_eval(tmpl, x) + (hash01((uint32_t)i * 2654435761u) - 0.5f) * p[0] * 0.4f * taper;
    if (p[3] != 0.0f) {
      y = BK_CLAMP(y, 0.0f, 1.0f);
    }
    xy[i][0] = x;
    xy[i][1] = y;
  }
  xy[n - 1][0] = 1.0f;
  xy[n - 1][1] = 1.0f;
  const int ok = BK_ease_points((const float(*)[2])xy, n, r_ease);
  MEM_freeN(xy);
  return ok;
}

/* An SVG path's numbers (CustomEase): commands M L C (and lowercase) in 0..1 space. */
int BK_ease_path(const char *path, BKEase *r_ease)
{
  if (!path) {
    return 0;
  }
  enum { SEG = 32, MAX = 4096 };
  float(*xy)[2] = (float(*)[2])MEM_mallocN(sizeof(float) * 2 * MAX, "ease path");
  int n = 0;
  float cx = 0.0f, cy = 0.0f;
  char cmd = 'M';
  const char *s = path;
  while (*s && n < MAX - SEG) {
    while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\t') {
      s++;
    }
    if (isalpha((unsigned char)*s)) {
      cmd = *s++;
      continue;
    }
    char *end;
    float v[6];
    const int need = (cmd == 'C' || cmd == 'c') ? 6 : 2;
    int got = 0;
    for (; got < need; got++) {
      while (*s == ' ' || *s == ',') {
        s++;
      }
      v[got] = strtof(s, &end);
      if (end == s) {
        break;
      }
      s = end;
    }
    if (got < need) {
      break;
    }
    const bool rel = islower((unsigned char)cmd);
    if (rel) {
      for (int i = 0; i < need; i += 2) {
        v[i] += cx;
        v[i + 1] += cy;
      }
    }
    if (cmd == 'C' || cmd == 'c') { /* sample the cubic */
      for (int k = 1; k <= SEG; k++) {
        const float t = (float)k / SEG, u = 1.0f - t;
        const float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
        xy[n][0] = b0 * cx + b1 * v[0] + b2 * v[2] + b3 * v[4];
        xy[n][1] = b0 * cy + b1 * v[1] + b2 * v[3] + b3 * v[5];
        n++;
      }
      cx = v[4];
      cy = v[5];
    }
    else {
      if (cmd == 'M' || cmd == 'm' || n == 0) {
        n = n ? n : 0;
      }
      xy[n][0] = v[0];
      xy[n][1] = v[1];
      n++;
      cx = v[0];
      cy = v[1];
      if (cmd == 'M') {
        cmd = 'L';
      }
      else if (cmd == 'm') {
        cmd = 'l';
      }
    }
  }
  int ok = 0;
  if (n >= 2) { /* normalized: x from the first to the last point, y likewise */
    const float x0 = xy[0][0], x1 = xy[n - 1][0], y0 = xy[0][1], y1 = xy[n - 1][1];
    const float sx = fabsf(x1 - x0) > 1e-9f ? 1.0f / (x1 - x0) : 1.0f;
    const float sy = fabsf(y1 - y0) > 1e-9f ? 1.0f / (y1 - y0) : 1.0f;
    for (int i = 0; i < n; i++) {
      xy[i][0] = (xy[i][0] - x0) * sx;
      xy[i][1] = (xy[i][1] - y0) * sy;
    }
    ok = BK_ease_points((const float(*)[2])xy, n, r_ease);
  }
  MEM_freeN(xy);
  return ok;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Names
 * \{ */

int BK_ease_register(const char *name, const BKEase *e)
{
  if (!name || !e) {
    return 0;
  }
  for (int i = 0; i < P.nnames; i++) {
    if (BLI_strcasecmp(P.names[i].name, name) == 0) {
      P.names[i].ease = *e;
      return 1;
    }
  }
  if (P.nnames == P.cap_names) {
    P.cap_names = P.cap_names ? P.cap_names * 2 : 8;
    P.names = (struct Named *)MEM_reallocN(P.names, sizeof(*P.names) * (size_t)P.cap_names);
  }
  P.names[P.nnames++] = (struct Named){BLI_strdup(name), *e};
  return 1;
}

/* the arguments in parentheses: numbers ("75%" is 0.75), and a word (the steps position,
 * a type) in `r_word` */
static int read_args(const char *s, float *v, int max, char *r_word, size_t word_size)
{
  int n = 0;
  if (r_word) {
    r_word[0] = '\0';
  }
  while (*s && *s != ')') {
    while (*s == ' ' || *s == ',' || *s == '(' || *s == '{' || *s == '}') {
      s++;
    }
    if (!*s || *s == ')') {
      break;
    }
    char *end;
    const float f = strtof(s, &end);
    if (end != s) {
      if (n < max) {
        v[n++] = *end == '%' ? f / 100.0f : f;
      }
      s = *end == '%' ? end + 1 : end;
      continue;
    }
    size_t k = 0;
    while (*s && *s != ',' && *s != ')' && *s != ' ' && *s != '}') {
      if (r_word && k + 1 < word_size) {
        r_word[k++] = (char)tolower((unsigned char)*s);
      }
      s++;
    }
    if (r_word) {
      r_word[k] = '\0';
    }
  }
  return n;
}

static bool eq(const char *a, const char *b)
{
  return strcmp(a, b) == 0;
}

/* the mode after a GSAP name: ".in", ".out", ".inout", the old ".easein" ... */
static int mode_of(const char *suffix, int fallback)
{
  if (!suffix || !*suffix) {
    return fallback;
  }
  if (eq(suffix, "in") || eq(suffix, "easein")) {
    return BK_EASE_IN;
  }
  if (eq(suffix, "out") || eq(suffix, "easeout")) {
    return BK_EASE_OUT;
  }
  if (eq(suffix, "inout") || eq(suffix, "easeinout")) {
    return BK_EASE_IN_OUT;
  }
  return -1;
}

/* the Penner families by name: kind and exponent */
static bool family(const char *base, BKEase *e)
{
  static const struct { const char *name; uint8_t kind; float p0; } F[] = {
      {"power0", BK_EASE_LINEAR, 0}, {"linear", BK_EASE_LINEAR, 0}, {"none", BK_EASE_LINEAR, 0},
      {"power1", BK_EASE_POWER, 2},  {"quad", BK_EASE_POWER, 2},     {"power2", BK_EASE_POWER, 3},
      {"cubic", BK_EASE_POWER, 3},   {"power3", BK_EASE_POWER, 4},   {"quart", BK_EASE_POWER, 4},
      {"power4", BK_EASE_POWER, 5},  {"quint", BK_EASE_POWER, 5},    {"strong", BK_EASE_POWER, 5},
      {"sine", BK_EASE_SINE, 0},     {"expo", BK_EASE_EXPO, 0},      {"circ", BK_EASE_CIRC, 0},
      {"back", BK_EASE_BACK, 1.70158f}, {"elastic", BK_EASE_ELASTIC, 1}, {"bounce", BK_EASE_BOUNCE, 0},
  };
  for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
    if (eq(base, F[i].name)) {
      *e = (BKEase){F[i].kind, BK_EASE_OUT, 0, {F[i].p0, 0.3f, 0, 0}};
      return true;
    }
  }
  return false;
}

/* names that are one curve, whole (CSS, Motion, Blender) */
static bool whole(const char *n, BKEase *e)
{
  static const struct { const char *name; float p[4]; } B[] = {
      {"ease", {0.25f, 0.1f, 0.25f, 1.0f}},       {"ease-in", {0.42f, 0, 1, 1}},
      {"ease-out", {0, 0, 0.58f, 1}},             {"ease-in-out", {0.42f, 0, 0.58f, 1}},
      {"easein", {0.42f, 0, 1, 1}},               {"easeout", {0, 0, 0.58f, 1}},
      {"easeinout", {0.42f, 0, 0.58f, 1}},        {"bezier", {1.0f / 3, 0, 2.0f / 3, 1}},
      {"backout", {0.33f, 1.53f, 0.69f, 0.99f}},
  };
  for (size_t i = 0; i < sizeof(B) / sizeof(B[0]); i++) {
    if (eq(n, B[i].name)) {
      *e = BK_ease_bezier(B[i].p[0], B[i].p[1], B[i].p[2], B[i].p[3]);
      return true;
    }
  }
  if (eq(n, "step-start") || eq(n, "constant")) {
    *e = (BKEase){BK_EASE_STEPS, BK_EASE_AS_IS, 0, {1.0f, (float)(n[0] == 'c' ? BK_STEPS_END : BK_STEPS_START)}};
    return true;
  }
  if (eq(n, "step-end")) {
    *e = (BKEase){BK_EASE_STEPS, BK_EASE_AS_IS, 0, {1, BK_STEPS_END}};
    return true;
  }
  if (eq(n, "circin") || eq(n, "circout") || eq(n, "circinout")) {
    *e = (BKEase){BK_EASE_CIRC, (uint8_t)(n[4] == 'i' && n[5] == 'n' && !n[6] ? BK_EASE_IN :
                                         n[4] == 'o' ? BK_EASE_OUT : BK_EASE_IN_OUT), 0, {0}};
    return true;
  }
  if (eq(n, "anticipate")) {
    *e = (BKEase){BK_EASE_ANTICIPATE, BK_EASE_AS_IS, 0, {0}};
    return true;
  }
  return false;
}

int BK_ease_parse(const char *name, BKEase *r_ease)
{
  *r_ease = (BKEase){BK_EASE_LINEAR, BK_EASE_AS_IS, 0, {0}};
  if (!name) {
    return 0;
  }
  while (*name == ' ') {
    name++;
  }
  /* registered names first (exact, before the lowercasing) */
  for (int i = 0; i < P.nnames; i++) {
    if (BLI_strcasecmp(P.names[i].name, name) == 0) {
      *r_ease = P.names[i].ease;
      return 1;
    }
  }
  char low[64];
  size_t k = 0;
  const char *args = NULL;
  for (const char *s = name; *s && k + 1 < sizeof(low); s++) {
    if (*s == '(') {
      args = s + 1;
      break;
    }
    if (*s != ' ') {
      low[k++] = (char)tolower((unsigned char)*s);
    }
  }
  low[k] = '\0';
  float v[8] = {0};
  char word[32];
  const int nargs = args ? read_args(args, v, 8, word, sizeof(word)) : 0;
  /* ".config(...)" (GSAP 2): the arguments are the config's */
  char *config = strstr(low, ".config");
  if (config) {
    *config = '\0';
  }

  /* curves given whole */
  if (eq(low, "cubic-bezier") && nargs == 4) {
    *r_ease = BK_ease_bezier(v[0], v[1], v[2], v[3]);
    return 1;
  }
  if (eq(low, "steps")) {
    const int pos = eq(word, "start") || eq(word, "jump-start") ? BK_STEPS_START :
                    eq(word, "jump-none") ? BK_STEPS_NONE : eq(word, "jump-both") ? BK_STEPS_BOTH :
                                                             BK_STEPS_END;
    *r_ease = (BKEase){BK_EASE_STEPS, BK_EASE_AS_IS, 0, {nargs ? v[0] : 1.0f, (float)pos}};
    return 1;
  }
  if (eq(low, "linear") && args) { /* linear(0, 0.25 75%, 1): outputs, with optional input stops */
    float pts[2 * 64];
    int n = 0;
    const char *s = args;
    while (*s && *s != ')' && n < 64) {
      while (*s == ' ' || *s == ',') {
        s++;
      }
      char *end;
      const float y = strtof(s, &end);
      if (end == s) {
        break;
      }
      s = end;
      float x = -1.0f;
      while (*s == ' ') {
        s++;
      }
      const float maybe = strtof(s, &end);
      if (end != s && *end == '%') {
        x = maybe / 100.0f;
        s = end + 1;
      }
      pts[2 * n] = x;
      pts[2 * n + 1] = y;
      n++;
    }
    if (n < 2) {
      return 0;
    }
    if (pts[0] < 0.0f) {
      pts[0] = 0.0f;
    }
    if (pts[2 * (n - 1)] < 0.0f) {
      pts[2 * (n - 1)] = 1.0f;
    }
    for (int i = 1; i < n - 1; i++) { /* stops without an input: spread between their neighbors */
      if (pts[2 * i] >= 0.0f) {
        continue;
      }
      int j = i + 1;
      while (pts[2 * j] < 0.0f) {
        j++;
      }
      const float a = pts[2 * (i - 1)], b = pts[2 * j];
      for (int m = i; m < j; m++) {
        pts[2 * m] = a + (b - a) * (float)(m - i + 1) / (float)(j - i + 1);
      }
    }
    return BK_ease_points((const float(*)[2])pts, n, r_ease);
  }
  if (whole(low, r_ease)) {
    return 1;
  }
  if (eq(low, "backin") || eq(low, "backinout")) { /* Motion's: reverse / mirror of backOut */
    const float rev[4] = {1.0f - MOTION_BACK_OUT[2], 1.0f - MOTION_BACK_OUT[3], 1.0f - MOTION_BACK_OUT[0],
                          1.0f - MOTION_BACK_OUT[1]};
    if (eq(low, "backin")) {
      *r_ease = BK_ease_bezier(rev[0], rev[1], rev[2], rev[3]);
      return 1;
    }
    BKEase in = BK_ease_bezier(rev[0], rev[1], rev[2], rev[3]);
    float xy[65][2];
    for (int i = 0; i <= 64; i++) {
      const float t = (float)i / 64.0f;
      xy[i][0] = t;
      xy[i][1] = t < 0.5f ? 0.5f * BK_ease_eval(&in, 2.0f * t) : 1.0f - 0.5f * BK_ease_eval(&in, 2.0f - 2.0f * t);
    }
    return BK_ease_points((const float(*)[2])xy, 65, r_ease);
  }
  /* EasePack and the ease plugins */
  if (eq(low, "slow")) {
    *r_ease = (BKEase){BK_EASE_SLOW, BK_EASE_AS_IS, 0,
                       {nargs > 0 ? v[0] : 0.7f, nargs > 1 ? v[1] : 0.7f, nargs > 2 ? v[2] : 0.0f, 0}};
    return 1;
  }
  if (eq(low, "exposcale")) {
    BKEase inner = {BK_EASE_LINEAR, BK_EASE_AS_IS, 0, {0}};
    if (word[0]) {
      BK_ease_parse(word, &inner);
    }
    *r_ease = (BKEase){BK_EASE_EXPO_SCALE, BK_EASE_AS_IS, (uint16_t)pool_add(NULL, 0, &inner),
                       {nargs > 0 ? v[0] : 1.0f, nargs > 1 ? v[1] : 2.0f}};
    return 1;
  }
  if (eq(low, "wiggle")) {
    const float type = eq(word, "easeinout") ? 1.0f : eq(word, "anticipate") ? 2.0f :
                       eq(word, "uniform")   ? 3.0f : eq(word, "random") ? 4.0f : 0.0f;
    const float p[2] = {nargs > 0 ? v[0] : 10.0f, type};
    return sampled(wiggle_fn, p, 256, r_ease);
  }
  if (eq(low, "custombounce")) {
    const float p[1] = {nargs > 0 ? v[0] : 0.7f};
    return sampled(custom_bounce_fn, p, 256, r_ease);
  }
  if (eq(low, "rough")) {
    /* rough({strength: 1, points: 20, taper: none, clamp: false}): the numbers in order of
     * the keys present; a taper word picks the taper */
    const float p[4] = {nargs > 0 ? v[0] : 1.0f, nargs > 1 ? v[1] : 20.0f,
                        eq(word, "in") ? 1.0f : eq(word, "out") ? 2.0f : eq(word, "both") ? 3.0f : 0.0f,
                        strstr(args ? args : "", "clamp: true") || strstr(args ? args : "", "clamp:true") ? 1.0f : 0.0f};
    const BKEase none = {BK_EASE_LINEAR, BK_EASE_AS_IS, 0, {0}};
    return rough(p, &none, r_ease);
  }

  /* the Penner families: "power2.out", "back.inOut(2)", "Power2.easeOut", "quad" */
  char base[32];
  const char *dot = strchr(low, '.');
  const size_t blen = dot ? (size_t)(dot - low) : strlen(low);
  if (blen >= sizeof(base)) {
    return 0;
  }
  memcpy(base, low, blen);
  base[blen] = '\0';
  /* Motion's and Blender's run-together names: "quadin", "sineinout" */
  const char *tail = dot ? dot + 1 : "";
  static const char *const MODES[] = {"inout", "in", "out"};
  if (!dot) {
    for (int m = 0; m < 3; m++) {
      const size_t ml = strlen(MODES[m]);
      if (blen > ml && strcmp(base + blen - ml, MODES[m]) == 0) {
        BKEase probe;
        char head[32];
        memcpy(head, base, blen - ml);
        head[blen - ml] = '\0';
        if (family(head, &probe)) {
          memcpy(base, head, blen - ml + 1);
          tail = MODES[m];
          break;
        }
      }
    }
  }
  if (!family(base, r_ease)) {
    *r_ease = (BKEase){BK_EASE_LINEAR, BK_EASE_AS_IS, 0, {0}};
    return 0;
  }
  const int mode = mode_of(tail, BK_EASE_OUT);
  if (mode < 0) {
    *r_ease = (BKEase){BK_EASE_LINEAR, BK_EASE_AS_IS, 0, {0}};
    return 0;
  }
  r_ease->mode = r_ease->kind == BK_EASE_LINEAR ? BK_EASE_AS_IS : (uint8_t)mode;
  if (r_ease->kind == BK_EASE_BACK && nargs > 0) {
    r_ease->p[0] = v[0];
  }
  if (r_ease->kind == BK_EASE_ELASTIC) {
    r_ease->p[0] = nargs > 0 ? v[0] : 1.0f;
    r_ease->p[1] = nargs > 1 ? v[1] : (mode == BK_EASE_IN_OUT ? 0.45f : 0.3f);
  }
  return 1;
}

/** \} */
