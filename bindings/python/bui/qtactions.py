"""Qt's actions on BUI: QAction (a command shared by menus, tool buttons and shortcuts), QActionGroup,
QMenu and QMenuBar (Blender's pulldown menus and header bar), QToolButton (a button showing an
action) and QButtonGroup. An action's shortcut is bound on its window like Blender's keymap; its
status tip shows in the window's status bar when it is hovered (showStatusText).

SPDX-License-Identifier: GPL-2.0-or-later
"""

from typing import List

import buipy

from bui._qtinputs import ButtonMenuMore
from bui._qtprops import QtProperty, qt_properties
from bui.components.menus import MenuItem
from bui.qtcore import QKeySequence, QShortcut, _shortcuts
from bui.qtsyntax import EV_CLICK, QAbstractButton, QFont, QIcon, QPoint, QWidget, Qt, Signal


def _plain(text):
    """Qt's iconText / toolTip default: the text without "&" mnemonics and a trailing "..."."""
    return text.replace("&&", "\0").replace("&", "").replace("\0", "&").removesuffix("...").removesuffix("…")


def _key_text(seq):
    """A key sequence as Blender's menus write it: "Ctrl Shift S" (and "Ctrl +" for Ctrl++)."""
    text = seq.toString()
    if not text:
        return None
    mods, _, key = text[:-1].rpartition("+") if text.endswith("+") else text.rpartition("+")
    key = key or "+"
    return " ".join([*filter(None, mods.split("+")), key])


class _ActionShortcut(QShortcut):
    """An action's key: bound on the window of a widget the action is in (its parent, or a widget,
    menu or tool button it was added to)."""

    def __init__(self, action, key):
        self._action = action
        super().__init__(key, None, action._from_shortcut)

    def _widget(self):
        return self._action._widget()


# ==========================================================================
# Actions


@qt_properties(statusTip=QtProperty(default="", changed="changed"),
               whatsThis=QtProperty(default="", changed="changed"),
               data=QtProperty(default=None),
               priority=QtProperty(default=128, changed="changed"),
               menuRole=QtProperty(default=1, changed="changed"),
               autoRepeat=QtProperty(default=True, changed="changed"),
               shortcutContext=QtProperty(default=1, changed="changed"),
               iconVisibleInMenu=QtProperty(default=True, getter="isIconVisibleInMenu", changed="changed"),
               shortcutVisibleInContextMenu=QtProperty(default=True, getter="isShortcutVisibleInContextMenu",
                                                       changed="changed"))
