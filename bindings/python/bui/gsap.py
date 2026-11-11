"""GSAP's API on BUI's animation engine (``kit/intern/anim``, the same C engine every binding uses).

Keyword arguments where GSAP has a vars object, snake_case names::

    from bui import gsap

    gsap.to(slider, value=80, duration=0.4, ease="power2.out")
    gsap.from_(rows, value=0, stagger=0.05)              # `from` is a keyword in Python
    gsap.from_to(swatch, {"color": "#000"}, {"color": "#f80", "duration": 1})
    tl = gsap.timeline(repeat=1, yoyo=True, defaults={"duration": 0.5})
    tl.to(a, value=1).to(b, value=1, position="<").add_label("mid").call(print, "half", position="mid")
    t = gsap.to(state, x=100, on_update=lambda: label.set("text", f"{state.x:.0f}"))
    t.pause(); t.progress(0.5); t.time_scale(2); t.reverse()

Targets are BUI widgets (kit nodes), lists of them, or any Python object (its attributes).
Widgets animate what the kit draws: ``value``, colors (``color``, ``background_color``)
and the numbers a widget has (``factor``, ``min``, ``max`` ...). The kit's raster has no
transforms and no opacity, so ``x``, ``scale``, ``rotate`` or ``opacity`` on a widget raise
``ValueError`` (on a plain object they are just attributes).

Values: numbers, strings (``"50%"``, ``"+=10"``, ``"#f80"``, ``"rgb(...)"``), tuples (a color,
rgb or rgba floats) and lists (keyframes; ``None`` in one is "where it is"). Eases: every GSAP,
Motion, CSS and Blender name, ``[x1, y1, x2, y2]``, or an :class:`Ease`. Options: ``duration``,
``delay``, ``ease``, ``repeat``, ``repeat_delay``, ``yoyo``, ``time_scale``, ``paused``,
``reversed``, ``immediate_render``, ``overwrite``, ``stagger`` (seconds, or a dict of
``each`` / ``amount`` / ``from`` / ``grid`` / ``axis`` / ``ease``), ``on_start``, ``on_update``,
``on_complete``, ``on_repeat``, ``on_reverse_complete``, ``on_interrupt``. Anything else is a
property. Everything is read once, when the tween is made; frames run in C, and Python only for
callbacks and plain-object targets.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

import buipy

Ease = buipy.Ease
#: A tween or timeline handle: ``play``, ``pause``, ``resume``, ``reverse``, ``restart``,
#: ``seek``, ``progress``, ``total_progress``, ``time``, ``time_scale``, ``paused``,
#: ``reversed``, ``duration``, ``total_duration``, ``is_active``, ``kill``.
Tween = buipy.Motion

_open: list = []  # timelines open as a context (`with gsap.timeline() as tl:`)


def _joined(motion):
    """A motion made inside ``with gsap.timeline()`` goes on the end of that timeline."""
    if _open:
        _open[-1].add(motion)
    return motion


class Timeline(buipy.Motion):
    """Motions placed in time. ``position`` is GSAP's: seconds, ``"<"`` / ``">"`` (the start /
    end of the last one added), ``"+=0.5"`` / ``"-=0.5"`` (after / before the end), ``"<0.2"``,
    a label, ``"label+=1"``; empty (the default) is the end."""

    _defaults: dict = {}

    def to(self, targets, position="", **vars):
        return self.add(_tween(targets, {**self._defaults, **vars}, None, buipy.MOTION_TO), position)

    def from_(self, targets, position="", **vars):
        return self.add(_tween(targets, {**self._defaults, **vars}, None, buipy.MOTION_FROM), position)

    def from_to(self, targets, from_vars: dict, to_vars: dict = None, position="", **vars):
        to = {**self._defaults, **(to_vars or {}), **vars}
        return self.add(_tween(targets, to, from_vars, buipy.MOTION_FROM_TO), position)

    def set(self, targets, position="", **vars):
        return self.add(_tween(targets, vars, None, buipy.MOTION_SET), position)

    def __enter__(self):
        _open.append(self)
        return self

    def __exit__(self, *exc):
        _open.remove(self)
        return False


def _tween(targets, vars, from_vars, mode):
    return buipy.motion_tween(targets, vars, None, from_vars, mode, buipy.FLAVOR_GSAP, Tween)


def to(targets, **vars) -> Tween:
    """Animate ``targets`` from where they are to ``vars``."""
    return _joined(_tween(targets, vars, None, buipy.MOTION_TO))


def from_(targets, **vars) -> Tween:
    """Animate ``targets`` from ``vars`` to where they are (rendered at once)."""
    return _joined(_tween(targets, vars, None, buipy.MOTION_FROM))


def from_to(targets, from_vars: dict, to_vars: dict = None, **vars) -> Tween:
    """Animate ``targets`` from ``from_vars`` to ``to_vars`` (the options are ``to_vars``')."""
    return _joined(_tween(targets, {**(to_vars or {}), **vars}, from_vars, buipy.MOTION_FROM_TO))


def set(targets, **vars) -> Tween:  # noqa: A001 (GSAP's name)
    """Set ``vars`` now (a zero-length tween)."""
    return _joined(_tween(targets, vars, None, buipy.MOTION_SET))


def timeline(*, defaults: dict = None, **vars) -> Timeline:
    """A timeline (``delay``, ``repeat``, ``yoyo``, ``paused``, ``time_scale``, callbacks);
    ``defaults`` go to every tween its ``to`` / ``from_`` / ``from_to`` make. As a context
    (``with gsap.timeline() as tl:``) the tweens made inside go on its end, in order."""
    tl = buipy.motion_timeline(vars, Timeline)
    if defaults:
        tl._defaults = dict(defaults)
    return _joined(tl)


def delayed_call(delay: float, fn, *args) -> Timeline:
    """``fn(*args)`` after ``delay`` seconds."""
    return timeline().call(fn, *args, position=float(delay))


def quick_to(target, prop: str, **vars):
    """A function that animates ``prop`` of ``target`` to a value each call, with ``vars``
    (a new call takes over from the running one, with its velocity for springs)."""
    def move(value):
        return to(target, **{prop: value}, **vars)
    return move


def kill_tweens_of(targets, props=None) -> None:
    """Stop the motions of ``targets`` (of ``props``: a name or names)."""
    for t in targets if isinstance(targets, (list, tuple)) else (targets,):
        buipy.motion_kill_target(t, props)


def register_ease(name: str, ease) -> Ease:
    """Name an ease: an :class:`Ease`, a name, ``[x1, y1, x2, y2]``, an SVG path
    (``"M0,0 C..."``, GSAP's CustomEase) or a curve's points ``[(x, y), ...]`` (a Blender
    curve)."""
    if isinstance(ease, str) and ease.lstrip().upper().startswith("M"):
        e = Ease.path(ease)
    elif isinstance(ease, (list, tuple)) and ease and isinstance(ease[0], (list, tuple)):
        e = Ease.points(ease)
    else:
        e = Ease(ease)
    return e.register(name)


def parse_ease(ease) -> Ease:
    """An ease by name (``ease(t)`` evaluates it); ``ValueError`` for a name it doesn't know."""
    return Ease(ease)


def global_time_scale(scale: float) -> None:
    """Every motion's speed (``gsap.globalTimeline.timeScale``)."""
    buipy.motion_set_time_scale(scale)


def lag_smoothing(threshold: float, adjusted: float = 0.033) -> None:
    """GSAP's lagSmoothing: a frame later than ``threshold`` seconds counts as ``adjusted``
    (0 turns it off)."""
    buipy.motion_set_lag_smoothing(threshold, adjusted)
