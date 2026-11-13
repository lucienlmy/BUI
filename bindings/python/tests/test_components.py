"""bui.components on the C kit: layout, Blender interactions, popups,
rendering and frame-time budgets.

Input is driven through the same scene protocol the Tk host uses (press /
motion / release / key / wheel / context_menu), headlessly.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import os
import sys
import time
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import bui  # noqa: E402,F401  (locates buipy)
import buipy  # noqa: E402
from bui.components import (App, Button, Checkbox, ColorSwatch, Column, Dropdown,  # noqa: E402
                            EnumRow, Label, ListView, NumberField, NumberSlider, Panel, Row,
                            Split, TextField, Toggle, VectorField, eval_number)

CNR_TOP, CNR_BOTTOM, CNR_LEFT, CNR_RIGHT = 3, 12, 9, 6

buipy.kit_set_animations(False)  # deterministic sizes; test_panel_animation turns them on


def mods(shift=False, ctrl=False, alt=False, button1=False):
    return SimpleNamespace(shift=shift, ctrl=ctrl, alt=alt, button1=button1)


class Driver:
    """Headless app + input helpers."""

    def __init__(self, *children, size=(400, 600), header=None):
        self.app = App("test", size, *children, header=header)
        self.timers, self.cursors, self.warps = [], [], []
        self.app.ctx.set_host(schedule=lambda ms, tid: self.timers.append(tid),
                              cursor=self.cursors.append,
                              warp=lambda x, y: self.warps.append((x, y)))
        self.frame()

    @property
    def ctx(self):
        return self.app.ctx

    def frame(self):
        return self.app.render_raster()

    @staticmethod
    def center(c):
        x, y, w, h = c.rect
        return (x + w // 2, y + h // 2)

    def click(self, c=None, dx=None, m=None, at=None):
        x, y = at or self.center(c)
        if dx is not None:
            x = c.rect[0] + dx
        self.app.press(x, y, m or mods())
        self.app.release(x, y, m or mods())
        self.frame()

    def drag(self, c, dx, m=None, start_dx=None):
        x, y = self.center(c)
        if start_dx is not None:
            x = c.x + start_dx
        self.app.press(x, y, m or mods())
        for step in (dx / 3, 2 * dx / 3, dx):
            self.app.motion(int(x + step), y, m or mods(button1=True))
        self.app.release(int(x + dx), y, m or mods())
        self.frame()

    def hover(self, c):
        self.app.motion(*self.center(c), mods())

    def key(self, keysym, char="", m=None):
        self.app.key(char, keysym, 0, m or mods())
        self.frame()

    def type(self, text):
        for ch in text:
            self.key(ch, ch)

    def wheel(self, c, delta, m=None):
        r = self.app.wheel(*self.center(c), delta, m or mods())
        self.frame()
        return r

    def popups(self):
        return self.ctx.popup_count()


# -- layout ---------------------------------------------------------------------


def test_layout():
    a, b = Button("A"), Button("B", weight=3)
    Driver(Row(a, b))
    assert abs(b.w - 3 * a.w) <= 2, (a.w, b.w)
    left, right = Label("L"), NumberField("", 1.0)
    Driver(Split(left, right, factor=0.4))
    assert abs(left.w / (left.w + right.w) - 0.4) < 0.03
    x, y, z = Button("x"), Button("y"), Button("z")
    Driver(Column(x, y, z, align=True))
    assert x.corners == CNR_TOP and y.corners == 0 and z.corners == CNR_BOTTOM
    assert y.y == x.y + x.h, "aligned column must have no gaps"
    t1, t2 = Toggle("a"), Toggle("b")
    Driver(Row(t1, t2, align=True))
    assert t1.corners == CNR_LEFT and t2.corners == CNR_RIGHT
    icon = Button("", icon="ADD", weight=0)
    wide = Button("wide")
    Driver(Row(wide, icon))
    assert icon.w == buipy.kit_unit(), "weight=0 keeps the natural width"


# -- numbers ---------------------------------------------------------------------


def test_number_scrub_and_modifiers():
    changes = []
    f = NumberField("Size", 1.0, step=0.1, on_change=changes.append)
    d = Driver(f)
    d.drag(f, 50)                                     # drag right increases
    plain = f.value
    assert plain > 1.0 and changes, plain
    f.set_value(1.0)
    d.drag(f, 50, m=mods(shift=True))                 # Shift: 10x finer
    assert 1.0 < f.value < plain and abs((f.value - 1) * 10 - (plain - 1)) < 0.2, f.value
    f.set_value(1.0)
    d.drag(f, 37, m=mods(ctrl=True))                  # Ctrl: snaps to increments
    assert abs(f.value - round(f.value)) < 1e-9, f.value


def test_number_arrows_and_typing():
    f = NumberField("N", 5, min=0, max=10)
    d = Driver(f)
    d.click(f, dx=f.w - 3)                             # right arrow
    assert f.value == 6
    d.click(f, dx=3)                                   # left arrow
    assert f.value == 5
    d.click(f)                                         # middle: type a value
    assert f.editing and d.ctx.focus is f
    d.type("2*3")
    d.key("Return")
    assert f.value == 6 and not f.editing
    d.click(f)
    d.type("9")
    d.key("Escape")                                    # cancel keeps the old value
    assert f.value == 6
    d.click(f)
    d.type("99")
    d.key("Return")
    assert f.value == 10, "typed values are clamped to max"


def test_slider_relative_drag():
    s = NumberSlider("Level", 0.5, min=0.0, max=1.0)
    d = Driver(s)
    d.drag(s, s.w // 4)
    assert abs(s.value - 0.75) < 0.02, s.value
    d.drag(s, -10 * s.w)
    assert s.value == 0.0


def test_value_shortcuts():
    a, b = NumberField("A", 3.5), NumberField("B", 2.0)   # B's default is the 2.0 it was made with
    d = Driver(Column(a, b))
    d.hover(a)
    d.key("c", "c", mods(ctrl=True))                  # copy
    d.hover(b)
    d.key("v", "v", mods(ctrl=True))                  # paste
    assert b.value == 3.5
    d.key("minus", "-")                               # negate
    assert b.value == -3.5
    d.key("BackSpace")                                # reset to the default, not to zero
    assert b.value == 2.0
    assert d.wheel(b, +1, mods(ctrl=True)) and abs(b.value - 2.1) < 1e-9   # Ctrl+Wheel


def test_checkbox_drag_toggle():
    boxes = [Checkbox("c%d" % i, False) for i in range(4)]
    d = Driver(Column(*boxes))
    d.app.press(boxes[0].x + 4, d.center(boxes[0])[1], mods())
    for c in boxes[1:3]:
        d.app.motion(c.x + 4, c.y + c.h // 2, mods(button1=True))
    d.app.release(boxes[2].x + 4, boxes[2].y + 5, mods())
    assert [c.value for c in boxes] == [True, True, True, False]


def test_text_field_editing():
    got = []
    t = TextField("", "hello world", on_change=got.append)
    d = Driver(t)
    d.click(t)
    assert t.editing
    d.key("a", "a", mods(ctrl=True))                  # select all
    d.type("Cube")
    d.key("BackSpace", m=mods(ctrl=True))             # Ctrl+Backspace: word
    d.type("Sphere")
    d.key("Return")
    assert t.value == "Sphere" and got == ["Sphere"], (t.value, got)


def test_dropdown_menu():
    dd = Dropdown("", ["One", "Two", "Three"])
    other = Button("other")
    d = Driver(Column(other, dd))                      # the menu opens below dd
    d.click(dd)
    assert d.popups() == 1
    text, (x, y, w, h) = d.ctx.menu_rows(0)[2]
    assert text == "Three"
    d.app.motion(x + 10, y + h // 2, mods())
    d.app.press(x + 10, y + h // 2, mods())
    d.app.release(x + 10, y + h // 2, mods())
    assert dd.value == "Three" and not d.popups()
    clicks = []
    other.on_click = lambda: clicks.append(1)
    d.click(dd)
    d.click(other)                                     # outside click only closes
    assert not d.popups() and clicks == []
    d.click(dd)
    d.key("Escape")
    assert not d.popups()
    d.hover(dd)
    d.wheel(dd, +1, mods(ctrl=True))                   # Ctrl+Wheel cycles
    assert dd.value == "Two"


def test_enum_row():
    e = EnumRow("", ["A", "B", "C"])
    d = Driver(e)
    d.click(e, dx=int(e.w * 0.9))
    assert e.value == "C"


def test_panel_collapse_and_solo():
    p1 = Panel("One", Button("x"))
    p2 = Panel("Two", Button("y"))
    d = Driver(p1, p2)
    h = p1.h
    d.click(at=(p1.x + 20, p1.y + 5))
    assert p1.collapsed and p1.h < h
    d.click(at=(p2.x + 20, p2.y + 5), m=mods(ctrl=True))   # solo
    assert p1.collapsed and not p2.collapsed


def test_listview():
    renamed = []
    lv = ListView(["a%d" % i for i in range(20)], rows=4,
                  on_rename=lambda i, n: renamed.append((i, n)))
    d = Driver(lv)
    u = buipy.kit_unit()
    pad = round(0.15 * u)
    d.click(at=(lv.x + 20, lv.y + pad + u + 2))
    assert lv.value == 1
    d.wheel(lv, -1)                                    # scroll down
    assert lv.scroll == 1
    x, y = lv.x + 20, lv.y + pad + 2
    d.app.press(x, y, mods())
    d.app.release(x, y, mods())
    d.app.press(x, y, mods())                          # double-click: rename
    d.app.release(x, y, mods())
    assert lv.editing
    d.key("a", "a", mods(ctrl=True))
    d.type("Renamed")
    d.key("Return")
    assert renamed == [(1, "Renamed")] and lv.items[1] == "Renamed", renamed


def test_vector_and_color():
    seen = []
    v = VectorField("Loc", (1.0, 2.0, 3.0), on_change=seen.append)
    sw = ColorSwatch("", (1.0, 0.0, 0.0, 1.0))
    d = Driver(Column(v, sw))
    v.fields[1].set_value(7.0)
    d.hover(v.fields[1])
    d.key("BackSpace")                                # back to the 2.0 it was made with
    assert v.value == (1.0, 2.0, 3.0) and seen[-1] == (1.0, 2.0, 3.0)
    d.click(sw)
    tabs = d.ctx.picker_part("tabs")
    d.click(tabs, dx=5)                                # RGB tab
    g = d.ctx.picker_part("slider1")
    d.drag(g, g.w)                                     # G -> 1
    r, gg, b, a = sw.value
    assert r > 0.99 and gg > 0.99 and b < 0.01, sw.value
    assert ColorSwatch.to_text(sw.value) == "#ffff00ff"


def test_tooltip():
    b = Button("tip", description="Does a thing")
    d = Driver(b)
    d.hover(b)
    assert d.timers, "tooltip timer not scheduled"
    d.ctx.timer(d.timers[-1])
    assert d.ctx.tooltip
    d.frame()                                          # draws without error
    d.app.motion(b.x + 2, b.y + 2, mods())             # moving hides it
    assert not d.ctx.tooltip


def test_disabled_ignores_input():
    f = NumberField("", 1.0, enabled=False)
    d = Driver(f)
    d.drag(f, 60)
    d.click(f)
    assert f.value == 1.0 and not f.editing


def test_text_edit_undo():
    """Ctrl+Z / Ctrl+Shift+Z inside an active text edit (Blender's interface_undo.cc)."""
    t = TextField("", "")
    d = Driver(t)
    d.click(t)
    d.type("abc")
    assert t.get_str("edit") == "abc"
    d.key("z", "z", mods(ctrl=True))
    assert t.get_str("edit") == "ab", t.get_str("edit")
    d.key("z", "z", mods(ctrl=True))
    d.key("z", "z", mods(ctrl=True))
    assert t.get_str("edit") == ""
    d.key("z", "z", mods(ctrl=True))                   # nothing before the start
    assert t.get_str("edit") == ""
    d.key("Z", "Z", mods(ctrl=True, shift=True))       # redo
    d.key("y", "y", mods(ctrl=True))
    assert t.get_str("edit") == "ab", t.get_str("edit")
    d.type("X")                                         # new typing drops the redo states
    d.key("Z", "Z", mods(ctrl=True, shift=True))
    assert t.get_str("edit") == "abX"
    d.key("Return")
    assert t.value == "abX"


def test_float_display_precision():
    """Blender's UI_calc_float_precision: small values get the decimals they need."""
    f = NumberField("", 0.00002)                       # precision 3 would read "0.000"
    assert f.get_str("display") == "0.0000", f.get_str("display")  # Blender's digit count
    f.value = 0.000123
    assert f.get_str("display") == "0.00012", f.get_str("display")
    f.value = 1.5
    assert f.get_str("display") == "1.500"
    f.value = 10.0001                                   # only small values get more
    assert f.get_str("display") == "10.000"


def test_eval_number():
    assert abs(eval_number("2*pi") - 2 * 3.141592653589793) < 1e-12
    assert eval_number("__import__('os')") is None
    assert eval_number("1/0") is None
    assert eval_number("10 ** 2 - 1") == 99
    assert eval_number("2^3") == 8 and eval_number("sqrt(16)") == 4
    # Blender's expr_pylike_eval (core/intern): its functions, conditionals and min/max
    assert eval_number("max(1, 7, 3) + min(4, 2)") == 9
    assert eval_number("1 if 3 > 2 else 0") == 1
    assert abs(eval_number("atan2(1, 1)") - 3.141592653589793 / 4) < 1e-12
    assert eval_number("7 // 2") == 3 and eval_number("7 % 4") == 3
    assert abs(eval_number("tau - 2*pi")) < 1e-12 and abs(eval_number("e") - 2.718281828459045) < 1e-12
    assert eval_number("1 +") is None


# -- cursors, continuous grab, context menus -------------------------------------


def test_cursors_and_continuous_grab():
    f, t = NumberSlider("S", 0.5), TextField("", "abc")
    d = Driver(Column(f, t))
    d.hover(f)
    assert d.app.cursor == "ew", "number fields show Blender's horizontal arrows"
    d.hover(t)
    assert d.app.cursor == "text"
    x, y = d.center(f)
    d.app.press(x, y, mods())
    d.app.motion(x + 20, y, mods(button1=True))
    assert d.app.cursor == "none", "scrubbing hides the pointer"
    before = f.value
    w = d.app.size[0]
    d.app.motion(w - 2, y, mods(button1=True))         # hits the edge -> warp
    assert d.warps, "continuous grab must warp the pointer"
    wx, wy = d.warps[-1]
    d.app.motion(wx, wy, mods(button1=True))           # the warp's own event: ignored
    d.app.motion(wx + 10, wy, mods(button1=True))      # keeps scrubbing past the edge
    assert f.value >= before
    d.app.release(wx + 10, wy, mods())
    wx, wy = d.warps[-1]  # Blender (USE_CONT_MOUSE_CORRECT): a slider puts it at its value
    assert wy == y and wx > x, (d.warps[-1], (x, y))
    assert d.app.cursor == "ew"


def test_context_menu():
    f = NumberField("N", 2.0, default=1.0)
    d = Driver(f)
    f.set_value(5.0)
    x, y = d.center(f)
    d.app.context_menu(x, y, mods())
    rows = [t for t, _ in d.ctx.menu_rows(0) if t != "-"]
    assert rows == ["Copy Value", "Paste Value", "Reset to Default Value"], rows
    d.frame()
    _, (rx, ry, rw, rh) = d.ctx.menu_rows(0)[3]
    d.click(at=(rx + 10, ry + rh // 2))
    assert f.value == 1.0 and not d.popups()


# -- menus ------------------------------------------------------------------------


def test_menu_search_submenu_and_bar():
    from bui.components import MenuBar, MenuItem
    picked = []
    bar = MenuBar(("File", [MenuItem("New", lambda: picked.append("new")),
                            MenuItem("Recent", submenu=[MenuItem("a.blend",
                                                                 lambda: picked.append("a"))]),
                            "-", MenuItem("Quit")]),
                  ("Edit", [MenuItem("Undo")]))
    d = Driver(Label("body"), header=bar)
    file_b, edit_b = bar.buttons
    d.click(file_b)
    _, (x, y, w, h) = d.ctx.menu_rows(0)[1]
    d.app.motion(x + 10, y + h // 2, mods())            # hover "Recent"
    assert d.popups() == 2, "hovering a submenu item opens it"
    d.frame()
    d.hover(edit_b)                                     # switch menus
    assert d.popups() == 1 and d.ctx.popup_owner(0) is edit_b
    d.ctx.close_popups()
    d.click(file_b)
    d.type("blend")                                     # type to search
    assert [t for t, _ in d.ctx.menu_rows(0)] == ["Recent \u25b8 a.blend"]
    d.key("Return")
    assert picked == ["a"] and not d.popups()


def test_menu_search_fuzzy():
    """Blender's string search (blenlib string_search.cc): word initials, typos, recent first."""
    from bui.components import MenuBar, MenuItem
    names = ["Shade Smooth", "Subdivision Surface", "Add Cube", "Add Cylinder", "Separate"]
    picked = []
    bar = MenuBar(("Object", [MenuItem(n, (lambda n=n: picked.append(n))) for n in names]))
    d = Driver(Label("body"), header=bar)
    (obj_b,) = bar.buttons

    def search(q):
        d.click(obj_b)
        d.type(q)
        rows = [t for t, _ in d.ctx.menu_rows(0)]
        return rows

    rows = search("ss")                                 # word-initial match
    assert set(rows[:2]) == {"Shade Smooth", "Subdivision Surface"}, rows
    d.ctx.close_popups()
    rows = search("cylnder")                            # a typo still finds it
    assert rows and rows[0] == "Add Cylinder", rows
    d.ctx.close_popups()
    assert search("zzzz") == []                         # no substring fallback junk
    d.ctx.close_popups()
    # choosing an item makes it rank first next time for a short query
    rows = search("s")
    i = rows.index("Separate")
    for _ in range(i):
        d.key("Down")
    d.key("Return")
    assert picked == ["Separate"]
    assert search("s")[0] == "Separate"
    d.ctx.close_popups()


def test_press_drag_release_picks():
    dd = Dropdown("", ["One", "Two", "Three"])
    d = Driver(dd)
    x, y = d.center(dd)
    d.app.press(x, y, mods())
    d.frame()
    _, (rx, ry, rw, rh) = d.ctx.menu_rows(0)[1]
    d.app.motion(rx + 10, ry + rh // 2, mods(button1=True))
    d.app.release(rx + 10, ry + rh // 2, mods())
    assert dd.value == "Two"


def test_pie_menu():
    from bui.components import MenuItem, PieMenu
    got = []
    d = Driver(Label("x"), size=(600, 500))
    d.ctx.set_pointer(300, 250)
    d.app.bind("q", lambda: PieMenu.show(d.ctx, [
        MenuItem(n, (lambda n=n: got.append(n))) for n in "WESN"]))
    d.app.key("q", "q", 0, mods())
    d.frame()
    d.app.motion(300, 120, mods())                     # aim north
    d.app.press(300, 120, mods())
    assert got == ["N"] and not d.popups()


def _pie_on_q(got):
    from bui.components import MenuItem, PieMenu
    d = Driver(Label("x"), size=(600, 500))
    d.ctx.set_pointer(300, 250)
    d.app.bind("q", lambda: PieMenu.show(d.ctx, [
        MenuItem(n, (lambda n=n: got.append(n))) for n in "WESN"]))
    d.app.key("q", "q", 0, mods())
    d.frame()
    return d


def test_pie_hold_release():
    import time
    got = []
    d = _pie_on_q(got)
    d.app.key("q", "q", 0, mods())                     # auto-repeat is ignored
    d.app.motion(301, 249, mods())                     # still inside the click threshold
    time.sleep(0.25)                                   # past pie_tap_timeout
    d.app.motion(300, 120, mods())
    d.ctx.key_release("q")
    assert got == ["N"] and not d.popups()


def test_pie_drag_release():
    got = []
    d = _pie_on_q(got)
    d.app.motion(430, 250, mods())                     # a drag: release picks at once
    d.ctx.key_release("q")
    assert got == ["E"] and not d.popups()


def test_pie_tap_stays_open():
    got = []
    d = _pie_on_q(got)
    d.ctx.key_release("q")                             # a quick tap: click style
    assert d.popups() == 1 and not got
    d.app.motion(170, 250, mods())
    d.app.press(170, 250, mods())
    assert got == ["W"] and not d.popups()


def test_pie_sub_menu():
    from bui.components import MenuItem, PieMenu
    got = []
    d = Driver(Label("x"), size=(600, 500))
    d.ctx.set_pointer(300, 250)
    leaf = [MenuItem(n, (lambda n=n: got.append(n))) for n in "wesn"]
    PieMenu.show(d.ctx, [MenuItem("W"), MenuItem("Sub", submenu=leaf)])
    d.frame()
    d.app.motion(430, 250, mods())                     # east: "Sub"
    d.app.press(430, 250, mods())
    assert d.popups() == 1 and not got                 # the sub-pie, centred at the pointer
    d.frame()
    d.app.motion(430, 120, mods())                     # north of the new centre
    d.app.press(430, 120, mods())
    assert got == ["n"] and not d.popups()


def test_pie_more_level():
    from bui.components import MenuItem, PieMenu
    got = []
    d = Driver(Label("x"), size=(600, 500))
    d.ctx.set_pointer(300, 250)
    PieMenu.show(d.ctx, [MenuItem(str(i), (lambda i=i: got.append(i))) for i in range(10)])
    d.frame()
    d.app.key("3", "3", 0, mods())                     # numpad SE: the 8th slot, "More"
    d.frame()
    assert d.popups() == 1 and not got
    d.app.key("2", "2", 0, mods())                     # items 7, 8, 9 at W, E, S: S is 9
    assert got == [9] and not d.popups()


def test_pie_number_keys():
    from bui.components import MenuItem, PieMenu
    got = []
    d = Driver(Label("x"), size=(600, 500))
    PieMenu.show(d.ctx, [MenuItem(n, (lambda n=n: got.append(n))) for n in "WESN"], at=(300, 250))
    d.frame()
    d.app.key("5", "5", 0, mods())                     # the centre: no item
    d.app.key("1", "1", 0, mods())                     # SW: no item there
    assert d.popups() == 1
    d.app.key("8", "8", 0, mods())                     # numpad 8 is north
    assert got == ["N"] and not d.popups()


def test_pie_accelerator_keys():
    from bui.components import MenuItem, PieMenu
    got = []
    d = Driver(Label("x"), size=(600, 500))
    names = ["Object Mode", "Edit Mode", "Sculpt", "Open Shading"]
    PieMenu.show(d.ctx, [MenuItem(n, (lambda n=n: got.append(n))) for n in names], at=(300, 250))
    d.frame()
    # first letters of words: o, e, s; both of "Open Shading"'s are taken, so the second pass
    # gives it its first free letter, p
    d.app.key("m", "m", 0, mods())                     # no item has m
    assert d.popups() == 1
    d.app.key("p", "p", 0, mods())
    assert got == ["Open Shading"] and not d.popups()


def test_pie_open_animation():
    import time
    from bui.components import MenuItem, PieMenu

    def east_x(d):
        d.frame()
        x, y, w, h = dict(d.ctx.menu_rows(-1))["E"]
        return x + w / 2

    d = Driver(Label("x"), size=(600, 500))
    buipy.kit_set_animations(True)
    try:
        PieMenu.show(d.ctx, [MenuItem(n) for n in "WESN"], at=(300, 250))
        start = east_x(d)                              # items start at the center
        time.sleep(0.1)                                # past pie_animation_timeout
        end = east_x(d)
        assert start < 330 < 380 < end, (start, end)
    finally:
        buipy.kit_set_animations(False)
    d.ctx.close_popups()
    PieMenu.show(d.ctx, [MenuItem(n) for n in "WESN"], at=(300, 250))
    assert east_x(d) == end                            # reduced motion: no animation


def test_pie_menu_at():
    from bui.components import MenuItem, PieMenu
    got = []
    d = Driver(Label("x"), size=(600, 500))
    d.ctx.set_pointer(10, 10)
    PieMenu.show(d.ctx, [MenuItem(n, (lambda n=n: got.append(n))) for n in "WESN"], at=(300, 250))
    d.frame()
    d.app.motion(430, 250, mods())                     # east of the given centre
    d.app.press(430, 250, mods())
    assert got == ["E"] and not d.popups()


def test_hit():
    from bui.components import Button
    btn = Button("x")
    d = Driver(btn, size=(300, 200))
    x, y, w, h = btn.rect
    assert d.ctx.hit(x + w // 2, y + h // 2) is btn
    assert d.ctx.hit(-5, -5) is None


# -- color ------------------------------------------------------------------------------


def test_color_wheel_and_eyedropper():
    sw = ColorSwatch("", (1.0, 1.0, 1.0, 1.0))
    d = Driver(sw)
    d.click(sw)
    wheel = d.ctx.picker_part("area")
    x, y, w, h = wheel.rect
    cx, cy, r = x + w / 2, y + h / 2, min(w, h) / 2 - 1
    d.click(at=(int(cx), int(cy + r - 3)))             # bottom rim = red (Blender)
    rr, g, b, _ = sw.value
    assert rr > 0.9 and g < 0.15 and b < 0.15, sw.value
    strip = d.ctx.picker_part("strip")
    sx, sy, sw_, sh = strip.rect
    d.click(at=(sx + sw_ // 2, sy + sh - 1))           # value -> 0
    assert max(sw.value[:3]) < 0.02
    d.click(at=(sx + sw_ // 2, sy + 1))                # back up: hue survived black
    assert sw.value[0] > 0.9 and sw.value[1] < 0.15
    d.click(d.ctx.picker_part("dropper"))
    assert d.ctx.modal and d.app.cursor == "crosshair"
    d.app.press(2, d.app.size[1] - 2, mods())          # region background
    d.app.release(2, d.app.size[1] - 2, mods())
    assert not d.ctx.modal
    assert abs(sw.value[0] - 0x30 / 255) < 0.02, sw.value


# -- views ------------------------------------------------------------------------------


def test_tree_view():
    from bui.components import TreeItem, TreeView
    toggled = []
    a = TreeItem("A", TreeItem("A1"), TreeItem("A2"), toggles={"hide": False})
    b = TreeItem("B")
    tv = TreeView([a, b], columns=("hide",), on_toggle=lambda *t: toggled.append(t))
    d = Driver(tv)
    assert tv.row_count() == 4
    rows = tv.rows_info()
    x, y, w, h = rows[0][1]
    d.click(at=(x + 3, y + 5))                          # arrow: collapse A
    assert tv.row_count() == 2
    d.click(at=(x + 60, tv.rows_info()[1][1][1] + 5))   # select B
    assert tv.value is b
    d.key("Up")
    assert tv.value is a
    d.key("Right")
    assert a.expanded and tv.row_count() == 4
    col_x = x + w - buipy.kit_unit()
    d.click(at=(col_x + 5, y + 5))                      # eye toggle
    assert a.toggles["hide"] is True and toggled


def test_grid_view():
    from bui.components import GridView
    act = []
    g = GridView([("a", (1, 0, 0, 1)), ("b", "WORLD"), ("c", None)], on_activate=act.append)
    d = Driver(g)
    tx, ty, tw, th = g.tile_rect(1)
    d.click(at=(tx + tw // 2, ty + th // 2))
    assert g.value == 1
    d.key("Right")
    assert g.value == 2
    d.key("Return")
    assert act == [2]


# -- editors -----------------------------------------------------------------------------


def test_color_ramp():
    from bui.components import ColorRamp
    ramp = ColorRamp()
    d = Driver(ramp)
    band = ramp.band
    mid = (band.x + band.w // 2, band.y + 5)
    d.app.press(*mid, mods(ctrl=True))                 # Ctrl+click adds a stop
    d.app.release(*mid, mods())
    assert len(ramp.stops) == 3 and abs(ramp.active.pos - 0.5) < 0.02
    assert abs(ramp.active.color[0] - 0.5) < 0.03      # evaluated mid color
    assert abs(ramp.pos_field.value - ramp.active.pos) < 1e-6, "fields follow the active stop"
    d.drag(band, band.w // 4, start_dx=band.w // 2)     # drag it right
    assert ramp.active.pos > 0.7
    d.hover(band)
    d.key("x", "x")
    assert len(ramp.stops) == 2
    ramp.interpolation = "Constant"
    assert ramp.evaluate(0.99) == ramp.sorted_stops()[0].color


def test_curve_mapping():
    from bui.components import CurveMapping
    cm = CurveMapping()
    d = Driver(cm)
    g = cm.graph
    x, y = g.x + g.w // 2, g.y + g.h // 2               # on the diagonal
    d.app.press(x, y, mods())
    d.app.motion(x, y - 30, mods(button1=True))
    d.app.motion(x, y - 40, mods(button1=True))
    d.app.release(x, y - 40, mods())
    assert len(cm.points) == 3 and cm.evaluate(0.5) > 0.6
    d.hover(g)
    d.key("Delete")
    assert len(cm.points) == 2 and abs(cm.evaluate(0.5) - 0.5) < 1e-6


def test_normal_ball_hotkey_path():
    from bui.components import HotkeyField, NormalBall, PathField
    nb = NormalBall("", (0, 0, 1))
    hk = HotkeyField("", "F3")
    pf = PathField("", "//", on_browse=lambda: "C:/tmp/x.png")
    d = Driver(Column(nb, hk, pf))
    d.click(at=(nb.x + nb.w - 4, nb.y + nb.h // 2))
    assert nb.value[0] > 0.9
    d.click(hk)
    assert hk.capturing
    d.key("Control_L")                                 # modifiers alone don't bind
    d.key("k", "k", mods(ctrl=True, shift=True))
    assert hk.value == "Ctrl Shift K" and not hk.capturing
    d.click(pf.button)
    assert pf.value == "C:/tmp/x.png"


def test_id_selector():
    from bui.components import IDSelector
    ids = IDSelector(["Mat", "Glass"], "Mat")
    d = Driver(ids)
    d.click(ids.new)
    assert ids.value == "Mat.001" and "Mat.001" in ids.items
    ids.browse.set_value("Glass")
    assert ids.value == "Glass"
    assert ids.new.w == ids.unlink.w == buipy.kit_unit(), "icon buttons keep their size"


# -- containers ---------------------------------------------------------------------------


def test_panel_drag_collapse_and_reorder():
    ps = [Panel("P%d" % i, Button("b%d" % i)) for i in range(3)]
    d = Driver(*ps)
    x = ps[0].x + 30
    d.app.press(x, ps[0].y + 5, mods())                # collapse P0 ...
    for p in ps[1:]:
        d.frame()
        d.app.motion(x, p.y + 5, mods(button1=True))   # ... and drag over the others
    d.app.release(x, ps[2].y + 5, mods())
    d.frame()
    assert all(p.collapsed for p in ps)
    gx = ps[0].x + ps[0].w - 5                          # drag P0's grip below P2
    d.app.press(gx, ps[0].y + 5, mods())
    d.app.motion(gx, ps[2].y + ps[2].h, mods(button1=True))
    d.app.motion(gx, ps[2].y + ps[2].h + 5, mods(button1=True))
    d.app.release(gx, ps[2].y + ps[2].h + 5, mods())
    assert [p.title for p in d.app.root.children] == ["P1", "P2", "P0"]


def test_vector_alt_and_copy():
    v = VectorField("", (1.0, 2.0, 3.0))
    d = Driver(v)
    d.drag(v.fields[0], 40, m=mods(alt=True))          # Alt: all axes together
    delta = v.value[0] - 1.0
    assert delta > 0 and abs(v.value[1] - 2.0 - delta) < 1e-6 and abs(v.value[2] - 3.0 - delta) < 1e-6
    d.hover(v.fields[1])
    d.key("c", "c", mods(ctrl=True, alt=True))
    v.set_value((0, 0, 0))
    d.key("v", "v", mods(ctrl=True, alt=True))
    assert abs(v.value[2] - 3.0 - delta) < 1e-3, v.value


def test_scroll_side_tabs_popover():
    from bui.components import Popover, ScrollView, SideTabs
    sv = ScrollView(Column(*[Button(str(i)) for i in range(30)]), rows=4)
    inner = Dropdown("", ["a", "b"])
    pop = Popover("Opts", inner)
    tabs = SideTabs([("OBJECT_DATA", "Object", Column(sv, pop)),
                     ("MODIFIER", "Modifiers", Label("mods"))])
    d = Driver(tabs)
    d.wheel(sv, -1)
    assert sv.scroll > 0
    d.click(pop)
    assert d.popups() == 1
    d.click(inner)                                      # a menu inside the popover ...
    assert d.popups() == 2, "... nests instead of closing the popover"
    d.ctx.close_popups()
    u = buipy.kit_unit()
    d.click(at=(tabs.x + 5, tabs.y + round(0.4 * u) + round(1.4 * u) + 5))
    assert tabs.current == 1


def test_icons_render():
    names = buipy.kit_icon_names()
    assert len(names) > 100
    d = Driver(Column(*[Label(n, icon=n) for n in names]), size=(300, 3000))
    d.frame()


# -- rendering & performance ------------------------------------------------------------


def test_damage_driven_rendering():
    f = NumberField("N", 1.0)
    other = [Button("b%d" % i) for i in range(20)]
    d = Driver(Column(f, *other))
    raster = d.app._raster
    assert d.ctx.render(*d.app.size, raster) == 0, "idle frames draw nothing"
    d.hover(f)
    assert d.ctx.render(*d.app.size, raster) >= 1
    dmg = raster.damage()
    assert dmg and all(h <= 64 for _, _, _, h in dmg), "hover damages only the widget's tiles"


def test_render_and_perf():
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", "..", "examples"))
    import components_demo
    app = components_demo.build()
    r = app.render_raster()
    assert r.size == app.size
    w, h = app.size
    n = 50
    t0 = time.perf_counter()
    for _ in range(n):
        app.ctx.render(w, h, r, True)
    full_ms = (time.perf_counter() - t0) / n * 1000
    # hover over a number field and back: partial frames
    page = app.root.children[0].children[0]
    field = page.children[1].body.children[0].children[1].children[0]
    fx, fy = field.x + field.w // 2, field.y + field.h // 2
    fields = page.children[1].body.children[0].children[1].children
    points = [(f.x + f.w // 2, f.y + f.h // 2) for f in fields]
    t0 = time.perf_counter()
    for i in range(n):                                 # hover moves between fields
        app.motion(*points[i % len(points)], None)
        assert app.ctx.render(w, h, r) >= 1
    del fx, fy
    hover_ms = (time.perf_counter() - t0) / n * 1000
    print("  demo full frame: %.3f ms, hover frame: %.3f ms" % (full_ms, hover_ms))
    assert full_ms < 8.3, "full frames must fit a 120 Hz budget"
    assert hover_ms < 1.0, "interaction frames must be far below 1 ms"


# -- interaction polish: undo, Tab, multi-drag, animation, text areas ------------


def test_undo_redo():
    f = NumberField("Size", 1.0, step=0.1)
    t = TextField("Name", "abc")
    d = Driver(f, t)
    d.drag(f, 50)
    scrubbed = f.value
    assert scrubbed > 1.0
    d.click(t)
    d.type("d")
    d.key("Return")
    assert t.value == "abcd", t.value
    d.key("z", "z", mods(ctrl=True))                  # undo the text edit
    assert t.value == "abc", t.value
    d.key("z", "z", mods(ctrl=True))                  # undo the scrub (one step)
    assert f.value == 1.0, f.value
    assert d.ctx.undo_depth()[0] == 0
    d.key("z", "Z", mods(ctrl=True, shift=True))      # redo
    assert f.value == scrubbed, f.value
    d.key("y", "y", mods(ctrl=True))                  # Ctrl+Y redo too
    assert t.value == "abcd", t.value


def test_tab_moves_between_fields():
    a, b, c = TextField("A", ""), NumberField("B", 1), TextField("C", "")
    d = Driver(a, b, c)
    d.click(a)
    d.type("x")
    d.key("Tab")
    assert a.value == "x", "Tab must confirm the field it leaves"
    d.type("5")
    d.key("Tab")
    assert b.value == 5, b.value
    d.type("z")
    d.key("ISO_Left_Tab", m=mods(shift=True))          # Shift+Tab goes back
    assert c.value == "z", c.value
    d.key("Escape")
    assert d.ctx.focus is None or d.ctx.focus is b


def test_vertical_multi_drag():
    fields = [NumberField(n, 0.0, step=0.1) for n in "XYZ"]
    d = Driver(Column(*fields, align=True))
    x, y = d.center(fields[0])
    _, y2 = d.center(fields[2])
    d.app.press(x, y, mods())
    for yy in range(y + 2, y2 + 1, 4):                 # drag down over the fields
        d.app.motion(x, yy, mods(button1=True))
    for dx in (20, 40, 60):                            # then sideways: edit all
        d.app.motion(x + dx, y2, mods(button1=True))
    d.app.release(x + 60, y2, mods())
    d.frame()
    vals = [f.value for f in fields]
    assert vals[0] > 0 and all(abs(v - vals[0]) < 1e-9 for v in vals), vals
    d.key("z", "z", mods(ctrl=True))                  # one undo step for the group
    assert [f.value for f in fields] == [0.0, 0.0, 0.0]


def test_panel_animation_timing():
    """Blender's interface_panel.cc do_animate: ANIMATION_TIME 0.30 s eased by sqrt(fac)."""
    buipy.kit_set_animations(True)
    try:
        p = Panel("One", Column(*[Button(str(i)) for i in range(12)]))
        d = Driver(p, Button("after"))
        h_open = p.h
        t0 = time.perf_counter()
        d.click(at=(p.x + 20, p.y + 5))
        d.frame()
        samples = []
        while d.ctx.animating() and time.perf_counter() - t0 < 2:
            time.sleep(0.005)
            d.frame()
            samples.append((time.perf_counter() - t0, p.h))
        took = samples[-1][0]
        h_closed = p.h
        assert 0.25 < took < 0.6, took                 # ~0.30 s (was a linear 0.12 s)
        # sqrt easing: early on, it has moved well over its share of the time. A wide
        # window: a busy machine can sleep tens of ms between two frames
        early = [(t, hh) for t, hh in samples if 0.03 < t < 0.15]
        assert early, samples
        t, hh = early[0]
        moved = (h_open - hh) / (h_open - h_closed)
        assert moved > (t / 0.30) + 0.1, (t, moved)
    finally:
        buipy.kit_set_animations(False)


def test_panel_animation():
    buipy.kit_set_animations(True)
    try:
        p = Panel("One", Column(*[Button(str(i)) for i in range(6)]))
        d = Driver(p, Button("after"))
        h = p.h
        d.click(at=(p.x + 20, p.y + 5))
        assert p.collapsed and d.ctx.animating()
        seen, deadline = set(), time.time() + 2
        while d.ctx.animating() and time.time() < deadline:
            time.sleep(0.01)
            d.frame()
            seen.add(p.h)
        assert not d.ctx.animating() and p.h < h
        assert any(p.h < v < h for v in seen), "no in-between frames"
    finally:
        buipy.kit_set_animations(False)


def test_textarea_editing():
    root = buipy.KitNode("frame")
    ta = buipy.KitNode("textarea")
    root.add(ta)
    ta.geom(1, fill=3, expand=1)
    ctx = buipy.KitContext(root)
    r = buipy.Raster(300, 200)
    ctx.render(300, 200, r, True)
    x, y, w, h = ta.rect
    ctx.press(x + 10, y + 10, 0)
    ctx.release(x + 10, y + 10, 0)
    for ch in "ab":
        ctx.key(ch, ch, 0)
    ctx.key("", "Return", 0)
    ctx.key("c", "c", 0)
    assert ta.get_str("value") == "ab\nc", repr(ta.get_str("value"))
    ctx.key("", "Up", 0)
    ctx.key("", "BackSpace", 0)
    assert ta.get_str("value") == "b\nc", repr(ta.get_str("value"))  # caret was after "a"
    ctx.key("z", "z", 2)                       # a typing burst is one undo step
    assert ta.get_str("value") == "", repr(ta.get_str("value"))
    ctx.key("Z", "Z", 3)
    assert ta.get_str("value") == "b\nc"
    ctx.render(300, 200, r, False)


def test_textarea_component():
    from bui.components import TextArea
    got = []
    ta = TextArea("one", rows=3, on_change=got.append)
    tall = TextArea(rows=6)
    d = Driver(ta, tall)
    assert ta.type == "textarea" and ta.value == "one"
    assert tall.h > ta.h                                  # rows sets the natural height
    d.click(ta)
    d.key("End")
    d.key("Return")
    d.type("two")
    assert ta.value == "one\ntwo", repr(ta.value)
    assert got[-1] == "one\ntwo"                          # on_change on every edit
    got.clear()
    ta.set_value("a\nb", notify=False)
    assert ta.value == "a\nb" and got == []
    ta.value = "c"
    assert got == ["c"]
    ta.value = "c"                                        # unchanged: no callback
    assert got == ["c"]
    ta.reset()
    assert ta.value == "" and got == ["c", ""]


def test_file_browser_component():
    import shutil
    import tempfile
    from bui.components import FileBrowser
    tmp = tempfile.mkdtemp(prefix="bui_fbc_")
    saved = os.environ.get("BUI_BOOKMARKS")
    os.environ["BUI_BOOKMARKS"] = os.path.join(tmp, "bookmarks.txt")
    try:
        root = os.path.join(tmp, "files")
        os.makedirs(os.path.join(root, "sub"))
        open(os.path.join(root, "a.txt"), "w").close()
        folders, chosen = [], []
        fb = FileBrowser(root, filters=["Text\t*.txt"], on_change=folders.append,
                         on_submit=chosen.append, fill=True)
        d = Driver(Label("Pick a file"), fb, size=(900, 600))
        assert fb.type == "filebrowser" and fb.directory == root + os.sep
        assert fb.w > 0 and fb.h > 0 and fb.y > 0             # laid out below the label
        assert fb.get("count") == 3                           # "..", sub, a.txt
        fb.directory = os.path.join(root, "sub")
        assert folders == [os.path.join(root, "sub") + os.sep]
        fb.directory = root
        fb.file = "a.txt"
        fb.execute()
        assert fb.state == 1 and chosen == [[os.path.join(root, "a.txt")]]
        assert fb.result == [os.path.join(root, "a.txt")]
        d.frame()

        saver = FileBrowser(root, mode="save", file="new.txt", on_submit=chosen.append)
        Driver(saver, size=(900, 600))
        saver.cancel()
        assert saver.state == -1 and chosen[-1] == []
        try:
            FileBrowser(mode="load")
        except ValueError:
            pass
        else:
            raise AssertionError("unknown mode accepted")
    finally:
        if saved is None:
            os.environ.pop("BUI_BOOKMARKS", None)
        else:
            os.environ["BUI_BOOKMARKS"] = saved
        shutil.rmtree(tmp, ignore_errors=True)


def test_editing_children():
    a, b, c = Label("a"), Label("b"), Label("c")
    col = Column(a, b)
    panel = Panel("Panel", Label("x"))
    d = Driver(col, panel)
    texts = lambda node: [n.get_str("text") for n in node.children]  # noqa: E731
    assert col.insert(1, c) is c and texts(col) == ["a", "c", "b"]
    d.frame()
    assert a.y < c.y < b.y                                # relaid out in the new order
    assert col.remove(a) is a and texts(col) == ["c", "b"] and a.parent is None
    assert col.insert(-1, a) is a and texts(col) == ["c", "b", "a"]
    new = Label("n")
    assert col.replace(b, new) is new and texts(col) == ["c", "n", "a"]
    try:
        col.replace(b, Label("z"))
    except ValueError:
        pass
    else:
        raise AssertionError("replace() of a non-child accepted")
    assert col.clear() is col and col.children == []
    # a Panel edits its body, like add()
    y = panel.insert(0, Label("y"))
    assert texts(panel.body) == ["y", "x"] and len(panel.children) == 1
    panel.replace(y, a)                                   # moves a from nowhere into the body
    assert texts(panel.body) == ["a", "x"]
    col.add(a)                                            # adding elsewhere moves it
    assert texts(panel.body) == ["x"] and texts(col) == ["a"]
    panel.clear()
    assert panel.body.children == [] and len(panel.children) == 1
    d.frame()



def test_editing_review_fixes():
    from bui.components import Node, NodeEditor
    # replace(): an earlier sibling takes old's place, not the slot after it
    a, b, c, d_ = Label("a"), Label("b"), Label("c"), Label("d")
    col = Column(a, b, c, d_)
    col.replace(c, a)
    assert [n.get_str("text") for n in col.children] == ["b", "a", "d"]
    # a NodeEditor follows every way of editing its children, and its links follow their nodes
    n1, n2, n3 = (Node(t, outputs=[("o", "float")], inputs=[("i", "float")]) for t in "xyz")
    ed = NodeEditor([n1, n2], links=[(n1, 0, n2, 0)])
    ed.add(n3)
    assert ed.nodes == [n1, n2, n3] and ed.links == [(n1, 0, n2, 0)]
    first = Node("w", inputs=[("i", "float")])
    ed.insert(0, first)                                   # every index moves up by one
    assert ed.nodes == [first, n1, n2, n3] and ed.links == [(n1, 0, n2, 0)]
    ed.remove(first)
    assert ed.nodes == [n1, n2, n3] and ed.links == [(n1, 0, n2, 0)]
    other = Node("v", outputs=[("o", "float")], inputs=[("i", "float")])
    ed.replace(n3, other)
    assert ed.nodes == [n1, n2, other]
    ed.remove(n1)                                         # a link to a removed node goes with it
    assert ed.nodes == [n2, other] and ed.links == []
    assert ed.clear() is ed and ed.nodes == [] and ed.links == []
    # a TextArea written with the text it holds keeps its selection
    from bui.components import TextArea
    ta = TextArea("hello")
    ta.set_value("hello")
    assert ta.value == "hello"


def test_node_editor_height():
    from bui.components import Node, NodeEditor
    u = buipy.kit_unit()
    short = NodeEditor([Node("Value", outputs=[("Value", "float")])], height=8)
    default = NodeEditor()
    assert short.natural()[1] == 8 * u
    assert default.natural()[1] == 14 * u
    Driver(short, default, size=(500, 900))
    assert short.h == 8 * u and default.h == 14 * u

def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print("ok", t.__name__)
    print("all component tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
