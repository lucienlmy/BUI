/* A BUI frame buffer: the software raster or a GPU-backed raster.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.raster;

import bui.errors : BuiException;
import bui.gpu;
import bui.internal.util : cstr;
import bui.kit : BKWidgetDraw, BK_draw_widget;

/// A frame buffer an application can render the kit into and read back.
final class Raster {
    BuiRaster* handle;

    private bool owned;

    this(int width, int height) {
        handle = BUI_raster_create(width, height);
        if (handle is null) {
            throw new BuiException("Raster", "creation failed");
        }
        owned = true;
    }

    /// Wrap an existing raster; borrowed rasters (a window's) are not freed.
    package this(BuiRaster* existing, bool owned) {
        handle = existing;
        this.owned = owned;
    }

    ~this() {
        free();
    }

    void free() {
        if (handle !is null && owned) {
            BUI_raster_gpu_release(handle);
            BUI_raster_free(handle);
        }
        handle = null;
    }

    int width() const {
        return BUI_raster_width(handle);
    }

    int height() const {
        return BUI_raster_height(handle);
    }

    /// The pixels as premultiplied 0xAARRGGBB, one row per `width()`.
    const(uint)[] pixels() const {
        return BUI_raster_pixels(handle)[0 .. width * height];
    }

    void beginFrame(int width, int height) {
        BUI_raster_begin_frame(handle, width, height);
    }

    void endFrame() {
        BUI_raster_end_frame(handle);
    }

    /// The regions changed by the last `endFrame`, as x, y, w, h tuples.
    int[4][] damage() const {
        const(int[4])* rects;
        const n = BUI_raster_damage(handle, &rects);
        if (n <= 0 || rects is null) {
            return null;
        }
        auto rectsOut = new int[4][](n);
        foreach (i; 0 .. n) {
            rectsOut[i][] = rects[i][];
        }
        return rectsOut;
    }

    void invalidate() {
        BUI_raster_invalidate(handle);
    }

    /// Keep alpha: frames become premultiplied 0xAARRGGBB. Set before a GPU renderer attaches.
    void setAlpha(bool on) {
        BUI_raster_set_alpha(handle, on ? 1 : 0);
    }

    /// Whether frames keep alpha.
    bool alpha() const {
        return BUI_raster_alpha(handle) != 0;
    }

    /// Set the pixels of a rect to transparent, within the clip.
    void clear(int x, int y, int w, int h) {
        BUI_raster_clear(handle, x, y, w, h);
    }

    void setClip(int x, int y, int w, int h) {
        BUI_raster_set_clip(handle, x, y, w, h);
    }

    void fillRect(float x, float y, float w, float h, const float[4] rgba) {
        BUI_raster_fill_rect(handle, x, y, w, h, rgba.ptr);
    }

    void frameRect(float x, float y, float w, float h, float thickness, const float[4] rgba) {
        BUI_raster_frame_rect(handle, x, y, w, h, thickness, rgba.ptr);
    }

    void roundRect(float x, float y, float w, float h, float radius, const float[4] rgba) {
        BUI_raster_round_rect(handle, x, y, w, h, radius, rgba.ptr);
    }

    void circle(float cx, float cy, float radius, float lineWidth, const float[4] rgba) {
        BUI_raster_circle(handle, cx, cy, radius, lineWidth, rgba.ptr);
    }

    void line(float x1, float y1, float x2, float y2, float width, const float[4] rgba) {
        BUI_raster_line(handle, x1, y1, x2, y2, width, rgba.ptr);
    }

    void ellipse(
        float cx, float cy, float rx, float ry, float lineWidth, const float[4] rgba) {
        BUI_raster_ellipse(handle, cx, cy, rx, ry, lineWidth, rgba.ptr);
    }

    void polygon(const float[] points, const float[4] rgba) {
        BUI_raster_polygon(handle, points.ptr, cast(int) (points.length / 2), rgba.ptr);
    }

    void shadow(float x, float y, float w, float h, float radius, float size, float alpha) {
        BUI_raster_shadow(handle, x, y, w, h, radius, size, alpha);
    }

    void drawWidget(const(BKWidgetDraw)* w) {
        BK_draw_widget(handle, w);
    }

    void setFont(string family, int flags) {
        BUI_raster_set_font(handle, family.length ? cstr(family) : null, flags);
    }

    int text(float x, float y, string utf8, float size, const float[4] rgba) {
        return BUI_raster_text(handle, x, y, utf8.ptr, cast(int) utf8.length, size, rgba.ptr);
    }

    int textureCreate(int w, int h, const(void)* rgba, int pitch) {
        return BUI_texture_create(w, h, rgba, pitch);
    }

    void textureFree(int tex) {
        BUI_texture_free(tex);
    }

    /* -- GPU --------------------------------------------------------------- */

    bool gpuAttach(void* windowHandle = null, bool wait = false) {
        return BUI_raster_gpu_attach(handle, windowHandle, wait ? 1 : 0) != 0;
    }

    bool gpuPoll() {
        return BUI_raster_gpu_poll(handle) != 0;
    }

    bool gpuPending() const {
        return BUI_raster_gpu_pending(handle) != 0;
    }

    bool isGpu() const {
        return BUI_raster_is_gpu(handle) != 0;
    }
}
