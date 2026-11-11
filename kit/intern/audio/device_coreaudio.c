/* Sound on macOS: an AudioQueue (AudioToolbox) on its own thread, float
 * stereo at 48 kHz (the queue converts to the device and follows the default
 * output as it changes). Three 20 ms buffers go round: each one played comes
 * back to the callback, which fills it again.
 *
 * The position heard is the queue's sample time, less the silence the
 * buffers were primed with (fill() didn't write that).
 *
 * UNTESTED here: written against the documented API without a Mac to run it
 * on; AudioToolbox is weak-linked, so a system without it has silent video
 * and nothing else changes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef BUI_AUDIO_COREAUDIO

#  include <AudioToolbox/AudioToolbox.h>
#  include <stdatomic.h>
#  include <string.h>

#  include "MEM_guardedalloc.h"

#  include "audio.h"

#  define RATE 48000
#  define CHANNELS 2
#  define BLOCK 960 /* 20 ms */
#  define BUFFERS 3

struct BKAudioDevice {
  BKAudioFill fill;
  void *data;
  AudioQueueRef queue;
  atomic_bool stopping;
};

static void refill(void *user, AudioQueueRef queue, AudioQueueBufferRef buf)
{
  BKAudioDevice *d = (BKAudioDevice *)user;
  if (atomic_load(&d->stopping)) {
    return;
  }
  d->fill(d->data, (float *)buf->mAudioData, BLOCK, RATE, CHANNELS);
  buf->mAudioDataByteSize = sizeof(float) * BLOCK * CHANNELS;
  AudioQueueEnqueueBuffer(queue, buf, 0, NULL);
}

BKAudioDevice *bk_audio_device_open(BKAudioFill fill, void *data, int flags)
{
  (void)flags; /* no effects to choose here */
  if (&AudioQueueNewOutput == NULL) {
    return NULL; /* weak-linked and absent */
  }
  BKAudioDevice *d = (BKAudioDevice *)MEM_callocN(sizeof(BKAudioDevice), "BKAudioDevice");
  d->fill = fill;
  d->data = data;
  AudioStreamBasicDescription fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.mSampleRate = RATE;
  fmt.mFormatID = kAudioFormatLinearPCM;
  fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  fmt.mBytesPerPacket = fmt.mBytesPerFrame = sizeof(float) * CHANNELS;
  fmt.mFramesPerPacket = 1;
  fmt.mChannelsPerFrame = CHANNELS;
  fmt.mBitsPerChannel = 32;
  /* NULL run loop: the callback runs on the queue's own thread */
  if (AudioQueueNewOutput(&fmt, refill, d, NULL, NULL, 0, &d->queue) != noErr) {
    MEM_freeN(d);
    return NULL;
  }
  for (int i = 0; i < BUFFERS; i++) {
    AudioQueueBufferRef buf = NULL;
    if (AudioQueueAllocateBuffer(d->queue, sizeof(float) * BLOCK * CHANNELS, &buf) != noErr) {
      continue;
    }
    /* primed with silence here; the queue's thread fills them from now on
     * (fill() opens the decoder: not on this, the UI thread) */
    memset(buf->mAudioData, 0, sizeof(float) * BLOCK * CHANNELS);
    buf->mAudioDataByteSize = sizeof(float) * BLOCK * CHANNELS;
    AudioQueueEnqueueBuffer(d->queue, buf, 0, NULL);
  }
  if (AudioQueueStart(d->queue, NULL) != noErr) {
    bk_audio_device_close(d);
    return NULL;
  }
  return d;
}

double bk_audio_device_played(BKAudioDevice *d)
{
  AudioTimeStamp ts;
  memset(&ts, 0, sizeof(ts));
  if (AudioQueueGetCurrentTime(d->queue, NULL, &ts, NULL) != noErr ||
      !(ts.mFlags & kAudioTimeStampSampleTimeValid))
  {
    return 0.0;
  }
  const double t = (ts.mSampleTime - (double)BLOCK * BUFFERS) / RATE; /* less the primed silence */
  return t > 0.0 ? t : 0.0;
}

void bk_audio_device_close(BKAudioDevice *d)
{
  if (!d) {
    return;
  }
  atomic_store(&d->stopping, true);
  AudioQueueStop(d->queue, true);    /* now: no more callbacks once it returns */
  AudioQueueDispose(d->queue, true); /* and its buffers */
  MEM_freeN(d);
}

#endif /* BUI_AUDIO_COREAUDIO */
