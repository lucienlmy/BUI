"""Qt's layouts on BUI: QBoxLayout (Tk's packer: Blender's column and row), QGridLayout, QFormLayout
(Blender's property split: labels right-aligned beside their fields) and QStackedLayout, with spacer
items, stretch factors and size policies deciding which widgets take extra space as in Qt.

A layout is a kit frame inside its widget; its items are (node, place) pairs: the stretch factor in a
box, the cell (row, column, row span, column span, alignment) in a grid.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import buipy

from bui.qtsyntax import GRID, PACK, E, N, S, W, QLabel, QRect, QSize, QWidget, Qt, Signal


class QSizePolicy:
    Fixed, Minimum, Maximum, Preferred, Expanding, MinimumExpanding, Ignored = range(7)

    def __init__(self, h=3, v=3, control_type=None):
        self._h, self._v = h, v
        self._hs = self._vs = 0

    def horizontalPolicy(self):  # noqa: N802
        return self._h

    def verticalPolicy(self):  # noqa: N802
        return self._v

    def setHorizontalPolicy(self, p):  # noqa: N802
        self._h = p

    def setVerticalPolicy(self, p):  # noqa: N802
        self._v = p

    def horizontalStretch(self):  # noqa: N802
        return self._hs

    def verticalStretch(self):  # noqa: N802
        return self._vs

    def setHorizontalStretch(self, s):  # noqa: N802
        self._hs = int(s)

    def setVerticalStretch(self, s):  # noqa: N802
        self._vs = int(s)

    def expandingDirections(self):  # noqa: N802
        return ((Qt.Horizontal if self._h in _GROWS else 0) | (Qt.Vertical if self._v in _GROWS else 0))

    def transposed(self):
        return QSizePolicy(self._v, self._h)

    def __eq__(self, other):
        return isinstance(other, QSizePolicy) and (self._h, self._v) == (other._h, other._v)


_GROWS = (QSizePolicy.Expanding, QSizePolicy.MinimumExpanding)


class QMargins:
    def __init__(self, left=0, top=0, right=0, bottom=0):
        self._m = (int(left), int(top), int(right), int(bottom))
        self._v = self._m[0]

    def left(self):
        return self._m[0]

    def top(self):
        return self._m[1]

    def right(self):
        return self._m[2]

    def bottom(self):
        return self._m[3]

    def isNull(self):  # noqa: N802
        return not any(self._m)

    def __eq__(self, other):
        return isinstance(other, QMargins) and self._m == other._m


# ==========================================================================
# Items


class _Spacer(buipy.KitNode):
    """A spacer item's node: empty, of its size, growing as its size policy says."""

    def __init__(self, w, h, h_policy, v_policy):
        super().__init__("frame")
        self._policy = (h_policy, v_policy)
        self._size = (int(w), int(h))
        self.geom(0, reqwidth=int(w), reqheight=int(h))


class QLayoutItem:
    """One entry of a layout: a widget, a layout or a spacer."""

    def __init__(self, node=None):
        self._w = node

    def widget(self):
        return self._w if isinstance(self._w, QWidget) else None

    def layout(self):
        return self._w if isinstance(self._w, QLayout) else None

    def spacerItem(self):  # noqa: N802
        return self if isinstance(self._w, _Spacer) else None

    def isEmpty(self):  # noqa: N802
        return self.widget() is None and self.layout() is None

    def expandingDirections(self):  # noqa: N802
        return (Qt.Horizontal if _expands(self._w, True) else 0) | (Qt.Vertical if _expands(self._w, False) else 0)

    def sizeHint(self):  # noqa: N802
        return QSize(*self._w.natural()) if hasattr(self._w, "natural") else QSize(0, 0)

    def geometry(self):
        return QRect(*buipy.KitNode.rect.__get__(self._w))

    def __eq__(self, other):
        return isinstance(other, QLayoutItem) and other._w is self._w

    def __hash__(self):
        return id(self._w)


QWidgetItem = QLayoutItem


