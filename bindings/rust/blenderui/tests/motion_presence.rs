//! `presence` (Motion's AnimatePresence) and `whileInView`, headless.

use std::time::Duration;

use blenderui::motion::presence;
use blenderui::prelude::*;

struct Page {
    open: bool,
}

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let open = self.open;
        rsx! {
            <body style="margin: 0">
                {...presence(open, || rsx! {
                    <motion.div id="p" style="width: 100px; height: 10px"
                        initial={{ width: 0 }} animate={{ width: 100 }}
                        exit={{ width: 0 }} transition={{ duration: 1, ease: "none" }} />
                })}
                <motion.div id="seen" style="width: 10px; height: 10px"
                    whileInView={{ width: 50 }} transition={{ duration: 0.5, ease: "none" }} />
                <motion.div id="far" style="position: absolute; top: 5000px; width: 10px; height: 10px"
                    whileInView={{ width: 50 }} transition={{ duration: 0.5, ease: "none" }} />
            </body>
        }
    }
}

fn width(cx: &mut App, w: WindowId, id: &str) -> Option<f32> {
    cx.element_bounds(w, id).map(|b| b.size.width)
}

fn ms(cx: &mut App, w: WindowId, n: u64) {
    cx.advance_clock(Duration::from_millis(n));
    cx.draw(w);
}

fn near(a: Option<f32>, b: f32) -> bool {
    a.is_some_and(|a| (a - b).abs() <= 0.6)
}

#[test]
fn presence_and_in_view() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(400.0), px(300.0)),
            })),
            ..Default::default()
        };
        let h = cx.open_window(opts, |_, cx| cx.new(|_| Page { open: true })).unwrap();
        let w = h.id;
        cx.draw(w);
        ms(cx, w, 1000);
        assert!(near(width(cx, w, "p"), 100.0), "entered: {:?}", width(cx, w, "p"));
        // in view: its whileInView target; far below the fold: not
        ms(cx, w, 600);
        assert!(near(width(cx, w, "seen"), 50.0), "in view: {:?}", width(cx, w, "seen"));
        assert!(near(width(cx, w, "far"), 10.0), "out of view: {:?}", width(cx, w, "far"));

        // taken away: it stays and animates out, then goes
        h.update(cx, |v, _, cx| {
            v.open = false;
            cx.notify();
        });
        cx.draw(w);
        ms(cx, w, 500);
        assert!(near(width(cx, w, "p"), 50.0), "exiting halfway: {:?}", width(cx, w, "p"));
        // shown again midway: back from where it is, not from `initial`
        h.update(cx, |v, _, cx| {
            v.open = true;
            cx.notify();
        });
        cx.draw(w);
        ms(cx, w, 250);
        let back = width(cx, w, "p").unwrap_or(0.0);
        assert!(back > 50.0 && back < 100.0, "back on its way: {back}");
        ms(cx, w, 1000);
        h.update(cx, |v, _, cx| {
            v.open = false;
            cx.notify();
        });
        cx.draw(w);
        ms(cx, w, 1100);
        ms(cx, w, 16);
        assert_eq!(width(cx, w, "p"), None, "gone after its exit");
    });
}
