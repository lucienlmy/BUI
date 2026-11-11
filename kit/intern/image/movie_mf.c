/* Video on Windows: Media Foundation's source reader (the OS demuxers and
 * decoders: H.264, HEVC, VP9 and AV1 where their extensions are installed,
 * MPEG-4, WMV), with DXVA hardware decoding through a shared Direct3D 11
 * device and the reader's video processor scaling to the size drawn on
 * the GPU (MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING). Frames come
 * back as NV12 at about the size drawn, copied into the frame anim.c gives.
 *
 * mfplat.dll, mfreadwrite.dll, d3d11.dll and ole32.dll are loaded on first
 * use: Windows "N" editions (no Media Feature Pack) start normally and just
 * have no video. The reader is free-threaded (COM MTA): one step at a time
 * runs on whichever worker takes it. The sound is a second reader on the
 * audio track, float out of the OS decoder (its resampler makes the device's
 * rate), pulled by the audio device's thread (audio/audio.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef BUI_VIDEO_MF

#  define COBJMACROS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <initguid.h> /* the GUIDs below, defined here: nothing links mfuuid.lib */

#  include <d3d11.h>
#  include <mfapi.h>
#  include <mferror.h>
#  include <mfidl.h>
#  include <mfreadwrite.h>

#  include <string.h>

#  include "MEM_guardedalloc.h"

#  include "image.h"

/* -------------------------------------------------------------------- */
/** \name The system libraries, loaded once
 * \{ */

typedef HRESULT(WINAPI *MFStartupFn)(ULONG, DWORD);
typedef HRESULT(WINAPI *MFCreateAttributesFn)(IMFAttributes **, UINT32);
typedef HRESULT(WINAPI *MFCreateMediaTypeFn)(IMFMediaType **);
typedef HRESULT(WINAPI *MFCreateDXGIDeviceManagerFn)(UINT *, IMFDXGIDeviceManager **);
typedef HRESULT(WINAPI *MFCreateSourceReaderFromURLFn)(LPCWSTR, IMFAttributes *,
                                                       IMFSourceReader **);
typedef HRESULT(WINAPI *CoInitializeExFn)(LPVOID, DWORD);
typedef HRESULT(WINAPI *D3D11CreateDeviceFn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                             const D3D_FEATURE_LEVEL *, UINT, UINT,
                                             ID3D11Device **, D3D_FEATURE_LEVEL *,
                                             ID3D11DeviceContext **);

static struct {
  INIT_ONCE once;
  bool ok;
  MFCreateAttributesFn create_attributes;
  MFCreateMediaTypeFn create_media_type;
  MFCreateSourceReaderFromURLFn create_reader;
  CoInitializeExFn co_init;
  /* DXVA: one device for every reader (NULL: software decoding) */
  IMFDXGIDeviceManager *manager;
} MF = {INIT_ONCE_STATIC_INIT};

static void *sym(HMODULE dll, const char *name)
{
  return dll ? (void *)GetProcAddress(dll, name) : NULL;
}

/* A video device for DXVA decoding and the reader's video processor. */
static void dxva_init(HMODULE mfplat)
{
  MFCreateDXGIDeviceManagerFn create_manager = (MFCreateDXGIDeviceManagerFn)sym(
      mfplat, "MFCreateDXGIDeviceManager");
  D3D11CreateDeviceFn create_device = (D3D11CreateDeviceFn)sym(LoadLibraryW(L"d3d11.dll"),
                                                               "D3D11CreateDevice");
  ID3D11Device *dev = NULL;
  UINT token = 0;
  if (!create_manager || !create_device ||
      FAILED(create_device(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           NULL, 0, D3D11_SDK_VERSION, &dev, NULL, NULL)))
  {
    return;
  }
  /* readers on several workers share it */
  ID3D10Multithread *mt = NULL;
  if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D10Multithread, (void **)&mt))) {
    ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
    ID3D10Multithread_Release(mt);
  }
  if (SUCCEEDED(create_manager(&token, &MF.manager)) &&
      FAILED(IMFDXGIDeviceManager_ResetDevice(MF.manager, (IUnknown *)dev, token)))
  {
    IMFDXGIDeviceManager_Release(MF.manager);
    MF.manager = NULL;
  }
  ID3D11Device_Release(dev); /* the manager keeps it for the process */
}

