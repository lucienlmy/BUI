"""Component base classes over the C kit (``buipy.KitNode``).

Every component *is* a C node: layout, input handling and drawing run in
``kit/`` (C). The Python classes only map constructor arguments to node
properties and turn C events into ``on_change`` / ``on_click`` callbacks.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

from typing import Callable, Optional

import buipy

# Event kinds delivered to Component._on_event (BUI_kit.h BK_EV_*).
EV_CHANGE, EV_CLICK, EV_CALL, EV_RENAME, EV_ACTIVATE, EV_TOGGLE, EV_BROWSE = range(1, 8)
# Raw events for nodes with the "events" property (Tk bind, Qt event handlers).
(EV_PRESS, EV_RELEASE, EV_DOUBLE, EV_MOTION, EV_ENTER, EV_LEAVE, EV_KEY, EV_WHEEL,
 EV_SUBMIT, EV_VIEW, EV_SCROLL, EV_CONFIGURE) = range(8, 20)

# Modifier bits for KitContext input calls.
SHIFT, CTRL, ALT, BUTTON1 = 1, 2, 4, 8

# Cursor ids (BK_CURSOR_*).
CURSORS = (None, "ew", "ns", "text", "crosshair", "hand", "move", "none")


def mods_bits(mods) -> int:
    """Modifier object (shift/ctrl/alt/button1 attributes) -> kit bits."""
    if mods is None:
        return 0
    if isinstance(mods, int):
        return mods
    return ((SHIFT if getattr(mods, "shift", False) else 0)
            | (CTRL if getattr(mods, "ctrl", False) else 0)
            | (ALT if getattr(mods, "alt", False) else 0)
            | (BUTTON1 if getattr(mods, "button1", False) else 0))


def unit() -> int:
    """U.widget_unit in pixels (20 at scale 1)."""
    return buipy.kit_unit()


class Component(buipy.KitNode):
    """Base of every component. ``x, y, w, h`` / ``rect`` come from the last
    layout (provided by the C node).

    Motion's props (:mod:`bui.motion`): ``initial`` / ``animate`` (dicts of property ->
    value) and ``transition`` (the options) animate the component from its first frame, e.g.
    ``NumberSlider("Volume", initial={"value": 0}, animate={"value": 0.8})``; the motion is
    ``self.motion``. Kit widgets animate values, colors and sizes: they have no transforms or
    opacity. ``while_hover``, ``while_tap`` and ``exit`` aren't supported yet and raise."""

    TYPE = "column"
    #: The keyword arguments this base class takes (subclasses that read the rest pass these on).
    KWARGS = frozenset(("description", "enabled", "visible", "weight", "fill", "initial", "animate",
                        "transition", "while_hover", "while_tap", "while_focus", "while_drag",
                        "while_in_view", "exit", "variants", "layout"))

    def __init__(self, *, description: str = "", enabled: bool = True,
                 visible: bool = True, weight: float = 1.0, fill: bool = False,
                 initial: Optional[dict] = None, animate: Optional[dict] = None,
                 transition: Optional[dict] = None, **unsupported):
        for name in unsupported:
            if name in Component.KWARGS:
                raise NotImplementedError(f"the motion prop '{name}' is not supported on components yet")
            raise TypeError(f"{type(self).__name__}() got an unexpected keyword argument '{name}'")
        super().__init__(self.TYPE)
        self.motion = None
        if initial or animate:
            from bui import motion
            self.motion = motion.mount(self, initial, animate, transition)
        if fill:  # in a column: grow into the spare height (a region filling its area)
            self.set("fill", True)
        if description:
            self.set("description", description)
        if not enabled:
            self.set("enabled", False)
        if not visible:
            self.set("visible", False)
        if weight != 1.0:
            self.set("weight", weight)

    # -- common properties ---------------------------------------------------------
    @property
    def enabled(self) -> bool:
        return bool(self.get("enabled"))

    @enabled.setter
    def enabled(self, v: bool) -> None:
        self.set("enabled", bool(v))

    @property
    def visible(self) -> bool:
        return bool(self.get("visible"))

    @visible.setter
    def visible(self, v: bool) -> None:
        self.set("visible", bool(v))

    @property
    def description(self) -> str:
        return self.get_str("description")

    @description.setter
    def description(self, text: str) -> None:
        self.set("description", text or "")

    @property
    def weight(self) -> float:
        return self.get("weight")

    @weight.setter
    def weight(self, v: float) -> None:
        self.set("weight", float(v))

    @property
    def corners(self) -> int:
        return int(self.get("corners"))

    def add(self, *components: "Component"):
        for c in components:
            buipy.KitNode.add(self, c)
        return components[0] if len(components) == 1 else self

    def _on_event(self, kind, ref, a, b):
        pass


class ValueComponent(Component):
    """A component holding a value: ``value``, ``set_value``, ``on_change``
    and Blender's value shortcuts (Ctrl+C/V, Backspace, Ctrl+Wheel, right
    click menu) — all handled in C."""

    def __init__(self, label: str = "", value=None, *, default=None,
                 on_change: Optional[Callable] = None, **kw):
        super().__init__(**kw)
        self.on_change = on_change
        if label:
            self.set("label", label)
        if value is not None:
            self._store(value, notify=False)
        self._set_default(self.value if default is None else default)

    # per-type value mapping (override) ---------------------------------------------
    def _store(self, v, notify: bool) -> None:
        self.set("set_value" if notify else "value", v)

    def _load(self):
        return self.get("value")

    def _set_default(self, v) -> None:
        if isinstance(v, (int, float, bool)):
            self.set("default", float(v))

    # API ------------------------------------------------------------------------------
    @property
    def value(self):
        return self._load()

    @value.setter
    def value(self, v) -> None:
        self.set_value(v)

    def set_value(self, v, notify: bool = True) -> None:
        self._store(v, notify)

    def reset(self) -> None:
        """Back to the default value."""
        self._store(self._default(), True)

    def _default(self):
        return self.get("default")

    @property
    def label(self) -> str:
        return self.get_str("label")

    @label.setter
    def label(self, text: str) -> None:
        self.set("label", text or "")

    def _on_event(self, kind, ref, a, b):
        if kind == EV_CHANGE and self.on_change is not None:
            self.on_change(self.value)
