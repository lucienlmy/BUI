//! Text whose motions ended is painted where they ended, while something else keeps animating.

use std::time::Duration;

use blenderui::motion::gsap;
use blenderui::vars;
use blenderui::prelude::*;

struct Page;

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body style="margin: 0; background: white">
                <div style="display: flex; margin-top: 50px">
                    <div class="l" style="font-size: 40px; white-space: pre">"M"</div>
                    <div class="l" style="font-size: 40px; white-space: pre">"M"</div>
                    <div class="l" style="font-size: 40px; white-space: pre">"M"</div>
                </div>
                <div id="dot" style="position: absolute; right: 10px; top: 10px; width: 10px; height: 10px; background: red" />
            </body>
        }
    }
}

/// Whether anything dark is in the rows `y0..y1`.
fn ink(cx: &mut App, w: WindowId, y0: usize, y1: usize) -> bool {
    let (pw, _, px) = cx.draw(w);
    (y0..y1).any(|y| (0..120).any(|x| px[y * pw + x] & 0xff > 0 && px[y * pw + x] & 0xff < 100))
}

#[test]
fn ended_motions_paint_their_end() {
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
        let _ = gsap::from(".l", vars! { y: 90, rotate: 10, duration: 0.5, ease: "none", stagger: 0.1 });
        let _ = gsap::to("#dot", vars! { y: 20, duration: 1, ease: "none", repeat: -1, yoyo: true });
        cx.draw(w);
        for _ in 0..40 {
            cx.advance_clock(Duration::from_millis(50));
            cx.draw(w);
        }
        // 2 s on: the letters are in their row (y 50..100), none where they started (140..190)
        assert!(ink(cx, w, 50, 100), "the letters are where their motions ended");
        assert!(!ink(cx, w, 140, 190), "and not where they began");
    });
}