class QAction:
    NoRole, TextHeuristicRole, ApplicationSpecificRole, AboutQtRole, AboutRole, PreferencesRole, QuitRole = \
        range(7)
    LowPriority, NormalPriority, HighPriority = 0, 128, 256
    Trigger, Hover = 0, 1
    triggered = Signal(bool)
    toggled = Signal(bool)
    hovered = Signal()
    changed = Signal()
    enabledChanged = Signal(bool)
    checkableChanged = Signal(bool)
    visibleChanged = Signal()

    def __init__(self, *args, parent=None, triggered=None, text=None, **kw):
        """QAction(parent) / QAction(text, parent) / QAction(icon, text, parent)."""
        args = list(args)
        self._icon = args.pop(0) if args and isinstance(args[0], QIcon) else None
        self._text = str(args.pop(0)) if args and isinstance(args[0], str) else str(text or "")
        self._parent = args.pop(0) if args and parent is None else parent
        self._icon_text = self._tool_tip = self._font = self._menu = self._group = None
        self._keys: List[QKeySequence] = []
        self._bound: List[_ActionShortcut] = []
        self._users = []  # the widgets and menus showing it (associatedObjects)
        self._checkable = self._checked = self._separator = False
        self._enabled = self._visible = True
        if triggered is not None:
            self.triggered.connect(triggered)

    # -- text -------------------------------------------------------------------------------------------------
    def text(self):
        return self._text

    def setText(self, t):  # noqa: N802
        self._set("_text", str(t))

    def iconText(self):  # noqa: N802
        return self._icon_text if self._icon_text is not None else _plain(self._text)

    def setIconText(self, t):  # noqa: N802
        self._set("_icon_text", str(t))

    def toolTip(self):  # noqa: N802
        return self._tool_tip if self._tool_tip is not None else _plain(self._text)

    def setToolTip(self, t):  # noqa: N802
        self._set("_tool_tip", str(t))

    def icon(self):
        return self._icon or QIcon()

    def setIcon(self, icon):  # noqa: N802
        self._set("_icon", icon)

    def font(self):
        return self._font or QFont()

    def setFont(self, font):  # noqa: N802
        self._set("_font", font)

    def _set(self, attr, value):
        if getattr(self, attr) != value:
            setattr(self, attr, value)
            self.changed.emit()

    # -- shortcuts --------------------------------------------------------------------------------------------
    def shortcut(self):
        return self._keys[0] if self._keys else QKeySequence()

    def setShortcut(self, key):  # noqa: N802
        self.setShortcuts([key] if key else [])

    def shortcuts(self):
        return list(self._keys)

    def setShortcuts(self, keys):  # noqa: N802
        """Bound on the action's window (QShortcut); the first one shows in menus."""
        if isinstance(keys, int):
            keys = QKeySequence.keyBindings(keys)
        for s in self._bound:
            s.setEnabled(False)  # the kit keeps a binding; an old key stays quiet
            if s in _shortcuts:
                _shortcuts.remove(s)
        self._keys = [QKeySequence(k) for k in keys if not QKeySequence(k).isEmpty()]
        self._bound = [_ActionShortcut(self, k) for k in self._keys]
        self.changed.emit()

    def _widget(self):
        """A widget the action lives in, for its window (where its shortcut is bound)."""
        for user in (self._parent, *self._users):
            if isinstance(user, QWidget):
                return user
            if isinstance(user, QMenu) and user._root_widget() is not None:
                return user._root_widget()
        return None

    def _attach(self, user):
        if user not in self._users:
            self._users.append(user)
        for s in self._bound:
            s._bind()

    def _detach(self, user):
        if user in self._users:
            self._users.remove(user)

    def _from_shortcut(self):
        if self._enabled and self._visible:
            self.trigger()

    # -- state --------------------------------------------------------------------------------------------------
    def isCheckable(self):  # noqa: N802
        return self._checkable

    def setCheckable(self, b):  # noqa: N802
        if bool(b) != self._checkable:
            self._checkable = bool(b)
            self.checkableChanged.emit(self._checkable)
            self.changed.emit()

    def isChecked(self):  # noqa: N802
        return self._checked and self._checkable

    def setChecked(self, b):  # noqa: N802
        if not self._checkable or bool(b) == self._checked:
            return
        self._checked = bool(b)
        if self._checked and self._group is not None:
            self._group._member_checked(self)
        self.toggled.emit(self._checked)
        self.changed.emit()

    def toggle(self):
        self.setChecked(not self._checked)

    def isEnabled(self):  # noqa: N802
        return self._enabled and (self._group is None or self._group.isEnabled())

    def setEnabled(self, b):  # noqa: N802
        if bool(b) != self._enabled:
            self._enabled = bool(b)
            self.enabledChanged.emit(self._enabled)
            self.changed.emit()

    def setDisabled(self, b):  # noqa: N802
        self.setEnabled(not b)

    def resetEnabled(self):  # noqa: N802
        self.setEnabled(True)

    def isVisible(self):  # noqa: N802
        return self._visible and (self._group is None or self._group.isVisible())

    def setVisible(self, b):  # noqa: N802
        """Menus and tool bars leave a hidden action out."""
        if bool(b) != self._visible:
            self._visible = bool(b)
            self.visibleChanged.emit()
            self.changed.emit()

    def isSeparator(self):  # noqa: N802
        return self._separator

    def setSeparator(self, b):  # noqa: N802
        self._set("_separator", bool(b))

    # -- menus, groups and owners -----------------------------------------------------------------------------
    def menu(self):
        return self._menu

    def setMenu(self, menu):  # noqa: N802
        """The submenu this action opens (QMenu.menuAction() is such an action)."""
        self._menu = menu
        self.changed.emit()

    def actionGroup(self):  # noqa: N802
        return self._group

    def setActionGroup(self, group):  # noqa: N802
        if self._group is not None:
            self._group.removeAction(self)
        if group is not None:
            group.addAction(self)

    def parent(self):
        return self._parent

    def parentWidget(self):  # noqa: N802
        return self._parent if isinstance(self._parent, QWidget) else None

    def associatedObjects(self):  # noqa: N802
        return list(self._users)

    def associatedWidgets(self):  # noqa: N802
        return [u for u in self._users if isinstance(u, QWidget)]

    # -- doing it -----------------------------------------------------------------------------------------------
    def trigger(self):
        """As a click: a checkable action toggles (one checked in an exclusive group stays checked)."""
        if not self.isEnabled():
            return
        if self._checkable and not (self._checked and self._group is not None
                                    and self._group.exclusionPolicy() == QActionGroup.ExclusionPolicy.Exclusive):
            self.toggle()
        self.triggered.emit(self.isChecked())
        if self._group is not None:
            self._group.triggered.emit(self)

    def hover(self):
        self.hovered.emit()
        self.showStatusText()
        if self._group is not None:
            self._group.hovered.emit(self)

    def activate(self, event):
        (self.trigger if event == self.Trigger else self.hover)()

    def showStatusText(self, widget=None):  # noqa: N802
        """The status tip in the status bar of the window the action is in."""
        widget = widget or self._widget()
        top = widget.window() if widget is not None else None
        status = getattr(top, "_status", None)
        if status is None:
            return False
        status.showMessage(self.statusTip())
        return True

    def _item(self):
        """This action as one of the kit's menu items (None when hidden)."""
        if not self.isVisible():
            return None
        if self._separator:
            return "-"
        tip = self._tool_tip if self._tool_tip is not None else self.statusTip()
        icon = getattr(self._icon, "name", "") if self.isIconVisibleInMenu() else ""
        return MenuItem(self._text.replace("&", ""), self.trigger, icon=icon or None,
                        shortcut=_key_text(self.shortcut()),
                        checked=self._checked if self._checkable else None, enabled=self.isEnabled(),
                        submenu=self._menu._items() if self._menu is not None else None, description=tip)


