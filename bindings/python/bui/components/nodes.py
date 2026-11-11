"""The node editor: Blender's node canvas (space_node), drawn and driven in C.

    a = Node("Value", outputs=[("Value", "float")], location=(-200, 0), kind="input")
    b = Node("Math", inputs=[("Value", "float", 0.5), ("Value", "float", 0.5)],
             outputs=[("Value", "float")], location=(0, 0), kind="converter")
    NodeEditor([a, b], links=[(a, 0, b, 0)], fill=True)

Interactions follow Blender:
- LMB on a node selects it (Shift toggles), and dragging moves the selection (Ctrl snaps).
- LMB drag on empty space box-selects; a click there deselects.
- LMB drag from a socket makes a link. Dragging from a linked input picks its link up.
- Ctrl+RMB drag cuts links; MMB pans (Ctrl+MMB zooms); the wheel zooms (Ctrl/Shift+wheel pans).
- X / Delete deletes the selection, A / Alt+A selects / deselects all, H collapses, Home frames all.
- Drag an unlinked input's field to change it.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

from typing import Callable, Optional, Sequence

from .core import EV_CHANGE, Component

# node class header colors (userdef_default_theme.c: space_node)
NODE_CLASS_COLORS = {
    "input": "#83314a", "output": "#3e232a", "shader": "#2b652b", "texture": "#79461d",
    "color": "#6e6e23", "vector": "#3c3c83", "converter": "#246283", "geometry": "#1d725e",
    "attribute": "#1d2546", "group": "#3b5959", "filter": "#412b51", "script": "#203c3c",
    "matte": "#5a3838", "distort": "#3e5a5b", "layout": "#6c696f", "pattern": "#4f4f4f",
}

EV_MOVED, EV_SELECT, EV_VALUE, EV_COLLAPSE, EV_LINKS, EV_DELETE, EV_ORDER = 1, 2, 3, 4, 10, 11, 12


def _rgba(color) -> tuple:
    if isinstance(color, str):
        h = color.lstrip("#")
        return tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)) + (1.0,)
    c = tuple(float(v) for v in color)
    return c if len(c) == 4 else c + (1.0,)


class Node(Component):
    """One node. ``inputs`` are ``(name, type)`` or ``(name, type, value)`` (a
    value draws a field while the input is unlinked); ``outputs`` are
    ``(name, type)``. Types color the sockets: float, int, bool, vector, color,
    shader, geometry, string, object, image, collection, texture, material,
    rotation, menu, matrix. ``kind`` picks the header color (see
    NODE_CLASS_COLORS) or pass ``color=``."""

    TYPE = "graphnode"

    def __init__(self, title: str = "Node", *, inputs: Sequence = (), outputs: Sequence = (),
                 location=(0.0, 0.0), width: float = 140.0, kind: str = "converter",
                 color=None, collapsed: bool = False,
                 on_change: Optional[Callable[["Node", int, object], None]] = None):
        super().__init__()
        self.title = title
        self.on_change = on_change
        self._inputs = [tuple(i) for i in inputs]
        self._outputs = [tuple(o) for o in outputs]
        self.set("label", title)
        self.set("color", _rgba(color if color is not None else NODE_CLASS_COLORS.get(kind, "#4f4f4f")))
        self.location = location
        self.set("nwidth", float(width))
        if collapsed:
            self.set("collapsed", True)
        self._push_sockets()

    def _push_sockets(self) -> None:
        items = []
        for o in self._outputs:
            items.append("o|%s|%s" % (o[0], o[1] if len(o) > 1 else "float"))
        for i in self._inputs:
            value = ""
            if len(i) > 2 and i[2] is not None:
                value = str(int(i[2])) if (i[1] == "int") else "%.3f" % float(i[2])
            items.append("i|%s|%s|%s" % (i[0], i[1] if len(i) > 1 else "float", value))
        self.set("items", items)

    @property
    def location(self) -> tuple:
        return (self.get("nx"), self.get("ny"))

    @location.setter
    def location(self, xy) -> None:
        self.set("nx", float(xy[0]))
        self.set("ny", float(xy[1]))

    @property
    def selected(self) -> bool:
        return bool(self.get("selected"))

    @selected.setter
    def selected(self, v: bool) -> None:
        self.set("selected", bool(v))

    @property
    def collapsed(self) -> bool:
        return bool(self.get("collapsed"))

    @collapsed.setter
    def collapsed(self, v: bool) -> None:
        self.set("collapsed", bool(v))

    def input_value(self, index: int):
        """The current value of input ``index`` (None without a field)."""
        k = 0
        for s in self.strs("items"):
            parts = s.split("|")
            if parts[0] == "i":
                if k == index:
                    if len(parts) < 4 or parts[3] == "":
                        return None
                    return int(parts[3]) if parts[2] == "int" else float(parts[3])
                k += 1
        raise IndexError(index)

    def _on_event(self, kind, ref, a, b):
        if kind != EV_CHANGE or self.on_change is None:
            return
        what = {EV_MOVED: "location", EV_SELECT: "select", EV_VALUE: "value",
                EV_COLLAPSE: "collapsed"}.get(int(a))
        if what == "value":
            self.on_change(self, "value", (int(b), self.input_value(int(b))))
        elif what == "location":
            self.on_change(self, what, self.location)
        elif what:
            self.on_change(self, what, bool(b))


class NodeEditor(Component):
    """Blender's node editor canvas. ``links`` are ``(from_node, output,
    to_node, input)`` with nodes or their indices. ``on_links(links)`` runs
    after the user links, relinks or cuts; ``on_delete(nodes)`` when X /
    Delete asks to delete the selection (remove them to delete)."""

    TYPE = "nodeeditor"

    def __init__(self, nodes: Sequence[Node] = (), *, links: Sequence = (),
                 on_links: Optional[Callable[[list], None]] = None,
                 on_delete: Optional[Callable[[list], None]] = None, **kw):
        super().__init__(**{k: v for k, v in kw.items()
                            if k in Component.KWARGS})
        self.on_links, self.on_delete = on_links, on_delete
        self._nodes: list = []
        for n in nodes:
            self.add_node(n)
        self.links = links

    @property
    def nodes(self) -> list:
        return list(self._nodes)

    def add_node(self, node: Node) -> Node:
        self._nodes.append(node)
        self.add(node)
        return node

    def remove_node(self, node: Node) -> None:
        links = [l for l in self.links if l[0] is not node and l[2] is not node]
        self._nodes.remove(node)
        self.remove(node)
        self.links = links

    @property
    def links(self) -> list:
        out = []
        for s in self.strs("items"):
            a, o, b, i = (int(v) for v in s.split())
            if a < len(self._nodes) and b < len(self._nodes):
                out.append((self._nodes[a], o, self._nodes[b], i))
        return out

    @links.setter
    def links(self, links) -> None:
        def idx(n):
            return n if isinstance(n, int) else self._nodes.index(n)
        self.set("items", ["%d %d %d %d" % (idx(a), o, idx(b), i) for a, o, b, i in links])

    @property
    def selected(self) -> list:
        return [n for n in self._nodes if n.selected]

    @property
    def zoom(self) -> float:
        return self.get("zoom")

    @zoom.setter
    def zoom(self, z: float) -> None:
        self.set("zoom", float(z))

    def view_all(self) -> None:
        self.set("view_all", 1.0)

    def _sync_order(self) -> None:
        """C raises the clicked node to the top: follow its child order."""
        self._nodes = list(self.children)

    def _on_event(self, kind, ref, a, b):
        if kind != EV_CHANGE:
            return
        if int(a) == EV_ORDER:
            self._sync_order()
        elif int(a) == EV_LINKS and self.on_links is not None:
            self.on_links(self.links)
        elif int(a) == EV_DELETE:
            doomed = self.selected
            if self.on_delete is not None:
                self.on_delete(doomed)
            else:
                for n in doomed:
                    self.remove_node(n)
