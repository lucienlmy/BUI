//! The builder (gpui) spelling of the Motion examples in the docs: the same elements as the `rsx!` ones.

use std::time::Duration;

use blenderui::motion::presence;
use blenderui::prelude::*;
use blenderui::vars;

fn card(title: &str) -> impl IntoElement {
    div().id("card").styled().p_4().rounded_xl()
        .initial(vars! { opacity: 0, y: 20 })
        .animate(vars! { opacity: 1, y: 0 })
        .exit(vars! { opacity: 0, scale: 0.95 })
        .while_hover(vars! { scale: 1.03 })
        .while_tap(vars! { scale: 0.97 })
        .transition(vars! { type: "spring", stiffness: 400, damping: 30 })
        .child(title.to_string())
}

struct Page {
    open: bool,
}

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        body().m_0()
            .child(card("Hello"))
            .children(presence(self.open, || {
                div().id("panel").key("panel")
                    .initial(vars! { opacity: 0, y: 12 })
                    .animate(vars! { opacity: 1, y: 0 })
                    .exit(vars! { opacity: 0, y: -12 })
                    .transition(vars! { duration: 0.25, ease: "power2.out" })
            }))
    }
}

#[test]
fn builders_animate_like_rsx() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let h = cx.open_window(WindowOptions::default(), |_, cx| cx.new(|_| Page { open: true })).unwrap();
        let w = h.id;
        cx.draw(w);
        cx.advance_clock(Duration::from_millis(1000));
        cx.draw(w);
        assert!(cx.element_bounds(w, "panel").is_some(), "the panel entered");
        h.update(cx, |v, _, cx| {
            v.open = false;
            cx.notify();
        });
        cx.draw(w);
        assert!(cx.element_bounds(w, "panel").is_some(), "and animates out first");
        cx.advance_clock(Duration::from_millis(600));
        cx.draw(w);
        cx.draw(w);
        assert!(cx.element_bounds(w, "panel").is_none(), "then goes");
    });
}
