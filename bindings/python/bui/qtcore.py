"""QtCore and QtGui pieces a widget program uses: QSettings (an INI file per application, Qt's
groups and arrays), QKeySequence, QShortcut (bound with the kit's key bindings: the same as
Blender's keymap), QClipboard (the window's clipboard).

SPDX-License-Identifier: GPL-2.0-or-later
"""

import configparser
import contextlib
import os
import sys
import tempfile

from bui import host
from bui.qtsyntax import QWidget, Signal


# ==========================================================================
# QSettings


def _config_dir():
    if sys.platform == "win32":
        return os.environ.get("APPDATA") or os.path.expanduser("~")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Preferences")
    return os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")


def _to_text(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (list, tuple)):
        return ", ".join(_to_text(v) for v in value)
    return str(value)


def _convert(text, type_):
    if type_ is None or text is None:
        return text
    if type_ is bool:
        return str(text).strip().lower() in ("true", "1", "yes", "on")
    if type_ in (list, tuple):
        items = [t.strip() for t in str(text).split(",")] if str(text) else []
        return type_(items)
    return type_(text)


class QSettings:
    """Settings stored as an INI file per organization and application, as Qt's IniFormat does
    (on Windows Qt's native format is the registry; here it is an INI file under %APPDATA%)."""
    NativeFormat, IniFormat = 0, 1
    UserScope, SystemScope = 0, 1
    NoError, AccessError, FormatError = 0, 1, 2
    _org, _app = "", ""  # QCoreApplication.setOrganizationName / setApplicationName

    def __init__(self, *args):
        self._format, self._scope = self.IniFormat, self.UserScope
        if len(args) == 2 and isinstance(args[1], int):  # (fileName, format)
            self._file, self._org_name, self._app_name = str(args[0]), "", ""
        else:
            strs = [a for a in args if isinstance(a, str)]
            org = strs[0] if strs else (QSettings._org or "BUI")
            app = strs[1] if len(strs) > 1 else (QSettings._app or "")
            self._org_name, self._app_name = org, app
            ext = ".ini" if sys.platform == "win32" else ".conf"
            self._file = os.path.join(_config_dir(), org, (app or org) + ext)
        self._groups = []
        self._arrays = []  # (prefix, index)
        self._data = {}
        self._status = self.NoError
        self._load()

    # -- the file -------------------------------------------------------------------------------------
    def _load(self):
        p = configparser.RawConfigParser(interpolation=None)
        p.optionxform = str
        try:
            p.read(self._file, encoding="utf-8")
        except (OSError, configparser.Error):
            self._status = self.FormatError
            return
        for section in p.sections():
            for k, v in p.items(section):
                key = k.replace("\\", "/")
                self._data[key if section == "General" else section + "/" + key] = v

    def sync(self):
        p = configparser.RawConfigParser(interpolation=None)
        p.optionxform = str
        for key in sorted(self._data):
            section, _, rest = key.partition("/")
            if not rest:
                section, rest = "General", key
            if not p.has_section(section):
                p.add_section(section)
            p.set(section, rest.replace("/", "\\"), self._data[key])
        try:
            os.makedirs(os.path.dirname(self._file) or ".", exist_ok=True)
            # write beside the target then swap, so a failed write never truncates the settings
            fd, tmp = tempfile.mkstemp(dir=os.path.dirname(self._file) or ".", suffix=".tmp")
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as f:
                    p.write(f, space_around_delimiters=False)  # width=640, as Qt writes it
                os.replace(tmp, self._file)
            except BaseException:
                with contextlib.suppress(OSError):
                    os.unlink(tmp)
                raise
            self._status = self.NoError
        except OSError:
            self._status = self.AccessError

    def fileName(self):  # noqa: N802
        return self._file

    def format(self):
        return self._format

    def scope(self):
        return self._scope

    def status(self):
        return self._status

    def isWritable(self):  # noqa: N802
        return os.access(os.path.dirname(self._file) or ".", os.W_OK) or not os.path.exists(
            os.path.dirname(self._file))

    def organizationName(self):  # noqa: N802
        return self._org_name

    def applicationName(self):  # noqa: N802
        return self._app_name

    # -- keys ---------------------------------------------------------------------------------------------
    def _prefix(self):
        parts = list(self._groups)
        for prefix, index, _ in self._arrays:
            parts.append("%s/%d" % (prefix, index + 1) if index >= 0 else prefix)
        return "/".join(p for p in parts if p)

    def _key(self, key):
        p = self._prefix()
        return (p + "/" + key) if p else key

    def value(self, key, defaultValue=None, type=None):  # noqa: N803,A002
        text = self._data.get(self._key(str(key)))
        return _convert(text, type) if text is not None else defaultValue

    def setValue(self, key, value):  # noqa: N802
        self._data[self._key(str(key))] = _to_text(value)
        self.sync()

    def contains(self, key):
        return self._key(str(key)) in self._data

    def remove(self, key):
        full = self._key(str(key)) if key else self._prefix()
        for k in [k for k in self._data if not full or k == full or k.startswith(full + "/")]:
            del self._data[k]
        self.sync()

    def clear(self):
        self._data = {}
        self.sync()

    def _under(self):
        p = self._prefix()
        return [k[len(p) + 1:] if p else k for k in self._data if not p or k.startswith(p + "/")]

    def allKeys(self):  # noqa: N802
        return sorted(self._under())

    def childKeys(self):  # noqa: N802
        return sorted(k for k in self._under() if "/" not in k)

    def childGroups(self):  # noqa: N802
        return sorted({k.split("/", 1)[0] for k in self._under() if "/" in k})

    def beginGroup(self, prefix):  # noqa: N802
        self._groups.append(str(prefix).strip("/"))

    def endGroup(self):  # noqa: N802
        if self._groups:
            self._groups.pop()

    def group(self):
        return "/".join(self._groups)

    # -- arrays: prefix/size, prefix/1/key, prefix/2/key ... -------------------------------------------------
    def beginReadArray(self, prefix):  # noqa: N802
        size = int(self.value(str(prefix) + "/size", 0, int))
        self._arrays.append([str(prefix), -1, None])  # [prefix, index, size written (None: reading)]
        return size

    def beginWriteArray(self, prefix, size=-1):  # noqa: N802
        self._arrays.append([str(prefix), -1, max(0, int(size))])

    def setArrayIndex(self, i):  # noqa: N802
        entry = self._arrays[-1]
        entry[1] = int(i)
        if entry[2] is not None:
            entry[2] = max(entry[2], entry[1] + 1)  # the size is the highest index written + 1, as in Qt

    def endArray(self):  # noqa: N802
        if not self._arrays:
            return
        prefix, _, written = self._arrays.pop()
        if written is not None:
            self._data[self._key(prefix + "/size")] = str(written)
            self.sync()