static BOOL CALLBACK load(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
  (void)once;
  (void)param;
  (void)ctx;
  HMODULE mfplat = LoadLibraryW(L"mfplat.dll"), mfrw = LoadLibraryW(L"mfreadwrite.dll");
  MFStartupFn startup = (MFStartupFn)sym(mfplat, "MFStartup");
  MF.create_attributes = (MFCreateAttributesFn)sym(mfplat, "MFCreateAttributes");
  MF.create_media_type = (MFCreateMediaTypeFn)sym(mfplat, "MFCreateMediaType");
  MF.create_reader = (MFCreateSourceReaderFromURLFn)sym(mfrw, "MFCreateSourceReaderFromURL");
  MF.co_init = (CoInitializeExFn)sym(LoadLibraryW(L"ole32.dll"), "CoInitializeEx");
  if (!startup || !MF.create_attributes || !MF.create_media_type || !MF.create_reader ||
      !MF.co_init)
  {
    return TRUE; /* no Media Foundation: no video, nothing else changes */
  }
  MF.co_init(NULL, COINIT_MULTITHREADED);
  MF.ok = SUCCEEDED(startup(MF_VERSION, MFSTARTUP_LITE)); /* no network sources */
  if (MF.ok) {
    dxva_init(mfplat);
  }
  return TRUE;
}

/* Media Foundation, ready on this thread (COM MTA). */
static bool mf_ready(void)
{
#  ifdef _MSC_VER
  static __declspec(thread) bool com;
#  else
  static _Thread_local bool com;
#  endif
  InitOnceExecuteOnce(&MF.once, load, NULL, NULL);
  if (MF.ok && !com) {
    const HRESULT hr = MF.co_init(NULL, COINIT_MULTITHREADED);
    com = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; /* an STA host thread works too */
  }
  return MF.ok && com;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The codec
 * \{ */

/* declared without DEFINE_GUID by the Windows SDK's mfobjects.h: defined here */
static const GUID k_IID_IMF2DBuffer = {
    0x7dc9d5f9, 0x9ed9, 0x44ec, {0x9b, 0xbf, 0x06, 0x00, 0xbb, 0x58, 0x9f, 0xbb}};
static const GUID k_IID_IMFDXGIBuffer = {
    0xe7174cfa, 0x1c9e, 0x48b1, {0x88, 0x66, 0x62, 0x62, 0x26, 0xbf, 0xc2, 0x58}};

typedef struct MFMovie {
  IMFSourceReader *reader;
  int w, h;         /* output frame (NV12) */
  int src_w, src_h; /* the coded frame */
  bool interlaced;
  double frame_dur; /* from the frame rate, for samples without a duration */
} MFMovie;

#  define VIDEO ((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM)
#  define AUDIO ((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM)

static void mf_close(void *state)
{
  MFMovie *m = (MFMovie *)state;
  if (m->reader) {
    IMFSourceReader_Release(m->reader);
  }
  MEM_freeN(m);
}

/* The output's size, as the reader set it up. */
static bool output_size(MFMovie *m)
{
  IMFMediaType *t = NULL;
  UINT64 size = 0;
  if (FAILED(IMFSourceReader_GetCurrentMediaType(m->reader, VIDEO, &t))) {
    return false;
  }
  const bool ok = SUCCEEDED(IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &size));
  IMFMediaType_Release(t);
  m->w = (int)(size >> 32);
  m->h = (int)(size & 0xFFFFFFFF);
  return ok;
}

/* NV12 at out_w x out_h (the reader's video processor scales and converts). */
static bool set_output(MFMovie *m, int out_w, int out_h, bool interlaced)
{
  IMFMediaType *t = NULL;
  if (FAILED(MF.create_media_type(&t))) {
    return false;
  }
  IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
  IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
  IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE, (UINT64)out_w << 32 | (UINT32)out_h);
  IMFMediaType_SetUINT64(t, &MF_MT_PIXEL_ASPECT_RATIO, (UINT64)1 << 32 | 1);
  if (interlaced) {
    IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); /* deinterlace */
  }
  const HRESULT hr = IMFSourceReader_SetCurrentMediaType(m->reader, VIDEO, NULL, t);
  IMFMediaType_Release(t);
  return SUCCEEDED(hr) && output_size(m);
}

