/* The shared image cache: decoded images by (source, target), on the UI
 * thread. The design of Blender's moviecache (imbuf/intern/moviecache.cc:
 * a GHash of entries, a memory limiter that frees the least recently used)
 * with the TextCache rule for what may go (blenderui-paint text_cache.rs:
 * nothing the current or the previous generation used).
 *
 *  - Lookups don't allocate; a miss allocates its entry and request once.
 *  - Bounded: over the byte budget, the least recently used entries go
 *    (oldest first) at the start of a frame, never ones drawn in the last
 *    two generations (they are on screen): visible images can exceed the
 *    budget, the rest can't.
 *  - Deterministic: BK_image_exit frees everything; MEM counts all of it.
 *  - Private entries (a <video> each, video.c) use the same requests,
 *    clock and visibility tracking, outside the table and the LRU: their
 *    owner frees them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <string.h>

#include "BLI_ghash.h"
#include "MEM_guardedalloc.h"

#include "../audio/audio.h"
#include "image.h"
#include "kit_intern.h"

#define MAX_WAITERS 4
#define DEFAULT_BUDGET ((size_t)256 << 20)
#define MAX_ENTRIES 65536 /* failed and tiny entries count too */

typedef struct Waiter {
  BKContext *ctx;
  int rect[4];   /* x0 y0 x1 y1, window pixels */
  unsigned pass; /* the drawing pass that last asked */
} Waiter;

typedef struct BKImEntry Entry;
struct BKImEntry {
  Entry *lru_prev, *lru_next;   /* most recently used first */
  Entry *pend_prev, *pend_next; /* while a request is out */
  BKImageSource *source;               /* referenced */
  BKImageFit target;
  BKImBuf *ib;       /* NULL while pending or after a failed decode */
  BKImRequest *req;  /* the request out (NULL when settled) */
  BKImRequest *spare; /* a step's request, kept for the next step (no allocation per frame) */
  unsigned used;     /* generation of the last use */
  size_t bytes;      /* counted in C.bytes: ib, and play's decoder and next frame */
  BKImPlay *play;    /* a playing animation (NULL for stills) */
  Entry *play_prev, *play_next; /* C.playing */
  /* private entries (owned: video.c): who hears state changes, and the
   * last state it heard; orphaned when the owner let go with a request out */
  bool owned, orphan;
  unsigned hosted; /* C.gen + 1 when a host loop last showed it without a rect (textures) */
  void (*notify)(void *owner);
  void *owner;
  BKImPlayState heard;
  /* where it shows (window rects per context): while pending, what redraws
   * on arrival; for animations, what redraws on every frame (a context
   * that redraws a rect without asking drops it: off screen, paused) */
  int nwaiters;
  Waiter waiters[MAX_WAITERS];
};

struct BKImageSource {
  int refs;
  int info_state; /* 0 unknown, 1 read, -1 not an image */
  BKImageInfo info;
  char path[1]; /* NUL-terminated, allocated with the struct */
};

static struct {
  GHash *sources; /* path -> BKImageSource */
  GHash *entries; /* Entry -> Entry */
  Entry *lru_head, *lru_tail, *pending, *playing;
  size_t bytes, budget;
  unsigned gen;
  int count;
  BKImageStats stats;
  struct {
    BKContext *ctx;
    int rect[4];
    unsigned serial;
  } pass;
  bool wake_ready;
} C = {.budget = DEFAULT_BUDGET};

/* -------------------------------------------------------------------- */
/** \name Sources
 * \{ */

BKImageSource *bk_image_source_get(const char *src)
{
  if (!src || !src[0]) {
    return NULL;
  }
  if (!C.sources) {
    C.sources = BLI_ghash_str_new("image sources");
  }
  BKImageSource *s = (BKImageSource *)BLI_ghash_lookup(C.sources, src);
  if (!s) {
    const size_t n = strlen(src);
    s = (BKImageSource *)MEM_callocN(sizeof(BKImageSource) + n, "BKImageSource");
    memcpy(s->path, src, n + 1);
    BLI_ghash_insert(C.sources, s->path, s);
  }
  s->refs++;
  return s;
}

