//! Imperative motions on elements by selector (`gsap.to("#id", ..)`): they
//! wait here until the next layout finds their elements.

use smallvec::SmallVec;

use super::value::{Key, Value};
use super::vars::{Flavor, Transition};
use crate::layout::ElData;

/// One compound selector: tag, `#id`, `.class`es, all matching.
#[derive(Debug, Default)]
struct Compound {
    tag: Option<Box<str>>,
    id: Option<Box<str>>,
    classes: SmallVec<[Box<str>; 2]>,
}

/// The elements of an imperative motion: `"#id"`, `".a.b"`, `"div.card"`, a
/// comma-separated list (any matching).
#[derive(Debug, Default)]
pub(crate) struct Selector(SmallVec<[Compound; 1]>);

impl Selector {
    pub(crate) fn parse(s: &str) -> Selector {
        Selector(s.split(',').map(|part| {
            let mut c = Compound::default();
            let mut rest = part.trim();
            let tag_end = rest.find(['#', '.']).unwrap_or(rest.len());
            if tag_end > 0 && &rest[..tag_end] != "*" {
                c.tag = Some(rest[..tag_end].into());
            }
            rest = &rest[tag_end..];
            while let Some(kind) = rest.chars().next() {
                let end = rest[1..].find(['#', '.']).map_or(rest.len(), |i| i + 1);
                let name: Box<str> = rest[1..end].into();
                if kind == '#' {
                    c.id = Some(name);
                } else {
                    c.classes.push(name);
                }
                rest = &rest[end..];
            }
            c
        }).collect())
    }

    pub(crate) fn matches(&self, el: &ElData) -> bool {
        let attr = |n: &str| el.attrs.iter().find(|(k, _)| &**k == n).map(|(_, v)| &**v);
        self.0.iter().any(|c| {
            c.tag.as_deref().is_none_or(|t| t == el.tag)
                && c.id.as_deref().is_none_or(|id| attr("id") == Some(id))
                && c.classes.iter().all(|cl| attr("class").is_some_and(|v| v.split_whitespace().any(|x| x == &**cl)))
        })
    }
}

/// An imperative motion waiting for its elements (they're found at the
/// next layout, in document order: the stagger order).
pub(crate) struct Pending {
    pub selector: Selector,
    pub props: Vec<(&'static Key, Value)>,
    pub transition: Transition,
    pub from: bool,
    pub flavor: Flavor,
    pub timeline: super::Tween,
    pub matched: usize,
}

thread_local! {
    pub(super) static PENDING: std::cell::Cell<Vec<Pending>> = const { std::cell::Cell::new(Vec::new()) };
}

pub(super) fn with_pending<R>(c: &std::cell::Cell<Vec<Pending>>, f: impl FnOnce(&mut Vec<Pending>) -> R) -> R {
    let mut v = c.take();
    let r = f(&mut v);
    c.set(v);
    r
}

pub(crate) fn queue(p: Pending) {
    PENDING.with(|c| with_pending(c, |v| v.push(p)));
}

/// Whether imperative motions wait for elements (every element is checked).
pub(crate) fn pending() -> bool {
    PENDING.with(|c| with_pending(c, |v| !v.is_empty()))
}

/// After a layout: the waiting motions found their elements (or none).
pub(crate) fn end_pending() {
    let done = PENDING.with(std::cell::Cell::take);
    drop(done);
}
