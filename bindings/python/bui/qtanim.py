"""Qt's animation framework (QPropertyAnimation, QVariantAnimation, the animation groups,
QEasingCurve) for :mod:`bui.qtsyntax`, on BUI's animation engine (``kit/intern/anim``, the one
:mod:`bui.gsap` and :mod:`bui.motion` use): an animation is an engine tween when it starts, a
group a timeline (sequential: one after another; parallel: all at 0), and the engine's frames
run it. Values are numbers (ints stay ints), QColor, QPoint, QSize, QRect or tuples.

Imported by :mod:`bui.qtsyntax` (which exports these names); use them from there.

SPDX-License-Identifier: GPL-2.0-or-later
"""

from __future__ import annotations

import math

import buipy

from bui.qtsyntax import QColor, QPoint, QRect, QSize, Signal

# ==========================================================================
# QEasingCurve


def _sampled(fn, n: int = 48) -> buipy.Ease:
    return buipy.Ease.points([(i / n, fn(i / n)) for i in range(n + 1)])


def _qt_mix(t):  # Qt's qt_smoothBeginEndMixFactor
    return min(max(1.0 - t * 2.0 + 0.3, 0.0), 1.0)


def _qt_sin(t):  # Qt's qt_sinProgress
    return math.sin(t * math.pi - math.pi / 2) / 2 + 0.5


# Qt's curves that are no named ease: sampled once (qeasingcurve.cpp)
_CURVES = {
    41: lambda t: _qt_mix(t) * _qt_sin(t) + (1 - _qt_mix(t)) * t,              # InCurve
    42: lambda t: _qt_mix(1 - t) * _qt_sin(t) + (1 - _qt_mix(1 - t)) * t,      # OutCurve
    43: lambda t: (math.sin(t * math.pi * 2 - math.pi / 2) + 1) / 2,           # SineCurve
    44: lambda t: (math.cos(t * math.pi * 2 - math.pi / 2) + 1) / 2,           # CosineCurve
}
_FAMILIES = ("quad", "cubic", "quart", "quint", "sine", "expo", "circ", "elastic", "back", "bounce")
_MODES = (".in", ".out", ".inOut", None)  # None: Qt's OutIn


