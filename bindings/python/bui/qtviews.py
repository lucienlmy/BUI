"""Qt's model/view classes on the kit: models (QStandardItemModel, QStringListModel and the
abstract ones you subclass) and views (QListView, QTreeView, QTableView) that draw any model in
Blender's tree list. A view redraws when its model reports a change (dataChanged, rowsInserted,
rowsRemoved, modelReset ...).

SPDX-License-Identifier: GPL-2.0-or-later
"""

import buipy

from bui.qtsyntax import EV_CHANGE, EV_DOUBLE, EV_PRESS, QWidget, Qt, Signal


class QModelIndex:
    """A cell of a model: row, column and parent, or invalid."""
    __slots__ = ("_m", "_r", "_c", "_p")

    def __init__(self, model=None, row=-1, column=-1, pointer=None):
        self._m, self._r, self._c, self._p = model, row, column, pointer

    def isValid(self):  # noqa: N802
        return self._m is not None and self._r >= 0 and self._c >= 0

    def row(self):
        return self._r

    def column(self):
        return self._c

    def model(self):
        return self._m

    def internalPointer(self):  # noqa: N802
        return self._p

    def parent(self):
        return self._m.parent(self) if self.isValid() else QModelIndex()

    def data(self, role=Qt.DisplayRole):
        return self._m.data(self, role) if self.isValid() else None

    def flags(self):
        return self._m.flags(self) if self.isValid() else 0

    def sibling(self, row, column):
        return self._m.index(row, column, self.parent()) if self.isValid() else QModelIndex()

    def siblingAtColumn(self, column):  # noqa: N802
        return self.sibling(self._r, column)

    def siblingAtRow(self, row):  # noqa: N802
        return self.sibling(row, self._c)

    def _key(self):
        return (id(self._m), self._r, self._c, id(self._p))

    def __eq__(self, other):
        return isinstance(other, QModelIndex) and self._key() == other._key()

    def __hash__(self):
        return hash(self._key())

    def __repr__(self):
        return "QModelIndex(%d, %d)" % (self._r, self._c) if self.isValid() else "QModelIndex()"


QPersistentModelIndex = QModelIndex


class QAbstractItemModel:
    """The model interface: subclass it and implement index, parent, rowCount, columnCount, data."""
    dataChanged = Signal(object, object, list)
    headerDataChanged = Signal(int, int, int)
    rowsInserted = Signal(object, int, int)
    rowsRemoved = Signal(object, int, int)
    columnsInserted = Signal(object, int, int)
    columnsRemoved = Signal(object, int, int)
    modelReset = Signal()
    layoutChanged = Signal()

    def __init__(self, parent=None):
        self._parent_obj = parent

    def index(self, row, column=0, parent=None):
        raise NotImplementedError("QAbstractItemModel.index: implement it in the subclass")

    def parent(self, index=None):
        return QModelIndex()

    def rowCount(self, parent=None):  # noqa: N802
        return 0

    def columnCount(self, parent=None):  # noqa: N802
        return 1

    def hasChildren(self, parent=None):  # noqa: N802
        return self.rowCount(parent or QModelIndex()) > 0

    def data(self, index, role=Qt.DisplayRole):
        return None

    def setData(self, index, value, role=Qt.EditRole):  # noqa: N802
        return False

    def headerData(self, section, orientation=Qt.Horizontal, role=Qt.DisplayRole):  # noqa: N802
        return section + 1 if role == Qt.DisplayRole else None

    def flags(self, index):
        return Qt.ItemIsSelectable | Qt.ItemIsEnabled if index.isValid() else 0

    def createIndex(self, row, column, pointer=None):  # noqa: N802
        return QModelIndex(self, row, column, pointer)

    def sibling(self, row, column, index):
        return self.index(row, column, self.parent(index))

    def match(self, start, role, value, hits=1, flags=Qt.MatchExactly):
        """Rows under start's parent whose data equals (or, with MatchContains, contains) value."""
        out, parent = [], start.parent()
        for r in range(start.row(), self.rowCount(parent)):
            idx = self.index(r, start.column(), parent)
            v = self.data(idx, role)
            ok = str(value) in str(v) if flags & Qt.MatchContains else v == value
            if ok:
                out.append(idx)
                if 0 < hits <= len(out):
                    break
        return out

    # the notifications a subclass brackets its changes with
    def beginResetModel(self):  # noqa: N802
        pass

    def endResetModel(self):  # noqa: N802
        self.modelReset.emit()

    def beginInsertRows(self, parent, first, last):  # noqa: N802
        self._pending = (parent, first, last)

    def endInsertRows(self):  # noqa: N802
        self.rowsInserted.emit(*self._pending)

    def beginRemoveRows(self, parent, first, last):  # noqa: N802
        self._pending = (parent, first, last)

    def endRemoveRows(self):  # noqa: N802
        self.rowsRemoved.emit(*self._pending)

    def beginInsertColumns(self, parent, first, last):  # noqa: N802
        self._pending = (parent, first, last)

    def endInsertColumns(self):  # noqa: N802
        self.columnsInserted.emit(*self._pending)

    def beginRemoveColumns(self, parent, first, last):  # noqa: N802
        self._pending = (parent, first, last)

    def endRemoveColumns(self):  # noqa: N802
        self.columnsRemoved.emit(*self._pending)


