//! `whileInView` respects scroll containers: a row the scroller clips is not in view
//! until it is scrolled in.

use std::time::Duration;

use blenderui::prelude::*;

struct Page;

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body style="margin: 0">
                <div id="scroller" style="height: 60px; width: 200px; overflow: auto">
                    <motion.div id="top" style="height: 40px; width: 10px"
                        whileInView={{ width: 90 }} transition={{ duration: 0.2, ease: "none" }} />
                    <motion.div id="mid" style="height: 40px; width: 10px"
                        whileInView={{ width: 90 }} transition={{ duration: 0.2, ease: "none" }} />
                    <motion.div id="low" style="height: 40px; width: 10px"
                        whileInView={{ width: 90 }} transition={{ duration: 0.2, ease: "none" }} />
                    <motion.div id="end" style="height: 400px; width: 10px"
                        whileInView={{ width: 90 }} transition={{ duration: 0.2, ease: "none" }} />
                </div>
            </body>
        }
    }
}

fn width(cx: &mut App, w: WindowId, id: &str) -> f32 {
    cx.element_bounds(w, id).map_or(-1.0, |b| b.size.width)
}

#[test]
fn clipped_rows_are_not_in_view() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(300.0), px(300.0)),
            })),
            ..Default::default()
        };
        let w = cx.open_window(opts, |_, cx| cx.new(|_| Page)).unwrap().id;
        cx.draw(w);
        cx.advance_clock(Duration::from_millis(400));
        cx.draw(w);
        assert!((width(cx, w, "top") - 90.0).abs() < 0.6, "visible row: {}", width(cx, w, "top"));
        // "low" starts at y 80, below the scroller's 60 px: clipped, though inside the window
        assert!((width(cx, w, "low") - 10.0).abs() < 0.6, "clipped row stays: {}", width(cx, w, "low"));
        assert!((width(cx, w, "end") - 10.0).abs() < 0.6, "clipped row stays: {}", width(cx, w, "end"));
        // scrolled in
        cx.simulate_scroll(w, 100.0, 30.0, -2);
        cx.draw(w);
        cx.advance_clock(Duration::from_millis(400));
        cx.draw(w);
        assert!((width(cx, w, "low") - 90.0).abs() < 0.6, "scrolled in: {} at {:?}", width(cx, w, "low"), cx.element_bounds(w, "low"));
    });
}
