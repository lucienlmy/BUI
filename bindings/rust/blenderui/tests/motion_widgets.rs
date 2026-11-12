//! A redraw of Motion's values has no layout: the widgets of the last frame stay.

use std::time::Duration;

use blenderui::prelude::*;

struct Page;

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body style="margin: 0; background: black">
                <motion.div style="height: 30px; background: blue" initial={{ opacity: 0 }}
                    animate={{ opacity: 1, transition: { duration: 1, ease: "none" } }} />
                <button id="btn">"Go"</button>
            </body>
        }
    }
}

fn lit(cx: &mut App, w: WindowId, x: usize, y: usize) -> bool {
    let (pw, _, px) = cx.draw(w);
    px[y * pw + x] & 0xff_ffff != 0
}

#[test]
fn redraw_keeps_widgets() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(200.0), px(100.0)),
            })),
            ..Default::default()
        };
        let w = cx.open_window(opts, |_, cx| cx.new(|_| Page)).unwrap().id;
        cx.draw(w);
        let b = cx.element_bounds(w, "btn").expect("the button has a box");
        let (x, y) = (b.origin.x as usize + 4, (b.origin.y + b.size.height / 2.0) as usize);
        assert!(lit(cx, w, x, y), "the button draws");
        cx.advance_clock(Duration::from_millis(300));
        assert!(lit(cx, w, x, y), "the button is still there after a redraw");
        cx.advance_clock(Duration::from_millis(900));
        assert!(lit(cx, w, x, y), "and after the motion ends");
    });
}