class QSpacerItem(QLayoutItem):
    def __init__(self, w, h, h_policy=QSizePolicy.Minimum, v_policy=QSizePolicy.Minimum):
        super().__init__(_Spacer(w, h, h_policy, v_policy))

    def changeSize(self, w, h, h_policy=QSizePolicy.Minimum, v_policy=QSizePolicy.Minimum):  # noqa: N802
        s = self._w
        s._policy, s._size = (h_policy, v_policy), (int(w), int(h))
        s.geom(int(s.geom_get("manager")), reqwidth=int(w), reqheight=int(h))
        lay = s.parent
        if isinstance(lay, QLayout):
            lay._relayout()

    def sizePolicy(self):  # noqa: N802
        return QSizePolicy(*self._w._policy)

    def sizeHint(self):  # noqa: N802
        return QSize(*self._w._size)


# Size policies (Qt defaults): which widgets take extra space.
_H_EXPANDING = ("text", "slider", "progress", "dropdown", "textarea", "list", "tree", "tabs",
                "canvas", "scroll", "number")
_V_EXPANDING = ("textarea", "list", "tree", "tabs", "canvas", "scroll")


def _expands(node, horizontal):
    if isinstance(node, QLayout):
        return any(_expands(c, horizontal) for c, _ in node._items)
    if isinstance(node, _Spacer):
        return node._policy[0 if horizontal else 1] in _GROWS
    if isinstance(node, QWidget) and node._policy is not None \
            and node._policy[0 if horizontal else 1] is not None:
        return node._policy[0 if horizontal else 1] in _GROWS
    kinds = _H_EXPANDING if horizontal else _V_EXPANDING
    if isinstance(node, QWidget) and node._layout is not None:
        return _expands(node._layout, horizontal)
    return getattr(node, "type", "") in kinds or (
        isinstance(node, QWidget) and node._custom_paint and not horizontal)


# ==========================================================================
# QLayout


