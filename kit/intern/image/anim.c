/* Animated media, one frame at a time, at the size drawn: GIF, animated
 * WebP and video behind one codec interface (BKImAnimCodec: Blender's
 * MovieReader shape, open / next / seek / close). In-memory codecs composite
 * frames (GIF disposal and blending, WebP's canvas) into a full-size
 * premultiplied canvas, fitted here with the plan of stills (imb_plan /
 * imb_finish); streamed codecs (movie.c: the OS decoders)
 * read the file as they go and decode NV12 at about the size drawn into a
 * recycled frame (no allocation per frame).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <math.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "image.h"

struct BKImAnim {
  const BKImAnimCodec *codec;
  void *state;
  uint8_t *file; /* in-memory codecs: the encoded file they read (owned) */
  size_t size;
  char *path;    /* streamed codecs: the file (owned) */
  BKImAnimInfo info;
  int frames;    /* from the header, at least what was decoded */
  int ftype;     /* BK_IMAGE_* */
  int index;     /* the frame next() returns next */
  double pts;    /* its time (seconds) when the codec doesn't say */
  double seek;   /* the frame to return next shows at this time (< 0: none) */
  uint64_t stream; /* streamed: the frames' BuiPixmap stream (one GPU texture) */
  BKImageFit target;
  BKImPlan plan; /* how frames are fitted */
};

/* The in-memory animation codecs this build has (features.json). */
static const BKImAnimCodec *const anim_codecs[] = {
#ifdef BUI_IMAGE_GIF
    &imb_gif_codec,
#endif
#ifdef BUI_IMAGE_WEBP
    &imb_webp_codec,
#endif
    NULL,
};

const BKImAnimCodec *imb_anim_codec(const uint8_t *mem, size_t size)
{
  for (int i = 0; anim_codecs[i]; i++) {
    if (anim_codecs[i]->is_a(mem, size)) {
      return anim_codecs[i];
    }
  }
  return NULL;
}

static BKImAnim *anim_new(const BKImAnimCodec *codec, const BKImageFit *target)
{
  BKImAnim *a = (BKImAnim *)MEM_callocN(sizeof(BKImAnim), "BKImAnim");
  a->codec = codec;
  a->target = *target;
  a->seek = -1.0;
  return a;
}

BKImAnim *imb_anim_open(uint8_t *file, size_t size, const BKImageFit *target)
{
  BKImageInfo info;
  const BKImFileType *t = imb_file_type(file, size);
  const BKImAnimCodec *codec = NULL;
  if (t && t->load_header(file, size, &info) && info.width > 0 && info.height > 0) {
    codec = imb_anim_codec(file, size);
  }
  if (!codec) {
    MEM_freeN(file);
    return NULL;
  }
  BKImAnim *a = anim_new(codec, target);
  a->ftype = t->ftype;
  a->file = file;
  a->size = size;
  a->frames = BK_MAX(1, info.frames);
  a->state = codec->open(NULL, file, size, &a->info);
  if (!a->state || !imb_plan(target, a->info.w, a->info.h, 1, &a->plan)) {
    imb_anim_free(a); /* frees `file` too */
    return NULL;
  }
  return a;
}

/* A streamed codec decoding at `a`'s target: the whole frame at the plan's
 * size (validated here, at the codec boundary: even-sized NV12 frames). */
static bool movie_output(BKImAnim *a)
{
  BKImPlan plan;
  BKImAnimInfo info = a->info;
  if (!imb_plan(&a->target, info.src_w, info.src_h, 1, &plan) ||
      !a->codec->output(a->state, plan.fw, plan.fh, &info) || info.w <= 0 || info.h <= 0 ||
      (info.w | info.h) & 1 || info.w > BK_IMB_MAX_SIDE || info.h > BK_IMB_MAX_SIDE ||
      (info.format != BUI_PIXMAP_NV12_BT601 && info.format != BUI_PIXMAP_NV12_BT709))
  {
    return false;
  }
  a->info = info;
  a->plan = plan;
  return true;
}

BKImAnim *imb_movie_open(const char *path, const BKImageFit *target)
{
  size_t n;
  uint8_t *head = imb_read_file(path, 64, &n);
  if (!head) {
    return NULL;
  }
  BKImAnim *a = NULL;
  for (int i = 0; imb_movie_codecs[i] && !a; i++) {
    const BKImAnimCodec *codec = imb_movie_codecs[i];
    if (codec->is_a && !codec->is_a(head, n)) {
      continue;
    }
    a = anim_new(codec, target);
    const size_t len = strlen(path);
    a->path = (char *)MEM_mallocN(len + 1, "movie path");
    memcpy(a->path, path, len + 1);
    a->ftype = BK_IMAGE_VIDEO;
    a->stream = imb_new_key();
    a->state = codec->open(path, NULL, 0, &a->info);
    if (!a->state || a->info.src_w <= 0 || a->info.src_h <= 0 ||
        a->info.src_w > BK_IMB_MAX_SIDE || a->info.src_h > BK_IMB_MAX_SIDE || !movie_output(a))
    {
      imb_anim_free(a);
      a = NULL; /* the next backend may read it */
    }
  }
  MEM_freeN(head);
  return a;
}

