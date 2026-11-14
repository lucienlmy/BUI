/* Sound for <video>: a movie's audio track played on the default output
 * device, as Blender plays a sequencer strip's sound through audaspace's
 * devices (WASAPI, CoreAudio, PulseAudio).
 *
 *  audio.c            the player: the codec's samples converted to the
 *                     device's format, volume, loop, seek, and the clock
 *  device_wasapi.c    Windows (WASAPI, shared mode)
 *  device_coreaudio.c macOS (an AudioQueue)
 *  device_pulse.c     Linux (PulseAudio's simple API; PipeWire serves it too)
 *
 * The device asks for samples on its own thread; the player decodes them
 * there (the codec's audio reader: movie_*.c), so nothing on the UI thread
 * waits on a decoder or a device. Every backend loads its system library
 * at run time, as the video backends do: a machine without one has silent
 * video and nothing else changes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <stdbool.h>

#include "../image/image.h"

/* -- the device (device_*.c): one stream to the default output ------------------------ */

/* `frames` interleaved float frames at the device's rate and channels, on
 * the device's thread. Writes them all (silence where it has nothing). */
typedef void (*BKAudioFill)(void *data, float *out, int frames, int rate, int channels);

typedef struct BKAudioDevice BKAudioDevice;
/* Open flags. */
enum {
  /* Keep the system's audio effects (the Windows stream is otherwise RAW: it opens in half the
   * time and a pause is heard sooner, without the effects chain; other systems have no such
   * choice and ignore it). */
  BK_AUDIO_EFFECTS = 1 << 0,
};
/* Starts a stream; the device opens on its own thread and calls `fill` from
 * there once it plays. NULL when this build has no audio output. */
BKAudioDevice *bk_audio_device_open(BKAudioFill fill, void *data, int flags);
/* Seconds of the stream that have reached the speakers (0 before it plays):
 * the position of the samples `fill` wrote, less what the device buffers. */
double bk_audio_device_played(BKAudioDevice *d);
/* Stops the stream; `fill` is not called once this returns. It may wait for the device's thread
 * (a stream opening, a decoder read): bk_audio_free runs it on its own thread, never the UI's. */
void bk_audio_device_close(BKAudioDevice *d);

/* -- the player (audio.c): one movie's sound ----------------------------------------- */

typedef struct BKAudio BKAudio;
/* The first audio track of `path`, read by `codec` (the one that plays its
 * video). Opens nothing yet: the device and the reader open on the device's
 * thread. NULL when the codec reads no audio or there is no device. */
BKAudio *bk_audio_new(const BKImAnimCodec *codec, const char *path, int flags);
/* Returns at once: the device, the reader and the player are released on a thread of their own. */
void bk_audio_free(BKAudio *a);
/* Shutdown: waits up to `max_seconds` for the players being freed (the process may exit then). */
void bk_audio_exit(double max_seconds);
void bk_audio_play(BKAudio *a, bool playing);
void bk_audio_seek(BKAudio *a, double t);
void bk_audio_set_loop(BKAudio *a, bool loop);
void bk_audio_set_gain(BKAudio *a, float gain); /* 0 (muted) .. 1; NaN is 0 */
/* The pure parts of the conversion (tested without a device). */
float bk_audio_clamp_gain(float gain);
/* One frame from `src` channels to `dst`: Blender's 5.1 to stereo downmix, mono to both sides. */
void bk_audio_map_channels(const float *in, int src, float *out, int dst);
/* Source frames stepped per output frame. */
double bk_audio_resample_step(int src_rate, int rate);
/* The movie time being heard now (seconds), or < 0 when the sound isn't the
 * clock: not playing yet, paused, ended, no audio track. */
double bk_audio_clock(BKAudio *a);
/* The file has an audio track that plays (false until the reader opened it,
 * and after it failed). */
bool bk_audio_has_track(BKAudio *a);