class QEasingCurve:
    """Qt's easing curves, as the engine's eases (``valueForProgress`` evaluates them)."""

    Linear = 0
    (InQuad, OutQuad, InOutQuad, OutInQuad, InCubic, OutCubic, InOutCubic, OutInCubic,
     InQuart, OutQuart, InOutQuart, OutInQuart, InQuint, OutQuint, InOutQuint, OutInQuint,
     InSine, OutSine, InOutSine, OutInSine, InExpo, OutExpo, InOutExpo, OutInExpo,
     InCirc, OutCirc, InOutCirc, OutInCirc, InElastic, OutElastic, InOutElastic, OutInElastic,
     InBack, OutBack, InOutBack, OutInBack, InBounce, OutBounce, InOutBounce, OutInBounce) = range(1, 41)
    InCurve, OutCurve, SineCurve, CosineCurve, BezierSpline, TCBSpline, Custom = range(41, 48)

    def __init__(self, type=Linear):  # noqa: A002 (Qt's name)
        other = type if isinstance(type, QEasingCurve) else None
        self._type = other._type if other else int(type)
        self._amplitude = other._amplitude if other else 1.0
        self._period = other._period if other else 0.3
        self._overshoot = other._overshoot if other else 1.70158
        self._custom = other._custom if other else None
        self._cache = None

    def type(self):
        return self._type

    def setType(self, t):  # noqa: N802
        self._type, self._cache = int(t), None

    def amplitude(self):
        return self._amplitude

    def setAmplitude(self, a):  # noqa: N802
        self._amplitude, self._cache = float(a), None

    def period(self):
        return self._period

    def setPeriod(self, p):  # noqa: N802
        self._period, self._cache = float(p), None

    def overshoot(self):
        return self._overshoot

    def setOvershoot(self, o):  # noqa: N802
        self._overshoot, self._cache = float(o), None

    def customType(self):  # noqa: N802
        return self._custom

    def setCustomType(self, fn):  # noqa: N802
        self._type, self._custom, self._cache = QEasingCurve.Custom, fn, None

    def _name(self, family: str, mode: str) -> str:
        if family == "elastic":
            return f"elastic{mode}({self._amplitude}, {self._period})"
        if family == "back":
            return f"back{mode}({self._overshoot})"
        return family + mode

    def _make(self) -> buipy.Ease:
        t = self._type
        if t == QEasingCurve.Linear:
            return buipy.Ease("none")
        if 1 <= t <= 40:
            family, mode = _FAMILIES[(t - 1) // 4], _MODES[(t - 1) % 4]
            if mode:
                return buipy.Ease(self._name(family, mode))
            out, into = buipy.Ease(self._name(family, ".out")), buipy.Ease(self._name(family, ".in"))
            return _sampled(lambda x: out(2 * x) / 2 if x < 0.5 else into(2 * x - 1) / 2 + 0.5)
        if t in _CURVES:
            return _sampled(_CURVES[t])
        if t == QEasingCurve.Custom and self._custom is not None:
            return _sampled(self._custom)
        raise NotImplementedError(f"QEasingCurve type {t} (BezierSpline / TCBSpline) is not supported")

    def ease(self) -> buipy.Ease:
        """The engine's ease for this curve (made once)."""
        if self._cache is None:
            self._cache = self._make()
        return self._cache

    def valueForProgress(self, t):  # noqa: N802
        return self.ease()(min(max(float(t), 0.0), 1.0))

    def __eq__(self, other):
        return isinstance(other, QEasingCurve) and (self._type, self._amplitude, self._period, self._overshoot,
                                                    self._custom) == (other._type, other._amplitude, other._period,
                                                                      other._overshoot, other._custom)

    __hash__ = None


# ==========================================================================
# Values: Qt's types <-> the engine's numbers


def _numbers(v):
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, QColor):
        return tuple(v.rgba())
    if isinstance(v, QPoint):
        return (float(v.x()), float(v.y()))
    if isinstance(v, QSize):
        return (float(v.width()), float(v.height()))
    if isinstance(v, QRect):
        return (float(v.x()), float(v.y()), float(v.width()), float(v.height()))
    if isinstance(v, (tuple, list)) and 1 <= len(v) <= 4:
        return tuple(float(c) for c in v)
    raise TypeError(f"can't animate a {type(v).__name__} (numbers, QColor, QPoint, QSize, QRect, tuples)")


def _like(template, v):
    """The engine's numbers `v` as a value of `template`'s type."""
    if isinstance(template, bool) or not isinstance(template, (int, float, QColor, QPoint, QSize, QRect, tuple, list)):
        return v
    if isinstance(template, int):
        return round(v)
    if isinstance(template, float):
        return v
    if isinstance(template, QColor):
        return QColor(*(round(min(max(c, 0.0), 1.0) * 255) for c in v))
    if isinstance(template, (QPoint, QSize, QRect)):
        return type(template)(*(round(c) for c in v[:4]))
    return tuple(v[:len(template)])


# ==========================================================================
# Animations


