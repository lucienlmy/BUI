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

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "anim_intern.h"

#define NEVER 1e300 /* the total duration of a motion repeating forever */

/* -------------------------------------------------------------------- */
/** \name Types
 * \{ */

/* What a frame reads first, then what it reads rarely: a tween's track is about one cache line
 * (physics state, only for springs and inertia, is in the motion's block after the tracks). */
typedef struct Track {
  int prop, n, nkeys, unit;
  bool instant : 1;  /* reduced motion: it jumps to its end */
  bool killed : 1;   /* another motion took the property */
  bool has_last : 1;
  float last[4];
  float *values;   /* nkeys * n: resolved when the tween starts (in `kv` when they fit) */
  float kv[8];     /* small keyframes (from -> to of a color, 3 keys of 2 numbers ...): in the
                    * track's own cache lines, not elsewhere in the block */
  BKEase *eases;   /* nkeys - 1 per segment, or NULL: the tween's ease */
  float *times;    /* nkeys, 0 .. 1 */
  uint8_t *current; /* per key: the value at the start */
  int8_t *rel;     /* per key: relative to the value at the start */
  BKPhysics *ph;   /* physics tracks: on the value (n == 1) or on 0 .. 1 (colors) */
} Track;

typedef struct Label {
  char *name;
  double time;
} Label;

/* The fields a frame reads come first (two cache lines), then the rest. */
struct BKMotion {
  /* -- every frame */
  bool timeline : 1, call : 1, paused : 1, reversed : 1, yoyo : 1, rendered : 1, started : 1, killed : 1,
      resolved : 1, olinked : 1,
      open : 1,       /* a timeline with a child that repeats for ever: it plays until killed */
      rendering : 1;  /* a timeline rendering its children (a callback can't render it again) */
  int refs;
  unsigned stamp;     /* the pass of the timeline render that last rendered it (see render_timeline) */
  unsigned edits;     /* a timeline: counts its children's coming and going */
  int run;      /* in the global timeline: its index in G.run */
  int repeat;
  int kind;
  int ntracks;
  int iteration;
  int events;
  /* time: local = anchor_local + dir * (parent_time - anchor_parent) * ts */
  double anchor_parent, anchor_local;
  double ts;
  double dur;      /* one iteration */
  double rdelay;
  double inv_dur, inv_cycle; /* 1 / dur, 1 / (dur + rdelay) (0 for 0): no division per frame */
  double t;        /* the total time last rendered */
  double it_time;  /* the time in its iteration then: its children's parent time */
  BKMotion *parent;
  Track *tracks; /* in the motion's own block, with their keyframes */
  const BKAnimClass *cls;
  void *obj;
  BKMotionFn on;
  void *data;
  BKEase ease;
  /* -- rarely */
  BKMotion *oprev, *onext; /* the target's other motions (see ObjEntry) */
  int overwrite;
  BKMotion *prev, *next; /* in a timeline's children, in order of start */
  void (*free_data)(void *data);
  BKSpring spring;
  BKInertia inertia;
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
  double (*clock)(void); /* the kit's clock (BK_anim_set_clock), or NULL */
  struct ObjEntry *objs; /* tweens by target; open addressing like the drivers */
  int nobjs, cap_objs;
  unsigned pass;  /* timeline renders so far (they stamp the children they rendered) */
  bool in_tick;   /* BK_anim_tick is running: a callback's motion doesn't tick the clock again */
} G ={NULL, 0, 0, 0, false, 1.0, 0.5, 0.033, false, false, NULL, 0, 0, NULL, NULL, NULL, 0, 0, NULL, NULL, 0, 0};

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

/* Every tween by its target: a target going away stops all of its motions (started or not,
 * overwriting or not) by walking its own, never every motion. */
typedef struct ObjEntry {
  void *obj; /* NULL: empty, TOMB: deleted */
  BKMotion *head;
} ObjEntry;

static ObjEntry *obj_find(void *obj)
{
  if (!G.cap_objs) {
    return NULL;
  }
  for (uint32_t i = driver_hash(obj, 0) & (G.cap_objs - 1), k = 0; k < (uint32_t)G.cap_objs;
       i = (i + 1) & (G.cap_objs - 1), k++)
  {
    ObjEntry *e = &G.objs[i];
    if (!e->obj) {
      return NULL;
    }
    if (e->obj == obj) {
      return e;
    }
  }
  return NULL;
}

