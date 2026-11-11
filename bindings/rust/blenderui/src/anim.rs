//! CSS animations (`@keyframes`, `animation-*`) and transitions
//! (`transition-*`).
//!
//! The cascade (`layout.rs`) computes and shares styles as usual; for an
//! element with animations or transitions, [`Animations::apply`] then
//! builds a private copy of its style with the animated values on top (an
//! override layer that is never put in the shared style cache). State lives
//! here per element key: when each animation started, running transitions,
//! and the element's previous style (to notice changes worth a transition).
//!
//! Values interpolate per [`Field`] (a longhand slot of [`Computed`]):
//! lengths and percentages, colors (premultiplied sRGB), numbers, shadows,
//! `translate` / `scale`, `visibility`; anything else flips at 50%.
//!
//! Composition order: cascade, then transitions, then animations (later
//! ones in `animation-name` win); fields set by `!important` declarations
//! keep their cascaded value against animations.

use taffy::prelude::*;
use taffy::style::CompactLength;

use crate::color::Rgba;
use crate::css::{Decl, Prop, Tok};
use crate::geometry::{Length, Units};
use crate::effects::{Filter as FilterFn, TransformFn};
use crate::style::{BorderStyle, Computed, Ctx, KeyMap, Gradient, ImageStyle, LineHeight, Shadow};

// ==========================================================================
// Timing functions

/// An easing function.
#[derive(Clone, Debug, PartialEq)]
pub enum Timing {
    Linear,
    /// `cubic-bezier(x1, y1, x2, y2)` (also `ease`, `ease-in` ...).
    Bezier(f32, f32, f32, f32),
    /// `steps(n, position)`.
    Steps(u32, StepPos),
    /// `linear(...)`: (input, output) points, inputs ascending.
    Points(Box<[(f32, f32)]>),
}

/// The jump position of `steps()`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StepPos {
    Start,
    End,
    None,
    Both,
}

pub const EASE: Timing = Timing::Bezier(0.25, 0.1, 0.25, 1.0);

impl Timing {
    /// Parse one easing function token.
    pub fn parse(t: &Tok) -> Option<Timing> {
        Some(match t {
            Tok::Ident(s) => match s.as_ref() {
                "linear" => Timing::Linear,
                "ease" => EASE,
                "ease-in" => Timing::Bezier(0.42, 0.0, 1.0, 1.0),
                "ease-out" => Timing::Bezier(0.0, 0.0, 0.58, 1.0),
                "ease-in-out" => Timing::Bezier(0.42, 0.0, 0.58, 1.0),
                "step-start" => Timing::Steps(1, StepPos::Start),
                "step-end" => Timing::Steps(1, StepPos::End),
                _ => return None,
            },
            Tok::Func(fx) => {
                let num = |a: &Vec<Tok>| match a.as_slice() {
                    [Tok::Num(v)] => Some(*v),
                    _ => None,
                };
                match fx.name.as_ref() {
                    "cubic-bezier" if fx.args.len() == 4 => {
                        let v: Option<Vec<f32>> = fx.args.iter().map(num).collect();
                        let v = v?;
                        if !(0.0..=1.0).contains(&v[0]) || !(0.0..=1.0).contains(&v[2]) {
                            return None;
                        }
                        Timing::Bezier(v[0], v[1], v[2], v[3])
                    }
                    "steps" => {
                        let n = num(fx.args.first()?)?;
                        let pos = match fx.args.get(1).map(|a| a.as_slice()) {
                            None => StepPos::End,
                            Some([Tok::Ident(p)]) => match p.as_ref() {
                                "jump-start" | "start" => StepPos::Start,
                                "jump-end" | "end" => StepPos::End,
                                "jump-none" => StepPos::None,
                                "jump-both" => StepPos::Both,
                                _ => return None,
                            },
                            _ => return None,
                        };
                        let min = if pos == StepPos::None { 2.0 } else { 1.0 };
                        if n < min || n.fract() != 0.0 {
                            return None;
                        }
                        Timing::Steps(n as u32, pos)
                    }
                    "linear" => linear_points(&fx.args)?,
                    _ => return None,
                }
            }
            _ => return None,
        })
    }

    /// The eased progress for input `x`. `before`: the animation is in its
    /// before phase (matters for `steps()` at a jump).
    pub fn apply(&self, x: f32, before: bool) -> f32 {
        match self {
            Timing::Linear => x,
            Timing::Bezier(x1, y1, x2, y2) => bezier(*x1, *y1, *x2, *y2, x),
            Timing::Steps(n, pos) => {
                let n = *n as f32;
                let mut step = (x * n).floor();
                if matches!(pos, StepPos::Start | StepPos::Both) {
                    step += 1.0;
                }
                if before && (x * n).fract() == 0.0 {
                    step -= 1.0;
                }
                let jumps = match pos {
                    StepPos::Start | StepPos::End => n,
                    StepPos::None => n - 1.0,
                    StepPos::Both => n + 1.0,
                };
                if x >= 0.0 && step < 0.0 {
                    step = 0.0;
                }
                if x <= 1.0 && step > jumps {
                    step = jumps;
                }
                step / jumps
            }
            Timing::Points(p) => {
                if p.is_empty() {
                    return x;
                }
                // the segment holding x (extrapolating past the ends)
                let k = p.iter().rposition(|&(i, _)| i <= x).unwrap_or(0).min(p.len().saturating_sub(2));
                if p.len() == 1 {
                    return p[0].1;
                }
                let ((i0, o0), (i1, o1)) = (p[k], p[k + 1]);
                if i1 == i0 {
                    o1
                } else {
                    o0 + (o1 - o0) * (x - i0) / (i1 - i0)
                }
            }
        }
    }
}

/// `linear(0, 0.25 75%, 1)`: outputs with optional input percentages.
fn linear_points(args: &[Vec<Tok>]) -> Option<Timing> {
    let mut pts: Vec<(Option<f32>, f32)> = Vec::new();
    for a in args {
        let out = match a.first()? {
            Tok::Num(v) => *v,
            _ => return None,
        };
        let ins: Vec<f32> = a[1..].iter().map(|t| match t {
            Tok::Pct(p) => Some(p / 100.0),
            _ => None,
        }).collect::<Option<_>>()?;
        if ins.is_empty() {
            pts.push((None, out));
        }
        for i in ins.into_iter().take(2) {
            pts.push((Some(i), out));
        }
    }
    if pts.len() < 2 {
        return None;
    }
    let n = pts.len();
    if pts[0].0.is_none() {
        pts[0].0 = Some(0.0);
    }
    if pts[n - 1].0.is_none() {
        pts[n - 1].0 = Some(1.0);
    }
    // inputs never go backwards; runs without one are spread evenly
    let mut last = 0.0f32;
    for p in pts.iter_mut() {
        if let Some(i) = &mut p.0 {
            *i = i.max(last);
            last = *i;
        }
    }
    let mut k = 0;
    while k < n {
        if pts[k].0.is_some() {
            k += 1;
            continue;
        }
        let start = k - 1;
        let end = (k..n).find(|&j| pts[j].0.is_some()).unwrap_or(n - 1);
        let (a, b) = (pts[start].0.unwrap_or(0.0), pts[end].0.unwrap_or(1.0));
        for j in k..end {
            pts[j].0 = Some(a + (b - a) * (j - start) as f32 / (end - start) as f32);
        }
        k = end;
    }
    Some(Timing::Points(pts.into_iter().map(|(i, o)| (i.unwrap_or(0.0), o)).collect()))
}

/// Solve a CSS cubic Bézier for y at x (Newton, then bisection).
fn bezier(x1: f32, y1: f32, x2: f32, y2: f32, x: f32) -> f32 {
    let (x1, y1, x2, y2, x) = (x1 as f64, y1 as f64, x2 as f64, y2 as f64, x as f64);
    // outside 0..1 the curve extends along its end tangents
    if x <= 0.0 {
        let slope = if x1 > 0.0 { y1 / x1 } else if y1 == 0.0 && x2 > 0.0 { y2 / x2 } else { 0.0 };
        return (slope * x) as f32;
    }
    if x >= 1.0 {
        let slope = if x2 < 1.0 { (y2 - 1.0) / (x2 - 1.0) } else if y2 == 1.0 && x1 < 1.0 { (y1 - 1.0) / (x1 - 1.0) } else { 0.0 };
        return (1.0 + slope * (x - 1.0)) as f32;
    }
    let (cx, cy) = (3.0 * x1, 3.0 * y1);
    let (bx, by) = (3.0 * (x2 - x1) - cx, 3.0 * (y2 - y1) - cy);
    let (ax, ay) = (1.0 - cx - bx, 1.0 - cy - by);
    let sx = |t: f64| ((ax * t + bx) * t + cx) * t;
    let sy = |t: f64| ((ay * t + by) * t + cy) * t;
    let dx = |t: f64| (3.0 * ax * t + 2.0 * bx) * t + cx;
    let mut t = x;
    for _ in 0..8 {
        let e = sx(t) - x;
        if e.abs() < 1e-7 {
            return sy(t) as f32;
        }
        let d = dx(t);
        if d.abs() < 1e-6 {
            break;
        }
        t -= e / d;
    }
    let (mut lo, mut hi) = (0.0, 1.0);
    t = x;
    for _ in 0..60 {
        let v = sx(t);
        if (v - x).abs() < 1e-7 {
            break;
        }
        if v < x {
            lo = t;
        } else {
            hi = t;
        }
        t = (lo + hi) / 2.0;
    }
    sy(t) as f32
}

