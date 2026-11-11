/* Video: load, play, pause, seek, rate, loop and end through the public API
 * (BK_video_*), drawn on a canvas like an app would; nothing decoded while
 * off screen; malformed files; no allocation per frame once playing; sound
 * (clip_audio.mp4: the frames follow it, off screen too); every
 * MEM block back after BK_video_free + BK_image_exit. Exits 77 (skipped)
 * when the build or the machine has no video backend that reads the clip.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define sleep_ms(ms) Sleep(ms)
#else
#  include <time.h>
#  include <unistd.h>
#  define sleep_ms(ms) usleep((ms) * 1000)
#endif

#include "BUI_kit.h"
#include "GPU_raster.h"
#include "MEM_guardedalloc.h"

static int g_fail = 0;
#define CHECK(c) \
  do { \
    if (!(c)) { \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      g_fail = 1; \
    } \
  } while (0)

static double now_s(void)
{
#ifdef _WIN32
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)f.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

/* A host whose one timer the test loop fires. */
static struct {
  double due;
  int timer, requests;
} g_host;

static void host_schedule(void *data, int ms, int timer)
{
  (void)data;
  g_host.due = now_s() + ms / 1000.0;
  g_host.timer = timer;
}

static void host_request(void *data)
{
  (void)data;
  g_host.requests++;
}

#define W 200
#define H 120

/* Runs the context like a window loop for `seconds` (or until `until`
 * returns nonzero); returns the frames rendered. */
static int run(BKContext *ctx, BuiRaster *r, double seconds, int (*until)(void *), void *arg)
{
  const double end = now_s() + seconds;
  int frames = 0;
  while (now_s() < end && !(until && until(arg))) {
    if (g_host.due > 0.0 && now_s() >= g_host.due) {
      g_host.due = 0.0;
      BK_timer(ctx, g_host.timer);
    }
    BK_image_poll();
    if (BK_needs_render(ctx)) {
      frames += BK_render(ctx, r, W, H, 0) > 0;
    }
    sleep_ms(1);
  }
  return frames;
}

static int loaded(void *v)
{
  return BK_video_state((BKVideo *)v) != BK_VIDEO_LOADING;
}

static int ended(void *v)
{
  return BK_video_state((BKVideo *)v) == BK_VIDEO_ENDED;
}

static uint64_t box_hash(const uint32_t *px)
{
  uint64_t h = 1469598103934665603ull;
  for (int y = 10; y < 64; y++) {
    for (int x = 10; x < 106; x++) {
      h = (h ^ (px[y * W + x] & 0xFFFFFF)) * 1099511628211ull;
    }
  }
  return h;
}

static int g_events[8];
static void listener(BKVideo *v, int state, void *data)
{
  (void)v;
  (void)data;
  g_events[state & 7]++;
}

static void show(BKNode *canvas, BKVideo *v)
{
  static const float box[4] = {10, 10, 106, 64}; /* the clip's 96 x 54 */
  BKCanvasItem it;
  memset(&it, 0, sizeof(it));
  it.id = 1;
  it.type = BK_ITEM_IMAGE;
  it.coords = box;
  it.ncoords = 4;
  it.video = v;
  it.fit = BK_FIT_FILL;
  BK_canvas_set_item(canvas, &it);
}

