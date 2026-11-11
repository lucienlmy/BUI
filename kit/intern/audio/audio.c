/* One movie's sound: its audio track read by the codec that plays its
 * frames, converted to the device's rate and channels, scaled by the volume,
 * looped and sought with the video, and the clock the frames follow (as
 * Blender's playback syncs the picture to the sound: "AV-sync").
 *
 * The clock is exact to the sample: each block written to the device is
 * remembered as a segment (where it starts in the device's stream, the movie
 * time it holds, its length), and the device says how much of its stream has
 * been heard; the segment under that point gives the movie time heard now.
 *
 * Threads: the device's thread runs fill(), the only user of the reader; the
 * UI thread changes the controls and reads the clock, under one lock that
 * fill() holds only to copy them and to add a segment.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <math.h>
#include <string.h>

#include "BLI_threads.h"
#include "MEM_guardedalloc.h"

#include "audio.h"

#define MAX_CHANNELS 8
#define SEGMENTS 64

/* A run of the device's stream that holds movie sound. */
typedef struct Segment {
  double dev;   /* where it starts in the device's stream (seconds) */
  double media; /* the movie time there */
  double len;   /* seconds */
} Segment;

struct BKAudio {
  const BKImAnimCodec *codec;
  char *path;
  BKAudioDevice *dev;

  /* -- the controls: the UI thread sets them (under `lock`) ----------------------- */
  ThreadMutex lock;
  bool playing, loop;
  float gain;
  double seek;    /* a seek to make (< 0: none) */
  unsigned seeks; /* seeks asked for: the clock forgets what was before */

  /* -- the clock: fill() adds, the UI thread reads (under `lock`) ------------------ */
  Segment seg[SEGMENTS];
  int nseg;       /* the newest SEGMENTS at most, oldest first */
  bool has_track; /* the reader opened */
  bool drained;   /* the track ended (not looping): the clock stops after the last segment */

  /* -- the reader and the conversion: the device's thread only --------------------- */
  void *reader;
  bool tried; /* the reader was opened (or failed to) */
  bool ended;
  int src_rate, src_channels;
  const float *block; /* the reader's block, `pos` of `n` frames used */
  int pos, n;
  double block_t;     /* the movie time of block[0] */
  double skip_to;     /* after a seek: frames before this time are dropped */
  float prev[MAX_CHANNELS], cur[MAX_CHANNELS]; /* the frames to interpolate between */
  double prev_t, cur_t; /* their movie times */
  double frac;        /* where the next output frame is between prev and cur (0..1) */
  bool primed;        /* prev / cur hold frames (false after a seek, a loop) */
  unsigned seeks_done;
  double written; /* seconds of the device's stream fill() has written */
};

static void fill(void *data, float *out, int frames, int rate, int channels);

BKAudio *bk_audio_new(const BKImAnimCodec *codec, const char *path)
{
  if (!codec || !codec->audio_open || !path) {
    return NULL;
  }
  BKAudio *a = (BKAudio *)MEM_callocN(sizeof(BKAudio), "BKAudio");
  a->codec = codec;
  const size_t len = strlen(path);
  a->path = (char *)MEM_mallocN(len + 1, "audio path");
  memcpy(a->path, path, len + 1);
  a->gain = 1.0f;
  a->seek = -1.0;
  BLI_mutex_init(&a->lock);
  a->dev = bk_audio_device_open(fill, a);
  if (!a->dev) {
    bk_audio_free(a);
    return NULL;
  }
  return a;
}

void bk_audio_free(BKAudio *a)
{
  if (!a) {
    return;
  }
  if (a->dev) {
    bk_audio_device_close(a->dev); /* fill() has returned for good */
  }
  if (a->reader) {
    a->codec->audio_close(a->reader);
  }
  BLI_mutex_destroy(&a->lock);
  MEM_freeN(a->path);
  MEM_freeN(a);
}

void bk_audio_play(BKAudio *a, bool playing)
{
  if (a) {
    BLI_mutex_lock(&a->lock);
    a->playing = playing;
    BLI_mutex_unlock(&a->lock);
  }
}

void bk_audio_seek(BKAudio *a, double t)
{
  if (a) {
    BLI_mutex_lock(&a->lock);
    a->seek = t > 0.0 ? t : 0.0;
    a->seeks++;
    a->nseg = 0; /* what the device still plays is from before: not the clock */
    a->drained = false;
    BLI_mutex_unlock(&a->lock);
  }
}