// ==========================================================================
// The animation-* / transition-* properties

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Direction {
    #[default]
    Normal,
    Reverse,
    Alternate,
    AlternateReverse,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Fill {
    #[default]
    None,
    Forwards,
    Backwards,
    Both,
}

/// The `animation-*` and `transition-*` longhands of one element (lists:
/// value `i` of a shorter list is `list[i % len]`). Not inherited.
#[derive(Clone, Debug, PartialEq, Default)]
pub struct AnimSpec {
    pub names: Vec<Box<str>>,
    pub durations: Vec<f32>,
    pub timings: Vec<Timing>,
    pub delays: Vec<f32>,
    /// `f32::INFINITY` for `infinite`.
    pub iterations: Vec<f32>,
    pub directions: Vec<Direction>,
    pub fills: Vec<Fill>,
    /// true = paused.
    pub paused: Vec<bool>,
    /// `transition-property` (`all`, `none` or a property name).
    pub t_props: Vec<Box<str>>,
    pub t_durations: Vec<f32>,
    pub t_timings: Vec<Timing>,
    pub t_delays: Vec<f32>,
    /// `transition-behavior: allow-discrete`.
    pub t_discrete: Vec<bool>,
}

fn nth<T: Clone>(list: &[T], i: usize, default: T) -> T {
    if list.is_empty() { default } else { list[i % list.len()].clone() }
}

impl AnimSpec {
    fn has_animations(&self) -> bool {
        self.names.iter().any(|n| &**n != "none")
    }

    fn has_transitions(&self) -> bool {
        (0..self.t_props.len().max(1)).any(|i| {
            nth(&self.t_durations, i, 0.0).max(0.0) + nth(&self.t_delays, i, 0.0) > 0.0
                && &*nth(&self.t_props, i, "all".into()) != "none"
        })
    }

    /// Duration, delay, easing and allow-discrete of the transition for
    /// `field` (None: not transitioned).
    fn transition_for(&self, field: Field) -> Option<(f32, f32, Timing, bool)> {
        let n = self.t_props.len().max(1);
        // the last matching entry wins
        (0..n).rev().find(|&i| {
            let p = nth(&self.t_props, i, "all".into());
            match &*p {
                "all" => ALL_FIELDS.contains(&field),
                "none" => false,
                name => fields_of(Prop::lookup(name)).contains(&field),
            }
        }).map(|i| (nth(&self.t_durations, i, 0.0).max(0.0), nth(&self.t_delays, i, 0.0),
                    nth(&self.t_timings, i, EASE), nth(&self.t_discrete, i, false)))
    }

    /// The fields that may transition.
    fn watched(&self) -> Vec<Field> {
        let mut out = Vec::new();
        for i in 0..self.t_props.len().max(1) {
            let p = nth(&self.t_props, i, "all".into());
            match &*p {
                "all" => out.extend_from_slice(ALL_FIELDS),
                "none" => {}
                name => out.extend(fields_of(Prop::lookup(name)).iter().copied()),
            }
        }
        out.sort_unstable_by_key(|f| field_rank(*f));
        out.dedup();
        out
    }
}

fn spec<'c>(c: &'c mut Ctx) -> &'c mut AnimSpec {
    c.c.anim.get_or_insert_with(Default::default)
}

fn items(v: &[Tok]) -> impl Iterator<Item = &[Tok]> {
    v.split(|t| matches!(t, Tok::Comma))
}

fn time(t: &Tok) -> Option<f32> {
    match t {
        Tok::Dim(v, u) if u == "s" => Some(*v),
        Tok::Dim(v, u) if u == "ms" => Some(*v / 1000.0),
        Tok::Num(v) if *v == 0.0 => Some(0.0),
        _ => None,
    }
}

fn list<T>(v: &[Tok], one: impl Fn(&[Tok]) -> Option<T>) -> Option<Vec<T>> {
    items(v).map(one).collect()
}

fn single<T>(f: impl Fn(&Tok) -> Option<T>) -> impl Fn(&[Tok]) -> Option<T> {
    move |item| match item {
        [t] => f(t),
        _ => None,
    }
}

fn direction(s: &str) -> Option<Direction> {
    Some(match s {
        "normal" => Direction::Normal,
        "reverse" => Direction::Reverse,
        "alternate" => Direction::Alternate,
        "alternate-reverse" => Direction::AlternateReverse,
        _ => return None,
    })
}

fn fill(s: &str) -> Option<Fill> {
    Some(match s {
        "none" => Fill::None,
        "forwards" => Fill::Forwards,
        "backwards" => Fill::Backwards,
        "both" => Fill::Both,
        _ => return None,
    })
}

fn iteration(t: &Tok) -> Option<f32> {
    match t {
        Tok::Num(v) if *v >= 0.0 => Some(*v),
        Tok::Ident(s) if s == "infinite" => Some(f32::INFINITY),
        _ => None,
    }
}

fn name(t: &Tok) -> Option<Box<str>> {
    match t {
        Tok::Ident(s) => Some(s.as_ref().into()),
        Tok::Str(s) => Some(s.as_ref().into()),
        _ => None,
    }
}

pub(crate) fn h_animation(c: &mut Ctx, v: &[Tok]) -> bool {
    let s = spec(c);
    // the shorthand resets every animation longhand, keeps the transition ones
    let mut out = AnimSpec {
        t_props: std::mem::take(&mut s.t_props),
        t_durations: std::mem::take(&mut s.t_durations),
        t_timings: std::mem::take(&mut s.t_timings),
        t_delays: std::mem::take(&mut s.t_delays),
        t_discrete: std::mem::take(&mut s.t_discrete),
        ..Default::default()
    };
    for item in items(v) {
        let (mut dur, mut delay, mut timing, mut iter, mut dir, mut fl, mut paused, mut nm) =
            (None, None, None, None, None, None, None, None);
        for t in item {
            if let Some(x) = time(t) {
                if dur.is_none() {
                    dur = Some(x);
                } else if delay.is_none() {
                    delay = Some(x);
                } else {
                    return false;
                }
            } else if let Some(x) = Timing::parse(t).filter(|_| timing.is_none()) {
                timing = Some(x);
            } else if let Some(x) = iteration(t).filter(|_| iter.is_none()) {
                iter = Some(x);
            } else if let Some(x) = t.ident().and_then(direction).filter(|_| dir.is_none()) {
                dir = Some(x);
            } else if let Some(x) = t.ident().filter(|s| *s != "none").and_then(fill).filter(|_| fl.is_none()) {
                fl = Some(x);
            } else if let Some(x) = t.ident().and_then(|s| match s {
                "running" => Some(false),
                "paused" => Some(true),
                _ => None,
            }).filter(|_| paused.is_none()) {
                paused = Some(x);
            } else if let Some(x) = name(t).filter(|_| nm.is_none()) {
                nm = Some(x);
            } else {
                return false;
            }
        }
        out.names.push(nm.unwrap_or_else(|| "none".into()));
        out.durations.push(dur.unwrap_or(0.0).max(0.0));
        out.delays.push(delay.unwrap_or(0.0));
        out.timings.push(timing.unwrap_or(EASE));
        out.iterations.push(iter.unwrap_or(1.0));
        out.directions.push(dir.unwrap_or_default());
        out.fills.push(fl.unwrap_or_default());
        out.paused.push(paused.unwrap_or(false));
    }
    *spec(c) = out;
    true
}

pub(crate) fn h_transition(c: &mut Ctx, v: &[Tok]) -> bool {
    let (mut props, mut durs, mut timings, mut delays, mut discrete) = (vec![], vec![], vec![], vec![], vec![]);
    for item in items(v) {
        let (mut prop, mut dur, mut delay, mut timing, mut disc) = (None, None, None, None, None);
        for t in item {
            if let Some(x) = time(t) {
                if dur.is_none() {
                    dur = Some(x);
                } else if delay.is_none() {
                    delay = Some(x);
                } else {
                    return false;
                }
            } else if let Some(x) = Timing::parse(t).filter(|_| timing.is_none()) {
                timing = Some(x);
            } else if let Some(x) = t.ident().and_then(|s| match s {
                "allow-discrete" => Some(true),
                "normal" => Some(false),
                _ => None,
            }).filter(|_| disc.is_none()) {
                disc = Some(x);
            } else if let Some(x) = t.ident().filter(|_| prop.is_none()) {
                prop = Some(Box::<str>::from(x));
            } else {
                return false;
            }
        }
        props.push(prop.unwrap_or_else(|| "all".into()));
        durs.push(dur.unwrap_or(0.0).max(0.0));
        delays.push(delay.unwrap_or(0.0));
        timings.push(timing.unwrap_or(EASE));
        discrete.push(disc.unwrap_or(false));
    }
    let s = spec(c);
    (s.t_props, s.t_durations, s.t_timings, s.t_delays, s.t_discrete) = (props, durs, timings, delays, discrete);
    true
}

