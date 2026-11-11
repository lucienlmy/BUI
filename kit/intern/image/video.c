/* <video>: a private cache entry (cache.c: the same requests, workers and
 * visibility tracking as images) with its own clock (play.c: pause, seek,
 * rate, loop), shown on "video" and "image" nodes and canvas items, or
 * copied into a texture for Dear ImGui. The "video" node draws Blender-style
 * controls over it: a play/pause button and a scrubber with the time, like
 * the Timeline's playback controls and frame slider, and a speaker button
 * when the video has sound.
 *
 * Sound (audio/audio.c): the file's audio track plays on the default output
 * once the first frame is loaded, and the frames follow it. It follows the
 * controls (pause, seek, loop); at a rate other than 1 it pauses and the
 * frames keep their own time. It plays on while the picture is off screen.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "../audio/audio.h"
#include "image.h"
#include "kit_intern.h"

struct BKVideo {
  BKImEntry *entry;  /* frames and clock */
  BKImageRef poster; /* shown until the first frame */
  bool controls;
  bool scrubbing; /* the scrubber is dragged (the video was playing: `resume`) */
  bool resume;
  void (*fn)(BKVideo *v, int state, void *data);
  void *data;
  uint64_t tex_key; /* BK_video_texture: the frame last copied */
  BKAudio *audio;   /* the sound (NULL: none, or not loaded yet) */
  bool audio_tried;
  bool muted, loop;
  float volume;
  double rate;
};

static BKImPlay *play_of(const BKVideo *v)
{
  return bk_image_entry_play(v->entry);
}

/* The frames' clock: the sound heard. Sound behind the picture (the output
 * took its time to open: Bluetooth wakes up, a device changed) goes where the
 * picture is, as a picture behind the sound goes to the sound (play.c); until
 * that is heard the frames keep their own time. */
static double sound_clock(void *data)
{
  BKVideo *v = (BKVideo *)data;
  const double c = bk_audio_clock(v->audio);
  if (c < 0.0) {
    return -1.0;
  }
  const double t = imb_play_time(play_of(v)), dur = BK_video_duration(v);
  double behind = t - c;
  if (v->loop && dur > 0.0 && behind > 0.5 * dur) {
    behind -= dur; /* the sound came round first: it isn't behind */
  }
  if (behind > 0.25) {
    bk_audio_seek(v->audio, t + 0.1); /* ahead by about the time it takes to be heard */
    return -1.0;
  }
  return c;
}

/* The sound plays while the video plays at its own speed. */
static void sound_follow(BKVideo *v)
{
  bk_audio_play(v->audio, BK_video_state(v) == BK_VIDEO_PLAYING && v->rate == 1.0);}

static void sound_gain(BKVideo *v)
{
  bk_audio_set_gain(v->audio, v->muted ? 0.0f : v->volume);
}

/* Loaded: the sound opens where the frames are (its device and reader open on
 * the device's thread), and becomes their clock. */
static void sound_open(BKVideo *v)
{
  BKImPlay *p = play_of(v);
  v->audio_tried = true;
  v->audio = bk_audio_new(imb_play_codec(p), bk_image_entry_path(v->entry));  if (!v->audio) {
    return;
  }
  sound_gain(v);
  bk_audio_set_loop(v->audio, v->loop);
  bk_audio_seek(v->audio, imb_play_time(p));
  imb_play_set_clock(p, sound_clock, v);
}

static void heard(void *owner)
{
  BKVideo *v = (BKVideo *)owner;
  const int state = BK_video_state(v);
  if (!v->audio_tried && state != BK_VIDEO_LOADING && state != BK_VIDEO_ERROR) {
    sound_open(v);
  }
  sound_follow(v);
  if (v->fn) {
    v->fn(v, state, v->data);
  }
}

BKVideo *BK_video_new(const char *src)
{
  BKImageSource *s = bk_image_source_get(src);
  if (!s) {
    return NULL;
  }
  BKVideo *v = (BKVideo *)MEM_callocN(sizeof(BKVideo), "BKVideo");
  v->volume = 1.0f;
  v->rate = 1.0;
  /* autoplay with sound, played once (HTML: loop off) */
  v->entry = bk_image_entry_new(s, imb_play_new(false, 1), heard, v);
  bk_image_source_release(s); /* the entry keeps it */
  return v;
}

