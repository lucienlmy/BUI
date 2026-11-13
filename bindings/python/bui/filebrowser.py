"""Blender's file browser as a blocking file dialog.

The kit's ``filebrowser`` node (kit/intern/kit_filebrowser.c, a port of
Blender's editors/space_file) shown in its own window, the way Blender opens
a file browser for an operator. ``bui.tksyntax.filedialog`` and
``bui.qtsyntax.QFileDialog`` use it; set ``BUI_FILEDIALOG=native`` to get the
operating system's dialogs (through the stdlib Tk) instead.

    from bui import filebrowser
    path = filebrowser.ask("open", title="Open Image",
                           filetypes=[("Images", "*.png *.jpg"), ("All files", "*")])

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

import os
import time

import buipy

from bui import host
from bui.components.views import FileBrowser

__all__ = ["ask", "enabled", "tk_filters", "qt_filters"]

_TITLES = {"open": "Open", "save": "Save As", "directory": "Select Folder"}
_POLL_SECONDS = 0.004


def enabled() -> bool:
    """False when ``BUI_FILEDIALOG`` asks for the system dialogs."""
    return os.environ.get("BUI_FILEDIALOG", "").lower() not in ("native", "system", "tk")


def _tk_entry(item) -> str:
    """One filetypes item: "Label", ("Label", "*.a *.b") or ("Label", ("*.a", "*.b"))."""
    if isinstance(item, str):
        return "%s\t%s" % (item, item)
    pats = item[1] if len(item) > 1 else "*"
    return "%s\t%s" % (item[0], pats if isinstance(pats, str) else " ".join(map(str, pats)))


def tk_filters(filetypes) -> list:
    """tkinter ``filetypes`` -> the node's "Label\\tpattern ..." entries.

    Accepts ``[("Python", "*.py"), ("Images", ("*.png", "*.jpg")), ("Text", ".txt")]``.
    """
    return [_tk_entry(item) for item in filetypes or ()]


def _qt_entry(part: str) -> str:
    label, _, rest = part.partition("(")
    pats = rest[:-1] if rest.endswith(")") else part
    return "%s\t%s" % (label.strip() or pats, pats)


def qt_filters(filter_str) -> list:
    """A Qt name filter ("Images (*.png *.jpg);;All (*)") -> node filter entries."""
    return [_qt_entry(p.strip()) for p in (filter_str or "").split(";;") if p.strip()]


def _browser(kind, initialdir, initialfile, entries, multiple):
    """A configured file browser component."""
    return FileBrowser(initialdir or "", file=initialfile or "",
                       mode=kind if kind in FileBrowser.MODES else "open",
                       multiple=multiple, filters=entries)


def _run(fb, title):
    """Show the node in a window until it is answered or closed; the chosen paths."""
    win = host.Window(buipy.KitContext(fb), title,
                      (int(fb.get("dialog_width")), int(fb.get("dialog_height"))))
    win.show()
    try:
        while fb.get("state") == 0 and win.is_open:
            host.update()
            time.sleep(_POLL_SECONDS)
    finally:
        if win.is_open:
            win.close()
    return list(fb.strs("result")) if fb.get("state") > 0 else []


def _with_extension(path, defaultextension):
    """Tk's defaultextension: added when the typed name has none."""
    if not defaultextension or os.path.splitext(path)[1]:
        return path
    return path + ("" if defaultextension.startswith(".") else ".") + defaultextension


def ask(kind: str = "open", title: str = None, initialdir: str = None,
        initialfile: str = None, filetypes=None, multiple: bool = False,
        defaultextension: str = None, filters: list = None):
    """Show the browser and wait for an answer.

    ``kind`` is "open", "save" or "directory". Returns the chosen path (a list
    with ``multiple``), or None when cancelled. ``filters`` takes node filter
    entries directly ("Label\\tpattern ..."), ``filetypes`` tkinter's form.
    """
    fb = _browser(kind, initialdir, initialfile, list(filters or ()) + tk_filters(filetypes),
                  multiple)
    result = _run(fb, title or _TITLES.get(kind, "Open"))
    if not result:
        return None
    if kind == "save":
        result[0] = _with_extension(result[0], defaultextension)
    return result if multiple else result[0]
