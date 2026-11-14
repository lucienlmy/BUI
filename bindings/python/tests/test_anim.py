"""bui.gsap, bui.motion and Qt syntax's animations on the kit's animation engine, headless:
the engine's clock is driven by hand (motion_tick), so every value is frame-exact.

Widgets' values and colors, plain objects' attributes, GSAP's from / from_to / set, controls,
timelines (positions, labels, calls), stagger, Motion's defaults and springs, component props,
QPropertyAnimation and the groups; callbacks fire once and are released with their motion;
unknown eases, options and properties raise at the call.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import gc
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import bui  # noqa: E402,F401  (locates buipy)
import buipy  # noqa: E402
from bui import gsap, motion  # noqa: E402
from bui.components import ColorSwatch, NumberSlider, ProgressBar  # noqa: E402

# the clock is the test's: frames of exactly the time asked for
buipy.motion_set_host_clock(True)
buipy.motion_set_lag_smoothing(0)
_now = [100.0]


def step(seconds):
    _now[0] += seconds
    buipy.motion_tick(_now[0])


step(0.0)


def near(a, b, eps=1e-3):
    return abs(a - b) <= eps


class Obj:
    def __init__(self, **kw):
        self.__dict__.update(kw)


def test_widget_value():
    s = NumberSlider("Volume", 10, min=0, max=100)
    t = gsap.to(s, value=90, duration=1, ease="none")
    step(0.25)
    assert near(s.value, 30), s.value
    step(0.25)
    assert near(s.value, 50) and near(t.progress(), 0.5)
    t.pause()
    step(0.5)
    assert near(s.value, 50) and t.paused()
    t.resume().time_scale(2)
    step(0.125)
    assert near(s.value, 70), s.value
    t.seek(0.25)
    assert near(s.value, 30)
    t.progress(1.0)
    assert near(s.value, 90)
    assert not t.is_active() and near(t.duration(), 1.0)


def test_widget_color_and_errors():
    sw = ColorSwatch("Tint", (0.0, 0.0, 1.0, 1.0))
    gsap.to(sw, value="#ff0000", duration=1, ease="none")
    step(0.5)
    r, g, b, a = sw.get_color("value")
    assert near(r, 0.5) and near(b, 0.5), (r, g, b)
    step(0.5)
    bar = ProgressBar(0.0)
    try:
        gsap.to(bar, opacity=0)
        raise AssertionError("opacity on a widget must raise")
    except ValueError as e:
        assert "opacity" in str(e) and "no transforms or opacity" in str(e)
    for bad, kind in (({"factor": 1, "ease": "power9.sideways"}, ValueError),
                      ({"factor": "12abc"}, ValueError),
                      ({"factor": 1, "repeat": "x"}, TypeError),
                      ({"factor": 1, "on_complete": 3}, TypeError)):
        try:
            gsap.to(bar, **bad)
            raise AssertionError(f"{bad} must raise")
        except kind:
            pass
    try:
        motion.animate(bar, {"factor": 1}, durration=1)  # a typo is an error, not a property
        raise AssertionError("an unknown option must raise")
    except TypeError as e:
        assert "durration" in str(e)
    try:
        gsap.parse_ease("nonsense")
        raise AssertionError("unknown ease must raise")
    except ValueError as e:
        assert "nonsense" in str(e)
    assert near(gsap.parse_ease("power1.out")(0.5), 0.75) and near(gsap.Ease([0.42, 0, 0.58, 1])(0.5), 0.5)


def test_from_from_to_set_relative():
    o = Obj(x=30.0, y=0.0, c=(0.0, 0.0, 0.0, 1.0))
    gsap.from_(o, x=100, duration=1, ease="none")
    assert near(o.x, 100)  # rendered at once
    step(0.5)
    assert near(o.x, 65)
    step(0.5)
    assert near(o.x, 30)
    gsap.from_to(o, {"y": 10}, {"y": 20, "duration": 1, "ease": "none"})
    assert near(o.y, 10)
    step(0.5)
    assert near(o.y, 15)
    step(0.5)
    gsap.set(o, x=5)
    assert near(o.x, 5)
    gsap.to(o, x="+=10", duration=1)
    step(1.0)
    assert near(o.x, 15)
    gsap.to(o, x=[0, 100, 0], duration=1, ease="none")  # keyframes
    step(0.25)
    assert near(o.x, 50)
    step(0.75)
    assert near(o.x, 0)


def test_callbacks_once_and_released():
    o = Obj(v=0.0)
    calls = []

    def done():
        calls.append(o.v)

    before = sys.getrefcount(done)
    t = gsap.to(o, v=1, duration=0.5, on_complete=done)
    assert sys.getrefcount(done) == before + 1  # the motion holds it
    for _ in range(4):
        step(0.25)
    assert calls == [1.0], calls
    del t  # the handle; the engine let go when it ended
    gc.collect()
    assert sys.getrefcount(done) == before, "the callback must be released with its motion"
    # an error in a callback comes out of the frame that ran it
    gsap.to(o, v=2, duration=0.1, on_complete=lambda: 1 / 0)
    try:
        step(0.2)
        raise AssertionError("the callback's error must surface")
    except ZeroDivisionError:
        pass


def test_timeline_positions():
    a, b, c = Obj(x=0.0), Obj(x=0.0), Obj(x=0.0)
    hits = []
    tl = gsap.timeline(defaults={"duration": 1, "ease": "none"})
    tl.to(a, x=100).to(b, x=100, position="<0.5").add_label("mid", ">")
    tl.to(c, x=100, position="mid+=0.25").call(hits.append, "mid", position="mid")
    assert near(tl.label_time("mid"), 1.5) and near(tl.duration(), 2.75), tl.duration()
    step(0.75)
    assert near(a.x, 75) and near(b.x, 25) and c.x == 0
    step(1.0)
    assert near(a.x, 100) and near(b.x, 100) and near(c.x, 0) and hits == ["mid"]
    tl.seek(0.25)
    assert near(a.x, 25) and near(b.x, 0)
    tl.kill()
    # as a context: tweens made inside go on its end, in order
    d, e = Obj(x=0.0), Obj(x=0.0)
    with gsap.timeline() as seq:
        gsap.to(d, x=1, duration=0.5, ease="none")
        gsap.to(e, x=1, duration=0.5, ease="none")
    assert near(seq.duration(), 1.0)
    step(0.75)
    assert near(d.x, 1) and near(e.x, 0.5)
    # repeat + yoyo
    f = Obj(x=0.0)
    t = gsap.to(f, x=100, duration=1, ease="none", repeat=1, yoyo=True)
    assert near(t.total_duration(), 2.0)
    step(1.5)
    assert near(f.x, 50)
    step(1.0)
    assert near(f.x, 0)


def test_stagger_and_lists():
    rows = [Obj(x=0.0) for _ in range(3)]
    done = []
    tl = gsap.to(rows, x=10, duration=1, ease="none", stagger=0.5, on_complete=lambda: done.append(1))
    step(0.5)
    assert near(rows[0].x, 5) and near(rows[1].x, 0) and rows[2].x == 0
    step(1.0)
    assert near(rows[0].x, 10) and near(rows[2].x, 5) and not done
    step(0.5)
    assert near(rows[2].x, 10) and done == [1] and near(tl.duration(), 2.0)
    # Motion's delay=stagger(...)
    cols = [Obj(v=0.0) for _ in range(3)]
    motion.animate(cols, {"v": 1}, duration=1, ease="linear", delay=motion.stagger(0.25, from_="last"))
    step(0.25)
    assert near(cols[2].v, 0.25) and near(cols[1].v, 0.0) and cols[0].v == 0


def test_motion_defaults_and_springs():
    o = Obj(opacity=0.0, x=0.0)
    c = motion.animate(o, {"opacity": 1})  # 0.3 s eased tween
    assert near(c.duration, 0.3)
    step(0.3)
    assert near(o.opacity, 1)
    s = motion.animate(o, {"x": 100})  # a spring (Motion's default for x)
    assert s.duration > 0.3
    for _ in range(240):
        step(1 / 60)
    assert near(o.x, 100, 0.5)
    k = motion.animate(o, {"x": [0, 50, 0]})  # keyframes: 0.8 s
    assert near(k.duration, 0.8)
    k.time = 0.4
    assert near(o.x, 50, 1.0) and near(k.time, 0.4)
    k.speed = 2
    assert k.speed == 2
    k.stop()
    b = Obj(y=0.0)
    motion.animate(b, {"y": 1}, type="spring", bounce=0, visual_duration=0.5)
    for _ in range(120):
        step(1 / 60)
    assert near(b.y, 1, 0.01)
    seq = motion.animate([(Obj(x=0.0), {"x": 1}, {"duration": 0.5}), (Obj(x=0.0), {"x": 1}, {"duration": 0.5, "at": "<"})])
    assert near(seq.duration, 0.5)


def test_component_props():
    s = NumberSlider("Volume", 0.3, min=0, max=1, initial={"value": 0}, animate={"value": 0.8},
                     transition={"duration": 1, "ease": "linear"})
    assert near(s.value, 0.3)  # nothing moves before the first frame
    step(0.5)
    assert near(s.value, 0.4), s.value
    step(0.5)
    assert near(s.value, 0.8) and s.motion is not None
    held = NumberSlider("Gain", 0.5, initial={"value": 0.1})
    step(0.1)
    assert near(held.value, 0.1)
    try:
        NumberSlider("X", while_hover={"value": 1})
        raise AssertionError("while_hover must say it isn't supported")
    except NotImplementedError as e:
        assert "while_hover" in str(e)


def test_qt_animations():
    from bui.qtsyntax import (QColor, QEasingCurve, QParallelAnimationGroup, QPropertyAnimation,
                              QSequentialAnimationGroup, QVariantAnimation)
    assert near(QEasingCurve(QEasingCurve.OutQuad).valueForProgress(0.5), 0.75)
    assert near(QEasingCurve(QEasingCurve.InOutCubic).valueForProgress(0.5), 0.5)
    assert near(QEasingCurve(QEasingCurve.OutInQuad).valueForProgress(0.25), 0.375, 2e-2)
    target = Obj(value=0)
    anim = QPropertyAnimation(target, b"value")
    anim.setEndValue(100)
    anim.setDuration(1000)
    seen = []
    anim.finished.connect(lambda: seen.append("done"))
    anim.start()
    step(0.5)
    assert target.value == 50 and isinstance(target.value, int) and anim.state() == anim.Running
    step(0.5)
    assert target.value == 100 and seen == ["done"] and anim.state() == anim.Stopped
    # colors, key values
    va = QVariantAnimation()
    va.setStartValue(QColor(0, 0, 0))
    va.setKeyValueAt(0.5, QColor(255, 0, 0))
    va.setEndValue(QColor(255, 255, 255))
    va.setDuration(1000)
    va.start()
    step(0.5)
    assert va.currentValue().red() == 255 and va.currentValue().green() == 0
    step(0.5)
    # groups: sequential with a pause, then parallel
    a, b = Obj(x=0.0), Obj(x=0.0)
    seq = QSequentialAnimationGroup()
    for t in (a, b):
        p = QPropertyAnimation(t, b"x")
        p.setStartValue(0.0)
        p.setEndValue(1.0)
        p.setDuration(500)
        seq.addAnimation(p)
        if t is a:
            seq.addPause(250)
    assert seq.duration() == 1250
    seq.start()
    step(0.5)
    assert near(a.x, 1) and b.x == 0
    step(0.5)
    assert near(b.x, 0.5), b.x
    step(0.5)
    assert seq.state() == seq.Stopped
    c, d = Obj(x=0.0), Obj(x=0.0)
    par = QParallelAnimationGroup()
    for t in (c, d):
        p = QPropertyAnimation(t, b"x", par)
        p.setEndValue(1.0)
        p.setDuration(1000)
    par.start()
    step(0.5)
    assert near(c.x, 0.5) and near(d.x, 0.5)
    step(0.5)


def test_attribute_spellings_stay_per_target():
    # scale_x and scaleX are one property to the engine; each object keeps its own spelling
    a, b = Obj(scale_x=1.0), Obj(scaleX=1.0)
    ta = gsap.to(a, scale_x=3, duration=1, ease="none")
    tb = gsap.to(b, scaleX=5, duration=1, ease="none")
    step(0.5)
    assert near(a.scale_x, 2.0) and near(b.scaleX, 3.0), (a.__dict__, b.__dict__)
    assert "scaleX" not in a.__dict__ and "scale_x" not in b.__dict__, (a.__dict__, b.__dict__)
    step(1.0)
    assert near(a.scale_x, 3.0) and near(b.scaleX, 5.0)
    ta.kill()
    tb.kill()


def test_one_property_one_spelling_while_it_runs():
    a = Obj(scale_x=1.0, scaleX=1.0)
    first = gsap.to(a, scale_x=3, duration=1, ease="none")
    # the other spelling of a running property is an error, and the running tween writes on as it did
    try:
        gsap.to(a, scaleX=9, duration=1, delay=0.5)
        raise AssertionError("two spellings of one running property")
    except ValueError as e:
        assert "same property" in str(e), e
    # a call that is rejected for another reason changes nothing either
    try:
        gsap.to(a, scale_x=9, duration=float("nan"))
    except ValueError:
        pass
    step(0.5)
    assert near(a.scale_x, 2.0) and a.scaleX == 1.0, a.__dict__
    first.kill()
    del first
    gc.collect()
    # nothing runs on it: the other spelling is free
    t = gsap.to(a, scaleX=9, duration=1, ease="none")
    step(0.5)
    assert near(a.scaleX, 5.0), a.__dict__
    t.kill()


def _refused(exc, fn, *args, **kw):
    try:
        fn(*args, **kw)
    except exc:
        return True
    return False


def test_numbers_and_positions_are_validated_once_by_the_engine():
    nan, inf = float("nan"), float("inf")
    a = Obj(x=0.0)
    # NaN and infinity are errors; a negative duration or delay is clamped (the engine's contract)
    for kw in ({"duration": nan}, {"delay": nan}, {"repeat_delay": nan}, {"duration": inf}, {"delay": -inf}):
        assert _refused(ValueError, gsap.to, a, x=1, **kw), kw
    t = gsap.to(a, x=10, duration=-1, ease="none")
    step(0.1)
    assert near(a.x, 10) and t.duration() == 0.0
    # a repeat of any size is the engine's clamp, not a cast of ours (2**32 is not 0)
    for rep in (2**31, 2**32, 2**70, 10**30):
        r = gsap.to(Obj(x=0.0), x=1, duration=1, repeat=rep, ease="none")
        assert r.total_duration() == 1000001.0, (rep, r.total_duration())
        r.kill()
    f = gsap.to(Obj(x=0.0), x=1, duration=1, repeat=-1, ease="none")
    assert f.total_duration() > 1e100
    f.kill()
    z = gsap.to(Obj(x=0.0), x=1, duration=0, repeat=-1)  # a repeat for ever of nothing ends
    step(0.1)
    assert not z.is_active() and z.progress() == 1.0
    # controls take finite numbers only
    m = gsap.to(Obj(x=0.0), x=1, duration=1, ease="none")
    for call in (m.seek, m.progress, m.total_progress, m.time_scale, m.time, m.total_time):
        assert _refused(ValueError, call, nan), call
    assert _refused(ValueError, m.seek, inf)
    m.kill()
    m.play().resume().seek(0.5)  # killed stays killed: nothing revives it
    step(1.0)
    assert not m.is_active() and _refused(ValueError, m.seek, float("nan"))
    # positions: GSAP's grammar and nothing else
    tl = gsap.timeline()
    tl.to(Obj(x=0.0), x=1, duration=2, ease="none")
    for bad in ("<=1", "a b", "-inf", "0x10", "+=", "1e999", "<x", "a++1", "1 2"):
        assert _refused(ValueError, tl.add_label, "l", bad), bad
        assert _refused(ValueError, tl.to, Obj(x=0.0), x=1, position=bad), bad
        assert _refused(ValueError, tl.call, print, position=bad), bad
    assert tl.label_time("l") is None and near(tl.duration(), 2.0)
    for good, at in (("<", 0.0), (">", 2.0), ("<0.5", 0.5), (">-0.5", 1.5), ("+=1", 3.0), ("-=1", 1.0), ("50%", 1.0), (0.25, 0.25)):
        tl.add_label("g", good)
        assert near(tl.label_time("g"), at), (good, tl.label_time("g"))
    tl.kill()


def main() -> int:
    if not buipy.motion_available():
        print("anim tests: no animation in this build, skipped")
        return 0
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print("ok", t.__name__)
    print("all animation tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
