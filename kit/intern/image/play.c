/* The one clock of everything that plays: GIF, animated WebP and video,
 * owned by a cache entry (a shared <img>, or a <video>'s private entry).
 *
 * The entry shows one frame; this keeps the decoder and the next frame,
 * decoded ahead on the workers (one step request at a time), and says when
 * the next frame is due (each frame's own duration over the playback rate,
 * GIF and WebP play counts, a video's loop). It only moves on when the frame
 * on screen was drawn: a window that isn't rendering (hidden, minimized)
 * costs one decode, then nothing, and an entry nobody shows is not stepped
 * at all (cache.c). Movies decode into the frame the screen no longer shows
 * (two frames, no allocation per frame).
 *
 * Controls (pause, seek, rate, loop) are the <video> element's; a seek's
 * frame shows as soon as it is decoded, paused or not.
 *
 * A movie with sound follows the sound instead (Blender's AV-sync): a frame
 * shows when the sound reaches its time (video.c sets the clock: the audio
 * device's position, audio/audio.c), and a picture that fell behind (it was
 * off screen while the sound played on) seeks to where the sound is.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <float.h>
#include <math.h>

#include "MEM_guardedalloc.h"

#include "image.h"

struct BKImPlay {
  BKImAnim *anim; /* NULL before the first frame and while a step request has it */
  BKImBuf *next;  /* the frame after the one shown (NULL while decoding) */
  double next_pts, next_dur;
  bool next_wrapped; /* `next` starts another play */
  bool next_seek;    /* `next` is a seek's frame: shown at once */
  BKImBuf *spare;    /* movies: the frame the screen let go of, decoded into next */
  double pts, dur;   /* the shown frame's time and duration (seconds) */
  double due;        /* when it is over (bk_time seconds; 0: not timed yet) */
  int plays;         /* plays finished */
  int loops;         /* the play count (0 = forever): the file's, or set */
  int loops_set;     /* -1: the file's */
  int file_loops, src_w, src_h;
  double duration;   /* seconds, 0 when unknown */
  double rate;
  double seek;       /* a seek to request (< 0: none) */
  double seek_t;     /* the last seek's time, while its frame isn't shown */
  bool seeking;
  unsigned seeks;    /* seeks asked for; a step taken before the last one is stale */
  unsigned step_seeks; /* `seeks` when the step out was taken */
  bool step_seek;      /* the step out seeks */
  bool loaded, failed, paused, drawn;
  bool finished; /* the last play's last frame shows */
  const BKImAnimCodec *codec; /* what reads the file (its sound too) */
  bool sound;                 /* the file has an audio track */
  double (*clock)(void *data); /* the movie time heard (< 0: none now) */
  void *clock_data;
};

BKImPlay *imb_play_new(bool paused, int loops)
{
  BKImPlay *p = (BKImPlay *)MEM_callocN(sizeof(BKImPlay), "BKImPlay");
  p->paused = paused;
  p->loops_set = loops;
  p->loops = BK_MAX(0, loops);
  p->rate = 1.0;
  p->seek = -1.0;
  return p;
}

void imb_play_free(BKImPlay *p)
{
  if (p) {
    imb_anim_free(p->anim);
    BK_imbuf_free(p->next);
    BK_imbuf_free(p->spare);
    MEM_freeN(p);
  }
}

size_t imb_play_bytes(const BKImPlay *p)
{
  return sizeof(*p) + (p->anim ? imb_anim_bytes(p->anim) : 0) + (p->next ? p->next->size : 0) +
         (p->spare ? p->spare->size : 0);
}

void imb_play_start(BKImPlay *p, BKImAnim *anim, double pts, double dur)
{
  p->anim = anim;
  p->codec = imb_anim_backend(anim);
  p->loaded = true;
  p->pts = pts;
  p->dur = dur;
  const BKImAnimInfo *info = imb_anim_info(anim);
  p->file_loops = info->loops;
  p->src_w = info->src_w;
  p->src_h = info->src_h;
  p->duration = info->duration;
  p->sound = info->sound && p->codec->audio_open;
  if (p->loops_set < 0) {
    p->loops = p->file_loops;
  }
}

