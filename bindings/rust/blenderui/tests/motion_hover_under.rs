//! An element that moves under a pointer that stays still is hovered (a page that opens under the mouse, a
//! list that scrolls): `whileHover` and `:hover` follow the layout, not only the pointer's moves.

use std::time::Duration;

use blenderui::prelude::*;

struct Page {
    moved: bool,
}

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let left = if self.moved { "0px" } else { "300px" };
        rsx! {
            <body style="margin: 0">
                <motion.div id="card" style={format!("position: absolute; top: 0; left: {left}; width: 100px; height: 100px; background: red")}
                    whileHover={{ width: 160 }} transition={{ duration: 0.2, ease: "linear" }} />
            </body>
        }
    }
}

#[test]
fn hover_follows_the_layout() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(500.0), px(200.0)),
            })),
            ..Default::default()
        };
        let h = cx.open_window(opts, |_, cx| cx.new(|_| Page { moved: false })).unwrap();
        let w = h.id;
        cx.draw(w);
        cx.simulate_mouse_move(w, 50.0, 50.0); // over nothing yet
        cx.draw(w);
        h.update(cx, |v, _, cx| {
            v.moved = true; // the card is under the pointer now
            cx.notify();
        });
        cx.draw(w);
        cx.advance_clock(Duration::from_millis(400));
        cx.draw(w);
        let width = cx.element_bounds(w, "card").unwrap().size.width;
        assert!((width - 160.0).abs() < 0.6, "hovered without the pointer moving: {width}");
    });
}

struct Rise;

impl Render for Rise {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body style="margin: 0">
                <motion.div id="card" style="position: absolute; top: 0; left: 0; width: 100px; height: 100px; background: red"
                    initial={{ y: 300 }} animate={{ y: 0 }} whileHover={{ width: 160 }}
                    transition={{ duration: 0.5, ease: "linear" }} />
            </body>
        }
    }
}

/// The card rises under a pointer that stays still.
#[test]
fn hover_follows_a_motion() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(500.0), px(400.0)),
            })),
            ..Default::default()
        };
        let w = cx.open_window(opts, |_, cx| cx.new(|_| Rise)).unwrap().id;
        cx.draw(w);
        cx.simulate_mouse_move(w, 50.0, 50.0);
        for _ in 0..80 {
            cx.advance_clock(Duration::from_millis(16));
            cx.draw(w);
        }
        let width = cx.element_bounds(w, "card").unwrap().size.width;
        assert!((width - 160.0).abs() < 0.6, "hovered once it arrived under the pointer: {width}");
    });
}