class QAbstractAnimation:
    """An animation's state and controls; ``start()`` makes the engine's motion."""

    Forward, Backward = 0, 1
    Stopped, Paused, Running = 0, 1, 2
    KeepWhenStopped, DeleteWhenStopped = 0, 1

    finished = Signal()
    stateChanged = Signal(int, int)
    directionChanged = Signal(int)

    def __init__(self, parent=None):
        self._motion = None  # the engine's, while it runs (it holds this animation)
        self._state = QAbstractAnimation.Stopped
        self._loops = 1
        self._direction = QAbstractAnimation.Forward
        self._group = None
        if isinstance(parent, QAnimationGroup):
            parent.addAnimation(self)

    # -- what subclasses make
    def _make(self):
        raise NotImplementedError

    def duration(self) -> int:
        return 0

    def _timing(self) -> dict:
        """The engine's options for its loops, direction and end."""
        backward = self._direction == QAbstractAnimation.Backward
        return {"repeat": -1 if self._loops < 0 else max(0, self._loops - 1), "reversed": backward,
                ("on_reverse_complete" if backward else "on_complete"): self._finish}

    def _set_state(self, new):
        old, self._state = self._state, new
        if old != new:
            self.stateChanged.emit(new, old)

    def _finish(self):
        self._motion = None  # the motion lets go of this animation
        self._set_state(QAbstractAnimation.Stopped)
        self.finished.emit()

    def _begin(self):
        """Make the motion (in a group: the group places it)."""
        if self._motion is not None:
            self._motion.kill()
        self._motion = self._make()
        self._set_state(QAbstractAnimation.Running)
        return self._motion

    # -- Qt's API
    def start(self, policy=KeepWhenStopped):
        if self._loops != 0:
            self._begin()

    def stop(self):
        if self._motion is not None:
            self._motion.kill()
            self._motion = None
        self._set_state(QAbstractAnimation.Stopped)

    def pause(self):
        if self._motion is not None:
            self._motion.pause()
            self._set_state(QAbstractAnimation.Paused)

    def resume(self):
        if self._motion is not None:
            self._motion.resume()
            self._set_state(QAbstractAnimation.Running)

    def setPaused(self, paused):  # noqa: N802
        self.pause() if paused else self.resume()

    def state(self):
        return self._state

    def loopCount(self):  # noqa: N802
        return self._loops

    def setLoopCount(self, n):  # noqa: N802
        self._loops = int(n)

    def direction(self):
        return self._direction

    def setDirection(self, d):  # noqa: N802
        if d != self._direction:
            self._direction = d
            self.directionChanged.emit(d)

    def totalDuration(self):  # noqa: N802
        return -1 if self._loops < 0 else self.duration() * self._loops

    def currentTime(self):  # noqa: N802
        return round(self._motion.total_time() * 1000) if self._motion is not None else 0

    def setCurrentTime(self, ms):  # noqa: N802
        if self._motion is not None:
            self._motion.seek(ms / 1000.0)

    def currentLoopTime(self):  # noqa: N802
        return round(self._motion.time() * 1000) if self._motion is not None else 0

    def group(self):
        return self._group


class QVariantAnimation(QAbstractAnimation):
    """Animates a value between key values (``valueChanged`` each frame)."""

    valueChanged = Signal(object)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._keys = {}
        self._duration = 250
        self._easing = QEasingCurve()
        self._current = None
        self._template = None

    # the engine writes this attribute every frame (it never reads it: every key value is given)
    @property
    def qt_value(self):
        return self._current

    @qt_value.setter
    def qt_value(self, v):
        self._current = _like(self._template, v)
        self.updateCurrentValue(self._current)
        self.valueChanged.emit(self._current)

    def updateCurrentValue(self, value):  # noqa: N802 (Qt's virtual)
        pass

    def _start_value(self):
        raise ValueError("QVariantAnimation: no start value (setStartValue)")

    def _make(self):
        keys = dict(self._keys)
        if 0.0 not in keys:
            keys[0.0] = self._start_value()
        if 1.0 not in keys:
            raise ValueError(f"{type(self).__name__}: no end value (setEndValue)")
        steps = sorted(keys)
        self._template = keys[1.0]
        opts = {"duration": self._duration / 1000.0, "ease": self._easing.ease(), "times": steps, **self._timing()}
        return buipy.motion_tween(self, {"qt_value": [_numbers(keys[s]) for s in steps]}, opts)

    def startValue(self):  # noqa: N802
        return self._keys.get(0.0)

    def setStartValue(self, v):  # noqa: N802
        self._keys[0.0] = v

    def endValue(self):  # noqa: N802
        return self._keys.get(1.0)

    def setEndValue(self, v):  # noqa: N802
        self._keys[1.0] = v

    def keyValueAt(self, step):  # noqa: N802
        return self._keys.get(float(step))

    def setKeyValueAt(self, step, v):  # noqa: N802
        if not 0.0 <= step <= 1.0:
            raise ValueError("a key value's step is 0 .. 1")
        self._keys[float(step)] = v

    def keyValues(self):  # noqa: N802
        return sorted(self._keys.items())

    def setKeyValues(self, values):  # noqa: N802
        self._keys = {}
        for step, v in values:
            self.setKeyValueAt(step, v)

    def currentValue(self):  # noqa: N802
        return self._current

    def duration(self):
        return self._duration

    def setDuration(self, ms):  # noqa: N802
        if ms < 0:
            raise ValueError("a duration can't be negative")
        self._duration = int(ms)

    def easingCurve(self):  # noqa: N802
        return self._easing

    def setEasingCurve(self, e):  # noqa: N802
        self._easing = e if isinstance(e, QEasingCurve) else QEasingCurve(e)