macro_rules! longhand {
    ($fn:ident, $field:ident, $parse:expr) => {
        pub(crate) fn $fn(c: &mut Ctx, v: &[Tok]) -> bool {
            match list(v, $parse) {
                Some(l) => {
                    spec(c).$field = l;
                    true
                }
                None => false,
            }
        }
    };
}

longhand!(h_animation_name, names, single(name));
longhand!(h_animation_duration, durations, single(|t| time(t).filter(|v| *v >= 0.0)));
longhand!(h_animation_timing, timings, single(Timing::parse));
longhand!(h_animation_delay, delays, single(time));
longhand!(h_animation_iterations, iterations, single(iteration));
longhand!(h_animation_direction, directions, single(|t: &Tok| t.ident().and_then(direction)));
longhand!(h_animation_fill, fills, single(|t: &Tok| t.ident().and_then(fill)));
longhand!(h_animation_play_state, paused, single(|t: &Tok| match t.ident() {
    Some("running") => Some(false),
    Some("paused") => Some(true),
    _ => None,
}));
longhand!(h_transition_property, t_props, single(|t: &Tok| t.ident().map(Box::<str>::from)));
longhand!(h_transition_duration, t_durations, single(|t| time(t).filter(|v| *v >= 0.0)));
longhand!(h_transition_timing, t_timings, single(Timing::parse));
longhand!(h_transition_delay, t_delays, single(time));
longhand!(h_transition_behavior, t_discrete, single(|t: &Tok| match t.ident() {
    Some("allow-discrete") => Some(true),
    Some("normal") => Some(false),
    _ => None,
}));

// ==========================================================================
// Fields: what interpolates, and how

/// One animatable slot of [`Computed`] (a longhand, or a few that only
/// make sense together).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum Field {
    Width,
    Height,
    MinWidth,
    MinHeight,
    MaxWidth,
    MaxHeight,
    FlexBasis,
    FlexGrow,
    FlexShrink,
    Order,
    Margin(u8),
    Padding(u8),
    Inset(u8),
    RowGap,
    ColumnGap,
    BorderWidth(u8),
    BorderColor(u8),
    /// `border-*-style` (with whether the side is drawn): discrete.
    BorderStyle(u8),
    Radius(u8),
    FontSize,
    FontWeight,
    LetterSpacing,
    WordSpacing,
    LineHeight,
    TextIndent,
    OutlineWidth,
    OutlineColor,
    OutlineOffset,
    OutlineStyle,
    Background,
    /// Gradient and image placement: discrete.
    BackgroundImage,
    Color,
    DecorationColor,
    Accent,
    Opacity,
    Visibility,
    ZIndex,
    /// `transform` / `translate`: translations stay a layout offset;
    /// `Transform` is the rest of the function list (rotate, skew, scale,
    /// matrix), interpolated function by function.
    Translate,
    Transform,
    /// The individual `rotate` and `scale` properties.
    Rotate,
    Scale,
    /// `filter` / `backdrop-filter` lists, function by function.
    Filter,
    Backdrop,
    BoxShadow,
    TextShadow,
    /// Any other property: applied as declared, flipping at 50%.
    Other(Prop),
}

/// What `transition-property: all` covers.
const ALL_FIELDS: &[Field] = &{
    use Field::*;
    [Width, Height, MinWidth, MinHeight, MaxWidth, MaxHeight, FlexBasis, FlexGrow, FlexShrink, Order,
     Margin(0), Margin(1), Margin(2), Margin(3), Padding(0), Padding(1), Padding(2), Padding(3),
     Inset(0), Inset(1), Inset(2), Inset(3), RowGap, ColumnGap,
     BorderWidth(0), BorderWidth(1), BorderWidth(2), BorderWidth(3),
     BorderColor(0), BorderColor(1), BorderColor(2), BorderColor(3),
     BorderStyle(0), BorderStyle(1), BorderStyle(2), BorderStyle(3),
     Radius(0), Radius(1), Radius(2), Radius(3),
     FontSize, FontWeight, LetterSpacing, WordSpacing, LineHeight, TextIndent,
     OutlineWidth, OutlineColor, OutlineOffset, OutlineStyle, Background, BackgroundImage, Color,
     DecorationColor, Accent, Opacity, Visibility, ZIndex, Translate, Transform, Rotate, Scale, Filter, Backdrop,
     BoxShadow, TextShadow]
};

fn field_rank(f: Field) -> usize {
    match f {
        Field::Other(p) => 1000 + p.0 as usize,
        f => ALL_FIELDS.iter().position(|x| *x == f).unwrap_or(999),
    }
}

/// The fields a property sets (an `Other` for properties without one).
pub(crate) fn fields_of(p: Prop) -> &'static [Field] {
    use Field::*;
    macro_rules! sides {
        ($f:ident) => {
            &[$f(0), $f(1), $f(2), $f(3)]
        };
    }
    match p.name() {
        "width" => &[Width],
        "height" => &[Height],
        "min-width" => &[MinWidth],
        "min-height" => &[MinHeight],
        "max-width" => &[MaxWidth],
        "max-height" => &[MaxHeight],
        "flex-basis" => &[FlexBasis],
        "flex-grow" => &[FlexGrow],
        "flex-shrink" => &[FlexShrink],
        "flex" => &[FlexGrow, FlexShrink, FlexBasis],
        "order" => &[Order],
        "margin" => sides!(Margin),
        "margin-top" => &[Margin(0)],
        "margin-right" => &[Margin(1)],
        "margin-bottom" => &[Margin(2)],
        "margin-left" => &[Margin(3)],
        "padding" => sides!(Padding),
        "padding-top" => &[Padding(0)],
        "padding-right" => &[Padding(1)],
        "padding-bottom" => &[Padding(2)],
        "padding-left" => &[Padding(3)],
        "inset" => sides!(Inset),
        "top" => &[Inset(0)],
        "right" => &[Inset(1)],
        "bottom" => &[Inset(2)],
        "left" => &[Inset(3)],
        "gap" | "grid-gap" => &[RowGap, ColumnGap],
        "row-gap" | "grid-row-gap" => &[RowGap],
        "column-gap" | "grid-column-gap" => &[ColumnGap],
        "border-width" => sides!(BorderWidth),
        "border-top-width" => &[BorderWidth(0)],
        "border-right-width" => &[BorderWidth(1)],
        "border-bottom-width" => &[BorderWidth(2)],
        "border-left-width" => &[BorderWidth(3)],
        "border-color" => sides!(BorderColor),
        "border-top-color" => &[BorderColor(0)],
        "border-right-color" => &[BorderColor(1)],
        "border-bottom-color" => &[BorderColor(2)],
        "border-left-color" => &[BorderColor(3)],
        "border-style" => sides!(BorderStyle),
        "border-top-style" => &[BorderStyle(0)],
        "border-right-style" => &[BorderStyle(1)],
        "border-bottom-style" => &[BorderStyle(2)],
        "border-left-style" => &[BorderStyle(3)],
        "border" => &[BorderWidth(0), BorderWidth(1), BorderWidth(2), BorderWidth(3),
                      BorderColor(0), BorderColor(1), BorderColor(2), BorderColor(3),
                      BorderStyle(0), BorderStyle(1), BorderStyle(2), BorderStyle(3)],
        "border-top" => &[BorderWidth(0), BorderColor(0), BorderStyle(0)],
        "border-right" => &[BorderWidth(1), BorderColor(1), BorderStyle(1)],
        "border-bottom" => &[BorderWidth(2), BorderColor(2), BorderStyle(2)],
        "border-left" => &[BorderWidth(3), BorderColor(3), BorderStyle(3)],
        "border-radius" => sides!(Radius),
        "border-top-left-radius" => &[Radius(0)],
        "border-top-right-radius" => &[Radius(1)],
        "border-bottom-right-radius" => &[Radius(2)],
        "border-bottom-left-radius" => &[Radius(3)],
        "font-size" => &[FontSize],
        "font-weight" => &[FontWeight],
        "letter-spacing" => &[LetterSpacing],
        "word-spacing" => &[WordSpacing],
        "line-height" => &[LineHeight],
        "text-indent" => &[TextIndent],
        "outline" => &[OutlineWidth, OutlineColor, OutlineStyle],
        "outline-width" => &[OutlineWidth],
        "outline-color" => &[OutlineColor],
        "outline-offset" => &[OutlineOffset],
        "outline-style" => &[OutlineStyle],
        "background-color" => &[Background],
        "background" => &[Background, BackgroundImage],
        "color" => &[Color],
        "text-decoration-color" => &[DecorationColor],
        "accent-color" => &[Accent],
        "opacity" => &[Opacity],
        "visibility" => &[Visibility],
        "z-index" => &[ZIndex],
        "transform" => &[Translate, Transform],
        "translate" => &[Translate],
        "rotate" => &[Rotate],
        "scale" => &[Scale],
        "filter" => &[Filter],
        "backdrop-filter" | "-webkit-backdrop-filter" => &[Backdrop],
        "box-shadow" => &[BoxShadow],
        "text-shadow" => &[TextShadow],
        _ => &[],
    }
}

