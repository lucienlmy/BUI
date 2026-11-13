"""Qt's container widgets on the kit, drawn the Blender way: QStackedWidget, QToolBox (Blender
panels), QSplitter (region edges you drag), QToolBar (a row of tool buttons), QDockWidget (a
panel beside the central widget of a QMainWindow), QTabBar, QFrame (a box or a separator line),
QGroupBox (checkable), QScrollArea and QStatusBar.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import buipy

from bui.qtactions import QAction, QToolButton
from bui.qtsyntax import EV_CHANGE, PACK, QWidget, Qt, Signal, _kids, _timers
from bui.tksyntax._widgets import _Sash


def _detach(w):
    if w.parent is not None:
        w.parent.remove(w)


class QStackedWidget(QWidget):
    """Pages on top of each other; only the current one shows."""
    KIND = "frame"
    currentChanged = Signal(int)
    widgetRemoved = Signal(int)

    def _setup(self):
        self._pages = []
        self._current = -1

    def addWidget(self, w):  # noqa: N802
        return self.insertWidget(len(self._pages), w)

    def insertWidget(self, index, w):  # noqa: N802
        if w in self._pages:
            return self._pages.index(w)
        _detach(w)
        index = max(0, min(int(index), len(self._pages)))
        self._pages.insert(index, w)
        self.add(w)
        w.geom(PACK, fill=3, expand=1)
        w.set("visible", False)
        if self._current < 0:
            self.setCurrentIndex(index)  # the first page becomes current, as in Qt
        elif index <= self._current:
            self._current += 1  # the current page stays current
        return index

    def removeWidget(self, w):  # noqa: N802
        if w not in self._pages:
            return
        index = self._pages.index(w)
        self._pages.pop(index)
        self.remove(w)
        if index < self._current:
            self._current -= 1
        elif index == self._current:
            self._current = -1
            if self._pages:
                self.setCurrentIndex(min(index, len(self._pages) - 1))
            else:
                self.currentChanged.emit(-1)
        self.widgetRemoved.emit(index)

    def count(self):
        return len(self._pages)

    def widget(self, index):
        return self._pages[index] if 0 <= index < len(self._pages) else None

    def indexOf(self, w):  # noqa: N802
        return self._pages.index(w) if w in self._pages else -1

    def currentIndex(self):  # noqa: N802
        return self._current

    def currentWidget(self):  # noqa: N802
        return self.widget(self._current)

    def setCurrentIndex(self, index):  # noqa: N802
        if not 0 <= index < len(self._pages) or index == self._current:
            return
        for i, page in enumerate(self._pages):
            page.set("visible", i == index)
        self._current = index
        self.currentChanged.emit(index)

    def setCurrentWidget(self, w):  # noqa: N802
        self.setCurrentIndex(self.indexOf(w))


class _Panel(QWidget):
    """A Blender panel: a header you click to open or close, and its contents."""
    KIND = "panel"


class QToolBox(QWidget):
    """Items as Blender panels. The Qt API keeps one open at a time (setCurrentIndex); clicking
    a header opens or closes that panel on its own, as in Blender, and currentIndex() reads it."""
    KIND = "column"
    currentChanged = Signal(int)

    def _setup(self):
        self._items = []  # (panel, widget)
        self._last = -1

    def addItem(self, w, *args):  # noqa: N802 - (widget, text) or (widget, icon, text)
        return self.insertItem(len(self._items), w, *args)

    def insertItem(self, index, w, *args):  # noqa: N802
        text = str(args[-1]) if args else ""
        panel = _Panel()
        panel.set("text", text)
        if len(args) > 1 and getattr(args[0], "name", ""):
            panel.set("icon", args[0].name)
        _detach(w)
        panel.add(w)
        index = max(0, min(int(index), len(self._items)))
        self._items.insert(index, (panel, w))
        self._order()
        if self._last < 0:
            self.setCurrentIndex(index)
        else:
            panel.set("collapsed", True)
        return index

    def _order(self):
        for panel, _ in self._items:
            if panel.parent is not None:
                self.remove(panel)
        for panel, _ in self._items:
            self.add(panel)

    def removeItem(self, index):  # noqa: N802
        if 0 <= index < len(self._items):
            panel, w = self._items.pop(index)
            panel.remove(w)
            self.remove(panel)
            if self.currentIndex() < 0 and self._items:
                self.setCurrentIndex(min(index, len(self._items) - 1))

    def count(self):
        return len(self._items)

    def widget(self, index):
        return self._items[index][1] if 0 <= index < len(self._items) else None

    def indexOf(self, w):  # noqa: N802
        return next((i for i, (_, x) in enumerate(self._items) if x is w), -1)

    def currentIndex(self):  # noqa: N802
        """The first open panel (the user may have opened others: Blender panels are independent)."""
        return next((i for i, (p, _) in enumerate(self._items) if not p.get("collapsed")), -1)

    def currentWidget(self):  # noqa: N802
        return self.widget(self.currentIndex())

    def setCurrentIndex(self, index):  # noqa: N802
        if not 0 <= index < len(self._items):
            return
        for i, (panel, _) in enumerate(self._items):
            panel.set("collapsed", i != index)
        if index != self._last:
            self._last = index
            self.currentChanged.emit(index)

    def setCurrentWidget(self, w):  # noqa: N802
        self.setCurrentIndex(self.indexOf(w))

    def _panel(self, index):
        return self._items[index][0]

    def itemText(self, index):  # noqa: N802
        return self._panel(index).get_str("text")

    def setItemText(self, index, text):  # noqa: N802
        self._panel(index).set("text", str(text))

    def itemToolTip(self, index):  # noqa: N802
        return self._panel(index).get_str("description")

    def setItemToolTip(self, index, tip):  # noqa: N802
        self._panel(index).set("description", str(tip))

    def isItemEnabled(self, index):  # noqa: N802
        return bool(self._panel(index).get("enabled"))

    def setItemEnabled(self, index, enabled):  # noqa: N802
        self._panel(index).set("enabled", bool(enabled))

    def setItemIcon(self, index, icon):  # noqa: N802
        if getattr(icon, "name", ""):
            self._panel(index).set("icon", icon.name)


class QSplitter(QWidget):
    """Widgets side by side (or stacked) with Blender's region edges between them to drag."""
    KIND = "frame"
    splitterMoved = Signal(int, int)

    def __init__(self, orientation=None, parent=None):
        if isinstance(orientation, QWidget):
            orientation, parent = None, orientation
        self._orientation = orientation or Qt.Horizontal
        super().__init__(parent)

    def _setup(self):
        self._panes = []
        self._sashes = []
        self._sizes = {}
        self._stretch = {}
        self._handle = 4
        self._collapsible = True

    def _horizontal(self):
        return self._orientation == Qt.Horizontal

    def orientation(self):
        return self._orientation

    def setOrientation(self, orientation):  # noqa: N802
        self._orientation = orientation
        self._relayout()

    def addWidget(self, w):  # noqa: N802
        self.insertWidget(len(self._panes), w)

    def insertWidget(self, index, w):  # noqa: N802
        if w in self._panes:
            self._panes.remove(w)
        _detach(w)
        self._panes.insert(max(0, min(int(index), len(self._panes))), w)
        self._relayout()

    def replaceWidget(self, index, w):  # noqa: N802
        old = self._panes[index]
        self.remove(old)
        self._sizes.pop(old, None)
        self._panes[index] = w
        _detach(w)
        self._relayout()
        return old

    def count(self):
        return len(self._panes)

    def widget(self, index):
        return self._panes[index] if 0 <= index < len(self._panes) else None

    def indexOf(self, w):  # noqa: N802
        return self._panes.index(w) if w in self._panes else -1

    def handleWidth(self):  # noqa: N802
        return self._handle

    def setHandleWidth(self, width):  # noqa: N802
        self._handle = int(width)
        self._relayout()

    def setStretchFactor(self, index, stretch):  # noqa: N802
        self._stretch[self._panes[index]] = int(stretch)
        self._relayout()

    def childrenCollapsible(self):  # noqa: N802
        return self._collapsible

    def setChildrenCollapsible(self, b):  # noqa: N802
        self._collapsible = bool(b)

    def sizes(self):
        return [p.width() if self._horizontal() else p.height() for p in self._panes]

    def setSizes(self, sizes):  # noqa: N802
        for i, size in enumerate(sizes[:len(self._panes)]):
            self._set_pane_size(i, size, emit=False)

    def _relayout(self):
        for node in self._sashes + [p for p in self._panes if p.parent is self]:
            self.remove(node)
        self._sashes = []
        h = self._horizontal()
        side = 2 if h else 0
        # without stretch factors the last pane takes the rest of the space, as Qt's does
        stretchy = [p for p in self._panes if self._stretch.get(p, 0)] or self._panes[-1:]
        for i, pane in enumerate(self._panes):
            if i:
                sash = _Sash(self, i - 1)
                sash.set("cursor", 1.0 if h else 2.0)
                self.add(sash)
                sash.geom(PACK, side=side, fill=2 if h else 1,
                          **{"reqwidth" if h else "reqheight": self._handle})
                self._sashes.append(sash)
            self.add(pane)
            pane.geom(PACK, side=side, fill=3, expand=1 if pane in stretchy else 0,
                      **{"reqwidth" if h else "reqheight": int(self._sizes.get(pane, 0))})

    def _set_pane_size(self, index, size, emit=True):
        pane = self._panes[index]
        size = max(0 if self._collapsible else 1, int(size))
        self._sizes[pane] = size
        pane.geom(PACK, **{"reqwidth" if self._horizontal() else "reqheight": size})
        if emit:
            self.splitterMoved.emit(size, index + 1)


