/* A node in the kit's retained tree: layouts, widgets, views, editors.
 *
 * `new Node("button")` owns its C node; `free()` releases it (the destructor
 * does it too). Nodes attached to a parent are kept alive by the kit's
 * reference hooks, exactly like Python wrappers: dropping the D reference is
 * safe while the parent holds the node.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.node;

import bui.errors : BuiException;
import bui.image : Image;
import bui.internal.refs : ensureRefHooks;
import bui.internal.util : cslice, cstr;
import bui.kit;
import bui.menu : MenuItem, setMenuItems;
import bui.video : Video;

/// One event from the kit, delivered to a node's handlers.
struct NodeEvent {
    /// The node that received it.
    Node node;
    /// `BK_EV_*`.
    int kind;
    /// The kit's payload: a callback object, a C string for raw key events, or null.
    void* ref_;
    double a;
    double b;
    /// The key text for `BK_EV_KEY` / `BK_EV_KEYRELEASE`, else empty.
    const(char)[] text;
}

alias NodeHandler = void delegate(NodeEvent event);

/// A live view of a node's string list property (`BK_strs_*`), without copying.
struct Strs {
    Node node;
    string key;

    int length() const {
        return BK_strs_count(node.handle, cstr(key));
    }

    const(char)[] opIndex(size_t i) const {
        return cslice(BK_strs_get(node.handle, cstr(key), cast(int) i));
    }

    int opApply(scope int delegate(const(char)[] value) dg) const {
        const auto n = length;
        foreach (i; 0 .. n) {
            if (auto r = dg(opIndex(i))) {
                return r;
            }
        }
        return 0;
    }
}

/// Wrap a C node: the existing wrapper when the kit has one, else a borrowed one.
package Node wrapNode(BKNode* h) {
    if (h is null) {
        return null;
    }
    if (auto ref_ = BK_node_ref(h)) {
        return cast(Node) ref_;
    }
    return new Node(h, false);
}

/// A node in the kit's tree.
final class Node {
    /// The C handle, for calls into the raw `bui.kit` API.
    BKNode* handle;

    private {
        bool owned;
        bool freeing;
        NodeHandler[] handlers;
        Image keepImage;
        Video keepVideo;
        Object[] keepAlive_;
        const(char)[][] keepStrings_;
    }

    /// Create a node of `type` (`"frame"`, `"button"`, `"slider"`, ...).
    this(string type) {
        ensureRefHooks();
        handle = BK_node_new(cstr(type));
        if (handle is null) {
            throw new BuiException("Node", "unknown node type '" ~ type ~ "'");
        }
        owned = true;
        BK_node_set_ref(handle, cast(void*) this);
    }

    /// Wrap an existing C node. `owned` wrappers free it; borrowed ones don't.
    package this(BKNode* existing, bool owned) {
        ensureRefHooks();
        this.handle = existing;
        this.owned = owned;
        if (owned) {
            BK_node_set_ref(handle, cast(void*) this);
        }
    }

    ~this() {
        free();
    }

    /// Free the C node. A rooted tree's C-owned children are freed by the kit;
    /// wrapper-owned children are detached and live on until their wrappers go.
    void free() {
        auto h = handle;
        if (h is null || freeing) {
            return;
        }
        freeing = true;
        if (owned) {
            if (auto parent = BK_node_parent(h)) {
                BK_node_remove(parent, h);
            }
            BK_node_free(h);
        }
        handle = null;
        keepImage = null;
        keepVideo = null;
        keepAlive_ = null;
        keepStrings_ = null;
        freeing = false;
    }

    /// Keep `object` alive for as long as this node holds it (custom nodes).
    package void keepAlive(Object object) {
        if (object !is null) {
            keepAlive_ ~= object;
        }
    }

    /// Keep a C string the kit stores without copying (a canvas item's font
    /// family) alive, and return the pointer to hand to the kit. A string with
    /// the same contents reuses the retained copy.
    package const(char)* keepCString(const(char)* value) {
        if (value is null) {
            return null;
        }
        const slice = cslice(value);
        foreach (existing; keepStrings_) {
            if (existing == slice) {
                return existing.ptr;
            }
        }
        keepStrings_ ~= slice;
        return value;
    }

    /// The node's type name.
    string typeName() const {
        return cslice(BK_node_type(handle)).idup;
    }

    /* -- tree -------------------------------------------------------------- */

    /// Add `child`; returns this node for chaining.
    Node add(Node child, int index = -1) {
        if (child is null || child.handle is null) {
            throw new BuiException("Node.add", "no child");
        }
        if (BK_node_add(handle, child.handle, index) == 0) {
            throw new BuiException("Node.add", "the kit refused the child");
        }
        return this;
    }

    /// Remove `child`; the child wrapper stays valid as a detached node.
    void remove(Node child) {
        if (child !is null && child.handle !is null) {
            BK_node_remove(handle, child.handle);
        }
    }

    int childCount() const {
        return BK_node_child_count(handle);
    }

    /// The child at `i`; a C-owned child comes back as a borrowed wrapper.
    Node child(int i) const {
        return wrapNode(BK_node_child(handle, i));
    }

    /// The parent, if there is one.
    Node parent() const {
        return wrapNode(BK_node_parent(handle));
    }

    /// x, y, w, h in window pixels.
    int[4] rect() const {
        int[4] r;
        BK_node_rect(handle, r.ptr);
        return r;
    }

    /// The size the node wants (width, height) in pixels.
    int[2] natural() {
        int[2] s;
        BK_node_natural(handle, s.ptr);
        return s;
    }

    void invalidate() {
        BK_node_invalidate(handle);
    }

    void relayout() {
        BK_node_relayout(handle);
    }

    /* -- properties -------------------------------------------------------- */

    Node set(string key, double value) {
        BK_set_num(handle, cstr(key), value);
        return this;
    }

    Node set(string key, bool value) {
        BK_set_num(handle, cstr(key), value ? 1 : 0);
        return this;
    }

    Node set(string key, const(char)[] value) {
        BK_set_str(handle, cstr(key), cstr(value));
        return this;
    }

    Node set(string key, const float[4] rgba) {
        BK_set_color(handle, cstr(key), rgba.ptr);
        return this;
    }

    /// A string list property (`"items"`, `"headers"`, `"filters"`, ...).
    Node setStrs(string key, const(char)[][] values) {
        auto ptrs = new const(char)*[values.length];
        foreach (i, v; values) {
            ptrs[i] = cstr(v);
        }
        BK_set_strs(handle, cstr(key), ptrs.ptr, cast(int) values.length);
        return this;
    }

    double num(string key) const {
        return BK_get_num(handle, cstr(key));
    }

    const(char)[] str(string key) const {
        return cslice(BK_get_str(handle, cstr(key)));
    }

    bool color(string key, out float[4] rgba) const {
        return BK_get_color(handle, cstr(key), rgba.ptr) != 0;
    }

    /// A zero-copy view of a string list property.
    Strs strs(string key) const {
        return Strs(cast(Node) this, key);
    }

    /// Link number fields so Alt-dragging one edits them together.
    Node linkFields(Node[] fields) {
        auto ptrs = new BKNode*[fields.length];
        foreach (i, f; fields) {
            ptrs[i] = f.handle;
        }
        BK_link_fields(ptrs.ptr, cast(int) fields.length);
        return this;
    }

    /* -- geometry managers ------------------------------------------------- */

    Node geom(int manager) {
        BK_geom_set_manager(handle, manager);
        return this;
    }

    Node geom(string key, double value) {
        BK_geom_set(handle, cstr(key), value);
        return this;
    }

    double geom(string key) const {
        return BK_geom_get(handle, cstr(key));
    }

    /// Tk's packer: `pack("fill", 1)`.
    Node pack(string key, double value) {
        BK_geom_set_manager(handle, BK_GEOM_PACK);
        BK_geom_set(handle, cstr(key), value);
        return this;
    }

    /// Tk's grid: `grid("row", 0)`, `grid("column", 1)`, ...
    Node grid(string key, double value) {
        BK_geom_set_manager(handle, BK_GEOM_GRID);
        BK_geom_set(handle, cstr(key), value);
        return this;
    }

    /// Tk's place: `place("x", 10)`, `place("relwidth", 0.5)`.
    Node place(string key, double value) {
        BK_geom_set_manager(handle, BK_GEOM_PLACE);
        BK_geom_set(handle, cstr(key), value);
        return this;
    }

    /// A frame's grid column/row weight (`column_axis` 0 = column, 1 = row).
    Node frameGridWeight(int columnAxis, int index, double weight, int minsize = 0) {
        BK_frame_grid_weight(handle, columnAxis, index, weight, minsize);
        return this;
    }

    /* -- events ------------------------------------------------------------ */

    /// Add an event handler; all handlers see every event.
    Node on(NodeHandler handler) {
        if (handler !is null) {
            handlers ~= handler;
        }
        return this;
    }

    /// Add a handler for one `BK_EV_*` kind.
    Node on(int kind, NodeHandler handler) {
        return on(delegate (NodeEvent e) {
            if (e.kind == kind) {
                handler(e);
            }
        });
    }

    Node onClick(NodeHandler handler) {
        return on(BK_EV_CLICK, handler);
    }

    Node onChange(NodeHandler handler) {
        return on(BK_EV_CHANGE, handler);
    }

    Node onSubmit(NodeHandler handler) {
        return on(BK_EV_SUBMIT, handler);
    }

    Node onActivate(NodeHandler handler) {
        return on(BK_EV_ACTIVATE, handler);
    }

    Node onToggle(NodeHandler handler) {
        return on(BK_EV_TOGGLE, handler);
    }

    Node onBrowse(NodeHandler handler) {
        return on(BK_EV_BROWSE, handler);
    }

    /// The kit's `BKHost.emit` path: dispatch one event to this node's handlers.
    void dispatch(int kind, void* ref_, double a, double b) {
        if (handlers.length == 0) {
            return;
        }
        const(char)[] text;
        if (kind == BK_EV_KEY || kind == BK_EV_KEYRELEASE) {
            text = cslice(cast(const(char)*) ref_);
        }
        auto event = NodeEvent(this, kind, ref_, a, b, text);
        foreach (h; handlers) {
            h(event);
        }
    }

    /* -- menus ------------------------------------------------------------- */

    /// Fill a menu button; each row's `onClick` runs on `BK_EV_CALL`.
    Node setMenu(MenuItem[] items) {
        setMenuItems(handle, items);
        return this;
    }

    /* -- images and video -------------------------------------------------- */

    /// Straight RGBA pixels, copied by the kit.
    Node setImage(const(ubyte)[] rgba, int w, int h) {
        if (rgba.length < w * h * 4) {
            throw new BuiException("Node.setImage", "rgba is smaller than w * h * 4");
        }
        BK_node_set_image(handle, rgba.ptr, w, h);
        return this;
    }

    /// A decoded image; the node borrows it, so this keeps it alive.
    Node setImage(Image image) {
        keepImage = image;
        BK_node_set_imbuf(handle, image.handle);
        return this;
    }

    /// A file or URL decoded through the shared cache at the drawn size.
    Node setImageSource(string src, int fit = BK_FIT_CONTAIN) {
        BK_node_set_image_source(handle, cstr(src), fit);
        return this;
    }

    /// An animated source: 0 plays, n > 0 shows frame n - 1 (paused).
    Node setImageFrame(int frame) {
        BK_node_set_image_frame(handle, frame);
        return this;
    }

    /// A player on an `"image"` or `"video"` node; the node borrows it.
    Node setVideo(Video video, int fit = BK_FIT_CONTAIN) {
        keepVideo = video;
        BK_node_set_video(handle, video.handle, fit);
        return this;
    }

    /* -- color ramps, curves, histograms ----------------------------------- */

    int rampCount() {
        return BK_ramp_count(handle);
    }

    void rampGet(int i, out float pos, out float[4] rgba) {
        BK_ramp_get(handle, i, &pos, rgba.ptr);
    }

    void rampSet(const float[] pos, const float[] rgba) {
        if (rgba.length < pos.length * 4) {
            throw new BuiException("Node.rampSet", "rgba needs 4 values per stop");
        }
        BK_ramp_set(handle, pos.ptr, rgba.ptr, cast(int) pos.length);
    }

    int rampActive() {
        return BK_ramp_active(handle);
    }

    void rampEvaluate(float t, out float[4] rgba) {
        BK_ramp_evaluate(handle, t, rgba.ptr);
    }

    void rampSetActiveColor(const float[4] rgba) {
        BK_ramp_set_active_color(handle, rgba.ptr);
    }

    int curveCount(int channel) {
        return BK_curve_count(handle, channel);
    }

    void curveGet(int channel, int i, out float[2] xy) {
        BK_curve_get(handle, channel, i, xy.ptr);
    }

    void curveSet(int channel, const float[] xy) {
        if (xy.length % 2) {
            throw new BuiException("Node.curveSet", "xy needs x/y pairs");
        }
        BK_curve_set(handle, channel, xy.ptr, cast(int) (xy.length / 2));
    }

    float curveEvaluate(int channel, float x) {
        return BK_curve_evaluate(handle, channel, x);
    }

    void histogramSet(const float[] bins, int channels = 1) {
        BK_histogram_set(handle, bins.ptr, cast(int) bins.length, channels);
    }

    void gridviewSetColors(const float[] colors) {
        if (colors.length % 4) {
            throw new BuiException("Node.gridviewSetColors", "colors needs RGBA values");
        }
        BK_gridview_set_colors(handle, colors.ptr, cast(int) (colors.length / 4));
    }
}
