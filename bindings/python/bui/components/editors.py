"""Blender's data editors (C: ``kit/intern/kit_color.c``): color ramp,
curve mapping, normal ball, histogram, and the Popover button.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

from typing import Callable, Optional, Sequence

from bui.components.containers import Column, Row
from bui.components.core import EV_CHANGE, Component, ValueComponent
from bui.components.widgets import ColorSwatch, Dropdown, EnumRow, IconButton, NumberField, Spacer

INTERPOLATIONS = ["Linear", "Ease", "B-Spline", "Constant"]


class _RampBand(Component):
    TYPE = "ramp"

    def __init__(self, owner: "ColorRamp"):
        super().__init__()
        self._owner = owner

    def _on_event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            self._owner._band_changed()


class ColorStop:
    __slots__ = ("pos", "color")

    def __init__(self, pos: float, color):
        self.pos, self.color = float(pos), tuple(color)

    def __repr__(self):
        return "ColorStop(%.3f, %r)" % (self.pos, self.color)


class ColorRamp(Column):
    """Blender's color ramp: add/remove, interpolation, the band (click a
    stop to select, drag to move, Ctrl+click to add, X/Delete to remove) and
    the active stop's position and color."""

    def __init__(self, stops=((0.0, (0, 0, 0, 1)), (1.0, (1, 1, 1, 1))), *,
                 interpolation: str = "Linear",
                 on_change: Optional[Callable[[list], None]] = None, **kw):
        self.on_change = on_change
        self.band = _RampBand(self)
        self.band.set_ramp([(p, tuple(c) + ((1.0,) if len(c) == 3 else ())) for p, c in stops])
        self.pos_field = NumberField("Pos", 0.0, min=0.0, max=1.0, step=0.01,
                                     on_change=self._pos_changed)
        self.swatch = ColorSwatch("", (0, 0, 0, 1), on_change=self._color_changed)
        self.interp = Dropdown("", INTERPOLATIONS, interpolation,
                               on_change=self._interp_changed, weight=3)
        tools = Row(IconButton("ADD", self.add_stop, emboss=True, weight=0,
                               description="Add a stop after the active one"),
                    IconButton("REMOVE", self.remove_stop, emboss=True, weight=0,
                               description="Delete the active stop"),
                    Spacer(weight=3), self.interp)
        super().__init__(tools, self.band, Row(self.pos_field, self.swatch), **kw)
        self._interp_changed(interpolation, notify=False)
        self._sync()

    # model ------------------------------------------------------------------------
    @property
    def stops(self):
        return [ColorStop(p, c) for p, c in self.band.ramp()]

    def sorted_stops(self):
        return sorted(self.stops, key=lambda s: s.pos)

    @property
    def active(self) -> ColorStop:
        p, c = self.band.ramp()[int(self.band.get("active"))]
        return ColorStop(p, c)

    @property
    def interpolation(self) -> str:
        return INTERPOLATIONS[int(self.band.get("interpolation"))]

    @interpolation.setter
    def interpolation(self, name: str) -> None:
        self.interp.set_value(name)

    def evaluate(self, t: float):
        return self.band.ramp_eval(t)

    def add_stop(self, pos: float = None) -> None:
        if pos is None:  # Blender adds halfway after the active stop
            a = self.active.pos
            later = [s.pos for s in self.stops if s.pos > a]
            pos = (a + min(later)) / 2 if later else a
        self.band.set("add", pos)
        self._band_changed()

    def remove_stop(self) -> None:
        self.band.set("remove", 1)
        self._band_changed()

    # wiring ------------------------------------------------------------------------
    def _sync(self):
        st = self.active
        self.pos_field.set_value(st.pos, notify=False)
        self.swatch.set_value(st.color, notify=False)

    def _band_changed(self):
        self._sync()
        if self.on_change is not None:
            self.on_change(self.sorted_stops())

    def _pos_changed(self, v):
        self.band.set("active_pos", v)
        self._band_changed()

    def _color_changed(self, c):
        self.band.set_ramp_color(c)
        if self.on_change is not None:
            self.on_change(self.sorted_stops())

    def _interp_changed(self, name, notify=True):
        self.band.set("interpolation", INTERPOLATIONS.index(name))
        if notify and self.on_change is not None:
            self.on_change(self.sorted_stops())


