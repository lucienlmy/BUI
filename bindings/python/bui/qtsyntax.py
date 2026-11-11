"""BUI with Qt syntax — Qt's widget API on the C kit.

Every QWidget is a kit node; QBoxLayout / QGridLayout / QFormLayout run on
the kit's C geometry managers, ``paintEvent`` + QPainter record into a kit
canvas, and top-level windows are native (Win32, X11, Cocoa) or Tk
(:mod:`bui.host`).
Signals and timers are plain Python (as in PySide's Python layer);
QPropertyAnimation, the animation groups and QEasingCurve run on BUI's
animation engine (:mod:`bui.qtanim`)::

    from bui.qtsyntax import QApplication, QWidget, QPushButton, QVBoxLayout

    app = QApplication([])
    w = QWidget()
    layout = QVBoxLayout(w)
    button = QPushButton("Click me")
    button.clicked.connect(lambda: print("clicked"))
    layout.addWidget(button)
    w.show()
    app.exec()

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

import os
import time
from typing import Callable, List, Optional

import buipy

from bui import _realtk, filebrowser, host
from bui.components.core import (EV_ACTIVATE, EV_CHANGE, EV_CLICK, EV_DOUBLE, EV_ENTER,  # noqa: F401
                                 EV_KEY, EV_LEAVE, EV_MOTION, EV_PRESS, EV_RELEASE,
                                 EV_RENAME, EV_SUBMIT, EV_TOGGLE, EV_WHEEL)
from bui.components.menus import MenuItem
from bui.tksyntax import PhotoImage, color as _tk_color

PACK, GRID, PLACE = 1, 2, 3
N, S, W, E = 1, 2, 4, 8


# ==========================================================================
# Namespace and value types


class Qt:
    AlignLeft, AlignRight, AlignHCenter, AlignJustify = 0x1, 0x2, 0x4, 0x8
    AlignTop, AlignBottom, AlignVCenter = 0x20, 0x40, 0x80
    AlignCenter = AlignHCenter | AlignVCenter
    Horizontal, Vertical = 1, 2
    Unchecked, PartiallyChecked, Checked = 0, 1, 2
    NoPen, NoBrush = "nopen", "nobrush"
    LeftButton, RightButton, MiddleButton = 1, 2, 4
    NoModifier, ShiftModifier, ControlModifier, AltModifier = 0, 0x02000000, 0x04000000, \
        0x08000000
    Key_Return, Key_Enter, Key_Escape, Key_Tab, Key_Backspace, Key_Delete = \
        0x01000004, 0x01000005, 0x01000000, 0x01000001, 0x01000003, 0x01000007
    Key_Left, Key_Up, Key_Right, Key_Down, Key_Home, Key_End = \
        0x01000012, 0x01000013, 0x01000014, 0x01000015, 0x01000010, 0x01000011
    Key_Space = 0x20
    WA_DeleteOnClose = 55
    ScrollBarAlwaysOff, ScrollBarAsNeeded = 1, 0
    ItemIsEditable, ItemIsSelectable, ItemIsEnabled = 2, 1, 32
    DisplayRole, UserRole = 0, 256
    white, black, red, green, blue, gray = "#ffffff", "#000000", "#ff0000", "#00ff00", \
        "#0000ff", "#808080"


for _c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    setattr(Qt, "Key_" + _c, ord(_c))
for _d in range(10):
    setattr(Qt, "Key_%d" % _d, ord(str(_d)))

_KEYSYM_TO_QT = {"Return": Qt.Key_Return, "KP_Enter": Qt.Key_Enter, "Escape": Qt.Key_Escape,
                 "Tab": Qt.Key_Tab, "BackSpace": Qt.Key_Backspace, "Delete": Qt.Key_Delete,
                 "Left": Qt.Key_Left, "Up": Qt.Key_Up, "Right": Qt.Key_Right,
                 "Down": Qt.Key_Down, "Home": Qt.Key_Home, "End": Qt.Key_End,
                 "space": Qt.Key_Space}


class Signal:
    """Qt signal: ``clicked = Signal()`` on a class, then
    ``obj.clicked.connect(fn)`` / ``.emit(...)``."""

    def __init__(self, *types, name=None):
        self._types = types
        self._name = None

    def __set_name__(self, owner, name):
        self._name = "_sig_" + name

    def __get__(self, obj, owner=None):
        if obj is None:
            return self
        bound = obj.__dict__.get(self._name)
        if bound is None:
            bound = BoundSignal(self._types)
            obj.__dict__[self._name] = bound
        return bound


Signal_ = pyqtSignal = Signal


class BoundSignal:
    def __init__(self, types=()):
        self._slots: List[Callable] = []
        self._types = types

    def connect(self, slot):
        self._slots.append(slot)
        return slot

    def disconnect(self, slot=None):
        if slot is None:
            self._slots.clear()
        elif slot in self._slots:
            self._slots.remove(slot)

    def emit(self, *args):
        for slot in list(self._slots):
            try:
                slot(*args)
            except TypeError:
                # Qt drops extra signal arguments a slot does not take
                if args:
                    slot(*args[:max(0, _arity(slot))])
                else:
                    raise


def _arity(fn) -> int:
    import inspect
    try:
        return len([p for p in inspect.signature(fn).parameters.values()
                    if p.default is p.empty and p.kind in (p.POSITIONAL_ONLY,
                                                           p.POSITIONAL_OR_KEYWORD)])
    except (TypeError, ValueError):
        return 0


class QColor:
    def __init__(self, *args):
        if len(args) == 1 and isinstance(args[0], QColor):
            self._rgba = args[0]._rgba
        elif len(args) == 1:
            c = _tk_color(args[0]) or (0, 0, 0, 1)
            self._rgba = tuple(c)
        elif len(args) >= 3:
            a = args[3] if len(args) > 3 else 255
            self._rgba = (args[0] / 255.0, args[1] / 255.0, args[2] / 255.0, a / 255.0)
        else:
            self._rgba = (0.0, 0.0, 0.0, 1.0)

    @staticmethod
    def fromRgb(r, g, b, a=255):  # noqa: N802
        return QColor(r, g, b, a)

    def red(self):
        return round(self._rgba[0] * 255)

    def green(self):
        return round(self._rgba[1] * 255)

    def blue(self):
        return round(self._rgba[2] * 255)

    def alpha(self):
        return round(self._rgba[3] * 255)

    def setAlpha(self, a):  # noqa: N802
        self._rgba = self._rgba[:3] + (a / 255.0,)

    def name(self):
        return "#%02x%02x%02x" % (self.red(), self.green(), self.blue())

    def lighter(self, f=150):
        k = f / 100.0
        return QColor(*(min(255, round(c * 255 * k)) for c in self._rgba[:3]), self.alpha())

    def darker(self, f=200):
        k = 100.0 / f
        return QColor(*(round(c * 255 * k) for c in self._rgba[:3]), self.alpha())

    def rgba(self):
        return self._rgba


def _rgba(c):
    if c is None or c in (Qt.NoPen, Qt.NoBrush):
        return None
    if isinstance(c, QColor):
        return c._rgba
    if isinstance(c, (QPen, QBrush)):
        return _rgba(c._color)
    return QColor(c)._rgba


class QPen:
    def __init__(self, color=None, width=1, *args):
        self._color = color if color is not None else QColor("#000000")
        self._width = width

    def setWidth(self, w):  # noqa: N802
        self._width = w

    def setColor(self, c):  # noqa: N802
        self._color = c

    def color(self):
        return QColor(self._color) if not isinstance(self._color, QColor) else self._color


class QBrush:
    def __init__(self, color=None, *args):
        self._color = color

    def color(self):
        return QColor(self._color) if not isinstance(self._color, QColor) else self._color


class QFont:
    Bold, Normal = 75, 50

    def __init__(self, family="", size=-1, weight=-1, italic=False):
        self._family, self._size, self._bold = family, size, weight >= 75

    def setPointSize(self, s):  # noqa: N802
        self._size = s

    def pointSize(self):  # noqa: N802
        return self._size

    def setPixelSize(self, s):  # noqa: N802
        self._size = s * 0.75

    def setBold(self, b):  # noqa: N802
        self._bold = bool(b)

    def bold(self):
        return self._bold

    def setFamily(self, f):  # noqa: N802
        self._family = f


class QPoint:
    def __init__(self, x=0, y=0):
        self._x, self._y = int(x), int(y)

    def x(self):
        return self._x

    def y(self):
        return self._y


QPointF = QPoint


class QSize:
    def __init__(self, w=0, h=0):
        self._w, self._h = int(w), int(h)

    def width(self):
        return self._w

    def height(self):
        return self._h


class QRect:
    def __init__(self, x=0, y=0, w=0, h=0):
        self._x, self._y, self._w, self._h = int(x), int(y), int(w), int(h)

    def x(self):
        return self._x

    def y(self):
        return self._y

    def width(self):
        return self._w

    def height(self):
        return self._h

    left, top = x, y

    def right(self):
        return self._x + self._w - 1

    def bottom(self):
        return self._y + self._h - 1

    def center(self):
        return QPoint(self._x + self._w // 2, self._y + self._h // 2)

    def contains(self, p, y=None):
        px, py = (p.x(), p.y()) if y is None else (p, y)
        return self._x <= px < self._x + self._w and self._y <= py < self._y + self._h

    def adjusted(self, dx1, dy1, dx2, dy2):
        return QRect(self._x + dx1, self._y + dy1, self._w - dx1 + dx2, self._h - dy1 + dy2)


QRectF = QRect


class QPixmap:
    """Image backed by RGBA pixels (shared with tksyntax.PhotoImage)."""

    def __init__(self, *args):
        if args and isinstance(args[0], str):
            self._img = PhotoImage(file=args[0])
        elif len(args) >= 2:
            self._img = PhotoImage(width=int(args[0]), height=int(args[1]))
        elif args and isinstance(args[0], QSize):
            self._img = PhotoImage(width=args[0].width(), height=args[0].height())
        else:
            self._img = PhotoImage(width=0, height=0)

    def width(self):
        return self._img.width()

    def height(self):
        return self._img.height()

    def isNull(self):  # noqa: N802
        return self._img.width() == 0

    def fill(self, c=None):
        rgba = bytes(round(v * 255) for v in (_rgba(c) or (1, 1, 1, 1)))
        self._img._px = bytearray(rgba * (self._img.width() * self._img.height()))

    def load(self, path):
        self._img = PhotoImage(file=path)
        return True


QImage = QPixmap


class QIcon:
    def __init__(self, *args):
        self.name = args[0] if args else ""


# ==========================================================================
# Events


class QEvent:
    def __init__(self, kind=0):
        self._type, self._accepted = kind, True

    def type(self):
        return self._type

    def accept(self):
        self._accepted = True

    def ignore(self):
        self._accepted = False

    def isAccepted(self):  # noqa: N802
        return self._accepted


class QCloseEvent(QEvent):
    pass


class QPaintEvent(QEvent):
    def __init__(self, rect):
        super().__init__()
        self._rect = rect

    def rect(self):
        return self._rect


class QMouseEvent(QEvent):
    def __init__(self, x, y, button=Qt.LeftButton, modifiers=0):
        super().__init__()
        self._x, self._y, self._button, self._mods = int(x), int(y), button, modifiers

    def pos(self):
        return QPoint(self._x, self._y)

    position = pos

    def x(self):
        return self._x

    def y(self):
        return self._y

    def button(self):
        return self._button

    def modifiers(self):
        return self._mods


class QKeyEvent(QEvent):
    def __init__(self, key, text="", modifiers=0):
        super().__init__()
        self._key, self._text, self._mods = key, text, modifiers

    def key(self):
        return self._key

    def text(self):
        return self._text

    def modifiers(self):
        return self._mods


class QWheelEvent(QEvent):
    def __init__(self, delta):
        super().__init__()
        self._delta = delta

    def angleDelta(self):  # noqa: N802
        return QPoint(0, self._delta)


def _mods_qt(bits):
    return ((Qt.ShiftModifier if bits & 1 else 0) | (Qt.ControlModifier if bits & 2 else 0)
            | (Qt.AltModifier if bits & 4 else 0))


# ==========================================================================
# QApplication, timers


class QApplication:
    _instance: Optional["QApplication"] = None

    def __init__(self, argv=None):
        QApplication._instance = self
        self._windows: list = []

    @staticmethod
    def instance():
        return QApplication._instance

    def exec(self) -> int:
        for w in list(_pending_show):
            w._open()
        _pending_show.clear()
        _timers.flush()
        host.run()
        return 0

    exec_ = exec

    def quit(self) -> None:
        host.quit()

    exit = quit

    @staticmethod
    def processEvents():  # noqa: N802
        _timers.flush()
        host.update()

    def setStyle(self, *a):  # noqa: N802
        pass

    setStyleSheet = setApplicationName = setStyle


QCoreApplication = QApplication


class _TimerService:
    """Timers run on a top-level window's clock; until one exists they wait."""

    def __init__(self):
        self.queue = []

    def window(self):
        for w in host._windows:
            if w.is_open:
                return w
        return None

    def after(self, ms, fn):
        win = self.window()
        if win is None:
            self.queue.append((ms, fn))
            return ("q", fn)
        return ("w", win, win.after(int(ms), fn))

    def cancel(self, token):
        if token is None:
            return
        if token[0] == "q":
            self.queue = [(m, f) for m, f in self.queue if f is not token[1]]
        else:
            try:
                token[1].after_cancel(token[2])
            except Exception:
                pass

    def flush(self):
        win = self.window()
        if win is None:
            return
        queue, self.queue = self.queue, []
        for ms, fn in queue:
            win.after(int(ms), fn)