def menu_items(actions, tips_visible=False, collapsible=True):
    """The visible actions as the kit's menu items; when collapsible, no leading, trailing or doubled
    separators (Qt's default)."""
    out = []
    for a in actions:
        item = a._item()
        if item is None:
            continue
        if not tips_visible and item != "-":
            item.description = ""
        if item == "-" and collapsible and (not out or out[-1] == "-"):
            continue
        out.append(item)
    while collapsible and out and out[-1] == "-":
        out.pop()
    return out


class _Exclusive:
    """What QActionGroup and QButtonGroup share: the members, of which one may be checked."""

    def _setup_group(self):
        self._members = []
        self._exclusive = True

    def _member_checked(self, member):
        if self._exclusive:
            for m in self._members:
                if m is not member and m.isChecked():
                    m.setChecked(False)

    def _checked_member(self):
        return next((m for m in self._members if m.isChecked()), None)


class QActionGroup(_Exclusive):
    class ExclusionPolicy:
        None_, Exclusive, ExclusiveOptional = 0, 1, 2

    triggered = Signal(object)
    hovered = Signal(object)

    def __init__(self, parent=None):
        self._setup_group()
        self._policy = self.ExclusionPolicy.Exclusive
        self._enabled = self._visible = True

    def addAction(self, action, *args):  # noqa: N802 - (action) / (text) / (icon, text)
        if not isinstance(action, QAction):
            action = QAction(action, *args)
        if action._group is not None and action._group is not self:
            action._group.removeAction(action)
        if action not in self._members:
            self._members.append(action)
            action._group = self
            if action.isChecked():
                self._member_checked(action)
        return action

    def removeAction(self, action):  # noqa: N802
        if action in self._members:
            self._members.remove(action)
            action._group = None

    def actions(self):
        return list(self._members)

    def checkedAction(self):  # noqa: N802
        return self._checked_member()

    def exclusionPolicy(self):  # noqa: N802
        return self._policy

    def setExclusionPolicy(self, policy):  # noqa: N802
        self._policy = int(policy)
        self._exclusive = self._policy != self.ExclusionPolicy.None_

    def isExclusive(self):  # noqa: N802
        return self._exclusive

    def setExclusive(self, b):  # noqa: N802
        self.setExclusionPolicy(self.ExclusionPolicy.Exclusive if b else self.ExclusionPolicy.None_)

    def isEnabled(self):  # noqa: N802
        return self._enabled

    def setEnabled(self, b):  # noqa: N802
        self._enabled = bool(b)
        for a in self._members:
            a.changed.emit()

    def setDisabled(self, b):  # noqa: N802
        self.setEnabled(not b)

    def isVisible(self):  # noqa: N802
        return self._visible

    def setVisible(self, b):  # noqa: N802
        self._visible = bool(b)
        for a in self._members:
            a.changed.emit()


