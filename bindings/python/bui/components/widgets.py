"""Leaf components: labels, buttons, toggles, fields, sliders, enums, colors.

Each class is a C node type (``kit/intern/kit_widgets.c``) with Blender's
drawing and behavior; Python maps arguments and callbacks.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

from typing import Callable, Optional, Sequence

import buipy

from bui.components.core import EV_CLICK, Component, ValueComponent


def eval_number(text: str) -> Optional[float]:
    """Blender-style number input: ``2*pi``, ``1/3 + 2^2``, ``sqrt(2)``."""
    return buipy.kit_eval_number(text)


# ==========================================================================
# Static


class Label(Component):
    TYPE = "label"

    def __init__(self, text: str = "", *, icon: str = None, align: str = "left",
                 color=None, bold: bool = False, **kw):
        super().__init__(**kw)
        self.set("text", text)
        if icon:
            self.set("icon", icon)
        if align != "left":
            self.set("text_align", align)
        if color is not None:
            self.set("text_color", color)
        if bold:
            self.set("bold", True)

    @property
    def text(self) -> str:
        return self.get_str("text")

    @text.setter
    def text(self, s: str) -> None:
        self.set("text", s)


class Image(Component):
    """An image file (PNG, JPEG, WebP, GIF, BMP, TGA, QOI, Radiance HDR, PNM,
    SVG) fitted into the component like CSS ``object-fit`` (``"contain"``,
    ``"cover"``, ``"fill"``, ``"none"``, ``"scale-down"``). Its natural size
    comes from the file's header; the pixels decode on the kit's worker
    threads at the size drawn and appear when ready."""

    TYPE = "image"
    FITS = {"fill": 0, "contain": 1, "cover": 2, "none": 3, "scale-down": 4}

    def __init__(self, src: str = "", *, fit: str = "contain", frame: Optional[int] = None,
                 **kw):
        super().__init__(**kw)
        if fit not in self.FITS:
            raise ValueError(f"fit must be one of {sorted(self.FITS)}, not {fit!r}")
        self._fit = fit
        self._src = ""
        self._frame = None
        self.src = src
        self.frame = frame

    @property
    def frame(self) -> Optional[int]:
        """Animations (GIF, animated WebP): ``None`` plays them (each frame
        for its delay, paused while off screen), a number holds that frame."""
        return self._frame

    @frame.setter
    def frame(self, n: Optional[int]) -> None:
        if n is not None and int(n) < 0:
            raise ValueError("frame must be None or >= 0")
        self._frame = None if n is None else int(n)
        self.set_image_frame(0 if n is None else int(n) + 1)

    @property
    def src(self) -> str:
        return self._src

    @src.setter
    def src(self, path: str) -> None:
        self._src = str(path or "")
        self.set_image_source(self._src or None, self.FITS[self._fit])


VIDEO_ENDED = 3  # buipy.Video.state


class Video(Component):
    """A video file (MP4, WebM, MOV, ... whatever the OS decodes: Media
    Foundation, AVFoundation, GStreamer), fitted like CSS ``object-fit``.
    Muted autoplay unless ``autoplay=False``; it decodes at the size drawn
    and only while it shows. ``controls=True`` draws Blender's play/pause
    button and a scrubber with the time. ``player`` is the
    :class:`bui.Video` (play, pause, seek, rate, time, duration, on_state)."""

    TYPE = "video"
    FITS = Image.FITS

    def __init__(self, src: str, *, autoplay: bool = True, loop: bool = False,
                 controls: bool = False, poster: Optional[str] = None, fit: str = "contain",
                 on_ended: Optional[Callable[[], None]] = None, **kw):
        super().__init__(**kw)
        if fit not in self.FITS:
            raise ValueError(f"fit must be one of {sorted(self.FITS)}, not {fit!r}")
        self.player = buipy.Video(str(src))  # kept by the node: it borrows the player
        self.player.loop = loop
        self.player.controls = controls
        if poster:
            self.player.poster = str(poster)
        if not autoplay:
            self.player.pause()
        if on_ended is not None:
            self.player.on_state = lambda s: on_ended() if s == VIDEO_ENDED else None
        self.player.attach(self, self.FITS[fit])

    def play(self) -> None:
        self.player.play()

    def pause(self) -> None:
        self.player.pause()

    def seek(self, seconds: float) -> None:
        self.player.seek(seconds)


class Separator(Component):
    """Vertical space (``factor`` widget units), optionally with a line."""

    TYPE = "separator"

    def __init__(self, factor: float = 0.5, line: bool = False, **kw):
        super().__init__(**kw)
        self.set("factor", factor)
        if line:
            self.set("line", True)


class Spacer(Separator):
    """Empty space that takes its ``weight`` share of a Row."""

    def __init__(self, **kw):
        super().__init__(1.0, **kw)


class ProgressBar(Component):
    TYPE = "progress"

    def __init__(self, value: float = 0.0, text: Optional[str] = None, **kw):
        super().__init__(**kw)
        self.set("factor", value)
        if text is not None:
            self.set("text", text)

    @property
    def value(self) -> float:
        return self.get("factor")

    def set_value(self, value: float) -> None:
        self.set("factor", max(0.0, min(1.0, float(value))))

    @property
    def text(self) -> str:
        return self.get_str("text")

    @text.setter
    def text(self, s: str) -> None:
        self.set("text", s)


# ==========================================================================
# Buttons


class Button(Component):
    """Operator button: ``on_click`` runs when released over it. ``icon``
    adds a Blender icon; ``emboss=False`` draws it flat."""

    TYPE = "button"

    def __init__(self, text: str = "", on_click: Optional[Callable[[], None]] = None, *,
                 icon: str = None, emboss: bool = True, **kw):
        super().__init__(**kw)
        self.on_click = on_click
        self.set("text", text)
        if icon:
            self.set("icon", icon)
        if not emboss:
            self.set("emboss", False)

    @property
    def text(self) -> str:
        return self.get_str("text")

    @text.setter
    def text(self, s: str) -> None:
        self.set("text", s)

    def click(self) -> None:
        if self.enabled and self.on_click is not None:
            self.on_click()

    def _on_event(self, kind, ref, a, b):
        if kind == EV_CLICK and self.on_click is not None:
            self.on_click()


class IconButton(Button):
    """Icon-only button (flat unless ``emboss=True``)."""

    def __init__(self, icon: str, on_click=None, *, emboss: bool = False, **kw):
        super().__init__("", on_click, icon=icon, emboss=emboss, **kw)


# ==========================================================================
# Booleans


class Checkbox(ValueComponent):
    """Option box. Press and drag across other toggles to set them all."""

    TYPE = "checkbox"

    def __init__(self, label: str = "", value: bool = False, **kw):
        super().__init__(label, bool(value), **kw)

    def _store(self, v, notify):
        self.set("set_value" if notify else "value", 1.0 if v else 0.0)

    def _load(self):
        return self.get("value") != 0

    def _default(self):
        return self.get("default") != 0

    def toggle(self) -> None:
        self.set_value(not self.value)


class Toggle(Checkbox):
    """Button that stays pressed. ``icon``/``icon_on`` for icon toggles,
    ``emboss=False`` for Blender's flat outliner/header toggles."""

    TYPE = "toggle"

    def __init__(self, label: str = "", value: bool = False, *, icon: str = None,
                 icon_on: str = None, emboss: bool = True, **kw):
        super().__init__(label, value, **kw)
        if icon:
            self.set("icon", icon)
        if icon_on:
            self.set("icon_on", icon_on)
        if not emboss:
            self.set("emboss", False)