class QToolBar(QWidget):
    """A row of tool buttons, one per action (Blender's header row)."""
    KIND = "row"
    actionTriggered = Signal(object)
    toolButtonStyleChanged = Signal(int)

    def __init__(self, title="", parent=None):
        if isinstance(title, QWidget):
            title, parent = "", title
        super().__init__(parent)
        self.setWindowTitle(str(title))

    def _setup(self):
        self._items = {}  # action -> its node (a button, a separator or a widget)
        self._style = Qt.ToolButtonIconOnly

    def addAction(self, *args):  # noqa: N802 - (action) / (text[, slot]) / (icon, text[, slot])
        action = args[0] if isinstance(args[0], QAction) else None
        if action is None:
            icon = args[0] if not isinstance(args[0], str) else None
            rest = args[1:] if icon is not None else args
            action = QAction(str(rest[0]) if rest else "", self)
            action.setIcon(icon)
            if len(rest) > 1 and callable(rest[1]):
                action.triggered.connect(rest[1])
        button = QToolButton()
        button.setDefaultAction(action)
        button.setToolButtonStyle(self._style)
        button.triggered.connect(self.actionTriggered.emit)
        self._insert(None, action, button)
        return action

    def addWidget(self, w):  # noqa: N802
        action = QAction("", self)
        _detach(w)
        self._insert(None, action, w)
        return action

    def addSeparator(self):  # noqa: N802
        action = QAction("", self)
        action.setSeparator(True)
        self._insert(None, action, buipy.KitNode("separator"))
        return action

    def insertWidget(self, before, w):  # noqa: N802
        action = QAction("", self)
        _detach(w)
        self._insert(before, action, w)
        return action

    def _insert(self, before, action, node):
        QWidget.insertAction(self, before, action)
        self._items[action] = node
        order = self.actions()
        for a in order:
            n = self._items[a]
            if n.parent is self:
                self.remove(n)
        for a in order:
            self.add(self._items[a])

    def removeAction(self, action):  # noqa: N802
        node = self._items.pop(action, None)
        if node is not None:
            self.remove(node)
        QWidget.removeAction(self, action)

    def clear(self):
        for a in self.actions():
            self.removeAction(a)

    def widgetForAction(self, action):  # noqa: N802
        return self._items.get(action)

    def toolButtonStyle(self):  # noqa: N802
        return self._style

    def setToolButtonStyle(self, style):  # noqa: N802
        if style != self._style:
            self._style = style
            for node in self._items.values():
                if isinstance(node, QToolButton):
                    node.setToolButtonStyle(style)
            self.toolButtonStyleChanged.emit(style)

    def orientation(self):
        return Qt.Horizontal


