/* The kit context: one instance of a node tree, its input state, focus,
 * popups and render bookkeeping.
 *
 * The context installs a `BKHost` with C callbacks that forward to the
 * handlers in `bui.context.Host`. Handlers must not throw through the C
 * frames: an exception is stored and rethrown by the next `check()` call (the
 * input and render methods call it for you).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.context;

import core.stdc.stdlib : cfree = free, malloc;
import core.stdc.string : memcpy;

import bui.errors : BuiException;
import bui.gpu : BuiRaster;
import bui.internal.callback : CallbackBox;
import bui.internal.refs : ensureRefHooks;
import bui.internal.util : cslice, cstr;
import bui.kit;
import bui.menu : MenuItem, menuPie = openPie, menuOpen = openMenu, pieAtPointer;
import bui.node : Node, wrapNode;
import bui.raster : Raster;

/// Host services the kit asks for while it runs.
struct Host {
    /// A redraw was requested; schedule `render()`.
    void delegate() request;
    /// The pointer is over a node with a cursor (`BK_CURSOR_*`).
    void delegate(int cursor) setCursor;
    /// Move the pointer to window pixels `x`, `y`.
    void delegate(int x, int y) warp;
    /// Clipboard text, or null when the clipboard has no text.
    string delegate() clipboardGet;
    /// Put `text` on the clipboard.
    void delegate(string text) clipboardSet;
    /// Call `timer(timer)` after `ms` milliseconds.
    void delegate(int ms, int timer) schedule;
}

/// The IME state after a render; `changed` is false when nothing needs to be
/// told to the platform.
struct ImeState {
    bool enabled;
    bool restart;
    int x;
    int y;
    bool changed;
}

/// One instance of a node tree.
final class Context {
    /// The C handle, for calls into the raw `bui.kit` API.
    BKContext* handle;

    /// Live `Window`s driving this context; maintained by `Window`.
    package int windows;

    private {
        Node rootNode;
        Node headerNode;
        Host services;
        BKHost cHost;
        Throwable pending;
    }

    /// Create a context for `root`; the wrapper keeps the root alive.
    this(Node root) {
        if (root is null || root.handle is null) {
            throw new BuiException("Context", "no root node");
        }
        ensureRefHooks();
        rootNode = root;
        handle = BK_context_new(root.handle);
        if (handle is null) {
            throw new BuiException("Context", "creation failed");
        }
        cHost.data = cast(void*) this;
        cHost.request = &requestCallback;
        cHost.set_cursor = &setCursorCallback;
        cHost.warp = &warpCallback;
        cHost.clipboard_get = &clipboardGetCallback;
        cHost.clipboard_set = &clipboardSetCallback;
        cHost.schedule = &scheduleCallback;
        cHost.emit = &emitCallback;
        BK_context_set_host(handle, &cHost);
    }

    ~this() {
        windows = 0; // only reachable at teardown; a finalizer cannot throw
        free();
    }

    /// Release the context. The root node is left to its wrapper.
    /// Throws while a `Window` still drives it; free the window first.
    void free() {
        if (handle is null) {
            return;
        }
        if (windows != 0) {
            throw new BuiException("Context.free", "a window still uses this context");
        }
        auto h = handle;
        handle = null;
        BK_context_free(h);
        rootNode = null;
        headerNode = null;
        services = Host.init;
        pending = null;
    }

    /// Replace the host handlers; call before creating a native window.
    void setHost(Host handlers) {
        services = handlers;
    }

    /// The current host handlers.
    Host host() const {
        return services;
    }

    /// The exception a host callback caught, if any, without clearing it.
    Throwable pendingError() {
        return pending;
    }

    /// Re-throw a host callback exception, if one was caught.
    void check() {
        auto e = pending;
        if (e !is null) {
            pending = null;
            throw e;
        }
    }

    /* -- tree -------------------------------------------------------------- */

    /// The root node this context was created for.
    Node root() {
        return rootNode;
    }

    /// Install a fixed top bar; pass null to remove it.
    void setHeader(Node header) {
        headerNode = header;
        BK_context_set_header(ctx(), header is null ? null : header.handle);
    }

    /// The fixed top bar, if any.
    Node header() {
        return headerNode;
    }

    /* -- rendering --------------------------------------------------------- */

    /// Render into `raster`; returns the number of damaged regions redrawn.
    int render(Raster raster, bool full = false) {
        if (raster is null) {
            throw new BuiException("Context.render", "no raster");
        }
        return render(raster.handle, raster.width, raster.height, full);
    }

    /// Render into a raw raster at `width` x `height`.
    int render(BuiRaster* raster, int width, int height, bool full = false) {
        if (raster is null) {
            throw new BuiException("Context.render", "no raster");
        }
        auto c = ctx();
        check();
        const n = BK_render(c, raster, width, height, full ? 1 : 0);
        check();
        return n;
    }

    /// Non-zero when a redraw is needed.
    bool needsRender() const {
        return BK_needs_render(ctx()) != 0;
    }

    /// Non-zero while panel animations need frames.
    bool animating() const {
        return BK_animating(ctx()) != 0;
    }

    /* -- menus ------------------------------------------------------------- */

    /// Open a context menu owned by `owner` at window pixels `x`, `y`.
    void openMenu(
        Node owner, MenuItem[] items, string title = null, int x = 0, int y = 0) {
        menuOpen(ctx(), owner is null ? null : owner.handle, items, title, x, y);
    }

    /// Open a pie menu (at most eight rows are reachable), centered at window pixels x, y.
    void openPie(MenuItem[] items,
                 string title = null,
                 int x = pieAtPointer,
                 int y = pieAtPointer) {
        menuPie(ctx(), items, title, x, y);
    }

    /// The interactive node under window pixels x, y, or null.
    Node hitAt(int x, int y) {
        auto c = ctx();
        check();
        auto node = wrapNode(BK_hit_at(c, x, y));
        check();
        return node;
    }

    /* -- input ------------------------------------------------------------- */

    /// Mouse button press; returns true when something changed.
    bool press(int x, int y, int mods = 0) {
        auto c = ctx();
        check();
        const changed = BK_press(c, x, y, mods) != 0;
        check();
        return changed;
    }

    /// Mouse button release.
    bool release(int x, int y, int mods = 0) {
        auto c = ctx();
        check();
        const changed = BK_release(c, x, y, mods) != 0;
        check();
        return changed;
    }

    /// Pointer motion.
    bool motion(int x, int y, int mods = 0) {
        auto c = ctx();
        check();
        const changed = BK_motion(c, x, y, mods) != 0;
        check();
        return changed;
    }

    /// Wheel notch; positive is up.
    bool wheel(int x, int y, int delta, int mods = 0) {
        auto c = ctx();
        check();
        const changed = BK_wheel(c, x, y, delta, mods) != 0;
        check();
        return changed;
    }

    /// Precise scrolling in wheel notches; positive `dx` is right, `dy` up.
    bool scroll(int x, int y, float dx, float dy, int mods = 0) {
        auto c = ctx();
        check();
        const changed = BK_scroll(c, x, y, dx, dy, mods) != 0;
        check();
        return changed;
    }

    /// Key press with the character `text` and the key name `keysym`.
    bool key(string text, string keysym = null, int mods = 0) {
        auto c = ctx();
        check();
        const changed =
            BK_key(c, cstr(text), keysym.length ? cstr(keysym) : null, mods) != 0;
        check();
        return changed;
    }

    /// Right-click context menu at `x`, `y`.
    bool contextMenu(int x, int y, int mods = 0) {
        auto c = ctx();
        check();
        const changed = BK_context_menu(c, x, y, mods) != 0;
        check();
        return changed;
    }

    /// Raw key release for "events" nodes.
    bool keyRelease(string keysym, int mods = 0) {
        auto c = ctx();
        check();
        const changed = BK_key_release(c, cstr(keysym), mods) != 0;
        check();
        return changed;
    }

    /// Raw button input for "events" nodes; `button` is 3, 4 or 5.
    bool button(int x, int y, int button, bool release, int mods = 0) {
        auto c = ctx();
        check();
        const changed = BK_button(c, x, y, button, release ? 1 : 0, mods) != 0;
        check();
        return changed;
    }

    /* -- input methods ----------------------------------------------------- */

    /// Forward an IME composition event.
    bool imeEvent(int type,
                  string result,
                  string composite,
                  int cursorPos = -1,
                  int selStart = -1,
                  int selEnd = -1) {
        BKImeEvent e;
        e.result = result.length ? cstr(result) : null;
        e.composite = composite.length ? cstr(composite) : null;
        e.cursor_pos = cursorPos;
        e.sel_start = selStart;
        e.sel_end = selEnd;
        return BK_ime_event(ctx(), type, &e) != 0;
    }

    /// What a host should tell the platform after a render.
    ImeState imeState() {
        BKImeState s;
        const changed = BK_ime_state(ctx(), &s);
        return ImeState(s.enabled != 0, s.restart != 0, s.x, s.y, changed != 0);
    }

    /// Non-zero while a composition is in progress.
    bool imeComposing() const {
        return BK_ime_composing(ctx()) != 0;
    }

    /* -- scheduling and shortcuts ------------------------------------------ */

    /// The kit asks the host to call this timer later.
    void timer(int id) {
        BK_timer(ctx(), id);
    }

    /// Bind `"ctrl+s"` style keys; the callback runs on `BK_EV_CALL`.
    void bind(string combo, void delegate() callback) {
        if (callback is null) {
            return;
        }
        auto box = new CallbackBox(callback);
        BK_bind(ctx(), cstr(combo), cast(void*) box);
    }

    /// Close every open popup, tooltip and menu.
    void closePopups() {
        BK_close_popups(ctx());
    }

    /// Tell the kit where the pointer is (after a warp).
    void setPointer(int x, int y) {
        BK_set_pointer(ctx(), x, y);
    }

    /* -- introspection ----------------------------------------------------- */

    /// The cursor the node under the pointer asks for (`BK_CURSOR_*`).
    int cursor() const {
        return BK_cursor(ctx());
    }

    /// The node under the pointer, if it has a D wrapper.
    Node hover() const {
        return wrapNode(BK_hover(ctx()));
    }

    /// The focused node, if it has a D wrapper.
    Node focus() const {
        return wrapNode(BK_focus(ctx()));
    }

    /// Give a text-editing node focus; null ends the current edit.
    void focusNode(Node node) {
        BK_focus_node(ctx(), node is null ? null : node.handle);
    }

    /// Clipboard text, or null.
    string clipboard() const {
        auto p = BK_clipboard_get(ctx());
        if (p is null) {
            return null;
        }
        auto s = cslice(p).idup;
        cfree(p);
        return s;
    }

    /// Replace the clipboard text.
    void clipboard(string text) {
        BK_clipboard_set(ctx(), cstr(text));
    }

    /// Non-zero while a modal node is open.
    bool modalActive() const {
        return BK_modal_active(ctx()) != 0;
    }

    /// Non-zero while a tooltip is visible.
    bool tooltipVisible() const {
        return BK_tooltip_visible(ctx()) != 0;
    }

    /// Number of open popups.
    int popupCount() const {
        return BK_popup_count(ctx());
    }

    /// Popup `i`, if it has a D wrapper.
    Node popup(int i) const {
        return wrapNode(BK_popup(ctx(), i));
    }

    /// The owner of popup `i`, if it has a D wrapper.
    Node popupOwner(int i) const {
        return wrapNode(BK_popup_owner(ctx(), i));
    }

    /// Visible rows of a menu popup.
    int menuRowCount(Node menu) const {
        return BK_menu_row_count(menu is null ? null : menu.handle);
    }

    /// Text of menu row `i`.
    string menuRowText(Node menu, int i) const {
        return cslice(BK_menu_row_text(menu is null ? null : menu.handle, i)).idup;
    }

    /// Rect (x, y, w, h) of menu row `i`.
    int[4] menuRowRect(Node menu, int i) const {
        int[4] r;
        BK_menu_row_rect(menu is null ? null : menu.handle, i, r.ptr);
        return r;
    }

    /// A color picker popup part: "area", "strip", "tabs", "slider0".., "alpha", ...
    Node pickerPart(Node picker, string name) const {
        return wrapNode(BK_picker_part(picker is null ? null : picker.handle, cstr(name)));
    }

    /* -- undo -------------------------------------------------------------- */

    /// Undo the last UI value edit.
    bool undo() {
        auto c = ctx();
        check();
        const changed = BK_undo(c) != 0;
        check();
        return changed;
    }

    /// Redo.
    bool redo() {
        auto c = ctx();
        check();
        const changed = BK_redo(c) != 0;
        check();
        return changed;
    }

    /// Undo and redo step counts.
    int[2] undoDepth() const {
        int redoSteps;
        const undoSteps = BK_undo_depth(ctx(), &redoSteps);
        return [undoSteps, redoSteps];
    }

    /* -- host callbacks ---------------------------------------------------- */

    private BKContext* ctx() const {
        if (handle is null) {
            throw new BuiException("Context", "freed");
        }
        return cast(BKContext*) handle;
    }

    private void store(Throwable e) {
        if (pending is null) {
            pending = e;
        }
    }

    private static extern(C) void requestCallback(void* data) {
        auto self = cast(Context) data;
        if (self is null || self.services.request is null) {
            return;
        }
        try {
            self.services.request();
        }
        catch (Throwable e) {
            self.store(e);
        }
    }

    private static extern(C) void setCursorCallback(void* data, int cursor) {
        auto self = cast(Context) data;
        if (self is null || self.services.setCursor is null) {
            return;
        }
        try {
            self.services.setCursor(cursor);
        }
        catch (Throwable e) {
            self.store(e);
        }
    }

    private static extern(C) void warpCallback(void* data, int x, int y) {
        auto self = cast(Context) data;
        if (self is null || self.services.warp is null) {
            return;
        }
        try {
            self.services.warp(x, y);
        }
        catch (Throwable e) {
            self.store(e);
        }
    }

    private static extern(C) char* clipboardGetCallback(void* data) {
        auto self = cast(Context) data;
        if (self is null || self.services.clipboardGet is null) {
            return null;
        }
        try {
            return cstrDup(self.services.clipboardGet());
        }
        catch (Throwable e) {
            self.store(e);
            return null;
        }
    }

    private static extern(C) void clipboardSetCallback(void* data, const(char)* text) {
        auto self = cast(Context) data;
        if (self is null || self.services.clipboardSet is null) {
            return;
        }
        try {
            self.services.clipboardSet(cslice(text).idup);
        }
        catch (Throwable e) {
            self.store(e);
        }
    }

    private static extern(C) void scheduleCallback(void* data, int ms, int timer) {
        auto self = cast(Context) data;
        if (self is null || self.services.schedule is null) {
            return;
        }
        try {
            self.services.schedule(ms, timer);
        }
        catch (Throwable e) {
            self.store(e);
        }
    }

    private static extern(C) void emitCallback(
        void* data, BKNode* node, int kind, void* ref_, double a, double b) {
        auto self = cast(Context) data;
        if (self is null) {
            return;
        }
        try {
            if (kind == BK_EV_CALL && ref_ !is null) {
                (cast(CallbackBox) ref_).call();
                return;
            }
            if (node !is null) {
                if (auto wrapper = BK_node_ref(node)) {
                    (cast(Node) wrapper).dispatch(kind, ref_, a, b);
                }
            }
        }
        catch (Throwable e) {
            self.store(e);
        }
    }
}

/// malloc'd UTF-8 copy for `BKHost.clipboard_get`; null for an empty string.
private char* cstrDup(string text) {
    if (text.length == 0) {
        return null;
    }
    auto mem = cast(char*) malloc(text.length + 1);
    if (mem is null) {
        return null;
    }
    memcpy(mem, text.ptr, text.length);
    mem[text.length] = '\0';
    return mem;
}
