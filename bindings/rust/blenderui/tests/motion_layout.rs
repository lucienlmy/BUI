//! Motion's `layout` (FLIP), headless: a box pushed by a change of layout
//! starts where it was seen and glides to its new place.

use std::time::Duration;

use blenderui::prelude::*;

struct Page {
    tall: bool,
}

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let h = if self.tall { "height: 60px" } else { "height: 10px" };
        rsx! {
            <body style="margin: 0">
                <div style={h} />
                <motion.div id="m" style="width: 40px; height: 20px" layout
                    transition={{ duration: 1, ease: "none" }} />
                <motion.div id="p" style="width: 40px; height: 20px" layout="position"
                    transition={{ duration: 1, ease: "none" }} />
            </body>
        }
    }
}

fn y(cx: &mut App, w: WindowId, id: &str) -> f32 {
    cx.element_bounds(w, id).unwrap().origin.y
}

fn ms(cx: &mut App, w: WindowId, n: u64) {
    cx.advance_clock(Duration::from_millis(n));
    cx.draw(w);
}

#[test]
fn layout_animation() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let h = cx.open_window(WindowOptions::default(), |_, cx| cx.new(|_| Page { tall: false })).unwrap();
        let w = h.id;
        cx.draw(w);
        ms(cx, w, 16);
        assert_eq!(y(cx, w, "m"), 10.0);
        assert_eq!(y(cx, w, "p"), 30.0);
        h.update(cx, |v, _, cx| {
            v.tall = true;
            cx.notify();
        });
        cx.draw(w);
        assert!((y(cx, w, "p") - 30.0).abs() < 0.6, "seen where it was: {}", y(cx, w, "p"));
        ms(cx, w, 500);
        assert!((y(cx, w, "p") - 55.0).abs() < 0.6, "halfway to 80: {}", y(cx, w, "p"));
        ms(cx, w, 600);
        assert!((y(cx, w, "p") - 80.0).abs() < 0.6, "there: {}", y(cx, w, "p"));
        assert!((y(cx, w, "m") - 60.0).abs() < 0.6, "the other too: {}", y(cx, w, "m"));
        assert!(!cx.window(w).unwrap().is_animating());
    });
}