void BK_video_free(BKVideo *v)
{
  if (v) {
    bk_audio_free(v->audio); /* its device thread stops first */
    bk_image_entry_free(v->entry);
    bk_image_ref_clear(&v->poster);
    MEM_freeN(v);
  }
}

/* A control changed the clock: decode and redraw now, tell the listener. */
static void changed(BKVideo *v)
{
  bk_image_entry_changed(v->entry);
}

void BK_video_play(BKVideo *v)
{
  if (v) {
    if (BK_video_state(v) == BK_VIDEO_ENDED) {
      bk_audio_seek(v->audio, 0.0); /* an ended video starts over (imb_play_pause) */
    }
    imb_play_pause(play_of(v), false);
    changed(v);
  }
}

void BK_video_pause(BKVideo *v)
{
  if (v) {
    imb_play_pause(play_of(v), true);
    changed(v);
  }
}

void BK_video_seek(BKVideo *v, double seconds)
{
  if (v && isfinite(seconds)) {
    imb_play_seek(play_of(v), seconds);
    bk_audio_seek(v->audio, imb_play_time(play_of(v))); /* clamped to the duration */
    changed(v);
  }
}

void BK_video_set_rate(BKVideo *v, double rate)
{
  if (v && rate > 0.0 && isfinite(rate)) {
    const double was = v->rate;
    imb_play_set_rate(play_of(v), rate);
    v->rate = BK_CLAMP(rate, 1.0 / 16.0, 16.0);
    if (v->rate == 1.0 && was != 1.0) {
      bk_audio_seek(v->audio, BK_video_time(v)); /* the sound waited: from the frames' time */
    }
    sound_follow(v);
  }
}

void BK_video_set_loop(BKVideo *v, int loop)
{
  if (v) {
    v->loop = loop != 0;
    imb_play_set_loops(play_of(v), loop ? 0 : 1);
    bk_audio_set_loop(v->audio, v->loop);
    changed(v);
  }
}

void BK_video_set_poster(BKVideo *v, const char *src)
{
  if (v) {
    if (src && src[0]) {
      bk_image_ref_set_source(&v->poster, src);
    }
    else {
      bk_image_ref_clear(&v->poster);
    }
  }
}

void BK_video_set_controls(BKVideo *v, int on)
{
  if (v) {
    v->controls = on != 0;
    changed(v);
  }
}

int BK_video_state(const BKVideo *v)
{
  return v ? (int)imb_play_state(play_of(v)) : BK_VIDEO_ERROR;
}

double BK_video_time(const BKVideo *v)
{
  return v ? imb_play_time(play_of(v)) : 0.0;
}

double BK_video_duration(const BKVideo *v)
{
  return v ? imb_play_duration(play_of(v)) : 0.0;
}

int BK_video_size(const BKVideo *v, int *r_w, int *r_h)
{
  int w = 0, h = 0;
  const bool ok = v && bk_video_size(v, &w, &h);
  if (r_w) {
    *r_w = w;
  }
  if (r_h) {
    *r_h = h;
  }
  return ok;
}

void BK_video_set_muted(BKVideo *v, int muted)
{
  if (v) {
    v->muted = muted != 0;
    sound_gain(v);
    changed(v); /* the speaker button */
  }
}

int BK_video_muted(const BKVideo *v)
{
  return v && v->muted;
}

void BK_video_set_volume(BKVideo *v, double volume)
{
  if (v && isfinite(volume)) {
    v->volume = (float)BK_CLAMP(volume, 0.0, 1.0);
    sound_gain(v);
  }
}

double BK_video_volume(const BKVideo *v)
{
  return v ? v->volume : 0.0;
}

int BK_video_has_audio(const BKVideo *v)
{
  return v && bk_audio_has_track(v->audio);
}

void BK_video_set_listener(BKVideo *v, void (*fn)(BKVideo *v, int state, void *data), void *data)
{
  if (v) {
    v->fn = fn;
    v->data = data;
  }
}

bool bk_video_size(const BKVideo *v, int *r_w, int *r_h)
{
  return imb_play_size(play_of(v), r_w, r_h) || bk_image_ref_size(&v->poster, r_w, r_h);
}

