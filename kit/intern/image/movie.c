/* Movies: the video backends this build has (chosen at build time from the
 * feature table, features.cmake: BUI_VIDEO_*), tried in order behind the one
 * codec interface (BKImAnimCodec), and what a movie file looks like.
 *
 *  movie_mf.c     Windows Media Foundation (the OS decoders, DXVA)
 *  movie_avf.m    macOS AVFoundation / VideoToolbox
 *  movie_gst.c    Linux GStreamer (VA-API through its plugins)
 *
 * Every backend loads its system libraries at run time (LoadLibrary /
 * dlopen / weak frameworks): a machine without them starts normally and
 * just has no video.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <string.h>

#include "MEM_guardedalloc.h"

#include "image.h"

float *imb_audio_block_reserve(float **buf, size_t *cap, size_t floats)
{
  if (floats > *cap) {
    if (*buf) {
      MEM_freeN(*buf);
    }
    *buf = (float *)MEM_mallocN(sizeof(float) * floats, "audio block");
    *cap = floats;
  }
  return *buf;
}

bool imb_is_a_movie(const uint8_t *m, size_t n)
{
  static const uint8_t ebml[4] = {0x1A, 0x45, 0xDF, 0xA3};     /* WebM, Matroska */
  static const uint8_t asf[4] = {0x30, 0x26, 0xB2, 0x75};      /* WMV, ASF */
  static const uint8_t mpeg_ps[4] = {0x00, 0x00, 0x01, 0xBA};  /* MPEG program stream */
  return (n >= 12 && memcmp(m + 4, "ftyp", 4) == 0 && memcmp(m + 8, "avif", 4) != 0 &&
          memcmp(m + 8, "heic", 4) != 0) || /* MP4, MOV, M4V, 3GP (not AVIF/HEIC stills) */
         (n >= 8 && (memcmp(m + 4, "moov", 4) == 0 || memcmp(m + 4, "mdat", 4) == 0 ||
                     memcmp(m + 4, "wide", 4) == 0 || memcmp(m + 4, "free", 4) == 0)) ||
         (n >= 4 && (memcmp(m, ebml, 4) == 0 || memcmp(m, asf, 4) == 0 ||
                     memcmp(m, mpeg_ps, 4) == 0 || memcmp(m, "OggS", 4) == 0)) ||
         (n >= 12 && memcmp(m, "RIFF", 4) == 0 && memcmp(m + 8, "AVI ", 4) == 0) ||
         (n >= 3 && memcmp(m, "FLV", 3) == 0) || (n >= 1 && m[0] == 0x47 && n >= 64); /* TS */
}

const BKImAnimCodec *const imb_movie_codecs[] = {
#ifdef BUI_VIDEO_MF
    &imb_movie_mf,
#endif
#ifdef BUI_VIDEO_AVF
    &imb_movie_avf,
#endif
#ifdef BUI_VIDEO_GST
    &imb_movie_gst,
#endif
    NULL,
};

const char *BK_video_backends(void)
{
  static char names[96];
  if (!names[0]) {
    size_t n = 0;
    for (int i = 0; imb_movie_codecs[i]; i++) {
      const char *s = imb_movie_codecs[i]->name;
      const size_t len = strlen(s);
      if (n + len + 2 >= sizeof(names)) {
        break;
      }
      if (n) {
        names[n++] = ',';
      }
      memcpy(names + n, s, len);
      n += len;
    }
    names[n] = '\0';
  }
  return names;
}