class QAbstractTableModel(QAbstractItemModel):
    def index(self, row, column=0, parent=None):
        if parent is not None and parent.isValid():
            return QModelIndex()
        ok = 0 <= row < self.rowCount() and 0 <= column < self.columnCount()
        return self.createIndex(row, column) if ok else QModelIndex()


class QAbstractListModel(QAbstractTableModel):
    def columnCount(self, parent=None):  # noqa: N802
        return 1


class QStringListModel(QAbstractListModel):
    def __init__(self, strings=None, parent=None):
        super().__init__(parent)
        self._list = [str(s) for s in (strings or [])]

    def stringList(self):  # noqa: N802
        return list(self._list)

    def setStringList(self, strings):  # noqa: N802
        self._list = [str(s) for s in strings]
        self.endResetModel()

    def rowCount(self, parent=None):  # noqa: N802
        return 0 if parent is not None and parent.isValid() else len(self._list)

    def data(self, index, role=Qt.DisplayRole):
        if index.isValid() and role in (Qt.DisplayRole, Qt.EditRole):
            return self._list[index.row()]
        return None

    def setData(self, index, value, role=Qt.EditRole):  # noqa: N802
        if not index.isValid() or role not in (Qt.DisplayRole, Qt.EditRole):
            return False
        self._list[index.row()] = str(value)
        self.dataChanged.emit(index, index, [role])
        return True

    def flags(self, index):
        return super().flags(index) | Qt.ItemIsEditable if index.isValid() else 0

    def insertRows(self, row, count, parent=None):  # noqa: N802
        self.beginInsertRows(QModelIndex(), row, row + count - 1)
        self._list[row:row] = [""] * count
        self.endInsertRows()
        return True

    def removeRows(self, row, count, parent=None):  # noqa: N802
        self.beginRemoveRows(QModelIndex(), row, row + count - 1)
        del self._list[row:row + count]
        self.endRemoveRows()
        return True


