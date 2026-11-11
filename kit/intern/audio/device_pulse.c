/* Sound on Linux: PulseAudio's simple API, loaded at run time (dlopen, as
 * GStreamer is: movie_gst.c). PipeWire serves the same API (pipewire-pulse),
 * so this covers both. Float stereo at 48 kHz: the server converts to the
 * device.
 *
 * The stream's thread writes 20 ms blocks (pa_simple_write blocks while the
 * server's buffer is full) and reads the latency after each; the position
 * heard is what was written less that latency, moved on by the time since.
 * A lost connection (the server restarted) is opened again, the position
 * going on from what was written.
 *
 * UNTESTED here: written against the documented ABI (pulse/simple.h) without
 * a Linux machine to run it on; no PulseAudio library, no sound, nothing else
 * changes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef BUI_AUDIO_PULSE

#  include <dlfcn.h>
#  include <pthread.h>
#  include <stdint.h>
#  include <string.h>
#  include <time.h>
#  include <unistd.h>

#  include "MEM_guardedalloc.h"

#  include "audio.h"

typedef struct pa_simple pa_simple;
typedef struct PaSampleSpec { /* pa_sample_spec */
  int format;
  uint32_t rate;
  uint8_t channels;
} PaSampleSpec;
typedef struct PaBufferAttr { /* pa_buffer_attr */
  uint32_t maxlength, tlength, prebuf, minreq, fragsize;
} PaBufferAttr;
enum { PA_STREAM_PLAYBACK = 1, PA_SAMPLE_FLOAT32LE = 5 };

static struct {
  pthread_once_t once;
  pa_simple *(*open)(const char *, const char *, int, const char *, const char *, const PaSampleSpec *,
                     const void *, const PaBufferAttr *, int *);
  int (*write)(pa_simple *, const void *, size_t, int *);
  uint64_t (*latency)(pa_simple *, int *);
  void (*free)(pa_simple *);
} P = {PTHREAD_ONCE_INIT};

static void load(void)
{
  void *lib = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
  if (lib) {
    *(void **)&P.open = dlsym(lib, "pa_simple_new");
    *(void **)&P.write = dlsym(lib, "pa_simple_write");
    *(void **)&P.latency = dlsym(lib, "pa_simple_get_latency");
    *(void **)&P.free = dlsym(lib, "pa_simple_free");
  }
}

#  define RATE 48000
#  define CHANNELS 2
#  define BLOCK 960 /* 20 ms */

struct BKAudioDevice {
  BKAudioFill fill;
  void *data;
  pthread_t thread;
  pthread_mutex_t lock; /* the position below */
  volatile int stop;
  double written; /* seconds given to the server */
  double latency; /* what it held after the last write (seconds) */
  double stamp;   /* when that was (monotonic seconds) */
  bool playing;
};

static double now_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static pa_simple *connect_server(void)
{
  const PaSampleSpec spec = {PA_SAMPLE_FLOAT32LE, RATE, CHANNELS};
  /* about 60 ms buffered: short enough for the picture to follow */
  const PaBufferAttr attr = {(uint32_t)-1, RATE * CHANNELS * 4 * 60 / 1000, (uint32_t)-1,
                             (uint32_t)-1, (uint32_t)-1};
  int err = 0;
  return P.open(NULL, "BUI", PA_STREAM_PLAYBACK, NULL, "video", &spec, NULL, &attr, &err);
}

static void *stream_thread(void *param)
{
  BKAudioDevice *d = (BKAudioDevice *)param;
  float *buf = (float *)MEM_mallocN(sizeof(float) * BLOCK * CHANNELS, "pulse block");
  pa_simple *s = NULL;
  while (!d->stop) {
    if (!s && !(s = connect_server())) {
      for (int i = 0; i < 10 && !d->stop; i++) {
        usleep(100000); /* no server now: try again in a second */
      }
      continue;
    }
    d->fill(d->data, buf, BLOCK, RATE, CHANNELS);
    int err = 0;
    if (P.write(s, buf, sizeof(float) * BLOCK * CHANNELS, &err) < 0) {
      P.free(s);
      s = NULL;
      continue;
    }
    const uint64_t lat = P.latency(s, &err);
    pthread_mutex_lock(&d->lock);
    d->written += (double)BLOCK / RATE;
    d->latency = lat == (uint64_t)-1 ? 0.0 : (double)lat / 1e6;
    d->stamp = now_s();
    d->playing = true;
    pthread_mutex_unlock(&d->lock);
  }
  if (s) {
    P.free(s); /* not drained: a closed video stops at once */
  }
  MEM_freeN(buf);
  return NULL;
}

BKAudioDevice *bk_audio_device_open(BKAudioFill fill, void *data, int flags)
{
  (void)flags; /* no effects to choose here */
  pthread_once(&P.once, load);
  if (!P.open || !P.write || !P.latency || !P.free) {
    return NULL;
  }
  BKAudioDevice *d = (BKAudioDevice *)MEM_callocN(sizeof(BKAudioDevice), "BKAudioDevice");
  d->fill = fill;
  d->data = data;
  pthread_mutex_init(&d->lock, NULL);
  if (pthread_create(&d->thread, NULL, stream_thread, d) != 0) {
    pthread_mutex_destroy(&d->lock);
    MEM_freeN(d);
    return NULL;
  }
  return d;
}

double bk_audio_device_played(BKAudioDevice *d)
{
  pthread_mutex_lock(&d->lock);
  double t = 0.0;
  if (d->playing) {
    const double heard = d->written - d->latency; /* at `stamp` */
    t = heard + (now_s() - d->stamp);
    t = t < heard ? heard : t > d->written ? d->written : t;
  }
  pthread_mutex_unlock(&d->lock);
  return t;
}

void bk_audio_device_close(BKAudioDevice *d)
{
  if (!d) {
    return;
  }
  d->stop = 1;
  pthread_join(d->thread, NULL);
  pthread_mutex_destroy(&d->lock);
  MEM_freeN(d);
}

#endif /* BUI_AUDIO_PULSE */