/// A field's value.
#[derive(Clone, Debug, PartialEq)]
enum Val {
    Dim(Dimension),
    Lpa(LengthPercentageAuto),
    Lp(LengthPercentage),
    Num(f32),
    Int(i32),
    Color(Rgba),
    OptColor(Option<Rgba>),
    Len(Length),
    Len2(Length, Length),
    Shadows(Vec<Shadow>),
    TextShadow(Option<Shadow>),
    Visible(bool),
    Z(Option<i32>),
    Lh(LineHeight),
    Border(BorderStyle, bool),
    Outline(BorderStyle),
    Bg(Option<Gradient>, Option<Box<ImageStyle>>),
    Pair(f32, f32),
    Fns(Vec<TransformFn>),
    Filters(Vec<FilterFn>),
}

fn side<T: Copy>(r: &taffy::geometry::Rect<T>, s: u8) -> T {
    match s {
        0 => r.top,
        1 => r.right,
        2 => r.bottom,
        _ => r.left,
    }
}

fn side_mut<T>(r: &mut taffy::geometry::Rect<T>, s: u8) -> &mut T {
    match s {
        0 => &mut r.top,
        1 => &mut r.right,
        2 => &mut r.bottom,
        _ => &mut r.left,
    }
}

fn get(f: Field, c: &Computed) -> Option<Val> {
    use Field::*;
    let l = &c.layout;
    Some(match f {
        Width => Val::Dim(l.size.width),
        Height => Val::Dim(l.size.height),
        MinWidth => Val::Lpa(l.min_size.width),
        MinHeight => Val::Lpa(l.min_size.height),
        MaxWidth => Val::Lpa(l.max_size.width),
        MaxHeight => Val::Lpa(l.max_size.height),
        FlexBasis => Val::Dim(l.flex_basis),
        FlexGrow => Val::Num(l.flex_grow),
        FlexShrink => Val::Num(l.flex_shrink),
        Order => Val::Int(c.order),
        Margin(s) => Val::Lpa(side(&l.margin, s)),
        Padding(s) => Val::Lp(side(&l.padding, s)),
        Inset(s) => Val::Lpa(side(&l.inset, s)),
        RowGap => Val::Lp(l.gap.height),
        ColumnGap => Val::Lp(l.gap.width),
        BorderWidth(s) => Val::Lp(side(&l.border, s)),
        BorderColor(s) => Val::Color(c.border_color[s as usize]),
        BorderStyle(s) => Val::Border(c.border_style[s as usize], c.border_drawn[s as usize]),
        Radius(s) => Val::Len(c.radius[s as usize]),
        FontSize => Val::Num(c.font_size),
        FontWeight => Val::Int(c.weight as i32),
        LetterSpacing => Val::Num(c.letter_spacing),
        WordSpacing => Val::Num(c.word_spacing),
        LineHeight => Val::Lh(c.line_height),
        TextIndent => Val::Len(c.text_indent),
        OutlineWidth => Val::Num(c.outline.0),
        OutlineColor => Val::Color(c.outline.1),
        OutlineOffset => Val::Num(c.outline.2),
        OutlineStyle => Val::Outline(c.outline_style),
        Background => Val::Color(c.background),
        BackgroundImage => Val::Bg(c.gradient.clone(), c.images.clone()),
        Color => Val::Color(c.color),
        DecorationColor => Val::OptColor(c.decoration_color),
        Accent => Val::OptColor(c.accent),
        Opacity => Val::Num(c.opacity),
        Visibility => Val::Visible(c.visible),
        ZIndex => Val::Z(c.z_index),
        Translate => Val::Len2(c.translate.0, c.translate.1),
        Transform => Val::Fns(c.effects.as_ref().map(|e| e.transform.clone()).unwrap_or_default()),
        Rotate => Val::Num(c.effects.as_ref().map_or(0.0, |e| e.rotate)),
        Scale => {
            let (x, y) = c.effects.as_ref().and_then(|e| e.scale2).unwrap_or((1.0, 1.0));
            Val::Pair(x, y)
        }
        Filter => Val::Filters(c.effects.as_ref().map(|e| e.filter.clone()).unwrap_or_default()),
        Backdrop => Val::Filters(c.effects.as_ref().map(|e| e.backdrop.clone()).unwrap_or_default()),
        BoxShadow => Val::Shadows(c.shadows.clone()),
        TextShadow => Val::TextShadow(c.text_shadow),
        Other(_) => return None,
    })
}

fn set(f: Field, c: &mut Computed, v: Val) {
    use Field::*;
    let l = &mut c.layout;
    match (f, v) {
        (Width, Val::Dim(d)) => l.size.width = d,
        (Height, Val::Dim(d)) => l.size.height = d,
        (MinWidth, Val::Lpa(d)) => l.min_size.width = d,
        (MinHeight, Val::Lpa(d)) => l.min_size.height = d,
        (MaxWidth, Val::Lpa(d)) => l.max_size.width = d,
        (MaxHeight, Val::Lpa(d)) => l.max_size.height = d,
        (FlexBasis, Val::Dim(d)) => l.flex_basis = d,
        (FlexGrow, Val::Num(n)) => l.flex_grow = n.max(0.0),
        (FlexShrink, Val::Num(n)) => l.flex_shrink = n.max(0.0),
        (Order, Val::Int(n)) => c.order = n,
        (Margin(s), Val::Lpa(d)) => *side_mut(&mut l.margin, s) = d,
        (Padding(s), Val::Lp(d)) => *side_mut(&mut l.padding, s) = d,
        (Inset(s), Val::Lpa(d)) => *side_mut(&mut l.inset, s) = d,
        (RowGap, Val::Lp(d)) => l.gap.height = d,
        (ColumnGap, Val::Lp(d)) => l.gap.width = d,
        (BorderWidth(s), Val::Lp(d)) => *side_mut(&mut l.border, s) = d,
        (BorderColor(s), Val::Color(x)) => c.border_color[s as usize] = x,
        (BorderStyle(s), Val::Border(st, drawn)) => {
            c.border_style[s as usize] = st;
            c.border_drawn[s as usize] = drawn;
        }
        (Radius(s), Val::Len(x)) => c.radius[s as usize] = x,
        (FontSize, Val::Num(n)) => c.font_size = n.max(0.0),
        (FontWeight, Val::Int(n)) => c.weight = n.clamp(1, 1000) as u16,
        (LetterSpacing, Val::Num(n)) => c.letter_spacing = n,
        (WordSpacing, Val::Num(n)) => c.word_spacing = n,
        (LineHeight, Val::Lh(x)) => c.line_height = x,
        (TextIndent, Val::Len(x)) => c.text_indent = x,
        (OutlineWidth, Val::Num(n)) => c.outline.0 = n.max(0.0),
        (OutlineColor, Val::Color(x)) => c.outline.1 = x,
        (OutlineOffset, Val::Num(n)) => c.outline.2 = n,
        (OutlineStyle, Val::Outline(x)) => c.outline_style = x,
        (Background, Val::Color(x)) => c.background = x,
        (BackgroundImage, Val::Bg(g, i)) => {
            c.gradient = g;
            c.images = i;
        }
        (Color, Val::Color(x)) => c.color = x,
        (DecorationColor, Val::OptColor(x)) => c.decoration_color = x,
        (Accent, Val::OptColor(x)) => c.accent = x,
        (Opacity, Val::Num(n)) => c.opacity = n.clamp(0.0, 1.0),
        (Visibility, Val::Visible(x)) => c.visible = x,
        (ZIndex, Val::Z(x)) => c.z_index = x,
        (Translate, Val::Len2(x, y)) => c.translate = (x, y),
        (Transform, Val::Fns(x)) => {
            if !x.is_empty() || c.effects.is_some() {
                c.effects_mut().transform = x;
            }
        }
        (Rotate, Val::Num(n)) => {
            if n != 0.0 || c.effects.is_some() {
                c.effects_mut().rotate = n;
            }
        }
        (Scale, Val::Pair(x, y)) => {
            if (x, y) != (1.0, 1.0) || c.effects.is_some() {
                c.effects_mut().scale2 = ((x, y) != (1.0, 1.0)).then_some((x, y));
            }
        }
        (Filter, Val::Filters(x)) => {
            if !x.is_empty() || c.effects.is_some() {
                c.effects_mut().filter = x;
            }
        }
        (Backdrop, Val::Filters(x)) => {
            if !x.is_empty() || c.effects.is_some() {
                c.effects_mut().backdrop = x;
            }
        }
        (BoxShadow, Val::Shadows(x)) => c.shadows = x,
        (TextShadow, Val::TextShadow(x)) => c.text_shadow = x,
        _ => {}
    }
}

