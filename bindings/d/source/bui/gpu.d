/* BUI GPU layer: the software raster, text engine, textures and the
 * immediate-mode sink seam.
 *
 * Raw D declarations for gpu/include/GPU_raster.h and
 * gpu/include/GPU_backend_sink.h. The high-level API in `bui` wraps the
 * pieces an application normally uses; this module is the complete,
 * one-to-one surface for everything else.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.gpu;

import core.stdc.config : c_ulong;
import core.stdc.stdint : uint8_t, uint32_t, uint64_t;

/// Opaque frame buffer (software raster or GPU-backed raster).
struct BuiRaster;

enum BUI_FONT_BOLD = 1 << 0;
enum BUI_FONT_ITALIC = 1 << 1;
enum BUI_FONT_UNDERLINE = 1 << 2;

enum BUI_CORNER_TOP_LEFT = 1 << 0;
enum BUI_CORNER_TOP_RIGHT = 1 << 1;
enum BUI_CORNER_BOTTOM_RIGHT = 1 << 2;
enum BUI_CORNER_BOTTOM_LEFT = 1 << 3;
enum BUI_CORNER_ALL = 15;

enum BUI_PIXMAP_ARGB = 0;
enum BUI_PIXMAP_NV12_BT601 = 1;
enum BUI_PIXMAP_NV12_BT709 = 2;

enum BUI_IMAGE_NEAREST = 1 << 0;
enum BUI_IMAGE_ATLAS_MAX = 2047;

enum BUI_MESH_INDEX32 = 1 << 0;
enum BUI_MESH_NEAREST = 1 << 1;

/// One image: premultiplied 0xAARRGGBB, or NV12 video (see GPU_raster.h).
struct BuiPixmap {
    const(uint32_t)* px;
    int w;
    int h;
    uint64_t key;
    int format;
    uint64_t stream;
}

/// Parameters of Blender's widget-base shader, in screen space (y down).
struct BuiWidgetBase {
    float[4] rect;
    float rad;
    float radi;
    int corners;
    float[4] inner1;
    float[4] inner2;
    float[4] outline;
    float[4] emboss;
    int shade_horizontal;
    int alpha_checker;
    float pixelsize;
}

/// One mesh vertex: the memory layout of Dear ImGui's ImDrawVert.
struct BuiMeshVertex {
    float x;
    float y;
    float u;
    float v;
    uint32_t col;
}

/// The primitive sink: BUI hands screen-space drawing to a host renderer.
struct BuiGpuSink {
    void* user;
    extern(C) void function(void*, int, int) begin_frame;
    extern(C) void function(void*, int, int, int, int) scissor;
    extern(C) void function(void*, const(float)*, const(float)*, int) triangles;
    extern(C) void function(void*, const(float)*, const(float)*, int) lines;
    extern(C) void function(void*, const(float)*, const(float)*, int, float) points;
    extern(C) void function(void*, float, float, const(char)*, float, const(float)*) text;
    extern(C) void function(void*) end_frame;
}

/// An X11 window for GPU attach: Display*, Window and its VisualID (0: default).
struct BuiGpuX11Window {
    void* display;
    c_ulong window;
    c_ulong visual;
}

extern(C) {

/* -- rasters --------------------------------------------------------------- */
BuiRaster* BUI_raster_create(int width, int height);
void BUI_raster_free(BuiRaster* r);

void BUI_raster_begin_frame(BuiRaster* r, int width, int height);
void BUI_raster_end_frame(BuiRaster* r);

int BUI_raster_width(const(BuiRaster)* r);
int BUI_raster_height(const(BuiRaster)* r);
const(uint32_t)* BUI_raster_pixels(const(BuiRaster)* r);

int BUI_raster_damage(const(BuiRaster)* r, const(int[4])** r_rects);
void BUI_raster_invalidate(BuiRaster* r);

void BUI_raster_set_alpha(BuiRaster* r, int on);
int BUI_raster_alpha(const(BuiRaster)* r);
void BUI_raster_clear(BuiRaster* r, int x, int y, int w, int h);

void BUI_raster_set_clip(BuiRaster* r, int x, int y, int w, int h);

void BUI_raster_fill_rect(BuiRaster* r, float x, float y, float w, float h, const(float)* rgba);
void BUI_raster_frame_rect(BuiRaster* r,
                           float x,
                           float y,
                           float w,
                           float h,
                           float thickness,
                           const(float)* rgba);
void BUI_raster_round_rect(BuiRaster* r,
                           float x,
                           float y,
                           float w,
                           float h,
                           float radius,
                           const(float)* rgba);
void BUI_raster_circle(BuiRaster* r,
                       float cx,
                       float cy,
                       float radius,
                       float line_width,
                       const(float)* rgba);
void BUI_raster_line(
    BuiRaster* r, float x1, float y1, float x2, float y2, float width, const(float)* rgba);
