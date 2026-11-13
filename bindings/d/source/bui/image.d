/* Decoded images and the kit's shared image cache.
 *
 * `Image` wraps a `BKImBuf`: decoded pixels, premultiplied 0xAARRGGBB. The
 * kit borrows an image attached to a node or canvas item, so a wrapper that
 * owns its buffer must outlive that use.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.image;

import core.atomic : atomicLoad, atomicStore;

import bui.errors : BuiException;
import bui.internal.util : cslice, cstr;
import bui.kit;

/// Format, size and frame count from `Image.probe`.
struct ImageInfo {
    int format;
    int width;
    int height;
    int frames;
}

/// The shared image cache counters.
struct ImageStats {
    size_t bytes;
    size_t budget;
    int entries;
    int pending;
    int decoded;
    int failed;
    int cancelled;
    int evicted;
    double decodeMs;
}

/// A decoded image.
final class Image {
    /// The C handle, for calls into the raw `bui.kit` API.
    BKImBuf* handle;

    private bool owned;

    /// Wrap an existing buffer; borrowed buffers are not freed.
    package this(BKImBuf* existing, bool owned = true) {
        handle = existing;
        this.owned = owned;
    }

    ~this() {
        free();
    }

    /// Free an owned buffer; borrowed buffers are left alone.
    void free() {
        if (handle !is null && owned) {
            BK_imbuf_free(handle);
        }
        handle = null;
    }

    /// Decode `data` (any format the kit supports), optionally downscaled.
    static Image decode(const(void)[] data, int maxW = 0, int maxH = 0) {
        if (data.length == 0) {
            throw new BuiException("Image.decode", "empty data");
        }
        return wrap(BK_imbuf_decode(data.ptr, data.length, maxW, maxH), "decode");
    }

    /// Decode one frame of an animated image.
    static Image decodeFrame(
        const(void)[] data, int frame, int maxW = 0, int maxH = 0) {
        if (data.length == 0) {
            throw new BuiException("Image.decodeFrame", "empty data");
        }
        return wrap(BK_imbuf_decode_frame(data.ptr, data.length, frame, maxW, maxH),
                    "decodeFrame");
    }

    /// Load and decode a file.
    static Image load(string path, int maxW = 0, int maxH = 0) {
        return wrap(BK_imbuf_load(cstr(path), maxW, maxH), "load");
    }

    /// Copy straight RGBA pixels; `stride` is bytes per row (0 = tightly packed).
    static Image fromRgba(const(ubyte)[] rgba, int width, int height, int stride = 0) {
        if (width <= 0 || height <= 0) {
            throw new BuiException("Image.fromRgba", "empty size");
        }
        if (stride < 0) {
            throw new BuiException("Image.fromRgba", "negative stride");
        }
        const row = stride != 0 ? stride : width * 4;
        if (rgba.length < (height - 1) * row + width * 4) {
            throw new BuiException("Image.fromRgba", "rgba is smaller than the image");
        }
        return wrap(BK_imbuf_from_rgba(rgba.ptr, width, height, stride), "fromRgba");
    }

    /// Copy premultiplied 0xAARRGGBB pixels.
    static Image fromArgb(const(uint)[] argb, int width, int height) {
        if (width <= 0 || height <= 0 || argb.length < width * height) {
            throw new BuiException("Image.fromArgb", "argb is smaller than w * h");
        }
        return wrap(BK_imbuf_from_argb(argb.ptr, width, height), "fromArgb");
    }

    /// Probe `data` without decoding it.
    static bool probe(const(void)[] data, out ImageInfo info) {
        BKImageInfo raw;
        const ok = data.length != 0 && BK_image_probe(data.ptr, data.length, &raw) != 0;
        info = ImageInfo(raw.format, raw.width, raw.height, raw.frames);
        return ok;
    }

    /// Probe a file without decoding it.
    static bool probeFile(string path, out ImageInfo info) {
        BKImageInfo raw;
        const ok = path.length != 0 && BK_image_probe_file(cstr(path), &raw) != 0;
        info = ImageInfo(raw.format, raw.width, raw.height, raw.frames);
        return ok;
    }

    /// A format name such as "png".
    static string formatName(int format) {
        return cslice(BK_image_format_name(format)).idup;
    }

    private static Image wrap(BKImBuf* buffer, string operation) {
        if (buffer is null) {
            throw new BuiException("Image." ~ operation, "decoding failed");
        }
        return new Image(buffer);
    }

    /* -- data -------------------------------------------------------------- */

    int format() const {
        return BK_imbuf_format(handle);
    }

    /// width, height
    int[2] size() const {
        int w, h;
        BK_imbuf_size(handle, &w, &h);
        return [w, h];
    }

    int width() const {
        return size()[0];
    }

    int height() const {
        return size()[1];
    }

    /// The number of frames (1 for a still image).
    int frames() const {
        int w, h;
        return BK_imbuf_size(handle, &w, &h);
    }

    /// Frame delay in milliseconds.
    int delay(int frame = 0) const {
        return BK_imbuf_delay(handle, frame);
    }

    /// How many times an animation loops (0 = forever).
    int loops() const {
        return BK_imbuf_loops(handle);
    }

    /// A zero-copy view of the premultiplied 0xAARRGGBB pixels of `frame`.
    const(uint)[] pixels(int frame = 0) const {
        auto p = BK_imbuf_pixels(handle, frame);
        if (p is null) {
            return null;
        }
        int w, h;
        BK_imbuf_size(handle, &w, &h);
        return p[0 .. w * h];
    }

    /// A straight RGBA copy of `frame` (4 bytes per pixel).
    ubyte[] toRgba(int frame = 0) const {
        int w, h;
        BK_imbuf_size(handle, &w, &h);
        auto rgba = new ubyte[w * h * 4];
        if (BK_imbuf_rgba(handle, frame, rgba.ptr) == 0) {
            return null;
        }
        return rgba;
    }

    /* -- GPU --------------------------------------------------------------- */

    /// Upload or reuse a texture for `frame`; returns 0 on failure.
    int texture(int frame = 0) const {
        return BK_imbuf_texture(handle, frame);
    }

    /// Update an existing texture; returns non-zero on success.
    bool textureUpdate(int texture, int frame = 0) const {
        return BK_imbuf_texture_update(handle, frame, texture) != 0;
    }
}

