/* Motions: tweens and timelines on one clock, as GSAP defines them (Motion's animations are
 * tweens; its sequences are timelines).
 *
 * Every motion maps its parent's time to its own through an anchor (a parent time and the
 * local time there), so pausing, reversing, changing speed and seeking stay continuous.
 * A tween's tracks are keyframes or physics (springs, inertia); a timeline renders its
 * children at their own times, so a jump leaves every child where it belongs.
 *
 * Who drives a property is kept in one hash (target, property) -> track: a new motion on the
 * same property takes it over at once (GSAP's overwrite "auto", Motion's interruption), and
 * a spring that takes over starts with the velocity the property had.
 *
 * A frame (BK_anim_tick) renders the running motions once; nothing is allocated or parsed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "anim_intern.h"

#define NEVER 1e300 /* the total duration of a motion repeating forever */

/* -------------------------------------------------------------------- */
/** \name Types
 * \{ */

typedef struct Track {
  int prop, n, nkeys, unit;
  float *values;   /* nkeys * n: resolved when the tween starts */
  float *times;    /* nkeys, 0 .. 1 */
  BKEase *eases;   /* nkeys - 1 per segment, or NULL: the tween's ease */
  uint8_t *current; /* per key: the value at the start */
  int8_t *rel;     /* per key: relative to the value at the start */
  BKPhysics ph;    /* physics tracks: on the value (n == 1) or on 0 .. 1 (colors) */
  float last[4];
  bool instant : 1;  /* reduced motion: it jumps to its end */
  bool killed : 1;   /* another motion took the property */
  bool has_last : 1;
} Track;

typedef struct Label {
  char *name;
  double time;
} Label;