/// A field as numbers (Motion / GSAP values): lengths in px (percentages as
/// they are, `percent` set), numbers, colors as 4. None for anything else.
pub(crate) fn field_numbers(f: Field, c: &Computed, u: &Units) -> Option<([f32; 4], usize, bool)> {
    let one = |v: f32| Some(([v, 0.0, 0.0, 0.0], 1, false));
    let compact = |x: CompactLength| match x.tag() {
        CompactLength::LENGTH_TAG => one(x.value()),
        CompactLength::PERCENT_TAG => Some(([x.value() * 100.0, 0.0, 0.0, 0.0], 1, true)),
        _ => None,
    };
    match get(f, c)? {
        Val::Dim(d) => compact(d.into_raw()),
        Val::Lpa(d) => compact(d.into_raw()),
        Val::Lp(d) => compact(d.into_raw()),
        Val::Num(n) => one(n),
        Val::Int(n) => one(n as f32),
        Val::Len(l) => one(l.to_px(u)?),
        Val::Color(x) | Val::OptColor(Some(x)) => Some(([x.r, x.g, x.b, x.a], 4, false)),
        _ => None,
    }
}

/// Field `f` of `to` as in `from`.
pub(crate) fn copy_field(f: Field, from: &Computed, to: &mut Computed) {
    if let Some(v) = get(f, from) {
        set(f, to, v);
    }
}

/// Write numbers to a field in the shape it has (the inverse of [`field_numbers`]).
pub(crate) fn set_field_numbers(f: Field, c: &mut Computed, v: &[f32], percent: bool) {
    let Some(shape) = get(f, c) else { return };
    let x = v.first().copied().unwrap_or(0.0);
    let val = match shape {
        Val::Dim(_) if percent => Val::Dim(Dimension::percent(x / 100.0)),
        Val::Dim(_) => Val::Dim(Dimension::length(x)),
        Val::Lpa(_) if percent => Val::Lpa(LengthPercentageAuto::percent(x / 100.0)),
        Val::Lpa(_) => Val::Lpa(LengthPercentageAuto::length(x)),
        Val::Lp(_) if percent => Val::Lp(LengthPercentage::percent(x / 100.0)),
        Val::Lp(_) => Val::Lp(LengthPercentage::length(x)),
        Val::Num(_) => Val::Num(x),
        Val::Int(_) => Val::Int(x.round() as i32),
        Val::Len(_) => Val::Len(if percent { Length::Percent(x) } else { Length::Px(x) }),
        Val::Color(_) | Val::OptColor(_) if v.len() >= 4 => {
            let rgba = Rgba { r: v[0], g: v[1], b: v[2], a: v[3] };
            if matches!(shape, Val::Color(_)) { Val::Color(rgba) } else { Val::OptColor(Some(rgba)) }
        }
        _ => return,
    };
    set(f, c, val);
}

fn mix(a: f32, b: f32, t: f32) -> f32 {
    a + (b - a) * t
}

/// Colors mix in premultiplied sRGB, as browsers do.
fn mix_color(a: Rgba, b: Rgba, t: f32) -> Rgba {
    let alpha = mix(a.a, b.a, t).clamp(0.0, 1.0);
    if alpha <= 0.0 {
        return Rgba::TRANSPARENT;
    }
    let ch = |x: f32, y: f32| (mix(x * a.a, y * b.a, t) / alpha).clamp(0.0, 1.0);
    Rgba { r: ch(a.r, b.r), g: ch(a.g, b.g), b: ch(a.b, b.b), a: alpha }
}

/// Two compact lengths (`Dimension`, `LengthPercentage(Auto)`): both pixels
/// or both percentages mix; anything else (auto, keywords, calc) doesn't.
fn mix_compact(a: CompactLength, b: CompactLength, t: f32) -> Option<(bool, f32)> {
    let (ta, tb) = (a.tag(), b.tag());
    if ta == CompactLength::LENGTH_TAG && tb == CompactLength::LENGTH_TAG {
        Some((false, mix(a.value(), b.value(), t)))
    } else if ta == CompactLength::PERCENT_TAG && tb == CompactLength::PERCENT_TAG {
        Some((true, mix(a.value(), b.value(), t)))
    } else {
        None
    }
}

fn mix_length(a: Length, b: Length, t: f32, u: &Units) -> Option<Length> {
    match (a, b) {
        (Length::Percent(x), Length::Percent(y)) => Some(Length::Percent(mix(x, y, t))),
        _ => Some(Length::Px(mix(a.to_px(u)?, b.to_px(u)?, t))),
    }
}

fn mix_shadow(a: &Shadow, b: &Shadow, t: f32) -> Option<Shadow> {
    (a.inset == b.inset).then(|| Shadow {
        x: mix(a.x, b.x, t),
        y: mix(a.y, b.y, t),
        blur: mix(a.blur, b.blur, t).max(0.0),
        spread: mix(a.spread, b.spread, t),
        color: mix_color(a.color, b.color, t),
        inset: a.inset,
    })
}

/// The identity function of the same kind (CSS: `none` interpolates
/// against a list of identities).
fn identity_fn(f: &TransformFn) -> TransformFn {
    match f {
        TransformFn::Translate(..) => TransformFn::Translate(Length::Px(0.0), Length::Px(0.0)),
        TransformFn::Scale(..) => TransformFn::Scale(1.0, 1.0),
        TransformFn::Rotate(_) => TransformFn::Rotate(0.0),
        TransformFn::Skew(..) => TransformFn::Skew(0.0, 0.0),
        TransformFn::Matrix(_) => TransformFn::Matrix(crate::effects::IDENTITY),
    }
}

/// Transform lists, function by function (the shorter padded with
/// identities); None when the kinds differ (the value then flips at 50%).
fn mix_transforms(a: &[TransformFn], b: &[TransformFn], t: f32, u: &Units) -> Option<Vec<TransformFn>> {
    let n = a.len().max(b.len());
    let mut out = Vec::with_capacity(n);
    for i in 0..n {
        let (x, y) = match (a.get(i), b.get(i)) {
            (Some(x), Some(y)) => (x.clone(), y.clone()),
            (Some(x), None) => (x.clone(), identity_fn(x)),
            (None, Some(y)) => (identity_fn(y), y.clone()),
            (None, None) => break,
        };
        out.push(match (x, y) {
            (TransformFn::Translate(x0, y0), TransformFn::Translate(x1, y1)) => {
                TransformFn::Translate(mix_length(x0, x1, t, u)?, mix_length(y0, y1, t, u)?)
            }
            (TransformFn::Scale(x0, y0), TransformFn::Scale(x1, y1)) => TransformFn::Scale(mix(x0, x1, t), mix(y0, y1, t)),
            (TransformFn::Rotate(a0), TransformFn::Rotate(a1)) => TransformFn::Rotate(mix(a0, a1, t)),
            (TransformFn::Skew(x0, y0), TransformFn::Skew(x1, y1)) => TransformFn::Skew(mix(x0, x1, t), mix(y0, y1, t)),
            (TransformFn::Matrix(m0), TransformFn::Matrix(m1)) => {
                TransformFn::Matrix(std::array::from_fn(|k| mix(m0[k], m1[k], t)))
            }
            _ => return None,
        });
    }
    Some(out)
}

fn identity_filter(f: &FilterFn) -> FilterFn {
    match f {
        FilterFn::Blur(_) => FilterFn::Blur(0.0),
        FilterFn::Brightness(_) => FilterFn::Brightness(1.0),
        FilterFn::Contrast(_) => FilterFn::Contrast(1.0),
        FilterFn::Grayscale(_) => FilterFn::Grayscale(0.0),
        FilterFn::HueRotate(_) => FilterFn::HueRotate(0.0),
        FilterFn::Invert(_) => FilterFn::Invert(0.0),
        FilterFn::Opacity(_) => FilterFn::Opacity(1.0),
        FilterFn::Saturate(_) => FilterFn::Saturate(1.0),
        FilterFn::Sepia(_) => FilterFn::Sepia(0.0),
        FilterFn::DropShadow { color, .. } => FilterFn::DropShadow { x: 0.0, y: 0.0, blur: 0.0, color: color.alpha(0.0) },
    }
}