void bk_audio_set_loop(BKAudio *a, bool loop)
{
  if (a) {
    BLI_mutex_lock(&a->lock);
    a->loop = loop;
    BLI_mutex_unlock(&a->lock);
  }
}

void bk_audio_set_gain(BKAudio *a, float gain)
{
  if (a) {
    BLI_mutex_lock(&a->lock);
    a->gain = gain < 0.0f ? 0.0f : gain > 1.0f ? 1.0f : gain;
    BLI_mutex_unlock(&a->lock);
  }
}

bool bk_audio_has_track(BKAudio *a)
{
  if (!a) {
    return false;
  }
  BLI_mutex_lock(&a->lock);
  const bool has = a->has_track;
  BLI_mutex_unlock(&a->lock);
  return has;
}

double bk_audio_clock(BKAudio *a)
{
  if (!a) {
    return -1.0;
  }
  const double heard = bk_audio_device_played(a->dev);
  double t = -1.0;
  BLI_mutex_lock(&a->lock);
  if (a->playing) {
    for (int i = a->nseg - 1; i >= 0; i--) {
      const Segment *s = &a->seg[i];
      if (s->dev <= heard) {
        const double into = heard - s->dev;
        /* past the last segment of an ended track: the frames go on by themselves */
        t = into < s->len ? s->media + into : (a->drained && i == a->nseg - 1) ? -1.0 : s->media + s->len;
        break;
      }
    }
  }
  BLI_mutex_unlock(&a->lock);
  return t;
}

/* -------------------------------------------------------------------- */
/** \name fill(): on the device's thread
 * \{ */

/* The clock: `len` seconds of sound at movie time `media` start at the end
 * of what was written. Joined to the last segment when it goes straight on. */
static void add_segment(BKAudio *a, double media, double len, unsigned seeks)
{
  BLI_mutex_lock(&a->lock);
  if (seeks == a->seeks) { /* not from before a seek asked for meanwhile */
    Segment *last = a->nseg ? &a->seg[a->nseg - 1] : NULL;
    if (last && fabs(last->dev + last->len - a->written) < 1e-6 &&
        fabs(last->media + last->len - media) < 1e-3)
    {
      last->len += len;
    }
    else {
      if (a->nseg == SEGMENTS) {
        memmove(a->seg, a->seg + 1, sizeof(Segment) * (SEGMENTS - 1));
        a->nseg--;
      }
      a->seg[a->nseg++] = (Segment){a->written, media, len};
    }
    a->has_track = true;
  }
  BLI_mutex_unlock(&a->lock);
}

/* The next frame of the track into `out` (src_channels), with its time;
 * false at the end (looping starts it over: `*r_jump`). */
static bool next_frame(BKAudio *a, float *out, double *r_t, bool loop, bool *r_jump)
{
  for (int tries = 0; tries < 64; tries++) {
    if (a->pos < a->n) {
      const double t = a->block_t + (double)a->pos / a->src_rate;
      const float *f = a->block + (size_t)a->pos * a->src_channels;
      a->pos++;
      if (t + 0.5 / a->src_rate < a->skip_to) {
        continue; /* before the seek's time: the reader starts a little early */
      }
      memcpy(out, f, sizeof(float) * (size_t)a->src_channels);
      *r_t = t;
      return true;
    }
    double pts = -1.0;
    const int got = a->ended ? 0 : a->codec->audio_read(a->reader, &a->block, &pts);
    if (got > 0) {
      a->block_t = pts >= 0.0 ? pts : a->block_t + (double)a->n / a->src_rate;
      a->n = got;
      a->pos = 0;
      continue;
    }
    a->n = a->pos = 0;
    if (got < 0 || !loop || !a->codec->audio_seek(a->reader, 0.0)) {
      a->ended = true;
      return false;
    }
    a->block_t = 0.0; /* looped: from the top */
    a->skip_to = 0.0;
    *r_jump = true;
  }
  return false;
}

/* One frame from `src` channels to `dst` (Blender's downmix weights for 5.1
 * to stereo; otherwise the first channels, mono to both sides). */
