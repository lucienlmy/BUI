/* Video on macOS: AVFoundation's AVAssetReader (the system demuxers and
 * VideoToolbox decoders: H.264, HEVC, ProRes, VP9/AV1 where the OS has
 * them), NV12 out of the decoder at the size drawn (kCVPixelBufferWidthKey
 * / HeightKey on the track output: VideoToolbox scales).
 *
 * UNTESTED: written against the documented API without a Mac to run it on;
 * it compiles only on Apple platforms (BUI_VIDEO_AVF) and the frameworks
 * are weak-linked, so a failure here can't affect other platforms or
 * start-up. Manual retain/release, as the Cocoa window host (-fno-objc-arc).
 *
 * AVAssetReader can't seek: a seek (or a new output size) starts a new
 * reader at that time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef BUI_VIDEO_AVF

#  import <AVFoundation/AVFoundation.h>
#  import <CoreMedia/CoreMedia.h>
#  import <CoreVideo/CoreVideo.h>

#  include <string.h>

#  include "MEM_guardedalloc.h"

#  include "image.h"

typedef struct AvfMovie {
  AVURLAsset *asset;          /* retained */
  AVAssetTrack *track;        /* retained */
  AVAssetReader *reader;      /* retained; NULL until the first frame */
  AVAssetReaderTrackOutput *out;
  int w, h;                   /* output frame (NV12) */
  double start;               /* where the reader starts (seconds) */
  double frame_dur;
} AvfMovie;

static void reader_drop(AvfMovie *m)
{
  if (m->reader) {
    [m->reader cancelReading];
    [m->reader release];
    m->reader = nil;
  }
  m->out = nil; /* owned by the reader */
}

static void avf_close(void *state)
{
  AvfMovie *m = (AvfMovie *)state;
  @autoreleasepool {
    reader_drop(m);
    [m->track release];
    [m->asset release];
  }
  MEM_freeN(m);
}

/* A reader from m->start, decoding NV12 at m->w x m->h. */
static bool reader_start(AvfMovie *m)
{
  reader_drop(m);
  NSError *err = nil;
  AVAssetReader *r = [[AVAssetReader alloc] initWithAsset:m->asset error:&err];
  if (!r) {
    return false;
  }
  NSDictionary *settings = @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
    (id)kCVPixelBufferWidthKey : @(m->w),
    (id)kCVPixelBufferHeightKey : @(m->h),
  };
  AVAssetReaderTrackOutput *out = [AVAssetReaderTrackOutput
      assetReaderTrackOutputWithTrack:m->track
                       outputSettings:settings];
  out.alwaysCopiesSampleData = NO;
  if (![r canAddOutput:out]) {
    [r release];
    return false;
  }
  [r addOutput:out];
  r.timeRange = CMTimeRangeMake(CMTimeMakeWithSeconds(m->start, 90000), kCMTimePositiveInfinity);
  if (![r startReading]) {
    [r release];
    return false;
  }
  m->reader = r;
  m->out = out;
  return true;
}

static void *avf_open(const char *path, const uint8_t *mem, size_t size, BKImAnimInfo *r_info)
{
  (void)mem;
  (void)size;
  if (!path) {
    return NULL;
  }
  AvfMovie *m = (AvfMovie *)MEM_callocN(sizeof(AvfMovie), "AvfMovie");
  @autoreleasepool {
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    m->asset = url ? [[AVURLAsset alloc] initWithURL:url options:nil] : nil;
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations" /* the synchronous API */
    NSArray<AVAssetTrack *> *tracks = [m->asset tracksWithMediaType:AVMediaTypeVideo];
    m->track = tracks.count ? [tracks[0] retain] : nil;
    r_info->sound = [m->asset tracksWithMediaType:AVMediaTypeAudio].count > 0;
    const CGSize natural = m->track ? m->track.naturalSize : CGSizeZero;
    const double duration = m->asset ? CMTimeGetSeconds(m->asset.duration) : 0.0;
    const float fps = m->track ? m->track.nominalFrameRate : 0.0f;
#  pragma clang diagnostic pop
    if (!m->track || natural.width < 2 || natural.height < 2) {
      avf_close(m);
      return NULL;
    }
    m->frame_dur = fps > 0 ? 1.0 / fps : 1.0 / 30.0;
    r_info->src_w = (int)natural.width;
    r_info->src_h = (int)natural.height;
    r_info->duration = duration > 0.0 ? duration : 0.0;
    r_info->loops = 1;
    r_info->format = r_info->src_h >= 720 ? BUI_PIXMAP_NV12_BT709 : BUI_PIXMAP_NV12_BT601;
  }
  return m;
}