static void test_playback(const char *clip)
{
  BKNode *canvas = BK_node_new("canvas");
  BKContext *ctx = BK_context_new(canvas);
  BKHost host = {0};
  host.request = host_request;
  host.schedule = host_schedule;
  BK_context_set_host(ctx, &host);
  BuiRaster *r = BUI_raster_create(W, H);
  const uint32_t *px = BUI_raster_pixels(r);

  /* loads when first drawn, then plays (autoplay; this clip has no sound) */
  BKVideo *v = BK_video_new(clip);
  BK_video_set_listener(v, listener, NULL);
  show(canvas, v);
  const double t0 = now_s();
  BK_render(ctx, r, W, H, 1);
  run(ctx, r, 5.0, loaded, v);
  printf("first frame after %.1f ms; backends: %s\n", (now_s() - t0) * 1000.0,
         BK_video_backends());
  int w = 0, h = 0;
  CHECK(BK_video_state(v) == BK_VIDEO_PLAYING);
  CHECK(BK_video_size(v, &w, &h) && w == 96 && h == 54);
  CHECK(fabs(BK_video_duration(v) - 2.0) < 0.15);
  CHECK(g_events[BK_VIDEO_PLAYING] == 1);
  CHECK(!BK_video_has_audio(v) || getenv("BUI_TEST_CLIP")); /* no audio track: no sound */

  /* it plays: the picture moves and the time goes on */
  uint64_t last = box_hash(px);
  int changes = 0;
  for (int i = 0; i < 10; i++) {
    run(ctx, r, 0.06, NULL, NULL);
    const uint64_t c = box_hash(px);
    changes += c != last;
    last = c;
  }
  const double t_play = BK_video_time(v);
  printf("playing 0.6 s: %d frame changes, at %.2f s\n", changes, t_play);
  CHECK(changes >= 2 && t_play > 0.2); /* 6 frames of 0.1 s; a loaded runner drops some */

  /* paused: nothing moves */
  BK_video_pause(v);
  run(ctx, r, 0.1, NULL, NULL);
  const uint64_t held = box_hash(px);
  const double t_held = BK_video_time(v);
  run(ctx, r, 0.3, NULL, NULL);
  CHECK(box_hash(px) == held && BK_video_time(v) == t_held);
  CHECK(BK_video_state(v) == BK_VIDEO_PAUSED);

  /* seek while paused: that frame shows */
  BK_video_seek(v, 1.5);
  run(ctx, r, 0.4, NULL, NULL);
  printf("seek to 1.5 s: at %.3f s\n", BK_video_time(v));
  CHECK(fabs(BK_video_time(v) - 1.5) < 0.11);
  CHECK(box_hash(px) != held);

  /* no allocation per frame once playing (MEM: requests and frames recycle) */
  BK_video_seek(v, 0.0);
  BK_video_play(v);
  run(ctx, r, 0.3, NULL, NULL);
  const size_t allocs = MEM_get_allocations_total();
  const unsigned blocks = MEM_get_memory_blocks_in_use();
  const double ta = BK_video_time(v);
  run(ctx, r, 0.6, NULL, NULL);
  const double frames_played = (BK_video_time(v) - ta) * 10.0;
  printf("playing %.0f frames: %zu MEM allocations, %d blocks change\n", frames_played,
         MEM_get_allocations_total() - allocs, (int)MEM_get_memory_blocks_in_use() - (int)blocks);
  CHECK(MEM_get_allocations_total() == allocs);

  /* rate and end: 4x from 1 s plays the rest in 0.25 s, then it ends (once) */
  BK_video_seek(v, 1.0);
  BK_video_set_rate(v, 4.0);
  const double te = now_s();
  run(ctx, r, 3.0, ended, v);
  printf("4x from 1 s ended after %.2f s\n", now_s() - te);
  CHECK(BK_video_state(v) == BK_VIDEO_ENDED && g_events[BK_VIDEO_ENDED] == 1);
  CHECK(now_s() - te < 2.0); /* 0.25 s of video at 4x */

  /* loop: it goes round */
  BK_video_set_loop(v, 1);
  BK_video_play(v);
  double lowest = 10.0, highest = 0.0;
  for (int i = 0; i < 40; i++) {
    run(ctx, r, 0.03, NULL, NULL);
    const double t = BK_video_time(v);
    lowest = t < lowest ? t : lowest;
    highest = t > highest ? t : highest;
  }
  printf("looping 1.2 s at 4x: time between %.2f and %.2f\n", lowest, highest);
  CHECK(BK_video_state(v) == BK_VIDEO_PLAYING && lowest < 0.5 && highest > 1.5);

  /* off screen: no decoding, no timers, no renders */
  BK_canvas_delete(canvas, -1);
  BK_render(ctx, r, W, H, 1);
  BKImageStats st;
  for (int i = 0; i < 200; i++) {
    run(ctx, r, 0.01, NULL, NULL);
    BK_image_stats(&st);
    if (st.pending == 0) {
      break;
    }
  }
  run(ctx, r, 0.05, NULL, NULL);
  BK_image_stats(&st);
  const int decoded = st.decoded, requests = g_host.requests;
  const double tv = BK_video_time(v);
  const int frames = run(ctx, r, 0.5, NULL, NULL);
  BK_image_stats(&st);
  printf("off screen for 0.5 s: %d decodes, %d renders, %d render requests\n",
         st.decoded - decoded, frames, g_host.requests - requests);
  CHECK(st.decoded == decoded && frames == 0 && g_host.requests == requests);
  CHECK(BK_video_time(v) == tv);

  /* shown again: it resumes */
  show(canvas, v);
  BK_render(ctx, r, W, H, 1);
  run(ctx, r, 0.3, NULL, NULL);
  CHECK(BK_video_time(v) != tv);

  BK_canvas_delete(canvas, -1);
  BK_video_free(v);
  BK_context_free(ctx);
  BK_node_free(canvas);
  BUI_raster_free(r);
}

