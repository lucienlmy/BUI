"""Motion's (Framer Motion's) API on BUI's animation engine (``kit/intern/anim``, the one every
binding uses; :mod:`bui.gsap` is the same engine with GSAP's names).

::

    from bui.motion import animate, stagger

    c = animate(slider, {"value": 80}, duration=0.8, ease="easeOut")
    animate(rows, {"value": [0, 1, 0.5]}, delay=stagger(0.05, from_="last"))
    animate(knob, {"factor": 1}, type="spring", stiffness=300, damping=20)
    animate([(a, {"value": 1}), (b, {"value": 1}, {"at": "<"})])     # a sequence
    c.pause(); c.time = 0.3; c.speed = 0.5; c.play()

and on components, Motion's props as keyword arguments::

    NumberSlider("Volume", 0.2, initial={"value": 0}, animate={"value": 0.8},
                 transition={"duration": 0.3})

Motion's defaults: springs for ``x`` / ``y`` / ``scale`` / ``rotate``, 0.3 s eased tweens
for the rest, 0.8 s for keyframes. Options (``transition``): ``type`` (``"tween"``,
``"spring"``, ``"inertia"``), ``duration``, ``delay``, ``ease`` (a name, ``[x1, y1, x2, y2]``,
or a list: one per keyframe segment), ``times``, ``repeat``, ``repeat_delay``, ``yoyo``,
spring's ``stiffness`` / ``damping`` / ``mass`` / ``bounce`` / ``visual_duration`` /
``velocity`` / ``rest_delta`` / ``rest_speed``, inertia's ``power`` / ``time_constant`` /
``min`` / ``max``, and the callbacks (``on_update``, ``on_complete`` ...). An unknown option
raises ``TypeError``.

Kit widgets have no transforms or opacity (the raster has none): they animate values, colors
and sizes. Any Python object animates its attributes.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

import buipy

#: ``type=spring`` / ``type=inertia``, as Motion spells them.
spring, inertia = "spring", "inertia"


class AnimationControls(buipy.Motion):
    """Motion's playback controls: ``play()``, ``pause()``, ``complete()``, ``stop()``,
    ``cancel()``, and the properties ``time``, ``speed``, ``duration``, ``state``."""

    @property
    def time(self) -> float:
        return buipy.Motion.total_time(self)

    @time.setter
    def time(self, seconds: float) -> None:
        self.seek(seconds)

    @property
    def speed(self) -> float:
        return buipy.Motion.time_scale(self)

    @speed.setter
    def speed(self, s: float) -> None:
        buipy.Motion.time_scale(self, s)

    @property
    def duration(self) -> float:
        return buipy.Motion.total_duration(self)

    @property
    def state(self) -> str:
        if buipy.Motion.paused(self):
            return "paused"
        return "running" if self.is_active() else "finished"

    def stop(self) -> None:
        """Stop where it is."""
        self.kill()

    cancel = stop


def stagger(each: float = 0.1, *, from_="first", ease=None, start: float = 0.0) -> dict:
    """``delay=stagger(0.05)``: each target starts ``each`` seconds after the previous;
    ``from_`` is ``"first"``, ``"last"``, ``"center"``, ``"edges"`` or an index."""
    if start:
        raise NotImplementedError("stagger(start=...) is not supported: use delay on a timeline")
    spec = {"each": each, "from": from_}
    if ease is not None:
        spec["ease"] = ease
    return spec


def animate(targets, values: dict = None, transition: dict = None, **options) -> AnimationControls:
    """Animate ``targets`` (a widget, an object, or a list) to ``values``; ``transition`` and
    keyword arguments are the options. ``animate([(target, values, options), ...])`` plays a
    sequence: each segment after the last (``"at"``: ``"<"`` with the last, ``"+0.2"`` /
    ``"-0.2"`` after its end, seconds, or a label)."""
    if values is None and isinstance(targets, list):
        return _sequence(targets, {**(transition or {}), **options})
    return buipy.motion_tween(targets, values, {**(transition or {}), **options}, None,
                              buipy.MOTION_TO, buipy.FLAVOR_MOTION, AnimationControls)


def _at(at) -> str:
    """Motion's ``at`` as GSAP's position."""
    if isinstance(at, str) and at[:1] in "+-":
        return ">" + at
    return "" if at is None else at


def _sequence(segments, options: dict) -> AnimationControls:
    seq = buipy.motion_timeline(options, AnimationControls)
    for seg in segments:
        target, values, *rest = seg
        opts = dict(rest[0]) if rest else {}
        at = opts.pop("at", None)
        seq.add(buipy.motion_tween(target, values, opts, None, buipy.MOTION_TO, buipy.FLAVOR_MOTION), _at(at))
    return seq


def mount(target, initial: dict = None, animate: dict = None, transition: dict = None):
    """Motion's ``initial`` / ``animate`` / ``transition`` props on a component that was just
    made, from its first frame (so after its constructor set its own values): what ``initial``
    alone names holds that value, what ``animate`` names goes there, from ``initial``'s value
    when it has one, else from where it is."""
    initial, animate = initial or {}, animate or {}
    held = {k: v for k, v in initial.items() if k not in animate}
    opts = {"immediate_render": False, **(transition or {})}
    return buipy.motion_tween(target, {**animate, **held}, opts, initial, buipy.MOTION_FROM_TO,
                              buipy.FLAVOR_MOTION, AnimationControls)
