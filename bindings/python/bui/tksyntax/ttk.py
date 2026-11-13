"""tkinter.ttk on the BUI kit.

Themed widgets reuse the classic implementations and add ttk's state API
(``state`` / ``instate``); the theme is always Blender's, so ``Style`` keeps
settings without changing the look.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

import buipy

from bui.components.core import EV_CHANGE, EV_CLICK, EV_TOGGLE

from . import _core, _widgets
from ._core import (BaseWidget, EventType, TclError, Variable, XView, YView, _call,
                    _command, _pixels, _var_value, getboolean)
from ._widgets import _chars, _image_of

__all__ = ["Button", "Checkbutton", "Combobox", "Entry", "Frame", "Label", "Labelframe",
           "LabelFrame", "Menubutton", "Notebook", "Panedwindow", "PanedWindow",
           "Progressbar", "Radiobutton", "Scale", "Scrollbar", "Separator", "Sizegrip",
           "Spinbox", "Style", "Treeview", "LabeledScale", "OptionMenu", "tclobjs_to_py",
           "setup_master"]

__version__ = "0.3.1"


def tclobjs_to_py(adict):
    """Returns adict with its values converted from Tcl objects to Python objects."""
    return dict(adict)


def _convert_stringval(value):
    """Converts a value to, hopefully, a more appropriate Python object
    (tkinter.ttk's rule: integers become int)."""
    value = str(value)
    try:
        return int(value)
    except (TypeError, ValueError):
        return value


def setup_master(master=None):
    """If master is not None, itself is returned. Otherwise the default root."""
    return master if master is not None else _core._get_default_root()


# ==========================================================================
# Style


class Style:
    """Manipulate style database (kept; widgets always use Blender's theme)."""

    _name = "ttk::style"
    _db: dict = {}
    _maps: dict = {}
    _layouts: dict = {}
    _elements: dict = {}
    _themes = ["blender", "default", "clam", "alt", "classic"]
    _current = "blender"

    def __init__(self, master=None):
        self.master = setup_master(master)
        self.tk = self.master.tk

    def configure(self, style, query_opt=None, **kw):
        """Query or sets the default value of the specified option(s) in style."""
        opts = Style._db.setdefault(style, {})
        if query_opt is not None:
            return opts.get(query_opt.lstrip("-"), "")
        if not kw:
            return dict(opts)
        opts.update(kw)
        return None

    def map(self, style, query_opt=None, **kw):
        """Query or sets dynamic values of the specified option(s) in style."""
        maps = Style._maps.setdefault(style, {})
        if query_opt is not None:
            return maps.get(query_opt.lstrip("-"), [])
        if not kw:
            return dict(maps)
        maps.update(kw)
        return None

    def lookup(self, style, option, state=None, default=None):
        """Returns the value specified for option in style."""
        option = option.lstrip("-")
        if state:
            for spec in Style._maps.get(style, {}).get(option, []):
                if all(s in state for s in spec[:-1]):
                    return spec[-1]
        return Style._db.get(style, {}).get(option, default if default is not None else "")

    def layout(self, style, layoutspec=None):
        """Define the widget layout for given style."""
        if layoutspec is None:
            return Style._layouts.get(style, [])
        Style._layouts[style] = layoutspec
        return None

    def element_create(self, elementname, etype, *args, **kw):
        """Create a new element in the current theme of given etype."""
        Style._elements[elementname] = (etype, args, kw)

    def element_names(self):
        """Returns the list of elements defined in the current theme."""
        return tuple(Style._elements)

    def element_options(self, elementname):
        """Return the list of elementname's options."""
        return tuple(Style._elements.get(elementname, (None, (), {}))[2])

    def theme_create(self, themename, parent=None, settings=None):
        """Creates a new theme."""
        if themename in Style._themes:
            raise TclError('Theme %s already exists' % themename)
        Style._themes.append(themename)
        if settings:
            self.theme_settings(themename, settings)

    def theme_settings(self, themename, settings):
        """Temporarily sets the current theme to themename, apply specified settings."""
        for style, spec in settings.items():
            if "configure" in spec:
                self.configure(style, **spec["configure"])
            if "map" in spec:
                self.map(style, **spec["map"])
            if "layout" in spec:
                self.layout(style, spec["layout"])

    def theme_names(self):
        """Returns a list of all known themes."""
        return tuple(Style._themes)

    def theme_use(self, themename=None):
        """If themename is None, returns the theme in use, otherwise, set
        the current theme to themename (the look stays Blender's)."""
        if themename is None:
            return Style._current
        if themename not in Style._themes:
            raise TclError('can\'t find theme "%s"' % themename)
        Style._current = themename
        return None


# ==========================================================================
# Widget base: ttk states


_STATE_FLAGS = ("active", "disabled", "focus", "pressed", "selected", "background",
                "readonly", "alternate", "invalid", "hover")


class Widget(_core.Widget):
    """Base class for Tk themed widgets."""

    _TTK = True

    def __init__(self, master, widgetname, kw=None):
        _core.Widget.__init__(self, master, widgetname, {}, kw or {})

    def winfo_class(self):
        return getattr(self, "_class", None) or "T" + type(self).__name__

    def _apply(self, key, value):
        if key == "state":
            self._flags().discard("disabled")
            self._flags().discard("readonly")
            if str(value) in ("disabled", "readonly"):
                self._flags().add(str(value))
            self._sync_state()
        elif key in ("style", "padding", "takefocus", "class"):
            if key == "padding" and self._KIND == "frame":
                self._node.set("padding", int(max(_core._pads(value))))
        else:
            super()._apply(key, value)

    def _flags(self):
        return self.__dict__.setdefault("_state_flags", set())

    def _sync_state(self):
        flags = self._flags()
        if self._node is not None:
            self._node.set("enabled", "disabled" not in flags and "readonly" not in flags or
                           (self._KIND == "dropdown" and "disabled" not in flags))

    def state(self, statespec=None):
        """Modify or inquire widget state."""
        flags = self._flags()
        if statespec is None:
            return tuple(sorted(flags))
        specs = statespec.split() if isinstance(statespec, str) else statespec
        old = []
        for spec in specs:
            neg = spec.startswith("!")
            name = spec.lstrip("!")
            if name not in _STATE_FLAGS:
                raise TclError('Invalid state name %s' % name)
            if (name in flags) == neg:
                old.append(("!" if not neg else "") + name)
            (flags.discard if neg else flags.add)(name)
        self._sync_state()
        return tuple(old)

    def instate(self, statespec, callback=None, *args, **kw):
        """Test the widget's state."""
        flags = self._flags()
        specs = statespec.split() if isinstance(statespec, str) else statespec
        ok = all((s.lstrip("!") in flags) != s.startswith("!") for s in specs)
        if ok and callback is not None:
            return callback(*args, **kw)
        return ok

    def identify(self, x, y):
        """Returns the name of the element at position x, y."""
        if 0 <= int(x) < self.winfo_width() and 0 <= int(y) < self.winfo_height():
            return self.winfo_class().lstrip("T").lower() + ".border"
        return ""


def _ttk(base, default_name):
    """A themed version of a classic widget class (ttk.Widget + base)."""
    class Themed(Widget, base):
        def __init__(self, master=None, **kw):
            base.__init__(self, master, **kw)
    Themed.__name__ = Themed.__qualname__ = default_name
    Themed.__doc__ = "Ttk %s widget." % default_name
    return Themed


class Button(Widget, _widgets.Button):
    """Ttk Button widget, displays a textual label and/or image, and
    evaluates a command when pressed."""

    def __init__(self, master=None, **kw):
        _widgets.Button.__init__(self, master, **kw)

    def invoke(self):
        """Invokes the command associated with the button."""
        if "disabled" in self._flags():
            return ""
        return _widgets.Button.invoke(self)


class Checkbutton(Widget, _widgets.Checkbutton):
    """Ttk Checkbutton widget which is either in on- or off-state."""

    def __init__(self, master=None, **kw):
        _widgets.Checkbutton.__init__(self, master, **kw)

    def _sync(self):
        _widgets.Checkbutton._sync(self)
        on = self._node.get("value") != 0
        (self._flags().add if on else self._flags().discard)("selected")

    def invoke(self):
        """Toggles between the selected and deselected states and invokes
        the associated command."""
        if "disabled" in self._flags():
            return ""
        return _widgets.Checkbutton.invoke(self)


class Radiobutton(Widget, _widgets.Radiobutton):
    """Ttk Radiobutton widgets are used in groups to show or change a set
    of mutually-exclusive options."""

    def __init__(self, master=None, **kw):
        _widgets.Radiobutton.__init__(self, master, **kw)

    def _sync(self):
        _widgets.Radiobutton._sync(self)
        on = self._node.get("value") != 0
        (self._flags().add if on else self._flags().discard)("selected")

    def invoke(self):
        """Sets the option variable to the option value, selects the widget,
        and invokes the associated command."""
        if "disabled" in self._flags():
            return ""
        return _widgets.Radiobutton.invoke(self)


class Entry(Widget, _widgets.Entry):
    """Ttk Entry widget displays a one-line text string and allows that
    string to be edited by the user."""

    def __init__(self, master=None, widget=None, **kw):
        _widgets.Entry.__init__(self, master, **kw)

    def bbox(self, index):
        return _widgets.Entry.bbox(self, index)

    def validate(self):
        """Force revalidation, independent of the conditions specified by
        the validate option."""
        ok = self._validate("forced", self.get(), self.get())
        (self._flags().discard if ok else self._flags().add)("invalid")
        return ok


class Frame(Widget, _widgets.Frame):
    """Ttk Frame widget is a container, used to group other widgets together."""

    def __init__(self, master=None, **kw):
        _widgets.Frame.__init__(self, master, **kw)


class Label(Widget, _widgets.Label):
    """Ttk Label widget displays a textual label and/or image."""

    def __init__(self, master=None, **kw):
        _widgets.Label.__init__(self, master, **kw)


class Labelframe(Widget, _widgets.LabelFrame):
    """Ttk Labelframe widget is a container used to group other widgets
    together. It has an optional label."""

    def __init__(self, master=None, **kw):
        _widgets.LabelFrame.__init__(self, master, **kw)

    def winfo_class(self):
        return "TLabelframe"


LabelFrame = Labelframe


class Menubutton(Widget, _widgets.Menubutton):
    """Ttk Menubutton widget displays a textual label and/or image, and
    displays a menu when pressed."""

    def __init__(self, master=None, **kw):
        _widgets.Menubutton.__init__(self, master, **kw)


class Scrollbar(Widget, _widgets.Scrollbar):
    """Ttk Scrollbar controls the viewport of a scrollable widget."""

    def __init__(self, master=None, **kw):
        _widgets.Scrollbar.__init__(self, master, **kw)


class Scale(Widget, _widgets.Scale):
    """Ttk Scale widget is typically used to control the numeric value of
    a linked variable that varies uniformly over some range."""

    _DEFAULTS = dict(_widgets.Scale._DEFAULTS, orient="horizontal", resolution=0, value=0.0)

    def __init__(self, master=None, **kw):
        kw.setdefault("orient", "horizontal")
        _widgets.Scale.__init__(self, master, **kw)

    def _setup(self):
        _widgets.Scale._setup(self)
        self._node.set("integer", False)  # ttk scales are continuous
        self._node.set("precision", 2)
        self._node.set("step", 0.01)

    def _apply(self, key, value):
        if key == "value":
            self._node.set("value", float(value))
        else:
            super()._apply(key, value)

    def get(self, x=None, y=None):
        """Get the current value of the value option, or the value
        corresponding to the coordinates x, y if they are specified."""
        if x is not None:
            lo, hi = self._node.get("min"), self._node.get("max")
            return lo + (hi - lo) * max(0.0, min(1.0, float(x) / max(1, self.winfo_width())))
        return float(self._node.get("value"))

    def configure(self, cnf=None, **kw):
        """Modify or query scale options (setting value runs the command)."""
        retval = _widgets.Scale.configure(self, cnf, **kw)
        if not isinstance(cnf, str) and any(k in kw for k in ("from_", "to")):
            self.event_generate("<<RangeChanged>>")
        return retval

    config = configure


class Spinbox(Widget, _widgets.Spinbox):
    """Ttk Spinbox is an Entry with increment and decrement arrows."""

    def __init__(self, master=None, **kw):
        _widgets.Spinbox.__init__(self, master, **kw)

    def set(self, value):
        """Sets the value of the Spinbox to value."""
        self._set_text(str(value))

    validate = Entry.validate


class Separator(Widget):
    """Ttk Separator widget displays a horizontal or vertical separator bar."""

    _KIND = "separator"

    def __init__(self, master=None, **kw):
        Widget.__init__(self, master, "ttk::separator", kw)

    def _setup(self):
        self._node.set("line", True)

    def _apply(self, key, value):
        if key == "orient":
            # the kit's separator measures and draws across a row when vertical
            self._node.set("vertical", str(value).startswith("v"))
        else:
            super()._apply(key, value)


class Sizegrip(Widget):
    """Ttk Sizegrip allows the user to resize the containing toplevel
    window by pressing and dragging the grip."""

    _KIND = "frame"

    def __init__(self, master=None, **kw):
        Widget.__init__(self, master, "ttk::sizegrip", kw)
        self.bind("<ButtonPress-1>", self._grab)
        self.bind("<B1-Motion>", self._drag)

    def _setup(self):
        self._node.geom(0, reqwidth=round(0.6 * buipy.kit_unit()),
                        reqheight=round(0.6 * buipy.kit_unit()))
        self._node.set("cursor", 6.0)

    def _grab(self, ev):
        top = self._toplevel_window()
        self._start = (ev.x_root, ev.y_root) + tuple(top._window.size)

    def _drag(self, ev):
        x, y, w, h = self._start
        top = self._toplevel_window()
        top.geometry("%dx%d" % (max(50, w + ev.x_root - x), max(50, h + ev.y_root - y)))


# ==========================================================================
# Progressbar, Combobox, OptionMenu, LabeledScale


class Progressbar(Widget):
    """Ttk Progressbar widget shows the status of a long-running operation
    (Blender's progress bar)."""

    _KIND = "progress"
    _DEFAULTS = {"orient": "horizontal", "length": 100, "mode": "determinate",
                 "maximum": 100, "value": 0.0, "phase": 0}

    def __init__(self, master=None, **kw):
        Widget.__init__(self, master, "ttk::progressbar", kw)

    def _setup(self):
        self._timer = None
        self._node.set("text", "")

    def _apply(self, key, value):
        n = self._node
        if key in ("value", "maximum"):
            self._update()
        elif key == "variable":
            if isinstance(value, Variable):
                value.trace_add("write", lambda *a: self.winfo_exists() and self._from_var())
            self._from_var()
        elif key in ("length", "orient"):
            length = int(_pixels(self.cget("length")))
            vertical = str(self.cget("orient")).startswith("v")
            n.geom(int(n.geom_get("manager")), reqwidth=0 if vertical else length,
                   reqheight=length if vertical else 0)
        else:
            super()._apply(key, value)

    def _from_var(self):
        var = self._opts.get("variable")
        if var not in (None, ""):
            try:
                self._opts["value"] = float(_var_value(var))
            except (TypeError, ValueError):
                return
            self._update()

    def _update(self):
        mx = float(self.cget("maximum")) or 1.0
        v = float(self.cget("value"))
        if str(self.cget("mode")) == "indeterminate":  # a bouncing block
            phase = (v % (2 * mx)) / mx
            v = (2 - phase if phase > 1 else phase) * mx
        self._node.set("factor", max(0.0, min(1.0, v / mx)))

    def start(self, interval=None):
        """Begin autoincrement mode: schedules a recurring timer event that
        calls method step every interval milliseconds."""
        self.stop()
        ms = int(interval or 50)

        def tick():
            self.step()
            self._timer = self.after(ms, tick)
        self._timer = self.after(ms, tick)

    def step(self, amount=None):
        """Increments the value option by amount (1.0 by default)."""
        mx = float(self.cget("maximum")) or 1.0
        v = float(self.cget("value")) + float(amount if amount is not None else 1.0)
        if str(self.cget("mode")) == "determinate":
            v = v % mx if v > mx else v
        self.configure(value=v)
        var = self._opts.get("variable")
        if isinstance(var, Variable):
            var.set(v)

    def stop(self):
        """Stop autoincrement mode."""
        if self._timer:
            self.after_cancel(self._timer)
            self._timer = None


class Combobox(Entry):
    """Ttk Combobox widget combines a text field with a pop-down list of
    values (a Blender enum dropdown)."""

    _KIND = "dropdown"
    _DEFAULTS = {"values": (), "state": "normal", "height": 10, "width": 20,
                 "justify": "left", "exportselection": 1}

    def __init__(self, master=None, **kw):
        Widget.__init__(self, master, "ttk::combobox", kw)

    def _setup(self):
        self._values = []
        self._text = ""

    def _apply(self, key, value):
        n = self._node
        if key == "values":
            vals = value.split() if isinstance(value, str) else list(value)
            self._values = [str(v) for v in vals]
            self._push()
        elif key == "textvariable":
            if isinstance(value, Variable):
                value.trace_add("write", lambda *a: self.winfo_exists() and self._from_var())
            self._from_var()
        elif key == "width":
            n.geom(int(n.geom_get("manager")), reqwidth=_chars(int(value or 20)))
        else:
            Widget._apply(self, key, value)

    def _push(self):
        """Values plus a typed value that isn't one of them (editable box)."""
        items = list(self._values)
        if self._text and self._text not in items:
            items.append(self._text)
        self._node.set("items", items)
        if self._text in items:
            self._node.set("value", float(items.index(self._text)))

    def _from_var(self):
        var = self._opts.get("textvariable")
        if var not in (None, ""):
            self._text = str(_var_value(var))
            self._push()

    def current(self, newindex=None):
        """If newindex is supplied, sets the combobox value to the element
        at position newindex in the list of values. Otherwise, returns the
        index of the current value in the list of values or -1."""
        if newindex is None:
            return self._values.index(self._text) if self._text in self._values else -1
        i = int(newindex)
        if not 0 <= i < len(self._values):
            raise TclError("index %s out of range" % newindex)
        self.set(self._values[i])
        return None

    def get(self):
        return self._text

    def set(self, value):
        """Sets the value of the combobox to value."""
        self._text = str(value)
        self._push()
        self._to_var(self._text)

    def _set_text(self, text, reason="forced"):
        self.set(text)

    def _event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            i = int(self._node.get("value"))
            items = list(self._values) + ([self._text] if self._text and
                                          self._text not in self._values else [])
            if 0 <= i < len(items):
                self._text = items[i]
                self._to_var(self._text)
            self.event_generate("<<ComboboxSelected>>")


class OptionMenu(Menubutton, _widgets.OptionMenu):
    """Themed OptionMenu, based after tkinter's OptionMenu, which allows the
    user to select a value from a menu."""

    def __init__(self, master, variable, default=None, *values, **kwargs):
        command = kwargs.pop("command", None)
        style = kwargs.pop("style", None)
        kwargs.pop("direction", None)
        _widgets.OptionMenu.__init__(self, master, variable, default if default is not None
                                     else (values[0] if values else ""), *values,
                                     command=command)
        if style:
            self._opts["style"] = style
        self._command = command

    def set_menu(self, default=None, *values):
        """Build a new menu of radiobuttons with *values and optionally a default value."""
        menu = self["menu"]
        menu.delete(0, "end")
        for v in values:
            menu.add_radiobutton(label=v, command=None if self._command is None else
                                 (lambda v=v: self._command(v)), variable=self._variable,
                                 value=v)
        if default:
            self._variable.set(default)


class LabeledScale(Frame):
    """A Ttk Scale widget with a Ttk Label widget indicating its current value."""

    def __init__(self, master=None, variable=None, from_=0, to=10, **kw):
        self._label_top = kw.pop("compound", "top") == "top"
        Frame.__init__(self, master, **kw)
        self._variable = variable or _core.IntVar(master)
        self._variable.set(from_)
        self.label = Label(self)
        self.scale = Scale(self, variable=self._variable, from_=from_, to=to)
        side = "top" if self._label_top else "bottom"
        self.label.pack(side=side, anchor="w")
        self.scale.pack(side="top" if not self._label_top else "bottom", fill="x")
        self._cb = self._variable.trace_add("write", self._adjust)
        self._adjust()

    def _adjust(self, *args):
        try:
            self.label.configure(text=str(self._variable.get()))
        except TclError:
            pass

    def destroy(self):
        """Destroy this widget and possibly its associated variable."""
        try:
            self._variable.trace_remove("write", self._cb)
        except AttributeError:
            pass
        Frame.destroy(self)

    @property
    def value(self):
        """Return current scale value."""
        return self._variable.get()

    @value.setter
    def value(self, val):
        """Set new scale value."""
        self._variable.set(val)


# ==========================================================================
# Notebook


class Notebook(Widget):
    """Ttk Notebook widget manages a collection of windows and displays a
    single one at a time (Blender tabs)."""

    _KIND = "tabs"

    def __init__(self, master=None, **kw):
        Widget.__init__(self, master, "ttk::notebook", kw)

    def _setup(self):
        self._tabs = []  # [child, {"text": ..., "state": ...}]

    def _container_for(self, child):
        return None  # pages are shown through add()

    def _apply(self, key, value):
        if key in ("width", "height"):
            self._node.geom(int(self._node.geom_get("manager")),
                            **{"req" + key: int(_pixels(value))})
        else:
            super()._apply(key, value)

    def _shown(self):
        return [t for t in self._tabs if t[1].get("state") != "hidden"]

    def _push(self):
        node = self._node
        for c in node.children:
            node.remove(c)
        shown = self._shown()
        for child, opts in shown:
            node.add(child._node)
            child._node.geom(1)
            child._ginfo = {"manager": "notebook", "in": self}
        node.set("items", [str(o.get("text", "")) for _, o in shown])

    def _child(self, tab_id):
        if isinstance(tab_id, BaseWidget):
            return tab_id
        return self._tabs[self.index(tab_id)][0]

    def add(self, child, **kw):
        """Adds a new tab to the notebook."""
        for t in self._tabs:
            if t[0] is child:
                t[1].update(kw)
                t[1]["state"] = kw.get("state", "normal")
                self._push()
                return
        self._tabs.append([child, dict({"state": "normal", "text": ""}, **kw)])
        self._push()
        if len(self._shown()) == 1:
            self.event_generate("<<NotebookTabChanged>>")

    def insert(self, pos, child, **kw):
        """Inserts a pane at the specified position."""
        existing = [t for t in self._tabs if t[0] is child]
        if existing:
            self._tabs.remove(existing[0])
            kw = dict(existing[0][1], **kw)
        i = len(self._tabs) if str(pos) == "end" else self.index(pos)
        self._tabs.insert(i, [child, dict({"state": "normal", "text": ""}, **kw)])
        self._push()

    def forget(self, tab_id):
        """Removes the tab specified by tab_id, unmaps and unmanages the
        associated window."""
        child = self._child(tab_id)
        self._tabs = [t for t in self._tabs if t[0] is not child]
        child._ginfo = {}
        self._push()

    def hide(self, tab_id):
        """Hides the tab specified by tab_id (add() shows it again)."""
        child = self._child(tab_id)
        for t in self._tabs:
            if t[0] is child:
                t[1]["state"] = "hidden"
        self._push()

    def index(self, tab_id):
        """Returns the numeric index of the tab specified by tab_id, or the
        total number of tabs if tab_id is the string "end"."""
        if isinstance(tab_id, int):
            return tab_id
        s = str(tab_id)
        if s == "end":
            return len(self._tabs)
        if s == "current":
            shown = self._shown()
            i = int(self._node.get("current"))
            return self._tabs.index(shown[i]) if 0 <= i < len(shown) else -1
        if s.startswith("@"):
            x = int(s[1:].split(",")[0])
            shown = self._shown()
            w = self.winfo_width() / max(1, len(shown))
            return self._tabs.index(shown[min(len(shown) - 1, int(x // w))]) if shown else -1
        for i, (child, _) in enumerate(self._tabs):
            if child is tab_id or child._w == s:
                return i
        raise TclError('Slave %s not managed by %s' % (tab_id, self._w))

    def select(self, tab_id=None):
        """Selects the specified tab (returns the selected tab's name)."""
        if tab_id is None:
            i = self.index("current")
            return self._tabs[i][0]._w if i >= 0 else ""
        child = self._child(tab_id)
        shown = [c for c, _ in self._shown()]
        if child in shown:
            changed = shown.index(child) != int(self._node.get("current"))
            self._node.set("current", float(shown.index(child)))
            if changed:
                self.event_generate("<<NotebookTabChanged>>")
        return None

    def tab(self, tab_id, option=None, **kw):
        """Query or modify the options of the specific tab_id."""
        opts = self._tabs[self.index(tab_id)][1]
        if option is not None:
            return opts.get(option.lstrip("-"), "")
        if not kw:
            return dict(opts)
        opts.update(kw)
        self._push()
        return None

    def tabs(self):
        """Returns a list of windows managed by the notebook."""
        return tuple(c._w for c, _ in self._tabs)

    def enable_traversal(self):
        """Enable keyboard traversal (Ctrl+Tab, Ctrl+Shift+Tab)."""
        top = self._toplevel_window()

        def step(d):
            shown = self._shown()
            if shown:
                i = (int(self._node.get("current")) + d) % len(shown)
                self.select(shown[i][0])
            return "break"
        top.bind("<Control-Tab>", lambda e: step(1), add="+")
        top.bind("<Control-Shift-Tab>", lambda e: step(-1), add="+")

    def identify(self, x, y):
        """Returns the name of the tab element at position x, y."""
        return "label" if int(y) < buipy.kit_unit() else ""

    def _event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            self.event_generate("<<NotebookTabChanged>>")


# ==========================================================================
# Panedwindow


class Panedwindow(Widget, _widgets.PanedWindow):
    """Ttk Panedwindow widget displays a number of subwindows, stacked
    either vertically or horizontally."""

    _DEFAULTS = dict(_widgets.PanedWindow._DEFAULTS, orient="vertical")

    def __init__(self, master=None, **kw):
        _widgets.PanedWindow.__init__(self, master, **kw)

    def add(self, child, **kw):
        kw.setdefault("weight", 0)
        _widgets.PanedWindow.add(self, child, **kw)

    def insert(self, pos, child, **kw):
        """Inserts a pane at the specified positions."""
        kw.setdefault("weight", 0)
        _widgets.PanedWindow.insert(self, pos, child, **kw)

    def pane(self, pane, option=None, **kw):
        """Query or modify the options of the specified pane."""
        child = pane if isinstance(pane, BaseWidget) else self._panes[int(pane)]
        if option is not None:
            return self.panecget(child, option)
        return self.paneconfigure(child, **kw)

    def sashpos(self, index, newpos=None):
        """If newpos is specified, sets the position of sash number index."""
        if newpos is None:
            x, y = self.sash_coord(index)
            return x if self._horizontal() else y
        self.sash_place(index, newpos, newpos)
        return newpos

    def _weight(self, child):
        if not any(float(o.get("weight", 0)) for o in self._pane_opts.values()):
            return 1.0 if child is self._panes[-1] else 0.0
        return float(self._pane_opts.get(child, {}).get("weight", 0))


PanedWindow = Panedwindow


# ==========================================================================
# Treeview


class Treeview(Widget, XView, YView):
    """Ttk Treeview widget displays a hierarchical collection of items."""

    _KIND = "tree"
    _DEFAULTS = {"columns": (), "displaycolumns": "#all", "height": 10,
                 "selectmode": "extended", "show": "tree headings", "padding": 0}

    def __init__(self, master=None, **kw):
        Widget.__init__(self, master, "ttk::treeview", kw)

    def _setup(self):
        self._items = {"": None}
        self._kids = {"": []}
        self._parent = {}
        self._detached = set()
        self._item_opts = {}
        self._columns = []
        self._headings = {"#0": {"text": "", "anchor": "w"}}
        self._colopts = {}
        self._next = 1
        self._tagopts = {}
        self._tag_binds = {}
        self._focus = ""
        self._last_sel = ()
        self._node.set("rows", 10)
        self._node.set("selmode", 2)

    def _apply(self, key, value):
        n = self._node
        if key == "columns":
            self._columns = [str(c) for c in (value.split() if isinstance(value, str)
                                              else value)]
            self._refresh_all()
        elif key in ("displaycolumns", "show"):
            self._refresh_all()
        elif key == "height":
            n.set("rows", int(value) or 10)
        elif key == "selectmode":
            n.set("selmode", float({"extended": 2, "browse": 3, "none": 0}.get(str(value), 2)))
        elif key in ("xscrollcommand", "yscrollcommand"):
            n.set("noscrollbar", True)
            n.set("viewevents", True)
        else:
            super()._apply(key, value)

    # -- columns ------------------------------------------------------------------------
    def _display(self):
        d = self.cget("displaycolumns")
        if d in ("#all", ("#all",), ["#all"]):
            return list(self._columns)
        cols = d.split() if isinstance(d, str) else list(d)
        return [self._columns[int(c)] if str(c).isdigit() else str(c) for c in cols]

    def _tree_shown(self):
        show = self.cget("show")
        show = show.split() if isinstance(show, str) else list(show)
        return "tree" in show

    def _headings_shown(self):
        show = self.cget("show")
        show = show.split() if isinstance(show, str) else list(show)
        return "headings" in show

    def _push_headers(self):
        cols = self._display()
        names = ([str(self._headings.get("#0", {}).get("text", ""))] if self._tree_shown()
                 else []) + [str(self._headings.get(c, {}).get("text", c)) for c in cols]
        self._node.set("headers", names if self._headings_shown() else [])

    def _cells(self, iid):
        """(label, other columns) for the kit row of an item."""
        o = self._item_opts[iid]
        vals = list(o.get("values", ()))
        by_col = {c: (str(vals[i]) if i < len(vals) else "") for i, c in
                  enumerate(self._columns)}
        shown = [by_col.get(c, "") for c in self._display()]
        if self._tree_shown():
            return str(o.get("text", "")), shown
        return (shown[0] if shown else ""), shown[1:]

    def _push_item(self, iid):
        node = self._items[iid]
        label, values = self._cells(iid)
        node.label = label
        node.set_values(values)

    def _refresh_all(self):
        self._push_headers()
        for iid in self._items:
            if iid:
                self._push_item(iid)
        self._node.invalidate()

    def column(self, column, option=None, **kw):
        """Query or modify the options for the specified column."""
        col = self._colname(column)
        opts = self._colopts.setdefault(col, {"width": 200, "minwidth": 20, "stretch": 1,
                                              "anchor": "w", "id": col})
        if option is not None:
            return opts.get(option.lstrip("-"), "")
        if not kw:
            return dict(opts)
        opts.update(kw)
        return None

    def _colname(self, column):
        c = str(column)
        if c.startswith("#") and c[1:].isdigit():
            k = int(c[1:])
            return "#0" if k == 0 else self._display()[k - 1]
        return c

    def heading(self, column, option=None, **kw):
        """Query or modify the heading options for the specified column."""
        col = self._colname(column)
        opts = self._headings.setdefault(col, {"text": "", "anchor": "center"})
        if option is not None:
            return opts.get(option.lstrip("-"), "")
        if not kw:
            return dict(opts)
        opts.update(kw)
        self._push_headers()
        return None

    # -- items --------------------------------------------------------------------------
    def insert(self, parent, index, iid=None, **kw):
        """Creates a new item and return the item identifier of the newly
        created item."""
        if parent not in self._kids:
            raise TclError('Item %s not found' % parent)
        if iid is None:
            while "I%03X" % self._next in self._items:
                self._next += 1
            iid = "I%03X" % self._next
            self._next += 1
        iid = str(iid)
        if iid in self._items:
            raise TclError('Item %s already exists' % iid)
        node = buipy.KitTreeItem("")
        node.set("expanded", 0.0)  # Tk items start closed
        node._iid = iid
        self._items[iid] = node
        self._kids[iid] = []
        self._item_opts[iid] = {"text": "", "values": (), "open": False, "tags": (),
                                "image": ""}
        self._attach(iid, parent, index)
        self.item(iid, **kw)
        return iid

    def _attach(self, iid, parent, index):
        siblings = self._kids.setdefault(parent, [])
        i = len(siblings) if str(index) == "end" else max(0, min(len(siblings), int(index)))
        siblings.insert(i, iid)
        self._parent[iid] = parent
        self._detached.discard(iid)
        self._rebuild(parent)

    def _detach_from_parent(self, iid):
        parent = self._parent.get(iid, "")
        if iid in self._kids.get(parent, []):
            self._kids[parent].remove(iid)
        self._rebuild(parent)

    def _rebuild(self, parent):
        """Kit children of `parent` follow the Tk order."""
        if parent == "":
            self._node.set_tree_roots([self._items[i] for i in self._kids[""]])
            return
        node = self._items[parent]
        node.clear_children()
        for i in self._kids[parent]:
            node.add(self._items[i])
        self._node.invalidate()

    def item(self, item, option=None, **kw):
        """Query or modify the options for the specified item."""
        if item not in self._item_opts:
            raise TclError('Item %s not found' % item)
        opts = self._item_opts[item]
        if option is not None:
            v = opts.get(option.lstrip("-"), "")
            return int(v) if isinstance(v, bool) else v
        if not kw:  # as tkinter.ttk converts Tcl results
            out = dict(opts)
            out["values"] = [_convert_stringval(v) for v in out["values"]] or ""
            out["tags"] = list(out["tags"]) or ""
            out["open"] = int(bool(out["open"]))
            return out
        node = self._items[item]
        for k, v in kw.items():
            if k == "values":
                v = tuple(str(x) for x in (v.split() if isinstance(v, str) else v))
            elif k == "tags":
                v = tuple(v.split() if isinstance(v, str) else v)
            elif k == "open":
                v = getboolean(v)
                node.set("expanded", 1.0 if v else 0.0)
            elif k == "image":
                img = _image_of(v) if v else None
                if img is not None:
                    pass  # tree rows show kit icons; images are kept
            opts[k] = v
        self._push_item(item)
        self._node.invalidate()
        return None

    def delete(self, *items):
        """Delete all specified items and all their descendants."""
        for iid in items:
            if iid not in self._items:
                raise TclError('Item %s not found' % iid)
        for iid in items:
            self._remove(iid)
        self._selection_changed()

    def _remove(self, iid):
        if iid not in self._items:
            return
        for kid in list(self._kids.get(iid, ())):
            self._remove(kid)
        if self._node.tree_selected() is self._items[iid]:
            self._node.tree_select(None)
        self._detach_from_parent(iid)
        self._items.pop(iid, None)
        self._kids.pop(iid, None)
        self._parent.pop(iid, None)
        self._item_opts.pop(iid, None)
        if self._focus == iid:
            self._focus = ""

    def detach(self, *items):
        """Unlinks all of the specified items from the tree (reattach brings them back)."""
        for iid in items:
            self._detach_from_parent(iid)
            self._detached.add(iid)

    def move(self, item, parent, index):
        """Moves item to position index in parent's list of children."""
        self._detach_from_parent(item)
        self._attach(item, parent, index)

    reattach = move

    def exists(self, item):
        """Returns True if the specified item is present in the tree."""
        return item in self._items and item != ""

    def get_children(self, item=None):
        """Returns a tuple of children belonging to item."""
        return tuple(self._kids.get(item or "", ()))

    def set_children(self, item, *newchildren):
        """Replaces item's child with newchildren."""
        for kid in list(self._kids.get(item, ())):
            if kid not in newchildren:
                self._detach_from_parent(kid)
                self._detached.add(kid)
        for kid in newchildren:
            self._detach_from_parent(kid)
        self._kids[item] = []
        for kid in newchildren:
            self._attach(kid, item, "end")

    def parent(self, item):
        """Returns the ID of the parent of item, or '' if item is at the top level."""
        return self._parent.get(item, "")

    def index(self, item):
        """Returns the integer index of item within its parent's list of children."""
        return self._kids[self._parent.get(item, "")].index(item)

    def next(self, item):
        """Returns the identifier of item's next sibling, or '' if item is the last child."""
        sibs = self._kids[self._parent.get(item, "")]
        i = sibs.index(item)
        return sibs[i + 1] if i + 1 < len(sibs) else ""

    def prev(self, item):
        """Returns the identifier of item's previous sibling, or '' if item is the first child."""
        sibs = self._kids[self._parent.get(item, "")]
        i = sibs.index(item)
        return sibs[i - 1] if i > 0 else ""

    def depth(self, item):
        d = 0
        while self._parent.get(item, ""):
            item = self._parent[item]
            d += 1
        return d

    def set(self, item, column=None, value=None):
        """Query or set the value of given item."""
        opts = self._item_opts[item]
        vals = list(opts.get("values", ()))
        vals += [""] * (len(self._columns) - len(vals))
        if column is None:
            return {c: vals[i] for i, c in enumerate(self._columns)}
        col = self._colname(column)
        ci = self._columns.index(col) if col in self._columns else int(str(column).lstrip("#"))
        if value is None:
            return vals[ci]
        vals[ci] = value
        self.item(item, values=vals)
        return None

    def see(self, item):
        """Ensure that item is visible (opens its ancestors, scrolls to it)."""
        p = self._parent.get(item, "")
        while p:
            self.item(p, open=True)
            p = self._parent.get(p, "")
        rows = self._visible_rows()
        if item in rows:
            i = rows.index(item)
            total = max(1, len(rows))
            first = int(round(self._node.get("yfirst") * total))
            vis = max(1, int(self._node.get("yvisible")))
            if i < first:
                self._node.set("yview", i / total)
            elif i >= first + vis:
                self._node.set("yview", (i - vis + 1) / total)
            self._view_changed()

    def focus(self, item=None):
        """If item is specified, sets the focus item to item. Otherwise,
        returns the current focus item, or '' if there is none."""
        if item is None:
            sel = self._node.tree_selected()
            return getattr(sel, "_iid", self._focus) if sel is not None else self._focus
        self._focus = item
        if item in self._items:
            self._node.tree_select(self._items[item])
        return None

    def bbox(self, item, column=None):
        """Returns the bounding box (relative to the treeview widget's
        window) of the specified item in the form x y width height."""
        rows = self._visible_rows()
        if item not in rows:
            return ""
        u = buipy.kit_unit()
        total = max(1, len(rows))
        first = int(round(self._node.get("yfirst") * total))
        i = rows.index(item) - first
        if not 0 <= i < int(self._node.get("yvisible")):
            return ""
        top = round(0.15 * u) + (u if self._headings_shown() else 0)
        return (round(0.15 * u), top + i * u, self.winfo_width() - round(0.3 * u), u)

    # -- selection ------------------------------------------------------------------------
    def selection(self):
        """Returns the tuple of selected items."""
        return tuple(iid for iid in self._visible_order(all_items=True)
                     if iid in self._items and self._items[iid].get("marked"))

    def _visible_order(self, all_items=False):
        out = []

        def walk(parent):
            for iid in self._kids.get(parent, ()):
                out.append(iid)
                if all_items or self._item_opts[iid].get("open"):
                    walk(iid)
        walk("")
        return out

    def _visible_rows(self):
        for iid, opts in self._item_opts.items():  # the kit owns the open state
            opts["open"] = bool(self._items[iid].get("expanded"))
        return self._visible_order()

    def _set_marks(self, items, mode):
        for iid in items:
            if iid not in self._items:
                raise TclError('Item %s not found' % iid)
        if mode == "set":
            for node in self._items.values():
                if node is not None:
                    node.set("marked", 0.0)
        for iid in items:
            node = self._items[iid]
            on = {"set": True, "add": True, "remove": False}.get(mode)
            if on is None:
                on = not node.get("marked")
            node.set("marked", 1.0 if on else 0.0)
        if items and mode in ("set", "add"):
            self._node.tree_select(self._items[items[0]])
        self._node.invalidate()
        self._selection_changed()

    @staticmethod
    def _items_arg(items):
        if len(items) == 1 and isinstance(items[0], (tuple, list)):
            return tuple(items[0])
        if len(items) == 1 and isinstance(items[0], str) and " " in items[0]:
            return tuple(items[0].split())
        return items

    def selection_set(self, *items):
        """The specified items becomes the new selection."""
        self._set_marks(self._items_arg(items), "set")

    def selection_add(self, *items):
        """Add all of the specified items to the selection."""
        self._set_marks(self._items_arg(items), "add")

    def selection_remove(self, *items):
        """Remove all of the specified items from the selection."""
        self._set_marks(self._items_arg(items), "remove")

    def selection_toggle(self, *items):
        """Toggle the selection state of each specified item."""
        self._set_marks(self._items_arg(items), "toggle")

    def _selection_changed(self):
        sel = self.selection()
        if sel != self._last_sel:
            self._last_sel = sel
            if not getattr(self, "_select_pending", False):  # Tk queues the event
                self._select_pending = True
                self.after_idle(self._fire_select)

    def _fire_select(self):
        self._select_pending = False
        if self.winfo_exists():
            self.event_generate("<<TreeviewSelect>>")

    # -- identify -------------------------------------------------------------------------
    def identify_row(self, y):
        """Returns the item ID of the item at position y."""
        rows = self._visible_rows()
        u = buipy.kit_unit()
        top = round(0.15 * u) + (u if self._headings_shown() else 0)
        if int(y) < top:
            return ""
        first = int(round(self._node.get("yfirst") * max(1, len(rows))))
        i = first + (int(y) - top) // u
        return rows[i] if 0 <= i < len(rows) else ""

    def identify_column(self, x):
        """Returns the data column identifier of the cell at position x."""
        n = len(self._display()) + (1 if self._tree_shown() else 0)
        w = max(1, self.winfo_width())
        if n <= 1:
            k = 0
        else:
            first = w / 2 if n == 2 else w * 0.4
            k = 0 if int(x) < first else 1 + int((int(x) - first) / ((w - first) / (n - 1)))
        k = min(k, n - 1)
        return "#%d" % (k if self._tree_shown() else k + 1)

    def identify_region(self, x, y):
        """'heading', 'separator', 'tree', 'cell' or 'nothing' at x, y."""
        u = buipy.kit_unit()
        if self._headings_shown() and int(y) < round(0.15 * u) + u:
            return "heading"
        if not self.identify_row(y):
            return "nothing"
        return "tree" if self.identify_column(x) == "#0" else "cell"

    def identify_element(self, x, y):
        return "text" if self.identify_row(y) else ""

    def identify(self, component, x, y):
        return getattr(self, "identify_" + component)(*((y,) if component == "row" else
                                                        (x,) if component == "column" else
                                                        (x, y)))

    # -- tags -----------------------------------------------------------------------------
    def tag_configure(self, tagname, option=None, **kw):
        """Query or modify the options for the specified tagname (row colors
        are kept; rows use Blender's list colors)."""
        opts = self._tagopts.setdefault(tagname, {})
        if option is not None:
            return opts.get(option.lstrip("-"), "")
        if not kw:
            return dict(opts)
        opts.update(kw)
        return None

    def tag_has(self, tagname, item=None):
        """If item is specified, returns whether it has the given tagname.
        Otherwise, returns all items which have the tag."""
        if item is not None:
            return tagname in self._item_opts[item].get("tags", ())
        return tuple(i for i, o in self._item_opts.items() if tagname in o.get("tags", ()))

    def tag_bind(self, tagname, sequence=None, callback=None):
        """Bind a callback for the given event sequence to the tag tagname."""
        table = self._tag_binds.setdefault(tagname, _core._BindTable())
        self._node.set("events", True)
        if callback is not None:
            table.bind(sequence, callback, None)
        return None

    def _raw_event(self, ev):
        if not self._tag_binds or ev.type not in (EventType.ButtonPress,
                                                  EventType.ButtonRelease, EventType.KeyPress):
            return
        item = self.identify_row(ev.y) if ev.type != EventType.KeyPress else self.focus()
        if not item:
            return
        for tag in self._item_opts.get(item, {}).get("tags", ()):
            table = self._tag_binds.get(tag)
            if table is not None:
                for fn in list(table.match(ev)):
                    _call(self, fn, ev)

    # -- kit events -------------------------------------------------------------------------
    def _event(self, kind, ref, a, b):
        if kind == EV_CHANGE:
            sel = self._node.tree_selected()
            if sel is not None:
                self._focus = getattr(sel, "_iid", self._focus)
            if self._node.get("selmode") == 0 and sel is not None:  # selectmode none
                return
            self._selection_changed()
        elif kind == EV_CLICK:  # a column heading
            k = int(a)
            cols = (["#0"] if self._tree_shown() else []) + self._display()
            if 0 <= k < len(cols):
                cmd = self._headings.get(cols[k], {}).get("command")
                if cmd:
                    _call(self, _command(cmd))
        elif kind == EV_TOGGLE and int(a) == -1 and ref is not None:  # open / close
            iid = getattr(ref, "_iid", None)
            if iid is not None:
                self._item_opts[iid]["open"] = bool(b)
                self._focus = iid
                self.event_generate("<<TreeviewOpen>>" if b else "<<TreeviewClose>>")
