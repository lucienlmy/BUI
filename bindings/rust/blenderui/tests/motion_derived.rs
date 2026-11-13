//! `use_transform`, `use_velocity` and `use_spring` of a motion value follow it
//! without the view rendering again.

use std::cell::Cell;
use std::sync::atomic::{AtomicU32, Ordering::Relaxed};
use std::time::Duration;

use blenderui::motion::{MotionValue, use_motion_value, use_spring, use_transform, use_velocity};
use blenderui::prelude::*;
use blenderui::vars;

/// How often the view rendered (counted at compile time: no shared ownership).
static RENDERS: AtomicU32 = AtomicU32::new(0);

thread_local! {
    /// The values the view made: source, transform, velocity, spring.
    static SEEN: Cell<Option<[MotionValue; 4]>> = const { Cell::new(None) };
}

struct Page;

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        RENDERS.fetch_add(1, Relaxed);
        let x = use_motion_value(0.0);
        let t = use_transform(x, &[0.0, 100.0], &[0.0, 200.0]);
        let v = use_velocity(x);
        let s = use_spring(x, vars! { stiffness: 300.0, damping: 30.0 });
        SEEN.set(Some([x, t, v, s]));
        rsx! {
            <body style="margin: 0">
                <motion.div id="m" style="width: 10px; height: 10px" style={{ x: t }} />
            </body>
        }
    }
}

fn ms(cx: &mut App, w: WindowId, n: u64) {
    cx.advance_clock(Duration::from_millis(n));
    cx.draw(w);
}

/// 16 ms frames for `n` ms (a spring follows frame by frame).
fn frames(cx: &mut App, w: WindowId, n: u64) {
    for _ in 0..n / 16 {
        ms(cx, w, 16);
    }
}

#[test]
fn derived_values_follow_their_source() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let w = cx.open_window(WindowOptions::default(), |_, cx| cx.new(|_| Page)).unwrap().id;
        cx.draw(w);
        let [x, t, v, s] = SEEN.get().expect("rendered");

        assert_eq!((t.get(), v.get(), s.get()), (0.0, 0.0, 0.0));

        // set by hand: the transform is there at once, before any render
        x.set(50.0);
        assert_eq!(t.get(), 100.0, "mapped now");
        x.set(500.0);
        assert_eq!(t.get(), 200.0, "clamped");
        x.set(0.0);

        // animated: the transform and the velocity move with it, the spring after it
        drop(x.animate(100.0, vars! { duration: 1.0, ease: "none" }));
        ms(cx, w, 16); // the one render the call asked for
        let renders = RENDERS.load(Relaxed);
        frames(cx, w, 500);
        assert!((x.get() - 50.0).abs() < 3.0, "halfway: {}", x.get());
        assert!((t.get() - 100.0).abs() < 6.0, "the transform follows: {}", t.get());
        assert!(v.get() > 50.0, "the velocity: {}", v.get());
        assert!(s.get() > 0.0 && s.get() < x.get() + 1.0, "the spring is on its way: {} (source {})", s.get(), x.get());
        assert!((cx.element_bounds(w, "m").unwrap().origin.x - t.get()).abs() < 6.0, "the element draws it");
        frames(cx, w, 1500);
        assert!((s.get() - 100.0).abs() < 1.0, "the spring arrived: {}", s.get());
        assert!((t.get() - 200.0).abs() < 0.5);
        assert_eq!(RENDERS.load(Relaxed), renders, "nothing rendered again");

        // retargeted midway: the spring turns round without another render
        drop(x.animate(0.0, vars! { duration: 0.4, ease: "none" }));
        frames(cx, w, 100);
        frames(cx, w, 600);
        assert!(s.get() < 5.0, "the spring went back: {}", s.get());
    });
}