static ObjEntry *obj_entry(void *obj);

static void objs_grow(void)
{
  ObjEntry *old = G.objs;
  const int old_cap = G.cap_objs;
  int live = 0;
  for (int i = 0; i < old_cap; i++) {
    live += old[i].obj && old[i].obj != TOMB;
  }
  /* mostly tombstones: the same size again drops them; else double */
  G.cap_objs = !old_cap ? 64 : ((live + 1) * 4 <= old_cap ? old_cap : old_cap * 2);
  G.objs = (ObjEntry *)MEM_callocN(sizeof(ObjEntry) * (size_t)G.cap_objs, "anim objects");
  G.nobjs = 0;
  for (int i = 0; i < old_cap; i++) {
    if (old[i].obj && old[i].obj != TOMB) {
      obj_entry(old[i].obj)->head = old[i].head;
    }
  }
  MEM_SAFE_FREE(old);
}

/* the entry for `obj`, made when there is none */
static ObjEntry *obj_entry(void *obj)
{
  ObjEntry *e = obj_find(obj);
  if (e) {
    return e;
  }
  if ((G.nobjs + 1) * 2 > G.cap_objs) {
    objs_grow();
  }
  for (uint32_t i = driver_hash(obj, 0) & (G.cap_objs - 1);; i = (i + 1) & (G.cap_objs - 1)) {
    if (!G.objs[i].obj || G.objs[i].obj == TOMB) {
      G.nobjs += G.objs[i].obj == NULL; /* a reused tombstone is a slot counted already */
      G.objs[i] = (ObjEntry){obj, NULL};
      return &G.objs[i];
    }
  }
}

static void obj_link(BKMotion *m)
{
  ObjEntry *e = obj_entry(m->obj);
  m->oprev = NULL;
  m->onext = e->head;
  if (e->head) {
    e->head->oprev = m;
  }
  e->head = m;
  m->olinked = true;
}

static void obj_unlink(BKMotion *m)
{
  if (!m->olinked) {
    return;
  }
  if (m->oprev) {
    m->oprev->onext = m->onext;
  }
  else {
    ObjEntry *e = obj_find(m->obj);
    if (e) {
      e->head = m->onext;
      if (!m->onext) {
        e->obj = TOMB;
      }
    }
  }
  if (m->onext) {
    m->onext->oprev = m->oprev;
  }
  m->oprev = m->onext = NULL;
  m->olinked = false;
}

static void driver_set(void *obj, int prop, BKMotion *m, int track);