class QStandardItem:
    """A cell of a QStandardItemModel: data by role, and rows of child items."""

    def __init__(self, text_or_icon=None, text=None):
        self._data = {}
        self._rows = []  # each row: a list of items (columns)
        self._parent = None
        self._model = None
        self._flags = Qt.ItemIsSelectable | Qt.ItemIsEnabled | Qt.ItemIsEditable
        if isinstance(text_or_icon, str):
            self._data[Qt.DisplayRole] = text_or_icon
        elif text_or_icon is not None:
            self._data[Qt.DecorationRole] = text_or_icon
            if text is not None:
                self._data[Qt.DisplayRole] = str(text)

    # -- data ----------------------------------------------------------------------------------------
    def data(self, role=Qt.UserRole + 1):
        if role == Qt.EditRole:
            role = Qt.DisplayRole
        return self._data.get(role)

    def setData(self, value, role=Qt.UserRole + 1):  # noqa: N802
        self._data[Qt.DisplayRole if role == Qt.EditRole else role] = value
        if self._model is not None:
            idx = self.index()
            self._model.dataChanged.emit(idx, idx, [role])

    def text(self):
        v = self.data(Qt.DisplayRole)
        return "" if v is None else str(v)

    def setText(self, text):  # noqa: N802
        self.setData(str(text), Qt.DisplayRole)

    def toolTip(self):  # noqa: N802
        return self.data(Qt.ToolTipRole) or ""

    def setToolTip(self, tip):  # noqa: N802
        self.setData(str(tip), Qt.ToolTipRole)

    def icon(self):
        return self.data(Qt.DecorationRole)

    def setIcon(self, icon):  # noqa: N802
        self.setData(icon, Qt.DecorationRole)

    def checkState(self):  # noqa: N802
        return self.data(Qt.CheckStateRole) or Qt.Unchecked

    def setCheckState(self, state):  # noqa: N802
        self.setData(state, Qt.CheckStateRole)

    def flags(self):
        return self._flags

    def setFlags(self, flags):  # noqa: N802
        self._flags = flags

    def _flag(self, flag, on):
        self._flags = self._flags | flag if on else self._flags & ~flag

    def isEditable(self):  # noqa: N802
        return bool(self._flags & Qt.ItemIsEditable)

    def setEditable(self, b):  # noqa: N802
        self._flag(Qt.ItemIsEditable, b)

    def isEnabled(self):  # noqa: N802
        return bool(self._flags & Qt.ItemIsEnabled)

    def setEnabled(self, b):  # noqa: N802
        self._flag(Qt.ItemIsEnabled, b)

    def isSelectable(self):  # noqa: N802
        return bool(self._flags & Qt.ItemIsSelectable)

    def setSelectable(self, b):  # noqa: N802
        self._flag(Qt.ItemIsSelectable, b)

    def isCheckable(self):  # noqa: N802
        return bool(self._flags & Qt.ItemIsUserCheckable)

    def setCheckable(self, b):  # noqa: N802
        self._flag(Qt.ItemIsUserCheckable, b)

    # -- tree ----------------------------------------------------------------------------------------
    def parent(self):
        return self._parent

    def model(self):
        return self._model

    def _position(self):
        siblings = self._parent._rows if self._parent is not None else []
        for r, row in enumerate(siblings):
            for c, item in enumerate(row):
                if item is self:
                    return r, c
        return -1, -1

    def row(self):
        return self._position()[0]

    def column(self):
        return self._position()[1]

    def index(self):
        if self._model is None:
            return QModelIndex()
        r, c = self._position()
        return QModelIndex(self._model, r, c, self._parent)

    def rowCount(self):  # noqa: N802
        return len(self._rows)

    def columnCount(self):  # noqa: N802
        return max((len(r) for r in self._rows), default=0)

    def hasChildren(self):  # noqa: N802
        return bool(self._rows)

    def child(self, row, column=0):
        if 0 <= row < len(self._rows) and 0 <= column < len(self._rows[row]):
            return self._rows[row][column]
        return None

    def _adopt(self, item):
        item._parent, item._model = self, self._model
        for row in item._rows:
            for child in row:
                item._adopt(child)

    def insertRow(self, row, items):  # noqa: N802
        items = items if isinstance(items, (list, tuple)) else [items]
        row = max(0, min(int(row), len(self._rows)))
        for it in items:
            self._adopt(it)
        self._rows.insert(row, list(items))
        if self._model is not None:
            self._model.rowsInserted.emit(self.index() if self._parent is not None else QModelIndex(), row, row)

    def appendRow(self, items):  # noqa: N802
        self.insertRow(len(self._rows), items)

    def appendRows(self, items):  # noqa: N802
        for it in items:
            self.appendRow(it)

    def setChild(self, row, column, item=None):  # noqa: N802
        if item is None:
            row, column, item = row, 0, column
        while len(self._rows) <= row:
            self._rows.append([])
        cells = self._rows[row]
        while len(cells) <= column:
            cells.append(QStandardItem(""))
            self._adopt(cells[-1])
        self._adopt(item)
        cells[column] = item
        if self._model is not None:
            self._model.layoutChanged.emit()

    def takeChild(self, row, column=0):  # noqa: N802
        item = self.child(row, column)
        if item is not None:
            self._rows[row][column] = QStandardItem("")
            self._adopt(self._rows[row][column])
            item._parent = item._model = None
        return item

    def removeRows(self, row, count):  # noqa: N802
        del self._rows[row:row + count]
        if self._model is not None:
            self._model.rowsRemoved.emit(self.index() if self._parent is not None else QModelIndex(),
                                         row, row + count - 1)

    def removeRow(self, row):  # noqa: N802
        self.removeRows(row, 1)

    def __repr__(self):
        return "QStandardItem(%r)" % self.text()