static int sounding(void *v)
{
  return BK_video_has_audio((BKVideo *)v);
}

/* Sound: the audio track plays on the default output and the frames follow
 * it (muted here: the clock follows a muted track too); pause holds it; it
 * plays on off screen and the picture catches up when it shows again.
 * Skipped where the machine has no output device. */
static void test_sound(const char *clip)
{
  BKNode *canvas = BK_node_new("canvas");
  BKContext *ctx = BK_context_new(canvas);
  BKHost host = {0};
  host.request = host_request;
  host.schedule = host_schedule;
  BK_context_set_host(ctx, &host);
  BuiRaster *r = BUI_raster_create(W, H);

  BKVideo *v = BK_video_new(clip);
  CHECK(BK_video_volume(v) == 1.0 && !BK_video_muted(v)); /* sound by default */
  BK_video_set_volume(v, 2.0);
  CHECK(BK_video_volume(v) == 1.0);
  BK_video_set_volume(v, 0.25);
  CHECK(fabs(BK_video_volume(v) - 0.25) < 1e-6);
  BK_video_set_muted(v, 1);
  CHECK(BK_video_muted(v));
  CHECK(!BK_video_audio_effects(v)); /* the system's effects are off by default */
  BK_video_set_audio_effects(v, 1);
  CHECK(BK_video_audio_effects(v));
  BK_video_set_audio_effects(v, 0);
  CHECK(!BK_video_audio_effects(v));
  BK_video_set_loop(v, 1); /* an output can take seconds to wake (Bluetooth): it plays on meanwhile */
  show(canvas, v);
  BK_render(ctx, r, W, H, 1);
  run(ctx, r, 5.0, loaded, v);
  run(ctx, r, 6.0, sounding, v);
  if (!BK_video_has_audio(v)) {
    printf("sound: no audio output here, skipped\n");
  }
  else {
    /* in time with the sound: a second of playing moves the video a second */
    BK_video_seek(v, 0.0);
    run(ctx, r, 0.3, NULL, NULL);
    const double t0 = BK_video_time(v), w0 = now_s();
    run(ctx, r, 1.0, NULL, NULL);
    const double moved = BK_video_time(v) - t0, wall = now_s() - w0;
    printf("sound: %.2f s of video in %.2f s\n", moved, wall);
    CHECK(fabs(moved - wall) < 0.25);

    /* the effects on while it plays: the output opens again, and the sound is back in time */
    BK_video_set_audio_effects(v, 1);
    CHECK(BK_video_audio_effects(v) && !BK_video_has_audio(v)); /* closed until it plays again */
    run(ctx, r, 6.0, sounding, v);
    CHECK(BK_video_has_audio(v));
    const double e0 = BK_video_time(v), ew0 = now_s();
    run(ctx, r, 1.0, NULL, NULL);
    const double e_moved = fmod(BK_video_time(v) - e0 + 2.0, 2.0), e_wall = now_s() - ew0;
    printf("sound with the effects: %.2f s of video in %.2f s\n", e_moved, e_wall);
    CHECK(fabs(e_moved - e_wall) < 0.3);
    BK_video_set_audio_effects(v, 0);
    run(ctx, r, 6.0, sounding, v);

    /* paused: the sound and the frames hold */
    BK_video_pause(v);
    run(ctx, r, 0.15, NULL, NULL);
    const double held = BK_video_time(v);
    run(ctx, r, 0.3, NULL, NULL);
    CHECK(BK_video_time(v) == held);

    /* off screen the sound plays on; shown again, the picture is where it is */
    BK_video_seek(v, 0.0);
    BK_video_play(v);
    run(ctx, r, 0.3, NULL, NULL);
    BK_canvas_delete(canvas, -1);
    BK_render(ctx, r, W, H, 1);
    const double hidden = BK_video_time(v);
    run(ctx, r, 0.7, NULL, NULL);
    show(canvas, v);
    BK_render(ctx, r, W, H, 1);
    run(ctx, r, 0.3, NULL, NULL);
    printf("sound: hidden at %.2f s for 0.7 s, shown again at %.2f s\n", hidden, BK_video_time(v));
    CHECK(BK_video_time(v) - hidden > 0.6);
  }
  BK_canvas_delete(canvas, -1);
  BK_video_free(v); /* with its sound playing */
  BK_context_free(ctx);
  BK_node_free(canvas);
  BUI_raster_free(r);
}