# ==========================================================================
# Keys


class QKeySequence:
    """A key combination as Qt writes it ("Ctrl+Shift+S")."""
    NativeText, PortableText = 0, 1
    (UnknownKey, HelpContents, WhatsThis, Open, Close, Save, New, Delete, Cut, Copy, Paste, Undo, Redo,
     Back, Forward, Refresh, ZoomIn, ZoomOut, Print, AddTab, NextChild, PreviousChild, Find, FindNext,
     FindPrevious, Replace, SelectAll, Bold, Italic, Underline) = range(30)
    Quit, SaveAs = 65, 63
    _STANDARD = {Open: "Ctrl+O", Close: "Ctrl+W", Save: "Ctrl+S", New: "Ctrl+N", Delete: "Del", Cut: "Ctrl+X",
                 Copy: "Ctrl+C", Paste: "Ctrl+V", Undo: "Ctrl+Z", Redo: "Ctrl+Shift+Z", Refresh: "F5",
                 ZoomIn: "Ctrl++", ZoomOut: "Ctrl+-", Print: "Ctrl+P", Find: "Ctrl+F", FindNext: "F3",
                 Replace: "Ctrl+H", SelectAll: "Ctrl+A", Bold: "Ctrl+B", Italic: "Ctrl+I", Underline: "Ctrl+U",
                 HelpContents: "F1", Quit: "Ctrl+Q", SaveAs: "Ctrl+Shift+S", AddTab: "Ctrl+T"}
    _MODS = ("Ctrl", "Shift", "Alt", "Meta")

    def __init__(self, key=""):
        if isinstance(key, QKeySequence):
            text = key._text
        elif isinstance(key, int):
            text = self._STANDARD.get(key, "")
        else:
            text = str(key)
        self._text = self._normalize(text)

    @classmethod
    def _normalize(cls, text):
        if not text:
            return ""
        parts = [p.strip() for p in text.replace("++", "+Plus").split("+") if p.strip()]
        mods = [m for m in cls._MODS if any(p.lower() in (m.lower(), "control" if m == "Ctrl" else "") for p in parts)]
        keys = [p for p in parts if p.lower() not in ("ctrl", "control", "shift", "alt", "meta")]
        key = (keys[-1] if keys else "").replace("Plus", "+")
        key = key.upper() if len(key) == 1 else key[:1].upper() + key[1:]
        return "+".join(mods + ([key] if key else []))

    @staticmethod
    def fromString(text, fmt=1):  # noqa: N802
        return QKeySequence(text)

    @staticmethod
    def keyBindings(key):  # noqa: N802
        return [QKeySequence(key)]

    def toString(self, fmt=1):  # noqa: N802
        return self._text

    def isEmpty(self):  # noqa: N802
        return not self._text

    def count(self):
        return 1 if self._text else 0

    def _combo(self):
        """The kit's spelling: "ctrl+shift+s"."""
        return self._text.lower()

    def __eq__(self, other):
        return QKeySequence(other)._text == self._text if isinstance(other, (str, int, QKeySequence)) else False

    def __hash__(self):
        return hash(self._text)

    def __str__(self):
        return self._text

    def __repr__(self):
        return "QKeySequence(%r)" % self._text