class QStandardItemModel(QAbstractItemModel):
    """A model of QStandardItems: rows and columns, nested to any depth."""
    itemChanged = Signal(object)

    def __init__(self, rows=0, columns=0, parent=None):
        if isinstance(rows, QWidget):
            rows, columns, parent = 0, 0, rows
        super().__init__(parent)
        self._root = QStandardItem()
        self._root._model = self
        self._headers = {}
        self._columns = int(columns)
        self.dataChanged.connect(self._item_changed)
        for _ in range(int(rows)):
            self._root.appendRow([QStandardItem("") for _ in range(max(1, self._columns))])

    def _item_changed(self, top, bottom, roles=None):
        item = self.itemFromIndex(top)
        if item is not None:
            self.itemChanged.emit(item)

    def invisibleRootItem(self):  # noqa: N802
        return self._root

    def _item(self, index):
        return self._root if index is None or not index.isValid() else index.internalPointer().child(
            index.row(), index.column())

    def itemFromIndex(self, index):  # noqa: N802
        return self._item(index) if index.isValid() else None

    def indexFromItem(self, item):  # noqa: N802
        return item.index() if item is not None else QModelIndex()

    def index(self, row, column=0, parent=None):
        owner = self._item(parent)
        if owner is None or owner.child(row, column) is None:
            return QModelIndex()
        return QModelIndex(self, row, column, owner)

    def parent(self, index=None):
        if index is None or not index.isValid():
            return QModelIndex()
        owner = index.internalPointer()
        return owner.index() if owner is not self._root else QModelIndex()

    def rowCount(self, parent=None):  # noqa: N802
        item = self._item(parent)
        return item.rowCount() if item is not None else 0

    def columnCount(self, parent=None):  # noqa: N802
        item = self._item(parent)
        n = item.columnCount() if item is not None else 0
        return max(n, self._columns) if item is self._root else n

    def data(self, index, role=Qt.DisplayRole):
        item = self.itemFromIndex(index)
        return item.data(role) if item is not None else None

    def setData(self, index, value, role=Qt.EditRole):  # noqa: N802
        item = self.itemFromIndex(index)
        if item is None:
            return False
        item.setData(value, role)
        return True

    def flags(self, index):
        item = self.itemFromIndex(index)
        return item.flags() if item is not None else 0

    def item(self, row, column=0):
        return self._root.child(row, column)

    def setItem(self, row, column, item=None):  # noqa: N802
        self._root.setChild(row, column, item)

    def appendRow(self, items):  # noqa: N802
        self._root.appendRow(items)

    def insertRow(self, row, items):  # noqa: N802
        self._root.insertRow(row, items)

    def removeRow(self, row, parent=None):  # noqa: N802
        self._item(parent).removeRows(row, 1)
        return True

    def removeRows(self, row, count, parent=None):  # noqa: N802
        self._item(parent).removeRows(row, count)
        return True

    def takeItem(self, row, column=0):  # noqa: N802
        return self._root.takeChild(row, column)

    def setRowCount(self, n):  # noqa: N802
        n = max(0, int(n))  # Qt ignores negative counts
        while self._root.rowCount() > n:
            self._root.removeRow(self._root.rowCount() - 1)
        while self._root.rowCount() < n:
            self._root.appendRow([QStandardItem("") for _ in range(max(1, self.columnCount()))])

    def setColumnCount(self, n):  # noqa: N802
        self._columns = max(0, int(n))
        for row in self._root._rows:  # reducing the columns drops the extra cells
            del row[self._columns:]
        self.layoutChanged.emit()

    def clear(self):
        self._root = QStandardItem()
        self._root._model = self
        self._headers = {}
        self.endResetModel()

    def setHorizontalHeaderLabels(self, labels):  # noqa: N802
        for c, text in enumerate(labels):
            self._headers[c] = str(text)
        self._columns = max(self._columns, len(labels))
        self.headerDataChanged.emit(Qt.Horizontal, 0, len(labels) - 1)

    def setHeaderData(self, section, orientation, value, role=Qt.EditRole):  # noqa: N802
        if orientation != Qt.Horizontal:
            return False
        self._headers[section] = str(value)
        self.headerDataChanged.emit(orientation, section, section)
        return True

    def headerData(self, section, orientation=Qt.Horizontal, role=Qt.DisplayRole):  # noqa: N802
        if role == Qt.DisplayRole and orientation == Qt.Horizontal and section in self._headers:
            return self._headers[section]
        return super().headerData(section, orientation, role)

    def findItems(self, text, flags=Qt.MatchExactly, column=0):  # noqa: N802
        out = []
        for r in range(self._root.rowCount()):
            item = self._root.child(r, column)
            if item is not None and (text in item.text() if flags & Qt.MatchContains else item.text() == text):
                out.append(item)
        return out