class QPropertyAnimation(QVariantAnimation):
    """Animates a property of an object: its ``setName`` setter, else the attribute."""

    def __init__(self, target=None, prop=b"", parent=None):
        super().__init__(parent)
        self._target = target
        self._prop = prop.decode() if isinstance(prop, bytes) else prop

    def targetObject(self):  # noqa: N802
        return self._target

    def setTargetObject(self, o):  # noqa: N802
        self._target = o

    def propertyName(self):  # noqa: N802
        return self._prop.encode()

    def setPropertyName(self, name):  # noqa: N802
        self._prop = name.decode() if isinstance(name, bytes) else name

    def _start_value(self):
        v = getattr(self._target, self._prop)
        return v() if callable(v) else v

    def updateCurrentValue(self, value):  # noqa: N802
        setter = getattr(self._target, "set" + self._prop[:1].upper() + self._prop[1:], None)
        if callable(setter):
            setter(value)
        else:
            setattr(self._target, self._prop, value)


class QPauseAnimation(QAbstractAnimation):
    """A pause (in a sequential group)."""

    def __init__(self, ms=250, parent=None):
        super().__init__(parent)
        self._duration = int(ms)

    def duration(self):
        return self._duration

    def setDuration(self, ms):  # noqa: N802
        self._duration = int(ms)

    def _make(self):
        tl = buipy.motion_timeline(self._timing())
        return tl.call(_nothing, position=self._duration / 1000.0)  # an empty stretch of time


def _nothing():
    pass


class QAnimationGroup(QAbstractAnimation):
    """Animations run as one: a timeline of their motions."""

    _POSITION = ""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._children = []

    def addAnimation(self, a):  # noqa: N802
        self.insertAnimation(len(self._children), a)

    def insertAnimation(self, i, a):  # noqa: N802
        if a._group is not None:
            a._group.removeAnimation(a)
        a._group = self
        self._children.insert(i, a)

    def removeAnimation(self, a):  # noqa: N802
        self._children.remove(a)
        a._group = None

    def takeAnimation(self, i):  # noqa: N802
        a = self._children[i]
        self.removeAnimation(a)
        return a

    def animationAt(self, i):  # noqa: N802
        return self._children[i]

    def animationCount(self):  # noqa: N802
        return len(self._children)

    def indexOfAnimation(self, a):  # noqa: N802
        return self._children.index(a)

    def clear(self):
        for a in list(self._children):
            self.removeAnimation(a)

    def _make(self):
        tl = buipy.motion_timeline(self._timing())
        for a in self._children:
            tl.add(a._begin(), self._POSITION)
        return tl

    def stop(self):
        super().stop()
        for a in self._children:
            a.stop()


class QSequentialAnimationGroup(QAnimationGroup):
    """One after another."""

    def addPause(self, ms):  # noqa: N802
        p = QPauseAnimation(ms)
        self.addAnimation(p)
        return p

    def insertPause(self, i, ms):  # noqa: N802
        p = QPauseAnimation(ms)
        self.insertAnimation(i, p)
        return p

    def duration(self):
        return sum(max(0, a.totalDuration()) for a in self._children)


class QParallelAnimationGroup(QAnimationGroup):
    """All at once."""

    _POSITION = 0.0

    def duration(self):
        return max((a.totalDuration() for a in self._children), default=0)
