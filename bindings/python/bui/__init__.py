"""bui — Python companion package for the BUI UI toolkit.

High-level syntax layers (the recommended way to write apps):

* :mod:`bui.qtsyntax` — Qt syntax: ``QApplication``, ``QWidget``,
  ``QPushButton``, ``QVBoxLayout``, ``clicked.connect(...)``.
* :mod:`bui.tksyntax` — tkinter syntax: ``Tk()``, ``Button(command=...)``,
  ``pack()``, ``mainloop()``.

Lower-level pieces:

* :class:`bui.svg.SvgSink` — record a frame to an SVG file (no dependencies).
* :class:`bui.qt.BuiCanvas` — embed BUI in a real PySide6/PyQt6 widget.

Everything renders through the BUI engine (buipy, a small extension module).
If ``import buipy`` fails, this package looks for the compiled extension in
the CMake build tree automatically; you can point it at a build dir with the
``BUIPY_BUILD_DIR`` environment variable.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

import glob
import os
import sys

class Backend:
    """Protocol BUI calls with everything the toolkit draws.

    All coordinates are 2D pixels. Colors are RGBA floats in 0..1.
    Override only what you need; unimplemented methods are skipped.
    """

    def begin_frame(self, width: int, height: int) -> None:
        """Called before a batch of primitives."""

    def scissor(self, x: int, y: int, w: int, h: int) -> None:
        """Clip rect; ``w``/``h`` <= 0 removes the clip."""

    def triangles(self, coords, colors) -> None:
        """Non-indexed triangles: ``coords``/``colors`` are per vertex."""

    def lines(self, coords, colors) -> None:
        """Line list: vertices come in endpoint pairs."""

    def points(self, coords, colors, size) -> None:
        """Points at ``size`` pixels."""

    def text(self, x, y, text, size, color) -> None:
        """Text run at baseline position."""

    def end_frame(self) -> None:
        """Called after a batch of primitives."""


def _ensure_buipy_importable() -> None:
    """Make `import buipy` work when running from the source tree.

    The compiled extension lives in the CMake build tree
    (build*/bindings/python). Locating it here means apps can simply
    `import bui` — no PYTHONPATH juggling. Set BUIPY_BUILD_DIR to override.
    """
    try:
        import buipy  # noqa: F401

        return
    except ImportError:
        pass

    # __file__ = <repo>/bindings/python/bui/__init__.py → repo root is up 3 dirs.
    pkg_dir = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(pkg_dir, "..", "..", ".."))
    import importlib.machinery

    # Only extensions built for THIS interpreter (e.g. .cp314-win_amd64.pyd).
    suffixes = importlib.machinery.EXTENSION_SUFFIXES

    def _ext_in(d):
        found = []
        for suf in suffixes:
            found += glob.glob(os.path.join(d, "buipy" + suf))
        return found

    override = os.environ.get("BUIPY_BUILD_DIR", "")
    if override and _ext_in(override):
        best_dir = override
    else:
        # Several build trees may exist (build, build-ui, ...): use the most
        # recently built extension so a stale tree never shadows a fresh one.
        best_dir, best_mtime = None, -1.0
        for cand in glob.glob(os.path.join(repo, "build*", "bindings", "python")):
            for ext in _ext_in(cand):
                mtime = os.path.getmtime(ext)
                if mtime > best_mtime:
                    best_dir, best_mtime = cand, mtime
    if best_dir and best_dir not in sys.path:
        sys.path.insert(0, best_dir)


_ensure_buipy_importable()

try:
    import buipy  # noqa: F401
except ImportError as _e:  # pragma: no cover
    raise ImportError(
        "buipy (the BUI engine extension) was not found. Build it with CMake "
        "(cmake --build build --target buipy) and/or set BUIPY_BUILD_DIR to the "
        "directory containing the built buipy.pyd. Note: the extension must be "
        "built for the Python interpreter you are running."
    ) from _e


from bui._version import __version__  # noqa: E402


def version() -> str:
    """BUI library version as a string."""
    return buipy.version()


def set_backend(backend) -> None:
    """Route BUI drawing into ``backend`` (or None to disable rendering)."""
    buipy.set_backend(backend)


# Images: the kit's decoders and cache (kit/intern/image), the same C code
# every binding uses. image_decode(data, max_w=0, max_h=0) -> (format, w, h,
# [(rgba, delay_ms), ...]); image_probe(data) -> (format, w, h, frames);
# image_texture(data) -> (texture id for Dear ImGui, w, h), freed by
# texture_free; image_stats() -> the shared cache; set_image_budget(bytes).
image_decode = buipy.image_decode
image_probe = buipy.image_probe
image_texture = buipy.image_texture
texture_free = buipy.texture_free
image_stats = buipy.image_stats
set_image_budget = buipy.image_set_budget
image_decode_frame = buipy.image_decode_frame

# Video: the kit's <video> player (one for every binding). Video(src) plays
# (autoplay, with its sound: muted / volume) where it is attached and drawn; state is one of the
# VIDEO_* below; video_backends names the decoders this build has ("mf",
# "avfoundation", "gstreamer", ...; "" without video). The component is
# bui.components.Video.
Video = buipy.Video
video_backends = buipy.video_backends
VIDEO_LOADING, VIDEO_PLAYING, VIDEO_PAUSED, VIDEO_ENDED, VIDEO_ERROR = range(5)


class AnimatedTexture:
    """A GIF or animated WebP as a BUI texture (a Dear ImGui ``ImTextureID``)
    that steps to its next frame when its delay is over: call ``update()``
    every frame, draw ``texture`` at ``size``. Decoded one frame at a time,
    at most ``max_w`` x ``max_h``. ``close()`` frees the texture."""

    def __init__(self, data: bytes, max_w: int = 0, max_h: int = 0):
        import time
        self._clock = time.monotonic
        self._anim, w, h, self.frames, self.loops = buipy.anim_open(data, max_w, max_h)
        self.texture, delay = buipy.anim_next(self._anim, 0)
        self.size = (w, h) if max_w == max_h == 0 else self._fitted(w, h, max_w, max_h)
        self._due = self._clock() + delay / 1000.0

    @staticmethod
    def _fitted(w, h, max_w, max_h):
        k = min(max_w / w if max_w else 1.0, max_h / h if max_h else 1.0, 1.0)
        return max(1, round(w * k)), max(1, round(h * k))

    def update(self) -> bool:
        """Steps to the next frame when it is due; True when it did."""
        now = self._clock()
        if self.texture == 0 or now < self._due:
            return False
        _, delay = buipy.anim_next(self._anim, self.texture)
        self._due = max(self._due + delay / 1000.0, now)
        return True

    def close(self) -> None:
        if self.texture:
            buipy.texture_free(self.texture)
            self.texture = 0


def set_reduced_motion(on: bool = True) -> None:
    """Reduced motion: animated images hold their frame and Blender's panel
    animations jump (``BK_set_animations``)."""
    buipy.kit_set_animations(not on)


def set_theme(name: str = "dark") -> None:
    """Switch every BUI window to Blender's ``"dark"`` (default) or
    ``"light"`` theme (its Blender_Light.xml preset), or to a theme loaded with
    :func:`load_theme`; open windows redraw.
    Without a call the ``BUI_THEME`` environment variable picks the theme."""
    from bui import theme as _theme
    buipy.kit_set_theme(str(name))
    _theme._sync()


def theme_name() -> str:
    """The active theme: ``"dark"``, ``"light"`` or the name of a theme you loaded."""
    return buipy.kit_theme_name()


def load_theme(source) -> list:
    """Register the themes of a theme file (a path) or of theme JSON text; returns the names of all
    the themes loaded so far.

    Choose one with :func:`set_theme`. The format is gpui-kit's, with Blender's color names
    (``docs/theming.md``). Raises ``ValueError`` with the line and column of a mistake; a file with a
    mistake in it adds none of its themes. Keys BUI has no use for are ignored.
    """
    import os
    if isinstance(source, os.PathLike) or (isinstance(source, str) and not source.lstrip().startswith(("{", "["))):
        buipy.kit_theme_load_file(source)
    else:
        buipy.kit_theme_load_json(str(source))
    return buipy.kit_theme_names()


def themes() -> list:
    """The names of the themes loaded with :func:`load_theme`."""
    return buipy.kit_theme_names()


def set_theme_color(key: str, color) -> None:
    """Set one color of the active theme (until the next :func:`set_theme`).

    ``key`` is a plain-English key (``"button.background"``) or Blender's (``"user_interface.wcol_regular.inner"``);
    ``color`` is ``"#rrggbb[aa]"`` or a tuple of 0..1 floats. Raises ``KeyError`` for an unknown key.
    """
    from bui import theme as _theme
    if isinstance(color, str):
        h = color.lstrip("#")
        if len(h) in (3, 4):
            h = "".join(ch * 2 for ch in h)
        vals = [int(h[i:i + 2], 16) / 255.0 for i in range(0, len(h), 2)]
        color = tuple(vals)
    buipy.kit_theme_set_color(key, tuple(color))
    _theme._sync()


def set_theme_value(key: str, value) -> None:
    """Set a number of the active theme: ``"button.roundness"``, ``"button.border.width"`` (pixels),
    ``"button.corners.top_left"`` (``False`` keeps that corner square), ``"button.emboss.enabled"`` ..."""
    buipy.kit_theme_set_num(key, float(value))


def theme_value(key: str) -> float:
    """A number of the active theme (booleans are 0.0 or 1.0)."""
    return buipy.kit_theme_num(key)


def theme_color(key: str):
    """A color of the active theme as ``(r, g, b, a)`` floats, e.g.
    ``"region_bg"``, ``"text"``, ``"regular.inner"`` (see BK_theme_color)."""
    return buipy.kit_theme_color(key)


def set_gpu(on: bool = True) -> None:
    """Draw new native windows on the GPU (Direct3D 11 / OpenGL 3.3): the same pixels as
    the software raster, with rasterization off the CPU. Falls back to the
    software raster where no device exists. ``BUI_GPU=1``/``0`` overrides.
    Per window: ``Tk(gpu=True)``."""
    buipy.set_gpu(bool(on))


def set_slug(on: bool = True) -> None:
    """How the GPU renderer draws text. ``True`` (the default): Slug, glyph
    outlines evaluated in the pixel shader, sharp at any size. ``False``: a
    per-glyph bitmap atlas like Blender's, with less GPU work. Takes effect
    on the next frame. ``BUI_SLUG=1``/``0`` overrides."""
    buipy.set_slug(bool(on))