class QItemSelectionModel:
    """The view's current item and selection (one row at a time: Blender's lists select one)."""
    currentChanged = Signal(object, object)
    currentRowChanged = Signal(object, object)
    selectionChanged = Signal(object, object)
    NoUpdate, Clear, Select, ClearAndSelect, Current, Rows = 0, 1, 2, 3, 16, 32

    def __init__(self, model=None, view=None):
        self._model, self._view, self._current = model, view, QModelIndex()

    def model(self):
        return self._model

    def currentIndex(self):  # noqa: N802
        return self._current

    def hasSelection(self):  # noqa: N802
        return self._current.isValid()

    def isSelected(self, index):  # noqa: N802
        return index.isValid() and index == self._current

    def selectedIndexes(self):  # noqa: N802
        return [self._current] if self._current.isValid() else []

    def selectedRows(self, column=0):  # noqa: N802
        return [self._current.siblingAtColumn(column)] if self._current.isValid() else []

    def setCurrentIndex(self, index, command=ClearAndSelect):  # noqa: N802
        if self._view is not None:
            self._view.setCurrentIndex(index)
        else:
            self._set(index)

    select = setCurrentIndex

    def clearSelection(self):  # noqa: N802
        self.setCurrentIndex(QModelIndex())

    clear = clearCurrentIndex = clearSelection

    def _set(self, index):
        old = self._current
        if index == old:
            return
        self._current = index
        self.currentChanged.emit(index, old)
        if index.row() != old.row() or index.parent() != old.parent():
            self.currentRowChanged.emit(index, old)
        self.selectionChanged.emit([index] if index.isValid() else [], [old] if old.isValid() else [])


def _text(v):
    return "" if v is None else str(v)