class QLayout(buipy.KitNode):
    SetDefaultConstraint, SetNoConstraint, SetMinimumSize, SetFixedSize, SetMaximumSize, SetMinAndMaxSize = \
        range(6)

    def __init__(self, parent=None):
        super().__init__("frame")
        self._owner = None
        self._items = []
        self._spacing = round(0.3 * buipy.kit_unit())
        self._margins = None
        self._enabled = True
        self._constraint = self.SetDefaultConstraint
        if isinstance(parent, QWidget):
            parent.setLayout(self)

    def setSpacing(self, s):  # noqa: N802
        self._spacing = int(s)
        self._relayout()

    def spacing(self):
        return self._spacing

    def setContentsMargins(self, left, top=None, right=None, bottom=None):  # noqa: N802
        """The kit pads a frame evenly: the left margin is used on every side."""
        m = left if isinstance(left, QMargins) else QMargins(left, left if top is None else top,
                                                             left if right is None else right,
                                                             left if bottom is None else bottom)
        self._margins = m
        self.set("padding", m.left())

    def contentsMargins(self):  # noqa: N802
        return self._margins or QMargins()

    def getContentsMargins(self):  # noqa: N802
        return self.contentsMargins()._m

    def contentsRect(self):  # noqa: N802
        m = self.contentsMargins()
        r = self.geometry()
        return QRect(r.x() + m.left(), r.y() + m.top(), r.width() - m.left() - m.right(),
                     r.height() - m.top() - m.bottom())

    def geometry(self):
        return QRect(*buipy.KitNode.rect.__get__(self))

    def parentWidget(self):  # noqa: N802
        return self._owner

    # -- items ----------------------------------------------------------------------------------------------
    def count(self):
        return len(self._items)

    def itemAt(self, i):  # noqa: N802
        return QLayoutItem(self._items[i][0]) if 0 <= i < len(self._items) else None

    def indexOf(self, w):  # noqa: N802
        w = w._w if isinstance(w, QLayoutItem) else w
        return next((i for i, (node, _) in enumerate(self._items) if node is w), -1)

    def isEmpty(self):  # noqa: N802
        return all(QLayoutItem(node).isEmpty() for node, _ in self._items)

    def takeAt(self, i):  # noqa: N802
        """Take item i out (its widget stays alive, without a parent)."""
        if not 0 <= i < len(self._items):
            return None
        node, _ = self._items.pop(i)
        if node.parent is self:
            self.remove(node)
        self._relayout()
        return QLayoutItem(node)

    def removeWidget(self, w):  # noqa: N802
        i = self.indexOf(w)
        if i >= 0:
            self.takeAt(i)

    def removeItem(self, item):  # noqa: N802
        self.removeWidget(item._w)

    def replaceWidget(self, old, new, options=None):  # noqa: N802
        """`new` takes `old`'s place (its stretch or its cell); returns old's item, or None."""
        i = self.indexOf(old)
        if i < 0:
            return None
        place = self._items[i][1]
        self._adopt(new)
        self._items[i] = (new, place)
        if old.parent is self:
            self.remove(old)
        self._placed(new, place)
        return QLayoutItem(old)

    def addItem(self, item):  # noqa: N802
        if item.widget() is not None:
            self.addWidget(item.widget())
        elif item.layout() is not None:
            self.addLayout(item.layout())
        else:
            self._append(item._w, 0)

    def addChildWidget(self, w):  # noqa: N802
        self._adopt(w)

    def addChildLayout(self, layout):  # noqa: N802
        layout._owner = self._owner
        self.add(layout)

    # -- state ----------------------------------------------------------------------------------------------
    def isEnabled(self):  # noqa: N802
        return self._enabled

    def setEnabled(self, b):  # noqa: N802
        self._enabled = bool(b)

    def sizeConstraint(self):  # noqa: N802
        return self._constraint

    def setSizeConstraint(self, c):  # noqa: N802
        """SetFixedSize keeps a window at its natural size (the host's min and max size)."""
        self._constraint = int(c)
        w = self._owner
        if w is not None and c == self.SetFixedSize:
            size = w.sizeHint()
            w.setMinimumSize(size.width(), size.height())
            w.setMaximumSize(size.width(), size.height())

    def update(self):
        self._relayout()

    def activate(self):
        self._relayout()
        return True

    def sizeHint(self):  # noqa: N802
        return QSize(*self.natural())

    def minimumSize(self):  # noqa: N802
        return self.sizeHint()

    def maximumSize(self):  # noqa: N802
        return QSize(16777215, 16777215)

    def expandingDirections(self):  # noqa: N802
        return QLayoutItem(self).expandingDirections()

    # -- what each layout does ----------------------------------------------------------------------------
    def _append(self, node, place):
        self._adopt(node)
        self._items.append((node, place))
        self._placed(node, place)

    def _placed(self, node, place):
        self._relayout()

    def _relayout(self):
        pass

    def _adopt(self, w):
        if w.parent is not None and w.parent is not self:
            w.parent.remove(w)
        if isinstance(w, QLayout):
            w._owner = self._owner
        if w.parent is not self:
            self.add(w)


# ==========================================================================
# Boxes