_timers = _TimerService()


class QTimer:
    timeout = Signal()

    def __init__(self, parent=None):
        self._interval, self._single, self._token = 0, False, None

    @staticmethod
    def singleShot(ms, fn):  # noqa: N802
        _timers.after(ms, fn)

    def setInterval(self, ms):  # noqa: N802
        self._interval = int(ms)

    def interval(self):
        return self._interval

    def setSingleShot(self, b):  # noqa: N802
        self._single = bool(b)

    def isActive(self):  # noqa: N802
        return self._token is not None

    def start(self, ms=None):
        if ms is not None:
            self._interval = int(ms)
        self.stop()
        self._token = _timers.after(self._interval, self._fire)

    def stop(self):
        _timers.cancel(self._token)
        self._token = None

    def _fire(self):
        self._token = None
        if not self._single:
            self._token = _timers.after(max(1, self._interval), self._fire)
        self.timeout.emit()


# Qt's animation framework runs on BUI's animation engine (bui/qtanim.py: the classes need the
# value types and Signal above).
from bui.qtanim import (QAbstractAnimation, QAnimationGroup, QEasingCurve,  # noqa: E402,F401
                        QParallelAnimationGroup, QPauseAnimation, QPropertyAnimation,
                        QSequentialAnimationGroup, QVariantAnimation)


# ==========================================================================
# QWidget

_pending_show: list = []


def _kids(node):
    return buipy.KitNode.children.__get__(node)

_EVENT_METHODS = ("mousePressEvent", "mouseReleaseEvent", "mouseMoveEvent",
                  "mouseDoubleClickEvent", "keyPressEvent", "wheelEvent", "enterEvent",
                  "leaveEvent")