static wchar_t *wide_path(const char *path)
{
  const int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  wchar_t *w = n > 0 ? (wchar_t *)MEM_mallocN(sizeof(wchar_t) * (size_t)n, "movie path") : NULL;
  if (w && !MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n)) {
    MEM_freeN(w);
    w = NULL;
  }
  return w;
}

/* A reader of `path`: for frames (`video`: DXVA, the video processor) or for sound. */
static IMFSourceReader *open_reader(const char *path, bool video)
{
  IMFAttributes *attr = NULL;
  IMFSourceReader *reader = NULL;
  wchar_t *wpath = wide_path(path);
  if (wpath && SUCCEEDED(MF.create_attributes(&attr, 4))) {
    if (video) {
      IMFAttributes_SetUINT32(attr, &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
      IMFAttributes_SetUINT32(attr, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
      if (MF.manager) {
        IMFAttributes_SetUnknown(attr, &MF_SOURCE_READER_D3D_MANAGER, (IUnknown *)MF.manager);
      }
    }
    if (FAILED(MF.create_reader(wpath, attr, &reader))) {
      reader = NULL;
    }
    IMFAttributes_Release(attr);
  }
  MEM_freeN(wpath);
  return reader;
}

static void *mf_open(const char *path, const uint8_t *mem, size_t size, BKImAnimInfo *r_info)
{
  (void)mem;
  (void)size;
  if (!path || !mf_ready()) {
    return NULL;
  }
  MFMovie *m = (MFMovie *)MEM_callocN(sizeof(MFMovie), "MFMovie");
  m->reader = open_reader(path, true);
  IMFMediaType *native = NULL;
  if (m->reader && SUCCEEDED(IMFSourceReader_GetNativeMediaType(m->reader, AUDIO, 0, &native))) {
    r_info->sound = true;
    IMFMediaType_Release(native);
    native = NULL;
  }
  if (!m->reader ||
      FAILED(IMFSourceReader_SetStreamSelection(m->reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS,
                                                FALSE)) ||
      FAILED(IMFSourceReader_SetStreamSelection(m->reader, VIDEO, TRUE)) ||
      FAILED(IMFSourceReader_GetNativeMediaType(m->reader, VIDEO, 0, &native)))
  {
    mf_close(m);
    return NULL;
  }
  /* the intrinsic size: the coded frame through its pixel aspect ratio */
  UINT64 fs = 0, par = 0, rate = 0;
  UINT32 matrix = 0, interlace = MFVideoInterlace_Progressive;
  IMFMediaType_GetUINT64(native, &MF_MT_FRAME_SIZE, &fs);
  const bool has_par = SUCCEEDED(IMFMediaType_GetUINT64(native, &MF_MT_PIXEL_ASPECT_RATIO, &par));
  IMFMediaType_GetUINT64(native, &MF_MT_FRAME_RATE, &rate);
  IMFMediaType_GetUINT32(native, &MF_MT_YUV_MATRIX, &matrix);
  IMFMediaType_GetUINT32(native, &MF_MT_INTERLACE_MODE, &interlace);
  IMFMediaType_Release(native);
  m->src_w = (int)(fs >> 32);
  m->src_h = (int)(fs & 0xFFFFFFFF);
  int src_w = m->src_w, src_h = m->src_h;
  const UINT32 pn = (UINT32)(par >> 32), pd = (UINT32)par;
  if (has_par && pn && pd && pn != pd) {
    src_w = (int)((double)src_w * pn / pd + 0.5);
  }
  m->interlaced = interlace != MFVideoInterlace_Progressive;
  const UINT32 rn = (UINT32)(rate >> 32), rd = (UINT32)rate;
  m->frame_dur = rn && rd ? (double)rd / rn : 1.0 / 30.0;
  PROPVARIANT dur;
  memset(&dur, 0, sizeof(dur)); /* VT_UI8: nothing to clear */
  r_info->duration =
      SUCCEEDED(IMFSourceReader_GetPresentationAttribute(
          m->reader, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &dur)) &&
              dur.vt == VT_UI8 ?
          (double)dur.uhVal.QuadPart / 1e7 :
          0.0;
  r_info->src_w = src_w;
  r_info->src_h = src_h;
  r_info->loops = 1;
  r_info->format = matrix == MFVideoTransferMatrix_BT601 ? BUI_PIXMAP_NV12_BT601 :
                   matrix == MFVideoTransferMatrix_BT709 ? BUI_PIXMAP_NV12_BT709 :
                   src_h >= 720                          ? BUI_PIXMAP_NV12_BT709 :
                                                           BUI_PIXMAP_NV12_BT601;
  return m;
}

static bool mf_output(void *state, int w, int h, BKImAnimInfo *r_info)
{
  MFMovie *m = (MFMovie *)state;
  /* the size asked (never above the coded frame), even for NV12; the
   * coded size when the video processor can't scale (no NV12 at all: no
   * decoder for this format) */
  const int out_w = BK_MAX(2, BK_MIN(w, m->src_w) & ~1), out_h = BK_MAX(2, BK_MIN(h, m->src_h) & ~1);
  if (!set_output(m, out_w, out_h, m->interlaced) &&
      !set_output(m, BK_MAX(2, m->src_w & ~1), BK_MAX(2, m->src_h & ~1), m->interlaced))
  {
    return false;
  }
  r_info->w = m->w;
  r_info->h = m->h;
  return true;
}

/* The frame's planes into `dst` (NV12, its size). */
static bool copy_frame(MFMovie *m, IMFSample *sample, BKImBuf *dst)
{
  IMFMediaBuffer *buf = NULL;
  if (FAILED(IMFSample_GetBufferByIndex(sample, 0, &buf))) {
    return false;
  }
  /* rows of a 2D buffer are `pitch` apart; chroma starts after the plane's
   * allocated height (a GPU surface may be taller than the frame) */
  IMF2DBuffer *b2 = NULL;
  IMFDXGIBuffer *dx = NULL;
  BYTE *scan0 = NULL;
  LONG pitch = 0;
  int plane_h = m->h;
  bool locked2d = false;
  if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &k_IID_IMFDXGIBuffer, (void **)&dx))) {
    ID3D11Texture2D *tex = NULL;
    if (SUCCEEDED(IMFDXGIBuffer_GetResource(dx, &IID_ID3D11Texture2D, (void **)&tex))) {
      D3D11_TEXTURE2D_DESC td;
      ID3D11Texture2D_GetDesc(tex, &td);
      plane_h = (int)td.Height;
      ID3D11Texture2D_Release(tex);
    }
    IMFDXGIBuffer_Release(dx);
  }
  if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &k_IID_IMF2DBuffer, (void **)&b2))) {
    locked2d = SUCCEEDED(IMF2DBuffer_Lock2D(b2, &scan0, &pitch));
  }
  DWORD len = 0;
  if (!locked2d && SUCCEEDED(IMFMediaBuffer_Lock(buf, &scan0, NULL, &len))) {
    pitch = m->w; /* a contiguous buffer: packed rows */
  }
  const bool ok = scan0 && pitch >= dst->x && plane_h >= dst->y;
  if (ok) {
    uint8_t *y = (uint8_t *)dst->rect, *uv = y + (size_t)dst->x * dst->y;
    const BYTE *sy = scan0, *suv = scan0 + (size_t)pitch * plane_h;
    for (int r = 0; r < dst->y; r++) {
      memcpy(y + (size_t)r * dst->x, sy + (size_t)r * pitch, (size_t)dst->x);
    }
    for (int r = 0; r < dst->y / 2; r++) {
      memcpy(uv + (size_t)r * dst->x, suv + (size_t)r * pitch, (size_t)dst->x);
    }
  }
  if (locked2d) {
    IMF2DBuffer_Unlock2D(b2);
  }
  else if (scan0) {
    IMFMediaBuffer_Unlock(buf);
  }
  if (b2) {
    IMF2DBuffer_Release(b2);
  }
  IMFMediaBuffer_Release(buf);
  return ok;
}

