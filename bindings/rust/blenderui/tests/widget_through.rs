//! `pointer-events: none` on an overlay: it draws over a widget, and the pointer goes through
//! to the widget below.

use std::sync::atomic::{AtomicU32, Ordering::Relaxed};

use blenderui::prelude::*;

static UNDER: AtomicU32 = AtomicU32::new(0);
static OVER: AtomicU32 = AtomicU32::new(0);

struct Page;

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body style="margin: 0">
                <input id="under" type="checkbox" style="position: absolute; left: 0; top: 0"
                    onChange={|| { UNDER.fetch_add(1, Relaxed); }} />
                <div id="veil" style="position: fixed; inset: 0; background-color: rgba(255, 0, 0, 0.5); z-index: 10; pointer-events: none"
                    onClick={|| { OVER.fetch_add(1, Relaxed); }} />
            </body>
        }
    }
}

#[test]
fn pointer_goes_through_an_overlay() {
    Application::headless().run(|cx| {
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(300.0), px(100.0)),
            })),
            ..Default::default()
        };
        let w = cx.open_window(opts, |_, cx| cx.new(|_| Page)).unwrap().id;
        cx.draw(w);
        cx.simulate_click(w, 8.0, 8.0);
        assert_eq!((UNDER.load(Relaxed), OVER.load(Relaxed)), (1, 0), "the checkbox under the veil toggles");
    });
}
