"""Layouts and containers (C node types in ``kit/intern/kit_widgets.c``).

* :class:`Column` / :class:`Row` — Blender's uiLayout column/row;
  ``align=True`` joins children into one block. Row children share the
  width by ``weight`` (``weight=0`` keeps the natural width).
* :class:`Split` (property split), :class:`Grid`, :class:`Box`.
* :class:`Panel` — collapsible (Ctrl+click solo, drag over headers,
  drag the grip to reorder, header components, sub-panels).
* :class:`Tabs`, :class:`SideTabs` (properties-editor icon tabs),
  :class:`ScrollView`, :class:`VectorField`.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

from typing import Callable, Optional, Sequence

import buipy

from bui.components.core import EV_CHANGE, Component
from bui.components.widgets import Label, NumberField


class _Stack(Component):
    def __init__(self, *children: Component, align: bool = False, spacing=None,
                 padding=0, **kw):
        super().__init__(**kw)
        if align:
            self.set("align", True)
        if spacing is not None:
            self.set("spacing", int(spacing))
        if padding:
            self.set("padding", int(padding))
        self.add(*children)


class Column(_Stack):
    TYPE = "column"


class Row(_Stack):
    TYPE = "row"


class Split(Row):
    """Property split: ``left`` gets ``factor`` of the width (Blender's
    ``layout.split``: the columns ``columnspace`` apart)."""

    TYPE = "split"

    def __init__(self, left: Component, right: Component, factor: float = 0.4, **kw):
        left.weight, right.weight = factor, 1.0 - factor
        super().__init__(left, right, **kw)


class Box(_Stack):
    """A padded column on Blender's box background."""

    TYPE = "box"


class Grid(_Stack):
    """Grid flow: children in ``columns`` equal-width columns."""

    TYPE = "grid"

    def __init__(self, *children: Component, columns: int = 2, **kw):
        super().__init__(*children, **kw)
        self.set("columns", columns)


class Panel(Component):
    """Collapsible panel. ``header=[Checkbox("", True)]`` puts components in
    the header; ``icon=`` shows an icon; a Panel in a Panel is a sub-panel."""

    TYPE = "panel"

    def __init__(self, title: str, *children: Component, collapsed: bool = False,
                 icon: str = None, header: Sequence[Component] = (),
                 on_toggle: Optional[Callable[[bool], None]] = None, category: str = None, **kw):
        super().__init__(**kw)
        self.on_toggle = on_toggle
        self.set("text", title)
        if category:  # in a Sidebar: the tab it shows under
            self.set("category", category)
        if icon:
            self.set("icon", icon)
        if collapsed:
            self.set("collapsed", True)
        for h in header:
            buipy.KitNode.add(self, h)
        body = self.body
        for c in children:
            body.add(c)

    @property
    def body(self) -> Component:
        return self.children[0]

    def _content(self) -> Component:
        return self.body

    @property
    def title(self) -> str:
        return self.get_str("text")

    @property
    def collapsed(self) -> bool:
        return bool(self.get("collapsed"))

    @collapsed.setter
    def collapsed(self, v: bool) -> None:
        self.set("collapsed", bool(v))

    def toggle(self) -> None:
        self.collapsed = not self.collapsed

    def _on_event(self, kind, ref, a, b):
        if kind == EV_CHANGE and self.on_toggle is not None:
            self.on_toggle(bool(a))


class Tabs(Component):
    """Tabbed pages: ``Tabs([("Object", page), ("Modifiers", page2)])``."""

    TYPE = "tabs"

    def __init__(self, pages: Sequence, current: int = 0,
                 on_change: Optional[Callable[[int], None]] = None, **kw):
        super().__init__(**kw)
        self.on_change = on_change
        self.set("items", [str(t) for t, _ in pages])
        self.add(*[c for _, c in pages])
        self.set("current", current)

    @property
    def current(self) -> int:
        return int(self.get("current"))

    def select(self, i: int) -> None:
        if i != self.current and 0 <= i < len(self.children):
            self.set("current", i)
            if self.on_change is not None:
                self.on_change(i)

    @property
    def titles(self):
        return self.strs("items")

    def _on_event(self, kind, ref, a, b):
        if kind == EV_CHANGE and self.on_change is not None:
            self.on_change(int(a))


class Sidebar(Component):
    """An editor's sidebar region (Blender's N-panel): Panels with a
    ``category`` show under a tab per category, drawn along the right edge
    with rotated labels; Ctrl+Wheel cycles them. ``current`` is the active
    category's index."""

    TYPE = "sidebar"

    def __init__(self, *panels: Component, current: int = 0, on_change=None, **kw):
        super().__init__(**kw)
        self.on_change = on_change
        self.add(*panels)
        if current:
            self.set("value", current)

    @property
    def current(self) -> int:
        return int(self.get("value"))

    @current.setter
    def current(self, i: int) -> None:
        self.set("value", i)

    def _on_event(self, kind, ref, a, b):
        if kind == EV_CHANGE and self.on_change is not None:
            self.on_change(int(a))