_shortcuts = []  # every QShortcut: bound when its window opens (the bindings live on the window)


class QShortcut:
    """A key combination that emits activated() while its widget's window has the keyboard: bound
    through the kit (BK_bind), which is how Blender's keymap runs too."""
    activated = Signal()
    activatedAmbiguously = Signal()

    def __init__(self, key=None, parent=None, member=None, *a, **kw):
        if isinstance(key, QWidget) and parent is None:
            key, parent = None, key
        self._key = QKeySequence(key or "")
        self._parent = parent
        self._enabled = True
        self._auto_repeat = True
        self._bound = set()
        if callable(member):
            self.activated.connect(member)
        _shortcuts.append(self)
        self._bind()

    def _fire(self):
        if self._enabled:
            self.activated.emit()

    def _widget(self):
        """The widget whose window the key is bound on."""
        return self._parent

    def _bind(self):
        widget = self._widget()
        win = widget.window() if widget is not None else None
        ctx = getattr(win, "_ctx", None)
        combo = self._key._combo()
        if ctx is not None and combo and (id(ctx), combo) not in self._bound:
            # the kit has no unbind: a stale combo (after setKey) is left bound but stops firing
            ctx.bind(combo, lambda combo=combo: self._key._combo() == combo and self._fire())
            self._bound.add((id(ctx), combo))

    def key(self):
        return self._key

    def setKey(self, key):  # noqa: N802
        self._key = QKeySequence(key)
        self._bind()

    def isEnabled(self):  # noqa: N802
        return self._enabled

    def setEnabled(self, b):  # noqa: N802
        self._enabled = bool(b)

    def autoRepeat(self):  # noqa: N802
        return self._auto_repeat

    def setAutoRepeat(self, b):  # noqa: N802
        self._auto_repeat = bool(b)

    def parentWidget(self):  # noqa: N802
        return self._parent


def bind_shortcuts(window):
    """Bind the shortcuts of `window`'s widgets (QWidget calls it when the window opens)."""
    for s in _shortcuts:
        widget = s._widget()
        if widget is not None and widget.window() is window:
            s._bind()


# ==========================================================================
# Clipboard


class QClipboard:
    """The system clipboard, through the open window (the kit's clipboard service)."""
    Clipboard, Selection, FindBuffer = 0, 1, 2
    changed = Signal(int)
    dataChanged = Signal()
    _instance = None

    @classmethod
    def instance(cls):
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    def __init__(self):
        self._local = ""  # used until a window is open: the system clipboard is reached through one

    @staticmethod
    def _ctx():
        windows = list(host._windows)
        return windows[0].ctx if windows else None

    def text(self, mode=0):
        ctx = self._ctx()
        return ctx.clipboard_get() if ctx is not None else self._local

    def setText(self, text, mode=0):  # noqa: N802
        text = str(text)
        ctx = self._ctx()
        if ctx is not None:
            ctx.clipboard_set(text)
        self._local = text
        self.changed.emit(mode)
        self.dataChanged.emit()

    def clear(self, mode=0):
        self.setText("", mode)

    def supportsSelection(self):  # noqa: N802
        return False
