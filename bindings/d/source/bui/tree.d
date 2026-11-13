/* Tree-view items and the "tree" node operations.
 *
 * Items are reference-counted by the kit, exactly like nodes: adding a child
 * or making an item a root roots its D wrapper. `TreeItem.free` therefore does
 * nothing while the item is attached; clear the parent (or replace the tree's
 * roots) first.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.tree;

import bui.errors : BuiException;
import bui.internal.refs : ensureRefHooks, refCount;
import bui.internal.util : cslice, cstr;
import bui.kit;
import bui.node : Node;
import std.conv : to;

/// One row of a tree view.
final class TreeItem {
    /// The C handle, for calls into the raw `bui.kit` API.
    BKTreeItem* handle;

    private {
        bool owned;
        TreeItem parent_;
    }

    /// A row with `label` and an icon id (`BK_icon_id`).
    this(string label, int icon = 0) {
        ensureRefHooks();
        handle = BK_tree_item_new(cstr(label), icon);
        if (handle is null) {
            throw new BuiException("TreeItem", "allocation failed");
        }
        owned = true;
        BK_tree_item_set_ref(handle, cast(void*) this);
    }

    package this(BKTreeItem* existing, bool owned) {
        handle = existing;
        this.owned = owned;
        if (owned) {
            BK_tree_item_set_ref(handle, cast(void*) this);
        }
    }

    ~this() {
        free();
    }

    /// Free an item no parent or tree holds; replace the tree's roots first.
    void free() {
        if (handle is null || !owned || parent_ !is null || refCount(cast(void*) this) != 0) {
            return;
        }
        const n = BK_tree_item_child_count(handle);
        foreach (i; 0 .. n) {
            if (auto child = wrapTreeItem(BK_tree_item_child(handle, i))) {
                child.parent_ = null;
            }
        }
        BK_tree_item_set_ref(handle, null);
        BK_tree_item_free(handle);
        handle = null;
    }

    /// The parent row, when this item was added through `add`.
    TreeItem parent() {
        return parent_;
    }

    /// Add `child`; returns it for chaining.
    TreeItem add(TreeItem child) {
        if (child is null || child.handle is null) {
            throw new BuiException("TreeItem.add", "no child");
        }
        if (child.parent_ !is null) {
            throw new BuiException("TreeItem.add", "the child already has a parent");
        }
        for (auto up = this; up !is null; up = up.parent_) {
            if (up is child) {
                throw new BuiException("TreeItem.add", "an item cannot contain itself");
            }
        }
        BK_tree_item_add(handle, child.handle);
        child.parent_ = this;
        return child;
    }

    /// Drop every child; their wrappers stay valid and detached.
    void clear() {
        const n = BK_tree_item_child_count(handle);
        foreach (i; 0 .. n) {
            if (auto child = wrapTreeItem(BK_tree_item_child(handle, i))) {
                child.parent_ = null;
            }
        }
        BK_tree_item_clear(handle);
    }

    /// Set an item flag: "expanded", "marked", "icon", "toggle0".."toggle31".
    TreeItem set(string key, double value) {
        BK_tree_item_set(handle, cstr(key), value);
        return this;
    }

    /// Read an item flag.
    double get(string key) const {
        return BK_tree_item_get(handle, cstr(key));
    }

    /// The row text.
    string label() const {
        return cslice(BK_tree_item_label(handle)).idup;
    }

    /// Replace the row text.
    void label(string text) {
        BK_tree_item_set_label(handle, cstr(text));
    }

    /// The icon id (`BK_icon_id`).
    int icon() const {
        return cast(int) get("icon");
    }

    /// Replace the icon.
    void icon(int value) {
        set("icon", value);
    }

    /// Non-zero while the row is expanded.
    bool expanded() const {
        return get("expanded") != 0;
    }

    /// Expand or collapse the row.
    void expanded(bool on) {
        set("expanded", on ? 1 : 0);
    }

    /// Non-zero while the row is marked.
    bool marked() const {
        return get("marked") != 0;
    }

    /// Mark the row.
    void marked(bool on) {
        set("marked", on ? 1 : 0);
    }

    /// A toggle-column bit.
    bool toggle(int bit) const {
        return get("toggle" ~ bit.to!string) != 0;
    }

    /// Set a toggle-column bit.
    void toggle(int bit, bool on) {
        set("toggle" ~ bit.to!string, on ? 1 : 0);
    }

    /// Column values for a multi-column tree.
    void setValues(const(char)[][] values) {
        auto ptrs = new const(char)*[values.length];
        foreach (i, value; values) {
            ptrs[i] = cstr(value);
        }
        BK_tree_item_set_values(handle, ptrs.ptr, cast(int) values.length);
    }

    int childCount() const {
        return BK_tree_item_child_count(handle);
    }

    /// Child `i`; a borrowed wrapper when the kit has no D one.
    TreeItem child(int i) const {
        return wrapTreeItem(BK_tree_item_child(handle, i));
    }
}

/// Wrap a C item: the existing wrapper when there is one, else a borrowed one.
package TreeItem wrapTreeItem(BKTreeItem* item) {
    if (item is null) {
        return null;
    }
    if (auto ref_ = BK_tree_item_ref(item)) {
        return cast(TreeItem) ref_;
    }
    return new TreeItem(item, false);
}

/// Replace a tree's root rows; the kit retains their references.
void setRoots(Node tree, TreeItem[] roots) {
    auto ptrs = new BKTreeItem*[roots.length];
    foreach (i, root; roots) {
        if (root is null || root.handle is null) {
            throw new BuiException("Tree.setRoots", "null root");
        }
        ptrs[i] = root.handle;
    }
    BK_tree_set_roots(handle(tree, "setRoots"), ptrs.ptr, cast(int) roots.length);
    foreach (root; roots) {
        root.parent_ = null;
    }
}

/// The selected row, if any.
TreeItem selected(Node tree) {
    return wrapTreeItem(BK_tree_selected(handle(tree, "selected")));
}

/// Select a row; null clears the selection.
void select(Node tree, TreeItem item) {
    BK_tree_select(handle(tree, "select"), item is null ? null : item.handle);
}

/// Number of visible rows.
int rowCount(Node tree) {
    return BK_tree_row_count(handle(tree, "rowCount"));
}

/// The item shown at `row`.
TreeItem rowItem(Node tree, int row) {
    return wrapTreeItem(BK_tree_row(handle(tree, "row"), row));
}

/// Rect (x, y, w, h) of `row`.
int[4] rowRect(Node tree, int row) {
    int[4] rect;
    BK_tree_row_rect(handle(tree, "rowRect"), row, rect.ptr);
    return rect;
}

private BKNode* handle(Node tree, string operation) {
    if (tree is null || tree.handle is null) {
        throw new BuiException("Tree." ~ operation, "no tree node");
    }
    return tree.handle;
}