class QWidget(buipy.KitNode):
    KIND = "frame"

    def __init__(self, parent=None, *args, **kw):
        cls = type(self)
        custom_paint = cls.paintEvent is not QWidget.paintEvent
        super().__init__("canvas" if custom_paint and self.KIND == "frame" else self.KIND)
        self._custom_paint = custom_paint
        self._layout = None
        self._title = ""
        self._size = None
        self._window = None
        self._ctx = None
        self._policy = None
        self._menubar = None
        self._painted_size = None
        if custom_paint:
            self.set("bg", (0, 0, 0, 0))
        if any(getattr(cls, m) is not getattr(QWidget, m) for m in _EVENT_METHODS):
            self.set("events", True)
        self._setup()
        if parent is not None:
            self.setParent(parent)

    def _setup(self):
        pass

    @property
    def _parent_widget(self):
        p = buipy.KitNode.parent.__get__(self)
        while p is not None and not isinstance(p, QWidget):
            p = buipy.KitNode.parent.__get__(p)
        return p

    @property
    def _r(self):
        return buipy.KitNode.rect.__get__(self)

    # -- tree ----------------------------------------------------------------------
    def setParent(self, parent):  # noqa: N802
        if self.parent is not None:
            self.parent.remove(self)
        if parent is not None:
            target = parent._content_node()
            target.add(self)
            if target.type == "frame" and self.geom_get("manager") == 0:
                self.geom(PLACE, reset_place=1, x=0, y=0)  # free child: placed at (0, 0)

    def _content_node(self):
        return self

    def parentWidget(self):  # noqa: N802
        return self._parent_widget

    def children(self):
        return [c for c in _kids(self) if isinstance(c, QWidget)]

    def setLayout(self, layout):  # noqa: N802
        if self._layout is not None:
            self.remove(self._layout)
        self._layout = layout
        layout._owner = self
        if layout._margins is None:  # top-level layouts get Qt's default margin
            layout.set("padding", round(0.45 * buipy.kit_unit()))
        self._content_node().add(layout)
        layout.geom(PACK, fill=3, expand=1)

    def layout(self):
        return self._layout

    # -- geometry --------------------------------------------------------------------
    def resize(self, w, h=None):
        if h is None:
            w, h = w.width(), w.height()
        self._size = (int(w), int(h))
        if self._window is not None:
            self._window.set_size(*self._size)
        elif self.geom_get("manager") == PLACE:
            self.geom(PLACE, width=int(w), height=int(h))
        else:
            self.geom(int(self.geom_get("manager")), reqwidth=int(w), reqheight=int(h))

    def move(self, x, y=None):
        if y is None:
            x, y = x.x(), x.y()
        if self.geom_get("manager") in (0, PLACE):
            self.geom(PLACE, x=int(x), y=int(y))

    def setGeometry(self, x, y=None, w=None, h=None):  # noqa: N802
        if isinstance(x, QRect):
            x, y, w, h = x.x(), x.y(), x.width(), x.height()
        self.geom(PLACE, x=int(x), y=int(y), width=int(w), height=int(h))
        self._size = (int(w), int(h))

    def setFixedSize(self, w, h=None):  # noqa: N802
        if h is None:
            w, h = w.width(), w.height()
        self.geom(int(self.geom_get("manager")), reqwidth=int(w), reqheight=int(h))
        self._size = (int(w), int(h))
        self._fixed(0)
        self._fixed(1)

    def setFixedWidth(self, w):  # noqa: N802
        self.geom(int(self.geom_get("manager")), reqwidth=int(w))
        self._fixed(0)

    def setFixedHeight(self, h):  # noqa: N802
        self.geom(int(self.geom_get("manager")), reqheight=int(h))
        self._fixed(1)

    def _fixed(self, axis):
        p = list(self._policy or (None, None))
        p[axis] = QSizePolicy.Fixed
        self._policy = tuple(p)
        lay = self._parent_layout()
        if isinstance(lay, QBoxLayout):
            lay._policies()

    def setMinimumWidth(self, w):  # noqa: N802
        self.geom(int(self.geom_get("manager")), reqwidth=int(w))

    def setMinimumHeight(self, h):  # noqa: N802
        self.geom(int(self.geom_get("manager")), reqheight=int(h))

    def setMinimumSize(self, w, h=None):  # noqa: N802
        pass

    setMaximumSize = setMaximumWidth = setMaximumHeight = setMinimumSize

    def width(self):
        if self._window is not None:
            return self._window.size[0]
        return self._r[2] or (self._size or self.natural())[0]

    def height(self):
        if self._window is not None:
            return self._window.size[1]
        return self._r[3] or (self._size or self.natural())[1]

    def x(self):
        return self._r[0]

    def y(self):
        return self._r[1]

    def pos(self):
        return QPoint(self._r[0], self._r[1])

    def size(self):
        return QSize(self.width(), self.height())

    def geometry(self):
        return QRect(*self._r)

    def rect(self):  # noqa: F811 - Qt API (QWidget.rect())
        return QRect(0, 0, self.width(), self.height())

    def sizeHint(self):  # noqa: N802
        return QSize(*self.natural())

    def absolutePos(self):  # noqa: N802 - tools/tests
        r = self._r
        return r[0], r[1]

    def setSizePolicy(self, h, v=None):  # noqa: N802
        self._policy = (h._h, h._v) if isinstance(h, QSizePolicy) else (h, v)
        if isinstance(self._parent_layout(), QBoxLayout):
            self._parent_layout()._policies()

    def _parent_layout(self):
        p = self.parent
        return p if isinstance(p, QLayout) else None

    # -- state ------------------------------------------------------------------------
    def setEnabled(self, b):  # noqa: N802
        self.set("enabled", bool(b))

    def isEnabled(self):  # noqa: N802
        return bool(self.get("enabled"))

    def setVisible(self, b):  # noqa: N802
        if self._parent_widget is None and self._layout is not None or self._window:
            self.show() if b else self.hide()
            return
        self.set("visible", bool(b))

    def isVisible(self):  # noqa: N802
        return bool(self.get("visible"))

    def hide(self):
        if self._window is not None:
            self._window.close()
            self._window = None
        else:
            self.set("visible", False)

    def setToolTip(self, text):  # noqa: N802
        self.set("description", str(text))

    def setStyleSheet(self, css):  # noqa: N802
        for decl in str(css).split(";"):
            k, _, v = decl.partition(":")
            k, v = k.strip().lower(), v.strip()
            if k == "color":
                c = _tk_color(v)
                if c:
                    try:
                        self.set("text_color", c)
                    except KeyError:
                        pass
            elif k in ("background", "background-color"):
                c = _tk_color(v)
                if c and self.type in ("frame", "canvas"):
                    self.set("bg", c)
            elif k == "font-weight" and "bold" in v:
                try:
                    self.set("bold", True)
                except KeyError:
                    pass

    def setFont(self, font):  # noqa: N802
        if isinstance(font, QFont) and font.bold():
            try:
                self.set("bold", True)
            except KeyError:
                pass

    def setObjectName(self, name):  # noqa: N802
        self._object_name = name

    def objectName(self):  # noqa: N802
        return getattr(self, "_object_name", "")

    def setAttribute(self, *a):  # noqa: N802
        pass

    setWindowIcon = setWindowFlags = setFocusPolicy = setContentsMargins = setAttribute
    installEventFilter = setMouseTracking = setAutoFillBackground = setAttribute

    def setFocus(self, *a):  # noqa: N802
        pass

    def setCursor(self, *a):  # noqa: N802
        pass

    # -- window -------------------------------------------------------------------------
    def setWindowTitle(self, title):  # noqa: N802
        self._title = str(title)
        if self._window is not None:
            self._window.title(self._title)

    def windowTitle(self):  # noqa: N802
        return self._title

    def show(self):
        if self._parent_widget is not None:
            self.set("visible", True)
            return
        if QApplication._instance is None or getattr(QApplication, "_running", False) \
                or host._windows:
            self._open()
        else:
            _pending_show.append(self)
            self._open()

    def _open(self):
        if self._window is not None:
            return
        self._ctx = buipy.KitContext(self)
        if self._menubar is not None:
            self._ctx.set_header(self._menubar._node)
        size = self._size or tuple(max(a, b) for a, b in zip(self.natural(), (200, 100)))
        self._window = host.Window(self._ctx, self._title or "BUI", size)
        self._window.on_close = self._close_requested
        self._window.show()
        _timers.flush()
        _schedule_paint()

    def _close_requested(self):
        ev = QCloseEvent()
        self.closeEvent(ev)
        if ev.isAccepted():
            self._window = None
            return True
        return False

    def close(self):
        if self._window is not None:
            w, self._window = self._window, None
            w.close()
        return True

    def isWindow(self):  # noqa: N802
        return self._parent_widget is None

    def window(self):
        w = self
        while w._parent_widget is not None:
            w = w._parent_widget
        return w

    def _host_window(self):
        return self.window()._window

    def setMenuBar(self, menubar):  # noqa: N802
        self._menubar = menubar
        if self._ctx is not None:
            self._ctx.set_header(menubar._node)

    # -- painting --------------------------------------------------------------------
    def paintEvent(self, event):  # noqa: N802
        pass

    def update(self, *args):
        if self._custom_paint:
            self._repaint()
        else:
            self.invalidate()

    repaint = update

    def _repaint(self):
        w, h = self._r[2], self._r[3]
        if w <= 0 or h <= 0:
            return
        self._painted_size = (w, h)
        self.canvas_delete(-1)
        self._paint_id = 0
        self.paintEvent(QPaintEvent(QRect(0, 0, w, h)))

    def closeEvent(self, event):  # noqa: N802
        event.accept()

    # -- input overrides ----------------------------------------------------------------
    def mousePressEvent(self, e):  # noqa: N802
        pass

    def mouseReleaseEvent(self, e):  # noqa: N802
        pass

    def mouseMoveEvent(self, e):  # noqa: N802
        pass

    def mouseDoubleClickEvent(self, e):  # noqa: N802
        pass

    def keyPressEvent(self, e):  # noqa: N802
        pass

    def wheelEvent(self, e):  # noqa: N802
        pass

    def enterEvent(self, e):  # noqa: N802
        pass

    def leaveEvent(self, e):  # noqa: N802
        pass

    def _on_event(self, kind, ref, a, b):
        if kind == EV_PRESS:
            self.mousePressEvent(QMouseEvent(a, b))
        elif kind == EV_RELEASE:
            self.mouseReleaseEvent(QMouseEvent(a, b))
        elif kind == EV_MOTION:
            self.mouseMoveEvent(QMouseEvent(a, b))
        elif kind == EV_DOUBLE:
            self.mouseDoubleClickEvent(QMouseEvent(a, b))
        elif kind == EV_ENTER:
            self.enterEvent(QEvent())
        elif kind == EV_LEAVE:
            self.leaveEvent(QEvent())
        elif kind == EV_WHEEL:
            self.wheelEvent(QWheelEvent(int(a) * 120))
        elif kind == EV_KEY:
            text, _, keysym = (ref or "").partition("\t")
            key = _KEYSYM_TO_QT.get(keysym, ord(keysym.upper()) if len(keysym) == 1 else 0)
            self.keyPressEvent(QKeyEvent(key, text, _mods_qt(int(a))))
        else:
            self._event(kind, ref, a, b)

    def _event(self, kind, ref, a, b):
        pass


def _schedule_paint():
    """Custom-painted widgets paint once their size is known (after layout)."""
    _timers.after(0, _paint_pending)


def _paint_pending():
    for w in list(host._windows):
        w.render_now()
    for w in _custom_widgets():
        if w._painted_size != (w._r[2], w._r[3]):
            w._repaint()


def _custom_widgets():
    import gc
    return [o for o in gc.get_objects() if isinstance(o, QWidget) and o._custom_paint]


class QMainWindow(QWidget):
    def _setup(self):
        self._central = None
        self._status = None

    def setCentralWidget(self, w):  # noqa: N802
        if self._central is not None:
            self.remove(self._central)
        self._central = w
        self.add(w)
        w.geom(PACK, fill=3, expand=1)

    def centralWidget(self):  # noqa: N802
        return self._central

    def menuBar(self):  # noqa: N802
        if self._menubar is None:
            self.setMenuBar(QMenuBar())
        return self._menubar

    def statusBar(self):  # noqa: N802
        if self._status is None:
            self._status = QStatusBar()
            self.add(self._status)
            self._status.geom(PACK, side=1, fill=1)
        return self._status


class QDialog(QWidget):
    accepted = Signal()
    rejected = Signal()

    def exec(self):
        self.show()
        while self._window is not None and self._window.is_open:
            host.update()
            time.sleep(0.004)
        return getattr(self, "_result", 0)

    exec_ = exec

    def accept(self):
        self._result = 1
        self.accepted.emit()
        self.close()

    def reject(self):
        self._result = 0
        self.rejected.emit()
        self.close()


class QFrame(QWidget):
    NoFrame, Box, Panel, StyledPanel, HLine, VLine = range(6)

    def setFrameShape(self, *a):  # noqa: N802
        pass

    setFrameShadow = setLineWidth = setFrameShape


# ==========================================================================
# Layouts