class Decorator(Checkbox):
    """Blender's property decorator: a dot that becomes a keyframe diamond."""

    TYPE = "decorator"


# ==========================================================================
# Numbers


class NumberField(ValueComponent):
    """Blender number field: drag to scrub (cursor hidden, continuous grab;
    Shift precise, Ctrl snap, Alt all linked), arrows to step, click to type
    (``2*pi``), Ctrl+Wheel, Ctrl+C/V, Backspace, Minus."""

    TYPE = "number"

    def __init__(self, label: str = "", value=0.0, *, min=None, max=None, step=None,
                 precision=None, integer=None, suffix: str = "", **kw):
        if integer is None:
            integer = isinstance(value, int) and not isinstance(value, bool)
        self._integer = integer
        Component.__init__(self, **{k: v for k, v in kw.items()
                                    if k in Component.KWARGS})
        if integer:
            self.set("integer", True)
        if min is not None:
            self.set("min", min)
        if max is not None:
            self.set("max", max)
        self.set("step", step if step is not None else (1 if integer else 0.1))
        self.set("precision", 0 if integer else (3 if precision is None else precision))
        if suffix:
            self.set("suffix", suffix)
        rest = {k: v for k, v in kw.items()
                if k not in Component.KWARGS}
        self.on_change = rest.pop("on_change", None)
        default = rest.pop("default", None)
        if label:
            self.set("label", label)
        self.set("value", value)
        self.set("default", float(value if default is None else default))

    def _load(self):
        v = self.get("value")
        return int(round(v)) if self._integer else v

    def _default(self):
        return self.get("default")

    @property
    def editing(self) -> bool:
        return bool(self.get("editing"))