class SideTabs(Tabs):
    """Properties-editor navigation: ``SideTabs([("OBJECT_DATA", "Object",
    page), ...])`` — icon tabs on the left; hover for the name. ``None``
    between pages starts a new tab group, as in Blender's navigation bar."""

    TYPE = "sidetabs"

    def __init__(self, pages: Sequence, current: int = 0, on_change=None, **kw):
        Component.__init__(self, **kw)
        self.on_change = on_change
        starts, entries = set(), []
        for p in pages:
            if p is None:
                starts.add(len(entries))
            else:
                entries.append(p)
        self.set("items", [str(t) for _, t, _ in entries])
        self.set("icons", [str(i) for i, _, _ in entries])
        self.set("groups", ["1" if i in starts else "0" for i in range(len(entries))])
        self.add(*[c for _, _, c in entries])
        self.set("current", current)

    def page(self) -> Component:
        return self.children[self.current]


class ScrollView(Component):
    """Fixed-height viewport over a taller child (wheel, draggable bar)."""

    TYPE = "scroll"

    def __init__(self, child: Component, height: int = None, rows: float = 8, **kw):
        super().__init__(**kw)
        if height:
            self.set("height", height / buipy.kit_unit())
        else:
            self.set("rows", int(rows))
        self.add(child)

    @property
    def scroll(self) -> int:
        return int(self.get("scroll"))


class VectorField(Column):
    """Label + aligned number fields (X/Y/Z). Alt+drag edits all components;
    Ctrl+Alt+C / Ctrl+Alt+V copy/paste the whole vector."""

    def __init__(self, label: str = "", value=(0.0, 0.0, 0.0), *, axes: str = "XYZW",
                 on_change: Optional[Callable] = None, **number_kw):
        self.on_change = on_change
        self.fields = [NumberField(axes[i] if i < len(axes) else str(i), v,
                                   on_change=self._field_changed, **number_kw)
                       for i, v in enumerate(value)]
        block = Column(*self.fields, align=True)
        children = ([Label(label)] if label else []) + [block]
        super().__init__(*children, spacing=max(1, round(0.1 * buipy.kit_unit())))
        buipy.kit_link_fields(self.fields)

    @property
    def value(self):
        return tuple(f.value for f in self.fields)

    def set_value(self, value, notify: bool = True) -> None:
        for f, v in zip(self.fields, value):
            f.set_value(v, notify=False)
        if notify and self.on_change is not None:
            self.on_change(self.value)

    def _field_changed(self, _v):
        if self.on_change is not None:
            self.on_change(self.value)


# Blender's editor-type menu (ED_area_type_hud / space_type_menu): groups of (name, icon).
EDITOR_TYPES = [
    [("3D Viewport", "VIEW3D"), ("Image Editor", "IMAGE"), ("UV Editor", "UV"),
     ("Compositor", "NODE_COMPOSITING"), ("Texture Node Editor", "NODE_TEXTURE"),
     ("Geometry Node Editor", "GEOMETRY_NODES"), ("Shader Editor", "NODE_MATERIAL"),
     ("Video Sequencer", "SEQUENCE"), ("Movie Clip Editor", "TRACKER")],
    [("Dope Sheet", "ACTION"), ("Timeline", "TIME"), ("Graph Editor", "GRAPH"),
     ("Drivers", "DRIVER"), ("Nonlinear Animation", "NLA")],
    [("Text Editor", "TEXT"), ("Python Console", "CONSOLE"), ("Info", "INFO")],
    [("Outliner", "OUTLINER"), ("Properties", "PROPERTIES"), ("File Browser", "FILEBROWSER"),
     ("Asset Browser", "ASSET_MANAGER"), ("Spreadsheet", "SPREADSHEET"),
     ("Preferences", "PREFERENCES")],
]


class AreaHeader(Component):
    """An editor's header region (``ED_region_header_layout``): 1.3 units tall,
    items left to right from the corner offset; a ``Spacer()`` pushes the rest
    to the right (``layout.separator_spacer()``)."""

    TYPE = "areaheader"

    def __init__(self, *children: Component, **kw):
        super().__init__(**kw)
        for c in children:
            c.weight = 0 if not isinstance(c, _spacer_type()) else c.weight
        self.add(*children)


def _spacer_type():
    from .widgets import Spacer
    return Spacer


class Area(Column):
    """A Blender area: the header with the editor-type menu, then the editor.

        Area("Geometry Node Editor", NodeEditor(...), header=[MenuButton("View", [...])],
             on_editor=lambda name: ...)
    """

    def __init__(self, editor: str, body: Component, *, header: Sequence[Component] = (),
                 on_editor: Optional[Callable[[str], None]] = None, **kw):
        from .menus import MenuButton, MenuItem
        self.editor, self.on_editor = editor, on_editor
        icon = next((i for group in EDITOR_TYPES for n, i in group if n == editor), "QUESTION")
        items = []
        for g, group in enumerate(EDITOR_TYPES):
            if g:
                items.append(None)
            for name, ic in group:
                items.append(MenuItem(name, (lambda n=name: self._pick(n)), icon=ic,
                                      checked=None))
        self.editor_button = MenuButton("", items, icon=icon, description="Editor Type")
        self.header = AreaHeader(self.editor_button, *header)
        body.set("fill", True)
        kw.setdefault("fill", True)  # an area fills its place in the screen
        super().__init__(self.header, body, spacing=0, **kw)
        self.body = body

    def _pick(self, name: str) -> None:
        self.editor = name
        icon = next((i for group in EDITOR_TYPES for n, i in group if n == name), "QUESTION")
        self.editor_button.set("icon", icon)
        if self.on_editor is not None:
            self.on_editor(name)
