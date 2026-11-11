//! Motion's props on elements (`<motion.div animate={{..}}>`), on the virtual
//! clock of headless windows. One test: the engine is the UI thread's.

use std::time::Duration;

use std::cell::Cell;
use std::rc::Rc;

use blenderui::motion::{MotionValue, Value, gsap};
use blenderui::prelude::*;
use blenderui::vars;

fn open<V: Render>(cx: &mut App, v: V) -> WindowHandle<V> {
    let opts = WindowOptions {
        window_bounds: Some(WindowBounds::Windowed(Bounds {
            origin: point(px(0.0), px(0.0)),
            size: size(px(400.0), px(300.0)),
        })),
        ..Default::default()
    };
    let w = cx.open_window(opts, |_, cx| cx.new(|_| v)).unwrap();
    cx.draw(w.id);
    w
}

fn bounds(cx: &mut App, win: WindowId, id: &str) -> Bounds<f32> {
    cx.element_bounds(win, id).unwrap_or_else(|| panic!("no element {id}"))
}

fn ms(cx: &mut App, win: WindowId, n: u64) {
    cx.advance_clock(Duration::from_millis(n));
    cx.draw(win);
}

fn near(a: f32, b: f32, eps: f32) -> bool {
    (a - b).abs() <= eps
}

struct Page {
    open: bool,
    mv: MotionValue,
}

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let (wide, mv) = (self.open, self.mv);
        rsx! {
            <body style="margin: 0">
                <motion.div id="tween" style="height: 10px; width: 10px"
                    animate={{ width: 110, transition: { duration: 1, ease: "linear" } }} />
                <motion.div id="keys" style="height: 10px; width: 0px"
                    animate={{ width: [0, 100, 50], transition: { duration: 2, ease: "none" } }} />
                <motion.div id="off" style="height: 10px; width: 10px" initial={false}
                    animate={{ width: 60 }} />
                <motion.div id="from" style="height: 10px; width: 10px" initial={{ width: 200 }}
                    animate={{ width: 100, transition: { duration: 1, ease: "linear" } }} />
                <motion.div id="spring" style="height: 10px; width: 10px"
                    animate={{ width: if wide { 300.0 } else { 10.0 }, transition: { type: "spring", stiffness: 300, damping: 30 } }} />
                <motion.div id="variant" style="height: 10px; width: 10px"
                    variants={[("small", vars! { width: 20 }), ("big", vars! { width: 80 })]}
                    initial="small" animate={if wide { "big" } else { "small" }}
                    transition={{ duration: 0.5, ease: "linear" }} />
                <motion.div id="x" style="height: 10px; width: 10px"
                    animate={{ x: if wide { 100.0 } else { 0.0 }, transition: { duration: 1, ease: "linear" } }} />
                <motion.div id="hover" style="position: absolute; left: 0; top: 200px; height: 50px; width: 50px"
                    whileHover={{ width: 150 }} transition={{ duration: 0.5, ease: "linear" }} />
                <div class="box" id="b1" style="height: 10px; width: 10px" />
                <div class="box" id="b2" style="height: 10px; width: 10px" />
                <motion.div id="bound" style="height: 10px" style={{ width: mv }} />
            </body>
        }
    }
}