static void map_channels(const float *in, int src, float *out, int dst)
{
  if (src == dst) {
    memcpy(out, in, sizeof(float) * (size_t)dst);
    return;
  }
  memset(out, 0, sizeof(float) * (size_t)dst);
  if (src == 1) {
    out[0] = in[0];
    if (dst > 1) {
      out[1] = in[0];
    }
  }
  else if (dst == 1) {
    out[0] = 0.5f * (in[0] + in[1]);
  }
  else if (src >= 6 && dst == 2) { /* FL FR FC LFE BL BR */
    const float c = 0.7071f, k = 1.0f / (1.0f + 2.0f * 0.7071f);
    out[0] = k * (in[0] + c * in[2] + c * in[4]);
    out[1] = k * (in[1] + c * in[2] + c * in[5]);
  }
  else {
    for (int c = 0; c < dst && c < src; c++) {
      out[c] = in[c];
    }
  }
}

/* The reader, opened at the device's format on first use. */
static bool reader_ready(BKAudio *a, int rate, int channels)
{
  if (!a->tried) {
    a->tried = true;
    a->reader = a->codec->audio_open(a->path, rate, channels, &a->src_rate, &a->src_channels);
    if (a->reader && (a->src_rate <= 0 || a->src_channels <= 0 || a->src_channels > MAX_CHANNELS)) {
      a->codec->audio_close(a->reader);
      a->reader = NULL;
    }
  }
  return a->reader != NULL;
}

static void fill(void *data, float *out, int frames, int rate, int channels)
{
  BKAudio *a = (BKAudio *)data;
  memset(out, 0, sizeof(float) * (size_t)frames * (size_t)channels);
  BLI_mutex_lock(&a->lock);
  const bool playing = a->playing, loop = a->loop;
  const float gain = a->gain;
  const double seek = a->seek;
  const unsigned seeks = a->seeks;
  a->seek = -1.0;
  BLI_mutex_unlock(&a->lock);

  if (!reader_ready(a, rate, channels) || channels > MAX_CHANNELS) {
    a->written += (double)frames / rate;
    return;
  }
  if (seek >= 0.0) {
    a->ended = !a->codec->audio_seek(a->reader, seek);
    a->n = a->pos = 0;
    a->skip_to = seek;
    a->block_t = seek;
    a->primed = false;
  }
  if (seeks != a->seeks_done) {
    a->seeks_done = seeks;
    a->primed = false;
  }
  const double step = (double)a->src_rate / rate;
  int done = 0;
  while (playing && done < frames) {
    /* a run of frames that goes straight on (a loop starts the next one) */
    const int start = done;
    double t0 = -1.0;
    bool jump = false;
    while (done < frames && !jump) {
      if (!a->primed) {
        double t;
        if (!next_frame(a, a->cur, &t, loop, &jump)) {
          break;
        }
        memcpy(a->prev, a->cur, sizeof(a->cur));
        a->prev_t = a->cur_t = t;
        a->frac = 0.0;
        a->primed = true;
        jump = false;
      }
      while (a->frac >= 1.0) {
        double t;
        bool looped = false;
        memcpy(a->prev, a->cur, sizeof(a->cur));
        a->prev_t = a->cur_t;
        if (!next_frame(a, a->cur, &t, loop, &looped)) {
          a->primed = false;
          break;
        }
        a->frac -= 1.0;
        a->cur_t = t;
        if (looped) { /* from the top: no blending across the seam */
          memcpy(a->prev, a->cur, sizeof(a->cur));
          a->prev_t = t;
          a->frac = 0.0;
          jump = true;
          break;
        }
      }
      if (!a->primed || jump) {
        break;
      }
      if (t0 < 0.0) {
        t0 = a->prev_t + a->frac / a->src_rate; /* between prev and cur */
      }
      float mixed[MAX_CHANNELS];
      const float f = (float)a->frac;
      for (int c = 0; c < a->src_channels; c++) {
        mixed[c] = a->prev[c] + (a->cur[c] - a->prev[c]) * f;
      }
      float *o = out + (size_t)done * channels;
      map_channels(mixed, a->src_channels, o, channels);
      for (int c = 0; c < channels; c++) {
        o[c] *= gain;
      }
      a->frac += step;
      done++;
    }
    if (done > start && t0 >= 0.0) {
      const double len = (double)(done - start) / rate;
      add_segment(a, t0, len, seeks);
      a->written += len;
    }
    if (!a->primed && !jump) {
      break; /* the end, or an error: silence from here */
    }
  }
  a->written += (double)(frames - done) / rate; /* the silence */
  if (a->ended) {
    BLI_mutex_lock(&a->lock);
    a->drained = true;
    BLI_mutex_unlock(&a->lock);
  }
}

/** \} */