class _ItemView(QWidget):
    """A model drawn in Blender's tree list: QListView (flat, one column), QTreeView (nested,
    every column) and QTableView (flat, every column) differ only in those two settings."""
    KIND = "tree"
    _NESTED = False
    _ALL_COLUMNS = True
    clicked = Signal(object)
    doubleClicked = Signal(object)
    pressed = Signal(object)
    activated = Signal(object)
    NoSelection, SingleSelection = 0, 1

    def _setup(self):
        self._model = None
        self._sel = QItemSelectionModel()
        self._root = QModelIndex()
        self._column = 0
        self._nodes = {}
        self._headers_hidden = not self._ALL_COLUMNS
        self.set("rows", 8)
        self.set("events", True)

    # -- model -----------------------------------------------------------------------------------------
    def setModel(self, model):  # noqa: N802
        signals = ("dataChanged", "rowsInserted", "rowsRemoved", "columnsInserted", "columnsRemoved",
                   "modelReset", "layoutChanged", "headerDataChanged")
        if self._model is not None:
            for s in signals:
                getattr(self._model, s).disconnect(self._refresh)
        self._model = model
        self._sel = QItemSelectionModel(model, self)
        self._root = QModelIndex()
        if model is not None:
            for s in signals:
                getattr(model, s).connect(self._refresh)
        self._refresh()

    def model(self):
        return self._model

    def selectionModel(self):  # noqa: N802
        return self._sel

    def rootIndex(self):  # noqa: N802
        return self._root

    def setRootIndex(self, index):  # noqa: N802
        self._root = index
        self._refresh()

    def _columns(self):
        if not self._ALL_COLUMNS:
            return [self._column]
        return list(range(max(1, self._model.columnCount(self._root))))

    def _refresh(self, *_):
        current = self.currentIndex()
        self._nodes = {}
        roots = []
        m = self._model
        if m is not None:
            cols = self._columns()
            self.set("headers", [] if self._headers_hidden else
                     [_text(m.headerData(c, Qt.Horizontal, Qt.DisplayRole)) for c in cols])
            roots = [self._build(m.index(r, cols[0], self._root), cols) for r in range(m.rowCount(self._root))]
        self.set_tree_roots(roots)
        node = self._nodes.get(current)
        if node is not None:
            self.tree_select(node)  # the current row stays current across a refresh
        else:
            self._sel._set(QModelIndex())

    def _build(self, index, cols):
        m = self._model
        parent = index.parent()
        texts = [_text(m.data(m.index(index.row(), c, parent), Qt.DisplayRole)) for c in cols]
        node = buipy.KitTreeItem(texts[0])
        node.set_values(texts[1:])
        node._qindex = index
        self._nodes[index] = node
        if self._NESTED:
            for r in range(m.rowCount(index)):
                node.add(self._build(m.index(r, cols[0], index), cols))
        return node

    # -- current item ------------------------------------------------------------------------------
    def currentIndex(self):  # noqa: N802
        n = self.tree_selected()
        return getattr(n, "_qindex", QModelIndex()) if n is not None else QModelIndex()

    def setCurrentIndex(self, index):  # noqa: N802
        node = self._nodes.get(index.siblingAtColumn(self._columns()[0]) if index.isValid() else index)
        self.tree_select(node)
        self._sel._set(self.currentIndex())

    def clearSelection(self):  # noqa: N802
        self.setCurrentIndex(QModelIndex())

    def _on_event(self, kind, ref, a, b):
        index = self.currentIndex()
        if kind == EV_CHANGE:
            self._sel._set(index)
        elif kind == EV_PRESS and index.isValid():
            self.pressed.emit(index)
            self.clicked.emit(index)
        elif kind == EV_DOUBLE and index.isValid():
            self.doubleClicked.emit(index)
            self.activated.emit(index)

    def isHeaderHidden(self):  # noqa: N802
        return self._headers_hidden

    def setHeaderHidden(self, hidden):  # noqa: N802
        self._headers_hidden = bool(hidden)
        self._refresh()


class QListView(_ItemView):
    _ALL_COLUMNS = False

    def modelColumn(self):  # noqa: N802
        return self._column

    def setModelColumn(self, column):  # noqa: N802
        self._column = int(column)
        self._refresh()


class QTreeView(_ItemView):
    _NESTED = True

    def _node_of(self, index):
        return self._nodes.get(index.siblingAtColumn(self._columns()[0])) if index.isValid() else None

    def isExpanded(self, index):  # noqa: N802
        node = self._node_of(index)
        return node is not None and bool(node.get("expanded"))

    def setExpanded(self, index, expanded):  # noqa: N802
        node = self._node_of(index)
        if node is not None:
            node.set("expanded", 1.0 if expanded else 0.0)
            self.invalidate()

    def expand(self, index):
        self.setExpanded(index, True)

    def collapse(self, index):
        self.setExpanded(index, False)

    def expandAll(self):  # noqa: N802
        for node in self._nodes.values():
            node.set("expanded", 1.0)
        self.invalidate()

    def collapseAll(self):  # noqa: N802
        for node in self._nodes.values():
            node.set("expanded", 0.0)
        self.invalidate()


class QTableView(_ItemView):
    def selectRow(self, row):  # noqa: N802
        if self._model is not None:
            self.setCurrentIndex(self._model.index(row, 0, self._root))