# ==========================================================================
# Menus


class QMenu:
    """A Blender pulldown menu: opened by a QMenuBar, a button, a context click or popup()."""
    aboutToShow = Signal()
    aboutToHide = Signal()
    triggered = Signal(object)
    hovered = Signal(object)

    def __init__(self, title="", parent=None):
        if isinstance(title, QWidget):
            title, parent = "", title
        self._title = str(title)
        self._actions: List[QAction] = []
        self._relays = {}  # action -> the slot relaying its triggered() to this menu
        self._parent = parent
        self._icon = None
        self._default = self._active = None
        self._tips_visible = False
        self._collapsible = True
        self._menu_action = None

    def title(self):
        return self._title

    def setTitle(self, t):  # noqa: N802
        self._title = str(t)
        if self._menu_action is not None:
            self._menu_action.setText(self._title)
        self._changed()

    def icon(self):
        return self._icon or QIcon()

    def setIcon(self, icon):  # noqa: N802
        self._icon = icon
        self._changed()

    def menuAction(self):  # noqa: N802
        """The action that opens this menu inside another (its text is the title)."""
        if self._menu_action is None:
            self._menu_action = QAction(self._title, self._parent)
            self._menu_action.setMenu(self)
            self._menu_action.setIcon(self._icon)
        return self._menu_action

    # -- adding -------------------------------------------------------------------------------------------------
    def addAction(self, *args, **kw):  # noqa: N802 - (action) / (text[, slot[, shortcut]]) / (icon, text, ...)
        return self.insertAction(None, self._action_of(*args, **kw))

    def _action_of(self, action, *rest, **kw):
        if isinstance(action, QAction):
            return action
        icon = action if isinstance(action, QIcon) else None
        if icon is not None:
            action, rest = rest[0], rest[1:]
        a = QAction(str(action), self._parent)
        a.setIcon(icon)
        slot = rest[0] if rest else kw.get("slot")
        if callable(slot):
            a.triggered.connect(lambda *_: slot())
        shortcut = rest[1] if len(rest) > 1 else kw.get("shortcut")
        if shortcut:
            a.setShortcut(shortcut)
        return a

    def addActions(self, actions):  # noqa: N802
        for a in actions:
            self.addAction(a)

    def insertAction(self, before, action):  # noqa: N802
        if action in self._actions:
            self._actions.remove(action)
        else:
            action.changed.connect(self._changed)
            self._relays[action] = self._relay(action)
            action.triggered.connect(self._relays[action])
        self._actions.insert(self._actions.index(before) if before in self._actions else len(self._actions),
                             action)
        action._attach(self)
        self._changed()
        return action

    def _relay(self, action):
        def relay(*_):
            if action in self._actions:
                self._triggered(action)
        return relay

    def insertActions(self, before, actions):  # noqa: N802
        for a in actions:
            self.insertAction(before, a)

    def removeAction(self, action):  # noqa: N802
        if action in self._actions:
            self._actions.remove(action)
            action._detach(self)
            action.changed.disconnect(self._changed)
            action.triggered.disconnect(self._relays.pop(action))
            self._changed()

    def _triggered(self, action):
        """triggered() of this menu, the menus it is in and their menu bar."""
        self.triggered.emit(action)
        owner = getattr(self, "_owner_menu", None)
        if owner is not None:
            owner._triggered(action)
        elif getattr(self, "_bar", None) is not None:
            self._bar.triggered.emit(action)

    def addMenu(self, menu, *rest):  # noqa: N802 - (menu) -> its action; (title) / (icon, title) -> the menu
        sub = _submenu(menu, rest, self._parent)
        sub._owner_menu = self
        action = self.insertAction(None, sub.menuAction())
        return action if isinstance(menu, QMenu) else sub

    def insertMenu(self, before, menu):  # noqa: N802
        menu._owner_menu = self
        return self.insertAction(before, menu.menuAction())

    def addSeparator(self):  # noqa: N802
        return self.insertSeparator(None)

    def insertSeparator(self, before):  # noqa: N802
        a = QAction("", self._parent)
        a.setSeparator(True)
        return self.insertAction(before, a)

    def addSection(self, *args):  # noqa: N802 - (text) / (icon, text)
        return self.insertSection(None, *args)

    def insertSection(self, before, *args):  # noqa: N802
        """A separator with a title: Blender's menu label row (uiItemL in a menu)."""
        a = QAction(*args)
        a.setEnabled(False)
        self.insertSeparator(before)
        return self.insertAction(before, a)

    def actions(self):
        return list(self._actions)

    def isEmpty(self):  # noqa: N802
        return not any(a.isVisible() and not a.isSeparator() for a in self._actions)

    def clear(self):
        for a in list(self._actions):
            self.removeAction(a)

    # -- defaults and options -------------------------------------------------------------------------------------
    def defaultAction(self):  # noqa: N802
        return self._default

    def setDefaultAction(self, action):  # noqa: N802
        self._default = action

    def activeAction(self):  # noqa: N802
        return self._active

    def setActiveAction(self, action):  # noqa: N802
        self._active = action
        if action is not None:
            action.hover()
            self.hovered.emit(action)

    def toolTipsVisible(self):  # noqa: N802
        return self._tips_visible

    def setToolTipsVisible(self, b):  # noqa: N802
        self._tips_visible = bool(b)
        self._changed()

    def separatorsCollapsible(self):  # noqa: N802
        return self._collapsible

    def setSeparatorsCollapsible(self, b):  # noqa: N802
        self._collapsible = bool(b)
        self._changed()

    # -- the kit's items ----------------------------------------------------------------------------------------
    def _items(self):
        return menu_items(self._actions, self._tips_visible, self._collapsible)

    def _changed(self, *_):
        owner = getattr(self, "_bar", None) or getattr(self, "_owner_menu", None)
        if owner is not None:
            owner._changed()

    def _root_widget(self):
        """The widget this menu belongs to: its parent, or the window of the menu bar it is in."""
        owner = getattr(self, "_bar", None) or getattr(self, "_owner_menu", None)
        if isinstance(self._parent, QWidget):
            return self._parent
        return owner._root_widget() if owner is not None else None

    # -- showing ------------------------------------------------------------------------------------------------
    def exec(self, pos=None, at=None):
        self.popup(pos, at)
        return None

    exec_ = exec

    def popup(self, pos=None, at=None):
        win = self._window_widget()
        if win is None or win._ctx is None:
            return
        where = (pos.x(), pos.y()) if pos is not None else None
        self.aboutToShow.emit()
        win._ctx.open_menu(None, self._items(), None, where)

    def _window_widget(self):
        w = self._root_widget()
        if w is None:
            return next(iter(_open_windows()), None)
        return w.window()