void imb_play_fail(BKImPlay *p)
{
  p->failed = true;
}

void imb_play_recycle(BKImPlay *p, BKImBuf *old)
{
  if (old && old->stream && !p->spare) {
    p->spare = old; /* the next decode goes into it */
  }
  else {
    BK_imbuf_free(old);
  }
}

BKImAnim *imb_play_take_step(BKImPlay *p, BKImBuf **r_reuse, double *r_seek)
{
  if (!p->anim || p->failed || (p->seek < 0.0 && (p->next || p->finished))) {
    return NULL;
  }
  BKImAnim *a = p->anim;
  p->anim = NULL;
  *r_reuse = p->spare;
  p->spare = NULL;
  *r_seek = p->seek;
  p->step_seek = p->seek >= 0.0;
  p->step_seeks = p->seeks;
  p->seek = -1.0;
  return a;
}

void imb_play_step_done(BKImPlay *p, BKImAnim *anim, BKImBuf *next, double pts, double dur,
                        bool wrapped)
{
  p->anim = anim;
  if (p->step_seeks != p->seeks) {
    imb_play_recycle(p, next); /* decoded before a seek: stale */
    return;
  }
  if (!next) {
    p->finished = true; /* a decoding error: stay on this frame */
    return;
  }
  imb_play_recycle(p, p->next);
  p->next = next;
  p->next_pts = pts;
  p->next_dur = dur;
  p->next_wrapped = wrapped;
  p->next_seek = p->step_seek;
}

void imb_play_step_cancelled(BKImPlay *p, BKImAnim *anim, BKImBuf *reuse)
{
  p->anim = anim; /* stepped again when it is wanted */
  imb_play_recycle(p, reuse);
  if (p->step_seek && p->step_seeks == p->seeks && p->seek < 0.0) {
    p->seek = p->seek_t; /* the seek is still to do */
  }
}

void imb_play_drawn(BKImPlay *p, double now)
{
  p->drawn = true;
  if (p->due == 0.0) {
    p->due = now + p->dur / p->rate; /* the clock starts when it first shows */
  }
}

/* The sound's time (< 0: none: the frames keep their own time). */
static double heard(const BKImPlay *p)
{
  return p->clock ? p->clock(p->clock_data) : -1.0;
}

/* The sound's time held against a frame at `pts`. Only within a window of the loop boundary does the
 * sound count from the neighbouring loop (it just wrapped while the frame is at the end, or the
 * frame just wrapped and it hasn't): a nearest-cycle remainder() read a sound far ahead as far behind,
 * so the catch-up seek never ran; past the window it is ahead, and the seek goes where it is. */
double imb_play_heard_for(int loops, double duration, double c, double pts)
{
  if (loops != 0 || duration <= 0.0) {
    return c;
  }
  const double window = BK_MIN(BK_PLAY_WRAP_WINDOW, duration * 0.5);
  const double d = c - pts;
  if (d > duration - window) {
    return c - duration; /* the frame just came round, the sound hasn't */
  }
  if (d < window - duration) {
    return c + duration; /* the sound just came round, the frame is at the end */
  }
  return c;
}

static double heard_for(const BKImPlay *p, double c, double pts)
{
  return imb_play_heard_for(p->loops, p->duration, c, pts);
}

/* With sound: when the shown frame is over (the sound reached the next one). */
static double sound_wait(const BKImPlay *p, double c)
{
  if (p->next_wrapped) { /* the last frame: until its end, or the sound came round */
    return p->loops == 0 && c < p->pts ? 0.0 : p->pts + p->dur - c;
  }
  return p->next_pts - heard_for(p, c, p->next_pts);
}

