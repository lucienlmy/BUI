//! A frame of moving values touches the animated elements and what is under
//! them, not every box of the page.

use std::time::Duration;

use blenderui::motion::redraw_work;
use blenderui::prelude::*;

const ROWS: usize = 3000;

struct Page;

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let rows = div().children((0..ROWS).map(|_| div().w(px(10.0)).h(px(1.0))));
        rsx! {
            <body style="margin: 0">
                <div>{rows}</div>
                <motion.div id="m" style="position: absolute; left: 0; top: 0; width: 20px; height: 20px"
                    animate={{ x: 100, opacity: 0.5, transition: { duration: 1, ease: "none" } }}>
                    <div style="width: 5px; height: 5px" />
                </motion.div>
            </body>
        }
    }
}

#[test]
fn redraws_walk_the_animated_subtree() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(200.0), px(200.0)),
            })),
            ..Default::default()
        };
        let w = cx.open_window(opts, |_, cx| cx.new(|_| Page)).unwrap().id;
        cx.draw(w);
        cx.advance_clock(Duration::from_millis(16));
        cx.draw(w);
        let before = redraw_work();
        for _ in 0..10 {
            cx.advance_clock(Duration::from_millis(16));
            cx.draw(w);
        }
        let per_frame = (redraw_work() - before) / 10;
        // wall time of the same frames (the draw included), for the PR's numbers
        let t0 = std::time::Instant::now();
        for _ in 0..40 {
            cx.advance_clock(Duration::from_millis(5));
            cx.draw(w);
        }
        eprintln!("{ROWS} rows: {per_frame} boxes visited per redraw, {:?} per frame", t0.elapsed() / 40);
        assert!(per_frame > 0, "frames redrew");
        assert!(per_frame < 20, "a frame visited {per_frame} of {ROWS} rows: only the animated element and its child");
        let b = cx.element_bounds(w, "m").unwrap();
        assert!(b.origin.x > 10.0, "and it moved: {}", b.origin.x);
    });
}
