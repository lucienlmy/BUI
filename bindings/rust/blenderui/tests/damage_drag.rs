//! What the kit repaints on the pixels of the last frame equals a full render, through a drag and the
//! momentum after it: a lost damaged region leaves a ghost of the old place.

use std::time::Duration;

use blenderui::prelude::*;

struct Page {
    ticks: u32,
}

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let label = format!("tick {}", self.ticks);
        rsx! {
            <body style="margin: 0">
                <div style="position: absolute; left: 4px; top: 4px; color: white">{label}</div>
                <div style="position: relative; width: 300px; height: 150px; background: #101014">
                    <motion.div id="blue" style="position: absolute; left: 150px; top: 30px; width: 60px; height: 60px; background: #3aa0ff"
                        drag={true} whileDrag={{ scale: 1.1 }} transition={{ type: "spring", stiffness: 400, damping: 22 }} />
                </div>
            </body>
        }
    }
}

/// What a native window would show: the screen is updated from the damaged rectangles alone.
fn differs(cx: &mut App, w: WindowId, screen: &mut Vec<u32>) -> Option<(usize, usize)> {
    let (pw, ph, partial, full, damage) = cx.draw_partial_and_full(w);
    if screen.len() != pw * ph {
        *screen = partial.clone(); // the first frame is a full present
    }
    for [x, y, rw, rh] in damage {
        for row in y..y + rh {
            let at = row as usize * pw + x as usize;
            screen[at..at + rw as usize].copy_from_slice(&partial[at..at + rw as usize]);
        }
    }
    assert_eq!(partial, full, "the kit's own frame is the full frame");
    screen.iter().zip(&full).position(|(a, b)| a != b).map(|i| (i % pw, i / pw))
}

#[test]
fn drag_leaves_no_ghosts() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(500.0), px(300.0)),
            })),
            ..Default::default()
        };
        let h = cx.open_window(opts, |_, cx| cx.new(|_| Page { ticks: 0 })).unwrap();
        let w = h.id;
        cx.draw(w);
        let mut screen = Vec::new();
        assert_eq!(differs(cx, w, &mut screen), None);
        cx.simulate_mouse_move(w, 170.0, 60.0);
        cx.simulate_press(w, 170.0, 60.0);
        let (mut x, mut y) = (170.0, 60.0);
        for step in 0..40 {
            x += 6.0;
            y += 4.0;
            cx.simulate_mouse_move(w, x, y);
            if step % 7 == 3 {
                // the view renders again mid-drag (a label changes), as an app's own updates do
                h.update(cx, |v, _, cx| {
                    v.ticks += 1;
                    cx.notify();
                });
            }
            cx.advance_clock(Duration::from_millis(16));
            assert_eq!(differs(cx, w, &mut screen), None, "ghost while dragging, step {step}");
        }
        cx.simulate_release(w, x, y);
        for step in 0..120 {
            if step % 11 == 5 {
                h.update(cx, |v, _, cx| {
                    v.ticks += 1;
                    cx.notify();
                });
            }
            cx.advance_clock(Duration::from_millis(16));
            assert_eq!(differs(cx, w, &mut screen), None, "ghost in the momentum, step {step}");
        }
    });
}