class QDockWidget(QWidget):
    """A titled panel beside a QMainWindow's central widget (QMainWindow.addDockWidget)."""
    KIND = "panel"
    visibilityChanged = Signal(bool)
    featuresChanged = Signal(int)
    allowedAreasChanged = Signal(int)
    dockLocationChanged = Signal(int)
    DockWidgetClosable, DockWidgetMovable, DockWidgetFloatable = 0x01, 0x02, 0x04
    NoDockWidgetFeatures = 0

    def __init__(self, title="", parent=None):
        if isinstance(title, QWidget):
            title, parent = "", title
        super().__init__(parent)
        self.setWindowTitle(str(title))

    def _setup(self):
        self._widget = None
        self._features = self.DockWidgetClosable | self.DockWidgetMovable | self.DockWidgetFloatable
        self._toggle = None
        self._areas = Qt.AllDockWidgetAreas

    def setWindowTitle(self, title):  # noqa: N802
        self._title = str(title)
        self.set("text", self._title)

    def windowTitle(self):  # noqa: N802
        return self._title

    def setWidget(self, w):  # noqa: N802
        if self._widget is not None:
            self.remove(self._widget)
        self._widget = w
        if w is not None:
            _detach(w)
            self.add(w)

    def widget(self):
        return self._widget

    def features(self):
        return self._features

    def setFeatures(self, features):  # noqa: N802
        if features != self._features:
            self._features = features
            self.featuresChanged.emit(features)

    def allowedAreas(self):  # noqa: N802
        return self._areas

    def setAllowedAreas(self, areas):  # noqa: N802
        if int(areas) != self._areas:
            self._areas = int(areas)
            self.allowedAreasChanged.emit(self._areas)

    def isAreaAllowed(self, area):  # noqa: N802
        return bool(self._areas & int(area))

    def isFloating(self):  # noqa: N802
        return False  # docks stay docked in their window

    def setVisible(self, visible):  # noqa: N802
        was = self.isVisible()
        super().setVisible(visible)
        if self._toggle is not None:
            self._toggle._checked = bool(visible)
        if was != bool(visible):
            self.visibilityChanged.emit(bool(visible))

    def toggleViewAction(self):  # noqa: N802
        """A checkable action that shows and hides the dock (for a View menu)."""
        if self._toggle is None:
            self._toggle = QAction(self._title, self)
            self._toggle.setCheckable(True)
            self._toggle._checked = self.isVisible()
            self._toggle.toggled.connect(self.setVisible)
        return self._toggle