const BKImBuf *bk_video_frame(BKVideo *v, const BKImageFit *target, const int rect[4])
{
  const BKImBuf *ib = bk_image_entry_get(v->entry, target, rect);
  if (!ib && v->poster.kind == BK_IMREF_SOURCE) {
    ib = bk_image_cache_get(v->poster.u.source, target, rect);
  }
  return ib;
}

int BK_video_texture(BKVideo *v, int tex, int max_w, int max_h)
{
  if (!v) {
    return 0;
  }
  const BKImageFit target = imb_fit_within(max_w, max_h);
  const BKImBuf *ib = bk_image_entry_get(v->entry, &target, NULL); /* shown by the host */
  if (!ib || ib->format == BUI_PIXMAP_ARGB) {
    return ib && !tex ? BK_imbuf_texture(ib, 0) : tex; /* the poster, or nothing yet */
  }
  int tw = 0, th = 0;
  if (tex && (!BUI_texture_size(tex, &tw, &th) || tw != ib->x || th != ib->y)) {
    BUI_texture_free(tex); /* resized: a new texture */
    tex = 0;
  }
  if (!tex) {
    tex = BK_imbuf_texture(ib, 0);
  }
  else if (ib->key != v->tex_key) {
    BK_imbuf_texture_update(ib, 0, tex); /* in place */
  }
  v->tex_key = tex ? ib->key : 0;
  return tex;
}

/* -------------------------------------------------------------------- */
/** \name The "video" node: the frame, fitted, and Blender-style controls
 * \{ */

void BK_node_set_video(BKNode *n, BKVideo *v, int fit)
{
  bk_image_ref_set_video(&n->image, v);
  n->image_fit = fit;
  bk_relayout(n);
  bk_invalidate(n);
}

static BKVideo *node_video(const BKNode *n)
{
  return n->image.kind == BK_IMREF_VIDEO ? n->image.u.video : NULL;
}

/* The controls bar along the bottom: the play button, the scrubber, and the
 * speaker (mute) when the video has sound (its width 0 when not). */
static bool controls_rects(const BKNode *n, BKRect *r_button, BKRect *r_scrub, BKRect *r_sound)
{
  const BKVideo *v = node_video(n);
  const int uu = bk_unit(), pad = BK_MAX(2, uu / 5);
  if (!v || !v->controls || n->h < uu + 2 * pad || n->w < 3 * uu) {
    return false;
  }
  const int y = n->y + n->h - uu - pad;
  const int sound = BK_video_has_audio(v) && n->w >= 4 * uu ? uu + pad : 0;
  *r_button = bk_rect(n->x + pad, y, uu, uu);
  *r_scrub = bk_rect(n->x + 2 * pad + uu, y, n->w - 3 * pad - uu - sound, uu);
  *r_sound = bk_rect(n->x + n->w - pad - uu, y, sound ? uu : 0, uu);
  return true;
}

static void video_measure(BKNode *n, int *rw, int *rh)
{
  if (!bk_node_image_size(n, rw, rh)) { /* HTML's default, until the size is known */
    *rw = (int)(300 * bk_px());
    *rh = (int)(150 * bk_px());
  }
}

static void format_time(char *buf, size_t size, double t)
{
  const int s = (int)BK_MAX(0.0, t);
  snprintf(buf, size, "%d:%02d", s / 60, s % 60);
}