void imb_anim_free(BKImAnim *a)
{
  if (a) {
    if (a->state) {
      a->codec->close(a->state);
    }
    if (a->stream) {
      BUI_image_forget(a->stream); /* its GPU texture */
    }
    MEM_freeN(a->file);
    MEM_freeN(a->path);
    MEM_freeN(a);
  }
}

const BKImAnimInfo *imb_anim_info(const BKImAnim *a)
{
  return &a->info;
}

const BKImAnimCodec *imb_anim_backend(const BKImAnim *a)
{
  return a->codec;
}

int imb_anim_frames(const BKImAnim *a)
{
  return a->frames;
}

size_t imb_anim_bytes(const BKImAnim *a)
{
  return sizeof(*a) + a->size + a->codec->bytes(a->state);
}

bool imb_anim_retarget(BKImAnim *a, const BKImageFit *target)
{
  if (memcmp(&a->target, target, sizeof(*target)) == 0) {
    return true;
  }
  const BKImageFit old = a->target;
  a->target = *target;
  if (!a->codec->streamed) {
    BKImPlan plan;
    if (imb_plan(target, a->info.w, a->info.h, 1, &plan)) {
      a->plan = plan;
      return true;
    }
  }
  else if (movie_output(a)) {
    imb_anim_seek(a, a->pts); /* where it was (the decoder flushes at the new size) */
    return true;
  }
  a->target = old;
  return false;
}

void imb_anim_seek(BKImAnim *a, double t)
{
  a->seek = BK_MAX(0.0, t);
}

/* The next frame from the codec into `dst`, wrapping to the first after the
 * last; -1 on errors. */
static int codec_next(BKImAnim *a, BKImBuf *dst, double *r_pts, double *r_dur, bool *r_wrapped)
{
  *r_wrapped = false;
  if (a->seek >= 0.0) {
    a->codec->seek(a->state, a->seek);
    a->index = 0;
    a->pts = 0.0;
  }
  for (;;) {
    double pts = -1.0, dur = 0.0;
    int got = a->codec->next(a->state, dst, &pts, &dur);
    if (got == 0 && a->index > 0) { /* past the last frame: from the top */
      a->codec->seek(a->state, 0.0);
      a->index = 0;
      a->pts = 0.0;
      *r_wrapped = true;
      got = a->codec->next(a->state, dst, &pts, &dur);
    }
    if (got <= 0) {
      a->seek = -1.0;
      return -1;
    }
    if (!a->codec->streamed && dur <= 0.0105) {
      dur = 0.1; /* GIF/WebP 0 and 10 ms mean "as fast as possible": browsers show 100 ms */
    }
    pts = pts >= 0.0 ? pts : a->pts;
    a->pts = pts + dur;
    a->index++;
    a->frames = BK_MAX(a->frames, a->index); /* headers can undercount */
    /* a seek lands on the frame showing at its time: skip the ones before */
    if (a->seek >= 0.0 && pts + dur <= a->seek + 1e-6 && !*r_wrapped) {
      continue;
    }
    a->seek = -1.0;
    *r_pts = pts;
    *r_dur = dur;
    return 1;
  }
}

/* The next composited frame at full size (a fresh buffer), in-memory codecs. */
static BKImBuf *next_full(BKImAnim *a, double *r_pts, double *r_dur, bool *r_wrapped)
{
  BKImBuf *ib = imb_alloc(a->info.w, a->info.h, 1);
  if (!ib) {
    return NULL;
  }
  ib->ftype = a->ftype;
  if (codec_next(a, ib, r_pts, r_dur, r_wrapped) < 0) {
    MEM_freeN(ib);
    return NULL;
  }
  return ib;
}