/// Filter lists, function by function (Filter Effects 1, "Interpolation of
/// filters"); None when the kinds differ.
fn mix_filters(a: &[FilterFn], b: &[FilterFn], t: f32) -> Option<Vec<FilterFn>> {
    use FilterFn as F;
    let n = a.len().max(b.len());
    let mut out = Vec::with_capacity(n);
    for i in 0..n {
        let (x, y) = match (a.get(i), b.get(i)) {
            (Some(x), Some(y)) => (x.clone(), y.clone()),
            (Some(x), None) => (x.clone(), identity_filter(x)),
            (None, Some(y)) => (identity_filter(y), y.clone()),
            (None, None) => break,
        };
        out.push(match (x, y) {
            (F::Blur(p), F::Blur(q)) => F::Blur(mix(p, q, t)),
            (F::Brightness(p), F::Brightness(q)) => F::Brightness(mix(p, q, t)),
            (F::Contrast(p), F::Contrast(q)) => F::Contrast(mix(p, q, t)),
            (F::Grayscale(p), F::Grayscale(q)) => F::Grayscale(mix(p, q, t)),
            (F::HueRotate(p), F::HueRotate(q)) => F::HueRotate(mix(p, q, t)),
            (F::Invert(p), F::Invert(q)) => F::Invert(mix(p, q, t)),
            (F::Opacity(p), F::Opacity(q)) => F::Opacity(mix(p, q, t)),
            (F::Saturate(p), F::Saturate(q)) => F::Saturate(mix(p, q, t)),
            (F::Sepia(p), F::Sepia(q)) => F::Sepia(mix(p, q, t)),
            (F::DropShadow { x: x0, y: y0, blur: b0, color: c0 }, F::DropShadow { x: x1, y: y1, blur: b1, color: c1 }) => {
                F::DropShadow { x: mix(x0, x1, t), y: mix(y0, y1, t), blur: mix(b0, b1, t), color: mix_color(c0, c1, t) }
            }
            _ => return None,
        });
    }
    Some(out)
}

const NO_SHADOW: Shadow = Shadow { x: 0.0, y: 0.0, blur: 0.0, spread: 0.0, color: Rgba::TRANSPARENT, inset: false };

/// Interpolate; None when the values don't interpolate (discrete).
fn lerp(a: &Val, b: &Val, t: f32, u: &Units) -> Option<Val> {
    Some(match (a, b) {
        (Val::Dim(x), Val::Dim(y)) => match mix_compact(x.into_raw(), y.into_raw(), t)? {
            (false, v) => Val::Dim(Dimension::length(v)),
            (true, v) => Val::Dim(Dimension::percent(v)),
        },
        (Val::Lpa(x), Val::Lpa(y)) => match mix_compact(x.into_raw(), y.into_raw(), t)? {
            (false, v) => Val::Lpa(LengthPercentageAuto::length(v)),
            (true, v) => Val::Lpa(LengthPercentageAuto::percent(v)),
        },
        (Val::Lp(x), Val::Lp(y)) => match mix_compact(x.into_raw(), y.into_raw(), t)? {
            (false, v) => Val::Lp(LengthPercentage::length(v)),
            (true, v) => Val::Lp(LengthPercentage::percent(v)),
        },
        (Val::Num(x), Val::Num(y)) => Val::Num(mix(*x, *y, t)),
        (Val::Int(x), Val::Int(y)) => Val::Int(mix(*x as f32, *y as f32, t).round() as i32),
        (Val::Color(x), Val::Color(y)) => Val::Color(mix_color(*x, *y, t)),
        (Val::OptColor(Some(x)), Val::OptColor(Some(y))) => Val::OptColor(Some(mix_color(*x, *y, t))),
        (Val::Len(x), Val::Len(y)) => Val::Len(mix_length(*x, *y, t, u)?),
        (Val::Len2(x0, y0), Val::Len2(x1, y1)) => Val::Len2(mix_length(*x0, *x1, t, u)?, mix_length(*y0, *y1, t, u)?),
        (Val::Shadows(x), Val::Shadows(y)) => {
            // the shorter list is padded with transparent zero shadows
            let n = x.len().max(y.len());
            let pad = |l: &Vec<Shadow>, o: &Vec<Shadow>, i: usize| {
                l.get(i).copied().unwrap_or(Shadow { inset: o[i].inset, ..NO_SHADOW })
            };
            Val::Shadows((0..n).map(|i| mix_shadow(&pad(x, y, i), &pad(y, x, i), t)).collect::<Option<_>>()?)
        }
        (Val::TextShadow(x), Val::TextShadow(y)) => {
            let (p, q) = (x.unwrap_or(NO_SHADOW), y.unwrap_or(NO_SHADOW));
            if x.is_none() && y.is_none() {
                Val::TextShadow(None)
            } else {
                Val::TextShadow(mix_shadow(&p, &q, t))
            }
        }
        (Val::Visible(x), Val::Visible(y)) => Val::Visible(if t <= 0.0 {
            *x
        } else if t >= 1.0 {
            *y
        } else {
            *x || *y
        }),
        (Val::Z(Some(x)), Val::Z(Some(y))) => Val::Z(Some(mix(*x as f32, *y as f32, t).round() as i32)),
        (Val::Pair(a0, a1), Val::Pair(b0, b1)) => Val::Pair(mix(*a0, *b0, t), mix(*a1, *b1, t)),
        (Val::Fns(x), Val::Fns(y)) => Val::Fns(mix_transforms(x, y, t, u)?),
        (Val::Filters(x), Val::Filters(y)) => Val::Filters(mix_filters(x, y, t)?),
        (Val::Lh(x), Val::Lh(y)) => Val::Lh(match (x, y) {
            (LineHeight::Factor(p), LineHeight::Factor(q)) => LineHeight::Factor(mix(*p, *q, t)),
            (LineHeight::Px(p), LineHeight::Px(q)) => LineHeight::Px(mix(*p, *q, t)),
            _ => return None,
        }),
        _ => return None,
    })
}

/// Interpolate, or flip at 50% when the values don't interpolate.
fn blend(a: &Val, b: &Val, t: f32, u: &Units) -> Val {
    lerp(a, b, t, u).unwrap_or_else(|| if t < 0.5 { a.clone() } else { b.clone() })
}

// ==========================================================================
// Clock

/// A window's animation clock in seconds: real time for native windows,
/// virtual (advanced only by [`Clock::advance`]) for headless ones so tests
/// are deterministic.
pub(crate) struct Clock {
    origin: Option<std::time::Instant>,
    offset: f64,
}

impl Clock {
    pub(crate) fn new(real: bool) -> Clock {
        Clock { origin: real.then(std::time::Instant::now), offset: 0.0 }
    }

    pub(crate) fn now(&self) -> f64 {
        self.origin.map_or(0.0, |o| o.elapsed().as_secs_f64()) + self.offset
    }

    pub(crate) fn advance(&mut self, d: std::time::Duration) {
        self.offset += d.as_secs_f64();
    }
}

// ==========================================================================
// Per-element state

/// One animation of an element (an `animation-name` entry).
struct Run {
    name: Box<str>,
    /// Clock time the animation started.
    start: f64,
    /// While paused: the local time it stopped at.
    held: Option<f64>,
}

/// A running transition of one field.
struct Transition {
    field: Field,
    from: Val,
    to: Val,
    start: f64,
    delay: f32,
    duration: f32,
    timing: Timing,
    /// For reversing a transition midway (css-transitions "reversing-adjusted
    /// start value" and "reversing shortening factor").
    reversing_start: Val,
    shortening: f32,
}

impl Transition {
    /// Eased progress at `now` (may sit outside 0..1 for overshooting easings).
    fn progress(&self, now: f64) -> f32 {
        let local = (now - self.start) as f32 - self.delay;
        if local < 0.0 {
            return 0.0;
        }
        if self.duration <= 0.0 {
            return 1.0;
        }
        self.timing.apply((local / self.duration).min(1.0), false)
    }

    fn done(&self, now: f64) -> bool {
        (now - self.start) as f32 >= self.delay + self.duration
    }

    fn value(&self, now: f64, u: &Units) -> Val {
        blend(&self.from, &self.to, self.progress(now), u)
    }
}

#[derive(Default)]
struct ElState {
    seen: u64,
    runs: Vec<Run>,
    transitions: Vec<Transition>,
    /// The element's cascaded style last frame (when it has transitions).
    prev: Option<Box<Computed>>,
}

