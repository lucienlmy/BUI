//! `layout` animates what the layout moved, not what its ancestors carried: a
//! parent that moves (an animated `x`) or scrolls doesn't make its child fly.

use std::time::Duration;

use blenderui::prelude::*;

struct Page {
    tall: bool,
}

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let h = if self.tall { "height: 60px" } else { "height: 10px" };
        rsx! {
            <body style="margin: 0">
                <div style={h} />
                <motion.div id="p" style="width: 100px; height: 40px" animate={{ x: 100, transition: { duration: 1, ease: "none" } }}>
                    <motion.div id="c" style="width: 40px; height: 20px" layout
                        transition={{ duration: 1, ease: "none" }} />
                </motion.div>
                <div id="s" style="margin-top: 100px; width: 100px; height: 40px; overflow: scroll">
                    <div style="height: 30px" />
                    <motion.div id="k" style="width: 40px; height: 20px" layout
                        transition={{ duration: 1, ease: "none" }} />
                    <div style="height: 300px" />
                </div>
            </body>
        }
    }
}

fn at(cx: &mut App, w: WindowId, id: &str) -> (f32, f32) {
    let b = cx.element_bounds(w, id).unwrap();
    (b.origin.x, b.origin.y)
}

fn ms(cx: &mut App, w: WindowId, n: u64) {
    cx.advance_clock(Duration::from_millis(n));
    cx.draw(w);
}

#[test]
fn a_moving_parent_does_not_move_its_child_by_layout() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let h = cx.open_window(WindowOptions::default(), |_, cx| cx.new(|_| Page { tall: false })).unwrap();
        let w = h.id;
        cx.draw(w);
        ms(cx, w, 500);
        let (px_, _) = at(cx, w, "p");
        assert!((px_ - 50.0).abs() < 1.0, "the parent is halfway: {px_}");
        // a layout while the parent moves (something above it grew): both go down, not apart
        h.update(cx, |v, _, cx| {
            v.tall = true;
            cx.notify();
        });
        cx.draw(w);
        let (cx_, cy) = at(cx, w, "c");
        let (px_, py) = at(cx, w, "p");
        assert!((cx_ - px_).abs() < 0.6, "the child stays with its parent: {cx_} vs {px_}");
        assert!((cy - py).abs() < 0.6, "and under it: {cy} vs {py}");
    });
}

#[test]
fn a_scrolled_parent_does_not_move_its_child_by_layout() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let h = cx.open_window(WindowOptions::default(), |_, cx| cx.new(|_| Page { tall: false })).unwrap();
        let w = h.id;
        cx.draw(w);
        ms(cx, w, 16);
        let (_, sy) = at(cx, w, "s");
        cx.simulate_scroll(w, 50.0, sy + 20.0, -1);
        ms(cx, w, 16);
        let scrolled = at(cx, w, "k").1;
        h.update(cx, |v, _, cx| {
            v.tall = true;
            cx.notify();
        });
        cx.draw(w);
        let after = at(cx, w, "k").1;
        // the scroller went down by 50; the child was carried, not animated
        assert!((after - scrolled - 50.0).abs() < 0.6, "carried by the scroller: {scrolled} -> {after}");
    });
}