BKImBuf *imb_anim_next(BKImAnim *a, BKImBuf *reuse, double *r_pts, double *r_dur, bool *r_wrapped)
{
  if (!a->codec->streamed) {
    BK_imbuf_free(reuse); /* composited and fitted into fresh buffers */
    BKImBuf *ib = next_full(a, r_pts, r_dur, r_wrapped);
    return ib ? imb_finish(ib, &a->plan) : NULL;
  }
  BKImBuf *ib = reuse;
  if (ib && (ib->x != a->info.w || ib->y != a->info.h || ib->format != a->info.format)) {
    BK_imbuf_free(ib); /* resized */
    ib = NULL;
  }
  if (!ib) {
    ib = imb_alloc_nv12(a->info.w, a->info.h, a->info.format);
    if (!ib) {
      return NULL;
    }
    ib->ftype = BK_IMAGE_VIDEO;
    ib->stream = a->stream;
  }
  if (codec_next(a, ib, r_pts, r_dur, r_wrapped) < 0) {
    BK_imbuf_free(ib);
    return NULL;
  }
  imb_rekey(ib); /* new pixels: GPU renderers update the stream's texture */
  /* the whole frame is decoded; the visible part is drawn from it */
  const double kx = ib->x / (double)a->info.src_w, ky = ib->y / (double)a->info.src_h;
  ib->src_x = a->info.src_w;
  ib->src_y = a->info.src_h;
  ib->crop[0] = (float)(a->plan.src[0] * kx);
  ib->crop[1] = (float)(a->plan.src[1] * ky);
  ib->crop[2] = (float)(a->plan.src[2] * kx);
  ib->crop[3] = (float)(a->plan.src[3] * ky);
  return ib;
}

BKImBuf *imb_anim_frame(const uint8_t *mem, size_t size, const BKImageFit *target, int frame)
{
  uint8_t *copy = (uint8_t *)MEM_mallocN(size, "anim file");
  if (!copy) {
    return NULL;
  }
  memcpy(copy, mem, size);
  BKImAnim *a = imb_anim_open(copy, size, target);
  BKImBuf *ib = NULL;
  for (int i = 0; a && i <= frame; i++) {
    double pts, dur;
    bool wrapped;
    MEM_freeN(ib);
    ib = next_full(a, &pts, &dur, &wrapped);
    if (!ib || wrapped) { /* past the end: no such frame */
      MEM_freeN(ib);
      ib = NULL;
      break;
    }
  }
  if (ib) {
    ib = imb_finish(ib, &a->plan);
  }
  imb_anim_free(a);
  return ib;
}

BKImBuf *imb_anim_all(const uint8_t *mem, size_t size, const BKImageFit *target)
{
  uint8_t *copy = (uint8_t *)MEM_mallocN(size, "anim file");
  if (!copy) {
    return NULL;
  }
  memcpy(copy, mem, size);
  BKImAnim *a = imb_anim_open(copy, size, target);
  if (!a) {
    return NULL;
  }
  /* every frame in one buffer (the synchronous API), at most 128 MiB of them */
  const int w = a->info.w, h = a->info.h;
  const int frames = (int)BK_MIN((size_t)a->frames,
                                 BK_MAX((size_t)1, ((size_t)1 << 25) / ((size_t)w * h)));
  BKImBuf *all = imb_alloc(w, h, frames);
  int n = 0;
  while (all && n < frames) {
    double pts, dur;
    bool wrapped;
    BKImBuf *ib = next_full(a, &pts, &dur, &wrapped);
    if (!ib || wrapped) {
      MEM_freeN(ib);
      break;
    }
    memcpy(imb_frame(all, n), ib->rect, sizeof(uint32_t) * (size_t)w * h);
    if (all->delays) {
      all->delays[n] = (int)lround(dur * 1000.0);
    }
    MEM_freeN(ib);
    n++;
  }
  if (all && n == 0) {
    MEM_freeN(all);
    all = NULL;
  }
  if (all) {
    all->frames = n;
    all->loops = a->info.loops;
    all->ftype = a->ftype;
    all = imb_finish(all, &a->plan);
  }
  imb_anim_free(a);
  return all;
}

/* -------------------------------------------------------------------- */
/** \name Public (BUI_kit.h): an animation stepped by the caller
 * \{ */

BKAnim *BK_anim_open(const void *data, size_t len, int max_w, int max_h)
{
  uint8_t *copy = data && len ? (uint8_t *)MEM_mallocN(len, "anim file") : NULL;
  if (!copy) {
    return NULL;
  }
  memcpy(copy, data, len);
  const BKImageFit f = imb_fit_within(max_w, max_h);
  return (BKAnim *)imb_anim_open(copy, len, &f);
}

BKImBuf *BK_anim_next(BKAnim *anim, int *r_delay_ms)
{
  double pts = 0.0, dur = 0.0;
  bool wrapped;
  BKImBuf *ib = anim ? imb_anim_next((BKImAnim *)anim, NULL, &pts, &dur, &wrapped) : NULL;
  if (r_delay_ms) {
    *r_delay_ms = (int)lround(dur * 1000.0);
  }
  return ib;
}

int BK_anim_info(const BKAnim *anim, int *r_w, int *r_h, int *r_loops)
{
  const BKImAnim *a = (const BKImAnim *)anim;
  if (r_w) {
    *r_w = a ? a->info.w : 0;
  }
  if (r_h) {
    *r_h = a ? a->info.h : 0;
  }
  if (r_loops) {
    *r_loops = a ? a->info.loops : 0;
  }
  return a ? a->frames : 0;
}

void BK_anim_free(BKAnim *anim)
{
  imb_anim_free((BKImAnim *)anim);
}

/** \} */