static int mf_next(void *state, BKImBuf *dst, double *r_pts, double *r_dur)
{
  MFMovie *m = (MFMovie *)state;
  for (int gaps = 0; gaps < 64; gaps++) {
    DWORD stream = 0, flags = 0;
    LONGLONG ts = 0;
    IMFSample *sample = NULL;
    if (FAILED(IMFSourceReader_ReadSample(m->reader, VIDEO, 0, &stream, &flags, &ts, &sample))) {
      return -1;
    }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      const int w = m->w, h = m->h;
      if (!output_size(m) || m->w != w || m->h != h) {
        if (sample) {
          IMFSample_Release(sample);
        }
        return -1; /* resized mid-stream: anim.c reopens at its size (a seek) */
      }
    }
    if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
      if (sample) {
        IMFSample_Release(sample);
      }
      return (flags & MF_SOURCE_READERF_ERROR) ? -1 : 0;
    }
    if (!sample) {
      continue; /* a stream tick (gap): read on */
    }
    LONGLONG d = 0;
    const bool has_dur = SUCCEEDED(IMFSample_GetSampleDuration(sample, &d)) && d > 0;
    const bool ok = copy_frame(m, sample, dst);
    IMFSample_Release(sample);
    if (!ok) {
      return -1;
    }
    *r_pts = (double)ts / 1e7;
    *r_dur = has_dur ? (double)d / 1e7 : m->frame_dur;
    return 1;
  }
  return -1;
}