class NumberSlider(NumberField):
    """Blender number slider: the field fills with the value."""

    TYPE = "slider"

    def __init__(self, label: str = "", value=0.0, *, min=0.0, max=1.0, **kw):
        super().__init__(label, value, min=min, max=max, **kw)


# ==========================================================================
# Text


class TextField(ValueComponent):
    """Single-line text (Blender's text editing: word jumps with Ctrl,
    selection, clipboard, double-click word). ``live=True`` reports every
    keystroke."""

    TYPE = "text"

    def __init__(self, label: str = "", value: str = "", *, placeholder: str = "",
                 icon: str = None, live: bool = False, **kw):
        Component.__init__(self, **{k: v for k, v in kw.items()
                                    if k in Component.KWARGS})
        self.on_change = kw.get("on_change")
        if label:
            self.set("label", label)
        if placeholder:
            self.set("placeholder", placeholder)
        if icon:
            self.set("icon", icon)
        if live:
            self.set("live", True)
        self.set("value", value or "")

    def _store(self, v, notify):
        self.set("set_value" if notify else "value", "" if v is None else str(v))

    def _load(self):
        return self.get_str("value")

    def _default(self):
        return ""

    @property
    def editing(self) -> bool:
        return bool(self.get("editing"))


class TextArea(ValueComponent):
    """Multi-line text (the kit's ``textarea``: Blender's text editing over
    several lines, wheel scrolling, clipboard, undo). ``rows`` sets the
    natural height in lines; ``on_change`` gets the text on every edit. The
    node's ``ta_insert`` / ``ta_delete`` / ``ta_caret`` / ``ta_selection`` /
    ``ta_see`` edit it by byte offset."""

    TYPE = "textarea"

    def __init__(self, value: str = "", *, rows: int = None, **kw):
        super().__init__("", value or "", **kw)
        if rows:
            self.set("rows", int(rows))

    def _store(self, v, notify):
        text = "" if v is None else str(v)
        changed = text != self._load()
        if changed:  # setting it again would clear the selection and the spans
            self.set("value", text)  # the kit's textarea has no "set_value": it notifies here
        if notify and changed and self.on_change is not None:
            self.on_change(text)

    def _load(self):
        return self.get_str("value")

    def _default(self):
        return ""


class SearchField(TextField):
    """Blender search box: magnifier icon, live filtering."""

    TYPE = "search"

    def __init__(self, value: str = "", *, placeholder: str = "Search", **kw):
        super().__init__("", value, placeholder=placeholder, **kw)


