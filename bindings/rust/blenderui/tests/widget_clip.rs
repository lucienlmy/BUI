//! A widget partly cut by an ancestor's `overflow: hidden` is cut there, not drawn past the edge
//! (and the pointer doesn't reach the cut part).

use std::sync::atomic::{AtomicU32, Ordering::Relaxed};

use blenderui::prelude::*;

static CLICKS: AtomicU32 = AtomicU32::new(0);

struct Page;

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body style="margin: 0; background: white">
                <div style="position: relative; width: 60px; height: 40px; overflow: hidden">
                    <Button style="position: absolute; left: 20px; top: 0; width: 120px; height: 30px" onClick={|| { CLICKS.fetch_add(1, Relaxed); }}>"Wide"</Button>
                </div>
            </body>
        }
    }
}

fn white(p: u32) -> bool {
    p & 0xff_ffff == 0xff_ffff
}

#[test]
fn widget_is_cut_by_overflow() {
    Application::headless().run(|cx| {
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(200.0), px(100.0)),
            })),
            ..Default::default()
        };
        let w = cx.open_window(opts, |_, cx| cx.new(|_| Page)).unwrap().id;
        let (pw, _, px_) = cx.draw(w);
        assert!(!white(px_[15 * pw + 40]), "the part inside shows: {:#08x}", px_[15 * pw + 40]);
        assert!(white(px_[15 * pw + 90]), "the part past the edge doesn't: {:#08x}", px_[15 * pw + 90]);
        cx.simulate_click(w, 90.0, 15.0);
        assert_eq!(CLICKS.load(Relaxed), 0, "nor does it take the pointer");
        cx.simulate_click(w, 40.0, 15.0);
        assert_eq!(CLICKS.load(Relaxed), 1, "the part inside does");
    });
}
