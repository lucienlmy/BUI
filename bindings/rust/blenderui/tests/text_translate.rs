//! `translate` moves the text of a box once: a `white-space: pre` run (words in a line box) used to be
//! moved twice, by its owner's translate and again by its own copy of it.

use blenderui::prelude::*;

struct Page;

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body style="margin: 0; background: white">
                <div style="display: flex; margin-top: 50px">
                    <div id="pre" style="font-size: 40px; white-space: pre; translate: 0 90px">"M"</div>
                    <div id="plain" style="font-size: 40px; translate: 0 90px">"M"</div>
                    <div style="font-size: 40px"><span style="position: relative; translate: 0 90px">"M"</span></div>
                </div>
            </body>
        }
    }
}

#[test]
fn text_is_translated_once() {
    Application::headless().run(|cx| {
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(300.0), px(250.0)),
            })),
            ..Default::default()
        };
        let w = cx.open_window(opts, |_, cx| cx.new(|_| Page)).unwrap().id;
        let (pw, _, px) = cx.draw(w);
        let top = |x0: usize, x1: usize| (0..250).find(|&y| (x0..x1).any(|x| px[y * pw + x] & 0xff_ffff != 0xff_ffff));
        let (pre, plain, span) = (top(0, 26), top(26, 52), top(52, 90));
        assert_eq!(pre, plain, "a pre run lands where a plain one does");
        assert_eq!(span, plain, "and so does an inline element's text");
    });
}
