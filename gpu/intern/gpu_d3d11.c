/* BUI GPU renderer: Direct3D 11 backend — see gpu_d3d11.h.
 *
 * Like Blender's widget drawing (interface_widgets.cc batches widget bases
 * into GPU_SHADER_2D_WIDGET_BASE_INST draws), every primitive becomes one
 * instance of a single uber-shader, so a frame of thousands of widgets and
 * text runs is a handful of DrawInstanced calls. Differences from Blender,
 * for a retained UI:
 *  - frames render into a persistent target: only damaged regions are
 *    redrawn (the kit's damage tracking) and the swap chain gets dirty rects,
 *    so the compositor only recomposes what changed;
 *  - text is pre-shaped glyph runs (the same masks the software raster draws)
 *    packed into one R8 atlas, images into one BGRA atlas;
 *  - the device is created on a worker thread so the first frame never waits
 *    for the driver (the software raster draws until it is ready).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#if defined(_WIN32) && defined(BUI_GPU) /* BUI_GPU: features.json */

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  define COBJMACROS
#  include <windows.h>
#  include <d3d11.h>
#  include <d3d11_1.h>
#  include <dxgi1_2.h>

#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>

#  include "GPU_raster.h"
#  include "gpu_d3d11.h"
#  include "gpu_d3d11_shaders.h"

