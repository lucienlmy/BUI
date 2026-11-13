"""Qt syntax beyond the basic widgets: containers (QStackedWidget, QToolBox, QSplitter, QToolBar,
QDockWidget, QTabBar, QFrame, QGroupBox, QScrollArea, QStatusBar, QMainWindow's docks and toolbars),
actions, menus, tool buttons and button groups, layouts, model/view, QDialogButtonBox / QMessageBox /
QInputDialog, input widget details (validators, spin boxes, sliders), QSettings, QKeySequence /
QShortcut, QClipboard and QObject / QWidget / QApplication behavior. Headless: widgets
lay out and render into a raster, no window opens.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import bui  # noqa: E402,F401  (locates buipy)
import buipy  # noqa: E402
from bui import qtsyntax as q  # noqa: E402
from bui.components.core import EV_CHANGE, EV_DOUBLE, EV_MOTION, EV_PRESS  # noqa: E402


def render(widget, w=400, h=300):
    ctx = buipy.KitContext(widget)
    ctx.render(w, h, buipy.Raster(w, h), True)
    return ctx


def spy(signal):
    got = []
    signal.connect(lambda *a: got.append(a))
    return got


def test_stacked_widget():
    s = q.QStackedWidget()
    pages = [q.QLabel("page %d" % i) for i in range(3)]
    changed = spy(s.currentChanged)
    for p in pages:
        s.addWidget(p)
    assert s.count() == 3 and s.currentIndex() == 0 and s.currentWidget() is pages[0], "the first page is current"
    assert [p.isVisible() for p in pages] == [True, False, False]
    s.setCurrentWidget(pages[2])
    assert s.currentIndex() == 2 and [p.isVisible() for p in pages] == [False, False, True]
    assert changed == [(0,), (2,)]
    s.insertWidget(0, q.QLabel("front"))
    assert s.currentWidget() is pages[2] and s.currentIndex() == 3, "inserting before keeps the current page"
    removed = spy(s.widgetRemoved)
    s.removeWidget(pages[2])
    assert removed == [(3,)] and s.currentWidget() is pages[1], "removing the current page shows its neighbour"
    assert s.indexOf(pages[2]) == -1
    render(s)


def test_toolbox_is_blender_panels():
    box = q.QToolBox()
    a, b, c = q.QLabel("a"), q.QLabel("b"), q.QLabel("c")
    box.addItem(a, "Alpha")
    box.addItem(b, q.QIcon(), "Beta")
    box.insertItem(1, c, "Gamma")
    assert [box.itemText(i) for i in range(3)] == ["Alpha", "Gamma", "Beta"]
    assert box.currentIndex() == 0 and box.indexOf(b) == 2
    panels = [box._panel(i) for i in range(3)]
    assert all(p.type == "panel" for p in panels), "each item is a Blender panel"
    changed = spy(box.currentChanged)
    box.setCurrentWidget(b)
    assert [bool(p.get("collapsed")) for p in panels] == [True, True, False], "the Qt API keeps one open"
    assert changed == [(2,)] and box.currentWidget() is b
    panels[0].set("collapsed", False)  # a click on a header opens that panel on its own, as in Blender
    assert box.currentIndex() == 0, "currentIndex reads the panels"
    box.setItemEnabled(1, False)
    box.setItemToolTip(1, "tip")
    assert not box.isItemEnabled(1) and box.itemToolTip(1) == "tip"
    box.removeItem(0)
    assert box.count() == 2 and box.widget(0) is c
    render(box)


def test_splitter_sizes_and_dragging():
    split = q.QSplitter(q.Qt.Horizontal)
    left, right = q.QWidget(), q.QWidget()
    split.addWidget(left)
    split.addWidget(right)
    split.setSizes([120, 200])
    ctx = buipy.KitContext(split)
    r = buipy.Raster(400, 100)
    ctx.render(400, 100, r, True)
    assert left.width() == 120, left.width()
    assert left.width() + right.width() + split.handleWidth() == 400, "the last pane takes the rest"
    moved = spy(split.splitterMoved)
    sash = split._sashes[0]
    sash._on_event(EV_PRESS, None, 1, 1)
    sash._on_event(EV_MOTION, None, 41, 1)  # drag the region edge 40 px right
    ctx.render(400, 100, r, True)
    assert left.width() == 160 and moved and moved[-1] == (160, 1)
    split.setOrientation(q.Qt.Vertical)
    ctx.render(400, 300, buipy.Raster(400, 300), True)
    assert left.height() == 160 and left.width() == 400, "vertical: the size is the height"


def test_toolbar_actions():
    bar = q.QToolBar("Tools")
    hits, triggered = [], spy(bar.actionTriggered)
    save = bar.addAction("Save", lambda *a: hits.append("save"))
    bar.addSeparator()
    grid = q.QAction("Grid", bar)
    grid.setCheckable(True)
    bar.addAction(grid)
    assert [a.text() for a in bar.actions()] == ["Save", "", "Grid"]
    bar.widgetForAction(save).click()
    bar.widgetForAction(grid).click()
    assert hits == ["save"] and grid.isChecked() and [t[0] for t in triggered] == [save, grid]
    bar.removeAction(save)
    assert len(bar.actions()) == 2 and bar.widgetForAction(save) is None
    bar.setToolButtonStyle(q.Qt.ToolButtonTextOnly)
    assert bar.toolButtonStyle() == q.Qt.ToolButtonTextOnly
    render(bar, 300, 40)


def test_main_window_docks_toolbars_status():
    win = q.QMainWindow()
    central = q.QWidget()
    win.setCentralWidget(central)
    dock = q.QDockWidget("Outliner")
    dock.setWidget(q.QLabel("tree"))
    win.addDockWidget(q.Qt.LeftDockWidgetArea, dock)
    bar = win.addToolBar("Main")
    bar.addAction("New")
    win.statusBar().showMessage("Ready")
    render(win, 500, 300)
    assert win.dockWidgetArea(dock) == q.Qt.LeftDockWidgetArea and win.toolBarArea(bar) == q.Qt.TopToolBarArea
    assert bar.y() < central.y(), "the toolbar sits above the central widget"
    assert dock.x() < central.x() and dock.x() + dock.width() <= central.x(), "the dock sits to its left"
    status = win.statusBar()
    assert status.y() >= central.y() + central.height(), "the status bar is at the bottom"
    assert dock.windowTitle() == "Outliner" and dock.type == "panel"
    toggle = dock.toggleViewAction()
    seen = spy(dock.visibilityChanged)
    toggle.setChecked(False)
    assert not dock.isVisible() and seen == [(False,)] and not toggle.isChecked()
    assert win.takeCentralWidget() is central and win.centralWidget() is None


def test_tab_bar():
    tabs = q.QTabBar()
    changed = spy(tabs.currentChanged)
    tabs.addTab("One")
    tabs.addTab("Two")
    tabs.insertTab(0, "Zero")
    assert [tabs.tabText(i) for i in range(tabs.count())] == ["Zero", "One", "Two"]
    tabs.setCurrentIndex(2)
    assert tabs.currentIndex() == 2 and changed[-1] == (2,)
    clicked = spy(tabs.tabBarClicked)
    tabs._event(EV_CHANGE, None, 1, 0)
    assert clicked == [(1,)]
    tabs.removeTab(0)
    assert tabs.count() == 2


def test_model_view():
    model = q.QStandardItemModel()
    model.setHorizontalHeaderLabels(["Name", "Type"])
    scene = q.QStandardItem("Scene")
    cube = q.QStandardItem("Cube")
    scene.appendRow([cube, q.QStandardItem("Mesh")])
    model.appendRow([scene, q.QStandardItem("Collection")])
    model.appendRow([q.QStandardItem("Camera"), q.QStandardItem("Camera")])
    assert model.rowCount() == 2 and model.columnCount() == 2
    idx = model.index(0, 0)
    assert idx.data() == "Scene" and model.rowCount(idx) == 1 and model.index(0, 1, idx).data() == "Mesh"
    assert cube.index().parent() == idx and model.itemFromIndex(model.index(0, 0, idx)) is cube

    tree = q.QTreeView()
    tree.setModel(model)
    render(tree)
    assert len(tree._nodes) == 3, "two rows and a child, nested"
    current = spy(tree.selectionModel().currentChanged)
    tree.setCurrentIndex(model.index(1, 1))
    assert tree.currentIndex() == model.index(1, 0) and current, "a row is current (its first column)"
    clicks = spy(tree.clicked)
    tree._on_event(EV_PRESS, None, 0, 0)
    assert clicks == [(model.index(1, 0),)]
    cube.setText("Suzanne")  # the model reports the change; the view redraws it
    assert tree._nodes[cube.index()].label == "Suzanne"
    assert tree.currentIndex() == model.index(1, 0), "the current row survives a redraw"

    table = q.QTableView()
    table.setModel(model)
    assert len(table._nodes) == 2, "a table shows the top rows only"
    table.selectRow(0)
    assert table.selectionModel().selectedRows() == [model.index(0, 0)]

    names = q.QStringListModel(["Cube", "Light"])
    lst = q.QListView()
    lst.setModel(names)
    assert [n.label for n in lst._nodes.values()] == ["Cube", "Light"]
    names.insertRows(1, 1)
    names.setData(names.index(1), "Camera")
    assert [n.label for n in lst._nodes.values()] == ["Cube", "Camera", "Light"]
    double = spy(lst.doubleClicked)
    lst.setCurrentIndex(names.index(2))
    lst._on_event(EV_DOUBLE, None, 0, 0)
    assert double == [(names.index(2),)]
    assert names.match(names.index(0), q.Qt.DisplayRole, "am", -1, q.Qt.MatchContains) == [names.index(1)]
    assert [i.text() for i in model.findItems("Cam", q.Qt.MatchContains)] == ["Camera"]


def test_message_box_and_input_dialog():
    box = q.QMessageBox(q.QMessageBox.Question, "Quit", "Save changes?",
                        q.QMessageBox.Save | q.QMessageBox.Discard | q.QMessageBox.Cancel)
    box.setInformativeText("The file has unsaved changes.")
    assert [b.text() for b in box.buttons()] == ["Save", "Cancel", "Discard"] or \
        sorted(b.text() for b in box.buttons()) == ["Cancel", "Discard", "Save"]
    assert box.icon() == q.QMessageBox.Question and box._icon_label.isVisible(), "Blender's question icon"
    done = spy(box.finished)
    box.button(q.QMessageBox.Discard).click()
    assert box.result() == q.QMessageBox.Discard and box.clickedButton() is box.button(q.QMessageBox.Discard)
    assert done == [(q.QMessageBox.Discard,)]
    assert box.standardButtons() == q.QMessageBox.Save | q.QMessageBox.Discard | q.QMessageBox.Cancel
    render(box, 300, 120)

    d = q.QInputDialog()
    d.setInputMode(q.QInputDialog.IntInput)
    d.setIntRange(0, 10)
    d.setIntValue(42)
    assert d.intValue() == 10, "clamped to the range"
    picked = spy(d.intValueSelected)
    d.accept()
    assert picked == [(10,)] and d.result() == 1
    d2 = q.QInputDialog()
    d2.setComboBoxItems(["Low", "High"])
    d2._items.setCurrentIndex(1)
    assert d2.textValue() == "High"


def test_settings_file():
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "app.ini")
        s = q.QSettings(path, q.QSettings.IniFormat)
        s.setValue("geometry/width", 640)
        s.beginGroup("view")
        s.setValue("grid", True)
        s.setValue("recent", ["a.blend", "b.blend"])
        s.endGroup()
        s.beginWriteArray("files")
        for i, name in enumerate(["x", "y"]):
            s.setArrayIndex(i)
            s.setValue("name", name)
        s.endArray()
        t = q.QSettings(path, q.QSettings.IniFormat)  # read back from the file
        assert t.value("geometry/width", type=int) == 640
        assert t.value("view/grid", type=bool) is True
        assert t.value("view/recent", type=list) == ["a.blend", "b.blend"]
        assert t.value("missing", "fallback") == "fallback" and not t.contains("missing")
        assert t.childGroups() == ["files", "geometry", "view"]
        n = t.beginReadArray("files")
        names = []
        for i in range(n):
            t.setArrayIndex(i)
            names.append(t.value("name"))
        t.endArray()
        assert names == ["x", "y"]
        t.beginGroup("view")
        assert t.childKeys() == ["grid", "recent"]
        t.remove("")
        t.endGroup()
        assert not q.QSettings(path, q.QSettings.IniFormat).contains("view/grid"), "remove persists"
        with open(path, encoding="utf-8") as f:
            text = f.read()
        assert "[geometry]" in text and "width=640" in text, "Qt's INI layout"


def test_keys_shortcuts_clipboard():
    assert q.QKeySequence("ctrl+shift+s").toString() == "Ctrl+Shift+S"
    assert q.QKeySequence(q.QKeySequence.Copy) == "Ctrl+C" and q.QKeySequence("Ctrl+C") == q.QKeySequence.Copy
    assert q.QKeySequence("").isEmpty()

    w = q.QWidget()
    w._ctx = buipy.KitContext(w)  # the window's context, as QWidget.show makes it
    fired = []
    q.QShortcut(q.QKeySequence("Ctrl+K"), w, lambda: fired.append(1))
    button = q.QPushButton("Run", w)
    clicks = spy(button.clicked)
    button.setShortcut("Ctrl+R")
    assert button.shortcut() == "Ctrl+R"
    from bui.qtcore import bind_shortcuts
    bind_shortcuts(w)
    ctrl = 0x2  # BK_CTRL
    w._ctx.key("", "k", ctrl)
    w._ctx.key("", "r", ctrl)
    assert fired == [1] and len(clicks) == 1, "the kit's key binding ran both"

    cb = q.QApplication.clipboard()
    cb.setText("hello")
    assert cb.text() == "hello" and q.QApplication.clipboard() is cb


def test_object_and_widget_behavior():
    w = q.QWidget()
    w.setObjectName("root")
    lay = q.QVBoxLayout(w)
    a, b = q.QPushButton("A"), q.QLineEdit()
    b.setObjectName("field")
    lay.addWidget(a)
    lay.addWidget(b)
    render(w)
    assert w.findChild(q.QLineEdit) is b and w.findChild(name="field") is b
    assert w.findChildren(q.QPushButton) == [a]
    assert w.isAncestorOf(b) and not b.isAncestorOf(w)
    p = b.mapTo(w, q.QPoint(1, 2))
    assert (p.x(), p.y()) == (b.x() + 1, b.y() + 2) or (p.x(), p.y()) == (
        b.absolutePos()[0] - w.absolutePos()[0] + 1, b.absolutePos()[1] - w.absolutePos()[1] + 2)
    assert w.childAt(b.mapTo(w, q.QPoint(2, 2))) is b

    hits = spy(a.clicked)
    old = a.blockSignals(True)
    a.click()
    assert hits == [] and old is False and a.signalsBlocked()
    a.blockSignals(False)
    a.click()
    assert len(hits) == 1, "blocked signals don't reach their slots"

    w.setProperty("mode", "edit")
    assert w.property("mode") == "edit" and w.dynamicPropertyNames() == [b"mode"]
    assert b.property("text") == "", "a Qt property reads through its getter"
    assert w.inherits("QWidget") and not w.inherits("QLabel")
    w.setStatusTip("hint")
    w.setToolTip("tip")
    assert w.statusTip() == "hint" and w.toolTip() == "tip"
    w.setFocusPolicy(q.Qt.StrongFocus)
    assert w.focusPolicy() == q.Qt.StrongFocus, "the setter isn't a no-op any more"
    q.QApplication.setApplicationName("Demo")
    assert q.QApplication.applicationName() == "Demo"
    shown = []

    class Probe(q.QWidget):
        def showEvent(self, e):  # noqa: N802
            shown.append("show")

        def hideEvent(self, e):  # noqa: N802
            shown.append("hide")

    child = Probe(w)
    child.setVisible(False)
    child.setVisible(True)
    assert shown == ["hide", "show"]
    act = q.QAction("Copy", w)
    w.addAction(act)
    assert w.actions() == [act]
    w.removeAction(act)
    assert w.actions() == []


def type_into(edit, text):
    edit.set("set_value", text)
    edit._on_event(EV_CHANGE, "", 0, 0)


def test_line_edit_limits_and_validators():
    from bui.qtgui import QIntValidator, QRegularExpressionValidator
    e = q.QLineEdit("hello")
    changed, rejected = spy(e.textChanged), spy(e.inputRejected)
    assert not e.isModified()
    e.setMaxLength(3)
    assert e.text() == "hel" and e.maxLength() == 3
    type_into(e, "abcdef")
    assert e.text() == "abc" and rejected and changed == [("hel",), ("abc",)] and e.isModified()
    e.setMaxLength(100)
    e.setText("12")
    assert not e.isModified(), "setText clears the modified flag"
    e.setValidator(QIntValidator(0, 99))
    type_into(e, "1x")
    assert e.text() == "12", "an Invalid edit is refused"
    type_into(e, "45")
    assert e.text() == "45" and e.hasAcceptableInput()
    e.setValidator(QRegularExpressionValidator("[a-z]+@x"))
    type_into(e, "bob")
    assert e.text() == "bob" and not e.hasAcceptableInput(), "Intermediate is kept but not acceptable"
    e.setPlaceholderText("name")
    assert e.placeholderText() == "name"


def test_spin_slider_progress():
    s = q.QSpinBox()
    s.setRange(0, 10)
    s.setPrefix("$")
    s.setSuffix(" kg")
    s.setValue(3)
    assert s.text() == "$3 kg" and s.cleanText() == "3" and s.prefix() == "$"
    s.setWrapping(True)
    s.stepBy(9)
    assert s.value() == 1, "wrapping steps round from the top"
    s.setSpecialValueText("none")
    s.setValue(0)
    assert s.text() == "none"
    d = q.QDoubleSpinBox()
    d.setDecimals(2)
    d.setValue(1.5)
    assert d.textFromValue(1.5) == "1.50" and d.decimals() == 2
    sl = q.QSlider(q.Qt.Horizontal)
    sl.setRange(0, 100)
    sl.setPageStep(25)
    acts = spy(sl.actionTriggered)
    sl.triggerAction(sl.SliderPageStepAdd)
    sl.triggerAction(sl.SliderToMaximum)
    assert sl.value() == 100 and acts == [(3,), (6,)]
    p = q.QProgressBar()
    p.setRange(0, 200)
    p.setValue(50)
    assert p.text() == "25%"
    p.setFormat("%v of %m")
    assert p.text() == "50 of 200"
    p.resetFormat()
    assert p.format() == "%p%"


def test_combo_tabs_label_button():
    c = q.QComboBox()
    c.addItem("a", 1)
    c.addItem("b", 2)
    assert c.findData(2) == 1
    c.setItemText(0, "A")
    assert c.itemText(0) == "A"
    c.setMaxCount(1)
    assert c.count() == 1
    c.addItem("c")
    assert c.count() == 1, "maxCount stops adding"
    m = q.QStringListModel(["x", "y", "z"])
    c.setMaxCount(10)
    c.setModel(m)
    assert c.count() == 3 and c.itemText(2) == "z" and c.model() is m
    t = q.QTabWidget()
    p0, p1 = q.QLabel("0"), q.QLabel("1")
    t.addTab(p0, "zero")
    t.insertTab(0, p1, "one")
    assert t.indexOf(p0) == 1 and t.tabText(0) == "one"
    t.setCurrentWidget(p0)
    assert t.currentWidget() is p0
    t.removeTab(0)
    assert t.count() == 1 and t.indexOf(p1) == -1
    lab = q.QLabel()
    lab.setNum(2.5)
    assert lab.text() == "2.5"
    lab.setNum(7)
    assert lab.text() == "7"
    b = q.QPushButton("b")
    toggled = spy(b.toggled)
    b.setChecked(True)
    assert not b.isChecked(), "not checkable yet"
    b.setCheckable(True)
    b.toggle()
    assert b.isChecked() and toggled == [(True,)]
    b.setFlat(True)
    b.setDefault(True)
    assert b.isFlat() and b.isDefault()
    menu = q.QMenu("m")
    b.setMenu(menu)
    assert b.menu() is menu
    render(b)


def test_window_hints_timers_palette():
    from bui.qtgui import QFontMetrics, QPalette
    w = q.QWidget()
    w.setWindowOpacity(2)
    w.setMinimumSize(100, 50)
    w.setMaximumWidth(400)
    w.setWindowFlags(q.Qt.WindowStaysOnTopHint)
    assert w.windowOpacity() == 1.0 and w.minimumWidth() == 100 and w.maximumSize().width() == 400
    assert w.windowFlags() & q.Qt.WindowStaysOnTopHint
    w.setWindowState(q.Qt.WindowMaximized)
    assert w.isMaximized(), "kept until the window exists"
    w.setWindowTitle("doc[*]")
    w.setWindowModified(True)
    assert w._shown_title() == "doc*" and w.windowTitle() == "doc[*]"
    pal = QPalette()
    pal.setColor(QPalette.Window, q.QColor(255, 0, 0))
    w.setPalette(pal)
    assert w.palette().color(QPalette.Window).red() == 255
    fm = QFontMetrics(w.font())
    assert fm.horizontalAdvance("wide text") > fm.horizontalAdvance("w") > 0
    assert fm.elidedText("a long piece of text", q.Qt.ElideRight, fm.horizontalAdvance("a long")).endswith("…")
    fired = []

    class T(q.QWidget):
        def timerEvent(self, e):  # noqa: N802
            fired.append(e.timerId())
            self.killTimer(e.timerId())

    t = T()
    tid = t.startTimer(0)
    assert tid == 1
    queue, q._timers.queue = q._timers.queue, []  # headless: no window clock, run the queued tick
    for _, fn in queue:
        fn()
    assert fired == [1] and not q._timers.queue, "fired once, then killed"


def test_context_menu_resize_keyrelease():
    from bui.components.core import EV_CONFIGURE
    got = []

    class W(q.QWidget):
        def contextMenuEvent(self, e):  # noqa: N802
            got.append(("menu", e.x(), e.y()))

        def resizeEvent(self, e):  # noqa: N802
            got.append(("size", e.size().width(), e.oldSize().width()))

        def keyReleaseEvent(self, e):  # noqa: N802
            got.append(("key", e.text()))

    w = W()
    w._on_event(EV_PRESS, "0 3", 5, 6)
    w._on_event(EV_CONFIGURE, "", 120, 80)
    w._on_event(q.EV_KEYRELEASE, "a\ta", 0, 0)
    assert got == [("menu", 5, 6), ("size", 120, -1), ("key", "a")]
    v = q.QWidget()
    v.setContextMenuPolicy(q.Qt.CustomContextMenu)
    asked = spy(v.customContextMenuRequested)
    v._on_event(EV_PRESS, "0 3", 1, 2)
    assert len(asked) == 1 and asked[0][0].x() == 1


def test_actions_and_groups():
    a = q.QAction("&Save As...")
    assert a.iconText() == "Save As" and a.toolTip() == "Save As", "Qt's defaults drop & and ..."
    changed = spy(a.changed)
    a.setStatusTip("Save under a new name")
    a.setData({"id": 7})
    assert a.statusTip() == "Save under a new name" and a.data() == {"id": 7} and changed
    a.setShortcuts(["Ctrl+Shift+S", "F12"])
    assert a.shortcut() == "Ctrl+Shift+S" and len(a.shortcuts()) == 2

    group = q.QActionGroup(None)
    x, y = group.addAction("X"), group.addAction("Y")
    for act in (x, y):
        act.setCheckable(True)
    fired = spy(group.triggered)
    x.trigger()
    y.trigger()
    assert y.isChecked() and not x.isChecked() and group.checkedAction() is y
    y.trigger()
    assert y.isChecked(), "an exclusive group keeps its checked action checked"
    assert fired == [(x,), (y,), (y,)]
    group.setExclusionPolicy(q.QActionGroup.ExclusionPolicy.ExclusiveOptional)
    y.trigger()
    assert not y.isChecked(), "exclusive-optional lets it go"
    group.setEnabled(False)
    assert not x.isEnabled()
    x.trigger()
    assert not x.isChecked(), "a disabled action does nothing"


def test_menus_and_menubar():
    win = q.QMainWindow()
    win._ctx = buipy.KitContext(win)
    bar = win.menuBar()
    file_menu = bar.addMenu("&File")
    opened = file_menu.addAction("Open")
    file_menu.addSeparator()
    file_menu.addSeparator()
    recent = q.QMenu("Recent")
    action = file_menu.addMenu(recent)
    assert action is recent.menuAction() and action.menu() is recent
    doc = recent.addAction("doc.txt")
    hidden = file_menu.addAction("Hidden")
    hidden.setVisible(False)
    items = file_menu._items()
    assert [getattr(i, "text", i) for i in items] == ["Open", "-", "Recent"], "doubled separators collapse"
    assert items[2].submenu[0].text == "doc.txt"
    got = spy(file_menu.triggered), spy(bar.triggered)
    doc.trigger()
    assert got[0] == [(doc,)] and got[1] == [(doc,)], "triggered goes up to the menu and the bar"
    file_menu.insertSection(opened, "Files")
    assert [getattr(i, "text", i) for i in file_menu._items()][:2] == ["Files", "Open"]
    assert not file_menu.isEmpty() and q.QMenu().isEmpty()
    help_action = bar.addAction("Help")
    assert isinstance(bar._buttons[help_action], q.QToolButton), "a plain action is a flat button"
    saved = []
    save = q.QAction("Save", win)
    save.triggered.connect(lambda *_: saved.append(1))
    file_menu.addAction(save)
    save.setShortcut("Ctrl+S")
    from bui.qtcore import bind_shortcuts
    bind_shortcuts(win)
    win._ctx.key("", "s", 0x2)
    assert saved == [1], "the action's shortcut is bound on its window"
    corner = q.QLabel("v1")
    bar.setCornerWidget(corner)
    assert bar.cornerWidget() is corner
    bar.clear()
    assert bar.actions() == []
    render(win)


def test_tool_buttons_and_button_groups():
    act = q.QAction("Bold")
    act.setCheckable(True)
    tb = q.QToolButton()
    tb.setDefaultAction(act)
    tb.setAutoRaise(True)
    triggered = spy(tb.triggered)
    tb.click()
    assert act.isChecked() and tb.isChecked() and triggered == [(act,)]
    act.setText("Strong")
    assert tb.text() == "Strong", "the button follows its action"
    act.setEnabled(False)
    assert not tb.isEnabled()
    tb.setArrowType(q.Qt.DownArrow)
    assert tb.arrowType() == q.Qt.DownArrow and tb.autoRaise()
    render(tb)

    parent = q.QWidget()
    group = q.QButtonGroup()
    buttons = [q.QPushButton(t, parent) for t in "abc"]
    for i, b in enumerate(buttons):
        b.setCheckable(True)
        group.addButton(b, i + 10)
    clicked = spy(group.idClicked)
    buttons[0].click()
    buttons[2].click()
    assert group.checkedId() == 12 and not buttons[0].isChecked() and clicked == [(10,), (12,)]
    assert group.button(11) is buttons[1] and group.id(buttons[2]) == 12
    auto = q.QButtonGroup()
    b1, b2 = q.QRadioButton("1"), q.QRadioButton("2")
    auto.addButton(b1)
    auto.addButton(b2)
    assert (auto.id(b1), auto.id(b2)) == (-2, -3), "ids count down from -2"

    row = q.QWidget()
    p, r = q.QPushButton("p", row), q.QPushButton("r", row)
    for b in (p, r):
        b.setCheckable(True)
        b.setAutoExclusive(True)
    p.setChecked(True)
    r.setChecked(True)
    assert not p.isChecked(), "auto-exclusive siblings"
    p.setDown(True)
    assert p.isDown()


def test_layout_api():
    w = q.QWidget()
    box = q.QVBoxLayout(w)
    a, b, c = q.QLabel("a"), q.QLabel("b"), q.QLabel("c")
    box.addWidget(a)
    box.addWidget(c, 2)
    box.insertWidget(1, b)
    box.insertSpacing(0, 10)
    box.addSpacerItem(q.QSpacerItem(0, 0, q.QSizePolicy.Minimum, q.QSizePolicy.Expanding))
    assert [box.indexOf(x) for x in (a, b, c)] == [1, 2, 3] and box.stretch(3) == 2
    assert box.setStretchFactor(b, 4) and box.stretch(2) == 4
    item = box.takeAt(2)
    assert item.widget() is b and box.indexOf(b) == -1 and not box.isEmpty()
    box.replaceWidget(a, b)
    assert box.indexOf(b) == 1 and box.indexOf(a) == -1
    box.setDirection(q.QBoxLayout.BottomToTop)
    assert box.direction() == q.QBoxLayout.BottomToTop
    box.setContentsMargins(4, 5, 6, 7)
    assert box.getContentsMargins() == (4, 5, 6, 7)
    render(w)

    g = q.QWidget()
    grid = q.QGridLayout(g)
    cells = {(r, col): q.QLabel("%d%d" % (r, col)) for r in range(2) for col in range(2)}
    for (r, col), lab in cells.items():
        grid.addWidget(lab, r, col)
    wide = q.QLabel("wide")
    grid.addWidget(wide, 2, 0, 1, 2)
    assert grid.itemAtPosition(2, 1).widget() is wide and grid.getItemPosition(grid.indexOf(wide)) == (2, 0, 1, 2)
    grid.setColumnStretch(1, 3)
    grid.setRowMinimumHeight(0, 40)
    assert grid.columnStretch(1) == 3 and grid.rowMinimumHeight(0) == 40 and grid.rowCount() == 3
    render(g)

    f = q.QWidget()
    form = q.QFormLayout(f)
    name, age = q.QLineEdit(), q.QSpinBox()
    form.addRow("Name", name)
    form.addRow("Age", age)
    note = q.QLabel("note")
    form.insertRow(1, note)
    assert form.rowCount() == 3 and form.getWidgetPosition(age) == (2, q.QFormLayout.FieldRole)
    assert form.labelForField(name).text() == "Name"
    form.setRowVisible(2, False)
    assert not form.isRowVisible(2)
    form.removeRow(1)
    assert form.rowCount() == 2 and form.getWidgetPosition(age)[0] == 1
    render(f)

    s = q.QWidget()
    stack = q.QStackedLayout(s)
    changed = spy(stack.currentChanged)
    p0, p1 = q.QLabel("0"), q.QLabel("1")
    stack.addWidget(p0)
    stack.addWidget(p1)
    stack.setCurrentWidget(p1)
    assert stack.currentWidget() is p1 and changed == [(0,), (1,)]
    stack.setStackingMode(q.QStackedLayout.StackAll)
    assert p0.isVisible()


def test_frames_groups_scroll_status():
    frame = q.QFrame()
    frame.setFrameStyle(q.QFrame.StyledPanel | q.QFrame.Sunken)
    lay = q.QVBoxLayout(frame)
    lay.addWidget(q.QLabel("inside"))
    assert frame.frameShape() == q.QFrame.StyledPanel and frame.frameShadow() == q.QFrame.Sunken
    assert lay.parent.type == "box", "a styled panel is a Blender box around the contents"
    line = q.QFrame()
    line.setFrameShape(q.QFrame.VLine)
    assert line._inner.type == "separator" and line._inner.get("vertical")
    render(frame)

    gb = q.QGroupBox("Options")
    inner = q.QVBoxLayout(gb)
    opt = q.QCheckBox("x")
    off = q.QPushButton("off")
    off.setEnabled(False)
    inner.addWidget(opt)
    inner.addWidget(off)
    toggled = spy(gb.toggled)
    gb.setCheckable(True)
    assert gb.isChecked() and gb.title() == "Options"
    gb.setChecked(False)
    assert not opt.isEnabled() and toggled == [(False,)]
    gb.setChecked(True)
    assert opt.isEnabled() and not off.isEnabled(), "each widget gets its own state back"
    render(gb)

    area = q.QScrollArea()
    content = q.QLabel("long")
    area.setWidget(content)
    assert area.widget() is content and area.takeWidget() is content and area.widget() is None
    area.setVerticalScrollBarPolicy(q.Qt.ScrollBarAlwaysOff)
    assert area.verticalScrollBarPolicy() == q.Qt.ScrollBarAlwaysOff

    bar = q.QStatusBar()
    normal, perm = q.QLabel("n"), q.QLabel("p")
    bar.addWidget(normal)
    bar.addPermanentWidget(perm)
    msgs = spy(bar.messageChanged)
    bar.showMessage("Saved")
    assert bar.currentMessage() == "Saved" and not normal.isVisible() and perm.isVisible()
    bar.clearMessage()
    assert normal.isVisible() and msgs == [("Saved",), ("",)]
    render(bar)

def test_spin_boxes_sliders_and_button_box():
    s = q.QSpinBox()
    assert isinstance(s, q.QAbstractSpinBox) and not isinstance(s, q.QAbstractSlider)
    s.setRange(0, 5)
    s.setValue(5)
    assert s.stepEnabled() == s.StepDownEnabled
    s.setWrapping(True)
    assert s.stepEnabled() == s.StepUpEnabled | s.StepDownEnabled
    s.setSuffix(" px")
    from bui.qtgui import QValidator
    assert s.validate("3 px")[0] == QValidator.Acceptable and s.validate("9")[0] == QValidator.Intermediate
    assert s.validate("x")[0] == QValidator.Invalid and s.hasAcceptableInput()
    s.setFrame(False)
    assert not s.hasFrame()
    d = q.QDoubleSpinBox()
    d.setRange(0.5, 2.5)
    assert d.minimum() == 0.5 and isinstance(d.maximum(), float)

    sl = q.QSlider(q.Qt.Vertical)
    assert sl.orientation() == q.Qt.Vertical
    sl.setTracking(False)
    values, moved = spy(sl.valueChanged), spy(sl.sliderMoved)
    sl._on_event(EV_PRESS, "0 1", 0, 0)
    sl.set("set_value", 10.0)  # the kit's drag changes the value, then reports it
    sl._event(EV_CHANGE, None, 0, 0)
    assert sl.isSliderDown() and moved == [(10,)] and values == [], "no valueChanged while held"
    from bui.components.core import EV_RELEASE
    sl._on_event(EV_RELEASE, "0 1", 0, 0)
    assert not sl.isSliderDown() and values == [(10,)], "released: the value is final"

    box = q.QDialogButtonBox(q.QDialogButtonBox.Ok | q.QDialogButtonBox.Cancel | q.QDialogButtonBox.Help)
    got = spy(box.accepted), spy(box.rejected), spy(box.helpRequested)
    box.button(box.Ok).click()
    box.button(box.Cancel).click()
    box.button(box.Help).click()
    assert [len(g) for g in got] == [1, 1, 1]
    apply = box.addButton("Apply now", box.ApplyRole)
    assert box.buttonRole(apply) == box.ApplyRole and box.standardButton(apply) == box.NoButton
    assert box.standardButtons() == box.Ok | box.Cancel | box.Help
    box.removeButton(apply)
    assert apply not in box.buttons()
    render(box)


def test_combo_icons_docks_application():
    c = q.QComboBox()
    c.addItem(q.QIcon.fromTheme("FILE") if hasattr(q.QIcon, "fromTheme") else q.QIcon(), "File", 1)
    c.addItem("Plain")
    c.setItemIcon(1, q.QIcon())
    assert c.count() == 2 and c.itemData(0) == 1 and c.itemText(0) == "File"
    picked = spy(c.activated)
    c._pick(1)
    assert c.currentIndex() == 1 and picked == [(1,)]

    win = q.QMainWindow()
    dock = q.QDockWidget("Tools")
    dock.setAllowedAreas(q.Qt.LeftDockWidgetArea | q.Qt.RightDockWidgetArea)
    assert dock.isAreaAllowed(q.Qt.LeftDockWidgetArea) and not dock.isAreaAllowed(q.Qt.TopDockWidgetArea)
    where = spy(dock.dockLocationChanged)
    win.addDockWidget(q.Qt.RightDockWidgetArea, dock)
    assert where == [(q.Qt.RightDockWidgetArea,)]

    font = q.QFont("Sans", 14)
    q.QApplication.setFont(font)
    try:
        assert q.QWidget().font() is font, "widgets default to the application's font"
    finally:
        q.QApplication.setFont(None)
    assert win in q.QApplication.topLevelWidgets() and dock not in q.QApplication.topLevelWidgets()

def test_setters_emit_and_regressions():
    """Qt emits the change signals when code changes a value (once, and only on a change)."""
    s = q.QSlider()
    got = spy(s.valueChanged)
    s.setValue(5)
    s.setValue(5)
    assert got == [(5,)]
    sp = q.QSpinBox()
    texts = spy(sp.textChanged)
    sp.setValue(3)
    assert texts == [("3",)]
    c = q.QComboBox()
    first = spy(c.currentIndexChanged)
    c.addItems(["a", "b"])
    c.setCurrentIndex(1)
    assert first == [(0,), (1,)], "the first item becomes current, then the one set"
    e = q.QLineEdit()
    edited, changed = spy(e.textEdited), spy(e.textChanged)
    e.setText("x")
    assert changed == [("x",)] and edited == []
    cb = q.QCheckBox()
    states = spy(cb.stateChanged)
    cb.setChecked(True)
    assert states == [(q.Qt.Checked,)]
    parent = q.QWidget()
    r1, r2 = q.QRadioButton("1", parent), q.QRadioButton("2", parent)
    off = spy(r1.toggled)
    r1.setChecked(True)
    r2.setChecked(True)
    assert off == [(True,), (False,)], "a sibling turned off says so"

    # a checked button in an exclusive group stays checked when clicked again
    group = q.QButtonGroup()
    a, b = q.QPushButton("a"), q.QPushButton("b")
    for x in (a, b):
        x.setCheckable(True)
        group.addButton(x)
    a.click()
    a.click()
    assert a.isChecked()
    # removing and adding again doesn't relay twice
    clicks = spy(group.buttonClicked)
    group.removeButton(b)
    group.addButton(b)
    b.click()
    assert clicks == [(b,)]
    menu = q.QMenu()
    act = menu.addAction("x")
    menu.removeAction(act)
    menu.addAction(act)
    fired = spy(menu.triggered)
    act.trigger()
    assert fired == [(act,)]

    # form rows keep the field growth policy
    w = q.QWidget()
    form = q.QFormLayout(w)
    form.setFieldGrowthPolicy(q.QFormLayout.FieldsStayAtSizeHint)
    form.addRow("Name", q.QLineEdit())
    assert form.columnStretch(1) == 0
    # a frame's layout can be replaced while it sits in the frame's box
    frame = q.QFrame()
    frame.setFrameShape(q.QFrame.Box)
    q.QVBoxLayout(frame)
    frame.setLayout(q.QHBoxLayout())
    render(frame)

def test_dates_and_times():
    d = q.QDate(2024, 1, 31)
    assert d.addMonths(1) == q.QDate(2024, 2, 29), "past the month's end: its last day (Qt)"
    assert d.toString("dddd d MMMM yyyy") == "Wednesday 31 January 2024" and d.toString(q.Qt.ISODate) == "2024-01-31"
    assert q.QDate.fromString("29 Feb 2024", "d MMM yyyy") == q.QDate(2024, 2, 29)
    assert not q.QDate(2023, 2, 29).isValid() and q.QDate.isLeapYear(2024)
    t = q.QTime(13, 5, 9)
    assert t.toString("h:mm AP") == "1:05 PM" and q.QTime.fromString("1:05 pm", "h:mm ap") == q.QTime(13, 5)
    assert t.addSecs(11 * 3600) == q.QTime(0, 5, 9), "wraps at midnight"
    dt = q.QDateTime(d, t)
    assert q.QDateTime.fromString(dt.toString(q.Qt.ISODate), q.Qt.ISODate) == dt
    assert dt.addDays(1).date() == q.QDate(2024, 2, 1) and dt.secsTo(dt.addSecs(90)) == 90

    e = q.QDateTimeEdit(dt)
    assert e.text() == "2024-01-31 13:05" and e.sectionCount() == 5
    dates = spy(e.dateChanged)
    e.setCurrentSection(e.MonthSection)
    e.stepBy(1)
    assert e.date() == q.QDate(2024, 2, 29) and dates == [(q.QDate(2024, 2, 29),)]
    e._fields[0][2].setValue(2025)  # the user changes the year field
    assert e.date() == q.QDate(2025, 2, 28)
    e.setDateRange(q.QDate(2025, 1, 1), q.QDate(2025, 1, 31))
    assert e.date() == q.QDate(2025, 1, 31), "clamped into the range"
    de = q.QDateEdit(q.QDate(2020, 5, 17))
    de.setDisplayFormat("d MMM yyyy")
    de._fields[1][2].setCurrentIndex(1)  # the month dropdown
    assert de.date() == q.QDate(2020, 2, 17) and de.text() == "17 Feb 2020"
    te = q.QTimeEdit(q.QTime(15, 30))
    te.setDisplayFormat("h:mm AP")
    assert te.text() == "3:30 PM" and te._fields[0][2].value() == 3
    te._fields[2][2].setCurrentIndex(0)  # AM
    assert te.time() == q.QTime(3, 30)
    render(e)

    c = q.QCalendarWidget()
    c.setSelectedDate(q.QDate(2024, 2, 10))
    assert (c.yearShown(), c.monthShown()) == (2024, 2) and c._days[0][1] == q.QDate(2024, 1, 29)
    picked = spy(c.clicked)
    c._pick(20)
    assert c.selectedDate() == q.QDate(2024, 2, 18) and picked == [(q.QDate(2024, 2, 18),)]
    pages = spy(c.currentPageChanged)
    c.showNextMonth()
    assert pages == [(2024, 3)]
    c.setFirstDayOfWeek(q.Qt.Sunday)
    assert c._heads[0].text() == "Sun"
    c.setDateRange(q.QDate(2024, 3, 5), q.QDate(2024, 3, 25))
    assert c.selectedDate() == q.QDate(2024, 3, 5), "the selection moves into the range"
    render(c, 360, 300)

def test_review_fixes():
    m = q.QStandardItemModel(3, 3)
    m.setRowCount(-1)
    assert m.rowCount() == 0, "a negative count is clamped to 0, and does not hang"
    m.setRowCount(2)
    m.setColumnCount(2)
    assert m.columnCount() == 2 and all(len(r) == 2 for r in m.invisibleRootItem()._rows), "extra cells dropped"

    s = q.QSpinBox()
    s.setRange(0, 100)
    s.setValue(50)
    got = spy(s.valueChanged)
    s.setRange(0, 10)
    assert s.value() == 10 and got == [(10,)], "the value is clamped into the new range, with a signal"

    w = q.QWidget()
    a = w.startTimer(1000)
    w.killTimer(a)
    assert w.startTimer(1000) != a, "timer ids are never reused"

    g = q.QGridLayout()
    g.addWidget(q.QLabel("x"), 1, 2, q.Qt.AlignRight)
    assert g._items[0][1][:4] == (1, 2, 1, 1) and g._items[0][1][4] == q.Qt.AlignRight, "4th arg is the alignment"
    g.addWidget(q.QLabel("y"), 0, 0, 2, 3)
    assert g._items[1][1][2:4] == (2, 3)

    e = q.QLineEdit()
    e.setEchoMode(q.QLineEdit.Password)
    assert e.echoMode() == q.QLineEdit.Password

    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "a.ini")
        st = q.QSettings(path, q.QSettings.IniFormat)
        st.setValue("k", 1)
        st.sync()
        assert os.listdir(d) == ["a.ini"], "no temp file left behind"
        assert str(q.QSettings(path, q.QSettings.IniFormat).value("k")) == "1"

    sc = q.QShortcut(q.QKeySequence("Ctrl+K"), q.QWidget(), lambda: None)
    sc.setKey(q.QKeySequence("Ctrl+J"))
    assert sc.key() == q.QKeySequence("Ctrl+J")


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
    print("ok")