class QLayout(buipy.KitNode):
    def __init__(self, parent=None):
        super().__init__("frame")
        self._owner = None
        self._items = []
        self._spacing = round(0.3 * buipy.kit_unit())
        self._margins = None
        if isinstance(parent, QWidget):
            parent.setLayout(self)

    def setSpacing(self, s):  # noqa: N802
        self._spacing = int(s)
        self._relayout()

    def spacing(self):
        return self._spacing

    def setContentsMargins(self, left, top=None, right=None, bottom=None):  # noqa: N802
        if isinstance(left, QMargins):
            left = left._v
        self._margins = int(left)
        self.set("padding", int(left))

    def count(self):
        return len(self._items)

    def itemAt(self, i):  # noqa: N802
        return _LayoutItem(self._items[i][0]) if 0 <= i < len(self._items) else None

    def _relayout(self):
        pass

    def _adopt(self, w):
        if isinstance(w, QWidget):
            if w.parent is not None:
                w.parent.remove(w)
        self.add(w)


class _LayoutItem:
    def __init__(self, w):
        self._w = w

    def widget(self):
        return self._w if isinstance(self._w, QWidget) else None

    def layout(self):
        return self._w if isinstance(self._w, QLayout) else None


# Size policies (Qt defaults): which widgets take extra space.
_H_EXPANDING = ("text", "slider", "progress", "dropdown", "textarea", "list", "tree", "tabs",
                "canvas", "scroll", "number")
_V_EXPANDING = ("textarea", "list", "tree", "tabs", "canvas", "scroll")


def _expands(node, horizontal):
    if isinstance(node, QLayout):
        return any(_expands(c, horizontal) for c, _ in node._items)
    if isinstance(node, QWidget) and node._policy is not None             and node._policy[0 if horizontal else 1] is not None:
        p = node._policy[0 if horizontal else 1]
        return p in (QSizePolicy.Expanding, QSizePolicy.MinimumExpanding)
    kinds = _H_EXPANDING if horizontal else _V_EXPANDING
    if isinstance(node, QWidget) and node._layout is not None:
        return _expands(node._layout, horizontal)
    return getattr(node, "type", "") in kinds or (
        isinstance(node, QWidget) and node._custom_paint and not horizontal)


class QMargins:
    def __init__(self, left=0, top=0, right=0, bottom=0):
        self._v = left


class QSizePolicy:
    Fixed, Minimum, Maximum, Preferred, Expanding, MinimumExpanding, Ignored = range(7)

    def __init__(self, h=3, v=3):
        self._h, self._v = h, v


class QBoxLayout(QLayout):
    LeftToRight, RightToLeft, TopToBottom, BottomToTop = range(4)
    _vertical = True

    def addWidget(self, w, stretch=0, alignment=0):  # noqa: N802
        self._adopt(w)
        self._items.append((w, stretch))
        self._policies()

    def addLayout(self, layout, stretch=0):  # noqa: N802
        layout._owner = self._owner
        self.add(layout)
        self._items.append((layout, stretch))
        self._policies()

    def insertWidget(self, index, w, stretch=0, alignment=0):  # noqa: N802
        self._adopt(w)
        self._items.insert(index, (w, stretch))
        self._reorder()
        self._policies()

    def addStretch(self, stretch=1):  # noqa: N802
        spacer = buipy.KitNode("frame")
        self.add(spacer)
        self._items.append((spacer, max(1, stretch)))
        self._policies()

    def addSpacing(self, size):  # noqa: N802
        spacer = buipy.KitNode("frame")
        spacer.geom(0, **({"reqheight": int(size)} if self._vertical else
                          {"reqwidth": int(size)}))
        self.add(spacer)
        self._items.append((spacer, 0))
        self._policies()

    def removeWidget(self, w):  # noqa: N802
        self._items = [(i, s) for i, s in self._items if i is not w]
        self.remove(w)
        self._policies()

    def setStretch(self, index, stretch):  # noqa: N802
        w, _ = self._items[index]
        self._items[index] = (w, stretch)
        self._policies()

    def _reorder(self):
        for i, (w, _) in enumerate(self._items):
            buipy.KitNode.add(self, w, i)

    def _relayout(self):
        self._policies()

    def _policies(self):
        """Qt box semantics on Tk's packer: items stack along the axis; extra
        space goes to stretch factors, else to expanding widgets, else (for
        horizontal boxes) is shared by all."""
        v = self._vertical
        any_stretch = any(s > 0 for _, s in self._items)
        expanders = [w for w, s in self._items if (s > 0 if any_stretch else _expands(w, not v))]
        if not v and not expanders:
            expanders = [w for w, _ in self._items]
        for k, (w, s) in enumerate(self._items):
            gap = self._spacing if k else 0
            w.geom(PACK, side=0 if v else 2, fill=1 if v else 2,
                   expand=1 if w in expanders else 0,
                   pady0=gap if v else 0, padx0=0 if v else gap)
            if (v and _expands(w, True)) or not v:
                w.geom(PACK, fill=3)
            if v and w in expanders:
                w.geom(PACK, fill=3)


class QVBoxLayout(QBoxLayout):
    _vertical = True


class QHBoxLayout(QBoxLayout):
    _vertical = False


class QGridLayout(QLayout):
    def addWidget(self, w, row=0, column=0, rowspan=1, columnspan=1, alignment=0):  # noqa: N802
        self._adopt(w)
        self._place(w, row, column, rowspan, columnspan, alignment)

    def addLayout(self, layout, row=0, column=0, rowspan=1, columnspan=1, alignment=0):  # noqa: N802
        layout._owner = self._owner
        self.add(layout)
        self._place(layout, row, column, rowspan, columnspan, alignment)

    def _place(self, w, row, column, rowspan, columnspan, alignment):
        self._items.append((w, (row, column)))
        sticky = W | E | ((N | S) if _expands(w, False) else 0)
        if alignment & Qt.AlignLeft:
            sticky &= ~E
        if alignment & Qt.AlignRight:
            sticky &= ~W
        g = self._spacing  # Qt spacing goes between cells, not around the edges
        w.geom(GRID, row=row, column=column, rowspan=rowspan, columnspan=columnspan,
               sticky=sticky, padx0=g if column else 0, pady0=g if row else 0)
        if _expands(w, True):
            self.grid_weight(1, column + columnspan - 1, 1.0)
        if _expands(w, False):
            self.grid_weight(0, row + rowspan - 1, 1.0)

    def setColumnStretch(self, column, stretch):  # noqa: N802
        self.grid_weight(1, int(column), float(stretch))

    def setRowStretch(self, row, stretch):  # noqa: N802
        self.grid_weight(0, int(row), float(stretch))

    def setHorizontalSpacing(self, s):  # noqa: N802
        self._spacing = int(s)

    setVerticalSpacing = setHorizontalSpacing

    def rowCount(self):  # noqa: N802
        return 1 + max((r for _, (r, c) in self._items), default=-1)

    def columnCount(self):  # noqa: N802
        return 1 + max((c for _, (r, c) in self._items), default=-1)


class QFormLayout(QGridLayout):
    def addRow(self, label, field=None):  # noqa: N802
        row = self.rowCount()
        if field is None:
            self.addWidget(label, row, 0, 1, 2)
            return
        if isinstance(label, str):
            label = QLabel(label)
            label.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        self.addWidget(label, row, 0)
        if isinstance(field, QLayout):
            self.addLayout(field, row, 1)
        else:
            self.addWidget(field, row, 1)
        self.setColumnStretch(1, 1)


class QStackedLayout(QLayout):
    def addWidget(self, w):  # noqa: N802
        self._adopt(w)
        self._items.append((w, 0))
        w.geom(PACK, fill=3, expand=1)
        self.setCurrentIndex(getattr(self, "_current", 0))
        return len(self._items) - 1

    def setCurrentIndex(self, i):  # noqa: N802
        self._current = i
        for k, (w, _) in enumerate(self._items):
            w.set("visible", k == i)

    def currentIndex(self):  # noqa: N802
        return getattr(self, "_current", 0)


# ==========================================================================
# Widgets


class QLabel(QWidget):
    KIND = "label"

    def __init__(self, text="", parent=None, **kw):
        super().__init__(parent)
        self.set("text", str(text))

    def setText(self, t):  # noqa: N802
        self.set("text", str(t))

    def text(self):
        return self.get_str("text")

    def setAlignment(self, a):  # noqa: N802
        if isinstance(a, str):
            a = {"center": Qt.AlignCenter, "right": Qt.AlignRight}.get(a, Qt.AlignLeft)
        self.set("text_align", "right" if a & Qt.AlignRight else
                 ("center" if a & Qt.AlignHCenter else "left"))

    def setPixmap(self, pm):  # noqa: N802
        self.set_image(pm.width(), pm.height(), pm._img.rgba())

    def setWordWrap(self, b):  # noqa: N802
        pass

    setTextFormat = setOpenExternalLinks = setWordWrap


class QAbstractButton(QWidget):
    clicked = Signal(bool)
    pressed = Signal()
    released = Signal()
    toggled = Signal(bool)

    def setText(self, t):  # noqa: N802
        self.set("text", str(t))

    def text(self):
        return self.get_str("text")

    def setIcon(self, icon):  # noqa: N802
        name = getattr(icon, "name", "")
        if name:
            self.set("icon", name)

    def click(self):
        self._event(EV_CLICK, None, 0, 0)

    def animateClick(self, *a):  # noqa: N802
        self.click()

    def setShortcut(self, *a):  # noqa: N802
        pass


