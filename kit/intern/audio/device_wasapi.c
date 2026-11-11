/* Sound on Windows: WASAPI in shared mode (the system mixer), event driven,
 * float samples at the mixer's rate and channels: the stream needs no
 * conversion (AUTOCONVERTPCM covers a mixer that isn't float).
 *
 * The stream is opened RAW (AUDCLIENT_STREAMOPTIONS_RAW: the system's effects chain is skipped).
 * Opening a stream through an effects chain (Realtek's, Dolby, Nahimic, "audio
 * enhancements") took 1.4 to 1.9 s here, while the same call takes half a second RAW, and every
 * other Windows audio API (waveOut, DirectSound, XAudio2, miniaudio) goes through the same chain.
 * The cost is that the effects aren't applied to a video's sound; BUI_AUDIO_EFFECTS=1 keeps them.
 *
 * Only about two device periods of sound are queued ahead of the speakers (not the whole buffer):
 * what is queued still plays after a pause, and a short queue keeps that short. The stream's thread
 * runs at the "Audio" priority class of the multimedia scheduler so the short queue doesn't run dry.
 *
 * Everything runs on the stream's thread: opening the default output, the
 * fill loop, and reopening when that output goes away (headphones unplugged,
 * the default changed); the position goes on from where it was, so the clock
 * a video follows doesn't jump. ole32.dll is loaded on first use, as
 * Media Foundation is (movie_mf.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef BUI_AUDIO_WASAPI

#  define COBJMACROS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <audioclient.h>
#  include <mmdeviceapi.h>
#  include <mmreg.h>

#  include "MEM_guardedalloc.h"

#  include "audio.h"

/* the GUIDs, here: nothing links uuid.lib */
static const CLSID k_CLSID_MMDeviceEnumerator = {
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const IID k_IID_IMMDeviceEnumerator = {
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const IID k_IID_IAudioClient = {
    0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const IID k_IID_IAudioRenderClient = {
    0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
static const IID k_IID_IAudioClock = {
    0xcd63314f, 0x3fba, 0x4a1b, {0x81, 0x2c, 0xef, 0x96, 0x35, 0x87, 0x28, 0xe7}};
static const GUID k_SUBTYPE_IEEE_FLOAT = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

static const IID k_IID_IAudioClient2 = {
    0x726778cd, 0xf60a, 0x4eda, {0x82, 0xde, 0xe4, 0x76, 0x10, 0xcd, 0x78, 0xaa}};

typedef HRESULT(WINAPI *CoInitializeExFn)(LPVOID, DWORD);
typedef HANDLE(WINAPI *AvSetMmThreadCharacteristicsFn)(LPCWSTR, LPDWORD);
typedef HRESULT(WINAPI *CoCreateInstanceFn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef void(WINAPI *CoTaskMemFreeFn)(LPVOID);
static struct {
  INIT_ONCE once;
  CoInitializeExFn co_init;
  CoCreateInstanceFn create;
  CoTaskMemFreeFn task_free;
  AvSetMmThreadCharacteristicsFn mmcss; /* avrt.dll: NULL without it */
  bool effects;                         /* BUI_AUDIO_EFFECTS: keep the system's effects (not RAW) */
} COM = {INIT_ONCE_STATIC_INIT};

static BOOL CALLBACK com_load(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
  (void)once;
  (void)param;
  (void)ctx;
  HMODULE ole = LoadLibraryW(L"ole32.dll");
  COM.co_init = ole ? (CoInitializeExFn)(void *)GetProcAddress(ole, "CoInitializeEx") : NULL;
  COM.create = ole ? (CoCreateInstanceFn)(void *)GetProcAddress(ole, "CoCreateInstance") : NULL;
  COM.task_free = ole ? (CoTaskMemFreeFn)(void *)GetProcAddress(ole, "CoTaskMemFree") : NULL;
  HMODULE avrt = LoadLibraryW(L"avrt.dll");
  COM.mmcss = avrt ? (AvSetMmThreadCharacteristicsFn)(void *)GetProcAddress(
                         avrt, "AvSetMmThreadCharacteristicsW") :
                     NULL;
  char flag[8];
  COM.effects = GetEnvironmentVariableA("BUI_AUDIO_EFFECTS", flag, sizeof(flag)) > 0 && flag[0] != '0';
  return TRUE;
}

struct BKAudioDevice {
  BKAudioFill fill;
  void *data;
  HANDLE thread, wake; /* `wake`: the stream's buffer event (and stop) */
  volatile LONG stop;
  CRITICAL_SECTION lock; /* `clock`, `base` for played() */
  IAudioClock *clock;
  UINT64 freq;
  double base;       /* where this stream starts in the whole (seconds) */
  double written;    /* seconds every stream was given: the next one starts there */
  int rate, channels;
};

/* The open stream (released by stream_close). */
typedef struct Stream {
  IAudioClient *client;
  IAudioRenderClient *render;
  UINT32 frames; /* its buffer */
  UINT32 target; /* the sound kept queued ahead (frames): about two device periods */
} Stream;

static void stream_close(BKAudioDevice *d, Stream *s)
{
  if (s->client) {
    IAudioClient_Stop(s->client);
  }
  EnterCriticalSection(&d->lock);
  if (d->clock) {
    IAudioClock_Release(d->clock);
    d->clock = NULL;
  }
  /* the next stream goes on where this one's samples ended (what it still
   * held is lost, and fill() wrote past it): the clock stays on the sound */
  d->base = d->written;
  LeaveCriticalSection(&d->lock);
  if (s->render) {
    IAudioRenderClient_Release(s->render);
  }
  if (s->client) {
    IAudioClient_Release(s->client);
  }
  memset(s, 0, sizeof(*s));
}

/* The default output, float at its mixer's rate and channels. */
static bool stream_open(BKAudioDevice *d, Stream *s)
{
  IMMDeviceEnumerator *en = NULL;
  IMMDevice *dev = NULL;
  WAVEFORMATEX *mix = NULL;
  IAudioClock *clock = NULL;
  bool ok = SUCCEEDED(COM.create(&k_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                 &k_IID_IMMDeviceEnumerator, (void **)&en)) &&
            SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) &&
            SUCCEEDED(IMMDevice_Activate(dev, &k_IID_IAudioClient, CLSCTX_ALL, NULL,
                                         (void **)&s->client)) &&
            SUCCEEDED(IAudioClient_GetMixFormat(s->client, &mix));
  if (ok) {
    WAVEFORMATEXTENSIBLE fmt;
    memset(&fmt, 0, sizeof(fmt));
    const int channels = mix->nChannels > 8 ? 8 : mix->nChannels;
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = (WORD)channels;
    fmt.Format.nSamplesPerSec = mix->nSamplesPerSec;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = (WORD)(4 * channels);
    fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
    fmt.Format.cbSize = sizeof(fmt) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ?
                            ((WAVEFORMATEXTENSIBLE *)mix)->dwChannelMask :
                        channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) :
                                        SPEAKER_FRONT_CENTER;
    fmt.SubFormat = k_SUBTYPE_IEEE_FLOAT;
    const REFERENCE_TIME buffer = 500000; /* 50 ms: room, not what is queued (feed) */
    if (!COM.effects) {
      IAudioClient2 *c2 = NULL;
      if (SUCCEEDED(IAudioClient_QueryInterface(s->client, &k_IID_IAudioClient2, (void **)&c2))) {
        AudioClientProperties props;
        memset(&props, 0, sizeof(props));
        props.cbSize = sizeof(props);
        props.eCategory = AudioCategory_Media;
        props.Options = AUDCLNT_STREAMOPTIONS_RAW; /* not supported by every driver: then as before */
        IAudioClient2_SetClientProperties(c2, &props);
        IAudioClient2_Release(c2);
      }
    }
    ok = SUCCEEDED(IAudioClient_Initialize(
             s->client, AUDCLNT_SHAREMODE_SHARED,
             AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
             buffer, 0, (WAVEFORMATEX *)&fmt, NULL)) &&
         SUCCEEDED(IAudioClient_SetEventHandle(s->client, d->wake)) &&
         SUCCEEDED(IAudioClient_GetBufferSize(s->client, &s->frames)) &&
         SUCCEEDED(IAudioClient_GetService(s->client, &k_IID_IAudioRenderClient,
                                           (void **)&s->render)) &&
         SUCCEEDED(IAudioClient_GetService(s->client, &k_IID_IAudioClock, (void **)&clock));
    d->rate = (int)fmt.Format.nSamplesPerSec;
    d->channels = channels;
    REFERENCE_TIME period = 0;
    IAudioClient_GetDevicePeriod(s->client, &period, NULL);
    /* two periods (at least 20 ms), within the buffer */
    const double seconds = period > 100000 ? 2.0 * (double)period / 1e7 : 0.021;
    s->target = (UINT32)((seconds < 0.020 ? 0.020 : seconds) * (double)d->rate);
    if (s->target > s->frames) {
      s->target = s->frames;
    }
  }
  if (ok) {
    UINT64 freq = 0;
    IAudioClock_GetFrequency(clock, &freq);
    EnterCriticalSection(&d->lock);
    d->clock = clock;
    d->freq = freq;
    LeaveCriticalSection(&d->lock);
    clock = NULL;
  }
  if (clock) {
    IAudioClock_Release(clock);
  }
  if (mix) {
    COM.task_free(mix);
  }
  if (dev) {
    IMMDevice_Release(dev);
  }
  if (en) {
    IMMDeviceEnumerator_Release(en);
  }
  if (!ok) {
    stream_close(d, s);
  }
  return ok;
}

/* Fill what the device has room for; false when the stream is gone. */
static bool feed(BKAudioDevice *d, Stream *s)
{
  UINT32 padding = 0;
  BYTE *buf = NULL;
  if (FAILED(IAudioClient_GetCurrentPadding(s->client, &padding))) {
    return false;
  }
  const UINT32 room = padding < s->target ? s->target - padding : 0; /* up to the target only */
  if (room == 0) {
    return true;
  }
  if (FAILED(IAudioRenderClient_GetBuffer(s->render, room, &buf))) {
    return false;
  }
  d->fill(d->data, (float *)buf, (int)room, d->rate, d->channels);
  d->written += (double)room / d->rate;
  return SUCCEEDED(IAudioRenderClient_ReleaseBuffer(s->render, room, 0));
}

static DWORD WINAPI stream_thread(LPVOID param)
{
  BKAudioDevice *d = (BKAudioDevice *)param;
  COM.co_init(NULL, COINIT_MULTITHREADED);
  if (COM.mmcss) {
    DWORD task = 0;
    COM.mmcss(L"Audio", &task); /* not going back: the thread ends with the stream */
  }
  Stream s;
  memset(&s, 0, sizeof(s));
  while (!d->stop) {
    if (!s.client) {
      if (!stream_open(d, &s) || !feed(d, &s) || FAILED(IAudioClient_Start(s.client))) {
        stream_close(d, &s);
        WaitForSingleObject(d->wake, 1000); /* no output now: try again in a while */
        continue;
      }
    }
    WaitForSingleObject(d->wake, 200);
    if (!d->stop && !feed(d, &s)) {
      stream_close(d, &s); /* the output went away: open the default again */
    }
  }
  stream_close(d, &s);
  return 0;
}

BKAudioDevice *bk_audio_device_open(BKAudioFill fill, void *data)
{
  InitOnceExecuteOnce(&COM.once, com_load, NULL, NULL);
  if (!COM.co_init || !COM.create || !COM.task_free) {
    return NULL;
  }
  BKAudioDevice *d = (BKAudioDevice *)MEM_callocN(sizeof(BKAudioDevice), "BKAudioDevice");
  d->fill = fill;
  d->data = data;
  InitializeCriticalSection(&d->lock);
  d->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
  d->thread = d->wake ? CreateThread(NULL, 0, stream_thread, d, 0, NULL) : NULL;
  if (!d->thread) {
    bk_audio_device_close(d);
    return NULL;
  }
  return d;
}

double bk_audio_device_played(BKAudioDevice *d)
{
  double t = 0.0;
  EnterCriticalSection(&d->lock);
  t = d->base;
  UINT64 pos = 0;
  if (d->clock && d->freq && SUCCEEDED(IAudioClock_GetPosition(d->clock, &pos, NULL))) {
    t += (double)pos / (double)d->freq;
  }
  LeaveCriticalSection(&d->lock);
  return t;
}

void bk_audio_device_close(BKAudioDevice *d)
{
  if (!d) {
    return;
  }
  if (d->thread) {
    InterlockedExchange(&d->stop, 1);
    SetEvent(d->wake);
    WaitForSingleObject(d->thread, INFINITE);
    CloseHandle(d->thread);
  }
  if (d->wake) {
    CloseHandle(d->wake);
  }
  DeleteCriticalSection(&d->lock);
  MEM_freeN(d);
}

#endif /* BUI_AUDIO_WASAPI */