double imb_play_due(const BKImPlay *p, double now)
{
  if (p->next && p->next_seek) {
    return DBL_MIN; /* now */
  }
  if (!p->drawn || p->finished || p->paused || !p->next) {
    return 0.0;
  }
  const double c = heard(p);
  if (c >= 0.0) {
    const double wait = sound_wait(p, c);
    return now + BK_CLAMP(wait, 0.0, 0.5); /* looked at again within half a second */
  }
  return p->due;
}

BKImBuf *imb_play_advance(BKImPlay *p, double now)
{
  const double due = imb_play_due(p, now);
  if (due == 0.0 || now < due) {
    return NULL;
  }
  const double c = p->next_seek ? -1.0 : heard(p);
  if (c >= 0.0) {
    /* (imb_play_due above returned `now + wait`: here the sound has reached the frame) */
    if (!p->next_wrapped && heard_for(p, c, p->next_pts) - p->next_pts > 0.5) {
      imb_play_seek(p, heard_for(p, c, p->next_pts)); /* far behind the sound: go where it is */
      return NULL;
    }
  }
  if (!p->next_seek && p->next_wrapped && p->loops > 0 && p->plays + 1 >= p->loops) {
    p->finished = true; /* the last frame of the last play stays */
    imb_play_recycle(p, p->next);
    p->next = NULL;
    return NULL;
  }
  BKImBuf *frame = p->next;
  p->next = NULL;
  p->pts = p->next_pts;
  p->dur = p->next_dur;
  if (p->next_seek) {
    p->due = 0.0; /* timed from when it shows */
    p->next_seek = false;
    p->seeking = false;
  }
  else {
    p->plays += p->next_wrapped;
    /* keep the beat, unless far behind (paused, slow decodes, the sound kept
     * it): restart it */
    p->due = c >= 0.0 || now - p->due > 0.25 ? now + p->dur / p->rate : p->due + p->dur / p->rate;
  }
  p->drawn = false;
  return frame;
}

void imb_play_pause(BKImPlay *p, bool paused)
{
  if (p->paused && !paused) {
    p->due = 0.0; /* resumed: the frame gets its time again */
  }
  if (!paused && p->finished) { /* playing an ended video starts it over */
    imb_play_seek(p, 0.0);
  }
  p->paused = paused;
}

void imb_play_seek(BKImPlay *p, double t)
{
  const double duration = imb_play_duration(p);
  p->seek = p->seek_t = BK_CLAMP(t, 0.0, duration > 0.0 ? duration : t);
  p->seeking = true;
  p->seeks++;
  p->finished = false;
  p->plays = 0;
  imb_play_recycle(p, p->next); /* decoded before the seek */
  p->next = NULL;
  p->next_seek = false;
}

void imb_play_set_rate(BKImPlay *p, double rate)
{
  p->rate = BK_CLAMP(rate, 1.0 / 16.0, 16.0);
}

void imb_play_set_loops(BKImPlay *p, int loops)
{
  p->loops_set = loops;
  p->loops = loops >= 0 ? loops : p->file_loops;
  if (p->loops == 0) {
    p->finished = false; /* an ended video looping again goes on from the top */
  }
}

BKImPlayState imb_play_state(const BKImPlay *p)
{
  return p->failed    ? BK_PLAY_ERROR :
         !p->loaded   ? BK_PLAY_LOADING :
         p->finished  ? BK_PLAY_ENDED :
         p->paused    ? BK_PLAY_PAUSED :
                        BK_PLAY_PLAYING;
}

double imb_play_time(const BKImPlay *p)
{
  return p->seeking ? p->seek_t : p->pts;
}

double imb_play_duration(const BKImPlay *p)
{
  return p->duration;
}

void imb_play_set_clock(BKImPlay *p, double (*clock)(void *data), void *data)
{
  p->clock = clock;
  p->clock_data = data;
}

const BKImAnimCodec *imb_play_codec(const BKImPlay *p)
{
  return p->sound ? p->codec : NULL;
}

bool imb_play_size(const BKImPlay *p, int *r_w, int *r_h)
{
  *r_w = p->src_w;
  *r_h = p->src_h;
  return p->loaded;
}
