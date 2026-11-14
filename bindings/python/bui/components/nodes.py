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

EV_MOVED, EV_SELECT, EV_VALUE, EV_COLLAPSE, EV_CONTROL, EV_LINKS, EV_DELETE, EV_ORDER = 1, 2, 3, 4, 5, 10, 11, 12


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
    NODE_CLASS_COLORS) or pass ``color=``.

    ``controls`` put widgets in the node's body, between the outputs and the
    inputs, as Blender's node buttons do: ``("button", "Run")``,
    ``("checkbox", "Wireframe", True)`` and ``("dropdown", "Mode", ["Fast",
    "Exact"], 0)`` (a label, the options, the chosen index). ``on_change(node,
    "control", (index, value))`` runs when one is pressed or changed: the value
    is None for a button, a bool for a checkbox and the chosen option's index
    for a dropdown. ``control_value(index)`` reads it."""

    TYPE = "graphnode"
    CONTROLS = ("button", "checkbox", "dropdown")

    def __init__(self, title: str = "Node", *, inputs: Sequence = (), outputs: Sequence = (),
                 location=(0.0, 0.0), width: float = 140.0, kind: str = "converter",
                 color=None, collapsed: bool = False, controls: Sequence = (),
                 on_change: Optional[Callable[["Node", int, object], None]] = None):
        super().__init__()
        self.title = title
        self.on_change = on_change
        self._inputs = [tuple(i) for i in inputs]
        self._outputs = [tuple(o) for o in outputs]
        self._controls = [tuple(c) for c in controls]
        for c in self._controls:
            if not c or c[0] not in self.CONTROLS:
                raise ValueError("a node control is one of %s, not %r" % (self.CONTROLS, c[:1]))
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
        for c in self._controls:
            kind, label = c[0], str(c[1]) if len(c) > 1 else ""
            if kind == "button":
                items.append("b|%s" % label)
            elif kind == "checkbox":
                items.append("c|%s|%d" % (label, 1 if len(c) > 2 and c[2] else 0))
            else:
                options = [str(o) for o in (c[2] if len(c) > 2 else ())]
                index = int(c[3]) if len(c) > 3 else 0
                items.append("d|%s|%d|%s" % (label, min(max(index, 0), max(len(options) - 1, 0)), ";".join(options)))
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

    def control_value(self, index: int):
        """The value of control ``index``: None (a button), a bool (a checkbox) or the
        chosen option's index (a dropdown)."""
        k = 0
        for s in self.strs("items"):
            parts = s.split("|")
            if parts[0] in ("b", "c", "d"):
                if k == index:
                    if parts[0] == "b":
                        return None
                    return bool(int(parts[2])) if parts[0] == "c" else int(parts[2])
                k += 1
        raise IndexError(index)

    def _on_event(self, kind, ref, a, b):
        if kind != EV_CHANGE or self.on_change is None:
            return
        what = {EV_MOVED: "location", EV_SELECT: "select", EV_VALUE: "value",
                EV_COLLAPSE: "collapsed", EV_CONTROL: "control"}.get(int(a))
        if what == "control":
            self.on_change(self, "control", (int(b), self.control_value(int(b))))
        elif what == "value":
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

    def __init__(self, nodes: Sequence[Node] = (), *, links: Sequence = (), height: float = None,
                 on_links: Optional[Callable[[list], None]] = None,
                 on_delete: Optional[Callable[[list], None]] = None, **kw):
        super().__init__(**{k: v for k, v in kw.items()
                            if k in Component.KWARGS})
        self.on_links, self.on_delete = on_links, on_delete
        if height:  # natural height in units (default 14)
            self.set("height", float(height))
        self._nodes: list = []
        for n in nodes:
            self.add_node(n)
        self.links = links

    @property
    def nodes(self) -> list:
        return list(self._nodes)

    # `_nodes` mirrors the children, and a link names nodes by their index, so every way of editing
    # the children (add, insert, remove, replace, clear) goes through here and re-encodes the links
    def _relink(self, links) -> None:
        self.links = [l for l in links if l[0] in self._nodes and l[2] in self._nodes]

    def add(self, *components):
        links = self.links
        for c in components:
            if c in self._nodes:
                self._nodes.remove(c)  # adding a child again moves it to the end
        self._nodes.extend(components)
        result = super().add(*components)
        self._relink(links)
        return result

    def insert(self, index: int, component):
        links = self.links
        if component in self._nodes:
            self._nodes.remove(component)  # it moves
        result = super().insert(index, component)
        n = len(self._nodes)
        self._nodes.insert(n if index < 0 or index > n else index, component)
        self._relink(links)
        return result

    def remove(self, component):
        links = self.links
        if component in self._nodes:
            self._nodes.remove(component)
        super().remove(component)
        self._relink(links)
        return component

    def replace(self, old, new):
        links = self.links
        if new in self._nodes:
            self._nodes.remove(new)  # it moves into old's place
        slot = self._nodes.index(old)  # ValueError for a non-child, as Component.replace
        result = super().replace(old, new)
        self._nodes[slot] = new
        self._relink(links)
        return result

    def clear(self):
        self._nodes.clear()
        super().clear()
        self.links = []
        return self

    def add_node(self, node: Node) -> Node:
        self.add(node)
        return node

    def remove_node(self, node: Node) -> None:
        if node not in self._nodes:
            raise ValueError("remove_node(): node is not in this editor")
        self.remove(node)

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
