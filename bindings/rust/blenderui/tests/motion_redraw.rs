//! Frames of Motion's values alone are redraws: the pixels move, the view
//! doesn't render again.

use std::cell::Cell;
use std::rc::Rc;
use std::time::Duration;

use blenderui::prelude::*;

struct Page {
    renders: Rc<Cell<u32>>,
}

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.renders.set(self.renders.get() + 1);
        rsx! {
            <body style="margin: 0; background: black">
                <motion.div id="c" style="position: absolute; left: 0; top: 0; width: 50px; height: 50px"
                    initial={{ backgroundColor: "#ff0000" }}
                    animate={{ backgroundColor: "#0000ff", x: 100, transition: { duration: 1, ease: "none" } }} />
                // no `initial`: its first value comes a frame after it appears
                <motion.div id="d" style="position: absolute; left: 0; top: 50px; width: 50px; height: 50px; background: #ff0000"
                    animate={{ backgroundColor: "#0000ff", transition: { duration: 1, ease: "none" } }} />
                // opacity on a redraw: half faded white over white stays white, so over black
                <div style="position: absolute; left: 150px; top: 0; width: 50px; height: 50px; background: black">
                    <motion.div style="width: 50px; height: 50px; background: white" initial={{ opacity: 1 }}
                        animate={{ opacity: 0, transition: { duration: 1, ease: "none" } }} />
                </div>
            </body>
        }
    }
}

fn pixel(cx: &mut App, w: WindowId, x: usize, y: usize) -> (u32, u32, u32) {
    let (pw, _, px) = cx.draw(w);
    let p = px[y * pw + x];
    ((p >> 16) & 255, (p >> 8) & 255, p & 255)
}

fn near(a: (u32, u32, u32), b: (u32, u32, u32)) -> bool {
    a.0.abs_diff(b.0) <= 4 && a.1.abs_diff(b.1) <= 4 && a.2.abs_diff(b.2) <= 4
}

#[test]
fn moving_values_redraw() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let renders = Rc::new(Cell::new(0));
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(200.0), px(100.0)),
            })),
            ..Default::default()
        };
        let r = renders.clone();
        let w = cx.open_window(opts, move |_, cx| cx.new(|_| Page { renders: r })).unwrap().id;
        assert!(near(pixel(cx, w, 10, 25), (255, 0, 0)), "starts red: {:?}", pixel(cx, w, 10, 25));
        let after_mount = renders.get();
        cx.advance_clock(Duration::from_millis(500));
        // halfway: purple, moved 50 px right (the left 50 px are background now)
        let mid = pixel(cx, w, 60, 25);
        assert!(near(mid, (128, 0, 128)) || near(mid, (127, 0, 127)), "halfway: {mid:?}");
        let o = pixel(cx, w, 175, 25);
        assert!(o.0.abs_diff(128) <= 6 && o.1.abs_diff(128) <= 6, "opacity halfway on a redraw: {o:?}");
        let d = pixel(cx, w, 10, 75);
        assert!(near(d, (128, 0, 128)) || near(d, (127, 0, 127)), "without initial, halfway too: {d:?}");
        assert!(!near(pixel(cx, w, 10, 25), (255, 0, 0)) && !near(pixel(cx, w, 10, 25), mid), "moved off its start: {:?}", pixel(cx, w, 10, 25));
        cx.advance_clock(Duration::from_millis(600));
        assert!(near(pixel(cx, w, 110, 25), (0, 0, 255)), "ends blue at x 100: {:?}", pixel(cx, w, 110, 25));
        assert_eq!(renders.get(), after_mount, "frames of moving values don't render the view");
        assert!(!cx.window(w).unwrap().is_animating());
    });
}