/// What [`Animations::apply`] needs from the cascade.
pub(crate) struct Env<'a> {
    pub parent: &'a Computed,
    pub viewport: (f32, f32),
    /// The element's custom properties (`var()` in keyframes).
    pub vars: &'a [(Prop, Box<[Tok]>)],
    pub sheet: &'a crate::css::Stylesheet,
    /// The sheet's media query results this frame.
    pub media: &'a [bool],
    /// Properties set by `!important` declarations on the element.
    pub important: &'a [Prop],
    /// The element, its Motion props, and the states its `while*` follow.
    pub el: &'a crate::layout::ElData,
    /// its box in the frame
    pub node: usize,
    pub motion: Option<&'a crate::motion::Spec>,
    pub gestures: crate::motion::Gestures,
}

/// Animation and transition state of a window's elements, by element key.
#[derive(Default)]
pub(crate) struct Animations {
    now: f64,
    frame: u64,
    els: KeyMap<u64, ElState>,
    /// Something is moving: render another frame.
    running: bool,
    /// Motion's elements (`<motion.div>`), moved by the animation engine.
    motion: crate::motion::Elements,
}

impl Animations {
    /// Before a layout: the time this frame shows.
    pub(crate) fn begin(&mut self, now: f64) {
        self.now = now;
        self.frame += 1;
        self.running = false;
        self.motion.begin();
        crate::motion::tick();
    }

    /// After a layout: forget elements that are gone (or lost their
    /// animations). Whether CSS animations and transitions (they need full
    /// frames) and Motion's values (redraws do) still move.
    pub(crate) fn end(&mut self) -> (bool, bool) {
        let frame = self.frame;
        self.els.retain(|_, e| e.seen == frame);
        self.motion.end();
        crate::motion::end_pending();
        (self.running, crate::motion::active())
    }

    /// A frame of Motion's values alone (see [`Elements::redraw`](crate::motion::Elements)):
    /// false when a full render is needed.
    pub(crate) fn redraw_motion(&mut self, frame: &mut crate::layout::Frame, styles: &mut crate::layout::StyleStore,
                                viewport: (f32, f32)) -> bool {
        crate::motion::tick();
        self.motion.redraw(frame, styles, viewport)
    }

    /// Motion's drag: the pointer pressed on `path`, moved, let go (true:
    /// something drags, or a drag ended).
    pub(crate) fn pointer_down(&mut self, path: &[u64], x: f32, y: f32) -> bool {
        self.motion.pointer_down(path, x, y)
    }

    pub(crate) fn pointer_move(&mut self, x: f32, y: f32) -> bool {
        self.motion.pointer_move(x, y)
    }

    pub(crate) fn pointer_up(&mut self) -> bool {
        self.motion.pointer_up()
    }

    /// After a layout: Motion's in-view states; true when one changed.
    pub(crate) fn after_layout(&mut self, frame: &crate::layout::Frame, viewport: (f32, f32)) -> bool {
        self.motion.after_layout(frame, viewport)
    }

    /// The boxes the last redraw changed (lend the list; give it back after).
    pub(crate) fn take_redrawn(&mut self) -> Vec<bool> {
        self.motion.take_dirty()
    }

    pub(crate) fn give_redrawn(&mut self, v: Vec<bool>) {
        self.motion.give_dirty(v);
    }

    /// Whether anything is animating (as of the last layout).
    pub(crate) fn running(&self) -> bool {
        self.running || crate::motion::active()
    }

    /// Whether any element has CSS animation state (time passing may matter;
    /// Motion's values: [`crate::motion::active`]).
    pub(crate) fn any(&self) -> bool {
        !self.els.is_empty()
    }

    /// Whether element `key` with cascaded style `base` needs [`apply`](Self::apply).
    pub(crate) fn wants(&self, key: u64, base: &Computed) -> bool {
        base.anim.is_some() || (!self.els.is_empty() && self.els.contains_key(&key)) || self.motion.has(key)
    }

    /// The animated style of element `key` whose cascaded style is `base`
    /// (None: the cascaded style as is).
    pub(crate) fn apply(&mut self, key: u64, base: &Computed, env: &Env) -> Option<Computed> {
        let spec = base.anim.as_deref().filter(|s| s.has_animations() || s.has_transitions());
        if spec.is_none() && !self.els.get(&key).is_some_and(|e| !e.transitions.is_empty()) {
            self.els.remove(&key);
            return self.apply_motion(key, base, env, None);
        }
        let (now, frame) = (self.now, self.frame);
        let st = self.els.entry(key).or_default();
        st.seen = frame;
        let u = Units { font_size: base.font_size, viewport: env.viewport };
        let mut running = false;

        // -- animations: keep the runs whose name stays, start new ones now
        let names: &[Box<str>] = spec.map(|s| &s.names[..]).unwrap_or(&[]);
        let mut old = std::mem::take(&mut st.runs);
        for n in names {
            let run = match old.iter().position(|r| r.name == *n) {
                Some(k) => old.remove(k),
                None => Run { name: n.clone(), start: now, held: None },
            };
            st.runs.push(run);
        }

        // -- transitions: start one for each watched field that changed
        let mut watched = Vec::new();
        if let Some(s) = spec.filter(|s| s.has_transitions()) {
            watched = s.watched();
            if let Some(prev) = &st.prev {
                for &f in &watched {
                    let (Some(before), Some(after)) = (get(f, prev), get(f, base)) else { continue };
                    if before == after {
                        continue;
                    }
                    let running_t = st.transitions.iter().position(|t| t.field == f);
                    if running_t.is_some_and(|k| st.transitions[k].to == after) {
                        continue;
                    }
                    let current = match running_t {
                        Some(k) => st.transitions[k].value(now, &u),
                        None => before,
                    };
                    let old_t = running_t.map(|k| st.transitions.remove(k));
                    let Some((mut dur, mut delay, timing, discrete)) = s.transition_for(f) else { continue };
                    if dur + delay <= 0.0 || current == after
                        || (!discrete && f != Field::Visibility && lerp(&current, &after, 0.5, &u).is_none()) {
                        continue;
                    }
                    let (mut reversing_start, mut shortening) = (current.clone(), 1.0);
                    if let Some(o) = old_t.filter(|o| o.reversing_start == after) {
                        // going back to where it came from: take as long as it has run
                        let p = o.progress(now).clamp(0.0, 1.0);
                        shortening = (p * o.shortening + (1.0 - o.shortening)).clamp(0.0, 1.0);
                        dur *= shortening;
                        if delay < 0.0 {
                            delay *= shortening;
                        }
                        reversing_start = o.to.clone();
                    }
                    st.transitions.push(Transition { field: f, from: current, to: after, start: now, delay,
                                                     duration: dur, timing, reversing_start, shortening });
                }
            }
            // remember the style the next frame compares against
            let stale = match &st.prev {
                Some(p) => ALL_FIELDS.iter().any(|&f| get(f, p) != get(f, base)),
                None => true,
            };
            if stale {
                st.prev = Some(Box::new(base.clone()));
            }
        } else {
            st.prev = None;
        }
        // a property no longer transitioned stops at once
        st.transitions.retain(|t| watched.contains(&t.field) && !t.done(now));

        let mut out: Option<Computed> = None;
        for t in &st.transitions {
            running = true;
            set(t.field, out.get_or_insert_with(|| base.clone()), t.value(now, &u));
        }

        // -- animations on top
        if let Some(s) = spec {
            let important: Vec<Field> = env.important.iter().flat_map(|p| fields_of(*p).iter().copied()).collect();
            for (i, run) in st.runs.iter_mut().enumerate() {
                if &*run.name == "none" {
                    continue;
                }
                let paused = nth(&s.paused, i, false);
                match (paused, run.held) {
                    (true, None) => run.held = Some(now - run.start),
                    (false, Some(h)) => {
                        run.start = now - h;
                        run.held = None;
                    }
                    _ => {}
                }
                let local = run.held.unwrap_or(now - run.start) as f32;
                let Some(kf) = keyframes(env.sheet, env.media, &run.name) else { continue };
                let anim = Anim {
                    duration: nth(&s.durations, i, 0.0),
                    delay: nth(&s.delays, i, 0.0),
                    iterations: nth(&s.iterations, i, 1.0),
                    direction: nth(&s.directions, i, Direction::Normal),
                    fill: nth(&s.fills, i, Fill::None),
                };
                let (progress, active) = anim.progress(local);
                if active && !paused {
                    running = true;
                }
                let Some((p, before)) = progress else { continue };
                let timing = nth(&s.timings, i, EASE);
                let c = out.get_or_insert_with(|| base.clone());
                sample(kf, p, before, &timing, base, c, env, &important, &u);
            }
        }
        if st.runs.is_empty() && st.transitions.is_empty() && st.prev.is_none() {
            self.els.remove(&key);
        }
        self.running |= running;
        self.apply_motion(key, base, env, out)
    }

