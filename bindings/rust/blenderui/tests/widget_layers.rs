//! Elements painted after a form widget draw over it (z-index, a later sibling),
//! and take the pointer there: issue #35 (a dialog over a TextArea and a Button).

use std::sync::atomic::{AtomicU32, Ordering::Relaxed};

use blenderui::prelude::*;

// clicks heard, counted at compile time (no shared ownership)
static UNDER: AtomicU32 = AtomicU32::new(0);
static OVER: AtomicU32 = AtomicU32::new(0);
static INSIDE: AtomicU32 = AtomicU32::new(0);
static HIGH: AtomicU32 = AtomicU32::new(0);
static LOW: AtomicU32 = AtomicU32::new(0);

struct Page {
    dialog: bool,
}

impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body style="margin: 0">
                <TextArea style="position: absolute; left: 0; top: 0; width: 200px; height: 80px" />
                <Button id="under" style="position: absolute; left: 0; top: 100px; width: 120px; height: 30px"
                    onClick={|| { UNDER.fetch_add(1, Relaxed); }}>"Under"</Button>
                // two Buttons that overlap: the later one in the document has the lower z-index
                <Button id="high" style="position: absolute; left: 250px; top: 100px; width: 100px; height: 30px; z-index: 5"
                    onClick={|| { HIGH.fetch_add(1, Relaxed); }}>"High"</Button>
                <Button id="low" style="position: absolute; left: 300px; top: 100px; width: 100px; height: 30px; z-index: 1"
                    onClick={|| { LOW.fetch_add(1, Relaxed); }}>"Low"</Button>
                {if self.dialog {
                    rsx! {
                        <div id="backdrop" style="position: fixed; inset: 0; background-color: rgb(255, 0, 0); z-index: 1000"
                            onClick={|| { OVER.fetch_add(1, Relaxed); }}>
                            <Button id="ok" style="position: absolute; left: 300px; top: 20px; width: 80px; height: 30px"
                                onClick={|| { INSIDE.fetch_add(1, Relaxed); }}>"OK"</Button>
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

/// Widgets that overlap take the pointer in paint order too: `z-index` among them, not the order they
/// were made in.
#[test]
fn widgets_over_widgets() {
    Application::headless().run(|cx| {
        let h = cx.open_window(WindowOptions::default(), |_, cx| cx.new(|_| Page { dialog: false })).unwrap();
        cx.draw(h.id);
        cx.simulate_click(h.id, 320.0, 115.0); // where they overlap (250..350 and 300..400)
        assert_eq!((HIGH.load(Relaxed), LOW.load(Relaxed)), (1, 0), "the higher z-index gets the click");
        cx.simulate_click(h.id, 380.0, 115.0); // only the low one is there
        assert_eq!((HIGH.load(Relaxed), LOW.load(Relaxed)), (1, 1));
    });
}

#[test]
fn dialog_over_widgets() {
    Application::headless().run(|cx| {
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(400.0), px(200.0)),
            })),
            ..Default::default()
        };
        let h = cx.open_window(opts, |_, cx| cx.new(|_| Page { dialog: false })).unwrap();
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
        assert_eq!((OVER.load(Relaxed), UNDER.load(Relaxed)), (1, 0), "over the Button: the dialog's");
        // the dialog's own Button is above its backdrop
        cx.simulate_click(w, 340.0, 35.0);
        assert_eq!(INSIDE.load(Relaxed), 1, "a Button in the dialog is clickable");
        assert_eq!(OVER.load(Relaxed), 2, "and its click bubbles to the backdrop, as in a browser");
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
        assert_eq!(UNDER.load(Relaxed), 1, "the Button works again");
    });
}
