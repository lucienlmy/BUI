"""Data views (C: ``kit/intern/kit_views.c``): UI list, tree view, grid view;
and Blender's File Browser (C: ``kit/intern/kit_filebrowser.c``).

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

import os
from typing import Callable, Optional, Sequence

import buipy

from bui.components.core import (EV_ACTIVATE, EV_CHANGE, EV_RENAME, EV_SUBMIT, EV_TOGGLE,
                                 Component, ValueComponent)


class ListView(ValueComponent):
    """Blender UI list: wheel scrolls, Ctrl+Wheel / Up / Down select,
    double-click or F2 renames (``on_rename(index, name)``). Value = index."""

    TYPE = "list"

    def __init__(self, items: Sequence[str] = (), value: int = 0, *, rows: int = 5,
                 on_rename: Optional[Callable[[int, str], None]] = None, icons=None, **kw):
        Component.__init__(self, **{k: v for k, v in kw.items()
                                    if k in ("description", "enabled", "visible", "weight", "fill")})
        self.on_change = kw.get("on_change")
        self.on_rename = on_rename
        self.set("rows", rows)
        self.items = items
        if icons:
            self.set("icons", list(icons))
        self.set("value", value)

    @property
    def items(self):
        return self.strs("items")

    @items.setter
    def items(self, items) -> None:
        self.set("items", [str(i) for i in items])

    def _load(self):
        return int(self.get("value"))

    @property
    def scroll(self) -> int:
        return int(self.get("scroll"))

    @property
    def editing(self) -> bool:
        return bool(self.get("editing"))

    def _on_event(self, kind, ref, a, b):
        if kind == EV_RENAME and self.on_rename is not None:
            self.on_rename(int(a), self.items[int(a)])
        else:
            super()._on_event(kind, ref, a, b)


# Outliner toggle columns: (off, on) icons.
TOGGLE_ICONS = {
    "hide": ("HIDE_OFF", "HIDE_ON"),
    "select": ("RESTRICT_SELECT_OFF", "RESTRICT_SELECT_ON"),
    "render": ("RESTRICT_RENDER_OFF", "RESTRICT_RENDER_ON"),
    "lock": ("UNLOCKED", "LOCKED"),
}


class TreeItem(buipy.KitTreeItem):
    """Tree view item: ``TreeItem("Cube", child, ..., icon=, expanded=,
    toggles={"hide": False})``."""

    def __init__(self, label: str, *children: "TreeItem", icon: str = None,
                 expanded: bool = True, data=None, toggles: dict = None):
        super().__init__(label, icon)
        self.data = data
        self.toggles = dict(toggles or {})
        self.set("expanded", 1.0 if expanded else 0.0)
        self.add(*children)

    def add(self, *items: "TreeItem"):
        for c in items:
            buipy.KitTreeItem.add(self, c)
        return items[0] if len(items) == 1 else self

    @property
    def expanded(self) -> bool:
        return bool(self.get("expanded"))

    @expanded.setter
    def expanded(self, v: bool) -> None:
        self.set("expanded", 1.0 if v else 0.0)

    def __repr__(self):
        return "TreeItem(%r)" % self.label


def _walk(items):
    for it in items:
        yield it
        yield from _walk(it.children)


class TreeView(ValueComponent):
    """Hierarchical view (outliner): arrows expand (Shift: recursive),
    Left/Right collapse/expand, double-click/F2 renames, toggle columns
    (``columns=("hide", "render")``) clicked or dragged down. Value = the
    selected TreeItem."""

    TYPE = "tree"

    def __init__(self, roots: Sequence[TreeItem] = (), *, rows: int = 8,
                 columns: Sequence[str] = (),
                 on_toggle: Optional[Callable[[TreeItem, str, bool], None]] = None,
                 on_rename: Optional[Callable[[TreeItem, str], None]] = None, **kw):
        Component.__init__(self, **{k: v for k, v in kw.items()
                                    if k in ("description", "enabled", "visible", "weight", "fill")})
        self.on_change = kw.get("on_change")
        self.on_toggle, self.on_rename = on_toggle, on_rename
        self.columns = list(columns)
        self.set("rows", rows)
        icons = []
        for c in self.columns:
            icons.extend(TOGGLE_ICONS.get(c, ("CHECKMARK", "X")))
        if icons:
            self.set("icons", icons)
        self.roots = roots

    @property
    def roots(self):
        return list(self._roots)

    @roots.setter
    def roots(self, roots) -> None:
        self._roots = list(roots)
        for it in _walk(self._roots):        # push toggle states to C
            for k, name in enumerate(self.columns):
                it.set("toggle%d" % k, 1.0 if it.toggles.get(name) else 0.0)
        self.set_tree_roots(self._roots)

    def _load(self):
        return self.tree_selected()

    def _store(self, v, notify):
        self.tree_select(v)

    def _set_default(self, v):
        pass

    def row_count(self) -> int:
        return len(self.tree_rows())

    def rows_info(self):
        """[(item, (x, y, w, h))] of the visible rows."""
        return self.tree_rows()

    def _on_event(self, kind, ref, a, b):
        if kind == EV_TOGGLE:
            name = self.columns[int(a)] if int(a) < len(self.columns) else str(int(a))
            ref.toggles[name] = bool(b)
            if self.on_toggle is not None:
                self.on_toggle(ref, name, bool(b))
        elif kind == EV_RENAME:
            item = self.tree_rows()[int(a)][0]
            if self.on_rename is not None:
                self.on_rename(item, item.label)
        elif kind == EV_CHANGE and self.on_change is not None:
            self.on_change(ref)


class GridView(ValueComponent):
    """Preview tiles: ``items`` are ``(label, preview)`` where preview is an
    icon name or an RGBA color. Double-click / Return: ``on_activate(i)``."""

    TYPE = "gridview"

    def __init__(self, items: Sequence = (), value: int = 0, *, tile: float = 4.0,
                 rows: float = 2.0, on_activate: Optional[Callable[[int], None]] = None,
                 **kw):
        Component.__init__(self, **{k: v for k, v in kw.items()
                                    if k in ("description", "enabled", "visible", "weight", "fill")})
        self.on_change = kw.get("on_change")
        self.on_activate = on_activate
        self.set("tile", tile)
        self.set("height", rows)
        self.items = items
        self.set("value", value)

    @property
    def items(self):
        return list(self._items)

    @items.setter
    def items(self, items) -> None:
        self._items = [i if isinstance(i, tuple) else (str(i), None) for i in items]
        self.set("items", [label for label, _ in self._items])
        self.set("icons", [p if isinstance(p, str) else "" for _, p in self._items])
        self.set_grid_colors([p if isinstance(p, tuple) else (0, 0, 0, 0)
                              for _, p in self._items])

    def _load(self):
        return int(self.get("value"))

    def tile_rect(self, i: int):
        """Tile rect (mirrors the C layout; for tests and tools)."""
        u = buipy.kit_unit()
        t = round(self.get("tile") * u)
        p = round(0.4 * u)
        cols = max(1, (self.w - p) // (t + p))
        gap = (self.w - cols * t) / (cols + 1)
        r, c = divmod(i, cols)
        return (round(self.x + gap + c * (t + gap)), self.y + p + r * (t + p), t, t)

    def _on_event(self, kind, ref, a, b):
        if kind == EV_ACTIVATE and self.on_activate is not None:
            self.on_activate(int(a))
        else:
            super()._on_event(kind, ref, a, b)


class FileBrowser(Component):
    """Blender's File Browser (editors/space_file) in a layout: bookmark
    panels, the directory-path bar, the file list (click a header to sort) and
    the execute bar. It fills the rect it gets.

        FileBrowser("~/renders", mode="save", file="out.png",
                    filters=["Images\t*.png *.jpg"], on_submit=save)

    ``mode`` is "open", "save" or "directory"; ``filters`` are "Label\\tpattern
    ..." entries (``bui.filebrowser.tk_filters`` converts tkinter's). Browsing
    reads the real file system. ``on_change(directory)`` follows folder
    changes; ``on_submit(paths)`` gets the chosen paths, ``[]`` when cancelled
    (Escape or the Cancel button)."""

    TYPE = "filebrowser"
    MODES = ("open", "save", "directory")

    def __init__(self, directory: str = "", *, file: str = "", mode: str = "open",
                 multiple: bool = False, filters: Sequence[str] = (), button: str = "",
                 on_change: Optional[Callable[[str], None]] = None,
                 on_submit: Optional[Callable[[list], None]] = None, **kw):
        if mode not in self.MODES:
            raise ValueError("FileBrowser mode must be one of %s, not %r" % (self.MODES, mode))
        super().__init__(**kw)
        self.on_change, self.on_submit = on_change, on_submit
        self.set("mode", self.MODES.index(mode))
        if multiple:
            self.set("multiple", 1)
        if filters:
            self.set("filters", [str(f) for f in filters])
        if button:
            self.set("button", button)
        if directory:
            self.set("dir", os.path.expanduser(os.fspath(directory)))
        if file:
            self.set("file", os.fspath(file))

    @property
    def directory(self) -> str:
        return self.get_str("dir")

    @directory.setter
    def directory(self, path: str) -> None:
        path = os.path.expanduser(os.fspath(path))
        if path != self.directory:  # navigating always reports a change
            self.set("dir", path)

    @property
    def file(self) -> str:
        """The file name in the execute bar (setting a path also moves the folder)."""
        return self.get_str("file")

    @file.setter
    def file(self, name: str) -> None:
        self.set("file", os.fspath(name))

    @property
    def hidden(self) -> bool:
        return bool(self.get("hidden"))

    @hidden.setter
    def hidden(self, show: bool) -> None:
        self.set("hidden", int(bool(show)))

    @property
    def state(self) -> int:
        """0 browsing, 1 chosen, -1 cancelled."""
        return int(self.get("state"))

    @property
    def result(self) -> list:
        """The chosen paths (empty until chosen)."""
        return list(self.strs("result"))

    def execute(self) -> None:
        """Press the execute button (Open / Save / Select Folder)."""
        self.set("execute", 1)

    def cancel(self) -> None:
        self.set("cancel", 1)

    def refresh(self) -> None:
        """Read the folder again."""
        self.set("refresh", 1)

    def _on_event(self, kind, ref, a, b):
        if kind == EV_CHANGE and self.on_change is not None:
            self.on_change(self.directory)
        elif kind == EV_SUBMIT and self.on_submit is not None:
            self.on_submit(self.result if a > 0 else [])