void bk_image_source_release(BKImageSource *s)
{
  if (s && --s->refs == 0) {
    BLI_ghash_remove(C.sources, s->path, NULL, NULL);
    MEM_freeN(s);
    if (BLI_ghash_len(C.sources) == 0) {
      BLI_ghash_free(C.sources, NULL, NULL);
      C.sources = NULL;
    }
  }
}

const char *bk_image_source_path(const BKImageSource *s)
{
  return s->path;
}

bool bk_image_source_info(BKImageSource *s, BKImageInfo *r_info)
{
  if (s->info_state == 0) {
    s->info_state = BK_image_probe_file(s->path, &s->info) ? 1 : -1;
  }
  *r_info = s->info;
  return s->info_state > 0;
}

int BK_image_info(const char *src, BKImageInfo *r_info)
{
  BKImageSource *s = bk_image_source_get(src);
  BKImageInfo info = {0};
  const bool ok = s && bk_image_source_info(s, &info);
  if (r_info) {
    *r_info = info;
  }
  bk_image_source_release(s); /* the info stays while anything else holds the source */
  return ok;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Entries
 * \{ */

static unsigned entry_hash(const void *key)
{
  const Entry *e = (const Entry *)key;
  uint64_t h = (uint64_t)(uintptr_t)e->source * 0x9E3779B97F4A7C15ull;
  h ^= ((uint64_t)(unsigned)e->target.box_w << 32 | (unsigned)e->target.box_h) * 0xC2B2AE3D27D4EB4Full;
  h ^= (uint64_t)e->target.fit * 0x165667B19E3779F9ull;
  h ^= (uint64_t)(int)(e->target.pos_x * 4096.0f) << 20 ^ (uint64_t)(int)(e->target.pos_y * 4096.0f);
  h ^= h >> 29;
  return (unsigned)(h ^ (h >> 32));
}

static bool entry_cmp(const void *a, const void *b)
{
  const Entry *x = (const Entry *)a, *y = (const Entry *)b;
  return x->source != y->source || memcmp(&x->target, &y->target, sizeof(BKImageFit)) != 0;
}

static void lru_unlink(Entry *e)
{
  *(e->lru_prev ? &e->lru_prev->lru_next : &C.lru_head) = e->lru_next;
  *(e->lru_next ? &e->lru_next->lru_prev : &C.lru_tail) = e->lru_prev;
  e->lru_prev = e->lru_next = NULL;
}

static void lru_front(Entry *e)
{
  e->lru_prev = NULL;
  e->lru_next = C.lru_head;
  *(C.lru_head ? &C.lru_head->lru_prev : &C.lru_tail) = e;
  C.lru_head = e;
}

static void pend_unlink(Entry *e)
{
  *(e->pend_prev ? &e->pend_prev->pend_next : &C.pending) = e->pend_next;
  if (e->pend_next) {
    e->pend_next->pend_prev = e->pend_prev;
  }
  e->pend_prev = e->pend_next = NULL;
}

/* Re-counts what `e` holds in C.bytes. */
static void entry_account(Entry *e)
{
  const size_t b = (e->ib ? e->ib->size : 0) + (e->play ? imb_play_bytes(e->play) : 0);
  C.bytes += b - e->bytes;
  e->bytes = b;
}

static void entry_free(Entry *e)
{
  if (!e->owned) {
    BLI_ghash_remove(C.entries, e, NULL, NULL);
    lru_unlink(e);
  }
  if (e->play) {
    *(e->play_prev ? &e->play_prev->play_next : &C.playing) = e->play_next;
    if (e->play_next) {
      e->play_next->play_prev = e->play_prev;
    }
    imb_play_free(e->play);
  }
  C.bytes -= e->bytes;
  BK_imbuf_free(e->ib);
  bk_image_source_release(e->source);
  MEM_freeN(e->spare);
  MEM_freeN(e);
  C.count--;
}

/* A first load, or (`step`) the next frame of what plays when it has one to decode. */
static void request(Entry *e, int priority, bool step)
{
  BKImBuf *reuse = NULL;
  double seek = -1.0;
  BKImAnim *anim = step ? imb_play_take_step(e->play, &reuse, &seek) : NULL;
  if (step && !anim) {
    return; /* nothing to decode now */
  }
  BKImRequest *req = e->spare && step ? e->spare :
                                       (BKImRequest *)MEM_mallocN(sizeof(BKImRequest), "BKImRequest");
  e->spare = req == e->spare ? NULL : e->spare;
  memset(req, 0, sizeof(*req));
  req->step = step;
  req->anim = anim;
  req->reuse = reuse;
  req->seek = seek;
  req->movie = e->owned && !step;
  req->source = e->source;
  e->source->refs++; /* the worker reads its path */
  req->target = e->target;
  req->priority = priority;
  req->wanted = C.gen;
  req->entry = e;
  e->req = req;
  e->pend_prev = NULL;
  e->pend_next = C.pending;
  if (C.pending) {
    C.pending->pend_prev = e;
  }
  C.pending = e;
  if (!C.wake_ready) {
    C.wake_ready = bk_window_wake_ready(); /* on this (the UI) thread */
  }
  bk_loader_push(req);
}

static void add_waiter(Entry *e, const int rect[4])
{
  if (!C.pass.ctx || !rect) {
    return;
  }
  Waiter *w = NULL;
  for (int i = 0; i < e->nwaiters && !w; i++) {
    w = e->waiters[i].ctx == C.pass.ctx ? &e->waiters[i] : NULL;
  }
  if (!w && e->nwaiters < MAX_WAITERS) {
    w = &e->waiters[e->nwaiters++];
    w->ctx = C.pass.ctx;
    memcpy(w->rect, rect, sizeof(w->rect));
  }
  else if (!w) {
    w = &e->waiters[MAX_WAITERS - 1]; /* more contexts than slots: share the last */
  }
  else {
    w->rect[0] = BK_MIN(w->rect[0], rect[0]);
    w->rect[1] = BK_MIN(w->rect[1], rect[1]);
    w->rect[2] = BK_MAX(w->rect[2], rect[2]);
    w->rect[3] = BK_MAX(w->rect[3], rect[3]);
  }
  w->pass = C.pass.serial;
}

static Entry *lookup(BKImageSource *s, const BKImageFit *target, bool create, int priority)
{
  if (!C.entries) {
    if (!create) {
      return NULL;
    }
    C.entries = BLI_ghash_new(entry_hash, entry_cmp, "image cache");
  }
  Entry key;
  key.source = s;
  key.target = *target;
  Entry *e = (Entry *)BLI_ghash_lookup(C.entries, &key);
  if (!e && create) {
    e = (Entry *)MEM_callocN(sizeof(Entry), "image cache entry");
    e->source = s;
    s->refs++;
    e->target = *target;
    BLI_ghash_insert(C.entries, e, e);
    lru_front(e);
    C.count++;
    request(e, priority, false);
  }
  else if (e) {
    if (e != C.lru_head) {
      lru_unlink(e);
      lru_front(e);
    }
    if (e->req && (priority > e->req->priority || e->used != C.gen)) {
      bk_loader_touch(e->req, BK_MAX(priority, e->req->priority), C.gen);
    }
  }
  if (e) {
    e->used = C.gen;
  }
  return e;
}

const BKImBuf *bk_image_cache_get(BKImageSource *s, const BKImageFit *target, const int rect[4])
{
  if (!s) {
    return NULL;
  }
  Entry *e = lookup(s, target, true, BK_IMAGE_PRIORITY_VISIBLE);
  if (e->req || e->play) {
    add_waiter(e, rect);
  }
  if (e->play && e->ib) {
    imb_play_drawn(e->play, bk_time());
  }
  return e->ib;
}

void bk_image_cache_prefetch(BKImageSource *s, const BKImageFit *target)
{
  if (s) {
    lookup(s, target, true, BK_IMAGE_PRIORITY_PREFETCH);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frames, passes, results
 * \{ */

static void damage(const Entry *e)
{
  for (int i = 0; i < e->nwaiters; i++) {
    const int *r = e->waiters[i].rect;
    bk_ctx_dirty_rect(e->waiters[i].ctx, (BKRect){r[0], r[1], r[2], r[3]});
  }
}

/* Timers for the contexts that show `e` (kit_custom.c arms the earliest). */
static void arm(const Entry *e)
{
  for (int i = 0; i < e->nwaiters; i++) {
    bk_ctx_arm_timer(e->waiters[i].ctx);
  }
}

/* A private entry's owner hears every state change (loaded, ended, error). */
static void heard(Entry *e)
{
  const BKImPlayState st = e->play ? imb_play_state(e->play) : BK_PLAY_LOADING;
  if (e->notify && st != e->heard) {
    e->heard = st;
    e->notify(e->owner);
  }
}

static void playing_link(Entry *e)
{
  e->play_next = C.playing;
  if (C.playing) {
    C.playing->play_prev = e;
  }
  C.playing = e;
}

/* A step of a playing animation came back. */
static void step_done(Entry *e, BKImRequest *req)
{
  if (req->state == BK_IMREQ_CANCELLED) {
    imb_play_step_cancelled(e->play, req->anim, req->reuse);
  }
  else {
    imb_play_step_done(e->play, req->anim, req->result, req->pts, req->dur, req->wrapped);
    req->result = NULL;
    C.stats.decoded++;
  }
  req->anim = NULL;
  req->reuse = NULL;
  entry_account(e);
  arm(e);
}

int BK_image_poll(void)
{
  int n = 0;
  BKImRequest *req = bk_loader_take_done();
  while (req) {
    BKImRequest *next = req->next;
    Entry *e = (Entry *)req->entry;
    Entry *keeper = NULL; /* keeps a step's request for its next step */
    if (e && !e->orphan && req->state == BK_IMREQ_CANCELLED && e->nwaiters > 0 && !req->step) {
      bk_loader_push(req); /* asked for again after the cancel: still wanted */
      req = next;
      continue;
    }
    if (e && e->orphan) { /* its owner let go while this was out */
      e->req = NULL;
      pend_unlink(e);
      entry_free(e);
    }
    else if (e && req->step) {
      e->req = NULL;
      pend_unlink(e);
      step_done(e, req);
      keeper = e->spare ? NULL : e;
    }
    else if (e) {
      e->req = NULL;
      pend_unlink(e);
      if (req->state == BK_IMREQ_CANCELLED) {
        if (!e->owned) {
          entry_free(e);
        }
        C.stats.cancelled++;
      }
      else {
        e->ib = req->result;
        req->result = NULL;
        if (e->ib && req->anim) { /* it plays: keep the decoder, show where it is */
          if (!e->play) {
            e->play = imb_play_new(false, -1);
            playing_link(e);
          }
          imb_play_start(e->play, req->anim, req->pts, req->dur);
          req->anim = NULL;
        }
        else if (e->play) {
          imb_play_fail(e->play); /* a video that didn't open */
        }
        entry_account(e);
        if (e->ib) {
          C.stats.decoded++;
          if (e->source->info_state == 0) { /* free intrinsic size for layout */
            e->source->info = (BKImageInfo){e->ib->ftype, e->ib->src_x, e->ib->src_y, e->ib->frames};
            e->source->info_state = 1;
          }
        }
        else {
          C.stats.failed++;
        }
        damage(e);
        if (!e->play) {
          e->nwaiters = 0; /* a still: nothing more to redraw */
        }
        heard(e);
        n++;
      }
    }
    BK_imbuf_free(req->result);
    BK_imbuf_free(req->reuse);
    imb_anim_free(req->anim); /* a first load whose entry is gone */
    bk_image_source_release(req->source);
    if (keeper) {
      keeper->spare = req;
    }
    else {
      MEM_freeN(req);
    }
    req = next;
  }
  return n;
}

void bk_image_tick(void)
{
  if (!C.playing || !bk_animations_enabled()) {
    return; /* reduced motion: animations hold their frame */
  }
  const double now = bk_time() + 0.002; /* timers fire a little early */
  for (Entry *e = C.playing; e; e = e->play_next) {
    if (e->nwaiters == 0 && !(e->hosted && C.gen + 1 - e->hosted <= 1)) {
      continue; /* nobody shows it: paused, no decoding */
    }
    BKImBuf *frame = imb_play_advance(e->play, now);
    if (frame) {
      imb_play_recycle(e->play, e->ib); /* a movie decodes its next frame into it */
      e->ib = frame;
      entry_account(e);
      damage(e); /* only its rectangles redraw */
    }
    if (!e->req) {
      request(e, BK_IMAGE_PRIORITY_VISIBLE, true); /* the one after, while this shows */
    }
    arm(e);
    heard(e);
  }
}

double bk_image_next_due(BKContext *ctx)
{
  double due = 0.0;
  if (!C.playing || !bk_animations_enabled()) {
    return 0.0; /* nothing plays: no clock read */
  }
  const double now = bk_time();
  for (const Entry *e = C.playing; e; e = e->play_next) {
    const double d = imb_play_due(e->play, now);
    for (int i = 0; d > 0.0 && i < e->nwaiters; i++) {
      if (e->waiters[i].ctx == ctx && (due == 0.0 || d < due)) {
        due = d;
      }
    }
  }
  return due;
}

static void evict(void)
{
  /* oldest first; what the last two generations drew stays */
  Entry *e = C.lru_tail;
  while (e && (C.bytes > C.budget || C.count > MAX_ENTRIES)) {
    Entry *prev = e->lru_prev;
    if (C.gen - e->used <= 1) {
      break; /* LRU order: everything before it is newer */
    }
    if (!e->req) {
      entry_free(e);
      C.stats.evicted++;
    }
    e = prev;
  }
}

void bk_image_frame_begin(void)
{
  C.gen++;
  evict();
  /* prefetches nobody renewed for two generations */
  for (Entry *e = C.pending, *next; e; e = next) {
    next = e->pend_next;
    if (e->req->priority == BK_IMAGE_PRIORITY_PREFETCH && C.gen - e->used > 2) {
      bk_loader_cancel(e->req);
    }
  }
}

void bk_image_pass_begin(BKContext *ctx, int x0, int y0, int x1, int y1)
{
  C.pass.ctx = ctx;
  C.pass.rect[0] = x0;
  C.pass.rect[1] = y0;
  C.pass.rect[2] = x1;
  C.pass.rect[3] = y1;
  C.pass.serial++;
}

/* Rects of `e` this pass covered without asking for it: dropped. */
static bool drop_waiters(Entry *e)
{
  const int *p = C.pass.rect;
  bool dropped = false;
  for (int i = e->nwaiters - 1; i >= 0; i--) {
    const Waiter *w = &e->waiters[i];
    if (w->ctx == C.pass.ctx && w->pass != C.pass.serial && w->rect[0] >= p[0] &&
        w->rect[1] >= p[1] && w->rect[2] <= p[2] && w->rect[3] <= p[3])
    {
      e->waiters[i] = e->waiters[--e->nwaiters];
      dropped = true;
    }
  }
  return dropped;
}

void bk_image_pass_end(void)
{
  /* A pending image whose whole rectangle this pass redrew without asking
   * for it no longer shows there; with no one left to show it, cancel. */
  for (Entry *e = C.pending; e; e = e->pend_next) {
    if (drop_waiters(e) && e->nwaiters == 0 && !e->play &&
        e->req->priority == BK_IMAGE_PRIORITY_VISIBLE)
    {
      bk_loader_cancel(e->req);
    }
  }
  /* and an animation drawn nowhere any more pauses (bk_image_tick skips it) */
  for (Entry *e = C.playing; e; e = e->play_next) {
    drop_waiters(e);
  }
  if (C.pass.ctx) {
    bk_ctx_arm_timer(C.pass.ctx); /* frames drawn now start their clocks */
  }
  C.pass.ctx = NULL;
}

static void forget_waiters(Entry *e, const BKContext *ctx)
{
  for (int i = e->nwaiters - 1; i >= 0; i--) {
    if (e->waiters[i].ctx == ctx) {
      e->waiters[i] = e->waiters[--e->nwaiters];
    }
  }
}

void bk_image_forget_context(BKContext *ctx)
{
  for (Entry *e = C.pending; e; e = e->pend_next) {
    forget_waiters(e, ctx);
  }
  for (Entry *e = C.playing; e; e = e->play_next) {
    forget_waiters(e, ctx);
  }
  if (C.pass.ctx == ctx) {
    C.pass.ctx = NULL;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Private entries (video.c)
 * \{ */

BKImEntry *bk_image_entry_new(BKImageSource *s, BKImPlay *play, void (*notify)(void *owner),
                              void *owner)
{
  Entry *e = (Entry *)MEM_callocN(sizeof(Entry), "image private entry");
  e->owned = true;
  e->source = s;
  s->refs++;
  e->play = play;
  e->notify = notify;
  e->owner = owner;
  playing_link(e);
  C.count++;
  entry_account(e);
  return e;
}

void bk_image_entry_free(BKImEntry *e)
{
  if (!e) {
    return;
  }
  if (e->req) {
    e->orphan = true; /* freed when the request comes back (BK_image_poll) */
    e->notify = NULL;
    e->nwaiters = 0;
    bk_loader_cancel(e->req);
    return;
  }
  entry_free(e);
}

/* Decoding at about the size drawn: a box much bigger, or much smaller, than
 * what the decoder makes asks for another size (resizing within that is
 * scaled when drawn, so a window resize doesn't reopen the decoder). */
static bool retarget_wanted(const BKImageFit *had, const BKImageFit *want)
{
  return had->fit != want->fit || want->box_w * 4 > had->box_w * 5 ||
         want->box_h * 4 > had->box_h * 5 || want->box_w * 2 < had->box_w ||
         want->box_h * 2 < had->box_h;
}

const BKImBuf *bk_image_entry_get(BKImEntry *e, const BKImageFit *target, const int rect[4])
{
  const bool loaded = imb_play_state(e->play) != BK_PLAY_LOADING;
  if ((!loaded && !e->req && imb_play_state(e->play) != BK_PLAY_ERROR) ||
      retarget_wanted(&e->target, target))
  {
    e->target = *target; /* the first load, or the next step, decodes at this size */
  }
  if (!loaded && !e->req && imb_play_state(e->play) != BK_PLAY_ERROR) {
    request(e, BK_IMAGE_PRIORITY_VISIBLE, false);
  }
  if (rect) {
    add_waiter(e, rect);
  }
  else {
    e->hosted = C.gen + 1;
  }
  if (e->ib) {
    imb_play_drawn(e->play, bk_time());
  }
  return e->ib;
}

BKImPlay *bk_image_entry_play(BKImEntry *e)
{
  return e->play;
}

const char *bk_image_entry_path(const BKImEntry *e)
{
  return bk_image_source_path(e->source);
}

void bk_image_entry_changed(BKImEntry *e)
{
  if (!e->req) {
    request(e, BK_IMAGE_PRIORITY_VISIBLE, true); /* a seek decodes now */
  }
  damage(e);
  arm(e);
  heard(e);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Settings, statistics, exit (BUI_kit.h)
 * \{ */

void BK_image_set_budget(size_t bytes)
{
  C.budget = bytes;
}

void BK_image_stats(BKImageStats *r)
{
  *r = C.stats;
  r->bytes = C.bytes;
  r->budget = C.budget;
  r->entries = C.count;
  double decode_s;
  r->pending = bk_loader_pending(&decode_s);
  r->decode_ms = decode_s * 1000.0;
}

void BK_image_prefetch(const char *src, int w, int h, int fit)
{
  BKImageSource *s = bk_image_source_get(src);
  const BKImageFit t = {fit, w, h, 0.5f, 0.5f, 0};
  bk_image_cache_prefetch(s, &t);
  bk_image_source_release(s); /* the entry keeps it */
}

void BK_image_clear_cache(void)
{
  for (Entry *e = C.lru_tail, *prev; e; e = prev) {
    prev = e->lru_prev;
    if (!e->req) {
      entry_free(e);
    }
  }
}

void BK_image_exit(void)
{
  bk_loader_exit(); /* queued requests come back cancelled, running ones finish */
  for (Entry *e = C.pending; e; e = e->pend_next) {
    e->nwaiters = 0; /* nothing is re-queued now */
  }
  BK_image_poll();
  BK_image_clear_cache();
  if (C.entries) {
    BLI_ghash_free(C.entries, NULL, NULL);
    C.entries = NULL;
  }
  C.wake_ready = false;
  memset(&C.stats, 0, sizeof(C.stats));
  bk_audio_exit(2.0); /* the sound of freed videos is released on threads of its own */
}

/** \} */