static void video_draw(BKNode *n)
{
  const float box[4] = {(float)n->x, (float)n->y, (float)n->w, (float)n->h};
  bk_image_ref_draw(&n->image, n->image_fit, 0, box, NODE_ENABLED(n) ? 1.0f : 0.5f);
  BKRect rb, rs, ra;
  if (!controls_rects(n, &rb, &rs, &ra)) {
    return;
  }
  const BKVideo *v = node_video(n);
  const int state = BK_video_state(v);
  BKWCol w;
  bk_widget_state(&w, &bk_theme.regular, NODE_ENABLED(n) ? 0 : ST_DISABLED);
  bk_widget_but(&w, &rb, 0, CNR_ALL);
  const bool playing = state == BK_VIDEO_PLAYING || (v->scrubbing && v->resume);
  bk_icon(bk_icons_lookup(playing ? "PAUSE" : "PLAY"), rb.xmin + bk_rw(&rb) * 0.5f,
          rb.ymin + bk_rh(&rb) * 0.5f, w.text, 1.0f, 0);
  /* the scrubber: Blender's number slider, filled to the time */
  const double dur = BK_video_duration(v), t = BK_video_time(v);
  bk_widget_state(&w, &bk_theme.numslider, v->scrubbing ? ST_SELECT : 0);
  bk_widget_numslider(&w, &rs, 0, CNR_ALL, dur > 0.0 ? (float)BK_CLAMP(t / dur, 0.0, 1.0) : 0.0f);
  char a[16], b[16], label[40];
  format_time(a, sizeof(a), t);
  format_time(b, sizeof(b), dur);
  snprintf(label, sizeof(label), "%s / %s", a, b);
  bk_text_in(&rs, label, w.text, BK_ALIGN_CENTER, false);
  if (bk_rw(&ra) > 0) { /* the speaker: Blender's sound icon, crossed out when muted */
    bk_widget_state(&w, &bk_theme.regular, NODE_ENABLED(n) ? 0 : ST_DISABLED);
    bk_widget_but(&w, &ra, 0, CNR_ALL);
    bk_icon(bk_icons_lookup(v->muted ? "MUTE_IPO_ON" : "PLAY_SOUND"), ra.xmin + bk_rw(&ra) * 0.5f,
            ra.ymin + bk_rh(&ra) * 0.5f, w.text, 1.0f, 0);
  }
}

static void scrub_to(BKNode *n, int x)
{
  BKRect rb, rs, ra;
  BKVideo *v = node_video(n);
  const double dur = BK_video_duration(v);
  if (controls_rects(n, &rb, &rs, &ra) && dur > 0.0) {
    const double f = (x - rs.xmin) / (double)BK_MAX(1, bk_rw(&rs));
    BK_video_seek(v, BK_CLAMP(f, 0.0, 1.0) * dur);
  }
}

static int video_press(BKNode *n, BKEvent *e)
{
  BKRect rb, rs, ra;
  BKVideo *v = node_video(n);
  if (!controls_rects(n, &rb, &rs, &ra) || !NODE_ENABLED(n)) {
    return 0;
  }
  if (bk_rw(&ra) > 0 && bk_rect_inside(&ra, e->x, e->y)) {
    BK_video_set_muted(v, !v->muted);
    return 1;
  }
  if (bk_rect_inside(&rb, e->x, e->y)) {
    if (BK_video_state(v) == BK_VIDEO_PLAYING) {
      BK_video_pause(v);
    }
    else {
      BK_video_play(v);
    }
    return 1;
  }
  if (bk_rect_inside(&rs, e->x, e->y)) {
    /* scrubbing holds the video, like dragging Blender's frame slider */
    v->resume = BK_video_state(v) == BK_VIDEO_PLAYING;
    v->scrubbing = true;
    BK_video_pause(v);
    scrub_to(n, e->x);
    return 1;
  }
  return 0;
}

static int video_drag(BKNode *n, BKEvent *e)
{
  BKVideo *v = node_video(n);
  if (v && v->scrubbing) {
    scrub_to(n, e->x);
  }
  return 1;
}

static int video_release(BKNode *n, BKEvent *e)
{
  (void)e;
  BKVideo *v = node_video(n);
  if (v && v->scrubbing) {
    v->scrubbing = false;
    if (v->resume) {
      BK_video_play(v);
    }
  }
  return 1;
}

static int video_key(BKNode *n, BKEvent *e)
{
  BKVideo *v = node_video(n);
  if (!v || !v->controls || !bk_keysym_is(e->keysym, "space")) {
    return 0;
  }
  if (BK_video_state(v) == BK_VIDEO_PLAYING) {
    BK_video_pause(v);
  }
  else {
    BK_video_play(v);
  }
  return 1;
}

const BKClass bk_cls_video = {.name = "video",
                              .flags = CLS_INTERACTIVE,
                              .measure = video_measure,
                              .draw = video_draw,
                              .press = video_press,
                              .drag = video_drag,
                              .release = video_release,
                              .key = video_key};

/** \} */