/* Frames and sound: the reader goes to `t` (the keyframe or block before it). */
static bool seek_reader(IMFSourceReader *reader, double t)
{
  PROPVARIANT pos;
  memset(&pos, 0, sizeof(pos));
  pos.vt = VT_I8;
  pos.hVal.QuadPart = (LONGLONG)(t * 1e7);
  static const GUID time_format = {0}; /* GUID_NULL: 100 ns units */
  return SUCCEEDED(IMFSourceReader_SetCurrentPosition(reader, &time_format, &pos));
}

static bool mf_seek(void *state, double t)
{
  return seek_reader(((MFMovie *)state)->reader, t);
}

static size_t mf_bytes(const void *state)
{
  const MFMovie *m = (const MFMovie *)state;
  /* what this process holds for it: the reader's output samples (an estimate;
   * decoder surfaces live in video memory) */
  return sizeof(*m) + (size_t)m->w * m->h * 3;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sound: a second reader on the audio track, float out of the OS decoder
 * \{ */

typedef struct MFSound {
  IMFSourceReader *reader;
  int channels;
  float *buf; /* the last block (frames x channels) */
  size_t cap; /* floats */
} MFSound;

static void mf_audio_close(void *state)
{
  MFSound *s = (MFSound *)state;
  if (s->reader) {
    IMFSourceReader_Release(s->reader);
  }
  if (s->buf) {
    MEM_freeN(s->buf);
  }
  MEM_freeN(s);
}

/* Float out, at `rate` and `channels` when they are given (the reader's
 * resampler), else at the track's own. */
static bool set_audio_output(IMFSourceReader *reader, int rate, int channels)
{
  IMFMediaType *t = NULL;
  if (FAILED(MF.create_media_type(&t))) {
    return false;
  }
  IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
  IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFAudioFormat_Float);
  if (rate > 0) {
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)(4 * channels));
    IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(4 * channels * rate));
  }
  const HRESULT hr = IMFSourceReader_SetCurrentMediaType(reader, AUDIO, NULL, t);
  IMFMediaType_Release(t);
  return SUCCEEDED(hr);
}

