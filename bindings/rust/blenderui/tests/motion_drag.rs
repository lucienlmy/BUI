//! Motion's drag, headless: the pointer moves the element (redraws), its
//! constraints give elastically, and letting go springs back or carries on.

use std::time::Duration;

use blenderui::prelude::*;

struct Page;

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body style="margin: 0">
                <motion.div id="a" style="position: absolute; left: 0; top: 0; width: 50px; height: 50px; background: red"
                    drag="x" dragConstraints={{ left: 0, right: 100 }} dragMomentum={false} />
                <motion.div id="b" style="position: absolute; left: 0; top: 100px; width: 50px; height: 50px; background: blue"
                    drag="x" />
            </body>
        }
    }
}

fn x(cx: &mut App, w: WindowId, id: &str) -> f32 {
    cx.element_bounds(w, id).unwrap().origin.x
}

fn ms(cx: &mut App, w: WindowId, n: u64) {
    cx.advance_clock(Duration::from_millis(n));
    cx.draw(w);
}

#[test]
fn drag() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(600.0), px(300.0)),
            })),
            ..Default::default()
        };
        let w = cx.open_window(opts, |_, cx| cx.new(|_| Page)).unwrap().id;
        cx.draw(w);
        // follows the pointer
        cx.simulate_press(w, 25.0, 25.0);
        ms(cx, w, 16);
        cx.simulate_mouse_move(w, 75.0, 25.0);
        ms(cx, w, 16);
        assert!((x(cx, w, "a") - 50.0).abs() < 0.6, "follows: {}", x(cx, w, "a"));
        // past its right constraint (100) with half the give: 100 + 100 * 0.5
        cx.simulate_mouse_move(w, 225.0, 25.0);
        ms(cx, w, 16);
        assert!((x(cx, w, "a") - 150.0).abs() < 0.6, "elastic: {}", x(cx, w, "a"));
        // let go: springs back to the constraint
        cx.simulate_release(w, 225.0, 25.0);
        for _ in 0..60 {
            ms(cx, w, 16);
        }
        assert!((x(cx, w, "a") - 100.0).abs() < 0.6, "back to its constraint: {}", x(cx, w, "a"));

        // momentum: thrown, it carries on past where it was let go
        cx.simulate_press(w, 25.0, 125.0);
        for k in 1..=4 {
            ms(cx, w, 16);
            cx.simulate_mouse_move(w, 25.0 + 20.0 * k as f32, 125.0);
        }
        ms(cx, w, 16);
        let at_release = x(cx, w, "b");
        cx.simulate_release(w, 105.0, 125.0);
        for _ in 0..30 {
            ms(cx, w, 16);
        }
        assert!(x(cx, w, "b") > at_release + 20.0, "carried on: {at_release} -> {}", x(cx, w, "b"));
    });
}