class QBoxLayout(QLayout):
    LeftToRight, RightToLeft, TopToBottom, BottomToTop = range(4)
    Down, Up = TopToBottom, BottomToTop
    _vertical = True

    def __init__(self, direction=None, parent=None):
        if isinstance(direction, QWidget):
            direction, parent = None, direction
        self._dir = (self.TopToBottom if self._vertical else self.LeftToRight) if direction is None \
            else int(direction)
        self._vertical = self._dir in (self.TopToBottom, self.BottomToTop)
        super().__init__(parent)

    def direction(self):
        return self._dir

    def setDirection(self, d):  # noqa: N802
        """Top-to-bottom, bottom-to-top (the packer's bottom side), left-to-right or right-to-left."""
        self._dir = int(d)
        self._vertical = self._dir in (self.TopToBottom, self.BottomToTop)
        self._policies()

    # -- adding ---------------------------------------------------------------------------------------------
    def addWidget(self, w, stretch=0, alignment=0):  # noqa: N802
        self.insertWidget(-1, w, stretch, alignment)

    def insertWidget(self, index, w, stretch=0, alignment=0):  # noqa: N802
        self._insert(index, w, stretch)

    def addLayout(self, layout, stretch=0):  # noqa: N802
        self.insertLayout(-1, layout, stretch)

    def insertLayout(self, index, layout, stretch=0):  # noqa: N802
        self._insert(index, layout, stretch)

    def addStretch(self, stretch=1):  # noqa: N802
        self.insertStretch(-1, stretch)

    def insertStretch(self, index, stretch=1):  # noqa: N802
        self._insert(index, buipy.KitNode("frame"), max(1, stretch))

    def addSpacing(self, size):  # noqa: N802
        self.insertSpacing(-1, size)

    def insertSpacing(self, index, size):  # noqa: N802
        spacer = buipy.KitNode("frame")
        spacer.geom(0, **({"reqheight": int(size)} if self._vertical else {"reqwidth": int(size)}))
        self._insert(index, spacer, 0)

    def addSpacerItem(self, item):  # noqa: N802
        self.insertSpacerItem(-1, item)

    def insertSpacerItem(self, index, item):  # noqa: N802
        self._insert(index, item._w, 0)

    def addItem(self, item):  # noqa: N802
        self.insertItem(-1, item)

    def insertItem(self, index, item):  # noqa: N802
        self._insert(index, item._w, 0)

    def addStrut(self, size):  # noqa: N802
        """The smallest size across the box: a vertical box at least `size` wide."""
        self.geom(int(self.geom_get("manager")), **({"reqwidth": int(size)} if self._vertical else
                                                     {"reqheight": int(size)}))

    def _insert(self, index, node, stretch):
        self._adopt(node)
        index = len(self._items) if index < 0 else min(int(index), len(self._items))
        self._items.insert(index, (node, stretch))
        for i, (n, _) in enumerate(self._items):
            buipy.KitNode.add(self, n, i)
        self._policies()

    # -- stretch --------------------------------------------------------------------------------------------
    def stretch(self, index):
        return self._items[index][1] if 0 <= index < len(self._items) else -1

    def setStretch(self, index, stretch):  # noqa: N802
        w, _ = self._items[index]
        self._items[index] = (w, stretch)
        self._policies()

    def setStretchFactor(self, w, stretch):  # noqa: N802
        i = self.indexOf(w)
        if i < 0:
            return False
        self.setStretch(i, stretch)
        return True

    def _placed(self, node, place):
        for i, (n, _) in enumerate(self._items):
            buipy.KitNode.add(self, n, i)
        self._policies()

    def _relayout(self):
        self._policies()

    def _policies(self):
        """Qt box semantics on Tk's packer: items stack along the axis; extra
        space goes to stretch factors, else to expanding widgets, else (for
        horizontal boxes) is shared by all."""
        v = self._vertical
        rev = self._dir in (self.BottomToTop, self.RightToLeft)
        any_stretch = any(s > 0 for _, s in self._items)
        expanders = [w for w, s in self._items if (s > 0 if any_stretch else _expands(w, not v))]
        if not v and not expanders:
            expanders = [w for w, _ in self._items]
        side = (1 if rev else 0) if v else (3 if rev else 2)
        for k, (w, s) in enumerate(self._items):
            gap = self._spacing if k else 0
            pads = {"pady0": 0, "pady1": 0, "padx0": 0, "padx1": 0}
            pads[("pady" if v else "padx") + ("1" if rev else "0")] = gap
            w.geom(PACK, side=side, fill=1 if v else 2, expand=1 if w in expanders else 0, **pads)
            if (v and _expands(w, True)) or not v:
                w.geom(PACK, fill=3)
            if v and w in expanders:
                w.geom(PACK, fill=3)


class QVBoxLayout(QBoxLayout):
    _vertical = True

    def __init__(self, parent=None):
        super().__init__(QBoxLayout.TopToBottom, parent)


class QHBoxLayout(QBoxLayout):
    _vertical = False

    def __init__(self, parent=None):
        super().__init__(QBoxLayout.LeftToRight, parent)


# ==========================================================================
# Grids and forms


