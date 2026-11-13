//! Selectors of imperative motions: what isn't read is an error, and one that
//! matches nothing is counted instead of silently dropped.

use blenderui::motion::{Targets, gsap, unmatched_selectors};
use blenderui::prelude::*;
use blenderui::vars;

struct Page;

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body style="margin: 0">
                <div id="box" class="a b" data-x="1" style="width: 10px; height: 10px" />
            </body>
        }
    }
}

#[test]
fn unsupported_selectors_are_errors() {
    for ok in ["#box", ".a.b", "div.a", "[data-x]", "[data-x=\"1\"]", "#box, .a", "*"] {
        assert!(Targets::selector(ok).is_ok(), "{ok}");
    }
    for bad in ["div p", "ul > li", "a + b", "a ~ b", "a:hover", "li:nth-child(2)", "", "a,,b", "#", ".", "[x", "[]", "a::before"] {
        let e = Targets::selector(bad).expect_err(bad);
        assert!(e.contains("supported") || e.contains("selector"), "{bad}: {e}");
    }
}

#[test]
fn a_selector_that_matches_nothing_is_counted() {
    Application::headless().run(|cx| {
        blenderui::motion::lag_smoothing(0.0, 0.0);
        let w = cx.open_window(WindowOptions::default(), |_, cx| cx.new(|_| Page)).unwrap().id;
        let before = unmatched_selectors();
        let _ = gsap::to("#box", vars! { x: 10, duration: 1 });
        let _ = gsap::to("#nope", vars! { x: 10, duration: 1 });
        let _ = gsap::to(".a .b", vars! { x: 10, duration: 1 }); // not read: does nothing, says so
        cx.draw(w);
        assert_eq!(unmatched_selectors() - before, 2, "#nope found nothing, `.a .b` isn't read; #box matched");
    });
}
