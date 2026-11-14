//! `repeat: -1` on elements by selector: it plays until killed (the timeline that held it had no length).

use std::time::Duration;

use blenderui::motion::gsap;
use blenderui::prelude::*;
use blenderui::vars;

struct Page;

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! { <body style="margin: 0"><div id="a" class="a" style="width: 20px; height: 20px" /></body> }
    }
}

#[test]
fn infinite_repeat_runs() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let w = cx.open_window(WindowOptions::default(), |_, cx| cx.new(|_| Page)).unwrap().id;
        cx.draw(w);
        let t = gsap::to(".a", vars! { x: 100, duration: 1, repeat: -1, ease: "none" });
        cx.draw(w);
        let x = |cx: &mut App| cx.element_bounds(w, "a").map_or(-1.0, |b| b.origin.x);
        for (t_ms, want) in [(500, 50.0), (1000, 0.0), (1500, 50.0)] {
            let _ = t_ms;
            cx.advance_clock(Duration::from_millis(500));
            cx.draw(w);
            assert!((x(cx) - want).abs() < 0.6, "at {t_ms} ms: {} (wanted {want})", x(cx));
        }
        assert!(cx.window(w).unwrap().is_animating(), "it keeps the window animating");
        t.kill();
    });
}
