//! Motion (Framer Motion) in rsx: `<motion.li>` with `initial` / `animate`, spring
//! transitions with a staggered `delay`, `whileHover` / `whileTap`, and `presence`
//! (AnimatePresence) for a panel that animates out before it leaves.
//!
//! ```text
//! cargo run --release --example site_motion
//! ```
//!
//! With `--screenshot` it renders three moments of the headless clock side by side.

#[path = "site/common.rs"]
mod common;

use std::time::Duration;

use blenderui::motion::presence;
use blenderui::prelude::*;

struct Demo {
    open: bool,
}

impl Render for Demo {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let open = self.open;
        rsx! {
            <body style="margin: 0; padding: 14px">
                <ul style="margin: 0; padding: 0; list-style: none; display: flex; gap: 8px">
                    <motion.li style="width: 56px; height: 56px; border-radius: 10px; background: #4772b3"
                        initial={{ opacity: 0, y: 24 }} animate={{ opacity: 1, y: 0 }}
                        whileHover={{ scale: 1.12 }} whileTap={{ scale: 0.94 }}
                        transition={{ type: "spring", stiffness: 400, damping: 22, delay: 0 }} />
                    <motion.li style="width: 56px; height: 56px; border-radius: 10px; background: #9b6fe0"
                        initial={{ opacity: 0, y: 24 }} animate={{ opacity: 1, y: 0 }}
                        whileHover={{ scale: 1.12 }} whileTap={{ scale: 0.94 }}
                        transition={{ type: "spring", stiffness: 400, damping: 22, delay: 0.1 }} />
                    <motion.li style="width: 56px; height: 56px; border-radius: 10px; background: #e8903c"
                        initial={{ opacity: 0, y: 24 }} animate={{ opacity: 1, y: 0 }}
                        whileHover={{ scale: 1.12 }} whileTap={{ scale: 0.94 }}
                        transition={{ type: "spring", stiffness: 400, damping: 22, delay: 0.2 }} />
                </ul>
                <button style="margin-top: 14px"
                    onClick={cx.listener(|this, _, _, cx| { this.open = !this.open; cx.notify(); })}>
                    "Toggle panel"
                </button>
                {...presence(open, || rsx! {
                    <motion.div key="panel" style="margin-top: 14px; height: 40px; border-radius: 8px; background: #3fa66b"
                        initial={{ opacity: 0, y: 12 }} animate={{ opacity: 1, y: 0 }}
                        exit={{ opacity: 0, y: -12 }} transition={{ duration: 0.25, ease: "power2.out" }} />
                })}
            </body>
        }
    }
}

fn main() {
    let Some(path) = common::screenshot_arg() else {
        common::run(260.0, 190.0, "Motion", || Demo { open: true });
        return;
    };
    Application::headless().run(move |cx| {
        let win = cx.open_window(common::options(260.0, 190.0, "Motion"), |_, cx| cx.new(|_| Demo { open: true }))
            .unwrap();
        cx.draw(win.id);
        let mut frames = Vec::new();
        let mut t = 0u64;
        for ms in [80u64, 200, 600] {
            cx.advance_clock(Duration::from_millis(ms - t));
            t = ms;
            frames.push(cx.draw(win.id));
        }
        let (fw, fh, ref first) = frames[0];
        let gap = fw / 40;
        let bg = first[fh / 2 * fw + 1];
        let w = fw * 3 + gap * 2;
        let mut out = vec![bg; w * fh];
        for (i, (_, _, px)) in frames.iter().enumerate() {
            for y in 0..fh {
                let x0 = i * (fw + gap);
                out[y * w + x0..y * w + x0 + fw].copy_from_slice(&px[y * fw..(y + 1) * fw]);
            }
        }
        common::save(&path, w, fh, &out);
    });
}