class QPushButton(QAbstractButton):
    KIND = "button"

    def __init__(self, text="", parent=None, **kw):
        if isinstance(text, QWidget):
            text, parent = "", text
        super().__init__(parent)
        self.set("text", str(text))

    def _event(self, kind, ref, a, b):
        if kind == EV_CLICK:
            self.clicked.emit(False)

    def setDefault(self, b):  # noqa: N802
        pass

    setAutoDefault = setFlat = setDefault


QToolButton = QPushButton


class QCheckBox(QAbstractButton):
    KIND = "checkbox"
    stateChanged = Signal(int)
    checkStateChanged = Signal(int)

    def __init__(self, text="", parent=None):
        super().__init__(parent)
        self.set("label", str(text))

    def setText(self, t):  # noqa: N802
        self.set("label", str(t))

    def text(self):
        return self.get_str("label")

    def isChecked(self):  # noqa: N802
        return self.get("value") != 0

    def setChecked(self, b):  # noqa: N802
        self.set("set_value", 1.0 if b else 0.0)

    def checkState(self):  # noqa: N802
        return Qt.Checked if self.isChecked() else Qt.Unchecked

    def setCheckState(self, s):  # noqa: N802
        self.setChecked(s == Qt.Checked or s is True)

    def toggle(self):
        self.setChecked(not self.isChecked())

    def click(self):
        self.toggle()

    def _event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            on = self.isChecked()
            self.toggled.emit(on)
            self.stateChanged.emit(Qt.Checked if on else Qt.Unchecked)
            self.checkStateChanged.emit(Qt.Checked if on else Qt.Unchecked)
            self.clicked.emit(on)


class QRadioButton(QAbstractButton):
    KIND = "radio"

    def __init__(self, text="", parent=None):
        super().__init__(parent)
        self.set("label", str(text))
        self._group = None

    def setText(self, t):  # noqa: N802
        self.set("label", str(t))

    def text(self):
        return self.get_str("label")

    def isChecked(self):  # noqa: N802
        return self.get("value") != 0

    def setChecked(self, b):  # noqa: N802
        if bool(b) != self.isChecked():
            self.set("set_value", 1.0 if b else 0.0)

    def click(self):
        self.setChecked(True)

    def _siblings(self):
        if self._group is not None:
            return [r for r in self._group._buttons if r is not self]
        container = self.parent
        if container is None:
            return []
        return [c for c in _kids(container) if isinstance(c, QRadioButton) and c is not self]

    def _event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            on = self.isChecked()
            if on:  # Qt auto-exclusive: siblings turn off
                for r in self._siblings():
                    if r.isChecked():
                        r.set("set_value", 0.0)
            self.toggled.emit(on)
            if on:
                self.clicked.emit(True)


class QButtonGroup:
    buttonClicked = Signal(object)
    idClicked = Signal(int)

    def __init__(self, parent=None):
        self._buttons = []

    def addButton(self, b, id=-1):  # noqa: N802, A002
        self._buttons.append(b)
        b._group = self
        b.clicked.connect(lambda *a, b=b: (self.buttonClicked.emit(b),
                                           self.idClicked.emit(self._buttons.index(b))))

    def buttons(self):
        return list(self._buttons)

    def checkedButton(self):  # noqa: N802
        for b in self._buttons:
            if b.isChecked():
                return b
        return None


class QLineEdit(QWidget):
    KIND = "text"
    textChanged = Signal(str)
    textEdited = Signal(str)
    returnPressed = Signal()
    editingFinished = Signal()

    def __init__(self, text="", parent=None):
        if isinstance(text, QWidget):
            text, parent = "", text
        super().__init__(parent)
        self.set("live", True)
        self.set("events", True)
        self.set("value", str(text))

    def text(self):
        return self.get_str("value")

    def setText(self, t):  # noqa: N802
        self.set("set_value", str(t))

    def clear(self):
        self.setText("")

    def setPlaceholderText(self, t):  # noqa: N802
        self.set("placeholder", str(t))

    def setReadOnly(self, b):  # noqa: N802
        self.set("enabled", not b)

    def setEchoMode(self, m):  # noqa: N802
        pass

    setMaxLength = setValidator = setClearButtonEnabled = setAlignment = setEchoMode

    def keyPressEvent(self, e):  # noqa: N802
        pass

    def _on_event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            self.textChanged.emit(self.text())
            self.textEdited.emit(self.text())
        elif kind == EV_SUBMIT:
            self.returnPressed.emit()
            self.editingFinished.emit()
        elif kind == EV_KEY:
            QWidget._on_event(self, kind, ref, a, b)


class QTextEdit(QWidget):
    KIND = "textarea"
    textChanged = Signal()

    def __init__(self, text="", parent=None):
        if isinstance(text, QWidget):
            text, parent = "", text
        super().__init__(parent)
        self.set("value", str(text))

    def toPlainText(self):  # noqa: N802
        return self.get_str("value")

    def setPlainText(self, t):  # noqa: N802
        self.set("value", str(t))
        self.textChanged.emit()

    setText = setHtml = setPlainText

    def append(self, t):
        cur = self.toPlainText()
        self.setPlainText(cur + ("\n" if cur else "") + str(t))

    def insertPlainText(self, t):  # noqa: N802
        caret = self.ta_caret()
        self.ta_insert(caret, str(t))
        self.textChanged.emit()

    def clear(self):
        self.setPlainText("")

    def setReadOnly(self, b):  # noqa: N802
        self.set("enabled", not b)

    def setPlaceholderText(self, t):  # noqa: N802
        pass

    setLineWrapMode = setWordWrapMode = setPlaceholderText

    def _event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            self.textChanged.emit()


QPlainTextEdit = QTextEdit


class QAbstractSlider(QWidget):
    valueChanged = Signal(int)
    sliderMoved = Signal(int)
    sliderReleased = Signal()
    rangeChanged = Signal(int, int)

    def _setup(self):
        self.set("integer", True)
        self.set("precision", 0)
        self.set("min", 0.0)
        self.set("max", 99.0)
        self.set("step", 1.0)

    def setRange(self, lo, hi):  # noqa: N802
        self.set("min", float(lo))
        self.set("max", float(hi))
        self.rangeChanged.emit(int(lo), int(hi))

    def setMinimum(self, v):  # noqa: N802
        self.set("min", float(v))

    def setMaximum(self, v):  # noqa: N802
        self.set("max", float(v))

    def minimum(self):
        return int(self.get("min"))

    def maximum(self):
        return int(self.get("max"))

    def value(self):
        return int(round(self.get("value")))

    def setValue(self, v):  # noqa: N802
        self.set("set_value", float(v))

    def setSingleStep(self, s):  # noqa: N802
        self.set("step", float(s))

    def setPageStep(self, s):  # noqa: N802
        pass

    setTickPosition = setTickInterval = setTracking = setPageStep

    def _event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            self.valueChanged.emit(self.value())
            self.sliderMoved.emit(self.value())


class QSlider(QAbstractSlider):
    KIND = "slider"

    def __init__(self, orientation=Qt.Horizontal, parent=None):
        if isinstance(orientation, QWidget):
            orientation, parent = Qt.Horizontal, orientation
        super().__init__(parent)

    def setOrientation(self, o):  # noqa: N802
        pass


class QDial(QSlider):
    pass


class QSpinBox(QAbstractSlider):
    KIND = "number"

    def __init__(self, parent=None):
        super().__init__(parent)
        self._prefix = ""

    def setSuffix(self, s):  # noqa: N802
        self.set("suffix", str(s))

    def setPrefix(self, p):  # noqa: N802
        self.set("label", str(p).rstrip(": "))

    def stepUp(self):  # noqa: N802
        self.setValue(self.value() + int(self.get("step")))

    def stepDown(self):  # noqa: N802
        self.setValue(self.value() - int(self.get("step")))

    def text(self):
        return str(self.value())


class QDoubleSpinBox(QSpinBox):
    valueChanged = Signal(float)

    def _setup(self):
        super()._setup()
        self.set("integer", False)
        self.set("precision", 2)
        self.set("step", 0.1)

    def value(self):
        return self.get("value")

    def setDecimals(self, d):  # noqa: N802
        self.set("precision", int(d))

    def _event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            self.valueChanged.emit(self.value())


class QProgressBar(QWidget):
    KIND = "progress"
    valueChanged = Signal(int)

    def _setup(self):
        self._min, self._max, self._value = 0, 100, 0
        self._format = "%p%"
        self._text_visible = True
        self._update()

    def setRange(self, lo, hi):  # noqa: N802
        self._min, self._max = lo, hi
        self._update()

    def setMinimum(self, v):  # noqa: N802
        self._min = v
        self._update()

    def setMaximum(self, v):  # noqa: N802
        self._max = v
        self._update()

    def setValue(self, v):  # noqa: N802
        self._value = int(v)
        self._update()
        self.valueChanged.emit(self._value)

    def value(self):
        return self._value

    def minimum(self):
        return self._min

    def maximum(self):
        return self._max

    def setFormat(self, f):  # noqa: N802
        self._format = f
        self._update()

    def setTextVisible(self, b):  # noqa: N802
        self._text_visible = bool(b)
        self._update()

    def reset(self):
        self.setValue(self._min)

    def _update(self):
        span = (self._max - self._min) or 1
        frac = max(0.0, min(1.0, (self._value - self._min) / span))
        self.set("factor", frac)
        pct = int(round(frac * 100))
        text = self._format.replace("%p", str(pct)).replace("%v", str(self._value)) \
            .replace("%m", str(self._max))
        self.set("text", text if self._text_visible else "")


