/* Display lists on a "canvas" node: Tk's canvas items with Blender's look.
 *
 * The kit copies item coordinates and text on every call, so the D arrays can
 * be temporary. An attached `Image` or `Video` is borrowed: keep the wrapper
 * alive while the item can be drawn.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.canvas;

import bui.errors : BuiException;
import bui.image : Image;
import bui.internal.util : cstr;
import bui.kit;
import bui.node : Node;
import bui.video : Video;

/// One canvas item; `type` is one of the `BK_ITEM_*` constants.
struct CanvasItem {
    int id;
    int type;
    /// x/y pairs or x/y/w/h quadruples, depending on `type`.
    const(float)[] coords;
    float[4] fill;
    float[4] outline;
    float width = 1;
    float radius;
    float size;
    float start;
    float extent = 90;
    string text;
    int anchor;
    /// Straight RGBA pixels for an image item (with `iw` x `ih`).
    const(ubyte)[] rgba;
    int iw;
    int ih;
    bool hidden;
    int textFlags;
    bool clipOn;
    float[4] clip;
    /// The font family; the canvas keeps a copy alive.
    string family;
    /// A decoded image; borrowed, so keep the wrapper alive.
    Image image;
    /// A file or URL decoded by the shared cache.
    string src;
    int fit = BK_FIT_CONTAIN;
    int frame;
    /// A player; borrowed, so keep the wrapper alive.
    Video video;

    /// A filled/outlined rectangle.
    static CanvasItem rect(int id,
                           float x0,
                           float y0,
                           float x1,
                           float y1,
                           const float[4] fill = [0, 0, 0, 0],
                           const float[4] outline = [0, 0, 0, 0]) {
        CanvasItem item;
        item.id = id;
        item.type = BK_ITEM_RECT;
        item.coords = [x0, y0, x1, y1];
        item.fill[] = fill[];
        item.outline[] = outline[];
        return item;
    }

    /// A filled/outlined oval touching the given box.
    static CanvasItem oval(int id,
                           float x0,
                           float y0,
                           float x1,
                           float y1,
                           const float[4] fill = [0, 0, 0, 0],
                           const float[4] outline = [0, 0, 0, 0]) {
        CanvasItem item;
        item.id = id;
        item.type = BK_ITEM_OVAL;
        item.coords = [x0, y0, x1, y1];
        item.fill[] = fill[];
        item.outline[] = outline[];
        return item;
    }

    /// A polyline through `points` (x/y pairs).
    static CanvasItem line(int id,
                           const float[] points,
                           const float[4] color = [1, 1, 1, 1],
                           float width = 1) {
        CanvasItem item;
        item.id = id;
        item.type = BK_ITEM_LINE;
        item.coords = points;
        item.outline[] = color[];
        item.width = width;
        return item;
    }

    /// A text label at `x`, `y`; `anchor` uses `BK_ANCHOR_*`.
    static CanvasItem label(int id,
                            float x,
                            float y,
                            string text,
                            const float[4] color = [1, 1, 1, 1],
                            int anchor = 0) {
        CanvasItem item;
        item.id = id;
        item.type = BK_ITEM_TEXT;
        item.coords = [x, y];
        item.text = text;
        item.fill[] = color[];
        item.anchor = anchor;
        return item;
    }

    package BKCanvasItem toC(Node canvas) {
        BKCanvasItem c;
        c.id = id;
        c.type = type;
        c.coords = coords.ptr;
        c.ncoords = cast(int) coords.length;
        c.fill[] = fill[];
        c.outline[] = outline[];
        c.width = width;
        c.radius = radius;
        c.size = size;
        c.start = start;
        c.extent = extent;
        c.text = text.length ? cstr(text) : null;
        c.anchor = anchor;
        c.rgba = rgba.ptr;
        c.iw = iw;
        c.ih = ih;
        c.hidden = hidden ? 1 : 0;
        c.text_flags = textFlags;
        c.clip_on = clipOn ? 1 : 0;
        c.clip[] = clip[];
        c.family = family.length ? canvas.keepCString(cstr(family)) : null;
        c.imbuf = image is null ? null : image.handle;
        c.src = src.length ? cstr(src) : null;
        c.fit = fit;
        c.frame = frame;
        c.video = video is null ? null : video.handle;
        return c;
    }
}

/// Insert or replace item `item.id`.
void setItem(Node canvas, CanvasItem item) {
    auto node = need(canvas, "setItem");
    auto c = item.toC(canvas);
    BK_canvas_set_item(node, &c);
}

/// Append an item, rebuilding a whole display list.
void addItem(Node canvas, CanvasItem item) {
    auto node = need(canvas, "addItem");
    auto c = item.toC(canvas);
    BK_canvas_add_item(node, &c);
}

/// Insert at `index`, or append when `index` is past the end.
void putItem(Node canvas, int index, CanvasItem item) {
    auto node = need(canvas, "putItem");
    auto c = item.toC(canvas);
    BK_canvas_put_item(node, index, &c);
}

/// Drop items after the first `count`.
void truncate(Node canvas, int count) {
    BK_canvas_truncate(need(canvas, "truncate"), count);
}

/// Delete item by id (a negative id deletes every item).
void deleteItem(Node canvas, int id) {
    BK_canvas_delete(need(canvas, "deleteItem"), id);
}

/// Raise or lower an item.
void raiseItem(Node canvas, int id, bool toTop = true) {
    BK_canvas_raise(need(canvas, "raiseItem"), id, toTop ? 1 : 0);
}

/// Number of items.
int itemCount(Node canvas) {
    return BK_canvas_count(need(canvas, "itemCount"));
}

private BKNode* need(Node canvas, string operation) {
    if (canvas is null || canvas.handle is null) {
        throw new BuiException("Canvas." ~ operation, "no canvas node");
    }
    return canvas.handle;
}