static const GUID k_IID_IDXGIDevice = {
    0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
static const GUID k_IID_IDXGIFactory2 = {
    0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
static const GUID k_IID_ID3D11Texture2D = {
    0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
static const GUID k_IID_ID3D11DeviceContext1 = {
    0xbb2c6faa, 0xb5fb, 0x4082, {0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1}};

#  define REL(p) \
    do { \
      if (p) { \
        IUnknown_Release((IUnknown *)(p)); \
        (p) = NULL; \
      } \
    } while (0)

/* -------------------------------------------------------------------- */
/** \name Atlases (shelf packing)
 * \{ */

#  define SHELVES 128

typedef struct Atlas {
  int size;
  int bpp; /* 1 (R8) or 4 (BGRA8) */
  ID3D11Texture2D *tex;
  ID3D11ShaderResourceView *srv;
  int shelf_y[SHELVES], shelf_h[SHELVES], shelf_x[SHELVES];
  int nshelves, next_y;
} Atlas;

/* cache entry: key -> placement */
typedef struct Slot {
  uint64_t k1, k2; /* 0 = empty */
  int x, y;
} Slot;

#  define MASK_MAP 16384 /* power of two */
#  define MASK_MAP_FILL 12000
#  define IMG_MAP 1024
#  define IMG_MAP_FILL 700

/* Slug glyph data: curves (2 RGBA32F texels each) and bands (RG32_UINT
 * texels), both addressed linearly over 4096-wide textures. */
#  define SLUG_W 4096
#  define SLUG_H 64
#  define SLUG_MAP 8192 /* power of two */
#  define SLUG_MAP_FILL 6000
#  define SLUG_MAX_BANDS 16

typedef struct SlugGlyph {
  uint32_t key; /* (face + 1) << 16 | glyph; 0 = empty */
  int ok;       /* 0: empty glyph */
  float bbox[4], band[4];
  int loc[4];
} SlugGlyph;

typedef struct Batch {
  int tri; /* 0: quad instances, 1: triangles, 2: textured mesh (indexed) */
  int first, count;
  int img; /* quads: the RGBA texture their images sample (-1 none yet, 0 atlas, n: big[n - 1]) */
  /* meshes: texture (0: white), sampler, scissor x0 y0 x1 y1 */
  int tex, nearest;
  int clip[4];
} Batch;

/* An image too big for the atlas, or a video frame, in a texture of its own. */
#  define BIG_MAX 32
#  define BIG_BUDGET ((size_t)256 << 20) /* bytes of big-image textures kept */
#  define BIG_IDLE 120                   /* frames unused before one is freed */
typedef struct BigTex {
  uint64_t key, base; /* 0 = empty */
  int w, h;
  int format;    /* BUI_PIXMAP_*: NV12 is luma in tex, chroma in uv_tex */
  int stream;    /* `base` is a stream: its frames update the textures in place */
  unsigned used; /* the frame it was last drawn in */
  ID3D11Texture2D *tex, *uv_tex;
  ID3D11ShaderResourceView *srv, *uv_srv;
} BigTex;

/* A registry texture (BUI_texture_*) on this device, by registry slot. */
typedef struct DevTex {
  int id; /* 0 = empty */
  unsigned gen;
  int w, h;
  ID3D11Texture2D *tex;
  ID3D11ShaderResourceView *srv;
} DevTex;

typedef struct TriV {
  float x, y;
  float c[4];
  float clip[4];
} TriV;

struct BuiGpu {
  HWND hwnd;
  volatile LONG state; /* 0 pending, 1 ready, -1 failed */
  HANDLE thread;

  ID3D11Device *dev;
  ID3D11DeviceContext *ctx;
  IDXGIFactory2 *factory;
  IDXGISwapChain1 *swap;
  int sw, sh; /* swap chain size */
  /* alpha: frames keep premultiplied alpha. The swap chain is a bitblt one (DWM blur-behind
   * shows its alpha, as in SDL) and ClearView clears the rects of a redrawn region. */
  int alpha;
  ID3D11DeviceContext1 *ctx1;
  int swap_fresh;

  ID3D11VertexShader *vs_quad, *vs_tri, *vs_mesh;
  ID3D11PixelShader *ps_quad, *ps_tri, *ps_mesh;
  ID3D11InputLayout *il_quad, *il_tri, *il_mesh;
  ID3D11Buffer *cb, *ib, *tb, *mvb, *mib;
  UINT ib_cap, tb_cap, mvb_cap, mib_cap; /* in elements */
  ID3D11BlendState *blend;
  ID3D11RasterizerState *rs, *rs_scissor;
  ID3D11SamplerState *smp_linear, *smp_point;
  ID3D11Texture2D *white_tex; /* 1x1 white: meshes without a texture */
  ID3D11ShaderResourceView *white_srv;
  DevTex *dtex;
  int n_dtex;
  unsigned dtex_free_gen;

  ID3D11Texture2D *target;
  ID3D11RenderTargetView *rtv;
  int tw, th; /* target texture size (>= the frame) */
  int fw, fh; /* the frame size the target holds */
  int w, h;   /* frame size */

  Atlas mask, rgba;
  BigTex big[BIG_MAX];
  size_t big_bytes;
  unsigned frame, forget_seen;
  int max_tex; /* the feature level's largest texture side */
  Slot *mask_map, *img_map;
  int mask_n, img_n;
  unsigned mask_gen;

  int slug; /* -1: off (env or unavailable), 0: not created yet, 1: ready */
  ID3D11Texture2D *curve_tex, *band_tex;
  ID3D11ShaderResourceView *curve_srv, *band_srv;
  SlugGlyph *slug_map;
  int slug_n, curve_next, band_next;
  unsigned slug_gen;

  BuiGpuInst *inst;
  int n_inst, cap_inst;
  TriV *tv;
  int n_tv, cap_tv;
  BuiMeshVertex *mv;
  int n_mv, cap_mv;
  uint32_t *mi;
  int n_mi, cap_mi;
  Batch *b;
  int n_b, cap_b;

  RECT dmg[16];
  int n_dmg, dmg_full;
  /* the last present's damage (whole: the whole frame): with two flip buffers, the buffer being
   * drawn is two frames old, so it needs this frame's damage and that one's, not everything */
  RECT prev[16];
  int n_prev, prev_whole;
};

static int atlas_create(BuiGpu *g, Atlas *a, int size, int bpp)
{
  memset(a, 0, sizeof(*a));
  a->size = size;
  a->bpp = bpp;
  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = td.Height = (UINT)size;
  td.MipLevels = td.ArraySize = 1;
  td.Format = bpp == 1 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(ID3D11Device_CreateTexture2D(g->dev, &td, NULL, &a->tex))) {
    return 0;
  }
  if (FAILED(ID3D11Device_CreateShaderResourceView(g->dev, (ID3D11Resource *)a->tex, NULL,
                                                   &a->srv)))
  {
    REL(a->tex);
    return 0;
  }
  return 1;
}

static void atlas_clear(Atlas *a)
{
  a->nshelves = 0;
  a->next_y = 0;
}

/* Finds room for w x h (+1 px gutter). 0 when full. */
static int atlas_place(Atlas *a, int w, int h, int *r_x, int *r_y)
{
  const int pw = w + 1, ph = h + 1;
  if (pw > a->size || ph > a->size) {
    return 0;
  }
  int best = -1;
  for (int i = 0; i < a->nshelves; i++) {
    if (a->shelf_h[i] >= ph && a->shelf_h[i] <= ph + ph / 4 + 2 && a->shelf_x[i] + pw <= a->size &&
        (best < 0 || a->shelf_h[i] < a->shelf_h[best]))
    {
      best = i;
    }
  }
  if (best < 0) {
    const int sh = (ph + 3) & ~3; /* round up: more strings share a shelf */
    if (a->nshelves == SHELVES || a->next_y + sh > a->size) {
      return 0;
    }
    best = a->nshelves++;
    a->shelf_y[best] = a->next_y;
    a->shelf_h[best] = sh;
    a->shelf_x[best] = 0;
    a->next_y += sh;
  }
  *r_x = a->shelf_x[best];
  *r_y = a->shelf_y[best];
  a->shelf_x[best] += pw;
  return 1;
}

static void atlas_upload(BuiGpu *g, Atlas *a, int x, int y, int w, int h, const void *data,
                         int pitch)
{
  D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
  ID3D11DeviceContext_UpdateSubresource(g->ctx, (ID3D11Resource *)a->tex, 0, &box, data,
                                        (UINT)pitch, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Device (worker thread)
 * \{ */

typedef HRESULT(WINAPI *CreateDeviceFn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                        const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
                                        D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

/* BUI_GPU_DEBUG=1: why the GPU path wasn't taken */
static int fail(const char *what, HRESULT hr)
{
  const char *dbg = getenv("BUI_GPU_DEBUG");
  if (dbg && dbg[0] == '1') {
    fprintf(stderr, "bui gpu: %s failed (0x%08lx)\n", what, (unsigned long)hr);
  }
  return 0;
}

static int device_init(BuiGpu *g)
{
  HMODULE dll = LoadLibraryW(L"d3d11.dll");
  if (!dll) {
    return fail("loading d3d11.dll", 0);
  }
  CreateDeviceFn create = (CreateDeviceFn)(void *)GetProcAddress(dll, "D3D11CreateDevice");
  if (!create) {
    return 0;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  const char *warp = getenv("BUI_GPU_WARP");
  const D3D_DRIVER_TYPE type = (warp && warp[0] == '1') ? D3D_DRIVER_TYPE_WARP :
                                                          D3D_DRIVER_TYPE_HARDWARE;
  D3D_FEATURE_LEVEL got;
  HRESULT hr = create(NULL, type, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4,
                      D3D11_SDK_VERSION, &g->dev, &got, &g->ctx);
  if (FAILED(hr)) { /* 11_1 unknown to the runtime: retry without it */
    hr = create(NULL, type, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3,
                D3D11_SDK_VERSION, &g->dev, &got, &g->ctx);
  }
  if (FAILED(hr)) {
    return fail("D3D11CreateDevice", hr);
  }
  if (g->alpha && FAILED(hr = ID3D11DeviceContext_QueryInterface(g->ctx, &k_IID_ID3D11DeviceContext1,
                                                                  (void **)&g->ctx1)))
  {
    return fail("ID3D11DeviceContext1", hr); /* Windows 8: ClearView */
  }
  g->max_tex = got >= D3D_FEATURE_LEVEL_11_0 ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION : 8192;
  const char *max_env = getenv("BUI_GPU_MAX_TEXTURE"); /* tests: force tiling */
  if (max_env && atoi(max_env) >= 64) {
    g->max_tex = atoi(max_env) < g->max_tex ? atoi(max_env) : g->max_tex;
  }
  IDXGIDevice *xd = NULL;
  IDXGIAdapter *ad = NULL;
  if (FAILED(ID3D11Device_QueryInterface(g->dev, &k_IID_IDXGIDevice, (void **)&xd)) ||
      FAILED(IDXGIDevice_GetAdapter(xd, &ad)) ||
      FAILED(IDXGIAdapter_GetParent(ad, &k_IID_IDXGIFactory2, (void **)&g->factory)))
  {
    REL(ad);
    REL(xd);
    return fail("DXGI factory", 0);
  }
  REL(ad);
  REL(xd);

  ID3D11Device *d = g->dev;
  if (FAILED(ID3D11Device_CreateVertexShader(d, g_vs_quad, sizeof(g_vs_quad), NULL, &g->vs_quad)) ||
      FAILED(ID3D11Device_CreatePixelShader(d, g_ps_quad, sizeof(g_ps_quad), NULL, &g->ps_quad)) ||
      FAILED(ID3D11Device_CreateVertexShader(d, g_vs_tri, sizeof(g_vs_tri), NULL, &g->vs_tri)) ||
      FAILED(ID3D11Device_CreatePixelShader(d, g_ps_tri, sizeof(g_ps_tri), NULL, &g->ps_tri)) ||
      FAILED(ID3D11Device_CreateVertexShader(d, g_vs_mesh, sizeof(g_vs_mesh), NULL, &g->vs_mesh)) ||
      FAILED(ID3D11Device_CreatePixelShader(d, g_ps_mesh, sizeof(g_ps_mesh), NULL, &g->ps_mesh)))
  {
    return fail("shaders", 0);
  }
#  define INST(name, idx, off) \
    {name, idx, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, off, D3D11_INPUT_PER_INSTANCE_DATA, 1}
  const D3D11_INPUT_ELEMENT_DESC il_quad[] = {
      INST("RECT", 0, 0),     INST("BOUNDS", 0, 16), INST("COLOR", 0, 32), INST("COLOR", 1, 48),
      INST("COLOR", 2, 64),   INST("COLOR", 3, 80),  INST("PARAMS", 0, 96), INST("EXTRA", 0, 112),
  };
#  undef INST
  const D3D11_INPUT_ELEMENT_DESC il_tri[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"CLIP", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  const D3D11_INPUT_ELEMENT_DESC il_mesh[] = { /* BuiMeshVertex (= ImDrawVert) */
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  if (FAILED(ID3D11Device_CreateInputLayout(d, il_quad, 8, g_vs_quad, sizeof(g_vs_quad),
                                            &g->il_quad)) ||
      FAILED(ID3D11Device_CreateInputLayout(d, il_tri, 3, g_vs_tri, sizeof(g_vs_tri), &g->il_tri)) ||
      FAILED(ID3D11Device_CreateInputLayout(d, il_mesh, 3, g_vs_mesh, sizeof(g_vs_mesh),
                                            &g->il_mesh)))
  {
    return fail("input layouts", 0);
  }
  D3D11_BUFFER_DESC bd = {0};
  bd.ByteWidth = 16;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(ID3D11Device_CreateBuffer(d, &bd, NULL, &g->cb))) {
    return fail("constant buffer", 0);
  }
  D3D11_BLEND_DESC bl = {0};
  bl.RenderTarget[0].BlendEnable = TRUE;
  bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(ID3D11Device_CreateBlendState(d, &bl, &g->blend))) {
    return fail("blend state", 0);
  }
  D3D11_RASTERIZER_DESC rd = {0};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  if (FAILED(ID3D11Device_CreateRasterizerState(d, &rd, &g->rs))) {
    return fail("rasterizer state", 0);
  }
  rd.ScissorEnable = TRUE; /* meshes: the draw command's clip */
  if (FAILED(ID3D11Device_CreateRasterizerState(d, &rd, &g->rs_scissor))) {
    return fail("rasterizer state", 0);
  }
  D3D11_SAMPLER_DESC sd = {0};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
  if (FAILED(ID3D11Device_CreateSamplerState(d, &sd, &g->smp_linear))) {
    return fail("sampler", 0);
  }
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  if (FAILED(ID3D11Device_CreateSamplerState(d, &sd, &g->smp_point))) {
    return fail("sampler", 0);
  }
  {
    const uint32_t white = 0xFFFFFFFFu;
    D3D11_TEXTURE2D_DESC wd = {0};
    wd.Width = wd.Height = 1;
    wd.MipLevels = wd.ArraySize = 1;
    wd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    wd.SampleDesc.Count = 1;
    wd.Usage = D3D11_USAGE_DEFAULT;
    wd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {&white, 4, 4};
    if (FAILED(ID3D11Device_CreateTexture2D(d, &wd, &init, &g->white_tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(d, (ID3D11Resource *)g->white_tex, NULL,
                                                     &g->white_srv)))
    {
      return fail("white texture", 0);
    }
  }
  if (!atlas_create(g, &g->mask, 2048, 1)) {
    return fail("atlas", 0);
  }
  return 1;
}

static DWORD WINAPI device_thread(void *arg)
{
  BuiGpu *g = (BuiGpu *)arg;
  const int ok = device_init(g);
  InterlockedExchange(&g->state, ok ? 1 : -1);
  return 0;
}

BuiGpu *bui_gpu_create(void *hwnd, int alpha)
{
  BuiGpu *g = (BuiGpu *)calloc(1, sizeof(BuiGpu));
  if (!g) {
    return NULL;
  }
  g->hwnd = (HWND)hwnd;
  g->alpha = alpha;
  g->mask_map = (Slot *)calloc(MASK_MAP, sizeof(Slot));
  g->img_map = (Slot *)calloc(IMG_MAP, sizeof(Slot));
  g->thread = CreateThread(NULL, 0, device_thread, g, 0, NULL);
  if (!g->thread || !g->mask_map || !g->img_map) {
    if (g->thread) {
      WaitForSingleObject(g->thread, INFINITE);
      CloseHandle(g->thread);
      g->thread = NULL;
    }
    bui_gpu_free(g);
    return NULL;
  }
  return g;
}

int bui_gpu_state(BuiGpu *g, int wait)
{
  if (wait && g->thread) {
    WaitForSingleObject(g->thread, INFINITE);
  }
  const LONG s = InterlockedCompareExchange(&g->state, 0, 0);
  if (s != 0 && g->thread) {
    WaitForSingleObject(g->thread, INFINITE);
    CloseHandle(g->thread);
    g->thread = NULL;
  }
  return (int)s;
}

void bui_gpu_free(BuiGpu *g)
{
  if (!g) {
    return;
  }
  if (g->thread) {
    WaitForSingleObject(g->thread, INFINITE);
    CloseHandle(g->thread);
  }
  if (g->ctx) {
    ID3D11DeviceContext_ClearState(g->ctx);
  }
  REL(g->mask.srv);
  REL(g->mask.tex);
  REL(g->rgba.srv);
  REL(g->rgba.tex);
  for (int i = 0; i < BIG_MAX; i++) {
    REL(g->big[i].srv);
    REL(g->big[i].tex);
    REL(g->big[i].uv_srv);
    REL(g->big[i].uv_tex);
  }
  REL(g->curve_srv);
  REL(g->curve_tex);
  REL(g->band_srv);
  REL(g->band_tex);
  for (int i = 0; i < g->n_dtex; i++) {
    REL(g->dtex[i].srv);
    REL(g->dtex[i].tex);
  }
  free(g->dtex);
  REL(g->white_srv);
  REL(g->white_tex);
  REL(g->smp_linear);
  REL(g->smp_point);
  REL(g->rs_scissor);
  REL(g->mvb);
  REL(g->mib);
  REL(g->il_mesh);
  REL(g->vs_mesh);
  REL(g->ps_mesh);
  REL(g->rtv);
  REL(g->target);
  REL(g->rs);
  REL(g->blend);
  REL(g->cb);
  REL(g->ib);
  REL(g->tb);
  REL(g->il_quad);
  REL(g->il_tri);
  REL(g->vs_quad);
  REL(g->vs_tri);
  REL(g->ps_quad);
  REL(g->ps_tri);
  REL(g->swap);
  REL(g->factory);
  REL(g->ctx1);
  REL(g->ctx);
  REL(g->dev);
  free(g->inst);
  free(g->tv);
  free(g->mv);
  free(g->mi);
  free(g->b);
  free(g->mask_map);
  free(g->img_map);
  free(g->slug_map);
  free(g);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Recording
 * \{ */

static void flush(BuiGpu *g);

static void big_sweep(BuiGpu *g);

void bui_gpu_begin(BuiGpu *g, int w, int h)
{
  g->w = w > 0 ? w : 1;
  g->h = h > 0 ? h : 1;
  g->frame++;
  big_sweep(g);
}

static Batch *batch_new(BuiGpu *g, int tri, int first);

/* The batch to append to: the last one when it is of the same kind. */
static Batch *batch_for(BuiGpu *g, int tri, int first)
{
  if (tri != 2 && g->n_b > 0 && g->b[g->n_b - 1].tri == tri) {
    return &g->b[g->n_b - 1];
  }
  return batch_new(g, tri, first);
}

static Batch *batch_new(BuiGpu *g, int tri, int first)
{
  if (g->n_b == g->cap_b) {
    const int cap = g->cap_b ? g->cap_b * 2 : 64;
    Batch *nb = (Batch *)realloc(g->b, sizeof(Batch) * (size_t)cap);
    if (!nb) {
      return NULL;
    }
    g->b = nb;
    g->cap_b = cap;
  }
  Batch *b = &g->b[g->n_b++];
  memset(b, 0, sizeof(*b));
  b->tri = tri;
  b->first = first;
  b->img = -1;
  return b;
}

/* -- textured meshes ----------------------------------------------------------- */

static void dtex_release(DevTex *t)
{
  REL(t->srv);
  REL(t->tex);
  t->id = 0;
}

/* Creates or refreshes the device copy of registry texture `id`. */
static ID3D11ShaderResourceView *dtex_get(BuiGpu *g, int id)
{
  BuiTexView v;
  if (id <= 0 || !bui_texture_view(id, &v)) {
    return g->white_srv;
  }
  const int slot = (id & 0xFFFF) - 1;
  if (slot >= g->n_dtex) {
    const int n = slot + 16;
    DevTex *nd = (DevTex *)realloc(g->dtex, sizeof(DevTex) * (size_t)n);
    if (!nd) {
      return g->white_srv;
    }
    memset(nd + g->n_dtex, 0, sizeof(DevTex) * (size_t)(n - g->n_dtex));
    g->dtex = nd;
    g->n_dtex = n;
  }
  DevTex *t = &g->dtex[slot];
  if (t->id == id && t->gen == v.gen) {
    return t->srv;
  }
  if (t->id == id && t->w == v.w && t->h == v.h) {
    /* updated: draws recorded so far still need the old pixels */
    flush(g);
    int rects[32][4];
    int n = bui_texture_changes(id, t->gen, rects, 32);
    if (n < 0) {
      rects[0][0] = rects[0][1] = 0;
      rects[0][2] = v.w;
      rects[0][3] = v.h;
      n = 1;
    }
    for (int i = 0; i < n; i++) {
      const int *r = rects[i];
      D3D11_BOX box = {(UINT)r[0], (UINT)r[1], 0, (UINT)(r[0] + r[2]), (UINT)(r[1] + r[3]), 1};
      ID3D11DeviceContext_UpdateSubresource(g->ctx, (ID3D11Resource *)t->tex, 0, &box,
                                            v.rgba + ((size_t)r[1] * v.w + r[0]) * 4,
                                            (UINT)v.w * 4, 0);
    }
    t->gen = v.gen;
    return t->srv;
  }
  if (t->id) {
    flush(g);
    dtex_release(t);
  }
  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = (UINT)v.w;
  td.Height = (UINT)v.h;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init = {v.rgba, (UINT)v.w * 4, 0};
  if (FAILED(ID3D11Device_CreateTexture2D(g->dev, &td, &init, &t->tex)) ||
      FAILED(ID3D11Device_CreateShaderResourceView(g->dev, (ID3D11Resource *)t->tex, NULL,
                                                   &t->srv)))
  {
    dtex_release(t);
    return g->white_srv;
  }
  t->id = id;
  t->gen = v.gen;
  t->w = v.w;
  t->h = v.h;
  return t->srv;
}

/* The device texture a recorded batch samples (already made by dtex_get). */
static ID3D11ShaderResourceView *dtex_srv(BuiGpu *g, int id)
{
  const int slot = (id & 0xFFFF) - 1;
  if (id > 0 && slot < g->n_dtex && g->dtex[slot].id == id && g->dtex[slot].srv) {
    return g->dtex[slot].srv;
  }
  return g->white_srv;
}

/* Frees device copies of textures freed in the registry. */
static void dtex_sweep(BuiGpu *g)
{
  const unsigned fg = bui_texture_free_gen();
  if (fg == g->dtex_free_gen) {
    return;
  }
  g->dtex_free_gen = fg;
  BuiTexView v;
  for (int i = 0; i < g->n_dtex; i++) {
    if (g->dtex[i].id && !bui_texture_view(g->dtex[i].id, &v)) {
      flush(g);
      dtex_release(&g->dtex[i]);
    }
  }
}

static int grow(void **p, int *cap, int need, size_t size)
{
  if (need <= *cap) {
    return 1;
  }
  int c = *cap ? *cap : 4096;
  while (c < need) {
    c *= 2;
  }
  void *np = realloc(*p, size * (size_t)c);
  if (!np) {
    return 0;
  }
  *p = np;
  *cap = c;
  return 1;
}

void bui_gpu_mesh(BuiGpu *g,
                  const BuiMeshVertex *vtx,
                  int n_vtx,
                  const void *idx,
                  int idx32,
                  int n_idx,
                  float ox,
                  float oy,
                  int tex,
                  int nearest,
                  const int clip[4])
{
  n_idx -= n_idx % 3;
  if (n_idx <= 0 || n_vtx <= 0) {
    return;
  }
  dtex_sweep(g);
  dtex_get(g, tex); /* may flush: before anything of this mesh is recorded */
  if (!grow((void **)&g->mv, &g->cap_mv, g->n_mv + n_vtx, sizeof(BuiMeshVertex)) ||
      !grow((void **)&g->mi, &g->cap_mi, g->n_mi + n_idx, sizeof(uint32_t)))
  {
    return;
  }
  Batch *b = NULL;
  if (g->n_b > 0) { /* same texture, sampler and clip: one draw */
    Batch *last = &g->b[g->n_b - 1];
    if (last->tri == 2 && last->tex == tex && last->nearest == nearest &&
        memcmp(last->clip, clip, sizeof(int[4])) == 0)
    {
      b = last;
    }
  }
  if (!b) {
    b = batch_for(g, 2, g->n_mi);
    if (!b) {
      return;
    }
    b->tex = tex;
    b->nearest = nearest;
    memcpy(b->clip, clip, sizeof(int[4]));
  }
  const uint32_t base = (uint32_t)g->n_mv;
  for (int i = 0; i < n_vtx; i++) {
    BuiMeshVertex *v = &g->mv[g->n_mv++];
    *v = vtx[i];
    v->x += ox;
    v->y += oy;
  }
  const uint16_t *i16 = (const uint16_t *)idx;
  const uint32_t *i32 = (const uint32_t *)idx;
  uint32_t *out = g->mi + g->n_mi;
  int n = 0;
  for (int i = 0; i < n_idx; i += 3) {
    const uint32_t a = idx32 ? i32[i] : i16[i], c1 = idx32 ? i32[i + 1] : i16[i + 1],
                   c2 = idx32 ? i32[i + 2] : i16[i + 2];
    if (a >= (uint32_t)n_vtx || c1 >= (uint32_t)n_vtx || c2 >= (uint32_t)n_vtx) {
      continue;
    }
    out[n++] = base + a;
    out[n++] = base + c1;
    out[n++] = base + c2;
  }
  g->n_mi += n;
  b->count += n;
}

/* Appends an instance to quad batch `b`. */
static BuiGpuInst *inst_in(BuiGpu *g, Batch *b, int kind, int x0, int y0, int x1, int y1);

BuiGpuInst *bui_gpu_inst_image(BuiGpu *g, int tex, int x0, int y0, int x1, int y1)
{
  Batch *b = g->n_b > 0 ? &g->b[g->n_b - 1] : NULL;
  if (!b || b->tri != 0 || (b->img >= 0 && b->img != tex)) {
    b = batch_new(g, 0, g->n_inst); /* another texture: another draw */
  }
  if (!b) {
    return NULL;
  }
  b->img = tex;
  return inst_in(g, b, BG_IMAGE, x0, y0, x1, y1);
}

BuiGpuInst *bui_gpu_inst(BuiGpu *g, int kind, int x0, int y0, int x1, int y1)
{
  Batch *b = batch_for(g, 0, g->n_inst);
  return b ? inst_in(g, b, kind, x0, y0, x1, y1) : NULL;
}

static BuiGpuInst *inst_in(BuiGpu *g, Batch *b, int kind, int x0, int y0, int x1, int y1)
{
  if (g->n_inst == g->cap_inst) {
    const int cap = g->cap_inst ? g->cap_inst * 2 : 1024;
    BuiGpuInst *ni = (BuiGpuInst *)realloc(g->inst, sizeof(BuiGpuInst) * (size_t)cap);
    if (!ni) {
      return NULL;
    }
    g->inst = ni;
    g->cap_inst = cap;
  }
  b->count++;
  BuiGpuInst *in = &g->inst[g->n_inst++];
  memset(in, 0, sizeof(*in));
  in->bounds[0] = (float)x0;
  in->bounds[1] = (float)y0;
  in->bounds[2] = (float)x1;
  in->bounds[3] = (float)y1;
  in->p[0] = (float)kind;
  return in;
}

void bui_gpu_triangles(BuiGpu *g,
                       const float (*co)[2],
                       const float (*col)[4],
                       int count,
                       const int clip[4])
{
  count -= count % 3;
  if (count <= 0) {
    return;
  }
  if (g->n_tv + count > g->cap_tv) {
    int cap = g->cap_tv ? g->cap_tv : 1024;
    while (cap < g->n_tv + count) {
      cap *= 2;
    }
    TriV *nt = (TriV *)realloc(g->tv, sizeof(TriV) * (size_t)cap);
    if (!nt) {
      return;
    }
    g->tv = nt;
    g->cap_tv = cap;
  }
  Batch *b = batch_for(g, 1, g->n_tv);
  if (!b) {
    return;
  }
  for (int i = 0; i < count; i++) {
    TriV *v = &g->tv[g->n_tv++];
    v->x = co[i][0];
    v->y = co[i][1];
    memcpy(v->c, col[i], sizeof(v->c));
    for (int k = 0; k < 4; k++) {
      v->clip[k] = (float)clip[k];
    }
  }
  b->count += count;
}

static uint64_t mix64(uint64_t h)
{
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  return h;
}

static void mask_reset(BuiGpu *g)
{
  flush(g); /* recorded instances still point into the old layout */
  atlas_clear(&g->mask);
  memset(g->mask_map, 0, sizeof(Slot) * MASK_MAP);
  g->mask_n = 0;
}

int bui_gpu_mask(BuiGpu *g, const void *key, unsigned gen, const uint8_t *mask, int w, int h,
                 int *r_x, int *r_y)
{
  if (w + 1 > g->mask.size || h + 1 > g->mask.size) {
    return 0;
  }
  if (gen != g->mask_gen) { /* text runs were freed: their keys may be reused */
    g->mask_gen = gen;
    mask_reset(g);
  }
  const uint64_t k1 = key ? (uint64_t)(uintptr_t)key : 0, k2 = ((uint64_t)w << 32) | (uint32_t)h;
  uint32_t idx = 0;
  if (key) {
    idx = (uint32_t)mix64(k1) & (MASK_MAP - 1);
    for (;;) {
      Slot *s = &g->mask_map[idx];
      if (s->k1 == 0) {
        break;
      }
      if (s->k1 == k1 && s->k2 == k2) {
        *r_x = s->x;
        *r_y = s->y;
        return 1;
      }
      idx = (idx + 1) & (MASK_MAP - 1);
    }
    if (g->mask_n >= MASK_MAP_FILL) {
      mask_reset(g);
      return bui_gpu_mask(g, key, gen, mask, w, h, r_x, r_y);
    }
  }
  int x, y;
  if (!atlas_place(&g->mask, w, h, &x, &y)) {
    mask_reset(g); /* empty now, and the map slot must be found again */
    return bui_gpu_mask(g, key, gen, mask, w, h, r_x, r_y);
  }
  atlas_upload(g, &g->mask, x, y, w, h, mask, w);
  if (key) {
    Slot *s = &g->mask_map[idx];
    s->k1 = k1;
    s->k2 = k2;
    s->x = x;
    s->y = y;
    g->mask_n++;
  }
  *r_x = x;
  *r_y = y;
  return 1;
}

/* -------------------------------------------------------------------- */
/** \name Slug glyphs
 *
 * Eric Lengyel's Slug algorithm (https://github.com/EricLengyel/Slug):
 * glyph outlines live on the GPU as quadratic curves, split into horizontal
 * and vertical bands; the pixel shader casts a ray each way through the
 * pixel's bands and integrates exact coverage. Resolution independent, one
 * quad per glyph, no rasterizing or atlas packing on the CPU.
 * \{ */

static int slug_tex(BuiGpu *g, DXGI_FORMAT fmt, ID3D11Texture2D **r_tex,
                    ID3D11ShaderResourceView **r_srv)
{
  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = SLUG_W;
  td.Height = SLUG_H;
  td.MipLevels = td.ArraySize = 1;
  td.Format = fmt;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  return SUCCEEDED(ID3D11Device_CreateTexture2D(g->dev, &td, NULL, r_tex)) &&
         SUCCEEDED(ID3D11Device_CreateShaderResourceView(g->dev, (ID3D11Resource *)*r_tex, NULL,
                                                         r_srv));
}

int bui_gpu_slug_on(BuiGpu *g)
{
  if (g->slug == 0) {
    g->slug = -1;
    if (slug_tex(g, DXGI_FORMAT_R32G32B32A32_FLOAT, &g->curve_tex, &g->curve_srv) &&
        slug_tex(g, DXGI_FORMAT_R32G32_UINT, &g->band_tex, &g->band_srv) &&
        (g->slug_map = (SlugGlyph *)calloc(SLUG_MAP, sizeof(SlugGlyph))) != NULL)
    {
      g->slug = 1;
    }
  }
  return g->slug > 0;
}

static void slug_reset(BuiGpu *g)
{
  flush(g); /* recorded instances still point at the old data */
  memset(g->slug_map, 0, sizeof(SlugGlyph) * SLUG_MAP);
  g->slug_n = g->curve_next = g->band_next = 0;
}

/* Uploads `count` texels at linear index `at`, row by row. */
static void slug_upload(BuiGpu *g, ID3D11Texture2D *tex, int at, const void *data, int count,
                        int texel)
{
  const char *p = (const char *)data;
  while (count > 0) {
    const int x = at % SLUG_W, y = at / SLUG_W;
    const int n = count < SLUG_W - x ? count : SLUG_W - x;
    D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)(x + n), (UINT)(y + 1), 1};
    ID3D11DeviceContext_UpdateSubresource(g->ctx, (ID3D11Resource *)tex, 0, &box, p,
                                          (UINT)(n * texel), 0);
    p += (size_t)n * texel;
    at += n;
    count -= n;
  }
}

typedef struct SlugSort {
  float key;
  uint32_t curve;
} SlugSort;

static int slug_cmp(const void *a, const void *b)
{
  const float ka = ((const SlugSort *)a)->key, kb = ((const SlugSort *)b)->key;
  return ka < kb ? 1 : ka > kb ? -1 : 0; /* descending */
}

static float min3(float a, float b, float c)
{
  return a < b ? (a < c ? a : c) : (b < c ? b : c);
}
static float max3(float a, float b, float c)
{
  return a > b ? (a > c ? a : c) : (b > c ? b : c);
}

/* Builds and uploads a glyph's curve and band data. */
static int slug_build(BuiGpu *g, SlugGlyph *sg, const float *cv, int n, const float bb[4])
{
  const int nb = n < 8 ? 1 : (n / 4 < SLUG_MAX_BANDS ? n / 4 : SLUG_MAX_BANDS);
  const float w = bb[2] - bb[0] > 1.0f ? bb[2] - bb[0] : 1.0f;
  const float h = bb[3] - bb[1] > 1.0f ? bb[3] - bb[1] : 1.0f;
  /* horizontal bands split y, vertical bands split x */
  const float hs = (float)nb / h, vs = (float)nb / w;
  const float eps = 1.0f / 1024.0f * (w > h ? w : h);

  /* worst case every curve lands in every band */
  const int max_entries = 2 * nb * n;
  const int need_band = 2 * nb + max_entries, need_curve = 2 * n;
  if (need_curve > SLUG_W * SLUG_H / 4 || need_band > SLUG_W * SLUG_H / 4) {
    return 0; /* absurd glyph */
  }
  if (g->curve_next + need_curve > SLUG_W * SLUG_H || g->band_next + need_band > SLUG_W * SLUG_H ||
      g->slug_n >= SLUG_MAP_FILL)
  {
    return -1; /* full: caller resets and retries */
  }
  float *ct = (float *)malloc(sizeof(float) * 8 * (size_t)n);
  uint32_t *bt = (uint32_t *)malloc(sizeof(uint32_t) * 2 * (size_t)need_band);
  SlugSort *tmp = (SlugSort *)malloc(sizeof(SlugSort) * (size_t)n);
  if (!ct || !bt || !tmp) {
    free(ct);
    free(bt);
    free(tmp);
    return 0;
  }
  const int cbase = g->curve_next; /* even: a curve never straddles a row */
  for (int i = 0; i < n; i++) {
    const float *q = cv + 6 * i;
    float *t = ct + 8 * i;
    t[0] = q[0];
    t[1] = q[1];
    t[2] = q[2];
    t[3] = q[3];
    t[4] = q[4];
    t[5] = q[5];
    t[6] = t[7] = 0.0f;
  }
  const int bbase = g->band_next;
  int at = 2 * nb; /* entries follow the 2 * nb headers */
  for (int dir = 0; dir < 2; dir++) {
    for (int b = 0; b < nb; b++) {
      /* band range in font units */
      const float s = dir == 0 ? hs : vs, o = dir == 0 ? bb[1] : bb[0];
      const float lo = o + (float)b / s - eps, hi = o + (float)(b + 1) / s + eps;
      int m = 0;
      for (int i = 0; i < n; i++) {
        const float *q = cv + 6 * i;
        /* dir 0 (horizontal rays) uses y extents, sorted by max x; dir 1 the reverse */
        const float a0 = dir == 0 ? q[1] : q[0], a1 = dir == 0 ? q[3] : q[2],
                    a2 = dir == 0 ? q[5] : q[4];
        const float amin = min3(a0, a1, a2), amax = max3(a0, a1, a2);
        if (amax < lo || amin > hi || (a0 == a1 && a1 == a2)) {
          continue; /* outside, or parallel to the ray: never crosses it */
        }
        tmp[m].key = dir == 0 ? max3(q[0], q[2], q[4]) : max3(q[1], q[3], q[5]);
        tmp[m].curve = (uint32_t)(cbase + 2 * i);
        m++;
      }
      qsort(tmp, (size_t)m, sizeof(SlugSort), slug_cmp);
      const int hdr = dir * nb + b;
      bt[2 * hdr] = (uint32_t)m;
      bt[2 * hdr + 1] = (uint32_t)(bbase + at);
      for (int k = 0; k < m; k++) {
        bt[2 * (at + k)] = tmp[k].curve;
        bt[2 * (at + k) + 1] = 0;
      }
      at += m;
    }
  }
  slug_upload(g, g->curve_tex, cbase, ct, 2 * n, 16);
  slug_upload(g, g->band_tex, bbase, bt, at, 8);
  g->curve_next += 2 * n;
  g->band_next += at;
  free(ct);
  free(bt);
  free(tmp);
  memcpy(sg->bbox, bb, sizeof(float[4]));
  sg->band[0] = hs;
  sg->band[1] = -bb[1] * hs;
  sg->band[2] = vs;
  sg->band[3] = -bb[0] * vs;
  sg->loc[0] = bbase;
  sg->loc[1] = nb;
  sg->loc[2] = nb;
  sg->loc[3] = 0;
  sg->ok = 1;
  return 1;
}

int bui_gpu_slug_glyph(BuiGpu *g, int face, int glyph, unsigned gen, float r_bbox[4],
                       float r_band[4], int r_loc[4])
{
  if (g->slug <= 0 || face < 0 || face > 0x7ffe || glyph < 0 || glyph > 0xffff) {
    return 0;
  }
  if (gen != g->slug_gen) { /* faces were freed: ids may be reused */
    g->slug_gen = gen;
    slug_reset(g);
  }
  const uint32_t key = ((uint32_t)(face + 1) << 16) | (uint32_t)glyph;
  uint32_t idx = (key * 2654435761u) >> 19 & (SLUG_MAP - 1);
  SlugGlyph *sg;
  for (;;) {
    sg = &g->slug_map[idx];
    if (sg->key == 0 || sg->key == key) {
      break;
    }
    idx = (idx + 1) & (SLUG_MAP - 1);
  }
  if (sg->key == 0) {
    float *cv = NULL;
    int n = 0;
    float bb[4];
    sg->ok = 0;
    if (bui_text_glyph_outline(face, glyph, &cv, &n, bb)) {
      int got = slug_build(g, sg, cv, n, bb);
      if (got < 0) {
        slug_reset(g);
        free(cv);
        return bui_gpu_slug_glyph(g, face, glyph, gen, r_bbox, r_band, r_loc);
      }
    }
    free(cv);
    sg->key = key;
    g->slug_n++;
  }
  if (!sg->ok) {
    return 0;
  }
  memcpy(r_bbox, sg->bbox, sizeof(float[4]));
  memcpy(r_band, sg->band, sizeof(float[4]));
  memcpy(r_loc, sg->loc, sizeof(int[4]));
  return 1;
}

/** \} */

static void img_reset(BuiGpu *g)
{
  flush(g);
  atlas_clear(&g->rgba);
  memset(g->img_map, 0, sizeof(Slot) * IMG_MAP);
  g->img_n = 0;
}

int bui_gpu_image(BuiGpu *g, uint64_t key, const uint32_t *argb, int w, int h, int *r_x, int *r_y)
{
  if (!g->rgba.tex && !atlas_create(g, &g->rgba, 2048, 4)) {
    return 0;
  }
  if (w + 1 > g->rgba.size || h + 1 > g->rgba.size) {
    return 0; /* TODO: images larger than the atlas */
  }
  /* key: BuiPixmap.key or its content hash (gpu_raster.c), with the size */
  const uint64_t k1 = key, k2 = ((uint64_t)w << 32) | (uint32_t)h;
  uint32_t idx = (uint32_t)mix64(k1) & (IMG_MAP - 1);
  for (;;) {
    Slot *s = &g->img_map[idx];
    if (s->k1 == 0) {
      break;
    }
    if (s->k1 == k1 && s->k2 == k2) {
      *r_x = s->x;
      *r_y = s->y;
      return 1;
    }
    idx = (idx + 1) & (IMG_MAP - 1);
  }
  int x, y;
  if (g->img_n >= IMG_MAP_FILL || !atlas_place(&g->rgba, w, h, &x, &y)) {
    img_reset(g);
    return bui_gpu_image(g, key, argb, w, h, r_x, r_y);
  }
  atlas_upload(g, &g->rgba, x, y, w, h, argb, w * 4);
  Slot *s = &g->img_map[idx];
  s->k1 = k1;
  s->k2 = k2;
  s->x = x;
  s->y = y;
  g->img_n++;
  *r_x = x;
  *r_y = y;
  return 1;
}

/* -- big images: textures of their own ------------------------------------------------ */

static size_t big_size(int format, int w, int h)
{
  return format == BUI_PIXMAP_ARGB ? (size_t)w * h * 4 : (size_t)w * h * 3 / 2;
}

static void big_release(BuiGpu *g, BigTex *t)
{
  if (t->srv) {
    g->big_bytes -= big_size(t->format, t->w, t->h);
  }
  REL(t->srv);
  REL(t->tex);
  REL(t->uv_srv);
  REL(t->uv_tex);
  memset(t, 0, sizeof(*t));
}

/* One plane: a texture and its view, initialized from `data`. */
static int big_plane(BuiGpu *g, DXGI_FORMAT fmt, int w, int h, const void *data, UINT pitch,
                     int dynamic, ID3D11Texture2D **r_tex, ID3D11ShaderResourceView **r_srv)
{
  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = (UINT)w;
  td.Height = (UINT)h;
  td.MipLevels = td.ArraySize = 1;
  td.Format = fmt;
  td.SampleDesc.Count = 1;
  td.Usage = dynamic ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init = {data, pitch, 0};
  if (FAILED(ID3D11Device_CreateTexture2D(g->dev, &td, &init, r_tex)) ||
      FAILED(ID3D11Device_CreateShaderResourceView(g->dev, (ID3D11Resource *)*r_tex, NULL, r_srv)))
  {
    REL(*r_tex);
    return 0;
  }
  return 1;
}

/* `px` into slot `t`'s textures: created, or (a stream's next frame) updated in place. */
static int big_upload(BuiGpu *g, BigTex *t, int create, const uint32_t *px, int stride)
{
  const int nv12 = t->format != BUI_PIXMAP_ARGB;
  const uint8_t *uv = (const uint8_t *)px + (size_t)t->w * t->h;
  if (!create) {
    ID3D11DeviceContext_UpdateSubresource(g->ctx, (ID3D11Resource *)t->tex, 0, NULL, px,
                                          (UINT)stride * (nv12 ? 1 : 4), 0);
    if (nv12) {
      ID3D11DeviceContext_UpdateSubresource(g->ctx, (ID3D11Resource *)t->uv_tex, 0, NULL, uv,
                                            (UINT)t->w, 0);
    }
    return 1;
  }
  if (!nv12) { /* 0xAARRGGBB in memory order */
    return big_plane(g, DXGI_FORMAT_B8G8R8A8_UNORM, t->w, t->h, px, (UINT)stride * 4, t->stream,
                     &t->tex, &t->srv);
  }
  if (!big_plane(g, DXGI_FORMAT_R8_UNORM, t->w, t->h, px, (UINT)t->w, t->stream, &t->tex, &t->srv)) {
    return 0;
  }
  if (!big_plane(g, DXGI_FORMAT_R8G8_UNORM, t->w / 2, t->h / 2, uv, (UINT)t->w, t->stream,
                 &t->uv_tex, &t->uv_srv))
  {
    REL(t->srv);
    REL(t->tex);
    return 0;
  }
  return 1;
}

/* Frees what the kit freed (BUI_image_forget) and what went unused. */
static void big_sweep(BuiGpu *g)
{
  uint64_t gone[64];
  const int n = bui_image_forgotten(&g->forget_seen, gone, 64); /* -1: missed some */
  for (int i = 0; i < BIG_MAX; i++) {
    BigTex *t = &g->big[i];
    if (!t->key) {
      continue;
    }
    int drop = g->frame - t->used > BIG_IDLE || (n < 0 && t->used != g->frame);
    for (int k = 0; k < n && !drop; k++) {
      drop = gone[k] == t->base;
    }
    if (drop) {
      big_release(g, t);
    }
  }
}

int bui_gpu_max_texture(BuiGpu *g)
{
  return g->max_tex > 0 ? g->max_tex : 8192;
}

int bui_gpu_image_tex(BuiGpu *g, uint64_t key, uint64_t base, int stream, int format,
                      const uint32_t *px, int stride, int w, int h)
{
  if (w <= 0 || h <= 0 || w > bui_gpu_max_texture(g) || h > bui_gpu_max_texture(g)) {
    return 0;
  }
  int free_slot = -1, lru = -1;
  for (int i = 0; i < BIG_MAX; i++) {
    BigTex *t = &g->big[i];
    if (t->key && t->w == w && t->h == h && t->format == format &&
        (t->key == key || (stream && t->stream && t->base == base)))
    {
      if (t->key != key) { /* the stream's next frame: in place, no new texture */
        big_upload(g, t, 0, px, stride);
        t->key = key;
      }
      t->used = g->frame;
      return i + 1;
    }
    if (!t->key) {
      free_slot = free_slot < 0 ? i : free_slot;
    }
    else if (t->used != g->frame && (lru < 0 || t->used < g->big[lru].used)) {
      lru = i; /* not drawn this frame: no recorded batch samples it */
    }
  }
  const size_t bytes = big_size(format, w, h);
  while (lru >= 0 && (free_slot < 0 || g->big_bytes + bytes > BIG_BUDGET)) {
    big_release(g, &g->big[lru]);
    free_slot = free_slot < 0 ? lru : free_slot;
    lru = -1;
    for (int i = 0; i < BIG_MAX; i++) {
      const BigTex *t = &g->big[i];
      if (t->key && t->used != g->frame && (lru < 0 || t->used < g->big[lru].used)) {
        lru = i;
      }
    }
  }
  if (free_slot < 0) {
    return 0;
  }
  BigTex *t = &g->big[free_slot];
  t->w = w;
  t->h = h;
  t->format = format;
  t->stream = stream;
  if (!big_upload(g, t, 1, px, stride)) {
    memset(t, 0, sizeof(*t));
    return 0;
  }
  t->key = key;
  t->base = base;
  t->used = g->frame;
  g->big_bytes += bytes;
  return free_slot + 1;
}

void bui_gpu_damage(BuiGpu *g, int x, int y, int w, int h)
{
  if (w <= 0 || h <= 0 || g->n_dmg == (int)(sizeof(g->dmg) / sizeof(g->dmg[0]))) {
    g->dmg_full = 1;
    return;
  }
  RECT *r = &g->dmg[g->n_dmg++];
  r->left = x;
  r->top = y;
  r->right = x + w;
  r->bottom = y + h;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing + present
 * \{ */

/* Resizing steps: the target and the swap chain are made a multiple of GPU_SLACK px wide and
 * high and kept while the frame stays within [cap - 2 * GPU_SLACK, cap], so a drag-resize
 * reallocates every few dozen sizes, not at each one. A bitblt (alpha) swap chain stretches its
 * buffer over the window, so it stays exact. */
#  define GPU_SLACK 256

static int slack_cap(const BuiGpu *g, int need)
{
  if (g->alpha) {
    return need;
  }
  const int cap = (need + GPU_SLACK - 1) / GPU_SLACK * GPU_SLACK;
  return cap < g->max_tex ? cap : need;
}

static int slack_fits(const BuiGpu *g, int cap, int need)
{
  return cap >= need && (g->alpha ? cap == need : cap - need < 2 * GPU_SLACK);
}

static int ensure_target(BuiGpu *g)
{
  if (g->target && slack_fits(g, g->tw, g->w) && slack_fits(g, g->th, g->h)) {
    if (g->fw != g->w || g->fh != g->h) { /* another size in the same texture: a redraw follows */
      g->fw = g->w;
      g->fh = g->h;
      const float clear[4] = {0, 0, 0, g->alpha ? 0.0f : 1.0f};
      ID3D11DeviceContext_ClearRenderTargetView(g->ctx, g->rtv, clear);
      g->dmg_full = 1;
    }
    return 1;
  }
  REL(g->rtv);
  REL(g->target);
  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = (UINT)slack_cap(g, g->w);
  td.Height = (UINT)slack_cap(g, g->h);
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (FAILED(ID3D11Device_CreateTexture2D(g->dev, &td, NULL, &g->target)) ||
      FAILED(ID3D11Device_CreateRenderTargetView(g->dev, (ID3D11Resource *)g->target, NULL,
                                                 &g->rtv)))
  {
    REL(g->target);
    return 0;
  }
  g->tw = (int)td.Width;
  g->th = (int)td.Height;
  g->fw = g->w;
  g->fh = g->h;
  const float clear[4] = {0, 0, 0, g->alpha ? 0.0f : 1.0f};
  ID3D11DeviceContext_ClearRenderTargetView(g->ctx, g->rtv, clear);
  g->dmg_full = 1;
  return 1;
}

static int ensure_buffer(BuiGpu *g, ID3D11Buffer **buf, UINT *cap, UINT need, UINT stride,
                         UINT bind)
{
  if (*buf && *cap >= need) {
    return 1;
  }
  REL(*buf);
  UINT c = *cap ? *cap : 4096;
  while (c < need) {
    c *= 2;
  }
  D3D11_BUFFER_DESC bd = {0};
  bd.ByteWidth = c * stride;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = bind;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(ID3D11Device_CreateBuffer(g->dev, &bd, NULL, buf))) {
    *cap = 0;
    return 0;
  }
  *cap = c;
  return 1;
}

static void upload(BuiGpu *g, ID3D11Buffer *buf, const void *data, size_t bytes)
{
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource *)buf, 0, D3D11_MAP_WRITE_DISCARD,
                                        0, &m)))
  {
    memcpy(m.pData, data, bytes);
    ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource *)buf, 0);
  }
}

static void flush(BuiGpu *g)
{
  if (g->n_b == 0) {
    return;
  }
  if (!ensure_target(g)) {
    g->n_b = g->n_inst = g->n_tv = g->n_mv = g->n_mi = 0;
    return;
  }
  ID3D11DeviceContext *c = g->ctx;
  if (g->n_inst && ensure_buffer(g, &g->ib, &g->ib_cap, (UINT)g->n_inst, sizeof(BuiGpuInst),
                                 D3D11_BIND_VERTEX_BUFFER))
  {
    upload(g, g->ib, g->inst, sizeof(BuiGpuInst) * (size_t)g->n_inst);
  }
  if (g->n_tv &&
      ensure_buffer(g, &g->tb, &g->tb_cap, (UINT)g->n_tv, sizeof(TriV), D3D11_BIND_VERTEX_BUFFER))
  {
    upload(g, g->tb, g->tv, sizeof(TriV) * (size_t)g->n_tv);
  }
  if (g->n_mv && ensure_buffer(g, &g->mvb, &g->mvb_cap, (UINT)g->n_mv, sizeof(BuiMeshVertex),
                               D3D11_BIND_VERTEX_BUFFER))
  {
    upload(g, g->mvb, g->mv, sizeof(BuiMeshVertex) * (size_t)g->n_mv);
  }
  if (g->n_mi && ensure_buffer(g, &g->mib, &g->mib_cap, (UINT)g->n_mi, sizeof(uint32_t),
                               D3D11_BIND_INDEX_BUFFER))
  {
    upload(g, g->mib, g->mi, sizeof(uint32_t) * (size_t)g->n_mi);
  }
  const float cbd[4] = {2.0f / (float)g->tw, 2.0f / (float)g->th, 0, 0};
  upload(g, g->cb, cbd, sizeof(cbd));

  ID3D11DeviceContext_OMSetRenderTargets(c, 1, &g->rtv, NULL);
  const float bf[4] = {0, 0, 0, 0};
  ID3D11DeviceContext_OMSetBlendState(c, g->blend, bf, 0xffffffffu);
  D3D11_VIEWPORT vp = {0, 0, (float)g->tw, (float)g->th, 0, 1};
  ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
  ID3D11DeviceContext_RSSetState(c, g->rs);
  ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 1, &g->cb);
  ID3D11ShaderResourceView *srv[4] = {g->mask.srv, g->rgba.srv, g->curve_srv, g->band_srv};
  ID3D11DeviceContext_PSSetShaderResources(c, 0, 4, srv);

  int cur = -1, img = 0;
  for (int i = 0; i < g->n_b; i++) {
    const Batch *b = &g->b[i];
    if (b->count == 0) {
      continue;
    }
    if (b->tri != cur) {
      if (cur == 2) {
        ID3D11DeviceContext_RSSetState(c, g->rs);
      }
      cur = b->tri;
      UINT off = 0;
      if (b->tri == 2) {
        UINT stride = sizeof(BuiMeshVertex);
        ID3D11DeviceContext_IASetInputLayout(c, g->il_mesh);
        ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &g->mvb, &stride, &off);
        ID3D11DeviceContext_IASetIndexBuffer(c, g->mib, DXGI_FORMAT_R32_UINT, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(c, g->vs_mesh, NULL, 0);
        ID3D11DeviceContext_PSSetShader(c, g->ps_mesh, NULL, 0);
        ID3D11DeviceContext_RSSetState(c, g->rs_scissor);
      }
      else if (!b->tri) {
        UINT stride = sizeof(BuiGpuInst);
        ID3D11DeviceContext_IASetInputLayout(c, g->il_quad);
        ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &g->ib, &stride, &off);
        ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11DeviceContext_VSSetShader(c, g->vs_quad, NULL, 0);
        ID3D11DeviceContext_PSSetShader(c, g->ps_quad, NULL, 0);
      }
      else {
        UINT stride = sizeof(TriV);
        ID3D11DeviceContext_IASetInputLayout(c, g->il_tri);
        ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &g->tb, &stride, &off);
        ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(c, g->vs_tri, NULL, 0);
        ID3D11DeviceContext_PSSetShader(c, g->ps_tri, NULL, 0);
      }
    }
    if (b->tri == 2) {
      ID3D11ShaderResourceView *ts = dtex_srv(g, b->tex);
      ID3D11DeviceContext_PSSetShaderResources(c, 4, 1, &ts);
      ID3D11DeviceContext_PSSetSamplers(c, 0, 1, b->nearest ? &g->smp_point : &g->smp_linear);
      const D3D11_RECT sr = {b->clip[0], b->clip[1], b->clip[2], b->clip[3]};
      ID3D11DeviceContext_RSSetScissorRects(c, 1, &sr);
      ID3D11DeviceContext_DrawIndexed(c, (UINT)b->count, (UINT)b->first, 0);
    }
    else if (!b->tri) {
      const int want = b->img > 0 && g->big[b->img - 1].srv ? b->img : 0;
      if (want != img) { /* t1: the atlas, or a big image's own texture (t5: NV12 chroma) */
        img = want;
        ID3D11ShaderResourceView *t1 = img ? g->big[img - 1].srv : g->rgba.srv;
        ID3D11ShaderResourceView *t5 = img ? g->big[img - 1].uv_srv : NULL;
        ID3D11DeviceContext_PSSetShaderResources(c, 1, 1, &t1);
        ID3D11DeviceContext_PSSetShaderResources(c, 5, 1, &t5);
      }
      ID3D11DeviceContext_DrawInstanced(c, 4, (UINT)b->count, 0, (UINT)b->first);
    }
    else {
      ID3D11DeviceContext_Draw(c, (UINT)b->count, (UINT)b->first);
    }
  }
  if (cur == 2) {
    ID3D11DeviceContext_RSSetState(c, g->rs);
  }
  g->n_b = g->n_inst = g->n_tv = g->n_mv = g->n_mi = 0;
}

