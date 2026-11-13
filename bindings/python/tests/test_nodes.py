"""Node editor and area header: links, cut, move, select, delete, values, pan, zoom.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import os
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from bui.components import (App, Area, Label, MenuButton, Node, NodeEditor,  # noqa: E402
                            Spacer, unit)

M0 = SimpleNamespace(shift=False, ctrl=False, alt=False, button1=False)
M1 = SimpleNamespace(shift=False, ctrl=False, alt=False, button1=True)
CTRL = SimpleNamespace(shift=False, ctrl=True, alt=False, button1=False)


def make():
    a = Node("Value", outputs=[("Value", "float")], location=(0, 0), kind="input")
    b = Node("Math", inputs=[("A", "float", 0.5), ("B", "float", 0.25)],
             outputs=[("Result", "float")], location=(300, 0))
    ed = NodeEditor([a, b], fill=True)
    app = App("nodes", (800, 500), ed, padding=0)
    app.render_raster()
    ed.set("zoom", 1.0)
    ed.set("panx", 50.0)
    ed.set("pany", 50.0)
    app.render_raster()
    return app, ed, a, b


def to_screen(ed, gx, gy):
    x, y, w, h = ed.rect
    return int(x + ed.get("panx") + gx * ed.get("zoom")), int(y + ed.get("pany") + gy * ed.get("zoom"))


def out_socket(ed, node, k=0):
    gx, gy = node.location
    return to_screen(ed, gx + node.get("nwidth"), gy + 20 + 5 + k * 22 + 10)


def in_socket(ed, node, k):
    gx, gy = node.location
    outs = sum(1 for s in node.strs("items") if s.startswith("o|"))
    return to_screen(ed, gx, gy + 20 + 5 + (outs + k) * 22 + 10)


def drag(app, p0, p1, steps=4, m=M1):
    app.press(*p0, M0)
    for i in range(1, steps + 1):
        app.motion(p0[0] + (p1[0] - p0[0]) * i // steps, p0[1] + (p1[1] - p0[1]) * i // steps, m)
    app.release(*p1, M0)
    app.render_raster()


def test_link_and_relink():
    app, ed, a, b = make()
    seen = []
    ed.on_links = seen.append
    drag(app, out_socket(ed, a), in_socket(ed, b, 1))
    assert ed.links == [(a, 0, b, 1)], ed.links
    assert seen and seen[-1] == ed.links
    # dragging from the linked input picks the link up; dropping on A relinks
    drag(app, in_socket(ed, b, 1), in_socket(ed, b, 0))
    assert ed.links == [(a, 0, b, 0)], ed.links
    # dropping on nothing removes it
    drag(app, in_socket(ed, b, 0), to_screen(ed, 150, 300))
    assert ed.links == [], ed.links


def test_cut_links():
    app, ed, a, b = make()
    ed.links = [(a, 0, b, 0)]
    x0, y0 = out_socket(ed, a)
    x1, y1 = in_socket(ed, b, 0)
    mx = (x0 + x1) // 2
    app.ctx.context_menu(mx, min(y0, y1) - 40, 2)  # Ctrl+RMB press
    for y in range(min(y0, y1) - 40, max(y0, y1) + 41, 10):
        app.motion(mx, y, CTRL)
    app.ctx.button(mx, max(y0, y1) + 40, 3, True, 2)
    assert ed.links == [], ed.links


def test_select_move_delete():
    app, ed, a, b = make()
    moved = []
    b.on_change = lambda n, what, v: moved.append((what, v))
    title = to_screen(ed, 300 + 70, 10)
    drag(app, title, (title[0] + 40, title[1] + 20))
    assert b.selected and not a.selected
    assert b.location == (340.0, 20.0), b.location
    assert ("location", (340.0, 20.0)) in moved, moved
    # box select both
    drag(app, to_screen(ed, -20, -20), to_screen(ed, 500, 200))
    assert a.selected and b.selected
    # click on empty space deselects
    p = to_screen(ed, 150, 350)
    app.press(*p, M0)
    app.release(*p, M0)
    assert not a.selected and not b.selected
    # X deletes the selection
    a.selected = True
    app.motion(*p, M0)
    app.key("x", "x", 0, M0)
    assert ed.nodes == [b], ed.nodes


def test_value_field_and_view():
    app, ed, a, b = make()
    changes = []
    b.on_change = lambda n, what, v: changes.append((what, v))
    x, y = in_socket(ed, b, 0)
    drag(app, (x + 60, y), (x + 110, y))
    assert abs(b.input_value(0) - 1.0) < 1e-6, b.input_value(0)
    assert changes and changes[-1][0] == "value"
    # MMB pans
    px = ed.get("panx")
    app.ctx.button(400, 300, 2, False, 0)
    app.motion(450, 300, M0)
    app.ctx.button(450, 300, 2, True, 0)
    assert ed.get("panx") == px + 50
    # the wheel zooms about the cursor
    z = ed.zoom
    app.wheel(400, 300, 1, M0)
    assert ed.zoom > z


def control_pos(ed, node, k):
    """The center of control ``k``: below the header and the outputs, in the padded block."""
    gx, gy = node.location
    outs = sum(1 for s in node.strs("items") if s.startswith("o|"))
    return to_screen(ed, gx + node.get("nwidth") / 2, gy + 20 + 5 + outs * 22 + 2.5 + k * 22 + 10)


def test_node_controls():
    events = []
    n = Node("Tool", inputs=[("In", "float")], outputs=[("Out", "float")], location=(0, 0),
             controls=[("button", "Run"), ("checkbox", "Wireframe", False),
                       ("dropdown", "Mode", ["Fast", "Exact", "Slow"], 0)],
             on_change=lambda node, what, v: events.append((what, v)))
    ed = NodeEditor([n], fill=True)
    app = App("nodes", (800, 500), ed, padding=0)
    app.render_raster()
    ed.set("zoom", 1.0)
    ed.set("panx", 50.0)
    ed.set("pany", 50.0)
    app.render_raster()
    # the block sits between the outputs and the inputs: the input socket moved down by it
    gx, gy = n.location
    ix, iy = to_screen(ed, gx, gy + 20 + 5 + 1 * 22 + (5 + 3 * 22) + 10)
    ox, oy = out_socket(ed, n)
    assert ix == to_screen(ed, 0, 0)[0] and iy > oy + 3 * 22, (iy, oy)
    # a button reports its press (and no value)
    app.press(*control_pos(ed, n, 0), M0)
    app.release(*control_pos(ed, n, 0), M0)
    app.render_raster()
    assert events[-1] == ("control", (0, None)), events
    # a checkbox toggles
    app.press(*control_pos(ed, n, 1), M0)
    app.release(*control_pos(ed, n, 1), M0)
    assert events[-1] == ("control", (1, True)) and n.control_value(1) is True, events
    app.press(*control_pos(ed, n, 1), M0)
    app.release(*control_pos(ed, n, 1), M0)
    assert n.control_value(1) is False
    # a dropdown opens a menu of its options; the picked one is the new value
    app.press(*control_pos(ed, n, 2), M0)
    app.release(*control_pos(ed, n, 2), M0)
    app.render_raster()
    assert app.ctx.popup_count() == 1, "the dropdown's menu is open"
    assert n.control_value(2) == 0
    rows = app.ctx.menu_rows(0)
    assert [r[0] for r in rows] == ["Fast", "Exact", "Slow"], rows
    x, y, w, h = rows[2][1]
    app.motion(x + 10, y + h // 2, M0)
    app.press(x + 10, y + h // 2, M0)
    app.release(x + 10, y + h // 2, M0)
    app.render_raster()
    assert app.ctx.popup_count() == 0 and n.control_value(2) == 2
    assert events[-1] == ("control", (2, 2)), events
    # pressing the control did not move or select the node
    assert n.location == (0.0, 0.0)


def test_node_controls_validated():
    try:
        Node("x", controls=[("slider", "S")])
    except ValueError:
        pass
    else:
        raise AssertionError("an unknown control kind was accepted")


def test_area_header():
    picked = []
    ed = NodeEditor()
    area = Area("Geometry Node Editor", ed, header=[MenuButton("View", ["Frame All"]), Spacer(),
                                                   Label("Right")], on_editor=picked.append)
    app = App("area", (600, 400), area, padding=0)
    app.render_raster()
    hx, hy, hw, hh = area.header.rect
    assert hh == round(26 * unit() / 20), hh
    assert ed.rect[1] == hy + hh and ed.rect[3] == 400 - hh, (ed.rect, area.header.rect)
    assert area.editor_button.rect[0] == hx + round(8 * unit() / 20)
    area._pick("Shader Editor")
    assert picked == ["Shader Editor"]


def main():
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("ok")


if __name__ == "__main__":
    main()