def _submenu(menu, rest, parent):
    """addMenu's argument as a menu: the menu itself, or a new one from (title) / (icon, title)."""
    if isinstance(menu, QMenu):
        return menu
    icon = menu if isinstance(menu, QIcon) else None
    sub = QMenu(str(rest[0] if icon is not None else menu), parent)
    sub._icon = icon
    return sub


def _open_windows():
    import gc
    return [o for o in gc.get_objects() if isinstance(o, QWidget) and o._window is not None]


class QMenuBar:
    """Blender's header menu row: one pulldown button per menu (and per plain action)."""
    triggered = Signal(object)
    hovered = Signal(object)

    def __init__(self, parent=None):
        self._actions: List[QAction] = []
        self._owner = parent if isinstance(parent, QWidget) else None
        self._node = buipy.KitNode("menubar")
        self._corner = {}
        self._buttons = {}  # plain action -> its QToolButton
        self._native = False
        self._active = None

    def _root_widget(self):
        return self._owner

    def addMenu(self, menu, *rest):  # noqa: N802 - (menu) -> its action; (title) / (icon, title) -> the menu
        sub = _submenu(menu, rest, self._owner)
        sub._bar = self
        action = self.insertAction(None, sub.menuAction())
        return action if isinstance(menu, QMenu) else sub

    def insertMenu(self, before, menu):  # noqa: N802
        menu._bar = self
        return self.insertAction(before, menu.menuAction())

    def menus(self):
        return [a.menu() for a in self._actions if a.menu() is not None]

    def addAction(self, action, *rest):  # noqa: N802
        """A plain action in the bar opens nothing: it is a button that triggers it."""
        if not isinstance(action, QAction):
            action = QAction(str(action), self._owner)
            if rest and callable(rest[0]):
                action.triggered.connect(lambda *_: rest[0]())
        return self.insertAction(None, action)

    def insertAction(self, before, action):  # noqa: N802
        if action in self._actions:
            self._actions.remove(action)
        else:
            action.changed.connect(self._changed)
        self._actions.insert(self._actions.index(before) if before in self._actions else len(self._actions),
                             action)
        action._attach(self)
        self._changed()
        return action

    def addSeparator(self):  # noqa: N802
        return self.insertSeparator(None)

    def insertSeparator(self, before):  # noqa: N802
        a = QAction("", self._owner)
        a.setSeparator(True)
        return self.insertAction(before, a)

    def removeAction(self, action):  # noqa: N802
        if action in self._actions:
            self._actions.remove(action)
            action._detach(self)
            action.changed.disconnect(self._changed)
            button = self._buttons.pop(action, None)
            if button is not None:
                button.setDefaultAction(None)
            self._changed()

    def actions(self):
        return list(self._actions)

    def clear(self):
        for a in list(self._actions):
            self.removeAction(a)

    def activeAction(self):  # noqa: N802
        return self._active

    def setActiveAction(self, action):  # noqa: N802
        self._active = action
        if action is not None:
            action.hover()
            self.hovered.emit(action)

    def cornerWidget(self, corner=Qt.TopRightCorner):  # noqa: N802
        return self._corner.get(corner)

    def setCornerWidget(self, widget, corner=Qt.TopRightCorner):  # noqa: N802
        """Blender's header keeps a right-hand side: the top-right widget goes there."""
        old = self._corner.get(corner)
        if old is not None and old.parent is self._node:
            self._node.remove(old)
        self._corner[corner] = widget
        self._changed()

    def isNativeMenuBar(self):  # noqa: N802
        return self._native

    def setNativeMenuBar(self, b):  # noqa: N802
        self._native = False  # always Blender's own bar, on every platform

    def setVisible(self, b):  # noqa: N802
        self._node.set("visible", bool(b))

    def isVisible(self):  # noqa: N802
        return bool(self._node.get("visible"))

    def _changed(self, *_):
        for c in self._node.children:
            self._node.remove(c)
        for a in self._actions:
            if not a.isVisible():
                continue
            if a.isSeparator():
                self._node.add(buipy.KitNode("separator"))
                continue
            if a.menu() is None:
                self._node.add(self._button(a))
                continue
            b = buipy.KitNode("menubutton")
            b.set("text", a.text().replace("&", ""))
            b.set("enabled", a.isEnabled())
            b.set_menu(a.menu()._items())
            self._node.add(b)
        for corner in (Qt.TopLeftCorner, Qt.TopRightCorner):
            w = self._corner.get(corner)
            if w is not None:
                if w.parent is not None:
                    w.parent.remove(w)
                self._node.add(w)

    def _button(self, action):
        """A plain action in the bar: a flat tool button that triggers it."""
        b = self._buttons.get(action)
        if b is None:
            b = self._buttons[action] = QToolButton()
            b.setDefaultAction(action)
            b.setAutoRaise(True)
            b.setToolButtonStyle(Qt.ToolButtonTextOnly)
            b.triggered.connect(self.triggered.emit)
        if b.parent is not None:
            b.parent.remove(b)
        return b

    def absolutePos(self):  # noqa: N802
        r = self._node.rect
        return r[0], r[1]