class QGridLayout(QLayout):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._hspacing = self._vspacing = None
        self._stretch = ({}, {})  # row -> stretch, column -> stretch
        self._minimum = ({}, {})  # row -> minimum height, column -> minimum width
        self._origin = Qt.TopLeftCorner

    @staticmethod
    def _cell(row, column, rest, alignment):
        """Qt's overloads: (row, column, alignment=0) and (row, column, rowSpan, columnSpan, alignment=0):
        a single argument after the column is the alignment, never a span."""
        if len(rest) == 1:
            return int(row), int(column), 1, 1, rest[0]
        span = (list(rest) + [1, 1])[:2]
        return int(row), int(column), int(span[0]), int(span[1]), rest[2] if len(rest) > 2 else alignment

    def addWidget(self, w, row=0, column=0, *rest, alignment=0):  # noqa: N802
        self._append(w, self._cell(row, column, rest, alignment))

    def addLayout(self, layout, row=0, column=0, *rest, alignment=0):  # noqa: N802
        self._append(layout, self._cell(row, column, rest, alignment))

    def addItem(self, item, row=0, column=0, *rest, alignment=0):  # noqa: N802
        self._append(item._w, self._cell(row, column, rest, alignment))

    def _placed(self, w, place):
        row, column, rowspan, columnspan, alignment = place
        sticky = W | E | ((N | S) if _expands(w, False) else 0)
        if alignment & Qt.AlignLeft:
            sticky &= ~E
        if alignment & Qt.AlignRight:
            sticky &= ~W
        if alignment & Qt.AlignTop:
            sticky &= ~S
        if alignment & Qt.AlignBottom:
            sticky &= ~N
        gx, gy = self.horizontalSpacing(), self.verticalSpacing()  # between cells, not around the edges
        w.geom(GRID, row=row, column=column, rowspan=rowspan, columnspan=columnspan,
               sticky=sticky, padx0=gx if column else 0, pady0=gy if row else 0)
        if _expands(w, True) and (column + columnspan - 1) not in self._stretch[1]:
            self._track(1, column + columnspan - 1, 1.0)
        if _expands(w, False) and (row + rowspan - 1) not in self._stretch[0]:
            self._track(0, row + rowspan - 1, 1.0)

    def _track(self, axis, index, weight=None):
        """A row (axis 0) or column (1) on the kit's grid: its weight and minimum size."""
        if weight is None:
            weight = self._stretch[axis].get(index, 0)
        self.grid_weight(axis, index, float(weight), self._minimum[axis].get(index, 0))

    def _relayout(self):
        for w, place in self._items:
            self._placed(w, place)

    # -- cells ----------------------------------------------------------------------------------------------
    def itemAtPosition(self, row, column):  # noqa: N802
        for w, (r, c, rs, cs, _) in self._items:
            if r <= row < r + rs and c <= column < c + cs:
                return QLayoutItem(w)
        return None

    def getItemPosition(self, index):  # noqa: N802
        r, c, rs, cs, _ = self._items[index][1]
        return r, c, rs, cs

    def cellRect(self, row, column):  # noqa: N802
        item = self.itemAtPosition(row, column)
        return item.geometry() if item is not None else QRect()

    def rowCount(self):  # noqa: N802
        return 1 + max((r + rs - 1 for _, (r, _c, rs, _cs, _a) in self._items), default=-1)

    def columnCount(self):  # noqa: N802
        return 1 + max((c + cs - 1 for _, (_r, c, _rs, cs, _a) in self._items), default=-1)

    # -- stretch, minimums and spacing ----------------------------------------------------------------------
    def setRowStretch(self, row, stretch):  # noqa: N802
        self._stretch[0][int(row)] = int(stretch)
        self._track(0, int(row))

    def setColumnStretch(self, column, stretch):  # noqa: N802
        self._stretch[1][int(column)] = int(stretch)
        self._track(1, int(column))

    def rowStretch(self, row):  # noqa: N802
        return self._stretch[0].get(int(row), 0)

    def columnStretch(self, column):  # noqa: N802
        return self._stretch[1].get(int(column), 0)

    def setRowMinimumHeight(self, row, h):  # noqa: N802
        self._minimum[0][int(row)] = int(h)
        self._track(0, int(row))

    def setColumnMinimumWidth(self, column, w):  # noqa: N802
        self._minimum[1][int(column)] = int(w)
        self._track(1, int(column))

    def rowMinimumHeight(self, row):  # noqa: N802
        return self._minimum[0].get(int(row), 0)

    def columnMinimumWidth(self, column):  # noqa: N802
        return self._minimum[1].get(int(column), 0)

    def horizontalSpacing(self):  # noqa: N802
        return self._spacing if self._hspacing is None else self._hspacing

    def verticalSpacing(self):  # noqa: N802
        return self._spacing if self._vspacing is None else self._vspacing

    def setHorizontalSpacing(self, s):  # noqa: N802
        self._hspacing = int(s)
        self._relayout()

    def setVerticalSpacing(self, s):  # noqa: N802
        self._vspacing = int(s)
        self._relayout()

    def originCorner(self):  # noqa: N802
        return self._origin

    def setOriginCorner(self, corner):  # noqa: N802
        self._origin = corner