class QTabBar(QWidget):
    """Tabs without pages (QTabWidget has both)."""
    KIND = "tabs"
    currentChanged = Signal(int)
    tabBarClicked = Signal(int)

    def _setup(self):
        self._titles = []

    def addTab(self, *args):  # noqa: N802 - (text) or (icon, text)
        return self.insertTab(len(self._titles), *args)

    def insertTab(self, index, *args):  # noqa: N802
        index = max(0, min(int(index), len(self._titles)))
        self._titles.insert(index, str(args[-1]))
        self.set("items", self._titles)
        if len(self._titles) == 1:
            self.currentChanged.emit(0)  # Qt: the first tab becomes current
        return index

    def removeTab(self, index):  # noqa: N802
        if 0 <= index < len(self._titles):
            self._titles.pop(index)
            self.set("items", self._titles)

    def count(self):
        return len(self._titles)

    def currentIndex(self):  # noqa: N802
        return int(self.get("current")) if self._titles else -1

    def setCurrentIndex(self, index):  # noqa: N802
        if 0 <= index < len(self._titles) and index != self.currentIndex():
            self.set("current", float(index))
            self.currentChanged.emit(int(index))

    def tabText(self, index):  # noqa: N802
        return self._titles[index] if 0 <= index < len(self._titles) else ""

    def setTabText(self, index, text):  # noqa: N802
        self._titles[index] = str(text)
        self.set("items", self._titles)

    def _event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            self.tabBarClicked.emit(int(a))
            self.currentChanged.emit(int(a))


def _descendants(node):
    for c in _kids(node):
        yield c
        yield from _descendants(c)


