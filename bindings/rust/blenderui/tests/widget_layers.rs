//! Elements painted after a form widget draw over it (z-index, a later sibling),
//! and take the pointer there: issue #35 (a dialog over a TextArea and a Button).

use std::cell::Cell;
use std::rc::Rc;

use blenderui::prelude::*;

struct Page {
    dialog: bool,
    under: Rc<Cell<u32>>,
    over: Rc<Cell<u32>>,
    inside: Rc<Cell<u32>>,
}

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let (under, over, inside) = (self.under.clone(), self.over.clone(), self.inside.clone());
        rsx! {
            <body style="margin: 0">
                <TextArea style="position: absolute; left: 0; top: 0; width: 200px; height: 80px" />
                <Button id="under" style="position: absolute; left: 0; top: 100px; width: 120px; height: 30px"
                    onClick={move || under.set(under.get() + 1)}>"Under"</Button>
                {if self.dialog {
                    rsx! {
                        <div id="backdrop" style="position: fixed; inset: 0; background-color: rgb(255, 0, 0); z-index: 1000"
                            onClick={move || over.set(over.get() + 1)}>
                            <Button id="ok" style="position: absolute; left: 300px; top: 20px; width: 80px; height: 30px"
                                onClick={move || inside.set(inside.get() + 1)}>"OK"</Button>
                        </div>
                    }.into_any_element()
                } else {
                    div().into_any_element()
                }}
            </body>
        }
    }
}

fn red(p: u32) -> bool {
    (p >> 16) & 255 > 200 && (p >> 8) & 255 < 60 && p & 255 < 60
}

fn pixels(cx: &mut App, w: WindowId) -> (usize, Vec<u32>) {
    let (pw, _, px) = cx.draw(w);
    (pw, px)
}

#[test]
fn dialog_over_widgets() {
    Application::headless().run(|cx| {
        let (under, over, inside) = (Rc::new(Cell::new(0)), Rc::new(Cell::new(0)), Rc::new(Cell::new(0)));
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(400.0), px(200.0)),
            })),
            ..Default::default()
        };
        let (u, o, i) = (under.clone(), over.clone(), inside.clone());
        let h = cx.open_window(opts, move |_, cx| cx.new(|_| Page { dialog: false, under: u, over: o, inside: i }))
            .unwrap();
        let w = h.id;
        let (pw, px_) = pixels(cx, w);
        assert!(!red(px_[40 * pw + 100]), "the TextArea is there without the dialog");

        h.update(cx, |v, _, cx| {
            v.dialog = true;
            cx.notify();
        });
        let (pw, px_) = pixels(cx, w);
        assert!(red(px_[40 * pw + 100]), "the dialog is over the TextArea: {:#08x}", px_[40 * pw + 100]);
        assert!(red(px_[115 * pw + 60]), "and over the Button: {:#08x}", px_[115 * pw + 60]);

        // clicks: on the dialog (over the Button) they reach the dialog, not the Button below
        cx.simulate_click(w, 60.0, 115.0);
        assert_eq!((over.get(), under.get()), (1, 0), "over the Button: the dialog's");
        // the dialog's own Button is above its backdrop
        cx.simulate_click(w, 340.0, 35.0);
        assert_eq!(inside.get(), 1, "a Button in the dialog is clickable");
        assert_eq!(over.get(), 2, "and its click bubbles to the backdrop, as in a browser");
        let (pw, px_) = pixels(cx, w);
        assert!(!red(px_[35 * pw + 340]), "and drawn over the backdrop: {:#08x}", px_[35 * pw + 340]);

        // gone again: the widgets are back on top, and clickable
        h.update(cx, |v, _, cx| {
            v.dialog = false;
            cx.notify();
        });
        let (pw, px_) = pixels(cx, w);
        assert!(!red(px_[40 * pw + 100]));
        cx.simulate_click(w, 60.0, 115.0);
        assert_eq!(under.get(), 1, "the Button works again");
    });
}