static bool avf_output(void *state, int w, int h, BKImAnimInfo *r_info)
{
  AvfMovie *m = (AvfMovie *)state;
  m->w = BK_MAX(2, w & ~1);
  m->h = BK_MAX(2, h & ~1);
  bool ok;
  @autoreleasepool {
    ok = reader_start(m);
  }
  r_info->w = m->w;
  r_info->h = m->h;
  return ok;
}

static bool copy_frame(CVPixelBufferRef px, BKImBuf *dst)
{
  if (CVPixelBufferGetPlaneCount(px) != 2 || (int)CVPixelBufferGetWidth(px) != dst->x ||
      (int)CVPixelBufferGetHeight(px) != dst->y ||
      CVPixelBufferLockBaseAddress(px, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
  {
    return false;
  }
  uint8_t *y = (uint8_t *)dst->rect, *uv = y + (size_t)dst->x * dst->y;
  const uint8_t *sy = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(px, 0);
  const uint8_t *suv = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(px, 1);
  const size_t py = CVPixelBufferGetBytesPerRowOfPlane(px, 0);
  const size_t puv = CVPixelBufferGetBytesPerRowOfPlane(px, 1);
  for (int r = 0; r < dst->y; r++) {
    memcpy(y + (size_t)r * dst->x, sy + r * py, (size_t)dst->x);
  }
  for (int r = 0; r < dst->y / 2; r++) {
    memcpy(uv + (size_t)r * dst->x, suv + r * puv, (size_t)dst->x);
  }
  CVPixelBufferUnlockBaseAddress(px, kCVPixelBufferLock_ReadOnly);
  return true;
}

static int avf_next(void *state, BKImBuf *dst, double *r_pts, double *r_dur)
{
  AvfMovie *m = (AvfMovie *)state;
  int got = -1;
  @autoreleasepool {
    if (!m->reader && !reader_start(m)) {
      return -1;
    }
    CMSampleBufferRef sb = [m->out copyNextSampleBuffer];
    if (!sb) {
      got = m->reader.status == AVAssetReaderStatusCompleted ? 0 : -1;
    }
    else {
      CVPixelBufferRef px = CMSampleBufferGetImageBuffer(sb);
      if (px && copy_frame(px, dst)) {
        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
        const CMTime dur = CMSampleBufferGetDuration(sb);
        *r_pts = CMTIME_IS_NUMERIC(pts) ? CMTimeGetSeconds(pts) : -1.0;
        *r_dur = CMTIME_IS_NUMERIC(dur) ? CMTimeGetSeconds(dur) : m->frame_dur;
        got = 1;
      }
      CFRelease(sb);
    }
  }
  return got;
}

static bool avf_seek(void *state, double t)
{
  AvfMovie *m = (AvfMovie *)state;
  m->start = t; /* readers start at a time (the keyframe before it: anim.c skips on) */
  bool ok;
  @autoreleasepool {
    ok = reader_start(m);
  }
  return ok;
}

static size_t avf_bytes(const void *state)
{
  const AvfMovie *m = (const AvfMovie *)state;
  return sizeof(*m) + (size_t)m->w * m->h * 3; /* decoded frames queued (an estimate) */
}

/* -------------------------------------------------------------------- */
/** \name Sound: a second reader on the audio track, float PCM at the device's format
 * \{ */

typedef struct AvfSound {
  AVURLAsset *asset;   /* retained */
  AVAssetTrack *track; /* retained */
  AVAssetReader *reader;
  AVAssetReaderTrackOutput *out;
  int rate, channels;
  double start;
  float *buf; /* the last block */
  size_t cap; /* floats */
} AvfSound;

static void sound_drop(AvfSound *s)
{
  if (s->reader) {
    [s->reader cancelReading];
    [s->reader release];
    s->reader = nil;
  }
  s->out = nil;
}

static void avf_audio_close(void *state)
{
  AvfSound *s = (AvfSound *)state;
  @autoreleasepool {
    sound_drop(s);
    [s->track release];
    [s->asset release];
  }
  if (s->buf) {
    MEM_freeN(s->buf);
  }
  MEM_freeN(s);
}

/* A reader from s->start, interleaved float at s->rate and s->channels. */
static bool sound_start(AvfSound *s)
{
  sound_drop(s);
  NSError *err = nil;
  AVAssetReader *r = [[AVAssetReader alloc] initWithAsset:s->asset error:&err];
  if (!r) {
    return false;
  }
  NSDictionary *settings = @{
    AVFormatIDKey : @(kAudioFormatLinearPCM),
    AVLinearPCMIsFloatKey : @YES,
    AVLinearPCMBitDepthKey : @32,
    AVLinearPCMIsNonInterleaved : @NO,
    AVLinearPCMIsBigEndianKey : @NO,
    AVSampleRateKey : @(s->rate),
    AVNumberOfChannelsKey : @(s->channels),
  };
  AVAssetReaderTrackOutput *out = [AVAssetReaderTrackOutput
      assetReaderTrackOutputWithTrack:s->track
                       outputSettings:settings];
  if (![r canAddOutput:out]) {
    [r release];
    return false;
  }
  [r addOutput:out];
  r.timeRange = CMTimeRangeMake(CMTimeMakeWithSeconds(s->start, 48000), kCMTimePositiveInfinity);
  if (![r startReading]) {
    [r release];
    return false;
  }
  s->reader = r;
  s->out = out;
  return true;
}

static void *avf_audio_open(const char *path, int rate, int channels, int *r_rate, int *r_channels)
{
  if (!path) {
    return NULL;
  }
  AvfSound *s = (AvfSound *)MEM_callocN(sizeof(AvfSound), "AvfSound");
  s->rate = rate;
  s->channels = channels;
  bool ok = false;
  @autoreleasepool {
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    s->asset = url ? [[AVURLAsset alloc] initWithURL:url options:nil] : nil;
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations" /* the synchronous API */
    NSArray<AVAssetTrack *> *tracks = [s->asset tracksWithMediaType:AVMediaTypeAudio];
#  pragma clang diagnostic pop
    s->track = tracks.count ? [tracks[0] retain] : nil;
    ok = s->track && sound_start(s); /* no audio track: no sound */
  }
  if (!ok) {
    avf_audio_close(s);
    return NULL;
  }
  *r_rate = rate; /* the output settings: AVFoundation converts */
  *r_channels = channels;
  return s;
}

static int avf_audio_read(void *state, const float **r_data, double *r_pts)
{
  AvfSound *s = (AvfSound *)state;
  int frames = -1;
  @autoreleasepool {
    if (!s->reader && !sound_start(s)) {
      return -1;
    }
    for (int empty = 0; empty < 64 && frames <= 0; empty++) {
      CMSampleBufferRef sb = [s->out copyNextSampleBuffer];
      if (!sb) {
        return s->reader.status == AVAssetReaderStatusCompleted ? 0 : -1;
      }
      CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sb);
      const size_t len = block ? CMBlockBufferGetDataLength(block) : 0;
      const size_t n = len / sizeof(float);
      frames = -1;
      if (block && CMBlockBufferCopyDataBytes(block, 0, n * sizeof(float),
                                              imb_audio_block_reserve(&s->buf, &s->cap, n)) ==
                       kCMBlockBufferNoErr)
      {
        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
        *r_pts = CMTIME_IS_NUMERIC(pts) ? CMTimeGetSeconds(pts) : -1.0;
        *r_data = s->buf;
        frames = (int)(n / (size_t)s->channels);
      }
      CFRelease(sb);
      if (frames < 0) {
        break; /* a block that can't be copied: an error */
      }
    }
    if (frames == 0) {
      frames = -1; /* only empty blocks for a long while: an error */
    }
  }
  return frames;
}

static bool avf_audio_seek(void *state, double t)
{
  AvfSound *s = (AvfSound *)state;
  s->start = t; /* readers start at a time: audio.c drops what comes before it */
  bool ok;
  @autoreleasepool {
    ok = sound_start(s);
  }
  return ok;
}

/** \} */

const BKImAnimCodec imb_movie_avf = {"avfoundation", true, imb_is_a_movie, avf_open, avf_output,
                                     avf_next, avf_seek, avf_bytes, avf_close, avf_audio_open,
                                     avf_audio_read, avf_audio_seek, avf_audio_close};

#endif /* BUI_VIDEO_AVF */
