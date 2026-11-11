//! Motion's `useScroll`: how far a window (or a scrolling element) is
//! scrolled, as [`MotionValue`]s: pixels and progress (0 .. 1). Bind them
//! (`style={{ scaleX: s.y_progress }}`) or map them (`use_transform`): they
//! change with the scroll, without rendering anything.

use std::cell::Cell;

use super::MotionValue;
use super::motion_value::use_motion_value;
use super::value::Read;
use crate::layout::Frame;

/// A scroll position: pixels and progress on each axis.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Scroll {
    pub scroll_x: MotionValue,
    pub scroll_y: MotionValue,
    pub x_progress: MotionValue,
    pub y_progress: MotionValue,
}

thread_local! {
    /// The scroll positions followed: (the scrolling element's key, 0: the
    /// window's), updated after each layout.
    static FOLLOWED: Cell<Vec<(u64, Scroll)>> = const { Cell::new(Vec::new()) };
}

#[track_caller]
fn follow(key: u64) -> Scroll {
    let s = Scroll {
        scroll_x: use_motion_value(0.0),
        scroll_y: use_motion_value(0.0),
        x_progress: use_motion_value(0.0),
        y_progress: use_motion_value(0.0),
    };
    FOLLOWED.with(|f| {
        let mut v = f.take();
        if !v.iter().any(|(k, x)| *k == key && *x == s) {
            v.push((key, s));
        }
        f.set(v);
    });
    s
}

/// The window's scroll (Motion's `useScroll()`): the page's, when `<html>`
/// or `<body>` scrolls (`overflow: auto` with a height: a window doesn't
/// scroll its page by itself).
#[track_caller]
pub fn use_scroll() -> Scroll {
    follow(0)
}

/// The scroll of the element with `.id(id)` (Motion's `useScroll({ container })`).
#[track_caller]
pub fn use_element_scroll(id: &str) -> Scroll {
    follow(crate::layout::id_key(id))
}

/// After a layout: the followed scroll positions from `frame`; true when one
/// changed (elements bound to them draw again).
pub(crate) fn update(frame: &Frame) -> bool {
    FOLLOWED.with(|f| {
        let mut v = f.take();
        let mut changed = false;
        v.retain(|(key, s)| {
            if !s.scroll_x.alive() {
                return false; // its component went
            }
            // the window's: the first of <html> / <body> that scrolls
            let node = if *key == 0 {
                frame.nodes.iter().take(2).find(|n| n.scroll_range != (0.0, 0.0))
            } else {
                frame.by_key().get(key).map(|&i| &frame.nodes[i])
            };
            if let Some(n) = node {
                let (pos, range) = (n.scroll_pos, n.scroll_range);
                let p = |a: f32, r: f32| if r > 0.0 { (a / r).clamp(0.0, 1.0) } else { 0.0 };
                for (mv, x) in [(s.scroll_x, pos.0), (s.scroll_y, pos.1), (s.x_progress, p(pos.0, range.0)),
                                (s.y_progress, p(pos.1, range.1))] {
                    changed |= mv.set_quiet(Read { v: [x, 0.0, 0.0, 0.0], n: 1, ..Read::default() });
                }
            }
            true
        });
        f.set(v);
        changed
    })
}
