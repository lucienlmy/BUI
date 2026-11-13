"""How much of Qt's widget API `bui.qtsyntax` has, measured against Qt's own headers.

Reads every exported class from reference/qtbase (src/widgets and src/gui/kernel), keeps what a
program can call or override: public methods, slots, signals, and the protected `*Event` handlers.
A name counts as present when the BUI class of the same name has it (inherited included).
Writes docs/qt-coverage.md and prints the totals.

    py tools/qt_coverage.py            # report
    py tools/qt_coverage.py --missing QLabel QWidget   # just those classes' missing names

SPDX-License-Identifier: GPL-2.0-or-later
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
QTBASE = ROOT / "reference" / "qtbase" / "src"
HEADER_DIRS = ["widgets/widgets", "widgets/kernel", "gui/kernel"]
sys.path.insert(0, str(ROOT / "bindings" / "python"))

# The classes the README's headline number is about: the widgets most programs use.
CORE = ["QWidget", "QFrame", "QLabel", "QAbstractButton", "QPushButton", "QCheckBox", "QRadioButton",
        "QToolButton", "QLineEdit", "QComboBox", "QAbstractSlider", "QSlider", "QAbstractSpinBox",
        "QSpinBox", "QDoubleSpinBox", "QProgressBar", "QGroupBox", "QTabWidget", "QMainWindow",
        "QMenu", "QMenuBar", "QStatusBar", "QScrollArea", "QTextEdit", "QPlainTextEdit",
        "QBoxLayout", "QGridLayout", "QFormLayout", "QSplitter", "QStackedWidget", "QToolBar",
        "QDockWidget", "QToolBox", "QDateTimeEdit", "QCalendarWidget", "QDialog"]

ACCESS = re.compile(r"^\s*(public|protected|private)(\s+Q_SLOTS|\s+slots)?\s*:|^\s*(Q_SIGNALS|signals)\s*:")
CLASS = re.compile(r"^class\s+Q_(?:WIDGETS|GUI)_EXPORT\s+(Q\w+)\b[^;]*$")
METHOD = re.compile(r"(?:^|[\s*&>:])(~?[a-zA-Z_]\w*)\s*\(")
SKIP = {"if", "for", "while", "switch", "return", "sizeof", "operator", "metaObject", "qt_metacast",
        "qt_metacall", "tr", "trUtf8", "d_func", "staticMetaObject", "Q_DECLARE_PRIVATE", "Q_DISABLE_COPY",
        "Q_OBJECT", "Q_PROPERTY", "Q_ENUM", "Q_FLAG", "Q_DECLARE_FLAGS", "Q_DECLARE_OPERATORS_FOR_FLAGS",
        "QT_DEPRECATED_VERSION_X_6_0", "Q_DECL_DEPRECATED", "Q_INVOKABLE", "Q_REVISION", "decltype",
        "noexcept", "Q_GADGET", "Q_PRIVATE_SLOT", "Q_DECLARE_METATYPE", "QT_DEPRECATED_X", "explicit"}


def qt_classes():
    """{class: set of names a program can use}, from the headers."""
    out = {}
    for d in HEADER_DIRS:
        for h in sorted((QTBASE / d).glob("q*.h")):
            if h.name.endswith("_p.h") or "platform" in h.name:
                continue
            cls, depth, access, names = None, 0, None, set()
            for line in h.read_text(encoding="utf-8", errors="ignore").splitlines():
                code = line.split("//")[0]
                if cls is None:
                    m = CLASS.match(code)
                    if m:
                        cls, depth, access, names = m.group(1), 0, "private", set()
                    else:
                        continue
                a = ACCESS.match(code)
                if a and depth == 1:
                    access = "signals" if a.group(3) else a.group(1) + ("slots" if a.group(2) else "")
                    continue
                if depth == 1 and access and not code.lstrip().startswith(("#", "Q_PROPERTY", "friend", "using", "typedef", "enum")):
                    for name in METHOD.findall(code):
                        if name in SKIP or not name[0].islower() or name.startswith(("~", "Q_", "QT_", "qt_")) or name == cls:
                            continue
                        public = access in ("public", "publicslots", "signals")
                        handler = access == "protected" and name.endswith("Event")
                        if public or handler:
                            names.add(name)
                        break  # one declaration per line: the first name is the method
                depth += code.count("{") - code.count("}")
                if depth <= 0 and "}" in code:
                    out.setdefault(cls, set()).update(names)
                    cls = None
    return out


def bui_names():
    import bui.qtsyntax as q
    return {name: set(dir(obj)) for name, obj in vars(q).items() if isinstance(obj, type) and name.startswith("Q")}


def main():
    qt = qt_classes()
    bui = bui_names()
    if "--missing" in sys.argv:
        for cls in sys.argv[sys.argv.index("--missing") + 1:]:
            print(cls + ":", " ".join(sorted(qt.get(cls, set()) - bui.get(cls, set()))))
        return
    rows, have_all, total_all, have_core, total_core = [], 0, 0, 0, 0
    for cls in sorted(qt):
        names = qt[cls]
        if not names:
            continue
        have = names & bui.get(cls, set())
        rows.append((cls, len(have), len(names), sorted(names - have), cls in bui))
        have_all += len(have)
        total_all += len(names)
        if cls in CORE:
            have_core += len(have)
            total_core += len(names)
    pct = lambda a, b: 100.0 * a / b if b else 0.0
    lines = ["# Qt syntax coverage", "",
             "Generated by `tools/qt_coverage.py` from Qt's headers in `reference/qtbase` (widgets and",
             "gui/kernel): public methods, slots, signals and protected `*Event` handlers per class, and",
             "whether the `bui.qtsyntax` class of the same name has them. Model/view and dialog classes have",
             "no headers there, so they aren't counted.", "",
             "**Core widget classes:** %d of %d names (%.0f%%). **All %d classes:** %d of %d (%.0f%%)." % (
                 have_core, total_core, pct(have_core, total_core), len(rows), have_all, total_all,
                 pct(have_all, total_all)), "",
             "| Class | Has | Of | % | Missing |", "| --- | ---: | ---: | ---: | --- |"]
    for cls, have, total, missing, present in rows:
        name = cls if present else cls + " (no class)"
        shown = ", ".join("`%s`" % m for m in missing[:40]) + (" …" if len(missing) > 40 else "")
        lines.append("| %s%s | %d | %d | %.0f | %s |" % (name, " ★" if cls in CORE else "", have, total,
                                                          pct(have, total), shown or "—"))
    lines += ["", "★ a core class (the headline number)."]
    (ROOT / "docs" / "qt-coverage.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print("core: %d/%d (%.0f%%), all: %d/%d (%.0f%%) in %d classes; wrote docs/qt-coverage.md" % (
        have_core, total_core, pct(have_core, total_core), have_all, total_all, pct(have_all, total_all), len(rows)))


if __name__ == "__main__":
    main()