static void *mf_audio_open(const char *path, int rate, int channels, int *r_rate, int *r_channels)
{
  if (!path || !mf_ready()) {
    return NULL;
  }
  MFSound *s = (MFSound *)MEM_callocN(sizeof(MFSound), "MFSound");
  s->reader = open_reader(path, false);
  IMFMediaType *t = NULL;
  UINT32 r = 0, c = 0;
  if (!s->reader ||
      FAILED(IMFSourceReader_SetStreamSelection(s->reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS,
                                                FALSE)) ||
      FAILED(IMFSourceReader_SetStreamSelection(s->reader, AUDIO, TRUE)) || /* no audio track */
      !(set_audio_output(s->reader, rate, channels) || set_audio_output(s->reader, 0, 0)) ||
      FAILED(IMFSourceReader_GetCurrentMediaType(s->reader, AUDIO, &t)))
  {
    mf_audio_close(s);
    return NULL;
  }
  IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
  IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, &c);
  IMFMediaType_Release(t);
  s->channels = (int)c;
  *r_rate = (int)r;
  *r_channels = (int)c;
  return s;
}

static int mf_audio_read(void *state, const float **r_data, double *r_pts)
{
  MFSound *s = (MFSound *)state;
  for (int gaps = 0; gaps < 64; gaps++) {
    DWORD stream = 0, flags = 0;
    LONGLONG ts = 0;
    IMFSample *sample = NULL;
    if (FAILED(IMFSourceReader_ReadSample(s->reader, AUDIO, 0, &stream, &flags, &ts, &sample))) {
      return -1;
    }
    if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
      if (sample) {
        IMFSample_Release(sample);
      }
      return (flags & MF_SOURCE_READERF_ERROR) ? -1 : 0;
    }
    if (!sample) {
      continue; /* a stream tick */
    }
    IMFMediaBuffer *buf = NULL;
    BYTE *p = NULL;
    DWORD len = 0;
    int frames = -1;
    if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buf)) &&
        SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len)))
    {
      const size_t n = len / sizeof(float);
      if (n > s->cap) {
        if (s->buf) {
          MEM_freeN(s->buf);
        }
        s->buf = (float *)MEM_mallocN(sizeof(float) * n, "MFSound block");
        s->cap = n;
      }
      memcpy(s->buf, p, n * sizeof(float));
      frames = (int)(n / (size_t)s->channels);
      IMFMediaBuffer_Unlock(buf);
    }
    if (buf) {
      IMFMediaBuffer_Release(buf);
    }
    IMFSample_Release(sample);
    if (frames == 0) {
      continue;
    }
    *r_data = s->buf;
    *r_pts = (double)ts / 1e7;
    return frames;
  }
  return -1;
}

static bool mf_audio_seek(void *state, double t)
{
  return seek_reader(((MFSound *)state)->reader, t);
}

const BKImAnimCodec imb_movie_mf = {"mf", true, imb_is_a_movie, mf_open, mf_output, mf_next,
                                    mf_seek, mf_bytes, mf_close, mf_audio_open, mf_audio_read,
                                    mf_audio_seek, mf_audio_close};

/** \} */

#endif /* BUI_VIDEO_MF */