class QComboBox(QWidget):
    KIND = "dropdown"
    currentIndexChanged = Signal(int)
    currentTextChanged = Signal(str)
    activated = Signal(int)

    def _setup(self):
        self._items = []
        self._data = []

    def addItem(self, text, data=None):  # noqa: N802
        if isinstance(text, QIcon):
            text, data = data, None
        self._items.append(str(text))
        self._data.append(data)
        self.set("items", self._items)

    def addItems(self, texts):  # noqa: N802
        for t in texts:
            self.addItem(t)

    def insertItem(self, index, text, data=None):  # noqa: N802
        self._items.insert(index, str(text))
        self._data.insert(index, data)
        self.set("items", self._items)

    def removeItem(self, index):  # noqa: N802
        del self._items[index]
        del self._data[index]
        self.set("items", self._items)

    def clear(self):
        self._items, self._data = [], []
        self.set("items", [])

    def count(self):
        return len(self._items)

    def itemText(self, i):  # noqa: N802
        return self._items[i]

    def itemData(self, i, role=None):  # noqa: N802
        return self._data[i]

    def currentIndex(self):  # noqa: N802
        return int(self.get("value")) if self._items else -1

    def setCurrentIndex(self, i):  # noqa: N802
        self.set("set_value", float(i))

    def currentText(self):  # noqa: N802
        i = self.currentIndex()
        return self._items[i] if 0 <= i < len(self._items) else ""

    def setCurrentText(self, t):  # noqa: N802
        if t in self._items:
            self.setCurrentIndex(self._items.index(t))

    def currentData(self, role=None):  # noqa: N802
        i = self.currentIndex()
        return self._data[i] if 0 <= i < len(self._data) else None

    def findText(self, t):  # noqa: N802
        return self._items.index(t) if t in self._items else -1

    def setEditable(self, b):  # noqa: N802
        pass

    def _event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            i = self.currentIndex()
            self.currentIndexChanged.emit(i)
            self.currentTextChanged.emit(self.currentText())
            self.activated.emit(i)


class QListWidgetItem:
    def __init__(self, text="", parent=None):
        self._text = str(text)
        self._data = {}
        if isinstance(parent, QListWidget):
            parent.addItem(self)

    def text(self):
        return self._text

    def setText(self, t):  # noqa: N802
        self._text = str(t)
        lw = getattr(self, "_list", None)
        if lw is not None:
            lw._push()

    def setData(self, role, value):  # noqa: N802
        self._data[role] = value

    def data(self, role):
        return self._data.get(role)

    def setFlags(self, f):  # noqa: N802
        pass

    def setIcon(self, *a):  # noqa: N802
        pass


class QListWidget(QWidget):
    KIND = "list"
    currentRowChanged = Signal(int)
    currentItemChanged = Signal(object, object)
    itemClicked = Signal(object)
    itemDoubleClicked = Signal(object)
    itemSelectionChanged = Signal()

    def _setup(self):
        self._items: List[QListWidgetItem] = []
        self.set("rows", 6)
        self.set("events", True)

    def _push(self):
        self.set("items", [i.text() for i in self._items])

    def addItem(self, item):  # noqa: N802
        if not isinstance(item, QListWidgetItem):
            item = QListWidgetItem(item)
        item._list = self
        self._items.append(item)
        self._push()

    def addItems(self, texts):  # noqa: N802
        for t in texts:
            self.addItem(t)

    def insertItem(self, row, item):  # noqa: N802
        if not isinstance(item, QListWidgetItem):
            item = QListWidgetItem(item)
        item._list = self
        self._items.insert(row, item)
        self._push()

    def takeItem(self, row):  # noqa: N802
        it = self._items.pop(row)
        self._push()
        return it

    def clear(self):
        self._items = []
        self._push()

    def count(self):
        return len(self._items)

    def item(self, row):
        return self._items[row] if 0 <= row < len(self._items) else None

    def currentRow(self):  # noqa: N802
        return int(self.get("value")) if self._items else -1

    def setCurrentRow(self, row):  # noqa: N802
        self.set("set_value", float(row))

    def currentItem(self):  # noqa: N802
        return self.item(self.currentRow())

    def row(self, item):
        return self._items.index(item)

    def selectedItems(self):  # noqa: N802
        it = self.currentItem()
        return [it] if it else []

    def _on_event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            self.currentRowChanged.emit(self.currentRow())
            self.currentItemChanged.emit(self.currentItem(), None)
            self.itemSelectionChanged.emit()
        elif kind == EV_PRESS:
            if self.currentItem() is not None:
                self.itemClicked.emit(self.currentItem())
        elif kind == EV_DOUBLE:
            if self.currentItem() is not None:
                self.itemDoubleClicked.emit(self.currentItem())
        elif kind == EV_RENAME:
            self._items[int(a)]._text = self.strs("items")[int(a)]


class QTreeWidgetItem:
    def __init__(self, parent_or_texts=None, texts=None):
        texts_ = texts if texts is not None else (
            parent_or_texts if isinstance(parent_or_texts, (list, tuple)) else [])
        texts_ = [str(t) for t in texts_]
        self._node = buipy.KitTreeItem(texts_[0] if texts_ else "")
        self._node._qitem = self
        self._texts = texts_
        self._children: list = []
        self._parent = None
        self._node.set_values(texts_[1:])
        if isinstance(parent_or_texts, QTreeWidgetItem):
            parent_or_texts.addChild(self)
        elif isinstance(parent_or_texts, QTreeWidget):
            parent_or_texts.addTopLevelItem(self)

    def text(self, col):
        return self._texts[col] if col < len(self._texts) else ""

    def setText(self, col, text):  # noqa: N802
        while len(self._texts) <= col:
            self._texts.append("")
        self._texts[col] = str(text)
        self._node.label = self._texts[0]
        self._node.set_values(self._texts[1:])

    def addChild(self, child):  # noqa: N802
        child._parent = self
        self._children.append(child)
        self._node.add(child._node)

    def child(self, i):
        return self._children[i]

    def childCount(self):  # noqa: N802
        return len(self._children)

    def parent(self):
        return self._parent

    def setExpanded(self, b):  # noqa: N802
        self._node.set("expanded", 1.0 if b else 0.0)

    def isExpanded(self):  # noqa: N802
        return bool(self._node.get("expanded"))

    def setBackground(self, *a):  # noqa: N802
        pass

    setForeground = setFlags = setIcon = setData = setBackground


class QTreeWidget(QWidget):
    KIND = "tree"
    itemClicked = Signal(object, int)
    itemDoubleClicked = Signal(object, int)
    currentItemChanged = Signal(object, object)
    itemSelectionChanged = Signal()

    def _setup(self):
        self._top: List[QTreeWidgetItem] = []
        self._headers = []
        self.set("rows", 8)
        self.set("events", True)

    def setHeaderLabels(self, labels):  # noqa: N802
        self._headers = [str(l) for l in labels]
        self.set("headers", self._headers)

    def setColumnCount(self, n):  # noqa: N802
        pass

    def setHeaderHidden(self, b):  # noqa: N802
        if b:
            self.set("headers", [])

    def addTopLevelItem(self, item):  # noqa: N802
        self._top.append(item)
        self.set_tree_roots([i._node for i in self._top])
        return item

    def addTopLevelItems(self, items):  # noqa: N802
        for i in items:
            self.addTopLevelItem(i)

    def topLevelItem(self, i):  # noqa: N802
        return self._top[i]

    def topLevelItemCount(self):  # noqa: N802
        return len(self._top)

    def clear(self):
        self._top = []
        self.set_tree_roots([])

    def currentItem(self):  # noqa: N802
        n = self.tree_selected()
        return getattr(n, "_qitem", None) if n is not None else None

    def setCurrentItem(self, item):  # noqa: N802
        self.tree_select(item._node if item is not None else None)

    def expandAll(self):  # noqa: N802
        def walk(items):
            for it in items:
                it.setExpanded(True)
                walk(it._children)
        walk(self._top)
        self.invalidate()

    def collapseAll(self):  # noqa: N802
        def walk(items):
            for it in items:
                it.setExpanded(False)
                walk(it._children)
        walk(self._top)
        self.invalidate()

    def selectedItems(self):  # noqa: N802
        it = self.currentItem()
        return [it] if it else []

    def _on_event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            self.currentItemChanged.emit(self.currentItem(), None)
            self.itemSelectionChanged.emit()
        elif kind == EV_PRESS and self.currentItem() is not None:
            self.itemClicked.emit(self.currentItem(), 0)
        elif kind == EV_DOUBLE and self.currentItem() is not None:
            self.itemDoubleClicked.emit(self.currentItem(), 0)


class QTableWidgetItem:
    def __init__(self, text=""):
        self._text = str(text)

    def text(self):
        return self._text

    def setText(self, t):  # noqa: N802
        self._text = str(t)

    def setFlags(self, *a):  # noqa: N802
        pass

    setData = setBackground = setForeground = setTextAlignment = setFlags


