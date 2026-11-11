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
/* Starts a stream; the device opens on its own thread and calls `fill` from
 * there once it plays. NULL when this build has no audio output. */
BKAudioDevice *bk_audio_device_open(BKAudioFill fill, void *data);
/* Seconds of the stream that have reached the speakers (0 before it plays):
 * the position of the samples `fill` wrote, less what the device buffers. */
double bk_audio_device_played(BKAudioDevice *d);
/* Stops the stream; `fill` is not called once this returns. */
void bk_audio_device_close(BKAudioDevice *d);

/* -- the player (audio.c): one movie's sound ----------------------------------------- */

typedef struct BKAudio BKAudio;
/* The first audio track of `path`, read by `codec` (the one that plays its
 * video). Opens nothing yet: the device and the reader open on the device's
 * thread. NULL when the codec reads no audio or there is no device. */
BKAudio *bk_audio_new(const BKImAnimCodec *codec, const char *path);
void bk_audio_free(BKAudio *a);
void bk_audio_play(BKAudio *a, bool playing);
void bk_audio_seek(BKAudio *a, double t);
void bk_audio_set_loop(BKAudio *a, bool loop);
void bk_audio_set_gain(BKAudio *a, float gain); /* 0 (muted) .. 1 */
/* The movie time being heard now (seconds), or < 0 when the sound isn't the
 * clock: not playing yet, paused, ended, no audio track. */
double bk_audio_clock(BKAudio *a);
/* The file has an audio track that plays (false until the reader opened it,
 * and after it failed). */
bool bk_audio_has_track(BKAudio *a);
