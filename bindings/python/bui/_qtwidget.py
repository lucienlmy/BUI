"""QObject and QWidget behavior beyond geometry and painting, for `bui.qtsyntax.QWidget`.

Signal blocking, dynamic properties, finding children, mapping points between widgets, actions,
show / hide events, and the descriptive properties Qt only stores (status tip, what's this,
accessible name). Everything here acts on the widget's kit node or is Qt's own stored state.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from bui._qtprops import QtProperty, qt_properties


def _name(n):
    return n.decode() if isinstance(n, bytes) else str(n)


class _KitView:
    """A QWidget seen as a plain kit node, for code shared with the tkinter layer: `rect` is the
    node's (x, y, w, h), not Qt's rect() method; everything else is the widget's."""
    __slots__ = ("_w",)

    def __init__(self, w):
        self._w = w

    @property
    def rect(self):
        return self._w._r

    def __getattr__(self, name):
        return getattr(self._w, name)


@qt_properties(
    statusTip=QtProperty(default=""), whatsThis=QtProperty(default=""),
    accessibleName=QtProperty(default=""), accessibleDescription=QtProperty(default=""),
    accessibleIdentifier=QtProperty(default=""), windowIconText=QtProperty(default=""),
    windowFilePath=QtProperty(default=""), windowRole=QtProperty(default=""),
    toolTipDuration=QtProperty(default=-1), focusPolicy=QtProperty(default=0),
    contextMenuPolicy=QtProperty(default=1), layoutDirection=QtProperty(default=0),
    inputMethodHints=QtProperty(default=0), windowModality=QtProperty(default=0),
)
class QWidgetMore:
    @property
    def _node(self):
        return _KitView(self)

    # -- QObject -----------------------------------------------------------------------------------
    def blockSignals(self, block):  # noqa: N802
        """Stop (or resume) this object's signals; returns the previous state, as Qt does."""
        old = self.__dict__.get("_blocked", False)
        self.__dict__["_blocked"] = bool(block)
        return old

    def signalsBlocked(self):  # noqa: N802
        return self.__dict__.get("_blocked", False)

    def setProperty(self, name, value):  # noqa: N802
        """A dynamic property (Qt returns False for those: they aren't declared ones)."""
        self.__dict__.setdefault("_dyn", {})[_name(name)] = value
        return False

    def property(self, name):
        name = _name(name)
        dyn = self.__dict__.get("_dyn", {})
        if name in dyn:
            return dyn[name]
        getter = getattr(type(self), name, None)
        return getter(self) if callable(getter) else None

    def dynamicPropertyNames(self):  # noqa: N802
        return [n.encode() for n in self.__dict__.get("_dyn", {})]

    def inherits(self, class_name):
        return any(c.__name__ == _name(class_name) for c in type(self).__mro__)

    def isWidgetType(self):  # noqa: N802
        return True

    def findChildren(self, type_=None, name=""):  # noqa: N802
        """Descendant widgets (through layouts too), filtered by class and object name."""
        from bui.qtsyntax import QWidget, _kids
        out, stack = [], list(_kids(self))
        while stack:
            node = stack.pop(0)
            if isinstance(node, QWidget) and (type_ is None or isinstance(node, type_)) and (
                    not name or node.objectName() == name):
                out.append(node)
            stack.extend(_kids(node))
        return out

    def findChild(self, type_=None, name=""):  # noqa: N802
        found = self.findChildren(type_, name)
        return found[0] if found else None

    def setContextMenuPolicy(self, policy):  # noqa: N802
        """Right clicks then go to contextMenuEvent (the default), show the widget's actions, or emit
        customContextMenuRequested; the kit reports them once a policy needs them."""
        self.__dict__.setdefault("_qp", {})["contextMenuPolicy"] = int(policy)
        if int(policy) in (2, 3):
            self.set("events", True)

    def deleteLater(self):  # noqa: N802
        parent = self.parent
        if parent is not None:
            parent.remove(self)

    # -- tree and state ------------------------------------------------------------------------------
    def toolTip(self):  # noqa: N802
        return self.get_str("description")

    def isHidden(self):  # noqa: N802
        return not self.isVisible()

    def setHidden(self, hidden):  # noqa: N802
        self.setVisible(not hidden)

    def setDisabled(self, disabled):  # noqa: N802
        self.setEnabled(not disabled)

    def isTopLevel(self):  # noqa: N802
        return self.isWindow()

    def isAncestorOf(self, child):  # noqa: N802
        w = child.parentWidget() if child is not None else None
        while w is not None:
            if w is self:
                return True
            w = w.parentWidget()
        return False

    def _up_to(self, ancestor):
        w = self
        while w is not None and w is not ancestor:
            yield w
            w = w.parentWidget()

    def isVisibleTo(self, ancestor):  # noqa: N802
        return all(w.isVisible() for w in self._up_to(ancestor))

    def isEnabledTo(self, ancestor):  # noqa: N802
        return all(w.isEnabled() for w in self._up_to(ancestor))

    def isLeftToRight(self):  # noqa: N802
        return self.layoutDirection() == 0

    def isRightToLeft(self):  # noqa: N802
        return self.layoutDirection() == 1

    # -- geometry between widgets -------------------------------------------------------------------------
    def _origin(self):
        x, y = self.absolutePos()
        return x, y

    def mapTo(self, parent, pos):  # noqa: N802
        from bui.qtsyntax import QPoint
        (ax, ay), (bx, by) = self._origin(), parent._origin()
        return QPoint(pos.x() + ax - bx, pos.y() + ay - by)

    def mapFrom(self, parent, pos):  # noqa: N802
        from bui.qtsyntax import QPoint
        (ax, ay), (bx, by) = self._origin(), parent._origin()
        return QPoint(pos.x() - ax + bx, pos.y() - ay + by)

    def mapToParent(self, pos):  # noqa: N802
        from bui.qtsyntax import QPoint
        return QPoint(pos.x() + self.x(), pos.y() + self.y())

    def mapFromParent(self, pos):  # noqa: N802
        from bui.qtsyntax import QPoint
        return QPoint(pos.x() - self.x(), pos.y() - self.y())

    def childAt(self, x, y=None):  # noqa: N802
        """The deepest child widget at a point of this widget, or None."""
        if y is None:
            x, y = x.x(), x.y()
        ox, oy = self._origin()
        hit = None
        for w in self.findChildren():
            if not w.isVisibleTo(self):
                continue
            wx, wy = w._origin()
            if wx <= ox + x < wx + w.width() and wy <= oy + y < wy + w.height():
                hit = w  # findChildren lists parents before their children: the last hit is deepest
        return hit

    def childrenRect(self):  # noqa: N802
        from bui.qtsyntax import QRect
        kids = [w for w in self.children() if w.isVisible()]
        if not kids:
            return QRect(0, 0, 0, 0)
        x0, y0 = min(w.x() for w in kids), min(w.y() for w in kids)
        x1 = max(w.x() + w.width() for w in kids)
        y1 = max(w.y() + w.height() for w in kids)
        return QRect(x0, y0, x1 - x0, y1 - y0)

    def adjustSize(self):  # noqa: N802
        """Resize to the size the contents need (the natural size)."""
        self.resize(*self.natural())

    def minimumSizeHint(self):  # noqa: N802
        return self.sizeHint()

    def hasFocus(self):  # noqa: N802
        """Whether this widget has the keyboard (text) focus of its window."""
        ctx = self.window()._ctx
        return ctx is not None and ctx.focus is self

    def clearFocus(self):  # noqa: N802
        if self.hasFocus():
            self.window()._ctx.focus_node(None)

    # -- actions (Qt keeps them on the widget for toolbars and context menus) --------------------------------
    def actions(self):
        return list(self.__dict__.get("_actions", []))

    def addAction(self, action):  # noqa: N802
        self.insertAction(None, action)
        return action

    def addActions(self, actions):  # noqa: N802
        for a in actions:
            self.addAction(a)

    def insertAction(self, before, action):  # noqa: N802
        acts = self.__dict__.setdefault("_actions", [])
        if action in acts:
            acts.remove(action)
        acts.insert(acts.index(before) if before in acts else len(acts), action)
        action._attach(self)

    def insertActions(self, before, actions):  # noqa: N802
        for a in actions:
            self.insertAction(before, a)

    def removeAction(self, action):  # noqa: N802
        acts = self.__dict__.get("_actions", [])
        if action in acts:
            acts.remove(action)
            action._detach(self)

    # -- show / hide hooks (QWidget.show, hide and setVisible call them) ------------------------------------
    def showEvent(self, event):  # noqa: N802
        pass

    def hideEvent(self, event):  # noqa: N802
        pass

    # -- the window, through the host's window-manager requests ------------------------------------------
    _STATE_TO_WM = {0: 0, 1: 1, 2: 3}  # Qt.WindowNoState / WindowMinimized / WindowMaximized -> host state

    def _host(self):
        top = self.window()
        return getattr(top, "_window", None)

    def _wm(self, key, a=0.0, b=0.0):
        host = self._host()
        if host is not None:
            host.wm(key, a, b)

    def _apply_window_hints(self):
        """Called when the window opens: what was set before it existed."""
        d = self.__dict__
        if "_min_size" in d:
            self._wm("minsize", *d["_min_size"])
        if "_max_size" in d:
            self._wm("maxsize", *d["_max_size"])
        if "_opacity" in d:
            self._wm("alpha", d["_opacity"])
        if d.get("_flags", 0) & 0x00040000:  # Qt.WindowStaysOnTopHint
            self._wm("topmost", 1)
        if d.get("_state", 0):
            self.setWindowState(d["_state"])

    def windowState(self):  # noqa: N802
        host = self._host()
        if host is not None:
            got = host.wm_get("state")
            return {1: 1, 3: 2}.get(int(got[0]), 0) if got else 0
        return self.__dict__.get("_state", 0)

    def setWindowState(self, state):  # noqa: N802
        self.__dict__["_state"] = int(state) & 3
        self._wm("state", self._STATE_TO_WM.get(int(state) & 3, 0))

    def isMinimized(self):  # noqa: N802
        return self.windowState() == 1

    def isMaximized(self):  # noqa: N802
        return self.windowState() == 2

    def showMinimized(self):  # noqa: N802
        self.show()
        self.setWindowState(1)

    def showMaximized(self):  # noqa: N802
        self.show()
        self.setWindowState(2)

    def showNormal(self):  # noqa: N802
        self.show()
        self.setWindowState(0)

    def activateWindow(self):  # noqa: N802
        self._wm("focus")

    def raise_(self):
        """In front: the window above the others, or this widget above its siblings."""
        if self.isWindow():
            self._wm("lift")
        elif self.parent is not None:
            parent = self.parent
            parent.remove(self)
            parent.add(self)

    def lower(self):
        if self.isWindow():
            self._wm("lower")
        elif self.parent is not None:
            parent = self.parent
            parent.remove(self)
            parent.add(self, 0)

    def stackUnder(self, w):  # noqa: N802
        parent = self.parent
        if parent is not None and w.parent is parent:
            parent.remove(self)
            kids = list(parent.children())
            parent.add(self, kids.index(w) if w in kids else 0)

    def windowOpacity(self):  # noqa: N802
        return self.__dict__.get("_opacity", 1.0)

    def setWindowOpacity(self, opacity):  # noqa: N802
        self.__dict__["_opacity"] = max(0.0, min(1.0, float(opacity)))
        self._wm("alpha", self.__dict__["_opacity"])

    def windowFlags(self):  # noqa: N802
        return self.__dict__.get("_flags", 0)

    def setWindowFlags(self, flags):  # noqa: N802
        """Kept, and Qt.WindowStaysOnTopHint applied (the host's topmost); other flags have no effect."""
        self.__dict__["_flags"] = int(flags)
        self._wm("topmost", 1 if int(flags) & 0x00040000 else 0)

    # -- sizes (a window's limits go to the host; a child's minimum is its requested size) -----------------
    def minimumSize(self):  # noqa: N802
        from bui.qtsyntax import QSize
        return QSize(*self.__dict__.get("_min_size", (0, 0)))

    def minimumWidth(self):  # noqa: N802
        return self.minimumSize().width()

    def minimumHeight(self):  # noqa: N802
        return self.minimumSize().height()

    def maximumSize(self):  # noqa: N802
        from bui.qtsyntax import QSize
        return QSize(*self.__dict__.get("_max_size", (16777215, 16777215)))

    def maximumWidth(self):  # noqa: N802
        return self.maximumSize().width()

    def maximumHeight(self):  # noqa: N802
        return self.maximumSize().height()

    @staticmethod
    def _wh(w, h):
        return (w.width(), w.height()) if h is None else (int(w), int(h))

    def setMinimumSize(self, w, h=None):  # noqa: N802
        w, h = self._wh(w, h)
        self.__dict__["_min_size"] = (w, h)
        if self.isWindow():
            self._wm("minsize", w, h)
        else:
            self.geom(int(self.geom_get("manager")), reqwidth=w, reqheight=h)

    def setMaximumSize(self, w, h=None):  # noqa: N802
        """A window's largest size (the host keeps it); child widgets have no maximum on the kit."""
        w, h = self._wh(w, h)
        self.__dict__["_max_size"] = (w, h)
        if self.isWindow():
            self._wm("maxsize", w, h)

    def setMaximumWidth(self, w):  # noqa: N802
        self.setMaximumSize(w, self.maximumHeight())

    def setMaximumHeight(self, h):  # noqa: N802
        self.setMaximumSize(self.maximumWidth(), h)

    def sizePolicy(self):  # noqa: N802
        from bui.qtsyntax import QSizePolicy
        h, v = self._policy or (None, None)
        return QSizePolicy(QSizePolicy.Preferred if h is None else h, QSizePolicy.Preferred if v is None else v)

    # -- focus ------------------------------------------------------------------------------------------
    def setFocus(self, *reason):  # noqa: N802
        """Give this widget the keyboard focus of its window (once the window is open)."""
        ctx = self.window()._ctx
        if ctx is not None:
            ctx.focus_node(self)

    # -- timers (QObject.startTimer) ------------------------------------------------------------------------
    def startTimer(self, interval, *a):  # noqa: N802
        from bui.qtsyntax import _timers
        timers = self.__dict__.setdefault("_qtimers", {})
        timer_id = self.__dict__.get("_qtimer_last", 0) + 1  # never reused: a stale fire can't hit a new timer
        self.__dict__["_qtimer_last"] = timer_id
        timers[timer_id] = int(interval)

        def fire():
            if timer_id in self.__dict__.get("_qtimers", {}):
                from bui.qtgui import QTimerEvent
                self.timerEvent(QTimerEvent(timer_id))
                if timer_id in self.__dict__.get("_qtimers", {}):
                    _timers.after(timers[timer_id], fire)
        _timers.after(int(interval), fire)
        return timer_id

    def killTimer(self, timer_id):  # noqa: N802
        self.__dict__.get("_qtimers", {}).pop(timer_id, None)

    def timerEvent(self, event):  # noqa: N802
        pass

    # -- painting and the title ---------------------------------------------------------------------------
    def updatesEnabled(self):  # noqa: N802
        return self.__dict__.get("_updates", True)

    def setUpdatesEnabled(self, enabled):  # noqa: N802
        self.__dict__["_updates"] = bool(enabled)
        if enabled:
            self.update()

    def isWindowModified(self):  # noqa: N802
        return self.__dict__.get("_modified", False)

    def setWindowModified(self, modified):  # noqa: N802
        """The "[*]" in the window title shows as "*" while modified, as in Qt."""
        self.__dict__["_modified"] = bool(modified)
        self.setWindowTitle(self.windowTitle())

    def _shown_title(self):
        return self.windowTitle().replace("[*]", "*" if self.isWindowModified() else "")

    # -- fonts and colors ---------------------------------------------------------------------------------
    def font(self):
        from bui.qtsyntax import QApplication
        return self.__dict__.get("_font") or QApplication.font()

    def fontMetrics(self):  # noqa: N802
        from bui.qtgui import QFontMetrics
        return QFontMetrics(self.font())

    def palette(self):
        from bui.qtgui import QPalette
        return QPalette(self.__dict__.get("_palette"))

    def setPalette(self, palette):  # noqa: N802
        """Window (or Base / Button) is the widget's background, WindowText (or Text) its text color."""
        from bui.qtgui import QPalette
        self.__dict__["_palette"] = palette
        colors = palette._colors
        bg = next((colors[r] for r in (QPalette.Window, QPalette.Base, QPalette.Button) if r in colors), None)
        fg = next((colors[r] for r in (QPalette.WindowText, QPalette.Text, QPalette.ButtonText) if r in colors), None)
        for prop, c in (("bg", bg), ("text_color", fg)):
            if c is not None:
                self.set(prop, (c.red() / 255.0, c.green() / 255.0, c.blue() / 255.0, c.alpha() / 255.0))
