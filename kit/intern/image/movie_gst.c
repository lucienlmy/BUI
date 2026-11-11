/* Video on Linux: GStreamer, loaded at run time (dlopen; no headers or
 * libraries needed to build). GStreamer rather than raw VA-API: it brings
 * the demuxers (MP4, WebM, MKV, ...) and every decoder the system has,
 * picks VA-API hardware decoding through its va plugin where the GPU and
 * driver offer it, and falls back to software (libav, openh264, dav1d)
 * where not; raw VA-API would need our own demuxers and bitstream parsing
 * per codec, and still a software fallback.
 *
 * The pipeline decodes, converts and scales to NV12 at the size drawn:
 *   filesrc ! decodebin ! videoconvert ! videoscale ! capsfilter ! appsink
 * The appsink holds at most two frames and blocks the pipeline when they
 * aren't pulled: a video that isn't shown costs nothing. Its sound is a
 * second pipeline, pulled by the audio device's thread (audio/audio.c):
 *   filesrc ! decodebin ! audioconvert ! audioresample ! float caps ! appsink
 *
 * The few GStreamer types used are mirrored here (stable 1.x ABI:
 * GstBuffer timestamps, GstMapInfo, GstVideoMeta).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef BUI_VIDEO_GST

#  include <dlfcn.h>
#  include <pthread.h>
#  include <stdio.h>
#  include <string.h>

#  include "MEM_guardedalloc.h"

#  include "image.h"

/* -------------------------------------------------------------------- */
/** \name GStreamer, loaded once
 * \{ */

typedef struct GstElement GstElement;
typedef struct GstSample GstSample;
typedef struct GstCaps GstCaps;
typedef struct GstStructure GstStructure;
typedef struct GstVideoMetaX GstVideoMetaX;
typedef struct GstBufferX { /* GstBuffer: GstMiniObject (64 bytes), then: */
  uint8_t mini_object[64];
  void *pool;
  uint64_t pts, dts, duration;
} GstBufferX;
typedef struct GstMapInfoX {
  void *memory;
  int flags;
  uint8_t *data;
  size_t size, maxsize;
  void *user_data[4];
  void *reserved[4];
} GstMapInfoX;
struct GstVideoMetaX { /* GstVideoMeta */
  int meta_flags;
  const void *info;
  GstBufferX *buffer;
  int flags, format;
  unsigned id, width, height, n_planes;
  size_t offset[4];
  int stride[4];
};

enum { GST_STATE_NULL = 1, GST_STATE_PAUSED = 3, GST_STATE_PLAYING = 4 };
enum { GST_STATE_CHANGE_FAILURE = 0 };
enum { GST_FORMAT_TIME = 3 };
enum { GST_SEEK_FLAG_FLUSH = 1 << 0, GST_SEEK_FLAG_KEY_UNIT = 1 << 2 };
enum { GST_MAP_READ = 1 };
#  define GST_SECOND 1000000000ull
#  define GST_NONE ((uint64_t)-1)

static struct {
  pthread_once_t once;
  bool ok;
  int (*init_check)(int *, char ***, void **);
  GstElement *(*parse_launch)(const char *, void **);
  GstElement *(*bin_get_by_name)(GstElement *, const char *);
  int (*set_state)(GstElement *, int);
  int (*get_state)(GstElement *, int *, int *, uint64_t);
  int (*query_duration)(GstElement *, int, int64_t *);
  int (*seek_simple)(GstElement *, int, int, int64_t);
  void (*object_unref)(void *);
  void (*mini_object_unref)(void *);
  GstBufferX *(*sample_get_buffer)(GstSample *);
  GstCaps *(*sample_get_caps)(GstSample *);
  GstCaps *(*caps_from_string)(const char *);
  GstStructure *(*caps_get_structure)(const GstCaps *, unsigned);
  int (*structure_get_int)(const GstStructure *, const char *, int *);
  int (*structure_get_fraction)(const GstStructure *, const char *, int *, int *);
  const char *(*structure_get_string)(const GstStructure *, const char *);
  int (*buffer_map)(GstBufferX *, GstMapInfoX *, int);
  void (*buffer_unmap)(GstBufferX *, GstMapInfoX *);
  void (*object_set)(void *, const char *, ...);
  void (*error_free)(void *);
  GstSample *(*pull_sample)(GstElement *, uint64_t);
  GstSample *(*pull_preroll)(GstElement *, uint64_t);
  int (*is_eos)(GstElement *);
  GstVideoMetaX *(*get_video_meta)(GstBufferX *);
} G = {PTHREAD_ONCE_INIT};

