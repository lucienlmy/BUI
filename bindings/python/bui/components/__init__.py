"""bui.components — Blender's UI as a component library, running in C.

Compose components into a tree and hand it to an :class:`App`::

    from bui.components import (App, Panel, NumberSlider, EnumRow, Checkbox,
                                Button)

    App("Demo", (420, 600),
        Panel("Object",
              NumberSlider("Size", 1.0, min=0, max=10, on_change=print),
              EnumRow("Mode", ["Object", "Edit"]),
              Checkbox("Smooth", True),
              Button("Apply", on_click=lambda: print("apply"))),
    ).run()

Each component is a node of the C kit (``kit/``): Blender's widget drawing
ported from ``interface_widgets.cc``, Blender's interaction rules, and
damage-driven rendering — an idle UI costs nothing, a hover or a scrub
redraws only the touched widget. Python builds the tree and receives
callbacks.

Modules: ``core`` (base classes), ``widgets``, ``containers``, ``views``,
``menus``, ``editors``, ``templates``, ``app``.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from bui.components.app import App
from bui.components.containers import (Area, AreaHeader, Box, Column, Grid, Panel, Row, ScrollView, Sidebar, SideTabs,
                                       Split, Tabs, VectorField)
from bui.components.core import Component, ValueComponent, unit
from bui.components.editors import (ColorRamp, ColorStop, CurveMapping, Histogram, NormalBall,
                                    Popover)
from bui.components.nodes import Node, NodeEditor
from bui.components.menus import Menu, MenuBar, MenuButton, MenuItem, PieMenu
from bui.components.templates import IDSelector, PathField
from bui.components.views import FileBrowser, GridView, ListView, TreeItem, TreeView
from bui.components.widgets import (Button, Checkbox, ColorSwatch, Decorator, Dropdown,
                                    EnumRow, HotkeyField, IconButton, Image, Label, NumberField,
                                    NumberSlider, ProgressBar, SearchField, Separator, Spacer,
                                    TextArea, TextField, Toggle, Video, eval_number)

__all__ = [
    "App", "Component", "ValueComponent", "unit",
    # layout
    "Column", "Row", "Split", "Grid", "Box", "Panel", "Tabs", "SideTabs", "Sidebar", "ScrollView", "Area", "AreaHeader",
    "VectorField",
    # widgets
    "Label", "Image", "Video", "Separator", "Spacer", "ProgressBar", "Button", "IconButton", "Toggle",
    "Checkbox", "Decorator", "TextField", "TextArea", "SearchField", "HotkeyField", "NumberField",
    "NumberSlider", "Dropdown", "EnumRow", "ColorSwatch", "eval_number",
    # views
    "ListView", "TreeView", "TreeItem", "GridView", "FileBrowser",
    # menus & popups
    "Menu", "MenuItem", "MenuButton", "MenuBar", "PieMenu", "Popover",
    # editors & templates
    "ColorRamp", "ColorStop", "CurveMapping", "NormalBall", "Histogram",
    "IDSelector", "PathField", "NodeEditor", "Node",
]