class QTableWidget(QWidget):
    """A flat table: one tree row per table row, columns from the headers."""

    KIND = "tree"
    cellClicked = Signal(int, int)
    cellDoubleClicked = Signal(int, int)
    currentCellChanged = Signal(int, int, int, int)
    itemSelectionChanged = Signal()

    def __init__(self, rows=0, columns=0, parent=None):
        if isinstance(rows, QWidget):
            rows, columns, parent = 0, 0, rows
        super().__init__(parent)
        self._rows, self._cols = int(rows), int(columns)
        self._cells = {}
        self._headers = [str(c + 1) for c in range(self._cols)]
        self._row_nodes = []
        self.set("rows", 8)
        self.set("events", True)
        self._rebuild()

    def _rebuild(self):
        self.set("headers", self._headers[:self._cols] or [""])
        self._row_nodes = []
        for r in range(self._rows):
            texts = [self._cells[(r, c)].text() if (r, c) in self._cells else ""
                     for c in range(max(1, self._cols))]
            node = buipy.KitTreeItem(texts[0])
            node.set_values(texts[1:])
            node._row = r
            self._row_nodes.append(node)
        self.set_tree_roots(self._row_nodes)

    def setRowCount(self, n):  # noqa: N802
        self._rows = int(n)
        self._rebuild()

    def setColumnCount(self, n):  # noqa: N802
        self._cols = int(n)
        while len(self._headers) < self._cols:
            self._headers.append(str(len(self._headers) + 1))
        self._rebuild()

    def rowCount(self):  # noqa: N802
        return self._rows

    def columnCount(self):  # noqa: N802
        return self._cols

    def setHorizontalHeaderLabels(self, labels):  # noqa: N802
        self._headers = [str(l) for l in labels]
        self._rebuild()

    def setItem(self, row, col, item):  # noqa: N802
        self._cells[(row, col)] = item
        node = self._row_nodes[row]
        texts = [self._cells[(row, c)].text() if (row, c) in self._cells else ""
                 for c in range(max(1, self._cols))]
        node.label = texts[0]
        node.set_values(texts[1:])
        self.invalidate()

    def item(self, row, col):
        return self._cells.get((row, col))

    def currentRow(self):  # noqa: N802
        n = self.tree_selected()
        return getattr(n, "_row", -1) if n is not None else -1

    def currentColumn(self):  # noqa: N802
        return 0

    def setCurrentCell(self, row, col):  # noqa: N802
        if 0 <= row < len(self._row_nodes):
            self.tree_select(self._row_nodes[row])

    def clear(self):
        self._cells = {}
        self._rebuild()

    def _on_event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            self.itemSelectionChanged.emit()
            self.currentCellChanged.emit(self.currentRow(), 0, -1, -1)
        elif kind == EV_PRESS and self.currentRow() >= 0:
            self.cellClicked.emit(self.currentRow(), 0)
        elif kind == EV_DOUBLE and self.currentRow() >= 0:
            self.cellDoubleClicked.emit(self.currentRow(), 0)


class QTabWidget(QWidget):
    KIND = "tabs"
    currentChanged = Signal(int)

    def _setup(self):
        self._pages, self._titles = [], []

    def addTab(self, w, title):  # noqa: N802
        if w.parent is not None:
            w.parent.remove(w)
        self.add(w)
        self._pages.append(w)
        self._titles.append(str(title))
        self.set("items", self._titles)
        return len(self._pages) - 1

    def currentIndex(self):  # noqa: N802
        return int(self.get("current"))

    def setCurrentIndex(self, i):  # noqa: N802
        self.set("current", float(i))
        self.currentChanged.emit(int(i))

    def count(self):
        return len(self._pages)

    def widget(self, i):
        return self._pages[i]

    def tabText(self, i):  # noqa: N802
        return self._titles[i]

    def setTabText(self, i, t):  # noqa: N802
        self._titles[i] = str(t)
        self.set("items", self._titles)

    def _event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            self.currentChanged.emit(int(a))


class QGroupBox(QWidget):
    """Drawn as a Blender panel (header + body)."""

    def __init__(self, title="", parent=None):
        if isinstance(title, QWidget):
            title, parent = "", title
        super().__init__(parent)
        self.set("text", str(title))
        self.set("padding", round(0.3 * buipy.kit_unit()))

    def setTitle(self, t):  # noqa: N802
        self.set("text", str(t))

    def title(self):
        return self.get_str("text")


class QScrollArea(QWidget):
    KIND = "scroll"

    def _setup(self):
        self.set("rows", 10)

    def setWidget(self, w):  # noqa: N802
        for c in _kids(self):
            self.remove(c)
        self.add(w)

    def setWidgetResizable(self, b):  # noqa: N802
        pass

    setHorizontalScrollBarPolicy = setVerticalScrollBarPolicy = setWidgetResizable


class QStatusBar(QWidget):
    KIND = "label"

    def showMessage(self, text, timeout=0):  # noqa: N802
        self.set("text", str(text))
        if timeout:
            _timers.after(timeout, self.clearMessage)

    def clearMessage(self):  # noqa: N802
        self.set("text", "")

    def currentMessage(self):  # noqa: N802
        return self.get_str("text")


# ==========================================================================
# Menus & actions


class QAction:
    triggered = Signal(bool)
    toggled = Signal(bool)

    def __init__(self, text="", parent=None, triggered=None):
        if isinstance(text, QIcon):
            text = parent if isinstance(parent, str) else ""
        self._text = str(text)
        self._shortcut = ""
        self._checkable = False
        self._checked = False
        self._enabled = True
        if triggered is not None:
            self.triggered.connect(triggered)

    def text(self):
        return self._text

    def setText(self, t):  # noqa: N802
        self._text = str(t)

    def setShortcut(self, s):  # noqa: N802
        self._shortcut = str(s)

    def shortcut(self):
        return self._shortcut

    def setCheckable(self, b):  # noqa: N802
        self._checkable = bool(b)

    def isCheckable(self):  # noqa: N802
        return self._checkable

    def setChecked(self, b):  # noqa: N802
        self._checked = bool(b)
        self.toggled.emit(self._checked)

    def isChecked(self):  # noqa: N802
        return self._checked

    def setEnabled(self, b):  # noqa: N802
        self._enabled = bool(b)

    def isEnabled(self):  # noqa: N802
        return self._enabled

    def trigger(self):
        if self._checkable:
            self.setChecked(not self._checked)
        self.triggered.emit(self._checked)

    def setStatusTip(self, *a):  # noqa: N802
        pass

    setToolTip = setIcon = setStatusTip

    def _item(self):
        return MenuItem(self._text, self.trigger, shortcut=self._shortcut.replace("+", " ")
                        if self._shortcut else None,
                        checked=self._checked if self._checkable else None,
                        enabled=self._enabled)


class QMenu:
    aboutToShow = Signal()
    triggered = Signal(object)

    def __init__(self, title="", parent=None):
        if isinstance(title, QWidget):
            title, parent = "", title
        self._title = str(title)
        self._actions = []
        self._parent = parent

    def title(self):
        return self._title

    def addAction(self, action, slot=None, shortcut=None):  # noqa: N802
        if not isinstance(action, QAction):
            action = QAction(action)
            if slot is not None:
                action.triggered.connect(lambda *a: slot())
            if shortcut:
                action.setShortcut(shortcut)
        self._actions.append(action)
        self._changed()
        return action

    def addMenu(self, menu):  # noqa: N802
        if not isinstance(menu, QMenu):
            menu = QMenu(menu)
        menu._owner_menu = self
        self._actions.append(menu)
        self._changed()
        return menu

    def addSeparator(self):  # noqa: N802
        self._actions.append("-")
        self._changed()

    def actions(self):
        return [a for a in self._actions if isinstance(a, QAction)]

    def clear(self):
        self._actions = []
        self._changed()

    def _items(self):
        out = []
        for a in self._actions:
            if a == "-":
                out.append("-")
            elif isinstance(a, QMenu):
                out.append(MenuItem(a._title, submenu=a._items()))
            else:
                out.append(a._item())
        return out

    def _changed(self):
        bar = getattr(self, "_bar", None) or getattr(getattr(self, "_owner_menu", None),
                                                     "_bar", None)
        if bar is not None:
            bar._rebuild()

    def exec(self, pos=None):
        self.popup(pos)

    exec_ = exec

    def popup(self, pos=None):
        win = self._window_widget()
        if win is None or win._ctx is None:
            return
        at = (pos.x(), pos.y()) if pos is not None else None
        self.aboutToShow.emit()
        win._ctx.open_menu(None, self._items(), None, at)

    def _window_widget(self):
        p = self._parent
        while p is not None and isinstance(p, QWidget) and p._parent_widget is not None:
            p = p._parent_widget
        if p is None:
            for w in _custom_or_top():
                return w
        return p


def _custom_or_top():
    import gc
    return [o for o in gc.get_objects() if isinstance(o, QWidget) and o._window is not None]


class QMenuBar:
    def __init__(self, parent=None):
        self._menus: List[QMenu] = []
        self._node = buipy.KitNode("menubar")

    def addMenu(self, menu):  # noqa: N802
        if not isinstance(menu, QMenu):
            menu = QMenu(menu)
        menu._bar = self
        self._menus.append(menu)
        self._rebuild()
        return menu

    def addAction(self, action):  # noqa: N802
        return action

    def _rebuild(self):
        for c in self._node.children:
            self._node.remove(c)
        for m in self._menus:
            b = buipy.KitNode("menubutton")
            b.set("text", m._title)
            b.set_menu(m._items())
            self._node.add(b)

    def absolutePos(self):  # noqa: N802
        r = self._node.rect
        return r[0], r[1]


# ==========================================================================
# QPainter (records into the widget's kit canvas)