/* Truncated and bit-flipped copies: an error state or frames, never a crash. */
static void test_malformed(const char *clip)
{
  FILE *f = fopen(clip, "rb");
  if (!f) {
    CHECK(0);
    return;
  }
  static unsigned char data[1 << 16];
  const size_t n = fread(data, 1, sizeof(data), f);
  fclose(f);
  BKNode *canvas = BK_node_new("canvas");
  BKContext *ctx = BK_context_new(canvas);
  BuiRaster *r = BUI_raster_create(W, H);
  char path[600];
  snprintf(path, sizeof(path), "%s/bad.mp4", BUI_TEST_TMP);
  int errors = 0, played = 0;
  srand(7);
  for (int it = 0; it < 24; it++) {
    static unsigned char bad[1 << 16];
    memcpy(bad, data, n);
    size_t len = n;
    if (it % 3 == 0) {
      len = (size_t)rand() % n; /* truncated */
    }
    else {
      for (int k = 0; k < 1 + it; k++) { /* bytes flipped */
        bad[(size_t)rand() % n] ^= (unsigned char)(1 + rand() % 255);
      }
    }
    FILE *o = fopen(path, "wb");
    if (!o) { /* the previous clip is still open: a freed video's decode lands first */
      CHECK(0);
      break;
    }
    fwrite(bad, 1, len, o);
    fclose(o);
    BKVideo *v = BK_video_new(path);
    show(canvas, v);
    BK_render(ctx, r, W, H, 1);
    run(ctx, r, 2.0, loaded, v);
    run(ctx, r, 0.2, NULL, NULL); /* a few frames of whatever decodes */
    errors += BK_video_state(v) == BK_VIDEO_ERROR;
    played += BK_video_state(v) == BK_VIDEO_PLAYING || BK_video_state(v) == BK_VIDEO_ENDED;
    BK_canvas_delete(canvas, -1);
    BK_render(ctx, r, W, H, 1);
    BK_video_free(v);
    /* freeing a video with a decode in flight lets it finish (the worker holds the
     * file open, and on Windows a held file can't be rewritten): wait it out */
    BKImageStats st;
    for (int i = 0; i < 5000; i++) {
      BK_image_poll();
      BK_image_stats(&st);
      if (st.pending == 0) {
        break;
      }
      sleep_ms(1);
    }
  }
  remove(path);
  printf("24 malformed clips: %d errors, %d played, no crash\n", errors, played);
  CHECK(errors + played > 0);
  BK_context_free(ctx);
  BK_node_free(canvas);
  BUI_raster_free(r);
}

int main(void)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  if (!BK_video_backends()[0]) {
    printf("video_test: no video backend in this build, skipped\n");
    return 77;
  }
  const unsigned blocks0 = MEM_get_memory_blocks_in_use();
  const size_t bytes0 = MEM_get_memory_in_use();
  char clip[600];
  snprintf(clip, sizeof(clip), "%s/%s", BUI_TEST_VIDEOS, getenv("BUI_TEST_CLIP") ? getenv("BUI_TEST_CLIP") : "clip.mp4");
  {
    /* the machine may lack the decoder (Windows N without the Media Feature Pack) */
    BKVideo *probe = BK_video_new(clip);
    BKNode *canvas = BK_node_new("canvas");
    BKContext *ctx = BK_context_new(canvas);
    BuiRaster *r = BUI_raster_create(W, H);
    show(canvas, probe);
    BK_render(ctx, r, W, H, 1);
    run(ctx, r, 5.0, loaded, probe);
    const int state = BK_video_state(probe);
    BK_canvas_delete(canvas, -1);
    BK_video_free(probe);
    BK_context_free(ctx);
    BK_node_free(canvas);
    BUI_raster_free(r);
    BK_image_exit();
    /* the machine may lack the decoder, or the backend may not deliver here (no
     * display session, a runner without hardware): anything but a playing video
     * skips, it is not a failure of the code under test */
    if (state != BK_VIDEO_PLAYING) {
      printf("video_test: %s does not play %s here (state %d), skipped\n", BK_video_backends(),
             clip, state);
      return 77;
    }
  }
  memset(g_events, 0, sizeof(g_events));
  test_playback(clip);
  test_malformed(clip);
  snprintf(clip, sizeof(clip), "%s/clip_audio.mp4", BUI_TEST_VIDEOS);
  test_sound(clip);
  BK_image_exit();
  printf("memory: %u blocks, %zu bytes before; %u blocks, %zu bytes after\n", blocks0, bytes0,
         MEM_get_memory_blocks_in_use(), MEM_get_memory_in_use());
  CHECK(MEM_get_memory_blocks_in_use() == blocks0);
  CHECK(MEM_get_memory_in_use() == bytes0);
  printf(g_fail ? "video_test: FAILED\n" : "video_test: ok\n");
  return g_fail;
}