static int ensure_swap(BuiGpu *g)
{
  if (g->swap && slack_fits(g, g->sw, g->w) && slack_fits(g, g->sh, g->h)) {
    return 1;
  }
  const int cw = slack_cap(g, g->w), ch = slack_cap(g, g->h);
  if (g->swap) {
    ID3D11DeviceContext_OMSetRenderTargets(g->ctx, 0, NULL, NULL);
    if (FAILED(IDXGISwapChain1_ResizeBuffers(g->swap, 0, (UINT)cw, (UINT)ch, DXGI_FORMAT_UNKNOWN,
                                             0)))
    {
      return 0;
    }
  }
  else {
    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = (UINT)cw;
    sd.Height = (UINT)ch;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    if (g->alpha) {
      /* bitblt model, as SDL's transparent D3D11 windows: Present copies the alpha channel into
       * the window's surface, which DWM blends (the host enables blur-behind) */
      sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
      sd.Scaling = DXGI_SCALING_STRETCH;
    }
    else {
      sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      sd.Scaling = DXGI_SCALING_NONE;
      sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    }
    if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(g->factory, (IUnknown *)g->dev, g->hwnd, &sd,
                                                    NULL, NULL, &g->swap)))
    {
      return 0;
    }
    IDXGIFactory2_MakeWindowAssociation(g->factory, g->hwnd, DXGI_MWA_NO_ALT_ENTER);
  }
  g->sw = cw;
  g->sh = ch;
  g->swap_fresh = 1;
  return 1;
}