struct BKMotion {
  /* state flags, one word */
  bool timeline : 1, call : 1, paused : 1, reversed : 1, yoyo : 1, rendered : 1, started : 1, killed : 1,
      resolved : 1;
  int refs;
  int run; /* in the global timeline: its index in G.run */
  BKMotion *parent, *prev, *next; /* in a timeline's children, in order of start */
  /* time: local = anchor_local + dir * (parent_time - anchor_parent) * ts */
  double anchor_parent, anchor_local;
  double ts;
  double dur;      /* one iteration */
  int repeat;
  double rdelay;
  double t;        /* the total time last rendered */
  double it_time;  /* the time in its iteration then: its children's parent time */
  int iteration;
  BKMotionFn on;
  int events;
  void *data;
  void (*free_data)(void *data);
  /* tween */
  const BKAnimClass *cls;
  void *obj;
  BKEase ease;
  int kind, overwrite;
  BKSpring spring;
  BKInertia inertia;
  Track *tracks; /* in the motion's own block, with their keyframes */
  int ntracks;
  /* timeline */
  BKMotion *first, *last, *recent;
  Label *labels;
  int nlabels;
  /* call */
  BKMotionFn fn;
  void *fn_data;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name The global timeline and the drivers hash
 * \{ */

typedef struct Driver {
  void *obj;
  int prop;
  BKMotion *m;
  int track;
} Driver;

static struct {
  BKMotion *root;
  double now, root_time, last_now;
  bool ticked;
  double time_scale;
  double lag_threshold, lag_adjusted;
  bool reduced, host_clock;
  Driver *drivers; /* open addressing; obj NULL = empty, (void*)1 = deleted */
  int ndrivers, cap_drivers;
  void (*wake)(void *data);
  void *wake_data;
  /* the global timeline's motions, dense (a frame is one pass over them; one that ends is
   * swapped with the last): no list to chase, no gaps */
  BKMotion **run;
  int nrun, cap_run;
} G = {NULL, 0, 0, 0, false, 1.0, 0.5, 0.033, false, false, NULL, 0, 0, NULL, NULL, NULL, 0, 0};

/* a motion runs: the host draws frames again */
static void wake(const BKMotion *m)
{
  if (G.wake && !m->paused) {
    G.wake(G.wake_data);
  }
}

void BK_anim_set_wake(void (*fn)(void *data), void *data)
{
  G.wake = fn;
  G.wake_data = data;
}

#define TOMB ((void *)1)

static uint32_t driver_hash(const void *obj, int prop)
{
  uint64_t h = (uint64_t)(uintptr_t)obj * 0x9E3779B97F4A7C15ull ^ (uint64_t)prop * 0xC2B2AE3D27D4EB4Full;
  return (uint32_t)(h ^ (h >> 29));
}

static Driver *driver_find(void *obj, int prop)
{
  if (!G.cap_drivers) {
    return NULL;
  }
  for (uint32_t i = driver_hash(obj, prop) & (G.cap_drivers - 1), k = 0; k < (uint32_t)G.cap_drivers;
       i = (i + 1) & (G.cap_drivers - 1), k++)
  {
    Driver *d = &G.drivers[i];
    if (!d->obj) {
      return NULL;
    }
    if (d->obj == obj && d->prop == prop) {
      return d;
    }
  }
  return NULL;
}

static void driver_set(void *obj, int prop, BKMotion *m, int track);

static void drivers_grow(void)
{
  Driver *old = G.drivers;
  const int old_cap = G.cap_drivers;
  G.cap_drivers = old_cap ? old_cap * 2 : 64;
  G.drivers = (Driver *)MEM_callocN(sizeof(Driver) * (size_t)G.cap_drivers, "anim drivers");
  G.ndrivers = 0;
  for (int i = 0; i < old_cap; i++) {
    if (old[i].obj && old[i].obj != TOMB) {
      driver_set(old[i].obj, old[i].prop, old[i].m, old[i].track);
    }
  }
  MEM_SAFE_FREE(old);
}

static void driver_set(void *obj, int prop, BKMotion *m, int track)
{
  Driver *d = driver_find(obj, prop);
  if (d) {
    d->m = m;
    d->track = track;
    return;
  }
  if ((G.ndrivers + 1) * 2 > G.cap_drivers) {
    drivers_grow();
  }
  for (uint32_t i = driver_hash(obj, prop) & (G.cap_drivers - 1);; i = (i + 1) & (G.cap_drivers - 1)) {
    if (!G.drivers[i].obj || G.drivers[i].obj == TOMB) {
      G.drivers[i] = (Driver){obj, prop, m, track};
      G.ndrivers++;
      return;
    }
  }
}

static void driver_drop(BKMotion *m, int track)
{
  const Track *tr = &m->tracks[track];
  Driver *d = driver_find(m->obj, tr->prop);
  if (d && d->m == m && d->track == track) {
    d->obj = TOMB;
    d->m = NULL;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lifetime
 * \{ */

int BK_anim_available(void)
{
  return 1;
}

/* a motion with `extra` zeroed bytes after it (a tween's tracks and keyframes: one block) */
static BKMotion *motion_alloc(const BKMotionVars *v, size_t extra)
{
  BKMotion *m = (BKMotion *)MEM_callocN(sizeof(BKMotion) + extra, "BKMotion");
  m->refs = 1;
  m->ts = v && v->time_scale > 0.0 ? v->time_scale : 1.0;
  m->dur = v ? BK_MAX(0.0, v->duration) : 0.0;
  m->repeat = v ? v->repeat : 0;
  m->rdelay = v ? BK_MAX(0.0, v->repeat_delay) : 0.0;
  m->yoyo = v && v->yoyo;
  m->paused = v && v->paused;
  m->reversed = v && v->reversed;
  if (v) {
    m->on = v->on;
    m->events = v->events;
    m->data = v->data;
    m->free_data = v->free_data;
  }
  return m;
}

static BKMotion *motion_new(const BKMotionVars *v)
{
  return motion_alloc(v, 0);
}

/* the tracks live in the motion's block: they only let go of what they drive */
static void free_tracks(BKMotion *m)
{
  for (int i = 0; i < m->ntracks; i++) {
    if (!m->tracks[i].killed) {
      driver_drop(m, i);
    }
  }
}

static void detach(BKMotion *m);

static void motion_free(BKMotion *m)
{
  free_tracks(m);
  if (m->cls && m->cls->release) {
    m->cls->release(m->obj);
  }
  for (BKMotion *c = m->first, *next; c; c = next) {
    next = c->next;
    c->parent = NULL;
    c->prev = c->next = NULL;
    BK_motion_release(c); /* the timeline's hold */
  }
  for (int i = 0; i < m->nlabels; i++) {
    MEM_freeN(m->labels[i].name);
  }
  MEM_SAFE_FREE(m->labels);
  if (m->free_data) {
    m->free_data(m->call ? m->fn_data : m->data);
  }
  MEM_freeN(m);
}

void BK_motion_retain(BKMotion *m)
{
  if (m) {
    m->refs++;
  }
}

void BK_motion_release(BKMotion *m)
{
  if (m && --m->refs == 0) {
    motion_free(m);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Durations and time
 * \{ */

static void timeline_measure(BKMotion *tl);

double BK_motion_total_duration(const BKMotion *m)
{
  if (!m) {
    return 0.0;
  }
  if (m->repeat < 0) {
    return NEVER;
  }
  return m->dur * (m->repeat + 1) + m->rdelay * m->repeat;
}

double BK_motion_duration(const BKMotion *m)
{
  return m ? m->dur : 0.0;
}

/* where this motion starts in its parent's time (local 0) */
static double start_in_parent(const BKMotion *m)
{
  return m->anchor_parent - (m->reversed ? -1.0 : 1.0) * m->anchor_local / m->ts;
}

/* the parent's current local time (the root's: the global time) */
static double parent_time(const BKMotion *m)
{
  return m->parent ? m->parent->it_time : G.root_time;
}

/* local time for a parent time */
static double local_at(const BKMotion *m, double pt)
{
  if (m->paused) {
    return m->anchor_local;
  }
  return m->anchor_local + (m->reversed ? -1.0 : 1.0) * (pt - m->anchor_parent) * m->ts;
}

/* keep the local time where it is from now on (before changing speed, direction, pause) */
static void reanchor(BKMotion *m)
{
  const double pt = parent_time(m);
  m->anchor_local = BK_CLAMP(local_at(m, pt), 0.0, BK_motion_total_duration(m));
  m->anchor_parent = pt;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tracks: values at a time
 * \{ */

static bool is_layout_prop(int prop)
{
  return prop != BK_PROP_OPACITY && prop != BK_PROP_COLOR && prop != BK_PROP_BACKGROUND_COLOR &&
         prop != BK_PROP_VALUE;
}

/* the track's start values: the target's current value for "current" keys, relative keys
 * on top of the value at the start */
static void resolve(BKMotion *m)
{
  if (m->resolved) {
    return;
  }
  m->resolved = true;
  double longest = 0.0;
  for (int ti = 0; ti < m->ntracks; ti++) {
    Track *t = &m->tracks[ti];
    if (t->killed) {
      continue;
    }
    /* overwrite: another motion driving this property stops driving it */
    Driver *d = m->overwrite != BK_OVERWRITE_NONE ? driver_find(m->obj, t->prop) : NULL;
    float handed_velocity = 0.0f;
    if (d && d->m && d->m != m) {
      handed_velocity = BK_motion_velocity(d->m, t->prop);
      d->m->tracks[d->track].killed = true;
      if (d->m->on && (d->m->events & BK_EV_MOTION_INTERRUPT)) {
        d->m->on(d->m, BK_EV_MOTION_INTERRUPT, d->m->data);
      }
    }
    if (m->overwrite != BK_OVERWRITE_NONE) {
      driver_set(m->obj, t->prop, m, ti);
    }
    float cur[4] = {0, 0, 0, 1};
    int cur_unit = t->unit;
    bool have = false;
    for (int k = 0; k < t->nkeys; k++) {
      if ((t->current && t->current[k]) || (t->rel && t->rel[k])) {
        if (!have && m->cls && m->cls->get) {
          have = m->cls->get(m->obj, t->prop, cur, &cur_unit) > 0;
        }
        break;
      }
    }
    for (int k = 0; k < t->nkeys; k++) {
      float *v = &t->values[k * t->n];
      if (t->current && t->current[k]) {
        memcpy(v, cur, sizeof(float) * (size_t)t->n);
        if (t->unit == BK_UNIT_NONE) {
          t->unit = cur_unit;
        }
      }
      if (t->rel && t->rel[k]) {
        const float *base = k > 0 ? &t->values[0] : cur;
        for (int c = 0; c < t->n; c++) {
          v[c] = base[c] + (float)t->rel[k] * v[c];
        }
      }
    }
    if (m->kind != BK_MOTION_TWEEN_KIND) {
      BKSpring s = m->spring;
      BKInertia in = m->inertia;
      if (s.velocity == 0.0f) {
        s.velocity = handed_velocity;
      }
      if (in.velocity == 0.0f) {
        in.velocity = handed_velocity;
      }
      const float from = t->n == 1 ? t->values[0] : 0.0f;
      const float to = t->n == 1 ? t->values[(t->nkeys - 1) * t->n] : 1.0f;
      if (t->n != 1) {
        s.velocity = 0.0f;
      }
      bk_physics_init(&t->ph, m->kind, &s, &in, from, to);
      longest = BK_MAX(longest, t->ph.duration);
    }
  }
  if (m->kind != BK_MOTION_TWEEN_KIND) {
    m->dur = longest; /* a physics tween lasts until its slowest value settles */
    if (m->parent && m->parent->timeline) {
      timeline_measure(m->parent);
    }
  }
}

/* the value of track `t` at `it` seconds into an iteration of `dur` (progress `p`) */
static void track_eval(const BKMotion *m, const Track *t, double it, double p, float *out)
{
  if (t->instant) {
    p = 1.0;
    it = m->dur;
  }
  if (m->kind != BK_MOTION_TWEEN_KIND) {
    float pos, vel;
    bk_physics_eval(&t->ph, it, &pos, &vel);
    if (m->kind == BK_MOTION_INERTIA_KIND || t->n == 1) {
      out[0] = pos;
      return;
    }
    const float *a = t->values, *b = &t->values[(t->nkeys - 1) * t->n];
    for (int c = 0; c < t->n; c++) {
      out[c] = a[c] + (b[c] - a[c]) * pos;
    }
    return;
  }
  if (t->nkeys == 1) {
    memcpy(out, t->values, sizeof(float) * (size_t)t->n);
    return;
  }
  /* the segment holding p (keyframe times are ascending) */
  const float pf = (float)p;
  float x = t->eases ? pf : BK_ease_eval(&m->ease, pf);
  int seg = 0;
  if (t->times) {
    while (seg < t->nkeys - 2 && x >= t->times[seg + 1]) {
      seg++;
    }
  }
  else {
    seg = BK_CLAMP((int)floorf(x * (float)(t->nkeys - 1)), 0, t->nkeys - 2);
  }
  const float t0 = t->times ? t->times[seg] : (float)seg / (float)(t->nkeys - 1);
  const float t1 = t->times ? t->times[seg + 1] : (float)(seg + 1) / (float)(t->nkeys - 1);
  float u = t1 > t0 ? (x - t0) / (t1 - t0) : 1.0f;
  if (t->eases) {
    u = BK_ease_eval(&t->eases[seg], BK_CLAMP(u, 0.0f, 1.0f));
  }
  const float *a = &t->values[seg * t->n], *b = &t->values[(seg + 1) * t->n];
  for (int c = 0; c < t->n; c++) {
    out[c] = a[c] + (b[c] - a[c]) * u;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rendering
 * \{ */

static void emit(BKMotion *m, int ev)
{
  if (m->on && (m->events & ev)) {
    m->on(m, ev, m->data);
  }
}

static void render(BKMotion *m, double t, bool force);

/* the iteration and the time in it for a total time */
static double iteration_time(const BKMotion *m, double T, int *r_iter)
{
  const double cycle = m->dur + m->rdelay;
  int iter = cycle > 0.0 ? (int)floor(T / cycle) : 0;
  if (m->repeat >= 0 && iter > m->repeat) {
    iter = m->repeat;
  }
  double it = T - iter * cycle;
  it = BK_MIN(it, m->dur);
  if (m->yoyo && (iter & 1)) {
    it = m->dur - it;
  }
  *r_iter = iter;
  return it;
}

static void render_tween(BKMotion *m, double it)
{
  const double p = m->dur > 0.0 ? it / m->dur : 1.0;
  for (int i = 0; i < m->ntracks; i++) {
    Track *t = &m->tracks[i];
    if (t->killed) {
      continue;
    }
    float v[4];
    track_eval(m, t, it, p, v);
    if (t->has_last && memcmp(v, t->last, sizeof(float) * (size_t)t->n) == 0) {
      continue; /* unchanged: no write, no redraw */
    }
    memcpy(t->last, v, sizeof(float) * (size_t)t->n);
    t->has_last = true;
    if (m->cls && m->cls->set) {
      m->cls->set(m->obj, t->prop, v, t->n, t->unit);
    }
  }
}

static void render_timeline(BKMotion *tl, double it, double prev_it)
{
  for (BKMotion *c = tl->first, *next; c; c = next) {
    next = c->next;
    if (c->call) { /* a callback fires when the playhead crosses it, either way */
      const double at = start_in_parent(c);
      if ((prev_it < at && it >= at) || (prev_it > at && it <= at) || (!tl->rendered && it >= at && at == 0.0)) {
        BK_motion_retain(c);
        c->fn(c, BK_EV_MOTION_COMPLETE, c->fn_data);
        BK_motion_release(c);
      }
      continue;
    }
    BK_motion_retain(c);
    render(c, local_at(c, it), false);
    BK_motion_release(c);
  }
}

/* render `m` at total time `T` (clamped); `force` renders even if the time didn't change */
static void render(BKMotion *m, double T, bool force)
{
  const double total = BK_motion_total_duration(m);
  T = BK_CLAMP(T, 0.0, total);
  if (!force && m->rendered && T == m->t) {
    return;
  }
  const double prev = m->rendered ? m->t : 0.0;
  if (!m->resolved && (T > 0.0 || force) && !m->timeline) {
    resolve(m);
  }
  int iter;
  const double it = iteration_time(m, T, &iter);
  int prev_iter;
  const double prev_it = m->rendered ? iteration_time(m, prev, &prev_iter) : -1.0;
  if (!m->rendered) {
    prev_iter = iter;
  }
  const bool first = !m->rendered;
  m->t = T;
  m->it_time = it;
  if (!m->started && (T > 0.0 || T >= total)) {
    m->started = true;
    emit(m, BK_EV_MOTION_START);
  }
  if (m->timeline) {
    render_timeline(m, it, prev_it);
  }
  else if (m->resolved) {
    render_tween(m, it);
  }
  m->rendered = true;
  emit(m, BK_EV_MOTION_UPDATE);
  if (iter != prev_iter) {
    m->iteration = iter;
    emit(m, BK_EV_MOTION_REPEAT);
  }
  if (T >= total && (prev < total || first)) {
    emit(m, BK_EV_MOTION_COMPLETE);
  }
  if (T <= 0.0 && prev > 0.0) {
    emit(m, BK_EV_MOTION_REVERSE_COMPLETE);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Placing motions in timelines
 * \{ */

static BKMotion *root(void)
{
  if (!G.root) {
    G.root = motion_new(NULL);
    G.root->timeline = true;
    G.root->repeat = -1;
    G.root->dur = NEVER;
  }
  return G.root;
}

static void link_child(BKMotion *tl, BKMotion *c)
{
  if (tl == G.root) {
    if (G.nrun == G.cap_run) {
      G.cap_run = G.cap_run ? G.cap_run * 2 : 64;
      G.run = (BKMotion **)MEM_reallocN(G.run, sizeof(BKMotion *) * (size_t)G.cap_run);
    }
    c->parent = tl;
    c->run = G.nrun;
    G.run[G.nrun++] = c;
    return;
  }
  /* a timeline's children in order of start (its calls and labels follow it) */
  const double s = start_in_parent(c);
  BKMotion *after = tl->last;
  while (after && start_in_parent(after) > s) {
    after = after->prev;
  }
  c->parent = tl;
  c->prev = after;
  c->next = after ? after->next : tl->first;
  if (c->next) {
    c->next->prev = c;
  }
  else {
    tl->last = c;
  }
  if (after) {
    after->next = c;
  }
  else {
    tl->first = c;
  }
}

static void detach(BKMotion *m)
{
  BKMotion *p = m->parent;
  if (!p) {
    return;
  }
  if (p == G.root) { /* swap and pop */
    BKMotion *last = G.run[--G.nrun];
    G.run[m->run] = last;
    last->run = m->run;
    m->parent = NULL;
    BK_motion_release(m); /* the global timeline's hold */
    return;
  }
  if (m->prev) {
    m->prev->next = m->next;
  }
  else {
    p->first = m->next;
  }
  if (m->next) {
    m->next->prev = m->prev;
  }
  else {
    p->last = m->prev;
  }
  if (p->recent == m) {
    p->recent = NULL;
  }
  m->parent = m->prev = m->next = NULL;
  if (p->timeline && p != G.root) {
    timeline_measure(p);
  }
  BK_motion_release(m); /* the parent's hold */
}

/* a timeline's duration: where its last child ends */
static void timeline_measure(BKMotion *tl)
{
  if (tl == G.root) {
    return;
  }
  double end = 0.0;
  for (BKMotion *c = tl->first; c; c = c->next) {
    const double total = BK_motion_total_duration(c);
    end = BK_MAX(end, start_in_parent(c) + (total >= NEVER ? 0.0 : total / c->ts));
  }
  tl->dur = end;
}

/* place `m` in `tl` starting (local 0) at `at` */
static void place(BKMotion *tl, BKMotion *m, double at)
{
  BK_motion_retain(m); /* the parent's hold, taken before letting go of the old one */
  detach(m);
  m->anchor_parent = at;
  m->anchor_local = 0.0;
  if (m->reversed) { /* a reversed motion starts at its end */
    m->anchor_local = BK_motion_total_duration(m);
  }
  link_child(tl, m);
  tl->recent = m;
  if (tl != G.root) {
    timeline_measure(tl);
  }
  else {
    wake(m);
  }
}

static void start_now(BKMotion *m, double delay)
{
  place(root(), m, G.root_time + delay);
}

double BK_timeline_label_time(const BKMotion *tl, const char *name)
{
  for (int i = 0; tl && name && i < tl->nlabels; i++) {
    if (strcmp(tl->labels[i].name, name) == 0) {
      return tl->labels[i].time;
    }
  }
  return -1.0;
}

static double label_add(BKMotion *tl, const char *name, double at)
{
  for (int i = 0; i < tl->nlabels; i++) {
    if (strcmp(tl->labels[i].name, name) == 0) {
      tl->labels[i].time = at;
      return at;
    }
  }
  tl->labels = (Label *)MEM_reallocN(tl->labels, sizeof(Label) * (size_t)(tl->nlabels + 1));
  tl->labels[tl->nlabels++] = (Label){BLI_strdup(name), at};
  return at;
}

/* GSAP's position parameter */
static double position(BKMotion *tl, const char *pos)
{
  const double end = tl->dur;
  if (!pos || !*pos) {
    return end;
  }
  char *rest;
  const double num = strtod(pos, &rest);
  if (rest != pos && !*rest) {
    return BK_MAX(0.0, num); /* an absolute time */
  }
  const BKMotion *r = tl->recent;
  const double rs = r ? start_in_parent(r) : end;
  const double rtotal = r ? BK_motion_total_duration(r) : 0.0;
  const double re = r ? rs + (rtotal >= NEVER ? 0.0 : rtotal / r->ts) : end;
  double base = end;
  const char *off = pos;
  if (pos[0] == '<' || pos[0] == '>') {
    base = pos[0] == '<' ? rs : re;
    off = pos + 1;
  }
  else if ((pos[0] == '+' || pos[0] == '-') && pos[1] == '=') {
    off = pos;
  }
  else { /* a label, maybe with "+=1" / "-=1" after it */
    const char *op = strstr(pos, "+=");
    if (!op) {
      op = strstr(pos, "-=");
    }
    char name[64];
    const size_t len = op ? (size_t)(op - pos) : strlen(pos);
    if (len >= sizeof(name)) {
      return end;
    }
    memcpy(name, pos, len);
    name[len] = '\0';
    double lt = BK_timeline_label_time(tl, name);
    if (lt < 0.0) {
      lt = label_add(tl, name, end); /* GSAP: an unknown label is made at the end */
    }
    base = lt;
    off = op ? op : "";
  }
  if (!*off) {
    return BK_MAX(0.0, base);
  }
  double sign = 1.0;
  if ((off[0] == '+' || off[0] == '-') && off[1] == '=') {
    sign = off[0] == '-' ? -1.0 : 1.0;
    off += 2;
  }
  const double v = strtod(off, &rest);
  const double amount = *rest == '%' ? v / 100.0 * (r ? rtotal / r->ts : 0.0) : v;
  return BK_MAX(0.0, base + sign * amount);
}

static void ensure_placed(BKMotion *m);

double BK_timeline_add(BKMotion *tl, BKMotion *child, const char *pos)
{
  if (!tl || !child || !tl->timeline || tl == child) {
    return 0.0;
  }
  const double at = position(tl, pos);
  place(tl, child, at);
  /* GSAP: a timeline that had finished (or was empty) plays on with what is added to it */
  if (!tl->parent && tl != G.root && !tl->paused && !tl->killed) {
    ensure_placed(tl);
  }
  return at;
}

double BK_timeline_label(BKMotion *tl, const char *name, const char *pos)
{
  return tl && name ? label_add(tl, name, position(tl, pos)) : 0.0;
}

double BK_timeline_call(BKMotion *tl, BKMotionFn fn, void *data, void (*free_data)(void *data),
                        const char *pos)
{
  if (!tl || !fn) {
    if (free_data && data) {
      free_data(data);
    }
    return 0.0;
  }
  BKMotion *c = motion_new(NULL);
  c->call = true;
  c->fn = fn;
  c->fn_data = data;
  c->free_data = free_data;
  const double at = BK_timeline_add(tl, c, pos);
  BK_motion_release(c); /* the timeline holds it */
  return at;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Making motions
 * \{ */

BKMotionVars BK_motion_vars_default(void)
{
  BKMotionVars v;
  memset(&v, 0, sizeof(v));
  v.duration = 0.5;
  v.time_scale = 1.0;
  BK_ease_parse("power1.out", &v.ease);
  v.spring = BK_spring_default();
  v.inertia = BK_inertia_default(0.0f);
  v.overwrite = BK_OVERWRITE_AUTO;
  return v;
}

BKMotion *BK_tween(const BKAnimClass *cls, void *obj, const BKTrack *tracks, int ntracks,
                   const BKMotionVars *vars)
{
  const BKMotionVars dv = BK_motion_vars_default();
  const BKMotionVars *v = vars ? vars : &dv;
  ntracks = BK_MAX(0, ntracks);
  /* the tracks' sizes: keys (gsap.to: from where it is now; gsap.from: then to where it is) */
  size_t nfloat = 0, nease = 0, nbyte = 0;
  for (int i = 0; i < ntracks; i++) {
    const BKTrack *in = &tracks[i];
    const int ink = BK_MAX(1, in->nkeys), n = BK_CLAMP(in->n, 1, 4);
    const int nk = in->from ? ink + 1 : (in->nkeys <= 1 ? 2 : ink);
    nfloat += (size_t)(nk * n) + (in->times && in->nkeys == nk ? (size_t)nk : 0);
    nease += in->eases && in->nkeys == nk && nk > 1 ? (size_t)(nk - 1) : 0;
    nbyte += (size_t)nk * (in->rel ? 2 : 1);
  }
  /* one block: the motion, its tracks, their floats, eases and flags (aligned in that order) */
  const size_t off_tracks = (sizeof(BKMotion) + 7) & ~(size_t)7;
  const size_t off_float = off_tracks + sizeof(Track) * (size_t)ntracks;
  const size_t off_ease = off_float + sizeof(float) * nfloat;
  const size_t off_byte = off_ease + sizeof(BKEase) * nease;
  BKMotion *m = motion_alloc(v, off_byte + nbyte - sizeof(BKMotion));
  char *block = (char *)m;
  float *fp = (float *)(block + off_float);
  BKEase *ep = (BKEase *)(block + off_ease);
  uint8_t *bp = (uint8_t *)(block + off_byte);
  m->cls = cls;
  m->obj = obj;
  if (cls && cls->retain) {
    cls->retain(obj);
  }
  m->ease = v->ease;
  m->kind = v->kind;
  m->spring = v->spring;
  m->inertia = v->inertia;
  m->overwrite = v->overwrite;
  m->ntracks = ntracks;
  m->tracks = ntracks ? (Track *)(block + off_tracks) : NULL;
  for (int i = 0; i < ntracks; i++) {
    const BKTrack *in = &tracks[i];
    Track *t = &m->tracks[i];
    const int ink = BK_MAX(1, in->nkeys), n = BK_CLAMP(in->n, 1, 4);
    const bool to = !in->from && in->nkeys <= 1; /* gsap.to: from where it is now */
    const int nk = in->from ? ink + 1 : (to ? 2 : ink);
    const int at = to ? 1 : 0; /* where the given keys go */
    t->prop = in->prop;
    t->n = n;
    t->nkeys = nk;
    t->unit = in->unit;
    t->values = fp;
    fp += nk * n;
    memcpy(&t->values[at * n], in->values, sizeof(float) * (size_t)(ink * n));
    t->current = bp;
    bp += nk;
    if (in->current) {
      memcpy(&t->current[at], in->current, (size_t)ink);
    }
    if (to) {
      t->current[0] = 1;
    }
    if (in->from) {
      t->current[nk - 1] = 1; /* gsap.from: then where it is now */
    }
    if (in->rel) {
      t->rel = (int8_t *)bp;
      bp += nk;
      memcpy(&t->rel[at], in->rel, (size_t)ink);
    }
    if (in->times && in->nkeys == nk) {
      t->times = fp;
      fp += nk;
      memcpy(t->times, in->times, sizeof(float) * (size_t)nk);
    }
    if (in->eases && in->nkeys == nk && nk > 1) {
      t->eases = ep;
      ep += nk - 1;
      memcpy(t->eases, in->eases, sizeof(BKEase) * (size_t)(nk - 1));
    }
    t->instant = G.reduced && is_layout_prop(t->prop);
  }
  m->dur = v->kind == BK_MOTION_TWEEN_KIND ? BK_MAX(0.0, v->duration) : 0.0;
  if (v->kind != BK_MOTION_TWEEN_KIND) {
    resolve(m); /* physics: the duration comes from the distance */
  }
  start_now(m, v->delay); /* a negative delay starts it partway (Motion) */
  if (v->immediate || (m->dur == 0.0 && v->delay <= 0.0)) {
    render(m, local_at(m, G.root_time), true);
  }
  return m;
}

BKMotion *BK_timeline(const BKMotionVars *vars)
{
  BKMotion *m = motion_new(vars);
  m->timeline = true;
  m->dur = 0.0;
  start_now(m, vars ? BK_MAX(0.0, vars->delay) : 0.0);
  return m;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Controls
 * \{ */

/* a motion out of every timeline (it ended, or was killed) goes back in the global one */
static void ensure_placed(BKMotion *m)
{
  if (!m->parent) {
    place(root(), m, G.root_time - (m->reversed ? -1.0 : 1.0) * m->t / m->ts);
    m->anchor_local = m->t;
    m->anchor_parent = G.root_time;
  }
}

void BK_motion_play(BKMotion *m)
{
  if (!m) {
    return;
  }
  ensure_placed(m);
  reanchor(m);
  m->paused = false;
  m->reversed = false;
  wake(m);
}

void BK_motion_pause(BKMotion *m)
{
  if (m && !m->paused) {
    reanchor(m);
    m->paused = true;
  }
}

void BK_motion_resume(BKMotion *m)
{
  if (m && m->paused) {
    ensure_placed(m);
    m->anchor_parent = parent_time(m);
    m->paused = false;
    wake(m);
  }
}

void BK_motion_reverse(BKMotion *m)
{
  if (!m) {
    return;
  }
  ensure_placed(m);
  reanchor(m);
  m->reversed = !m->reversed;
  m->paused = false;
  wake(m);
}

void BK_motion_seek(BKMotion *m, double time)
{
  if (!m) {
    return;
  }
  ensure_placed(m);
  m->anchor_parent = parent_time(m);
  m->anchor_local = BK_CLAMP(time, 0.0, BK_motion_total_duration(m));
  render(m, m->anchor_local, true);
}

void BK_motion_restart(BKMotion *m)
{
  if (!m) {
    return;
  }
  m->started = false;
  m->reversed = false;
  m->paused = false;
  BK_motion_seek(m, 0.0);
  wake(m);
}

void BK_motion_set_progress(BKMotion *m, double progress)
{
  if (m) {
    const int iter = m->iteration;
    double it = BK_CLAMP(progress, 0.0, 1.0) * m->dur;
    if (m->yoyo && (iter & 1)) {
      it = m->dur - it;
    }
    BK_motion_seek(m, iter * (m->dur + m->rdelay) + it);
  }
}

void BK_motion_set_total_progress(BKMotion *m, double progress)
{
  if (m) {
    const double total = BK_motion_total_duration(m);
    BK_motion_seek(m, BK_CLAMP(progress, 0.0, 1.0) * (total >= NEVER ? m->dur : total));
  }
}

void BK_motion_set_time_scale(BKMotion *m, double scale)
{
  if (m && scale > 0.0) {
    reanchor(m);
    m->ts = scale;
    if (m->parent && m->parent->timeline) {
      timeline_measure(m->parent);
    }
  }
}

void BK_motion_complete(BKMotion *m)
{
  if (m) {
    const double total = BK_motion_total_duration(m);
    BK_motion_seek(m, total >= NEVER ? m->dur : total);
    m->paused = true;
  }
}

void BK_motion_kill(BKMotion *m)
{
  if (!m || m->killed) {
    return;
  }
  m->killed = true;
  for (int i = 0; i < m->ntracks; i++) {
    if (!m->tracks[i].killed) {
      driver_drop(m, i);
      m->tracks[i].killed = true;
    }
  }
  for (BKMotion *c = m->first; c; c = c->next) {
    BK_motion_kill(c);
  }
  detach(m);
}

int BK_motion_is_active(const BKMotion *m)
{
  if (!m || !m->parent || m->paused || m->killed) {
    return 0;
  }
  const double t = local_at(m, parent_time(m));
  return t >= 0.0 && t < BK_motion_total_duration(m) ? 1 : 0;
}

int BK_motion_paused(const BKMotion *m)
{
  return m && m->paused;
}

int BK_motion_reversed(const BKMotion *m)
{
  return m && m->reversed;
}

double BK_motion_total_time(const BKMotion *m)
{
  return m ? m->t : 0.0;
}

double BK_motion_time(const BKMotion *m)
{
  if (!m) {
    return 0.0;
  }
  int iter;
  return iteration_time(m, m->t, &iter);
}

double BK_motion_progress(const BKMotion *m)
{
  return m && m->dur > 0.0 ? BK_motion_time(m) / m->dur : 1.0;
}

double BK_motion_time_scale(const BKMotion *m)
{
  return m ? m->ts : 1.0;
}

float BK_motion_velocity(const BKMotion *m, int prop)
{
  if (!m || m->timeline) {
    return 0.0f;
  }
  for (int i = 0; i < m->ntracks; i++) {
    const Track *t = &m->tracks[i];
    if (t->prop != prop || t->killed || !m->resolved) {
      continue;
    }
    int iter;
    const double it = iteration_time(m, m->t, &iter);
    if (m->kind != BK_MOTION_TWEEN_KIND && t->n == 1) {
      float pos, vel;
      bk_physics_eval(&t->ph, it, &pos, &vel);
      return vel;
    }
    /* a keyframed value: its slope at this time */
    const double h = 1.0 / 240.0;
    const double a = BK_MAX(0.0, it - h), b = BK_MIN(m->dur, it + h);
    if (b <= a || m->dur <= 0.0) {
      return 0.0f;
    }
    float va[4], vb[4];
    track_eval(m, t, a, a / m->dur, va);
    track_eval(m, t, b, b / m->dur, vb);
    return (float)((vb[0] - va[0]) / (b - a)) * (float)m->ts;
  }
  return 0.0f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The clock
 * \{ */

int BK_anim_tick(double now)
{
  if (G.ticked && now == G.last_now) {
    return BK_anim_active();
  }
  double dt = G.ticked ? now - G.last_now : 0.0;
  if (dt > G.lag_threshold) {
    dt = G.lag_adjusted; /* GSAP's lag smoothing: a stall isn't a jump */
  }
  G.last_now = now;
  G.ticked = true;
  G.root_time += BK_MAX(0.0, dt) * G.time_scale;
  BKMotion *r = G.root;
  if (!r) {
    return 0; /* nothing was ever started: nothing to keep */
  }
  r->t = r->it_time = G.root_time;
  for (int i = 0; i < G.nrun;) {
    BKMotion *c = G.run[i];
    BK_motion_retain(c);
    render(c, local_at(c, G.root_time), false);
    /* GSAP's global timeline lets go of what has finished playing */
    const double t = local_at(c, G.root_time);
    const bool over = !c->paused && (c->reversed ? t <= 0.0 : t >= BK_motion_total_duration(c));
    if (over && c->parent == r) {
      detach(c); /* the last one takes its place: look at index i again */
    }
    if (i < G.nrun && G.run[i] == c) {
      i++;
    }
    BK_motion_release(c);
  }
  if (!G.nrun && G.root == r) { /* all done: the global timeline goes until the next start */
    G.root = NULL;
    BK_motion_release(r);
    return 0;
  }
  return BK_anim_active();
}

int BK_anim_active(void)
{
  if (!G.root) {
    return 0;
  }
  for (int i = 0; i < G.nrun; i++) {
    if (!G.run[i]->paused) {
      return 1;
    }
  }
  return 0;
}

void BK_anim_set_host_clock(int on)
{
  G.host_clock = on != 0;
}

int BK_anim_host_clock(void)
{
  return G.host_clock;
}

void BK_anim_set_reduced_motion(int on)
{
  G.reduced = on != 0;
}

int BK_anim_reduced_motion(void)
{
  return G.reduced;
}

void BK_anim_set_time_scale(double scale)
{
  if (scale > 0.0) {
    G.time_scale = scale;
  }
}

void BK_anim_set_lag_smoothing(double threshold, double adjusted)
{
  G.lag_threshold = threshold > 0.0 ? threshold : 1e30; /* 0: off */
  G.lag_adjusted = BK_MAX(0.0, adjusted);
}

/* stop tween `c`'s tracks of `obj` (`prop` < 0: all); a tween left with none stops */
static void kill_tween(BKMotion *c, void *obj, int prop)
{
  if (c->obj != obj) {
    return;
  }
  bool any = false;
  for (int i = 0; i < c->ntracks; i++) {
    Track *t = &c->tracks[i];
    if (!t->killed && (prop < 0 || t->prop == prop)) {
      driver_drop(c, i);
      t->killed = true;
    }
    any |= !t->killed;
  }
  if (!any) {
    BK_motion_kill(c);
  }
}

static void kill_target(BKMotion *tl, void *obj, int prop)
{
  for (BKMotion *c = tl->first, *next; c; c = next) {
    next = c->next;
    if (c->timeline) {
      kill_target(c, obj, prop);
    }
    else {
      kill_tween(c, obj, prop);
    }
  }
}

void BK_anim_kill_target(void *obj, int prop)
{
  if (!G.root || !obj) {
    return;
  }
  /* one property: the motion driving it, from the drivers hash (no walk over every motion) */
  Driver *d = prop >= 0 ? driver_find(obj, prop) : NULL;
  if (d && d->m) {
    BKMotion *m = d->m;
    const int ti = d->track;
    driver_drop(m, ti);
    m->tracks[ti].killed = true;
    bool any = false;
    for (int i = 0; i < m->ntracks; i++) {
      any |= !m->tracks[i].killed;
    }
    if (!any) {
      BK_motion_kill(m);
    }
    return;
  }
  /* every property, or motions made without overwrite: a walk over every motion */
  for (int i = 0; i < G.nrun;) {
    BKMotion *c = G.run[i];
    BK_motion_retain(c);
    if (c->timeline) {
      kill_target(c, obj, prop);
    }
    else {
      kill_tween(c, obj, prop);
    }
    if (i < G.nrun && G.run[i] == c) {
      i++;
    }
    BK_motion_release(c);
  }
}

void bk_motion_exit(void)
{
  if (G.root) {
    while (G.nrun) {
      BK_motion_kill(G.run[G.nrun - 1]);
    }
    BK_motion_release(G.root);
    G.root = NULL;
  }
  MEM_SAFE_FREE(G.run);
  G.cap_run = 0;
  MEM_SAFE_FREE(G.drivers);
  G.ndrivers = G.cap_drivers = 0;
  G.root_time = G.last_now = 0.0;
  G.ticked = false;
}

void BK_anim_exit(void)
{
  bk_motion_exit();
  bk_ease_exit();
  bk_value_exit();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stagger (GSAP's distribute)
 * \{ */

void BK_stagger(const BKStagger *s, int count, double *r_offsets)
{
  if (!s || count <= 0 || !r_offsets) {
    return;
  }
  const bool grid = s->grid_rows > 0 && s->grid_cols > 0;
  const int cols = grid ? s->grid_cols : count, rows = grid ? s->grid_rows : 1;
  const char *from = s->from ? s->from : "start";
  /* the origin, in grid units (x along a row, y down the rows) */
  double ox = 0.0, oy = 0.0;
  bool edges = false, random = false;
  if (strcmp(from, "center") == 0) {
    ox = (cols - 1) * 0.5;
    oy = (rows - 1) * 0.5;
  }
  else if (strcmp(from, "end") == 0) {
    ox = cols - 1;
    oy = rows - 1;
    if (!grid) {
      ox = count - 1;
    }
  }
  else if (strcmp(from, "edges") == 0) {
    edges = true;
  }
  else if (strcmp(from, "random") == 0) {
    random = true;
  }
  else if (strcmp(from, "index") == 0 || s->from_index > 0) {
    ox = grid ? s->from_index % cols : s->from_index;
    oy = grid ? s->from_index / cols : 0;
  }
  const bool ax = s->axis && s->axis[0] == 'x', ay = s->axis && s->axis[0] == 'y';
  double dmax = 0.0;
  for (int i = 0; i < count; i++) {
    const double x = grid ? i % cols : i, y = grid ? i / cols : 0.0;
    double d;
    if (random) {
      uint32_t h = (uint32_t)i * 2654435761u ^ 0x5bd1e995u;
      h ^= h >> 15;
      d = (double)(h & 0xffff) / 65535.0 * (count - 1);
    }
    else if (edges) {
      const double ex = BK_MIN(x, (grid ? cols : count) - 1 - x), ey = grid ? BK_MIN(y, rows - 1 - y) : 0.0;
      d = ax ? ex : ay ? ey : BK_MIN(ex, grid ? ey : ex);
    }
    else {
      const double dx = x - ox, dy = y - oy;
      d = ax ? fabs(dx) : ay ? fabs(dy) : sqrt(dx * dx + dy * dy);
    }
    r_offsets[i] = d;
    dmax = BK_MAX(dmax, d);
  }
  const bool eased = s->ease.kind != BK_EASE_LINEAR;
  for (int i = 0; i < count; i++) {
    double f = dmax > 0.0 ? r_offsets[i] / dmax : 0.0;
    if (eased) {
      f = BK_ease_eval(&s->ease, (float)f);
    }
    r_offsets[i] = s->amount > 0.0 ? f * s->amount : f * dmax * s->each;
  }
}

/** \} */