void BUI_raster_ellipse(BuiRaster* r,
                        float cx,
                        float cy,
                        float rx,
                        float ry,
                        float line_width,
                        const(float)* rgba);
void BUI_raster_polygon(BuiRaster* r, const(float)* points, int n, const(float)* rgba);
void BUI_raster_widget(BuiRaster* r,
                       float x,
                       float y,
                       float w,
                       float h,
                       float radius,
                       int corners,
                       const(float)* inner_top,
                       const(float)* inner_bottom,
                       const(float)* outline,
                       float outline_width,
                       const(float)* emboss);
void BUI_raster_widget_base(BuiRaster* r, const(BuiWidgetBase)* params);
void BUI_raster_image(BuiRaster* r,
                      float x,
                      float y,
                      float w,
                      float h,
                      const(BuiPixmap)* img,
                      const(float)* src,
                      float alpha,
                      int flags);
void BUI_image_forget(uint64_t key);
void BUI_pixmap_read(const(BuiPixmap)* img, int y0, int rows, uint32_t* dst);
void BUI_raster_end_frame_region(BuiRaster* r, int x, int y, int w, int h);
void BUI_raster_shadow(
    BuiRaster* r, float x, float y, float w, float h, float radius, float size, float alpha);
void BUI_raster_triangles(BuiRaster* r, const(float)* coords, const(float)* colors, int count);
void BUI_raster_hsv_circle(BuiRaster* r, float cx, float cy, float radius, float value);
void BUI_raster_gradient(BuiRaster* r,
                         float x,
                         float y,
                         float w,
                         float h,
                         const(float)* tl,
                         const(float)* tr,
                         const(float)* br,
                         const(float)* bl);
void BUI_raster_sphere(
    BuiRaster* r, float cx, float cy, float radius, const(float)* light, const(float)* rgba);
void BUI_raster_points(
    BuiRaster* r, const(float)* coords, const(float)* colors, int count, float size);

/* -- textures and meshes --------------------------------------------------- */
int BUI_texture_create(int w, int h, const(void)* rgba, int pitch);
int BUI_texture_update(int tex, int x, int y, int w, int h, const(void)* rgba, int pitch);
void BUI_texture_free(int tex);
int BUI_texture_size(int tex, int* r_w, int* r_h);
void BUI_raster_mesh(BuiRaster* r,
                     const(BuiMeshVertex)* vtx,
                     int n_vtx,
                     const(void)* idx,
                     int n_idx,
                     int flags,
                     int tex,
                     const(float)* clip,
                     float ox,
                     float oy);

/* -- text ------------------------------------------------------------------ */
int BUI_text_native();
void BUI_raster_set_font(BuiRaster* r, const(char)* family, int flags);
int BUI_raster_text(BuiRaster* r,
                    float x,
                    float y,
                    const(char)* utf8,
                    int len,
                    float size,
                    const(float)* rgba);
int BUI_text_measure(
    const(char)* family, int flags, float size, const(char)* utf8, int len, float* r_width);

size_t BUI_raster_view_size();
void BUI_raster_view(const(BuiRaster)* src, BuiRaster* dst, int x0, int y0, int x1, int y1);

struct BuiTextRun;
const(BuiTextRun)* BUI_text_prepare(
    const(char)* family, int flags, float size, const(char)* utf8, int len, float* r_width);
uint BUI_text_generation();
void BUI_raster_text_run(BuiRaster* r, float x, float y, const(BuiTextRun)* run, const(float)* rgba);
const(uint8_t)* BUI_text_run_mask(const(BuiTextRun)* run,
                                  int* r_w,
                                  int* r_h,
                                  int* r_ox,
                                  int* r_ascent);
int BUI_text_add_font(const(char)* family, int flags, const(void)* data, int len);
const(void)* BUI_text_default_font(int* r_len);

/* -- GPU renderers --------------------------------------------------------- */
int BUI_raster_gpu_attach(BuiRaster* r, void* window_handle, int wait);
int BUI_raster_gpu_poll(BuiRaster* r);
int BUI_raster_gpu_pending(const(BuiRaster)* r);
void BUI_raster_gpu_release(BuiRaster* r);
int BUI_raster_is_gpu(const(BuiRaster)* r);
void BUI_gpu_set_default(int on);
int BUI_gpu_default();
void BUI_gpu_set_slug(int on);
int BUI_gpu_slug();
const(BuiGpuSink)* BUI_raster_sink(BuiRaster* r);
int BUI_raster_present_native(BuiRaster* r, void* window_handle, int full);

/* -- the sink seam --------------------------------------------------------- */
void GPU_backend_sink_set(const(BuiGpuSink)* sink);
const(BuiGpuSink)* GPU_backend_sink_get();
void GPU_viewport_size_set(int width, int height);

} // extern(C)