static void load(void)
{
  void *gst = dlopen("libgstreamer-1.0.so.0", RTLD_NOW | RTLD_LOCAL);
  void *app = gst ? dlopen("libgstapp-1.0.so.0", RTLD_NOW | RTLD_LOCAL) : NULL;
  void *video = gst ? dlopen("libgstvideo-1.0.so.0", RTLD_NOW | RTLD_LOCAL) : NULL;
  if (!gst || !app) {
    return; /* no GStreamer: no video, nothing else changes */
  }
#  define SYM(lib, field, name) *(void **)&G.field = dlsym(lib, name)
  SYM(gst, init_check, "gst_init_check");
  SYM(gst, parse_launch, "gst_parse_launch");
  SYM(gst, bin_get_by_name, "gst_bin_get_by_name");
  SYM(gst, set_state, "gst_element_set_state");
  SYM(gst, get_state, "gst_element_get_state");
  SYM(gst, query_duration, "gst_element_query_duration");
  SYM(gst, seek_simple, "gst_element_seek_simple");
  SYM(gst, object_unref, "gst_object_unref");
  SYM(gst, mini_object_unref, "gst_mini_object_unref");
  SYM(gst, sample_get_buffer, "gst_sample_get_buffer");
  SYM(gst, sample_get_caps, "gst_sample_get_caps");
  SYM(gst, caps_from_string, "gst_caps_from_string");
  SYM(gst, caps_get_structure, "gst_caps_get_structure");
  SYM(gst, structure_get_int, "gst_structure_get_int");
  SYM(gst, structure_get_fraction, "gst_structure_get_fraction");
  SYM(gst, structure_get_string, "gst_structure_get_string");
  SYM(gst, buffer_map, "gst_buffer_map");
  SYM(gst, buffer_unmap, "gst_buffer_unmap");
  SYM(gst, object_set, "g_object_set");  /* GLib, a dependency of GStreamer */
  SYM(gst, error_free, "g_error_free");
  SYM(app, pull_sample, "gst_app_sink_try_pull_sample");
  SYM(app, pull_preroll, "gst_app_sink_try_pull_preroll");
  SYM(app, is_eos, "gst_app_sink_is_eos");
  if (video) {
    SYM(video, get_video_meta, "gst_buffer_get_video_meta");
  }
#  undef SYM
  void **f = (void **)&G.init_check;
  for (void **end = (void **)&G.get_video_meta; f < end; f++) {
    if (!*f) {
      return;
    }
  }
  G.ok = G.init_check(NULL, NULL, NULL) != 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The codec
 * \{ */

typedef struct GstMovie {
  GstElement *pipeline, *sink, *size;
  int w, h; /* output frame (NV12) */
  double frame_dur;
  bool playing;
} GstMovie;

static void gst_close(void *state)
{
  GstMovie *m = (GstMovie *)state;
  if (m->pipeline) {
    G.set_state(m->pipeline, GST_STATE_NULL);
  }
  if (m->sink) {
    G.object_unref(m->sink);
  }
  if (m->size) {
    G.object_unref(m->size);
  }
  if (m->pipeline) {
    G.object_unref(m->pipeline);
  }
  MEM_freeN(m);
}

/* A launch line reading `path` (quoted with gst_parse_launch's escapes),
 * then `tail` (from the closing quote on). */
static char *pipeline_desc(const char *path, const char *tail)
{
  static const char head[] = "filesrc location=\"";
  const size_t n = strlen(path), tail_n = strlen(tail) + 1;
  char *s = (char *)MEM_mallocN(sizeof(head) + 2 * n + tail_n, "gst pipeline");
  char *p = s;
  memcpy(p, head, sizeof(head) - 1);
  p += sizeof(head) - 1;
  for (size_t i = 0; i < n; i++) {
    if (path[i] == '"' || path[i] == '\\') {
      *p++ = '\\';
    }
    *p++ = path[i];
  }
  memcpy(p, tail, tail_n);
  return s;
}

/* The pipeline of a launch line, and its appsink ("sink"). */
static GstElement *pipeline_open(const char *desc, GstElement **r_sink)
{
  void *err = NULL;
  GstElement *pipeline = G.parse_launch(desc, &err);
  if (err) {
    G.error_free(err);
  }
  *r_sink = pipeline ? G.bin_get_by_name(pipeline, "sink") : NULL;
  return pipeline;
}

static void *gst_open(const char *path, const uint8_t *mem, size_t size, BKImAnimInfo *r_info)
{
  (void)mem;
  (void)size;
  pthread_once(&G.once, load);
  if (!path || !G.ok) {
    return NULL;
  }
  GstMovie *m = (GstMovie *)MEM_callocN(sizeof(GstMovie), "GstMovie");
  char *desc = pipeline_desc(path,
                             "\" ! decodebin ! videoconvert ! videoscale ! capsfilter name=size "
                             "caps=video/x-raw,format=NV12 ! appsink name=sink sync=false "
                             "max-buffers=2 enable-last-sample=false");
  m->pipeline = pipeline_open(desc, &m->sink);
  MEM_freeN(desc);
  m->size = m->pipeline ? G.bin_get_by_name(m->pipeline, "size") : NULL;
  /* preroll (paused): the first frame tells the size; no decoder, an error */
  int st = 0;
  if (!m->sink || !m->size || G.set_state(m->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE ||
      G.get_state(m->pipeline, &st, NULL, 10 * GST_SECOND) == GST_STATE_CHANGE_FAILURE ||
      st != GST_STATE_PAUSED)
  {
    gst_close(m);
    return NULL;
  }
  GstSample *pre = G.pull_preroll(m->sink, 0);
  const GstStructure *s = pre ? G.caps_get_structure(G.sample_get_caps(pre), 0) : NULL;
  int w = 0, h = 0, fn = 0, fd = 1, pn = 1, pd = 1;
  const char *colorimetry = NULL;
  if (s) {
    G.structure_get_int(s, "width", &w);
    G.structure_get_int(s, "height", &h);
    G.structure_get_fraction(s, "framerate", &fn, &fd);
    G.structure_get_fraction(s, "pixel-aspect-ratio", &pn, &pd);
    colorimetry = G.structure_get_string(s, "colorimetry");
  }
  r_info->format = colorimetry && strstr(colorimetry, "709") ? BUI_PIXMAP_NV12_BT709 :
                   colorimetry && strstr(colorimetry, "601") ? BUI_PIXMAP_NV12_BT601 :
                   h >= 720                                   ? BUI_PIXMAP_NV12_BT709 :
                                                                BUI_PIXMAP_NV12_BT601;
  if (pre) {
    G.mini_object_unref(pre);
  }
  if (w <= 0 || h <= 0) {
    gst_close(m);
    return NULL;
  }
  m->w = w;
  m->h = h;
  m->frame_dur = fn > 0 && fd > 0 ? (double)fd / fn : 1.0 / 30.0;
  int64_t dur = 0;
  r_info->duration = G.query_duration(m->pipeline, GST_FORMAT_TIME, &dur) && dur > 0 ?
                         (double)dur / GST_SECOND :
                         0.0;
  r_info->src_w = pn > 0 && pd > 0 && pn != pd ? (int)((double)w * pn / pd + 0.5) : w;
  r_info->src_h = h;
  r_info->loops = 1;
  r_info->sound = true; /* decodebin doesn't say here: the sound pipeline finds out */
  return m;
}

static bool gst_output(void *state, int w, int h, BKImAnimInfo *r_info)
{
  GstMovie *m = (GstMovie *)state;
  /* even, and rows of 4-byte multiples (videoconvert's NV12 stride) */
  const int out_w = BK_MAX(4, w & ~3), out_h = BK_MAX(2, h & ~1);
  char caps_s[128];
  snprintf(caps_s, sizeof(caps_s),
           "video/x-raw,format=NV12,width=%d,height=%d,pixel-aspect-ratio=1/1", out_w, out_h);
  GstCaps *caps = G.caps_from_string(caps_s);
  if (!caps) {
    return false;
  }
  G.object_set(m->size, "caps", caps, NULL);
  G.mini_object_unref(caps);
  if (!m->playing) { /* the prerolled frame is at the old size: preroll again */
    G.seek_simple(m->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0);
  }
  m->w = out_w;
  m->h = out_h;
  r_info->w = out_w;
  r_info->h = out_h;
  return true;
}

/* The sample's planes into `dst` (NV12, its size); GstVideoMeta strides
 * where the buffer has them. */
static bool copy_frame(GstBufferX *buf, BKImBuf *dst)
{
  GstMapInfoX map;
  if (!G.buffer_map(buf, &map, GST_MAP_READ)) {
    return false;
  }
  const GstVideoMetaX *meta = G.get_video_meta ? G.get_video_meta(buf) : NULL;
  size_t off[2] = {0, (size_t)dst->x * dst->y};
  int stride[2] = {dst->x, dst->x};
  if (meta && meta->n_planes == 2) {
    off[0] = meta->offset[0];
    off[1] = meta->offset[1];
    stride[0] = meta->stride[0];
    stride[1] = meta->stride[1];
  }
  const bool ok = stride[0] >= dst->x && stride[1] >= dst->x &&
                  off[0] + (size_t)stride[0] * (dst->y - 1) + dst->x <= map.size &&
                  off[1] + (size_t)stride[1] * (dst->y / 2 - 1) + dst->x <= map.size;
  if (ok) {
    uint8_t *y = (uint8_t *)dst->rect, *uv = y + (size_t)dst->x * dst->y;
    for (int r = 0; r < dst->y; r++) {
      memcpy(y + (size_t)r * dst->x, map.data + off[0] + (size_t)r * stride[0], (size_t)dst->x);
    }
    for (int r = 0; r < dst->y / 2; r++) {
      memcpy(uv + (size_t)r * dst->x, map.data + off[1] + (size_t)r * stride[1], (size_t)dst->x);
    }
  }
  G.buffer_unmap(buf, &map);
  return ok;
}

static int gst_next(void *state, BKImBuf *dst, double *r_pts, double *r_dur)
{
  GstMovie *m = (GstMovie *)state;
  if (!m->playing) {
    m->playing = G.set_state(m->pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  }
  for (int stale = 0; stale < 8; stale++) {
    GstSample *sample = G.pull_sample(m->sink, 5 * GST_SECOND);
    if (!sample) {
      return G.is_eos(m->sink) ? 0 : -1;
    }
    /* frames negotiated before the last size change: skip */
    const GstStructure *s = G.caps_get_structure(G.sample_get_caps(sample), 0);
    int w = 0, h = 0;
    G.structure_get_int(s, "width", &w);
    G.structure_get_int(s, "height", &h);
    GstBufferX *buf = G.sample_get_buffer(sample);
    const bool fits = w == dst->x && h == dst->y && buf;
    const bool ok = fits && copy_frame(buf, dst);
    if (ok) {
      *r_pts = buf->pts != GST_NONE ? (double)buf->pts / GST_SECOND : -1.0;
      *r_dur = buf->duration != GST_NONE ? (double)buf->duration / GST_SECOND : m->frame_dur;
    }
    G.mini_object_unref(sample);
    if (ok) {
      return 1;
    }
    if (fits) {
      return -1;
    }
  }
  return -1;
}

static bool gst_seek(void *state, double t)
{
  GstMovie *m = (GstMovie *)state;
  return G.seek_simple(m->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                       (int64_t)(t * GST_SECOND)) != 0;
}

static size_t gst_bytes(const void *state)
{
  const GstMovie *m = (const GstMovie *)state;
  return sizeof(*m) + (size_t)m->w * m->h * 3; /* the appsink's two frames (an estimate) */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sound: a second pipeline, float at the device's rate and channels
 * \{ */

typedef struct GstSound {
  GstElement *pipeline, *sink;
  int channels;
  bool playing;
  float *buf; /* the last block */
  size_t cap; /* floats */
} GstSound;

static void gst_audio_close(void *state)
{
  GstSound *s = (GstSound *)state;
  if (s->pipeline) {
    G.set_state(s->pipeline, GST_STATE_NULL);
  }
  if (s->sink) {
    G.object_unref(s->sink);
  }
  if (s->pipeline) {
    G.object_unref(s->pipeline);
  }
  if (s->buf) {
    MEM_freeN(s->buf);
  }
  MEM_freeN(s);
}

static void *gst_audio_open(const char *path, int rate, int channels, int *r_rate, int *r_channels)
{
  pthread_once(&G.once, load);
  if (!path || !G.ok) {
    return NULL;
  }
  char tail[256];
  snprintf(tail, sizeof(tail),
           "\" ! decodebin ! audioconvert ! audioresample ! "
           "audio/x-raw,format=F32LE,layout=interleaved,rate=%d,channels=%d ! "
           "appsink name=sink sync=false max-buffers=8 enable-last-sample=false",
           rate, channels);
  char *desc = pipeline_desc(path, tail);
  GstSound *s = (GstSound *)MEM_callocN(sizeof(GstSound), "GstSound");
  s->pipeline = pipeline_open(desc, &s->sink);
  MEM_freeN(desc);
  /* preroll: a file without sound never fills the sink (or fails at once) */
  int st = 0;
  if (!s->sink || G.set_state(s->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE ||
      G.get_state(s->pipeline, &st, NULL, 3 * GST_SECOND) == GST_STATE_CHANGE_FAILURE ||
      st != GST_STATE_PAUSED)
  {
    gst_audio_close(s);
    return NULL;
  }
  s->channels = channels;
  *r_rate = rate; /* the caps above: audioresample and audioconvert make it so */
  *r_channels = channels;
  return s;
}

static int gst_audio_read(void *state, const float **r_data, double *r_pts)
{
  GstSound *s = (GstSound *)state;
  if (!s->playing) {
    s->playing = G.set_state(s->pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  }
  GstSample *sample = G.pull_sample(s->sink, 2 * GST_SECOND);
  if (!sample) {
    return G.is_eos(s->sink) ? 0 : -1;
  }
  GstBufferX *buf = G.sample_get_buffer(sample);
  GstMapInfoX map;
  int frames = -1;
  if (buf && G.buffer_map(buf, &map, GST_MAP_READ)) {
    const size_t n = map.size / sizeof(float);
    if (n > s->cap) {
      if (s->buf) {
        MEM_freeN(s->buf);
      }
      s->buf = (float *)MEM_mallocN(sizeof(float) * n, "GstSound block");
      s->cap = n;
    }
    memcpy(s->buf, map.data, n * sizeof(float));
    frames = (int)(n / (size_t)s->channels);
    *r_pts = buf->pts != GST_NONE ? (double)buf->pts / GST_SECOND : -1.0;
    G.buffer_unmap(buf, &map);
  }
  G.mini_object_unref(sample);
  *r_data = s->buf;
  return frames;
}

static bool gst_audio_seek(void *state, double t)
{
  GstSound *s = (GstSound *)state;
  return G.seek_simple(s->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                       (int64_t)(t * GST_SECOND)) != 0;
}

const BKImAnimCodec imb_movie_gst = {
    "gstreamer", true, imb_is_a_movie, gst_open, gst_output, gst_next, gst_seek, gst_bytes,
    gst_close, gst_audio_open, gst_audio_read, gst_audio_seek, gst_audio_close};

/** \} */

#endif /* BUI_VIDEO_GST */
