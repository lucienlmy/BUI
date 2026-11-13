//! Motion's `layout` (FLIP): when an element's box moves or resizes between
//! layouts, it starts where it was seen (an offset, and a scale for the
//! size, about its centre) and animates that offset away. The offsets are
//! four internal properties written on top of the style, like `x` / `scale`.

use blenderui_sys as sys;

use super::element::{Elements, Slot, start};
use super::spec::Layout;
use super::value::{Key, Value};
use crate::geometry::{Bounds, Units};
use crate::layout::{BoxNode, Frame, StyleStore};
use taffy::style::Position;

/// The offsets' properties (`PropState::layout` 1 .. 4).
pub(super) static LAYOUT_X: Key = Key::new(c"__layoutX");
pub(super) static LAYOUT_Y: Key = Key::new(c"__layoutY");
pub(super) static LAYOUT_SX: Key = Key::new(c"__layoutScaleX");
pub(super) static LAYOUT_SY: Key = Key::new(c"__layoutScaleY");

/// The role of property `id` (0: not a layout offset).
pub(super) fn role(id: i32) -> u8 {
    [&LAYOUT_X, &LAYOUT_Y, &LAYOUT_SX, &LAYOUT_SY].iter().position(|k| k.id() == id).map_or(0, |i| i as u8 + 1)
}

impl Elements {
    /// After a layout: the `layout` elements whose box changed start where
    /// they were seen. True when one did (the frame needs its values again).
    pub(super) fn flip(&mut self, frame: &Frame, styles: &StyleStore, viewport: (f32, f32)) -> bool {
        let mut started = false;
        for k in 0..self.els.live.len() {
            let i = self.els.live[k];
            let s = self.els.at(i);
            let Some(mode) = s.gest.as_ref().and_then(|g| g.layout) else { continue };
            let Some(n) = frame.nodes.get(s.node).filter(|n| n.key == s.key) else { continue };
            let now = laid_out(frame, n, styles, viewport);
            let Some(g) = s.gest.as_mut() else { continue };
            let was = g.last_box.replace(now);
            let Some(was) = was.filter(|b| moved(b, &now)) else { continue };
            let transition = std::mem::take(&mut g.layout_t); // put back below: no clone
            let slot: *mut Slot = s;
            // where it's seen now: its offsets (and scales) so far, plus the change
            let scaled = mode != Layout::Position && now.size.width > 0.0 && now.size.height > 0.0;
            let (dx, dy) = if scaled {
                let (a, b) = (center(&was), center(&now));
                (a.0 - b.0, a.1 - b.1)
            } else {
                (was.origin.x - now.origin.x, was.origin.y - now.origin.y)
            };
            // from the change on top of where the offset (or scale) is now
            let go = |key: &Key, change: f32, rest: f32| {
                // SAFETY: a live slot; no other borrow while the engine reads it
                let cur = unsafe { &*slot }.props.iter().find(|p| p.id == key.id()).and_then(|p| p.cur).map_or(rest, |r| r.v[0]);
                let from = if rest == 1.0 { change * cur } else { change + cur };
                let keys = [Value::Num(from), Value::Num(rest)];
                // SAFETY: our handle (the global timeline holds it while it runs)
                unsafe { sys::BK_motion_release(start(slot, key.id(), &keys, &transition)) };
            };
            if mode != Layout::Size {
                go(&LAYOUT_X, dx, 0.0);
                go(&LAYOUT_Y, dy, 0.0);
            }
            if scaled {
                go(&LAYOUT_SX, was.size.width / now.size.width, 1.0);
                go(&LAYOUT_SY, was.size.height / now.size.height, 1.0);
            }
            // SAFETY: a live slot; the engine kept no borrow of it
            if let Some(g) = unsafe { &mut *slot }.gest.as_mut() {
                g.layout_t = transition;
            }
            started = true;
        }
        started
    }
}

/// `n`'s box as its layout alone placed it: relative to its parent's box (what
/// the ancestors' translates, a drag, and the parent's scroll move is the
/// parent's own business), without its own translate. A `position: fixed` box
/// has no parent to follow.
fn laid_out(frame: &Frame, n: &BoxNode, styles: &StyleStore, viewport: (f32, f32)) -> Bounds<f32> {
    let st = &styles[n.style];
    let u = Units { font_size: st.font_size, viewport };
    let (w, h) = (n.bounds.size.width, n.bounds.size.height);
    let (mut x, mut y) = (n.bounds.origin.x - st.translate.0.resolve(&u, w), n.bounds.origin.y - st.translate.1.resolve(&u, h));
    if let Some(p) = n.parent.filter(|_| st.layout.position != Position::Fixed).and_then(|p| frame.nodes.get(p)) {
        // children sit at the parent's box, minus what it scrolled
        x += p.scroll_pos.0 - p.bounds.origin.x;
        y += p.scroll_pos.1 - p.bounds.origin.y;
    }
    Bounds { origin: crate::geometry::point(x, y), size: n.bounds.size }
}

fn center(b: &Bounds<f32>) -> (f32, f32) {
    (b.origin.x + b.size.width * 0.5, b.origin.y + b.size.height * 0.5)
}

/// A change worth animating (half a pixel or more).
fn moved(a: &Bounds<f32>, b: &Bounds<f32>) -> bool {
    (a.origin.x - b.origin.x).abs() >= 0.5 || (a.origin.y - b.origin.y).abs() >= 0.5
        || (a.size.width - b.size.width).abs() >= 0.5 || (a.size.height - b.size.height).abs() >= 0.5
}