class QFrame(QWidget):
    """A frame's shape is Blender's: a box (uiLayoutBox) for Box / Panel / StyledPanel, a separator
    line for HLine / VLine; its contents (a layout) go inside the box."""
    NoFrame, Box, Panel, WinPanel, HLine, VLine, StyledPanel = 0, 1, 2, 3, 4, 5, 6
    Plain, Raised, Sunken = 0x10, 0x20, 0x30
    Shape_Mask, Shadow_Mask = 0x0F, 0xF0
    _BOXES = (Box, Panel, WinPanel, StyledPanel)

    def _setup(self):
        self._shape, self._shadow = self.NoFrame, self.Plain
        self._line_width, self._mid_width = 1, 0
        self._inner = None  # the box or the line

    def frameShape(self):  # noqa: N802
        return self._shape

    def setFrameShape(self, shape):  # noqa: N802
        self._shape = int(shape)
        self._restyle()

    def frameShadow(self):  # noqa: N802
        return self._shadow

    def setFrameShadow(self, shadow):  # noqa: N802
        self._shadow = int(shadow)

    def frameStyle(self):  # noqa: N802
        return self._shape | self._shadow

    def setFrameStyle(self, style):  # noqa: N802
        self._shadow = int(style) & self.Shadow_Mask
        self.setFrameShape(int(style) & self.Shape_Mask)

    def lineWidth(self):  # noqa: N802
        return self._line_width

    def setLineWidth(self, w):  # noqa: N802
        self._line_width = int(w)

    def midLineWidth(self):  # noqa: N802
        return self._mid_width

    def setMidLineWidth(self, w):  # noqa: N802
        self._mid_width = int(w)

    def frameWidth(self):  # noqa: N802
        return 0 if self._shape == self.NoFrame else self._line_width

    def frameRect(self):  # noqa: N802
        return self.rect()

    def _content_node(self):
        inner = self._inner
        return inner if inner is not None and inner.type == "box" else self

    def _restyle(self):
        kind = "box" if self._shape in self._BOXES else "separator" if self._shape in (self.HLine, self.VLine) \
            else None
        old = self._inner
        if old is not None and old.type == kind:
            old.set("vertical", self._shape == self.VLine) if kind == "separator" else None
            return
        moved = [c for c in _kids(old)] if old is not None and old.type == "box" else []
        if old is not None:
            self.remove(old)
        if kind is None:
            self._inner = None
            target = self
        else:
            self._inner = inner = buipy.KitNode(kind)
            if kind == "separator":
                inner.set("line", True)
                inner.set("vertical", self._shape == self.VLine)
            moved += [c for c in _kids(self)]
            self.add(inner)
            inner.geom(PACK, fill=3, expand=1)
            target = inner if kind == "box" else self
        for c in moved:
            c.parent.remove(c)
            target.add(c)
            c.geom(PACK, fill=3, expand=1)


class QGroupBox(QWidget):
    """A titled group (Blender's sub-panel look); checkable puts a checkbox in the title that enables
    and disables the contents (a panel header with a toggle)."""
    clicked = Signal(bool)
    toggled = Signal(bool)

    def __init__(self, title="", parent=None):
        if isinstance(title, QWidget):
            title, parent = "", title
        super().__init__(parent)
        self._title = str(title)
        self._check = None
        self._disabled = {}  # descendant -> its own enabled state, while unchecked
        self._flat = False
        self.set("text", self._title)
        self.set("padding", round(0.3 * buipy.kit_unit()))

    def setTitle(self, t):  # noqa: N802
        self._title = str(t)
        if self._check is not None:
            self._check.setText(self._title)
        else:
            self.set("text", self._title)

    def title(self):
        return self._title

    def isFlat(self):  # noqa: N802
        return self._flat

    def setFlat(self, b):  # noqa: N802
        """Flat: the title only, no padding around the contents."""
        self._flat = bool(b)
        self.set("padding", 0 if b else round(0.3 * buipy.kit_unit()))

    def isCheckable(self):  # noqa: N802
        return self._check is not None

    def setCheckable(self, b):  # noqa: N802
        if bool(b) == self.isCheckable():
            return
        if b:
            from bui.qtsyntax import QCheckBox
            self._check = QCheckBox(self._title)
            self._check.setChecked(True)
            self._check.toggled.connect(self._toggled)
            self._check.clicked.connect(self.clicked.emit)
            self.set("text", "")
            buipy.KitNode.add(self, self._check, 0)
            self._check.geom(PACK, side=0, fill=1)
        else:
            self.setChecked(True)
            self.remove(self._check)
            self._check = None
            self.set("text", self._title)

    def isChecked(self):  # noqa: N802
        return self._check is not None and self._check.isChecked()

    def setChecked(self, b):  # noqa: N802
        if self._check is not None:
            self._check.setChecked(b)  # its toggled() does the rest

    def _toggled(self, on):
        self._apply_check(on)
        self.toggled.emit(on)

    def _apply_check(self, on):
        """Unchecked disables everything inside (Qt); checking gives each its own state back."""
        if on:
            for node, was in self._disabled.items():
                node.set("enabled", was)
            self._disabled = {}
            return
        for node in _descendants(self):
            if node is not self._check and node not in self._disabled:
                self._disabled[node] = bool(node.get("enabled"))
                node.set("enabled", False)