void bui_gpu_clear(BuiGpu *g, int x, int y, int w, int h)
{
  flush(g); /* what was recorded for earlier regions comes first */
  if (!g->ctx1 || !g->target || g->fw != g->w || g->fh != g->h) {
    return; /* a new target starts transparent */
  }
  const float clear[4] = {0, 0, 0, 0};
  const RECT rect = {x, y, x + w, y + h};
  ID3D11DeviceContext1_ClearView(g->ctx1, (ID3D11View *)g->rtv, clear, &rect, 1);
}

int bui_gpu_present(BuiGpu *g, int full)
{
  flush(g);
  if (!g->target || !ensure_swap(g)) {
    g->n_dmg = g->dmg_full = 0;
    return g->target != NULL && !g->alpha; /* no alpha surface: the renderer is given up */
  }
  ID3D11Texture2D *back = NULL;
  if (FAILED(IDXGISwapChain1_GetBuffer(g->swap, 0, &k_IID_ID3D11Texture2D, (void **)&back))) {
    return 0;
  }
  for (int i = 0; i < g->n_dmg; i++) { /* clamp: DXGI rejects rects outside the buffer */
    RECT *r = &g->dmg[i];
    r->left = r->left < 0 ? 0 : r->left;
    r->top = r->top < 0 ? 0 : r->top;
    r->right = r->right > g->w ? g->w : r->right;
    r->bottom = r->bottom > g->h ? g->h : r->bottom;
    if (r->right <= r->left || r->bottom <= r->top) {
      *r = g->dmg[--g->n_dmg];
      i--;
    }
  }
  const int whole = full || g->dmg_full || g->swap_fresh || g->n_dmg == 0 || g->alpha;
  /* copying rects adding up to half the frame or more: one copy of it all is cheaper */
  long area = 0;
  for (int k = 0; k < 2 && !whole && !g->prev_whole; k++) {
    const RECT *rs = k ? g->prev : g->dmg;
    for (int i = 0, n = k ? g->n_prev : g->n_dmg; i < n; i++) {
      area += (long)(rs[i].right - rs[i].left) * (rs[i].bottom - rs[i].top);
    }
  }
  if (whole || g->prev_whole || area * 2 >= (long)g->w * g->h) {
    /* the buffer is two frames old (or of the bitblt model): the whole frame */
    /* the target can be larger than the window (it grows in steps): copy the frame, not the resource */
    const D3D11_BOX frame = {0, 0, 0, (UINT)g->w, (UINT)g->h, 1};
    ID3D11DeviceContext_CopySubresourceRegion(g->ctx, (ID3D11Resource *)back, 0, 0, 0, 0,
                                              (ID3D11Resource *)g->target, 0, &frame);
  }
  else { /* what changed this frame and the last one: the rest is already there */
    for (int k = 0; k < 2; k++) {
      const RECT *rs = k ? g->prev : g->dmg;
      for (int i = 0, n = k ? g->n_prev : g->n_dmg; i < n; i++) {
        const D3D11_BOX b = {(UINT)rs[i].left, (UINT)rs[i].top, 0, (UINT)rs[i].right, (UINT)rs[i].bottom, 1};
        ID3D11DeviceContext_CopySubresourceRegion(g->ctx, (ID3D11Resource *)back, 0, b.left, b.top, 0,
                                                  (ID3D11Resource *)g->target, 0, &b);
      }
    }
  }
  REL(back);
  g->prev_whole = whole;
  g->n_prev = whole ? 0 : g->n_dmg;
  memcpy(g->prev, g->dmg, sizeof(RECT) * (size_t)g->n_prev);
  DXGI_PRESENT_PARAMETERS pp = {0};
  if (!(full || g->dmg_full || g->swap_fresh) && g->n_dmg > 0) {
    pp.DirtyRectsCount = (UINT)g->n_dmg;
    pp.pDirtyRects = g->dmg;
  }
  const HRESULT hr = IDXGISwapChain1_Present1(g->swap, 0, 0, &pp);
  /* occluded or minimized: DWM dropped the frame, so the next one must be whole */
  g->swap_fresh = hr == DXGI_STATUS_OCCLUDED;
  g->n_dmg = g->dmg_full = 0;
  return !(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET);
}

