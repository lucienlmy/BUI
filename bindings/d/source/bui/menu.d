/* Menu descriptions for `Context.openMenu`, `Context.openPie` and menu
 * buttons. The kit deep-copies a description, so the D arrays are temporary;
 * callbacks are retained by the kit through the reference hooks.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.menu;

import bui.errors : BuiException;
import bui.internal.callback : CallbackBox;
import bui.internal.util : cstr;
import bui.kit;

/// One row of a menu. `text == "-"` is a separator.
final class MenuItem {
    string text;
    int icon;
    string shortcut;
    /// -1 no check box, 0 off, 1 on.
    int checked = -1;
    bool enabled = true;
    void delegate() onClick;
    MenuItem[] sub;

    this(string text, void delegate() onClick = null) {
        this.text = text;
        this.onClick = onClick;
    }

    /// A separator row.
    static MenuItem separator() {
        return new MenuItem("-");
    }

    /// A check-box row.
    static MenuItem check(string text, bool on, void delegate() onClick = null) {
        auto item = new MenuItem(text, onClick);
        item.checked = on ? 1 : 0;
        return item;
    }

    /// A submenu row.
    static MenuItem submenu(string text, MenuItem[] children) {
        auto item = new MenuItem(text);
        item.sub = children;
        return item;
    }

    MenuItem shortcut_(string keys) {
        shortcut = keys;
        return this;
    }

    MenuItem icon_(int id) {
        icon = id;
        return this;
    }

    MenuItem disabled() {
        enabled = false;
        return this;
    }
}

/// A description tree that keeps every C array and callback alive for one call.
private final class SpecTree {
    BKMenuSpec[] specs;
    SpecTree[] subs;
    CallbackBox[] boxes;
}

private SpecTree buildTree(MenuItem[] items) {
    auto tree = new SpecTree();
    tree.specs = new BKMenuSpec[items.length];
    tree.subs = new SpecTree[items.length];

    foreach (i, item; items) {
        auto spec = &tree.specs[i];
        spec.text = cstr(item.text);
        spec.icon = item.icon;
        spec.shortcut = item.shortcut.length ? cstr(item.shortcut) : null;
        spec.checked = item.checked;
        spec.enabled = item.enabled ? 1 : 0;

        if (item.onClick !is null) {
            auto box = new CallbackBox(item.onClick);
            tree.boxes ~= box;
            spec.ref_ = cast(void*) box;
        }
        if (item.sub.length) {
            auto sub = buildTree(item.sub);
            tree.subs[i] = sub;
            spec.sub = sub.specs.ptr;
            spec.nsub = cast(int) sub.specs.length;
        }
    }
    return tree;
}

/// The kit copies the description and retains its callbacks.
package void setMenuItems(BKNode* button, MenuItem[] items) {
    auto tree = buildTree(items);
    BK_menubutton_set_items(button, tree.specs.ptr, cast(int) tree.specs.length);
}

/// Open a context menu at `x`, `y` in the owner's window.
void openMenu(
    BKContext* ctx, BKNode* owner, MenuItem[] items, string title = null, int x = 0, int y = 0) {
    if (ctx is null) {
        throw new BuiException("openMenu", "no context");
    }
    auto tree = buildTree(items);
    BK_open_menu(ctx, owner, tree.specs.ptr, cast(int) tree.specs.length,
                 title.length ? cstr(title) : null, x, y);
}

/// The pie center is the pointer when `openPie` gets this for x or y.
enum pieAtPointer = -100000;

/// Open a pie menu (at most eight rows are reachable), centered at window pixels x, y.
void openPie(BKContext* ctx,
             MenuItem[] items,
             string title = null,
             int x = pieAtPointer,
             int y = pieAtPointer) {
    if (ctx is null) {
        throw new BuiException("openPie", "no context");
    }
    auto tree = buildTree(items);
    BK_open_pie(ctx, tree.specs.ptr, cast(int) tree.specs.length,
                title.length ? cstr(title) : null, x, y);
}