class HotkeyField(TextField):
    """Key binding: click, then press a combination (Esc cancels)."""

    TYPE = "hotkey"

    def __init__(self, label: str = "", value: str = "", **kw):
        super().__init__(label, value, **kw)

    @property
    def capturing(self) -> bool:
        return bool(self.get("editing"))


# ==========================================================================
# Choices


def _icon_list(items, icons):
    if not icons:
        return None
    if isinstance(icons, dict):
        return [icons.get(i) or "" for i in items]
    return [i or "" for i in icons]


class _Choice(ValueComponent):
    """One of ``items`` (strings); value is the item string."""

    def __init__(self, label: str = "", items: Sequence[str] = (), value=None, *,
                 icons=None, **kw):
        Component.__init__(self, **{k: v for k, v in kw.items()
                                    if k in Component.KWARGS})
        self.on_change = kw.get("on_change")
        if label:
            self.set("label", label)
        self.set("items", [str(i) for i in items])
        icon_names = _icon_list(self.items, icons)
        if icon_names:
            self.set("icons", icon_names)
        if value is not None:
            self._store(value, notify=False)
        self.set("default", self.get("value"))

    @property
    def items(self):
        return self.strs("items")

    @items.setter
    def items(self, items) -> None:
        self.set("items", [str(i) for i in items])

    def _store(self, v, notify):
        items = self.items
        if v in items:
            self.set("set_value" if notify else "value", float(items.index(v)))

    def _load(self):
        items = self.items
        i = int(self.get("value"))
        return items[i] if 0 <= i < len(items) else None

    def _default(self):
        items = self.items
        i = int(self.get("default"))
        return items[i] if 0 <= i < len(items) else None

    @property
    def index(self) -> int:
        return int(self.get("value"))


class Dropdown(_Choice):
    """Enum menu button: click to open a menu of ``items`` (type to search).
    ``icon_only=True`` shows just the current item's icon."""

    TYPE = "dropdown"

    def __init__(self, label: str = "", items: Sequence[str] = (), value=None, *,
                 icon_only: bool = False, **kw):
        super().__init__(label, items, value, **kw)
        if icon_only:
            self.set("icon_only", True)


class EnumRow(_Choice):
    """Expanded enum: joined radio buttons. ``vertical=True`` stacks them,
    ``icon_only=True`` shows only icons."""

    TYPE = "enum"

    def __init__(self, label: str = "", items: Sequence[str] = (), value=None, *,
                 vertical: bool = False, icon_only: bool = False, **kw):
        super().__init__(label, items, value, **kw)
        if vertical:
            self.set("vertical", True)
        if icon_only:
            self.set("icon_only", True)


# ==========================================================================
# Color


class ColorSwatch(ValueComponent):
    """RGBA color (0..1). Click opens Blender's color picker (``picker=
    "circle"`` HSV wheel or ``"square"``) with RGB/HSV/HSL, hex and an
    eyedropper; Ctrl+C copies hex."""

    TYPE = "swatch"

    def __init__(self, label: str = "", value=(1.0, 1.0, 1.0, 1.0), *,
                 picker: str = "circle", alpha: bool = True, **kw):
        Component.__init__(self, **{k: v for k, v in kw.items()
                                    if k in Component.KWARGS})
        self.on_change = kw.get("on_change")
        if label:
            self.set("label", label)
        if picker == "square":
            self.set("square", True)
        if not alpha:
            self.set("alpha", False)
        self._store(value, notify=False)

    def _store(self, v, notify):
        v = tuple(float(c) for c in v)
        if len(v) == 3:
            v += (1.0,)
        self.set("set_value" if notify else "color", v)

    def _load(self):
        return self.get_color("color")

    def _default(self):
        return (0.0, 0.0, 0.0, 1.0)

    @staticmethod
    def to_text(v) -> str:
        return "#" + "".join("%02x" % int(round(c * 255)) for c in v)
