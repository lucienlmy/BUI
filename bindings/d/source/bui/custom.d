/* Custom nodes: the host draws and handles input for a "custom" node.
 *
 * A `Custom` object owns the D callbacks and is kept alive by its node. The
 * kit calls the callbacks from its normal input/render path; exceptions are
 * stored and can be rethrown with `check()`.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.custom;

import bui.errors : BuiException;
import bui.gpu : BuiRaster;
import bui.internal.util : cslice;
import bui.kit;
import bui.node : Node, wrapNode;
import bui.raster : Raster;

/// One input event forwarded to a custom node.
struct CustomEvent {
    /// `BK_CUSTOM_*`.
    int type;
    /// Position relative to the node.
    float x;
    float y;
    int button;
    int mods;
    int clicks;
    float wheelX;
    float wheelY;
    /// The typed text, for `BK_CUSTOM_KEY`.
    const(char)[] text;
    /// The key name, for `BK_CUSTOM_KEY` / `BK_CUSTOM_KEYUP`.
    const(char)[] keysym;
}

/// The custom node's implementations.
struct CustomHandlers {
    /// Return non-zero when the event was handled.
    int delegate(Node node, CustomEvent event) onEvent;
    /// Called before a render for each requested animation frame.
    void delegate(Node node) onFrame;
    /// Draw into `raster`; `clip` is the damage rectangle.
    void delegate(Node node, Raster raster, const(int)* clip) onDraw;
    /// The node is being freed.
    void delegate(Node node) onFree;
    int naturalWidth;
    int naturalHeight;
}

/// A custom implementation attached to a node.
final class Custom {
    /// The node this implementation belongs to.
    Node node;

    /// The handlers; replace them any time before the next callback.
    CustomHandlers handlers;

    private Raster raster_;
    private Throwable pending;

    /// The exception a callback caught, if any.
    Throwable pendingError() {
        return pending;
    }

    /// Re-throw a callback exception, if one was caught.
    void check() {
        auto e = pending;
        if (e !is null) {
            pending = null;
            throw e;
        }
    }

    /// The raster passed to the last `onDraw`, if any.
    Raster raster() {
        return raster_;
    }

    private void store(Throwable e) {
        if (pending is null) {
            pending = e;
        }
    }

    private static extern(C) int eventCallback(
        void* data, BKNode* node, const(BKCustomEvent)* raw) {
        auto self = cast(Custom) data;
        if (self is null || self.handlers.onEvent is null) {
            return 0;
        }
        auto wrapper = self.node;
        if (wrapper is null) {
            wrapper = wrapNode(node);
        }
        if (wrapper is null) {
            return 0;
        }
        CustomEvent event;
        event.type = raw.type;
        event.x = raw.x;
        event.y = raw.y;
        event.button = raw.button;
        event.mods = raw.mods;
        event.clicks = raw.clicks;
        event.wheelX = raw.wheel_x;
        event.wheelY = raw.wheel_y;
        event.text = cslice(raw.text);
        event.keysym = cslice(raw.keysym);
        try {
            return self.handlers.onEvent(wrapper, event);
        }
        catch (Throwable e) {
            self.store(e);
            return 0;
        }
    }

    private static extern(C) void frameCallback(void* data, BKNode* node) {
        auto self = cast(Custom) data;
        if (self is null || self.handlers.onFrame is null) {
            return;
        }
        auto wrapper = self.node;
        if (wrapper is null) {
            wrapper = wrapNode(node);
        }
        if (wrapper is null) {
            return;
        }
        try {
            self.handlers.onFrame(wrapper);
        }
        catch (Throwable e) {
            self.store(e);
        }
    }

    private static extern(C) void drawCallback(
        void* data, BKNode* node, BuiRaster* raster, const(int)* clip) {
        auto self = cast(Custom) data;
        if (self is null || self.handlers.onDraw is null) {
            return;
        }
        auto wrapper = self.node;
        if (wrapper is null) {
            wrapper = wrapNode(node);
        }
        if (wrapper is null) {
            return;
        }
        if (self.raster_ is null) {
            self.raster_ = new Raster(raster, false);
        }
        else {
            self.raster_.handle = raster;
        }
        try {
            self.handlers.onDraw(wrapper, self.raster_, clip);
        }
        catch (Throwable e) {
            self.store(e);
        }
    }

    private static extern(C) void freeCallback(void* data, BKNode* node) {
        auto self = cast(Custom) data;
        if (self is null) {
            return;
        }
        auto wrapper = self.node;
        if (wrapper is null) {
            wrapper = wrapNode(node);
        }
        if (self.handlers.onFree !is null && wrapper !is null) {
            try {
                self.handlers.onFree(wrapper);
            }
            catch (Throwable e) {
                self.store(e);
            }
        }
        self.raster_ = null;
        self.node = null;
    }
}

/// Attach `handlers` to a `"custom"` node.
Custom setCustom(Node node, CustomHandlers handlers) {
    if (node is null || node.handle is null) {
        throw new BuiException("Custom.set", "no node");
    }
    auto custom = new Custom();
    custom.node = node;
    custom.handlers = handlers;
    BKCustomClass cls;
    cls.event = &Custom.eventCallback;
    cls.frame = &Custom.frameCallback;
    cls.draw = &Custom.drawCallback;
    cls.free = &Custom.freeCallback;
    cls.natural_w = handlers.naturalWidth;
    cls.natural_h = handlers.naturalHeight;
    if (BK_custom_set(node.handle, &cls, cast(void*) custom) == 0) {
        throw new BuiException("Custom.set", "not a custom node");
    }
    node.keepAlive(custom);
    return custom;
}

/// Ask for an `onFrame` call; `ms <= 0` means the next render.
void requestFrame(Node node, int ms = 0) {
    BK_custom_request_frame(need(node, "requestFrame"), ms);
}

/// Set the pointer shape while it is over the node (`BK_CURSOR_*`).
void setCursor(Node node, int cursor) {
    BK_custom_set_cursor(need(node, "setCursor"), cursor);
}

/// Non-zero while the custom node has keyboard focus.
bool hasFocus(Node node) {
    return BK_custom_has_focus(need(node, "hasFocus")) != 0;
}

private BKNode* need(Node node, string operation) {
    if (node is null || node.handle is null) {
        throw new BuiException("Custom." ~ operation, "no custom node");
    }
    return node.handle;
}