class QScrollArea(QWidget):
    KIND = "scroll"

    def _setup(self):
        self.set("rows", 10)
        self._widget = None
        self._resizable = False
        self._policies = [Qt.ScrollBarAsNeeded, Qt.ScrollBarAsNeeded]

    def setWidget(self, w):  # noqa: N802
        for c in _kids(self):
            self.remove(c)
        self._widget = w
        if w is not None:
            _detach(w)
            self.add(w)

    def widget(self):
        return self._widget

    def takeWidget(self):  # noqa: N802
        w, self._widget = self._widget, None
        if w is not None and w.parent is self:
            self.remove(w)
        return w

    def widgetResizable(self):  # noqa: N802
        return self._resizable

    def setWidgetResizable(self, b):  # noqa: N802
        """The kit's scroll view always fits its contents' width (Qt's resizable widget)."""
        self._resizable = bool(b)

    def horizontalScrollBarPolicy(self):  # noqa: N802
        return self._policies[0]

    def setHorizontalScrollBarPolicy(self, p):  # noqa: N802
        self._policies[0] = int(p)

    def verticalScrollBarPolicy(self):  # noqa: N802
        return self._policies[1]

    def setVerticalScrollBarPolicy(self, p):  # noqa: N802
        """AlwaysOff hides the scroll bar (the view still scrolls with the wheel)."""
        self._policies[1] = int(p)
        self.set("noscrollbar", int(p) == Qt.ScrollBarAlwaysOff)

    def ensureVisible(self, x, y, xmargin=50, ymargin=50):  # noqa: N802
        """Scroll so content point (x, y) shows, `ymargin` from the edge."""
        off, h = self.get("scroll"), self.height()
        if y - ymargin < off:
            off = y - ymargin
        elif y + ymargin > off + h:
            off = y + ymargin - h
        self.set("scroll", max(0.0, float(off)))

    def ensureWidgetVisible(self, child, xmargin=50, ymargin=50):  # noqa: N802
        top = child._r[1] - self._r[1] + self.get("scroll")  # in content coordinates
        off, h = self.get("scroll"), self.height()
        if top - ymargin < off:
            off = top - ymargin
        elif top + child.height() + ymargin > off + h:
            off = top + child.height() + ymargin - h
        self.set("scroll", max(0.0, float(off)))


class QStatusBar(QWidget):
    """Blender's status bar row: widgets on the left (hidden while a message shows), the message, and
    permanent widgets on the right."""
    KIND = "row"
    messageChanged = Signal(str)

    def _setup(self):
        self._normal, self._permanent = [], []
        self._msg = buipy.KitNode("label")
        self._token = None
        self._grip = True
        self._order()

    def _order(self):
        for c in list(_kids(self)):
            self.remove(c)
        for w in self._normal:
            self.add(w)
        self.add(self._msg)
        self._msg.geom(PACK, side=2, fill=3, expand=1)
        for w in self._normal:
            w.geom(PACK, side=2, fill=2)
        for w in self._permanent:
            self.add(w)
            w.geom(PACK, side=2, fill=2)

    def _insert(self, kind, index, w, stretch):
        _detach(w)
        lst = self._permanent if kind else self._normal
        index = len(lst) if index < 0 else min(int(index), len(lst))
        lst.insert(index, w)
        self._order()
        w.set("visible", kind or not self.currentMessage())
        return index

    def addWidget(self, w, stretch=0):  # noqa: N802
        self._insert(False, -1, w, stretch)

    def insertWidget(self, index, w, stretch=0):  # noqa: N802
        return self._insert(False, index, w, stretch)

    def addPermanentWidget(self, w, stretch=0):  # noqa: N802
        self._insert(True, -1, w, stretch)

    def insertPermanentWidget(self, index, w, stretch=0):  # noqa: N802
        return self._insert(True, index, w, stretch)

    def removeWidget(self, w):  # noqa: N802
        for lst in (self._normal, self._permanent):
            if w in lst:
                lst.remove(w)
                self.remove(w)

    def showMessage(self, text, timeout=0):  # noqa: N802
        _timers.cancel(self._token)
        self._token = _timers.after(timeout, self.clearMessage) if timeout else None
        self._set_message(str(text))

    def clearMessage(self):  # noqa: N802
        _timers.cancel(self._token)
        self._token = None
        self._set_message("")

    def _set_message(self, text):
        if text == self.currentMessage():
            return
        self._msg.set("text", text)
        for w in self._normal:  # a temporary message covers the normal widgets (Qt)
            w.set("visible", not text)
        self.messageChanged.emit(text)

    def currentMessage(self):  # noqa: N802
        return self._msg.get_str("text")

    def isSizeGripEnabled(self):  # noqa: N802
        return self._grip

    def setSizeGripEnabled(self, b):  # noqa: N802
        """The window's own frame resizes it; there is no grip to draw."""
        self._grip = bool(b)
