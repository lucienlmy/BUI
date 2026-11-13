/* A native top-level window that drives a Context in C: input, layout,
 * rendering and presentation stay in the kit; D only runs callbacks.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.window;

import core.memory : GC;

import bui.context : Context;
import bui.errors : BuiException;
import bui.internal.util : cstr;
import bui.kit;
import bui.raster : Raster;

/// A native window for `context`.
final class Window {
    /// The C handle, for calls into the raw `bui.kit` API.
    BKWindow* handle;

    /// The context this window drives; kept alive for the window's lifetime.
    Context context;

    private {
        BKWindowCallbacks callbacks;
        void delegate(int id) timerHandler;
        bool delegate() closeHandler;
        void delegate() frameHandler;
        void delegate(bool active) focusHandler;
        string title_;
    }

    /// Create and (optionally) show a window.
    this(Context context, string title = "BUI", int width = 640, int height = 480) {
        if (context is null || context.handle is null) {
            throw new BuiException("Window", "no context");
        }
        this.context = context;
        title_ = title;
        handle = BK_window_new(context.handle, cstr(title), width, height);
        if (handle is null) {
            throw new BuiException("Window", "the platform window could not be created");
        }
        // The native side keeps a raw pointer to this object: root it until `free`.
        GC.addRoot(cast(void*) this);
        context.windows += 1;
        callbacks.data = cast(void*) this;
        callbacks.on_timer = &timerCallback;
        callbacks.on_close = &closeCallback;
        callbacks.on_frame = &frameCallback;
        callbacks.on_focus = &focusCallback;
        BK_window_set_callbacks(handle, &callbacks);
    }

    ~this() {
        free();
    }

    /// Free the native window. The context stays alive.
    void free() {
        if (handle is null) {
            return;
        }
        auto h = handle;
        handle = null;
        BK_window_free(h);
        context.windows -= 1;
        GC.removeRoot(cast(void*) this);
        context = null;
        timerHandler = null;
        closeHandler = null;
        frameHandler = null;
        focusHandler = null;
    }

    /* -- callbacks --------------------------------------------------------- */

    /// A `BK_window_timer` fired; receives the timer id.
    void onTimer(void delegate(int id) handler) {
        timerHandler = handler;
    }

    /// The user asked to close the window; return true to let it close.
    void onClose(bool delegate() handler) {
        closeHandler = handler;
    }

    /// Called right before a requested animation frame renders.
    void onFrame(void delegate() handler) {
        frameHandler = handler;
    }

    /// The window got (`true`) or lost (`false`) the keyboard focus.
    void onFocus(void delegate(bool active) handler) {
        focusHandler = handler;
    }

    /* -- window ------------------------------------------------------------ */

    void show() {
        BK_window_show(handle);
    }

    void close() {
        BK_window_close(handle);
    }

    bool isOpen() const {
        return BK_window_is_open(handle) != 0;
    }

    /// Set the title; `title` returns the last one set.
    void setTitle(string text) {
        title_ = text;
        BK_window_set_title(handle, cstr(text));
    }

    /// The last title set.
    string title() const {
        return title_;
    }

    /// Resize in logical pixels.
    void setSize(int width, int height) {
        BK_window_set_size(handle, width, height);
    }

    /// Current size in logical pixels.
    int[2] size() const {
        int w, h;
        BK_window_get_size(handle, &w, &h);
        return [w, h];
    }

    /// The window's raster, as a borrowed wrapper.
    Raster raster() const {
        return new Raster(BK_window_raster(handle), false);
    }

    /// Render on the GPU (Direct3D 11) when the platform supports it.
    void setGpu(bool on) {
        BK_window_set_gpu(handle, on ? 1 : 0);
    }

    /// Non-zero once frames are drawn on the GPU.
    bool gpu() const {
        return BK_window_gpu(handle) != 0;
    }

    /// Frames presented so far.
    int frames() const {
        return BK_window_frames(handle);
    }

    /// Render and present immediately.
    void renderNow() {
        BK_window_render_now(handle);
    }

    /// A one-shot timer; returns its id.
    int timer(int ms) {
        return BK_window_timer(handle, ms);
    }

    /// Ask for an animation frame; `vsync` paces frames to the display.
    void requestFrame(bool vsync = false) {
        BK_window_request_frame(handle, vsync ? 1 : 0);
    }

    /// Cancel a timer returned by `timer`.
    void cancelTimer(int id) {
        BK_window_cancel_timer(handle, id);
    }

    /// HWND / X11 Window / NSWindow *.
    void* nativeHandle() const {
        return BK_window_handle(handle);
    }

    /// Window-manager request: "pos", "minsize", "resizable", "topmost",
    /// "alpha", "state", "transparent", "layer", "anchor", "lift", ...
    void wm(string key, double a = 0, double b = 0) {
        BK_window_wm(handle, cstr(key), a, b);
    }

    /// Query "pos", "state", "screen", "pointer" or "scale".
    bool wmGet(string key, out double a, out double b) const {
        return BK_window_wm_get(handle, cstr(key), &a, &b) != 0;
    }

    private static extern(C) void timerCallback(void* data, BKWindow*, int id) {
        auto self = cast(Window) data;
        if (self is null || self.timerHandler is null) {
            return;
        }
        try {
            self.timerHandler(id);
        }
        catch (Throwable) {
            // The kit calls timers from its message loop; nothing can unwind.
        }
    }

    private static extern(C) int closeCallback(void* data, BKWindow*) {
        auto self = cast(Window) data;
        if (self is null || self.closeHandler is null) {
            return 1;
        }
        try {
            return self.closeHandler() ? 1 : 0;
        }
        catch (Throwable) {
            return 1;
        }
    }

    private static extern(C) void frameCallback(void* data, BKWindow*) {
        auto self = cast(Window) data;
        if (self is null || self.frameHandler is null) {
            return;
        }
        try {
            self.frameHandler();
        }
        catch (Throwable) {
            // See timerCallback.
        }
    }

    private static extern(C) void focusCallback(void* data, BKWindow*, int active) {
        auto self = cast(Window) data;
        if (self is null || self.focusHandler is null) {
            return;
        }
        try {
            self.focusHandler(active != 0);
        }
        catch (Throwable) {
            // See timerCallback.
        }
    }
}

/* -- platform and event loop ----------------------------------------------- */

/// Non-zero when native windows are available on this platform and thread.
bool windowSupported() {
    return BK_window_supported() != 0;
}

/// Run the message loop until every window is closed.
void run() {
    BK_window_run();
}

/// Handle pending messages and redraws once; returns the open window count.
int pump() {
    return BK_window_pump();
}

/// Stop `run` after the current message.
void quit() {
    BK_window_quit();
}

/// Wake the message loop from another thread.
void wake() {
    BK_window_wake();
}

/// Install the wake callback; returns non-zero when the platform supports it.
bool wakeInit(BKWakeInitFn callback) {
    return BK_window_wake_init(callback) != 0;
}

/// Return non-zero to stop the message loop; null clears the check.
void setStopCheck(BKStopCheckFn stop) {
    BK_window_set_stop_check(stop);
}

/// Keep the process alive while hidden windows still have timers.
void setKeepAlive(bool on) {
    BK_window_set_keep_alive(on ? 1 : 0);
}
