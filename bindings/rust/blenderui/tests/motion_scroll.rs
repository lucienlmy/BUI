//! Motion's useScroll, headless: the values follow the window's scroll.

use std::cell::Cell;
use std::rc::Rc;

use blenderui::motion::{Scroll, use_scroll};
use blenderui::prelude::*;

struct Page {
    seen: Rc<Cell<Option<Scroll>>>,
}

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let s = use_scroll();
        self.seen.set(Some(s));
        let p = s.y_progress;
        rsx! {
            <body style="margin: 0; height: 100vh; overflow-y: auto">
                <motion.div id="bar" style="position: fixed; top: 0; left: 0; height: 4px; width: 100px"
                    style={{ scaleX: p }} />
                <div style="height: 2000px" />
            </body>
        }
    }
}

#[test]
fn scroll_values() {
    Application::headless().run(|cx| {
        let seen = Rc::new(Cell::new(None));
        let s2 = seen.clone();
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(400.0), px(300.0)),
            })),
            ..Default::default()
        };
        let w = cx.open_window(opts, move |_, cx| cx.new(|_| Page { seen: s2 })).unwrap().id;
        cx.draw(w);
        let s = seen.get().expect("rendered");
        assert_eq!(s.scroll_y.get(), 0.0);
        cx.simulate_scroll(w, 100.0, 100.0, -5);
        cx.draw(w);
        cx.draw(w);
        let (y, p) = (s.scroll_y.get(), s.y_progress.get());
        assert!(y > 0.0 && p > 0.0 && p < 1.0, "scrolled: {y} px, {p}");
        assert!((p - y / 1704.0).abs() < 0.01, "progress of the range: {p} vs {y}/1704");
    });
}
