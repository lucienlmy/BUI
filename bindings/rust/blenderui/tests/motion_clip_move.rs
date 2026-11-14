//! An element that clips its children and moves takes their clip with it, also when that clip equals the one
//! its stationary parent has.

use std::sync::atomic::{AtomicU32, Ordering::Relaxed};
use std::time::Duration;

use blenderui::prelude::*;

static CLICKS: AtomicU32 = AtomicU32::new(0);

struct Page;

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body style="margin: 0">
                <div style="position: relative; width: 100px; height: 100px; overflow: hidden">
                    <motion.div id="e" style="position: relative; width: 100px; height: 100px; overflow: hidden"
                        animate={{ x: 50, transition: { duration: 0.1, ease: "none" } }}>
                        <div id="g" style="position: absolute; left: -40px; top: 0; width: 80px; height: 100px; background: red"
                            onClick={|| { CLICKS.fetch_add(1, Relaxed); }} />
                    </motion.div>
                </div>
            </body>
        }
    }
}

#[test]
fn a_moved_clip_moves_its_children_clip() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(300.0), px(200.0)),
            })),
            ..Default::default()
        };
        let w = cx.open_window(opts, |_, cx| cx.new(|_| Page)).unwrap().id;
        cx.draw(w);
        for _ in 0..20 {
            cx.advance_clock(Duration::from_millis(16));
            cx.draw(w);
        }
        // `e` is at 50..150 now (its parent shows 0..100); `g` reaches from 10 to 90, and e clips it at 50
        cx.simulate_click(w, 20.0, 50.0);
        assert_eq!(CLICKS.load(Relaxed), 0, "left of the moved clip: not g");
        cx.simulate_click(w, 70.0, 50.0);
        assert_eq!(CLICKS.load(Relaxed), 1, "inside it: g");
    });
}