# ==========================================================================
# Tool buttons and button groups


class QToolButton(ButtonMenuMore, QAbstractButton):
    """A button for one action (setDefaultAction), flat until hovered with autoRaise (Blender's header
    icons), an arrow icon, and a menu."""
    KIND = "button"
    DelayedPopup, MenuButtonPopup, InstantPopup = 0, 1, 2
    triggered = Signal(object)
    _ARROWS = {Qt.UpArrow: "TRIA_UP", Qt.DownArrow: "TRIA_DOWN", Qt.LeftArrow: "TRIA_LEFT",
               Qt.RightArrow: "TRIA_RIGHT"}

    def _setup(self):
        self._action = None
        self._arrow = Qt.NoArrow
        self._popup_mode = self.DelayedPopup
        self._style = Qt.ToolButtonIconOnly
        self._auto_raise = False

    def defaultAction(self):  # noqa: N802
        return self._action

    def setDefaultAction(self, action):  # noqa: N802
        """The button shows the action (text, icon, tip, enabled, checked) and triggers it when clicked."""
        if self._action is not None:
            self._action.changed.disconnect(self._sync)
            self._action._detach(self)
        self._action = action
        if action is None:
            self.setIcon(getattr(self, "_own_icon", None))
        else:
            action._attach(self)
            action.changed.connect(self._sync)
            if action.menu() is not None and self.menu() is None:
                self.setMenu(action.menu())
            self._sync()

    def _sync(self, *_):
        a = self._action
        self.setText(a.iconText())
        self.setToolTip(a.toolTip())
        self.set("enabled", a.isEnabled())
        self.set("visible", a.isVisible())
        self.setCheckable(a.isCheckable())
        self.setChecked(a.isChecked())
        self._apply_icon()

    def autoRaise(self):  # noqa: N802
        return self._auto_raise

    def setAutoRaise(self, b):  # noqa: N802
        """No emboss until hovered: Blender's UI_EMBOSS_NONE header buttons."""
        self._auto_raise = bool(b)
        self.set("emboss", not b)

    def arrowType(self):  # noqa: N802
        return self._arrow

    def setArrowType(self, arrow):  # noqa: N802
        self._arrow = int(arrow)
        self._apply_icon()

    def popupMode(self):  # noqa: N802
        return self._popup_mode

    def setPopupMode(self, mode):  # noqa: N802
        self._popup_mode = int(mode)

    def toolButtonStyle(self):  # noqa: N802
        return self._style

    def setToolButtonStyle(self, style):  # noqa: N802
        self._style = int(style)
        self._apply_icon()

    def setIcon(self, icon):  # noqa: N802
        self._own_icon = icon
        self._apply_icon()

    def icon(self):
        return getattr(self, "_own_icon", None) or (self._action.icon() if self._action else QIcon())

    def _apply_icon(self):
        name = self._ARROWS.get(self._arrow) or getattr(self.icon(), "name", "")
        if self._style == Qt.ToolButtonTextOnly:
            name = ""
        self.set("icon", name or "NONE")
        self.set("icon_only", bool(name) and self._style == Qt.ToolButtonIconOnly)

    def _event(self, kind, ref, a, b):
        if kind != EV_CLICK:
            return
        if self.menu() is not None and (self._action is None or self._popup_mode == self.InstantPopup):
            self.showMenu()
        elif self._action is not None:
            self._action.trigger()
            self.triggered.emit(self._action)
            self.clicked.emit(self._action.isChecked())
        else:
            QAbstractButton._event(self, kind, ref, a, b)