int bui_gpu_read(BuiGpu *g, uint32_t *dst, int w, int h)
{
  flush(g);
  if (!g->target || g->fw != w || g->fh != h) {
    return 0;
  }
  D3D11_TEXTURE2D_DESC td = {0};
  td.Width = (UINT)w;
  td.Height = (UINT)h;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *st = NULL;
  if (FAILED(ID3D11Device_CreateTexture2D(g->dev, &td, NULL, &st))) {
    return 0;
  }
  const D3D11_BOX frame = {0, 0, 0, (UINT)w, (UINT)h, 1};
  ID3D11DeviceContext_CopySubresourceRegion(g->ctx, (ID3D11Resource *)st, 0, 0, 0, 0,
                                            (ID3D11Resource *)g->target, 0, &frame);
  D3D11_MAPPED_SUBRESOURCE m;
  int ok = 0;
  if (SUCCEEDED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
    /* like the raster: 0x00RRGGBB, or premultiplied 0xAARRGGBB with alpha */
    const uint32_t mask = g->alpha ? 0xFFFFFFFFu : 0xFFFFFFu;
    for (int y = 0; y < h; y++) {
      const uint32_t *src = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
      for (int x = 0; x < w; x++) {
        dst[(size_t)y * w + x] = src[x] & mask;
      }
    }
    ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource *)st, 0);
    ok = 1;
  }
  REL(st);
  return ok;
}