class QPainter:
    Antialiasing = TextAntialiasing = SmoothPixmapTransform = 1

    def __init__(self, device=None):
        self._dev = None
        self._pen = QPen(QColor("#000000"), 1)
        self._brush = None
        self._font = QFont()
        self._opacity = 1.0
        self._dx = self._dy = 0.0
        self._stack = []
        if device is not None:
            self.begin(device)

    def begin(self, device):
        self._dev = device
        return True

    def end(self):
        self._dev = None
        return True

    def isActive(self):  # noqa: N802
        return self._dev is not None

    def setRenderHint(self, *a):  # noqa: N802
        pass

    setRenderHints = setRenderHint

    def save(self):
        self._stack.append((self._pen, self._brush, self._font, self._opacity, self._dx,
                            self._dy))

    def restore(self):
        if self._stack:
            (self._pen, self._brush, self._font, self._opacity, self._dx,
             self._dy) = self._stack.pop()

    def translate(self, dx, dy=None):
        if dy is None:
            dx, dy = dx.x(), dx.y()
        self._dx += dx
        self._dy += dy

    def setPen(self, p):  # noqa: N802
        if p is None or p == Qt.NoPen:
            self._pen = None
        elif isinstance(p, QPen):
            self._pen = p
        else:
            self._pen = QPen(p, 1)

    def setBrush(self, b):  # noqa: N802
        if b is None or b == Qt.NoBrush:
            self._brush = None
        else:
            self._brush = b if isinstance(b, QBrush) else QBrush(b)

    def setFont(self, f):  # noqa: N802
        self._font = f

    def setOpacity(self, o):  # noqa: N802
        self._opacity = float(o)

    def _c(self, c):
        rgba = _rgba(c)
        if rgba is None:
            return None
        return rgba[:3] + (rgba[3] * self._opacity,)

    def _add(self, kind, coords, fill=None, outline=None, width=1.0, text=None, **kw):
        dev = self._dev
        if dev is None:
            return
        dev._paint_id = getattr(dev, "_paint_id", 0) + 1
        xy = [v + (self._dx if i % 2 == 0 else self._dy) for i, v in enumerate(coords)]
        dev.canvas_set(dev._paint_id, kind, xy, fill=fill, outline=outline, width=width,
                       text=text, **kw)

    def _pen_rgba(self):
        return self._c(self._pen._color) if self._pen is not None else None

    def _pen_w(self):
        return float(self._pen._width or 1) if self._pen is not None else 1.0

    def _brush_rgba(self):
        return self._c(self._brush._color) if self._brush is not None else None

    @staticmethod
    def _rect_args(args):
        if len(args) == 1 and isinstance(args[0], QRect):
            r = args[0]
            return r.x(), r.y(), r.width(), r.height()
        return args[:4]

    def fillRect(self, *args):  # noqa: N802
        if isinstance(args[0], QRect):
            x, y, w, h = self._rect_args(args[:1])
            c = args[1]
        else:
            x, y, w, h, c = args[:5]
        self._add(0, [x, y, x + w, y + h], fill=self._c(c.color() if isinstance(c, QBrush)
                                                         else c))

    def drawRect(self, *args):  # noqa: N802
        x, y, w, h = self._rect_args(args)
        self._add(0, [x, y, x + w, y + h], fill=self._brush_rgba(), outline=self._pen_rgba(),
                  width=self._pen_w())

    def drawRoundedRect(self, *args):  # noqa: N802
        if isinstance(args[0], QRect):
            x, y, w, h = self._rect_args(args[:1])
            rx = args[1]
        else:
            x, y, w, h, rx = args[:5]
        self._add(7, [x, y, x + w, y + h], fill=self._brush_rgba(), outline=self._pen_rgba(),
                  width=self._pen_w(), radius=float(rx))

    def drawEllipse(self, *args):  # noqa: N802
        if len(args) == 3 and isinstance(args[0], QPoint):
            c, rx, ry = args
            x, y, w, h = c.x() - rx, c.y() - ry, 2 * rx, 2 * ry
        else:
            x, y, w, h = self._rect_args(args)
        self._add(1, [x, y, x + w, y + h], fill=self._brush_rgba(), outline=self._pen_rgba(),
                  width=self._pen_w())

    def drawLine(self, *args):  # noqa: N802
        if len(args) == 2:
            p1, p2 = args
            args = (p1.x(), p1.y(), p2.x(), p2.y())
        x1, y1, x2, y2 = args
        self._add(2, [x1, y1, x2, y2], outline=self._pen_rgba(), width=self._pen_w())

    def drawPolyline(self, points):  # noqa: N802
        flat = []
        for p in points:
            flat += [p.x(), p.y()] if isinstance(p, QPoint) else list(p)
        self._add(2, flat, outline=self._pen_rgba(), width=self._pen_w())

    def drawPolygon(self, points, *a):  # noqa: N802
        flat = []
        for p in points:
            flat += [p.x(), p.y()] if isinstance(p, QPoint) else list(p)
        self._add(3, flat, fill=self._brush_rgba(), outline=self._pen_rgba(),
                  width=self._pen_w())

    def drawPoint(self, x, y=None):  # noqa: N802
        if y is None:
            x, y = x.x(), x.y()
        self._add(0, [x, y, x + 1, y + 1], fill=self._pen_rgba())

    def _arc(self, kind_fill, args):
        if isinstance(args[0], QRect):
            x, y, w, h = self._rect_args(args[:1])
            start, span = args[1], args[2]
        else:
            x, y, w, h, start, span = args[:6]
        self._add(5, [x, y, x + w, y + h], fill=self._brush_rgba() if kind_fill else None,
                  outline=self._pen_rgba(), width=self._pen_w(), start=start / 16.0,
                  extent=span / 16.0)

    def drawArc(self, *args):  # noqa: N802
        self._arc(False, args)

    def drawPie(self, *args):  # noqa: N802
        self._arc(True, args)

    def drawText(self, *args):  # noqa: N802
        size = float(self._font._size) * 1.33 if self._font._size and self._font._size > 0 \
            else 0.0
        if isinstance(args[0], QRect):
            r, flags, text = args[0], args[1], args[2]
            ax = (r.x() + r.width() / 2 if flags & Qt.AlignHCenter else
                  (r.x() + r.width() if flags & Qt.AlignRight else r.x()))
            ay = (r.y() if flags & Qt.AlignTop else
                  (r.y() + r.height() if flags & Qt.AlignBottom else r.y() + r.height() / 2))
            anchor = (0 if flags & Qt.AlignHCenter else (E if flags & Qt.AlignRight else W)) | \
                (N if flags & Qt.AlignTop else (S if flags & Qt.AlignBottom else 0))
            self._add(4, [ax, ay], fill=self._pen_rgba(), text=str(text), size=size,
                      anchor=anchor)
        else:
            if isinstance(args[0], QPoint):
                x, y, text = args[0].x(), args[0].y(), args[1]
            else:
                x, y, text = args[:3]
            # Qt draws text at its baseline: anchor south-west on the baseline
            self._add(4, [x, y], fill=self._pen_rgba(), text=str(text), size=size,
                      anchor=S | W)

    def drawPixmap(self, *args):  # noqa: N802
        if isinstance(args[0], QPoint):
            x, y, pm = args[0].x(), args[0].y(), args[1]
        else:
            x, y, pm = args[0], args[1], args[2]
        img = pm._img
        self._add(6, [x, y], image=(img.width(), img.height(), img.rgba()), anchor=N | W)

    drawImage = drawPixmap


# ==========================================================================
# Dialog stubs (no blocking native dialogs)


class QMessageBox:
    Ok, Cancel, Yes, No = 0x400, 0x400000, 0x4000, 0x10000

    @staticmethod
    def information(parent, title, text, *a, **kw):
        return QMessageBox.Ok

    warning = critical = about = information

    @staticmethod
    def question(parent, title, text, *a, **kw):
        return QMessageBox.Yes


class QFileDialog:
    @staticmethod
    def _blender(kind, caption, directory, filter, multiple=False):  # noqa: A002
        """Blender's file browser (bui.filebrowser); None when BUI_FILEDIALOG=native."""
        if not filebrowser.enabled():
            return None
        initialdir, initialfile = (directory, None) if not directory or os.path.isdir(
            directory) else os.path.split(directory)
        result = filebrowser.ask(kind, title=caption or None, initialdir=initialdir or None,
                                 initialfile=initialfile, multiple=multiple,
                                 filters=filebrowser.qt_filters(filter))
        if result is None:
            return [] if multiple else ""
        return result

    @staticmethod
    def getOpenFileNames(parent=None, caption="", directory="", filter="", *a):  # noqa: N802,A002
        paths = QFileDialog._blender("open", caption, directory, filter, multiple=True)
        return (paths or [], "")

    @staticmethod
    def getOpenFileName(parent=None, caption="", directory="", filter="", *a):  # noqa: N802,A002
        path = QFileDialog._blender("open", caption, directory, filter)
        if path is not None:
            return path, ""
        try:
            filedialog = _realtk.module("tkinter.filedialog")
            return filedialog.askopenfilename(title=caption), ""
        except Exception:
            return "", ""

    @staticmethod
    def getSaveFileName(parent=None, caption="", directory="", filter="", *a):  # noqa: N802,A002
        path = QFileDialog._blender("save", caption, directory, filter)
        if path is not None:
            return path, ""
        try:
            filedialog = _realtk.module("tkinter.filedialog")
            return filedialog.asksaveasfilename(title=caption), ""
        except Exception:
            return "", ""

    @staticmethod
    def getExistingDirectory(parent=None, caption="", directory="", *a):  # noqa: N802
        path = QFileDialog._blender("directory", caption, directory, "")
        if path is not None:
            return path
        try:
            filedialog = _realtk.module("tkinter.filedialog")
            return filedialog.askdirectory(title=caption)
        except Exception:
            return ""


QStackedWidget = QWidget
