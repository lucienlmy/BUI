/* Video playback: a cache entry with its own clock, shown on "video" and
 * "image" nodes or drawn into a texture.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.video;

import bui.errors : BuiException;
import bui.internal.util : cslice, cstr;
import bui.kit;

/// A video source; the kit decodes frames in the background.
final class Video {
    /// The C handle, for calls into the raw `bui.kit` API.
    BKVideo* handle;

    private {
        void delegate(int state) listener;
        Throwable pending;
    }

    /// Open a local file or URL.
    this(string src) {
        if (src.length == 0) {
            throw new BuiException("Video", "empty source");
        }
        handle = BK_video_new(cstr(src));
        if (handle is null) {
            throw new BuiException("Video", "cannot open '" ~ src ~ "'");
        }
    }

    ~this() {
        free();
    }

    void free() {
        if (handle !is null) {
            BK_video_set_listener(handle, null, null);
            BK_video_free(handle);
            handle = null;
        }
        listener = null;
        pending = null;
    }

    /// Play.
    void play() {
        BK_video_play(handle);
    }

    /// Pause.
    void pause() {
        BK_video_pause(handle);
    }

    /// Seek to `seconds`.
    void seek(double seconds) {
        BK_video_seek(handle, seconds);
    }

    /// Playback rate (1 = normal).
    void setRate(double rate) {
        BK_video_set_rate(handle, rate);
    }

    /// Loop at the end.
    void setLoop(bool on) {
        BK_video_set_loop(handle, on ? 1 : 0);
    }

    /// Show `src` until the first frame arrives.
    void setPoster(string src) {
        BK_video_set_poster(handle, cstr(src));
    }

    /// Draw Blender-style play/scrub controls on "video" nodes.
    void setControls(bool on) {
        BK_video_set_controls(handle, on ? 1 : 0);
    }

    /// `BK_VIDEO_*`.
    int state() const {
        return BK_video_state(handle);
    }

    /// Playback position in seconds.
    double time() const {
        return BK_video_time(handle);
    }

    /// Duration in seconds (0 until known).
    double duration() const {
        return BK_video_duration(handle);
    }

    /// width, height
    int[2] size() const {
        int w, h;
        BK_video_size(handle, &w, &h);
        return [w, h];
    }

    /// Copy the current frame into `texture`; returns non-zero on success.
    bool texture(int texture, int maxW = 0, int maxH = 0) {
        return BK_video_texture(handle, texture, maxW, maxH) != 0;
    }

    /// Call `listener` whenever the state changes.
    void setListener(void delegate(int state) callback) {
        listener = callback;
        BK_video_set_listener(handle, callback is null ? null : &listenerCallback,
                              callback is null ? null : cast(void*) this);
    }

    /// The exception a listener caught, if any.
    Throwable pendingError() {
        return pending;
    }

    /// Re-throw a listener exception, if one was caught.
    void check() {
        auto e = pending;
        if (e !is null) {
            pending = null;
            throw e;
        }
    }

    private static extern(C) void listenerCallback(BKVideo*, int state, void* data) {
        auto self = cast(Video) data;
        if (self is null || self.listener is null) {
            return;
        }
        try {
            self.listener(state);
        }
        catch (Throwable e) {
            if (self.pending is null) {
                self.pending = e;
            }
        }
    }
}

/// The compiled-in video backends, for diagnostics.
string videoBackends() {
    return cslice(BK_video_backends()).idup;
}