class QFormLayout(QGridLayout):
    """Rows of (label, field): Blender's property split, the label right-aligned beside its field."""
    LabelRole, FieldRole, SpanningRole = 0, 1, 2
    FieldsStayAtSizeHint, ExpandingFieldsGrow, AllNonFixedFieldsGrow = 0, 1, 2
    DontWrapRows, WrapLongRows, WrapAllRows = 0, 1, 2

    def __init__(self, parent=None):
        super().__init__(parent)
        self._rows = []  # (label or None, field or None, spanning)
        self._growth = self.AllNonFixedFieldsGrow
        self._label_align = Qt.AlignRight | Qt.AlignVCenter
        self._form_align = Qt.AlignLeft | Qt.AlignTop
        self._wrap = self.DontWrapRows

    def addRow(self, label, field=None):  # noqa: N802
        self.insertRow(-1, label, field)

    def insertRow(self, row, label, field=None):  # noqa: N802
        """(label text or widget, field) or one widget (or layout) across both columns."""
        if isinstance(label, str) and field is not None:
            label = QLabel(label)
            label.setAlignment(self._label_align)
        entry = (None, label, True) if field is None else (label, field, False)
        row = len(self._rows) if row < 0 else min(int(row), len(self._rows))
        self._rows.insert(row, entry)
        self._regrid()

    def _regrid(self):
        """Every row in its grid cells (rows move when one is inserted or removed)."""
        for node, _ in list(self._items):
            if node.parent is self:
                self.remove(node)
        self._items = []
        for r, (label, field, spanning) in enumerate(self._rows):
            if spanning:
                self._append(field, (r, 0, 1, 2, 0))
                continue
            if label is not None:
                self._append(label, (r, 0, 1, 1, 0))
            if field is not None:
                self._append(field, (r, 1, 1, 1, 0))
        self.setColumnStretch(1, 0 if self._growth == self.FieldsStayAtSizeHint else 1)

    def rowCount(self):  # noqa: N802
        return len(self._rows)

    def removeRow(self, row):  # noqa: N802
        self.takeRow(row)

    def takeRow(self, row):  # noqa: N802
        if not 0 <= row < len(self._rows):
            return None
        label, field, _ = self._rows.pop(row)
        self._regrid()
        return (QLayoutItem(label) if label is not None else None, QLayoutItem(field))

    def labelForField(self, field):  # noqa: N802
        return next((lab for lab, f, _ in self._rows if f is field), None)

    def getWidgetPosition(self, w):  # noqa: N802
        for r, (label, field, spanning) in enumerate(self._rows):
            if w is field:
                return r, self.SpanningRole if spanning else self.FieldRole
            if w is label:
                return r, self.LabelRole
        return -1, self.LabelRole

    getLayoutPosition = getWidgetPosition

    def getItemPosition(self, index):  # noqa: N802
        return self.getWidgetPosition(self._items[index][0])

    def itemAt(self, *args):  # noqa: N802 - (index) or (row, role)
        if len(args) == 1:
            return super().itemAt(args[0])
        row, role = args
        if not 0 <= row < len(self._rows):
            return None
        label, field, spanning = self._rows[row]
        node = field if (spanning and role == self.SpanningRole) or (not spanning and role == self.FieldRole) \
            else label if role == self.LabelRole and not spanning else None
        return QLayoutItem(node) if node is not None else None

    def setWidget(self, row, role, w):  # noqa: N802
        while len(self._rows) <= row:
            self._rows.append((None, None, False))
        label, field, _ = self._rows[row]
        self._rows[row] = ((w, field, False) if role == self.LabelRole else
                           (None, w, True) if role == self.SpanningRole else (label, w, False))
        self._regrid()

    setLayout = setWidget

    def setItem(self, row, role, item):  # noqa: N802
        self.setWidget(row, role, item._w)

    def isRowVisible(self, row):  # noqa: N802
        node = next((n for n in self._rows[row][1::-1] if n is not None), None)
        return node is not None and bool(node.get("visible"))

    def setRowVisible(self, row, visible):  # noqa: N802
        for node in self._rows[row][:2]:
            if node is not None:
                node.set("visible", bool(visible))

    def labelAlignment(self):  # noqa: N802
        return self._label_align

    def setLabelAlignment(self, a):  # noqa: N802
        self._label_align = a
        for label, _, spanning in self._rows:
            if not spanning and isinstance(label, QLabel):
                label.setAlignment(a)

    def formAlignment(self):  # noqa: N802
        return self._form_align

    def setFormAlignment(self, a):  # noqa: N802
        self._form_align = a

    def fieldGrowthPolicy(self):  # noqa: N802
        return self._growth

    def setFieldGrowthPolicy(self, p):  # noqa: N802
        """FieldsStayAtSizeHint keeps fields at their natural width (no column stretch)."""
        self._growth = int(p)
        self.setColumnStretch(1, 0 if p == self.FieldsStayAtSizeHint else 1)

    def rowWrapPolicy(self):  # noqa: N802
        return self._wrap

    def setRowWrapPolicy(self, p):  # noqa: N802
        self._wrap = int(p)