#[test]
fn motion_elements() {
    // literal objects are statics: nothing allocated per render
    let v = vars! { x: 100, opacity: 0.5, rotate: [0, 90, null], duration: 1, ease: "power2.out" };
    assert!(matches!(v.props, std::borrow::Cow::Borrowed(_)), "a static object");
    assert_eq!(v.props.len(), 3);
    assert_eq!(v.transition.duration, Some(1.0));
    assert!(v.transition.ease.is_some());
    assert!(matches!(&v.props[2].1, Value::Keys(k) if k[2] == Value::Current), "null keyframe: current");
    let n = 42.0f32;
    let dynamic = vars! { x: n, type: "spring", stiffness: 300 };
    assert!(matches!(dynamic.props, std::borrow::Cow::Owned(_)));
    assert_eq!(dynamic.transition.kind, blenderui::motion::Kind::Spring);

    Application::headless().run(|cx| {
        // the steps here are long: GSAP's lag smoothing would take them for stalls
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let mv = MotionValue::new(30.0);
        let h = open(cx, Page { open: false, mv });
        let w = h.id;
        // a tween: 10 -> 110 over 1 s, linear
        assert!(near(bounds(cx, w, "tween").size.width, 10.0, 0.5), "starts where its style is");
        ms(cx, w, 250);
        assert!(near(bounds(cx, w, "tween").size.width, 35.0, 0.6), "a quarter: {}", bounds(cx, w, "tween").size.width);
        ms(cx, w, 250);
        assert!(near(bounds(cx, w, "tween").size.width, 60.0, 0.6));
        // keyframes: 0 -> 100 -> 50 over 2 s
        ms(cx, w, 500); // t = 1: at the middle keyframe
        assert!(near(bounds(cx, w, "keys").size.width, 100.0, 0.6), "keys at 1 s: {}", bounds(cx, w, "keys").size.width);
        assert!(near(bounds(cx, w, "tween").size.width, 110.0, 0.6), "the tween ended");
        // initial={false}: at its target from the first frame
        assert!(near(bounds(cx, w, "off").size.width, 60.0, 0.5));
        // initial={{..}}: from there (200 -> 100 over 1 s: done)
        assert!(near(bounds(cx, w, "from").size.width, 100.0, 0.6));
        ms(cx, w, 1000);
        assert!(near(bounds(cx, w, "keys").size.width, 50.0, 0.6));
        assert!(near(bounds(cx, w, "variant").size.width, 20.0, 0.5), "initial variant");

        // a state change: new targets animate from where they are
        h.update(cx, |v, _, cx| {
            v.open = true;
            cx.notify();
        });
        cx.draw(w);
        let x0 = bounds(cx, w, "x").origin.x;
        ms(cx, w, 250);
        assert!(near(bounds(cx, w, "variant").size.width, 50.0, 0.6), "variant halfway: {}", bounds(cx, w, "variant").size.width);
        assert!(near(bounds(cx, w, "x").origin.x - x0, 25.0, 0.6), "x moves the box: {}", bounds(cx, w, "x").origin.x - x0);
        let s1 = bounds(cx, w, "spring").size.width;
        assert!(s1 > 100.0 && s1 < 330.0, "the spring is on its way: {s1}");
        // and back midway: the spring turns around from where it is, without a jump
        h.update(cx, |v, _, cx| {
            v.open = false;
            cx.notify();
        });
        ms(cx, w, 16);
        let s2 = bounds(cx, w, "spring").size.width;
        assert!((s2 - s1).abs() < 40.0, "no jump: {s1} -> {s2}");
        for _ in 0..40 {
            ms(cx, w, 50);
        }
        assert!(near(bounds(cx, w, "spring").size.width, 10.0, 0.6), "settled back");
        assert!(near(bounds(cx, w, "x").origin.x, x0, 0.6), "x back");

        // whileHover: on top while the pointer is over it, back when it leaves
        cx.simulate_mouse_move(w, 20.0, 220.0);
        ms(cx, w, 250);
        assert!(near(bounds(cx, w, "hover").size.width, 100.0, 0.6), "hovered halfway: {}", bounds(cx, w, "hover").size.width);
        cx.simulate_mouse_move(w, 390.0, 290.0);
        ms(cx, w, 500);
        assert!(near(bounds(cx, w, "hover").size.width, 50.0, 0.6), "back: {}", bounds(cx, w, "hover").size.width);
        ms(cx, w, 100);
        assert!(!cx.window(w).unwrap().is_animating(), "idle once everything settled");

        // style={{ width: mv }}: the element follows the value every frame
        assert!(near(bounds(cx, w, "bound").size.width, 30.0, 0.5));
        let done = Rc::new(Cell::new(0));
        let d = done.clone();
        let t = gsap::to(mv, vars! { value: 130, duration: 1, ease: "none", onComplete: move || d.set(d.get() + 1) });
        ms(cx, w, 500);
        assert!(near(mv.get(), 80.0, 0.5), "gsap.to a value: {}", mv.get());
        assert!(near(bounds(cx, w, "bound").size.width, 80.0, 0.6), "the bound element moved with it");
        t.pause();
        ms(cx, w, 250);
        assert!(near(mv.get(), 80.0, 0.5), "paused");
        t.resume();
        ms(cx, w, 600);
        assert!(near(mv.get(), 130.0, 0.5) && done.get() == 1, "complete, once: {} {}", mv.get(), done.get());

        // gsap.to by selector: the elements, staggered in document order
        let _ = gsap::to(".box", vars! { width: 110, duration: 1, ease: "none", stagger: 0.5 });
        cx.draw(w);
        ms(cx, w, 500);
        assert!(near(bounds(cx, w, "b1").size.width, 60.0, 0.6), "b1 halfway: {}", bounds(cx, w, "b1").size.width);
        assert!(near(bounds(cx, w, "b2").size.width, 10.0, 0.6), "b2 starts 0.5 s later");
        ms(cx, w, 1000);
        assert!(near(bounds(cx, w, "b2").size.width, 110.0, 0.6) && near(bounds(cx, w, "b1").size.width, 110.0, 0.6));

        // a timeline: positions, labels, calls
        let calls = Rc::new(Cell::new(0));
        let c = calls.clone();
        let a = MotionValue::new(0.0);
        let b = MotionValue::new(0.0);
        let tl = gsap::timeline(vars! {});
        tl.to(a, vars! { value: 100, duration: 1, ease: "none" }, "")
            .add_label("half", "<0.5")
            .to(b, vars! { value: 100, duration: 1, ease: "none" }, "half")
            .call(move || c.set(c.get() + 1), ">");
        assert_eq!(tl.label_time("half"), Some(0.5));
        assert!(near(tl.duration() as f32, 1.5, 1e-6));
        ms(cx, w, 1000);
        assert!(near(a.get(), 100.0, 0.5) && near(b.get(), 50.0, 0.5), "a {} b {}", a.get(), b.get());
        ms(cx, w, 600);
        assert!(near(b.get(), 100.0, 0.5) && calls.get() == 1, "the call ran once at the end");
        tl.seek(0.25);
        assert!(near(a.get(), 25.0, 0.5) && near(b.get(), 0.0, 0.5), "seek back: every child where it belongs");
        a.dispose();
        b.dispose();
    });
}