class _CurveGraph(Component):
    TYPE = "curve"

    def __init__(self, owner: "CurveMapping", height: float):
        super().__init__()
        self._owner = owner
        self.set("height", height)

    def _on_event(self, kind, ref, a, b):
        if kind == EV_CHANGE and self._owner.on_change is not None:
            self._owner.on_change(self._owner.channel, self._owner.points)


class CurveMapping(Column):
    """Blender's curve widget: click the curve to add a point, Ctrl+click to
    add anywhere, drag points (Ctrl snaps), X/Delete removes the selected
    point. ``channels=("C", "R", "G", "B")`` adds channel tabs."""

    def __init__(self, points=((0.0, 0.0), (1.0, 1.0)), *, channels: Sequence[str] = (),
                 height: float = 8.0,
                 on_change: Optional[Callable[[str, list], None]] = None, **kw):
        self.on_change = on_change
        self.channels = list(channels) or ["C"]
        self.graph = _CurveGraph(self, height)
        self.graph.set("channels", len(self.channels))
        for ch in range(len(self.channels)):
            self.graph.set_curve(ch, list(points))
        first = (EnumRow("", self.channels, self.channels[0], on_change=self._set_channel,
                         weight=4) if len(self.channels) > 1 else Spacer(weight=4))
        tools = Row(first, IconButton("LOOP_BACK", self.reset, emboss=True, weight=0,
                                      description="Reset the curve"))
        super().__init__(tools, self.graph, **kw)

    @property
    def channel(self) -> str:
        return self.channels[int(self.graph.get("channel"))]

    @property
    def points(self):
        return self.graph.curve(int(self.graph.get("channel")))

    def evaluate(self, x: float, channel: str = None) -> float:
        ch = self.channels.index(channel) if channel else int(self.graph.get("channel"))
        return self.graph.curve_eval(ch, x)

    def _set_channel(self, name):
        self.graph.set("channel", self.channels.index(name))

    def reset(self):
        self.graph.set_curve(int(self.graph.get("channel")), [(0.0, 0.0), (1.0, 1.0)])
        if self.on_change is not None:
            self.on_change(self.channel, self.points)

    def remove_selected(self):
        self.graph.set("remove_selected", 1)


class NormalBall(ValueComponent):
    """Direction picker drawn as a lit sphere; drag to aim."""

    TYPE = "normal"

    def __init__(self, label: str = "", value=(0.0, 0.0, 1.0), *, size: float = 4.0, **kw):
        Component.__init__(self, **{k: v for k, v in kw.items()
                                    if k in ("description", "enabled", "visible", "weight", "fill")})
        self.on_change = kw.get("on_change")
        self.set("height", size)
        self._store(value, False)

    def _store(self, v, notify):
        x, y, z = (float(c) for c in v)
        n = (x * x + y * y + z * z) ** 0.5 or 1.0
        self.set("color", (x / n, y / n, z / n, 1.0))
        if notify and self.on_change is not None:
            self.on_change(self.value)

    def _load(self):
        return self.get_color("color")[:3]


class Histogram(Component):
    """Image histogram (display only): ``bins`` are luma values or
    ``(r, g, b)`` triples, each 0..1."""

    TYPE = "histogram"

    def __init__(self, bins: Sequence = (), *, height: float = 5.0, **kw):
        super().__init__(**kw)
        self.set("height", height)
        self.set_bins(bins)

    def set_bins(self, bins) -> None:
        bins = list(bins)
        channels = 3 if bins and isinstance(bins[0], (tuple, list)) else 1
        self.set_histogram(bins, channels)


class Popover(Component):
    """Button that opens a panel of components (Blender popover)."""

    TYPE = "popover"

    def __init__(self, text: str, *children: Component, icon: str = None, **kw):
        kw.pop("width", None)
        super().__init__(**kw)
        self.set("text", text)
        if icon:
            self.set("icon", icon)
        body = self.body
        for c in children:
            body.add(c)

    @property
    def body(self) -> Component:
        return self.children[0]

    def _content(self) -> Component:
        return self.body