# ==========================================================================
# Stacks


class QStackedLayout(QLayout):
    StackOne, StackAll = 0, 1
    currentChanged = Signal(int)
    widgetRemoved = Signal(int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._current = -1
        self._mode = self.StackOne

    def addWidget(self, w):  # noqa: N802
        return self.insertWidget(-1, w)

    def insertWidget(self, index, w):  # noqa: N802
        self._adopt(w)
        index = len(self._items) if index < 0 else min(int(index), len(self._items))
        self._items.insert(index, (w, 0))
        w.geom(PACK, fill=3, expand=1)
        if self._current < 0:
            self.setCurrentIndex(0)
        elif index <= self._current:
            self._current += 1  # the current widget stays current
        self._show()
        return index

    def takeAt(self, i):  # noqa: N802
        was = self._current
        item = super().takeAt(i)
        if item is not None:
            self.widgetRemoved.emit(i)
            if i < was or was >= len(self._items):
                self._current = was - 1 if self._items else -1
                if was == i:
                    self.currentChanged.emit(self._current)
            elif i == was:
                self.currentChanged.emit(self._current)
            self._show()
        return item

    def count(self):
        return len(self._items)

    def currentIndex(self):  # noqa: N802
        return self._current

    def setCurrentIndex(self, i):  # noqa: N802
        if 0 <= i < len(self._items) and i != self._current:
            self._current = i
            self._show()
            self.currentChanged.emit(i)

    def currentWidget(self):  # noqa: N802
        return self._items[self._current][0] if 0 <= self._current < len(self._items) else None

    def setCurrentWidget(self, w):  # noqa: N802
        self.setCurrentIndex(self.indexOf(w))

    def widget(self, i=None):
        if i is None:
            return self.currentWidget()
        return self._items[i][0] if 0 <= i < len(self._items) else None

    def stackingMode(self):  # noqa: N802
        return self._mode

    def setStackingMode(self, mode):  # noqa: N802
        """StackAll shows every page (stacked in their frame); StackOne only the current one."""
        self._mode = int(mode)
        self._show()

    def _show(self):
        for k, (w, _) in enumerate(self._items):
            w.set("visible", self._mode == self.StackAll or k == self._current)