class QButtonGroup(_Exclusive):
    """Buttons with ids, of which (when exclusive) one is checked."""
    buttonClicked = Signal(object)
    buttonPressed = Signal(object)
    buttonReleased = Signal(object)
    buttonToggled = Signal(object, bool)
    idClicked = Signal(int)
    idPressed = Signal(int)
    idReleased = Signal(int)
    idToggled = Signal(int, bool)

    def __init__(self, parent=None):
        self._setup_group()
        self._ids = {}
        self._slots = {}  # button -> its (signal, slot) connections

    def addButton(self, b, id=-1):  # noqa: N802, A002
        """Without an id, ids count down from -2 (Qt)."""
        if b in self._members:
            return
        if id == -1:
            id = min([-1, *self._ids.values()]) - 1  # noqa: A001
        self._members.append(b)
        self._ids[b] = int(id)
        b._group = self
        self._slots[b] = slots = (
            (b.clicked, self._relay(b, self.buttonClicked, self.idClicked)),
            (b.pressed, self._relay(b, self.buttonPressed, self.idPressed)),
            (b.released, self._relay(b, self.buttonReleased, self.idReleased)),
            (b.toggled, lambda on, b=b: self._toggled(b, on)))
        for signal, slot in slots:
            signal.connect(slot)

    def _relay(self, b, button_signal, id_signal):
        def relay(*_):
            if b in self._members:
                button_signal.emit(b)
                id_signal.emit(self._ids[b])
        return relay

    def _toggled(self, b, on):
        if b not in self._members:
            return
        if on:
            self._member_checked(b)
        self.buttonToggled.emit(b, on)
        self.idToggled.emit(self._ids[b], on)

    def removeButton(self, b):  # noqa: N802
        if b in self._members:
            self._members.remove(b)
            del self._ids[b]
            for signal, slot in self._slots.pop(b):
                signal.disconnect(slot)
            b._group = None

    def buttons(self):
        return list(self._members)

    def button(self, id):  # noqa: A002
        return next((b for b, i in self._ids.items() if i == id), None)

    def id(self, b):  # noqa: A003
        return self._ids.get(b, -1)

    def setId(self, b, id):  # noqa: N802, A002
        if b in self._ids and id != -1:
            self._ids[b] = int(id)

    def checkedButton(self):  # noqa: N802
        return self._checked_member()

    def checkedId(self):  # noqa: N802
        b = self._checked_member()
        return self._ids[b] if b is not None else -1

    def exclusive(self):
        return self._exclusive

    def setExclusive(self, b):  # noqa: N802
        self._exclusive = bool(b)
