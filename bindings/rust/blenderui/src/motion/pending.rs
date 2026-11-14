//! Imperative motions on elements by selector (`gsap.to("#id", ..)`): they
//! wait here until the next layout finds their elements.

use smallvec::SmallVec;

use super::value::{Key, Value};
use super::vars::{Flavor, Transition};
use crate::layout::ElData;

/// An attribute test: its name, and the value it must have (any, when none).
type Attr = (Box<str>, Option<Box<str>>);

/// One compound selector: tag, `#id`, `.class`es, `[attr]` / `[attr=value]`s,
/// all matching.
#[derive(Debug, Default)]
struct Compound {
    tag: Option<Box<str>>,
    id: Option<Box<str>>,
    classes: SmallVec<[Box<str>; 2]>,
    attrs: SmallVec<[Attr; 1]>,
}

/// The elements of an imperative motion: `"#id"`, `".a.b"`, `"div.card"`,
/// `"[data-x]"`, `"[data-x=\"1\"]"`, a comma-separated list (any matching).
/// Combinators (`a b`, `a > b`) and pseudo-classes aren't supported: parsing
/// says so.
#[derive(Debug, Default)]
pub(crate) struct Selector {
    any: SmallVec<[Compound; 1]>,
    /// as written (for what is said when nothing matches)
    source: Box<str>,
}

/// Why `s` isn't a selector this reads.
fn unsupported(s: &str, why: &str) -> String {
    format!("selector `{s}`: {why} (supported: tag, #id, .class, [attr], [attr=value], and comma-separated lists of those)")
}

/// Whether `c` outside brackets belongs to syntax this doesn't read.
const fn is_unsupported(c: char) -> bool {
    c.is_whitespace() || matches!(c, '>' | '+' | '~' | ':' | '(' | ')' | '|' | '&' | '\\' | '"' | '\'' | '=' | ']')
}

impl Selector {
    /// Check `s` without keeping it.
    pub(crate) fn check(s: &str) -> Result<(), String> {
        Self::parse(s).map(drop)
    }

    pub(crate) fn parse(s: &str) -> Result<Self, String> {
        let any = s.split(',').map(|part| Self::compound(s, part.trim())).collect::<Result<_, _>>()?;
        Ok(Self { any, source: s.into() })
    }

    /// One comma-separated part of `whole`.
    fn compound(whole: &str, part: &str) -> Result<Compound, String> {
        if part.is_empty() {
            return Err(unsupported(whole, "an empty selector"));
        }
        let mut c = Compound::default();
        let mut rest = part;
        let tag_end = rest.find(['#', '.', '[']).unwrap_or(rest.len());
        if let Some(c0) = rest[..tag_end].chars().find(|c| is_unsupported(*c)) {
            return Err(unsupported(whole, &format!("`{c0}` isn't supported")));
        }
        if tag_end > 0 && &rest[..tag_end] != "*" {
            c.tag = Some(rest[..tag_end].into());
        }
        rest = &rest[tag_end..];
        while let Some(kind) = rest.chars().next() {
            if kind == '[' {
                let Some(end) = rest.find(']') else {
                    return Err(unsupported(whole, "a `[` without its `]`"));
                };
                let inner = &rest[1..end];
                if inner.trim().is_empty() {
                    return Err(unsupported(whole, "an empty `[]`"));
                }
                c.attrs.push(match inner.split_once('=') {
                    Some((n, v)) => (n.trim().into(), Some(v.trim().trim_matches(['"', '\'']).into())),
                    None => (inner.trim().into(), None),
                });
                rest = &rest[end + 1..];
                continue;
            }
            let end = rest[1..].find(['#', '.', '[']).map_or(rest.len(), |i| i + 1);
            let name = &rest[1..end];
            if name.is_empty() {
                return Err(unsupported(whole, &format!("`{kind}` without a name")));
            }
            if let Some(c0) = name.chars().find(|c| is_unsupported(*c)) {
                return Err(unsupported(whole, &format!("`{c0}` isn't supported")));
            }
            if kind == '#' {
                c.id = Some(name.into());
            } else {
                c.classes.push(name.into());
            }
            rest = &rest[end..];
        }
        Ok(c)
    }

    pub(crate) fn matches(&self, el: &ElData) -> bool {
        let attr = |n: &str| el.attrs.iter().find(|(k, _)| &**k == n).map(|(_, v)| &**v);
        self.any.iter().any(|c| {
            c.tag.as_deref().is_none_or(|t| t == el.tag)
                && c.id.as_deref().is_none_or(|id| attr("id") == Some(id))
                && c.classes.iter().all(|cl| attr("class").is_some_and(|v| v.split_whitespace().any(|x| x == &**cl)))
                && c.attrs.iter().all(|(n, v)| attr(n).is_some_and(|a| v.as_deref().is_none_or(|v| v == a)))
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
    /// what keeps `timeline` from ending empty until the elements are found
    pub hold: super::Tween,
    pub timeline: super::Tween,
    pub matched: usize,
}

thread_local! {
    pub(super) static PENDING: std::cell::Cell<Vec<Pending>> = const { std::cell::Cell::new(Vec::new()) };
}

thread_local! {
    /// How many imperative motions found no element at their layout.
    static UNMATCHED: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
}

/// How many imperative motions by selector found no element (so did nothing)
/// on this thread: a selector that never matches is a mistake worth seeing.
#[must_use]
pub fn unmatched_selectors() -> u64 {
    UNMATCHED.get()
}

/// A selector this doesn't read (see [`Targets::selector`](super::Targets::selector)):
/// its motion does nothing.
pub(super) fn rejected(why: &str) {
    UNMATCHED.set(UNMATCHED.get() + 1);
    if cfg!(debug_assertions) {
        eprintln!("blenderui motion: {why}; its motion does nothing");
    }
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

/// Whether motion `m` waits for a layout to find its elements.
pub(crate) fn waits(m: *mut blenderui_sys::BKMotion) -> bool {
    PENDING.with(|c| with_pending(c, |v| v.iter().any(|p| p.timeline.raw() == m)))
}

/// After a layout: the waiting motions found their elements (or none), and
/// their timelines start (and take their places in timelines).
pub(crate) fn end_pending() {
    for p in PENDING.with(std::cell::Cell::take) {
        if p.matched == 0 {
            // it waited one layout and found nothing: it never will
            UNMATCHED.set(UNMATCHED.get() + 1);
            if cfg!(debug_assertions) {
                eprintln!("blenderui motion: `{}` matched no element at the next layout; its motion was dropped", p.selector.source);
            }
        }
        p.hold.kill();
    }
    super::tween::run_deferred();
}