static void drivers_grow(void)
{
  Driver *old = G.drivers;
  const int old_cap = G.cap_drivers;
  int live = 0;
  for (int i = 0; i < old_cap; i++) {
    live += old[i].obj && old[i].obj != TOMB;
  }
  G.cap_drivers = !old_cap ? 64 : ((live + 1) * 4 <= old_cap ? old_cap : old_cap * 2);
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
      G.ndrivers += G.drivers[i].obj == NULL;
      G.drivers[i] = (Driver){obj, prop, m, track};
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

/* every change of a motion's duration goes through here (the inverses follow it) */
static void set_dur(BKMotion *m, double dur)
{
  m->dur = dur;
  m->inv_dur = dur > 0.0 ? 1.0 / dur : 0.0;
  m->inv_cycle = dur + m->rdelay > 0.0 ? 1.0 / (dur + m->rdelay) : 0.0;
}

/* What comes in from outside (BUI_anim.h, BK_motion_vars_check): a time is 0 .. MAX_TIME, a
 * delay -MAX_TIME .. MAX_TIME, and NaN is 0 (the bindings reject it before it gets here). */
static double sane_time(double t)
{
  return t > 0.0 ? BK_MIN(t, BK_MOTION_MAX_TIME) : 0.0;
}

static double sane_delay(double d)
{
  return d == d ? BK_CLAMP(d, -BK_MOTION_MAX_TIME, BK_MOTION_MAX_TIME) : 0.0;
}

int BK_motion_clamp_repeat(double repeat)
{
  if (repeat != repeat) {
    return 0;
  }
  return repeat < 0.0 || repeat == (double)INFINITY ? -1 : (int)BK_MIN(repeat, (double)BK_MOTION_MAX_REPEAT);
}

int BK_motion_vars_check(BKMotionVars *v)
{
  const bool ok = isfinite(v->duration) && isfinite(v->delay) && isfinite(v->repeat_delay) &&
                  isfinite(v->time_scale);
  v->duration = sane_time(v->duration);
  v->delay = sane_delay(v->delay);
  v->repeat_delay = sane_time(v->repeat_delay);
  v->repeat = BK_motion_clamp_repeat(v->repeat);
  v->time_scale = v->time_scale > 0.0 && v->time_scale < 1e300 ? v->time_scale : 1.0;
  return ok;
}

void BK_motion_vars_motion_defaults(int nkeys, int custom, double *r_duration, BKEase *r_ease)
{
  *r_duration = nkeys > 1 ? 0.8 : 0.3;
  if (!custom && nkeys <= 1) {
    *r_ease = BK_ease_bezier(0.25f, 0.1f, 0.35f, 1.0f);
  }
  else {
    BK_ease_parse("easeInOut", r_ease);
  }
}

/* a motion with `extra` zeroed bytes after it (a tween's tracks and keyframes: one block) */
static BKMotion *motion_alloc(const BKMotionVars *v, size_t extra)
{
  BKMotion *m = (BKMotion *)MEM_callocN(sizeof(BKMotion) + extra, "BKMotion");
  m->refs = 1;
  m->ts = v && v->time_scale > 0.0 && v->time_scale < 1e300 ? v->time_scale : 1.0;
  m->repeat = v ? BK_motion_clamp_repeat(v->repeat) : 0;
  m->rdelay = v ? sane_time(v->repeat_delay) : 0.0;
  set_dur(m, v ? sane_time(v->duration) : 0.0);
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
  obj_unlink(m);
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
  if (m->open) {
    return NEVER;
  }
  if (m->repeat < 0) { /* for ever, unless an iteration takes no time: that is one evaluation */
    return m->dur + m->rdelay > 0.0 ? NEVER : 0.0;
  }
  /* doubles: durations and repeats are capped (BK_MOTION_MAX_*), their product can't overflow */
  return m->dur * ((double)m->repeat + 1.0) + m->rdelay * (double)m->repeat;
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
  /* the global timeline's time is the clock's (its it_time is only set when it ticks) */
  return m->parent && m->parent != G.root ? m->parent->it_time : G.root_time;
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
      bk_physics_init(t->ph, m->kind, &s, &in, from, to);
      longest = BK_MAX(longest, t->ph->duration);
    }
  }
  if (m->kind != BK_MOTION_TWEEN_KIND) {
    set_dur(m, longest); /* a physics tween lasts until its slowest value settles */
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
    bk_physics_eval(t->ph, it, &pos, &vel);
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
  if (t->nkeys == 2 && !t->times && !t->eases) { /* from -> to, the usual: one ease, one lerp */
    const float u = BK_ease_eval(&m->ease, (float)p);
    const float *a = t->values, *b = &t->values[t->n];
    for (int c = 0; c < t->n; c++) {
      out[c] = a[c] + (b[c] - a[c]) * u;
    }
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
  float u;
  if (t->times) {
    const float t0 = t->times[seg], t1 = t->times[seg + 1];
    u = t1 > t0 ? (x - t0) / (t1 - t0) : 1.0f;
  }
  else {
    /* evenly spaced: the segment and where in it, without a division (a cast, not floorf:
     * negatives clamp to 0 either way) */
    const float fk = x * (float)(t->nkeys - 1);
    seg = BK_CLAMP((int)fk, 0, t->nkeys - 2);
    u = fk - (float)seg;
  }
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
  if (m->open) { /* nothing ends it: the time is the time */
    *r_iter = 0;
    return T;
  }
  const double cycle = m->dur + m->rdelay;
  /* T >= 0: the cast is floor, without a call; times the inverse: no division. Clamped before
   * the cast (a tiny cycle makes a huge count), and NaN is the first iteration. */
  int iter = 0;
  if (cycle > 0.0) {
    const double n = T * m->inv_cycle, last = m->repeat >= 0 ? (double)m->repeat : (double)INT_MAX;
    iter = n < last ? (int)n : (n >= last ? (int)last : 0);
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
  const double p = m->dur > 0.0 ? it * m->inv_dur : 1.0;
  for (int i = 0; i < m->ntracks; i++) {
    Track *t = &m->tracks[i];
    if (t->killed) {
      continue;
    }
    float v[4];
    track_eval(m, t, it, p, v);
    bool same = t->has_last; /* 1 .. 4 numbers: a loop, not a memcmp call */
    for (int c = 0; c < t->n && same; c++) {
      same = v[c] == t->last[c];
    }
    if (same) {
      continue; /* unchanged: no write, no redraw */
    }
    for (int c = 0; c < t->n; c++) {
      t->last[c] = v[c];
    }
    t->has_last = true;
    if (m->cls && m->cls->set) {
      m->cls->set(m->obj, t->prop, v, t->n, t->unit);
    }
  }
}

/* one child of a timeline at the timeline's time `it` (it held while its callbacks run) */
static void render_child(const BKMotion *tl, BKMotion *c, double it, double prev_it)
{
  BK_motion_retain(c);
  if (c->call) { /* a callback fires when the playhead crosses it, either way */
    const double at = start_in_parent(c);
    if ((prev_it < at && it >= at) || (prev_it > at && it <= at) || (!tl->rendered && it >= at && at == 0.0)) {
      c->fn(c, BK_EV_MOTION_COMPLETE, c->fn_data);
    }
  }
  else {
    render(c, local_at(c, it), false);
  }
  BK_motion_release(c);
}

/* The children in order. A callback may kill, add or move children (even the timeline): the
 * timeline counts such edits, and when it did, the walk goes on with the first child not yet
 * rendered in this pass (the pass number is stamped on each) instead of the `next` it took
 * before the callback, which may be gone. The timeline can't be rendered again from one of
 * its own callbacks (the walk in progress would run on stale times); a seek there takes
 * effect on the next frame. */
static void render_timeline(BKMotion *tl, double it, double prev_it)
{
  if (tl->rendering) {
    return;
  }
  tl->rendering = true;
  BK_motion_retain(tl); /* a callback can kill it */
  const unsigned pass = ++G.pass;
  for (BKMotion *c = tl->first; c;) {
    BKMotion *next = c->next;
    const unsigned edits = tl->edits;
    c->stamp = pass;
    render_child(tl, c, it, prev_it);
    if (tl->edits != edits) {
      next = tl->first;
      while (next && next->stamp == pass) {
        next = next->next;
      }
    }
    c = next;
  }
  tl->rendering = false;
  BK_motion_release(tl);
}

/* render `m` at total time `T` (clamped); `force` renders even if the time didn't change */
static void render(BKMotion *m, double T, bool force)
{
  const double total = BK_motion_total_duration(m);
  T = BK_CLAMP(T, 0.0, total);
  if (!force && m->rendered && T == m->t) {
    return;
  }
  /* a running tween nobody listens to (every element's motion): the time, the values, done;
   * what follows is first frames, timelines and events */
  if (!force && m->rendered && m->resolved && m->started && !m->timeline && !m->on) {
    int iter;
    const double it = iteration_time(m, T, &iter);
    m->t = T;
    m->it_time = it;
    m->iteration = iter;
    render_tween(m, it);
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
    G.root->t = G.root->it_time = G.root_time;
    set_dur(G.root, NEVER);
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
  tl->edits++;
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
  p->edits++;
  m->parent = m->prev = m->next = NULL;
  if (!p->killed) { /* a timeline being killed measures once, after its children */
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
  bool open = false;
  for (BKMotion *c = tl->first; c; c = c->next) {
    const double total = BK_motion_total_duration(c);
    /* a child that repeats for ever counts as one of its iterations, and the timeline plays on */
    open |= total >= NEVER;
    end = BK_MAX(end, start_in_parent(c) + (total >= NEVER ? c->dur : total) / c->ts);
  }
  tl->open = open;
  set_dur(tl, end);
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
  /* nothing ran: frames didn't tick the clock, so it's behind; a motion starts now (not from a
   * callback of a tick: that one is the clock) */
  if (G.clock && !G.host_clock && !G.in_tick && !BK_anim_active()) {
    BK_anim_tick(G.clock());
  }
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

/* A plain decimal number at `s` (a sign, digits, a point, an exponent): strtod alone also
 * takes "nan", "inf" and hex. `r_end` is after it. */
static bool scan_number(const char *s, double *r_value, const char **r_end)
{
  const char *p = s + (*s == '+' || *s == '-');
  size_t digits = 0;
  for (; isdigit((unsigned char)*p); p++, digits++) {
  }
  if (*p == '.') {
    for (p++; isdigit((unsigned char)*p); p++, digits++) {
    }
  }
  if (!digits) {
    return false;
  }
  if (*p == 'e' || *p == 'E') {
    const char *q = p + 1 + (p[1] == '+' || p[1] == '-');
    if (isdigit((unsigned char)*q)) {
      for (; isdigit((unsigned char)*q); q++) {
      }
      p = q;
    }
  }
  char buf[64];
  const size_t len = (size_t)(p - s);
  if (len >= sizeof(buf)) {
    return false;
  }
  memcpy(buf, s, len);
  buf[len] = '\0';
  *r_value = strtod(buf, NULL);
  *r_end = p;
  return isfinite(*r_value);
}

/* The offset that ends a position, all of `s`: "+=n" / "-=n" (`bare`: or "n", "+n", "-n"),
 * with a "%" after n for a share of the last added's duration. */
static bool read_offset(const char *s, bool bare, double *r_amount, bool *r_percent)
{
  double sign = 1.0, n;
  const char *end;
  if ((s[0] == '+' || s[0] == '-') && s[1] == '=') {
    sign = s[0] == '-' ? -1.0 : 1.0;
    s += 2;
  }
  else if (!bare) {
    return false;
  }
  if (!scan_number(s, &n, &end)) {
    return false;
  }
  *r_percent = *end == '%';
  end += *r_percent;
  *r_amount = sign * n;
  return !*end;
}

/* GSAP's position parameter, all of it and nothing else: a number, "+=n", "-=n", "<", ">",
 * "<n", ">n" (n may be signed), "label", "label+=n", "label-=n", "n%" (of the timeline), and
 * a "%" after any n (of the last added). "" is the end. -1 when it isn't one of them. As in GSAP,
 * a label that doesn't exist yet is made at the end of the timeline, but only when the whole
 * position is valid: a refused position makes nothing. */
static bool label_name_ok(const char *s)
{
  /* the grammar of a new label: bindings/node/validate.js has the same one */
  if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_')) {
    return false;
  }
  for (; *s; s++) {
    const bool word = (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
                      *s == '_' || *s == '.' || *s == '-';
    if (!word) {
      return false;
    }
  }
  return true;
}

static double position(BKMotion *tl, const char *pos)
{
  const double end = tl->dur;
  if (!pos || !*pos) {
    return end;
  }
  double n;
  const char *e;
  if (scan_number(pos, &n, &e) && (!*e || (*e == '%' && !e[1]))) {
    return sane_time(*e ? n / 100.0 * end : n); /* an absolute time, or a share of the timeline */
  }
  const BKMotion *r = tl->recent;
  const double rs = r ? start_in_parent(r) : end;
  const double rtotal = r ? BK_motion_total_duration(r) : 0.0;
  const double rdur = r ? (rtotal >= NEVER ? r->dur : rtotal) / r->ts : 0.0;
  double base = end;
  const char *off = pos;
  char name[64];
  bool fresh = false; /* a label to make at the end */
  if (pos[0] == '<' || pos[0] == '>') {
    base = pos[0] == '<' ? rs : rs + (r ? rdur : 0.0);
    off = pos + 1;
  }
  else if ((pos[0] == '+' || pos[0] == '-') && pos[1] == '=') {
    off = pos;
  }
  else { /* a label, maybe with "+=1" / "-=1" after it */
    const char *op = pos;
    while (*op && !((*op == '+' || *op == '-') && op[1] == '=')) {
      op++;
    }
    const size_t len = (size_t)(op - pos);
    if (len == 0 || len >= sizeof(name)) {
      return -1.0;
    }
    memcpy(name, pos, len);
    name[len] = '\0';
    base = BK_timeline_label_time(tl, name);
    if (base < 0.0) {
      if (!label_name_ok(name)) {
        return -1.0;
      }
      base = end; /* GSAP makes it at the end */
      fresh = true;
    }
    off = op;
  }
  double at = base;
  if (*off) {
    double amount;
    bool percent;
    if (!read_offset(off, pos[0] == '<' || pos[0] == '>', &amount, &percent)) {
      return -1.0;
    }
    at = base + (percent ? amount / 100.0 * rdur : amount);
  }
  if (fresh) {
    label_add(tl, name, end);
  }
  return sane_time(at);
}

static void ensure_placed(BKMotion *m);

double BK_timeline_add(BKMotion *tl, BKMotion *child, const char *pos)
{
  if (!tl || !child || !tl->timeline || tl->killed || child->killed) { /* killed stays killed */
    return 0.0;
  }
  /* a timeline can't be its own ancestor: it would recurse for ever */
  for (const BKMotion *a = tl; a; a = a->parent) {
    if (a == child) {
      return 0.0;
    }
  }
  const double at = position(tl, pos);
  if (at < 0.0) {
    return -1.0;
  }
  place(tl, child, at);
  /* GSAP: a timeline that had finished (or was empty) plays on with what is added to it */
  if (!tl->parent && tl != G.root && !tl->paused && !tl->killed) {
    ensure_placed(tl);
  }
  return at;
}

double BK_timeline_label(BKMotion *tl, const char *name, const char *pos)
{
  if (!tl || !name || !tl->timeline) {
    return 0.0;
  }
  const double at = position(tl, pos);
  return at < 0.0 ? -1.0 : label_add(tl, name, at);
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

/* a track the engine can read: values for every key, finite */
static bool track_valid(const BKTrack *in)
{
  if (!in->values || in->nkeys < 1 || in->nkeys > 65536 || in->n < 1 || in->n > 4) {
    return false;
  }
  for (int i = 0; i < in->nkeys * in->n; i++) {
    if (!isfinite(in->values[i])) {
      return false;
    }
  }
  return true;
}

/* a spring or inertia from outside: NaN and infinity are the defaults */
static void physics_sane(BKMotionVars *v)
{
  const BKSpring ds = BK_spring_default();
  BKSpring *s = &v->spring;
  s->stiffness = finite_or(s->stiffness, ds.stiffness);
  s->damping = finite_or(s->damping, ds.damping);
  s->mass = finite_or(s->mass, ds.mass);
  s->velocity = finite_or(s->velocity, 0.0f);
  s->rest_delta = finite_or(s->rest_delta, 0.0f);
  s->rest_speed = finite_or(s->rest_speed, 0.0f);
  const BKInertia di = BK_inertia_default(0.0f);
  BKInertia *in = &v->inertia;
  in->velocity = finite_or(in->velocity, 0.0f);
  in->power = finite_or(in->power, di.power);
  in->time_constant = finite_or(in->time_constant, di.time_constant);
  in->min = finite_or(in->min, di.min);
  in->max = finite_or(in->max, di.max);
  in->bounce_stiffness = finite_or(in->bounce_stiffness, di.bounce_stiffness);
  in->bounce_damping = finite_or(in->bounce_damping, di.bounce_damping);
  in->rest_delta = finite_or(in->rest_delta, di.rest_delta);
}

BKMotion *BK_tween(const BKAnimClass *cls, void *obj, const BKTrack *tracks, int ntracks,
                   const BKMotionVars *vars)
{
  BKMotionVars sane = vars ? *vars : BK_motion_vars_default();
  const BKMotionVars *v = &sane;
  physics_sane(&sane);
  ntracks = BK_MAX(0, ntracks);
  bool valid = ntracks == 0 || tracks;
  for (int i = 0; valid && i < ntracks; i++) {
    valid = track_valid(&tracks[i]);
  }
  if (!valid) { /* the boundary: nothing past here reads a track that isn't there */
    if (v->free_data && v->data) {
      v->free_data(v->data);
    }
    return NULL;
  }
  /* the tracks' sizes: keys (gsap.to: from where it is now; gsap.from: then to where it is) */
  size_t nfloat = 0, nease = 0, nbyte = 0;
  for (int i = 0; i < ntracks; i++) {
    const BKTrack *in = &tracks[i];
    const int ink = BK_MAX(1, in->nkeys), n = BK_CLAMP(in->n, 1, 4);
    const int nk = in->from ? ink + 1 : (in->nkeys <= 1 ? 2 : ink);
    nfloat += (nk * n <= 8 ? 0 : (size_t)(nk * n)) + (in->times && in->nkeys == nk ? (size_t)nk : 0);
    nease += in->eases && in->nkeys == nk && nk > 1 ? (size_t)(nk - 1) : 0;
    nbyte += (size_t)nk * (in->rel ? 2 : 1);
  }
  /* one block: the motion, its tracks, physics (springs, inertia), floats, eases and flags
   * (aligned in that order) */
  const bool physics = v->kind != BK_MOTION_TWEEN_KIND;
  const size_t off_tracks = (sizeof(BKMotion) + 7) & ~(size_t)7;
  const size_t off_phys = off_tracks + sizeof(Track) * (size_t)ntracks;
  const size_t off_float = off_phys + (physics ? sizeof(BKPhysics) * (size_t)ntracks : 0);
  const size_t off_ease = off_float + sizeof(float) * nfloat;
  const size_t off_byte = off_ease + sizeof(BKEase) * nease;
  BKMotion *m = motion_alloc(v, off_byte + nbyte - sizeof(BKMotion));
  char *block = (char *)m;
  float *fp = (float *)(block + off_float);
  BKEase *ep = (BKEase *)(block + off_ease);
  uint8_t *bp = (uint8_t *)(block + off_byte);
  m->cls = cls;
  m->obj = obj;
  if (obj) {
    obj_link(m);
  }
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
    if (nk * n <= 8) {
      t->values = t->kv;
    }
    else {
      t->values = fp;
      fp += nk * n;
    }
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
    if (physics) {
      t->ph = (BKPhysics *)(block + off_phys) + i;
    }
  }
  set_dur(m, v->kind == BK_MOTION_TWEEN_KIND ? sane_time(v->duration) : 0.0);
  if (v->kind != BK_MOTION_TWEEN_KIND) {
    resolve(m); /* physics: the duration comes from the distance */
  }
  const double delay = sane_delay(v->delay);
  start_now(m, delay); /* a negative delay starts it partway (Motion) */
  if (v->immediate || (m->dur == 0.0 && delay <= 0.0)) {
    render(m, local_at(m, G.root_time), true);
  }
  return m;
}

BKMotion *BK_timeline(const BKMotionVars *vars)
{
  BKMotion *m = motion_new(vars);
  m->timeline = true;
  set_dur(m, 0.0);
  start_now(m, vars ? sane_time(vars->delay) : 0.0);
  return m;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Controls
 * \{ */

/* a motion out of every timeline (it ended) goes back in the global one; a killed one stays
 * out (the controls check, as they have no use for it) */
static void ensure_placed(BKMotion *m)
{
  if (!m->parent && !m->killed) {
    place(root(), m, G.root_time - (m->reversed ? -1.0 : 1.0) * m->t / m->ts);
    m->anchor_local = m->t;
    m->anchor_parent = G.root_time;
  }
}

void BK_motion_play(BKMotion *m)
{
  if (!m || m->killed) {
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
  if (m && m->paused && !m->killed) {
    ensure_placed(m);
    m->anchor_parent = parent_time(m);
    m->paused = false;
    wake(m);
  }
}

void BK_motion_reverse(BKMotion *m)
{
  if (!m || m->killed) {
    return;
  }
  ensure_placed(m);
  reanchor(m);
  m->reversed = !m->reversed;
  m->paused = false;
  wake(m);
}

int BK_motion_seek(BKMotion *m, double time)
{
  if (!isfinite(time)) {
    return 0;
  }
  if (!m || m->killed) {
    return 1;
  }
  ensure_placed(m);
  m->anchor_parent = parent_time(m);
  m->anchor_local = BK_CLAMP(time, 0.0, BK_motion_total_duration(m));
  render(m, m->anchor_local, true);
  return 1;
}

void BK_motion_restart(BKMotion *m)
{
  if (!m || m->killed) {
    return;
  }
  m->started = false;
  m->reversed = false;
  m->paused = false;
  BK_motion_seek(m, 0.0);
  wake(m);
}

int BK_motion_set_progress(BKMotion *m, double progress)
{
  if (!isfinite(progress)) {
    return 0;
  }
  if (m) {
    const int iter = m->iteration;
    double it = BK_CLAMP(progress, 0.0, 1.0) * m->dur;
    if (m->yoyo && (iter & 1)) {
      it = m->dur - it;
    }
    BK_motion_seek(m, iter * (m->dur + m->rdelay) + it);
  }
  return 1;
}

int BK_motion_set_total_progress(BKMotion *m, double progress)
{
  if (!isfinite(progress)) {
    return 0;
  }
  if (m) {
    const double total = BK_motion_total_duration(m);
    BK_motion_seek(m, BK_CLAMP(progress, 0.0, 1.0) * (total >= NEVER ? m->dur : total));
  }
  return 1;
}

int BK_motion_set_time_scale(BKMotion *m, double scale)
{
  if (!isfinite(scale)) {
    return 0;
  }
  if (m && scale > 0.0) {
    reanchor(m);
    m->ts = BK_MIN(scale, 1e300);
    if (m->parent && m->parent->timeline) {
      timeline_measure(m->parent);
    }
  }
  return 1;
}

void BK_motion_complete(BKMotion *m)
{
  if (m && !m->killed) {
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
  /* `next` first: killing a child detaches it (clearing its links, maybe freeing it) */
  for (BKMotion *c = m->first, *next; c; c = next) {
    next = c->next;
    BK_motion_kill(c);
  }
  if (m->timeline) {
    timeline_measure(m); /* once: its children left without measuring (see detach) */
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
      bk_physics_eval(t->ph, it, &pos, &vel);
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
  if ((G.ticked && now == G.last_now) || !isfinite(now) || G.in_tick) {
    return BK_anim_active(); /* the same time, no time, or a callback of this very tick */
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
  G.in_tick = true;
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
  G.in_tick = false;
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

void BK_anim_set_clock(double (*now)(void))
{
  G.clock = now;
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

void BK_anim_kill_target(void *obj, int prop)
{
  ObjEntry *e = obj ? obj_find(obj) : NULL;
  for (BKMotion *c = e ? e->head : NULL, *next; c; c = next) {
    next = c->onext;
    BK_motion_retain(c); /* stopping its last track lets go of the timeline's hold */
    kill_tween(c, obj, prop);
    BK_motion_release(c);
  }
}

void bk_motion_exit(void)
{
  if (G.root) {
    while (G.nrun) { /* every one leaves, killed or not (a kill alone ends nothing for a killed one) */
      BKMotion *m = G.run[G.nrun - 1];
      BK_motion_retain(m);
      BK_motion_kill(m);
      if (m->parent == G.root) {
        detach(m);
      }
      BK_motion_release(m);
    }
    BK_motion_release(G.root);
    G.root = NULL;
  }
  MEM_SAFE_FREE(G.run);
  G.cap_run = 0;
  MEM_SAFE_FREE(G.drivers);
  G.ndrivers = G.cap_drivers = 0;
  for (int i = 0; i < G.cap_objs; i++) { /* motions still held keep no stale links */
    if (G.objs[i].obj && G.objs[i].obj != TOMB) {
      for (BKMotion *c = G.objs[i].head, *next; c; c = next) {
        next = c->onext;
        c->oprev = c->onext = NULL;
        c->olinked = false;
      }
    }
  }
  MEM_SAFE_FREE(G.objs);
  G.nobjs = G.cap_objs = 0;
  G.root_time = G.last_now = 0.0;
  G.ticked = false;
  G.in_tick = false;
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
    r_offsets[i] = sane_time(s->amount > 0.0 ? f * s->amount : f * dmax * s->each); /* a start, finite */
  }
}

/** \} */