/// An animation stepped by the caller; `next` returns a new frame to free.
final class Animation {
    BKAnim* handle;

    /// Open an animated image (GIF/WebP/APNG) from memory.
    this(const(void)[] data, int maxW = 0, int maxH = 0) {
        if (data.length == 0) {
            throw new BuiException("Animation", "empty data");
        }
        handle = BK_anim_open(data.ptr, data.length, maxW, maxH);
        if (handle is null) {
            throw new BuiException("Animation", "decoding failed");
        }
    }

    ~this() {
        free();
    }

    void free() {
        if (handle !is null) {
            BK_anim_free(handle);
            handle = null;
        }
    }

    /// The next frame, owned by the returned `Image` (free it when done).
    Image next(out int delayMs) {
        return new Image(BK_anim_next(handle, &delayMs), true);
    }

    /// width, height, loops and frame count.
    int[4] info() const {
        int w, h, loops;
        const frames = BK_anim_info(handle, &w, &h, &loops);
        return [w, h, loops, frames];
    }
}

/* -- shared cache ---------------------------------------------------------- */

/// Set the cache budget in bytes (the kit's default applies before this).
void setImageBudget(size_t bytes) {
    BK_image_set_budget(bytes);
}

/// Set the number of decoding threads.
void setImageThreads(int count) {
    BK_image_set_threads(count);
}

private __gshared void delegate() g_imageNotify;
private shared bool g_imageReady;

/// Call `notify` from `pollImages` whenever a background decode finishes. The
/// callback runs on the polling thread, so it can touch the UI safely.
void setImageNotify(void delegate() notify) {
    g_imageNotify = notify;
    BK_image_set_notify(notify is null ? null : &imageNotifyCallback, null);
}

private extern(C) void imageNotifyCallback(void*) {
    /* A decode worker: only flag and wake. The delegate runs in pollImages,
     * on the host's thread, so a D exception never unwinds through C. */
    atomicStore(g_imageReady, true);
    BK_window_wake();
}

/// Finish background decodes; returns the number of completed entries. When
/// decodes arrived since the last poll, the `setImageNotify` delegate runs
/// first, on this thread.
int pollImages() {
    const n = BK_image_poll();
    if (atomicLoad(g_imageReady)) {
        atomicStore(g_imageReady, false);
        if (g_imageNotify !is null) {
            g_imageNotify();
        }
    }
    return n;
}

/// Current cache counters.
ImageStats imageStats() {
    BKImageStats raw;
    BK_image_stats(&raw);
    return ImageStats(raw.bytes, raw.budget, raw.entries, raw.pending, raw.decoded,
                      raw.failed, raw.cancelled, raw.evicted, raw.decode_ms);
}

/// Size and frame count of an image source, without decoding it.
bool imageInfo(string src, out ImageInfo info) {
    BKImageInfo raw;
    const ok = src.length != 0 && BK_image_info(cstr(src), &raw) != 0;
    info = ImageInfo(raw.format, raw.width, raw.height, raw.frames);
    return ok;
}

/// Ask the cache to decode `src` ahead of time.
void prefetchImage(string src, int width, int height, int fit = BK_FIT_CONTAIN) {
    BK_image_prefetch(cstr(src), width, height, fit);
}

/// Drop every cached image.
void clearImageCache() {
    BK_image_clear_cache();
}

/// Stop the image workers (at shutdown).
void imageExit() {
    BK_image_exit();
}