    /// Motion's values on top of the CSS animations (`out`: their result).
    fn apply_motion(&mut self, key: u64, base: &Computed, env: &Env, mut out: Option<Computed>) -> Option<Computed> {
        if env.motion.is_some() || self.motion.has(key) || crate::motion::pending() {
            let u = Units { font_size: base.font_size, viewport: env.viewport };
            let start = out.take();
            let from = start.as_ref().unwrap_or(base);
            let mut moved = None;
            self.motion.apply(key, env.node, env.el, env.motion, from, env.gestures, &u, &mut moved);
            out = moved.or(start);
        }
        out
    }
}

/// The `@keyframes` rule called `name` (the last one whose media match).
fn keyframes<'s>(sheet: &'s crate::css::Stylesheet, media: &[bool], name: &str) -> Option<&'s crate::css::Keyframes> {
    sheet.keyframes.iter().rev()
        .find(|k| k.name == name && k.media.iter().all(|&m| media.get(m as usize).copied().unwrap_or(true)))
}

/// One animation's timing (css-animations / web-animations timing model).
struct Anim {
    duration: f32,
    delay: f32,
    iterations: f32,
    direction: Direction,
    fill: Fill,
}

impl Anim {
    /// At local time `t`: the directed progress through the current
    /// iteration and whether in the before phase (None: no effect), and
    /// whether the animation is still to finish (before or active phase).
    fn progress(&self, t: f32) -> (Option<(f32, bool)>, bool) {
        let active_dur = if self.iterations.is_infinite() {
            f32::INFINITY
        } else {
            self.duration * self.iterations
        };
        let end = (self.delay + active_dur).max(0.0);
        let before_end = self.delay.min(end).max(0.0);
        let active_end = (self.delay + active_dur).min(end).max(0.0);
        let (phase_before, phase_after) = (t < before_end, t >= active_end);
        let active_time = if phase_before {
            if !matches!(self.fill, Fill::Backwards | Fill::Both) {
                return (None, true);
            }
            (t - self.delay).max(0.0)
        } else if phase_after {
            if !matches!(self.fill, Fill::Forwards | Fill::Both) {
                return (None, false);
            }
            (t - self.delay).min(active_dur).max(0.0)
        } else {
            t - self.delay
        };
        let overall = if self.duration == 0.0 {
            if phase_before { 0.0 } else { self.iterations }
        } else {
            active_time / self.duration
        };
        let mut simple = if overall.is_infinite() { 0.0 } else { overall.fract() };
        if simple == 0.0 && !phase_before && active_time == active_dur && self.iterations != 0.0 {
            simple = 1.0;
        }
        let iteration = if phase_after && self.iterations.is_infinite() {
            f32::INFINITY
        } else if simple == 1.0 {
            overall.floor() - 1.0
        } else {
            overall.floor()
        };
        let even = iteration.is_infinite() || (iteration as i64) % 2 == 0;
        let forward = match self.direction {
            Direction::Normal => true,
            Direction::Reverse => false,
            Direction::Alternate => even,
            Direction::AlternateReverse => !even,
        };
        let p = if forward { simple } else { 1.0 - simple };
        (Some((p, phase_before)), !phase_after)
    }
}

/// Apply keyframes `kf` at iteration progress `p` onto `out`.
#[allow(clippy::too_many_arguments)]
fn sample(kf: &crate::css::Keyframes, p: f32, before: bool, timing: &Timing, base: &Computed, out: &mut Computed,
          env: &Env, important: &[Field], u: &Units) {
    const TIMING: Prop = Prop::named("animation-timing-function");
    // per field: (offset, declaration, the keyframe's own easing), by offset
    let mut tracks: Vec<(Field, Vec<(f32, &Decl, Option<Timing>)>)> = Vec::new();
    for (offsets, decls) in &kf.frames {
        let own = decls.iter().rev().find(|d| d.prop == TIMING).and_then(|d| d.value.first().and_then(Timing::parse));
        for &o in offsets {
            if !(0.0..=1.0).contains(&o) {
                continue;
            }
            for d in decls.iter().filter(|d| !d.important && d.prop != TIMING && d.prop != Prop::UNKNOWN
                                          && !d.prop.name().starts_with("animation")) {
                let fs = fields_of(d.prop);
                let other = [Field::Other(d.prop)];
                for &f in if fs.is_empty() { &other[..] } else { fs } {
                    let k = match tracks.iter().position(|t| t.0 == f) {
                        Some(k) => k,
                        None => {
                            tracks.push((f, Vec::new()));
                            tracks.len() - 1
                        }
                    };
                    let list = &mut tracks[k].1;
                    // a later block with the same offset wins
                    list.retain(|e| e.0 != o);
                    list.push((o, d, own.clone()));
                }
            }
        }
    }
    let mut applied: Vec<(*const Decl, Computed)> = Vec::new();
    let with = |d: &Decl, applied: &mut Vec<(*const Decl, Computed)>| -> usize {
        if let Some(k) = applied.iter().position(|e| std::ptr::eq(e.0, d)) {
            return k;
        }
        let mut c = base.clone();
        apply_decl(&mut c, d, env, u);
        applied.push((d as *const Decl, c));
        applied.len() - 1
    };
    // discrete declarations first: interpolated fields are set over them
    tracks.sort_by_key(|t| !matches!(t.0, Field::Other(_)));
    for (f, mut list) in tracks {
        if important.contains(&f) || matches!(f, Field::Other(p) if env.important.contains(&p)) {
            continue;
        }
        list.sort_by(|a, b| a.0.total_cmp(&b.0));
        // implicit 0% / 100% keyframes hold the underlying value
        type Key<'d> = (f32, Option<&'d Decl>, Option<Timing>);
        let mut keys: Vec<Key> = list.into_iter().map(|(o, d, t)| (o, Some(d), t)).collect();
        if keys.first().is_none_or(|k| k.0 > 0.0) {
            keys.insert(0, (0.0, None, None));
        }
        if keys.last().is_none_or(|k| k.0 < 1.0) {
            keys.push((1.0, None, None));
        }
        let k = keys.iter().rposition(|e| e.0 <= p).unwrap_or(0).min(keys.len() - 2);
        let (a, b) = (&keys[k], &keys[k + 1]);
        let local = if b.0 > a.0 { (p - a.0) / (b.0 - a.0) } else { 1.0 };
        let eased = a.2.as_ref().unwrap_or(timing).apply(local, before);
        match f {
            Field::Other(_) => {
                // discrete: the nearer keyframe's declaration as is
                if let Some(d) = if eased < 0.5 { a.1 } else { b.1 } {
                    apply_decl(out, d, env, u);
                }
            }
            _ => {
                let val = |e: &Key, applied: &mut Vec<(*const Decl, Computed)>| match e.1 {
                    Some(d) => {
                        let k = with(d, applied);
                        get(f, &applied[k].1)
                    }
                    None => get(f, base),
                };
                let (Some(va), Some(vb)) = (val(a, &mut applied), val(b, &mut applied)) else { continue };
                set(f, out, blend(&va, &vb, eased, u));
            }
        }
    }
}

/// Apply one keyframe declaration (with `var()` substituted).
fn apply_decl(c: &mut Computed, d: &Decl, env: &Env, u: &Units) {
    if crate::vars::uses_vars(&d.value) {
        if let Some(v) = crate::vars::substitute(&d.value, env.vars) {
            let d = Decl { prop: d.prop, value: crate::css::Value::Owned(v.into_boxed_slice()), important: false };
            crate::style::apply(c, env.parent, *u, &d);
        }
    } else {
        crate::style::apply(c, env.parent, *u, d);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn easing_functions() {
        let ease = EASE;
        assert!((ease.apply(0.5, false) - 0.8024).abs() < 1e-3);
        assert_eq!(Timing::Linear.apply(0.3, false), 0.3);
        let s = Timing::Steps(4, StepPos::End);
        assert_eq!(s.apply(0.3, false), 0.25);
        assert_eq!(s.apply(1.0, false), 1.0);
        let s = Timing::Steps(4, StepPos::Start);
        assert_eq!(s.apply(0.3, false), 0.5);
        let s = Timing::Steps(3, StepPos::None);
        assert_eq!(s.apply(0.5, false), 0.5);
        let s = Timing::Steps(1, StepPos::Both);
        assert_eq!(s.apply(0.2, false), 0.5);
        let l = linear_points(&crate::css::tokenize("linear(0, 0.25 75%, 1)").iter().map(|t| match t {
            Tok::Func(f) => f.args.clone(),
            _ => vec![],
        }).next().unwrap()).unwrap();
        assert!((l.apply(0.75, false) - 0.25).abs() < 1e-6);
        assert!((l.apply(0.375, false) - 0.125).abs() < 1e-6);
    }

    #[test]
    fn premultiplied_colors() {
        let red = Rgba { r: 1.0, g: 0.0, b: 0.0, a: 1.0 };
        let c = mix_color(red, Rgba::TRANSPARENT, 0.5);
        assert_eq!((c.r, c.a), (1.0, 0.5), "no darkening through transparent black");
    }
}
