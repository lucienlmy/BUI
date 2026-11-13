"""QtGui pieces on BUI: font metrics (the kit measures the text), palettes (Blender's theme colors,
and widget colors you set), the cursor and screen positions (from the window host), validators
(QLineEdit enforces them) and the events for resizes, context menus and timers.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import re

import buipy

from bui import host
from bui.qtsyntax import QColor, QEvent, QFont, QPoint, QRect, QSize


# ==========================================================================
# Events


class QResizeEvent(QEvent):
    def __init__(self, size, old_size):
        super().__init__(14)
        self._size, self._old = size, old_size

    def size(self):
        return self._size

    def oldSize(self):  # noqa: N802
        return self._old


class QContextMenuEvent(QEvent):
    Mouse, Keyboard, Other = 0, 1, 2

    def __init__(self, reason, pos, global_pos=None, modifiers=0):
        super().__init__(82)
        self._reason, self._pos, self._global, self._mods = reason, pos, global_pos or pos, modifiers

    def reason(self):
        return self._reason

    def pos(self):
        return self._pos

    def x(self):
        return self._pos.x()

    def y(self):
        return self._pos.y()

    def globalPos(self):  # noqa: N802
        return self._global

    def modifiers(self):
        return self._mods


class QTimerEvent(QEvent):
    def __init__(self, timer_id):
        super().__init__(1)
        self._id = timer_id

    def timerId(self):  # noqa: N802
        return self._id


# ==========================================================================
# Fonts


class QFontMetrics:
    """Text sizes in a font, measured by the kit (the same shaping and font as it draws with)."""

    def __init__(self, font=None):
        font = font if font is not None else QFont()
        self._size = float(font.pointSize() if font.pointSize() > 0 else 11) * buipy.kit_scale() * 96 / 72
        self._family = getattr(font, "_family", "") or ""

    def horizontalAdvance(self, text, length=-1):  # noqa: N802
        text = str(text) if length < 0 else str(text)[:length]
        w = buipy.measure_text(text, self._size, self._family)
        return self._round(w or 0.0)

    width = horizontalAdvance

    def boundingRect(self, text):  # noqa: N802
        return QRect(0, -self.ascent(), self.horizontalAdvance(text), self.height())

    def size(self, flags, text):
        lines = str(text).split("\n")
        return QSize(max(self.horizontalAdvance(l) for l in lines), self.lineSpacing() * len(lines))

    def height(self):
        return self._round(self._size * 1.2)

    def ascent(self):
        return self._round(self._size * 0.95)

    def descent(self):
        return self.height() - self.ascent()

    def lineSpacing(self):  # noqa: N802
        return self.height()

    def leading(self):
        return 0

    def averageCharWidth(self):  # noqa: N802
        return self._round(self.horizontalAdvance("abcdefghijklmnopqrstuvwxyz") / 26.0)

    def elidedText(self, text, mode, width, flags=0):  # noqa: N802
        """`text` shortened with … to fit `width` (ElideRight 1, ElideLeft 0, ElideMiddle 2, ElideNone 3)."""
        text = str(text)
        if mode == 3 or self.horizontalAdvance(text) <= width:
            return text
        for n in range(len(text) - 1, -1, -1):
            if mode == 0:
                cand = "…" + text[len(text) - n:]
            elif mode == 2:
                cand = text[:(n + 1) // 2] + "…" + text[len(text) - n // 2:] if n else "…"
            else:
                cand = text[:n] + "…"
            if self.horizontalAdvance(cand) <= width:
                return cand
        return ""

    @staticmethod
    def _round(v):
        return int(round(v))


class QFontMetricsF(QFontMetrics):
    @staticmethod
    def _round(v):
        return float(v)


# ==========================================================================
# Palettes


class QPalette:
    """Colors by role. A fresh palette has Blender's theme colors; a widget's setPalette applies the
    window / base / button background and the text colors to it."""
    (WindowText, Button, Light, Midlight, Dark, Mid, Text, BrightText, ButtonText, Base, Window, Shadow,
     Highlight, HighlightedText, Link, LinkVisited, AlternateBase, NoRole, ToolTipBase, ToolTipText,
     PlaceholderText, Accent) = range(22)
    Foreground, Background = WindowText, Window
    Active, Disabled, Inactive = 0, 1, 2
    _THEME = {Window: "region_bg", WindowText: "text", Base: "regular_inner", Text: "regular_text",
              Button: "regular_inner", ButtonText: "regular_text", Highlight: "regular_inner_sel",
              HighlightedText: "regular_text_sel", ToolTipBase: "tooltip_inner", ToolTipText: "tooltip_text"}

    def __init__(self, other=None):
        self._colors = dict(other._colors) if isinstance(other, QPalette) else {}
        if isinstance(other, (QColor, str)):
            self._colors[self.Button] = QColor(other)

    def color(self, *args):
        role = args[-1]
        if role in self._colors:
            return QColor(self._colors[role])
        key = self._THEME.get(role)
        rgba = buipy.kit_theme_color(key) if key else None
        if rgba:
            return QColor.fromRgbF(*rgba) if hasattr(QColor, "fromRgbF") else QColor(
                *[int(round(c * 255)) for c in rgba])
        return QColor(0, 0, 0)

    brush = color

    def setColor(self, *args):  # noqa: N802 - (role, color) or (group, role, color)
        self._colors[args[-2]] = QColor(args[-1])

    setBrush = setColor

    def isBrushSet(self, group, role):  # noqa: N802
        return role in self._colors

    def resolve(self, other):
        p = QPalette(other)
        p._colors.update(self._colors)
        return p

    def window(self):
        return self.color(self.Window)

    def windowText(self):  # noqa: N802
        return self.color(self.WindowText)

    def base(self):
        return self.color(self.Base)

    def text(self):
        return self.color(self.Text)

    def button(self):
        return self.color(self.Button)

    def buttonText(self):  # noqa: N802
        return self.color(self.ButtonText)

    def highlight(self):
        return self.color(self.Highlight)

    def __eq__(self, other):
        return isinstance(other, QPalette) and self._colors == other._colors


# ==========================================================================
# Cursor and screens


def _main_window():
    windows = list(host._windows)
    return windows[0] if windows else None


class QCursor:
    def __init__(self, shape=0):
        self._shape = shape

    def shape(self):
        return self._shape

    @staticmethod
    def pos(*a):
        """Where the pointer is, in screen coordinates (asked of the window host); (0, 0) with no window."""
        w = _main_window()
        got = w.wm_get("pointer") if w is not None else None
        return QPoint(int(got[0]), int(got[1])) if got else QPoint(0, 0)


class QScreen:
    def __init__(self, width, height, scale):
        self._w, self._h, self._scale = int(width), int(height), float(scale or 1.0)

    def geometry(self):
        return QRect(0, 0, self._w, self._h)

    availableGeometry = virtualGeometry = availableVirtualGeometry = geometry

    def size(self):
        return QSize(self._w, self._h)

    availableSize = size

    def devicePixelRatio(self):  # noqa: N802
        return self._scale

    def logicalDotsPerInch(self):  # noqa: N802
        return 96.0 * self._scale

    def name(self):
        return "screen"


def primary_screen():
    """The screen of the first window (size and scale from the host), or None without a window."""
    w = _main_window()
    if w is None:
        return None
    size = w.wm_get("screen") or (0, 0)
    scale = (w.wm_get("scale") or (1.0, 0))[0]
    return QScreen(size[0], size[1], scale)


# ==========================================================================
# Validators (QLineEdit.setValidator enforces them)


class QValidator:
    Invalid, Intermediate, Acceptable = 0, 1, 2

    def validate(self, text, pos=0):
        return (self.Acceptable, text, pos)

    def fixup(self, text):
        return text


class QIntValidator(QValidator):
    def __init__(self, bottom=-2147483648, top=2147483647, parent=None):
        if not isinstance(bottom, int):
            bottom, top = -2147483648, 2147483647
        self._lo, self._hi = bottom, top

    def bottom(self):
        return self._lo

    def top(self):
        return self._hi

    def setRange(self, lo, hi):  # noqa: N802
        self._lo, self._hi = int(lo), int(hi)

    def setBottom(self, v):  # noqa: N802
        self._lo = int(v)

    def setTop(self, v):  # noqa: N802
        self._hi = int(v)

    def validate(self, text, pos=0):
        t = str(text).strip()
        if t in ("", "-", "+"):
            return (self.Intermediate, text, pos)
        if not re.fullmatch(r"[+-]?\d+", t):
            return (self.Invalid, text, pos)
        v = int(t)
        if self._lo <= v <= self._hi:
            return (self.Acceptable, text, pos)
        # too long for the range is invalid; a prefix of a valid number is intermediate
        return (self.Intermediate if len(t.lstrip("+-")) <= len(str(max(abs(self._lo), abs(self._hi))))
                else self.Invalid, text, pos)


class QDoubleValidator(QValidator):
    StandardNotation, ScientificNotation = 0, 1

    def __init__(self, bottom=float("-inf"), top=float("inf"), decimals=1000, parent=None):
        if not isinstance(bottom, (int, float)):
            bottom, top = float("-inf"), float("inf")
        self._lo, self._hi, self._dec = float(bottom), float(top), int(decimals)

    def bottom(self):
        return self._lo

    def top(self):
        return self._hi

    def decimals(self):
        return self._dec

    def setRange(self, lo, hi, decimals=None):  # noqa: N802
        self._lo, self._hi = float(lo), float(hi)
        if decimals is not None:
            self._dec = int(decimals)

    def setDecimals(self, d):  # noqa: N802
        self._dec = int(d)

    def validate(self, text, pos=0):
        t = str(text).strip()
        if t in ("", "-", "+", ".", "-.", "+."):
            return (self.Intermediate, text, pos)
        m = re.fullmatch(r"[+-]?(\d*)(?:\.(\d*))?(?:[eE][+-]?\d+)?", t)
        if not m:
            return (self.Invalid, text, pos)
        if m.group(2) and len(m.group(2)) > self._dec:
            return (self.Invalid, text, pos)
        try:
            v = float(t)
        except ValueError:
            return (self.Intermediate, text, pos)
        return (self.Acceptable if self._lo <= v <= self._hi else self.Intermediate, text, pos)


class QRegularExpression:
    def __init__(self, pattern=""):
        self._pattern = str(pattern)
        self._re = re.compile(self._pattern)

    def pattern(self):
        return self._pattern

    def setPattern(self, p):  # noqa: N802
        self.__init__(p)

    def isValid(self):  # noqa: N802
        return True

    def match(self, text):
        return self._re.search(str(text))


class QRegularExpressionValidator(QValidator):
    def __init__(self, regex=None, parent=None):
        self._rx = regex if isinstance(regex, QRegularExpression) else QRegularExpression(regex or ".*")

    def regularExpression(self):  # noqa: N802
        return self._rx

    def setRegularExpression(self, rx):  # noqa: N802
        self._rx = rx if isinstance(rx, QRegularExpression) else QRegularExpression(rx)

    def validate(self, text, pos=0):
        t = str(text)
        # Python's re can't tell a prefix that could still match from one that can't, so anything that
        # doesn't match yet is Intermediate: typing isn't blocked, hasAcceptableInput() says whether it fits
        return (self.Acceptable if self._rx._re.fullmatch(t) else self.Intermediate, text, pos)