/** \} */

#elif !defined(BUI_GPU_GL) /* no GPU backend here (gpu_gl.c provides one on Linux;
                            * built without BUI_GPU): every raster stays in software */

#  include "gpu_d3d11.h"

int bui_gpu_slug_on(BuiGpu *g)
{
  (void)g;
  return 0;
}
int bui_gpu_slug_glyph(BuiGpu *g, int face, int glyph, unsigned gen, float r_bbox[4],
                       float r_band[4], int r_loc[4])
{
  (void)g;
  (void)face;
  (void)glyph;
  (void)gen;
  (void)r_bbox;
  (void)r_band;
  (void)r_loc;
  return 0;
}

#  include <stddef.h>

#  include "gpu_d3d11.h"

BuiGpu *bui_gpu_create(void *hwnd, int alpha)
{
  (void)hwnd;
  (void)alpha;
  return NULL;
}
void bui_gpu_free(BuiGpu *g)
{
  (void)g;
}
int bui_gpu_state(BuiGpu *g, int wait)
{
  (void)g;
  (void)wait;
  return -1;
}
void bui_gpu_begin(BuiGpu *g, int w, int h)
{
  (void)g;
  (void)w;
  (void)h;
}
int bui_gpu_image_tex(BuiGpu *g, uint64_t key, uint64_t base, int stream, int format,
                      const uint32_t *argb, int stride, int w, int h)
{
  (void)g;
  (void)key;
  (void)base;
  (void)stream;
  (void)format;
  (void)argb;
  (void)stride;
  (void)w;
  (void)h;
  return 0;
}
int bui_gpu_max_texture(BuiGpu *g)
{
  (void)g;
  return 0;
}
BuiGpuInst *bui_gpu_inst_image(BuiGpu *g, int tex, int x0, int y0, int x1, int y1)
{
  (void)g;
  (void)tex;
  (void)x0;
  (void)y0;
  (void)x1;
  (void)y1;
  return NULL;
}
BuiGpuInst *bui_gpu_inst(BuiGpu *g, int kind, int x0, int y0, int x1, int y1)
{
  (void)g;
  (void)kind;
  (void)x0;
  (void)y0;
  (void)x1;
  (void)y1;
  return NULL;
}
int bui_gpu_mask(BuiGpu *g, const void *key, unsigned gen, const uint8_t *mask, int w, int h,
                 int *r_x, int *r_y)
{
  (void)g;
  (void)key;
  (void)gen;
  (void)mask;
  (void)w;
  (void)h;
  (void)r_x;
  (void)r_y;
  return 0;
}
int bui_gpu_image(BuiGpu *g, uint64_t key, const uint32_t *argb, int w, int h, int *r_x, int *r_y)
{
  (void)g;
  (void)key;
  (void)argb;
  (void)w;
  (void)h;
  (void)r_x;
  (void)r_y;
  return 0;
}
void bui_gpu_triangles(
    BuiGpu *g, const float (*co)[2], const float (*col)[4], int count, const int clip[4])
{
  (void)g;
  (void)co;
  (void)col;
  (void)count;
  (void)clip;
}
void bui_gpu_mesh(BuiGpu *g,
                  const struct BuiMeshVertex *vtx,
                  int n_vtx,
                  const void *idx,
                  int idx32,
                  int n_idx,
                  float ox,
                  float oy,
                  int tex,
                  int nearest,
                  const int clip[4])
{
  (void)g;
  (void)vtx;
  (void)n_vtx;
  (void)idx;
  (void)idx32;
  (void)n_idx;
  (void)ox;
  (void)oy;
  (void)tex;
  (void)nearest;
  (void)clip;
}
void bui_gpu_clear(BuiGpu *g, int x, int y, int w, int h)
{
  (void)g;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
}
void bui_gpu_damage(BuiGpu *g, int x, int y, int w, int h)
{
  (void)g;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
}
int bui_gpu_present(BuiGpu *g, int full)
{
  (void)g;
  (void)full;
  return 0;
}
int bui_gpu_read(BuiGpu *g, uint32_t *dst, int w, int h)
{
  (void)g;
  (void)dst;
  (void)w;
  (void)h;
  return 0;
}

#endif /* _WIN32 */
