//! Motion for the JavaScript runtimes, on the UI thread: GSAP's and Motion's
//! calls on `blenderui::motion` by handle (ids JavaScript chooses), motion
//! values by handle, and the elements' Motion props (`initial`, `animate` ...).
//!
//! JavaScript's objects arrive read once ([`DynVars`]); the engine runs the
//! frames. Events go back only for the callbacks a motion has (and its end).

use std::collections::HashMap;
use std::sync::{Arc, Weak};

use blenderui::motion::{
    Any, Drag, DynTarget, DynVars, Events, InView, Layout, MotionValue, Targets, Timeline, Transition, Tween, Value, Vars, When, animate,
    gsap,
};
use blenderui::prelude::*;
use smallvec::SmallVec;

use crate::{Event, WindowSink};

/// A call's verb.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verb {
    To,
    From,
    FromTo,
    Set,
    /// Motion's `animate`: Motion's defaults (springs for transforms)
    Animate,
}

/// What a call animates.
#[derive(Debug)]
pub enum Target {
    /// `"#id"`, `".class"`, `"tag"`, `"[attr=v]"`, comma-separated
    Selector(String),
    /// motion values, by handle
    Values(Vec<u32>),
    /// a plain object's numbers: (property, where it starts), in the vars' order
    Object(Vec<(String, f32)>),
}

/// The event bit of a timeline's `call()` (after the engine's `BK_EV_MOTION_*`).
pub const EV_CALL: i32 = 1 << 8;

/// A motion JavaScript holds, with what it owns.
struct Held {
    tween: Tween,
    /// a timeline's `defaults`
    defaults: Option<Transition>,
    /// the values a plain object's tween drives (freed with it)
    owned: SmallVec<[MotionValue; 2]>,
}

/// The UI thread's motions and motion values, by JavaScript's ids.
#[derive(Default)]
pub(crate) struct Motions {
    held: HashMap<u32, Held>,
    values: HashMap<u32, MotionValue>,
}

impl Motions {
    pub(crate) fn value(&self, h: u32) -> Option<MotionValue> {
        self.values.get(&h).copied()
    }
}

pub(crate) fn motions(cx: &mut App) -> &mut Motions {
    if cx.try_global::<Motions>().is_none() {
        cx.set_global(Motions::default());
    }
    cx.global_mut::<Motions>()
}

/// The callbacks of motion `id`: an event to JavaScript per bit of `mask`,
/// with the values `read` gives (a value target's, for `onUpdate`).
fn events(cx: &App, id: u32, mask: i32, read: Option<SmallVec<[MotionValue; 2]>>) -> Option<Box<Events>> {
    let sink = sink(cx);
    blenderui::motion::events(mask, |bit| {
        let sink = sink.clone();
        let read = read.clone();
        Box::new(move || {
            let Some(sink) = sink.upgrade() else { return };
            let mut e = Event { kind: "motion".into(), target: id, button: bit as u32, ..Event::default() };
            if let Some(vs) = &read {
                e.number = f64::from(vs.first().map_or(0.0, |v| v.get()));
                if vs.len() > 1 {
                    e.values = Some(vs.iter().map(|v| f64::from(v.get())).collect());
                }
            }
            sink(e);
        })
    })
}

/// Where motion events go: weakly (a motion the engine still runs mustn't
/// keep the runtime alive once the app is gone).
fn sink(cx: &App) -> Weak<dyn Fn(Event)> {
    Arc::downgrade(&cx.global::<WindowSink>().0)
}

fn run(verb: Verb, targets: Targets, vars: Vars, from: Option<Vars>) -> Tween {
    match verb {
        Verb::To => gsap::to(targets, vars),
        Verb::From => gsap::from(targets, vars),
        Verb::FromTo => gsap::from_to(targets, from.unwrap_or_default(), vars),
        Verb::Set => gsap::set(targets, vars),
        Verb::Animate => animate(targets, vars),
    }
}

/// `gsap.to / from / fromTo / set` and Motion's `animate` as motion `id`
/// (held for JavaScript), or placed in timeline `into` at `position`.
/// `mask`: the events JavaScript wants.
#[allow(clippy::too_many_arguments)]
pub fn tween(cx: &mut App, id: u32, verb: Verb, target: Target, mut vars: DynVars, mut from: Option<DynVars>,
             into: Option<(u32, String)>, mask: i32) {
    let m = motions(cx);
    vars.resolve(|h| m.value(h));
    if let Some(f) = &mut from {
        f.resolve(|h| m.value(h));
    }
    let defaults = into.as_ref().and_then(|(tl, _)| m.held.get(tl)?.defaults.clone());
    let mvs: SmallVec<[MotionValue; 4]> = match &target {
        Target::Values(hs) => hs.iter().filter_map(|h| m.value(*h)).collect(),
        _ => SmallVec::new(),
    };
    let mut v = vars.into_vars();
    if let Some(d) = &defaults {
        v.transition = v.transition.or(d);
    }
    let from = from.map(DynVars::into_vars);
    let (tween, owned) = match target {
        Target::Selector(s) => {
            v.events = events(cx, id, mask, None);
            (run(verb, Targets::Selector(s.into()), v, from), SmallVec::new())
        }
        Target::Values(_) => {
            // every value's frames: each listener hears its own
            v.events = events(cx, id, mask, (!mvs.is_empty()).then(|| mvs.iter().copied().collect()));
            (run(verb, Targets::Values(mvs), v, from), SmallVec::new())
        }
        Target::Object(props) => object(cx, id, verb, &props, v, from.as_ref(), mask),
    };
    let m = motions(cx);
    match into {
        Some((tl, position)) => {
            if let Some(parent) = m.held.get_mut(&tl) {
                Timeline(parent.tween.clone()).add(&tween, &position);
                parent.owned.extend(owned);
            }
        }
        None => {
            m.held.insert(id, Held { tween, defaults: None, owned });
        }
    }
}

/// A plain object's numbers: a value per property, each tweened to its own
/// target in one timeline (the last one tells JavaScript the values).
fn object(cx: &App, id: u32, verb: Verb, props: &[(String, f32)], v: Vars, from: Option<&Vars>, mask: i32)
          -> (Tween, SmallVec<[MotionValue; 2]>) {
    let owned: SmallVec<[MotionValue; 2]> = props.iter().map(|(_, start)| MotionValue::new(*start)).collect();
    let paused = v.transition.paused;
    let child_t = Transition { paused: None, ..v.transition.clone() };
    let tl = gsap::timeline(Vars::from(Transition { paused, ..Transition::default() }));
    let n = v.props.len();
    for (i, (key, value)) in v.props.iter().enumerate() {
        let Some(mv) = props.iter().position(|(p, _)| key.name().to_bytes() == p.as_bytes()).map(|j| owned[j]) else {
            continue;
        };
        let one = Vars { props: vec![(*key, value.clone())].into(), transition: child_t.clone(), per: v.per.clone(),
                         events: if i + 1 == n { events(cx, id, mask, Some(owned.clone())) } else { None } };
        let start = from.and_then(|f| f.props.iter().find(|(k, _)| k.id() == key.id())).map(|(k, x)| Vars {
            props: vec![(*k, x.clone())].into(),
            ..Vars::default()
        });
        tl.add(&run(verb, Targets::from(mv), one, start), "0");
    }
    (tl.0, owned)
}

/// `gsap.timeline(vars)` as motion `id`.
pub fn timeline(cx: &mut App, id: u32, mut vars: DynVars, mask: i32) {
    vars.resolve(|_| None);
    let defaults = vars.defaults().cloned();
    let mut v = vars.into_vars();
    v.events = events(cx, id, mask, None);
    let tl = gsap::timeline(v);
    motions(cx).held.insert(id, Held { tween: tl.0, defaults, owned: SmallVec::new() });
}

/// `tl.add(child, position)`: a motion JavaScript holds, placed in a timeline.
pub fn add(cx: &mut App, tl: u32, child: u32, position: &str) {
    let m = motions(cx);
    let Some(c) = m.held.get(&child).map(|h| h.tween.clone()) else { return };
    if let Some(parent) = m.held.get(&tl) {
        Timeline(parent.tween.clone()).add(&c, position);
    }
}

/// `tl.addLabel(name, position)`.
pub fn label(cx: &mut App, tl: u32, name: &str, position: &str) {
    if let Some(parent) = motions(cx).held.get(&tl) {
        Timeline(parent.tween.clone()).add_label(name, position);
    }
}

/// `tl.call(fn, params, position)`: JavaScript's call `id` runs there.
pub fn call(cx: &mut App, tl: u32, id: u32, position: &str) {
    let sink = sink(cx);
    if let Some(parent) = motions(cx).held.get(&tl) {
        Timeline(parent.tween.clone()).call(move || {
            if let Some(sink) = sink.upgrade() {
                sink(Event { kind: "motion".into(), target: id, button: EV_CALL as u32, ..Event::default() });
            }
        }, position);
    }
}

/// A control of motion `id` (GSAP's names).
pub fn control(cx: &mut App, id: u32, op: &str, value: f64) {
    let Some(h) = motions(cx).held.get(&id) else { return };
    let t = &h.tween;
    match op {
        "play" => { t.play(); }
        "pause" => { t.pause(); }
        "resume" => { t.resume(); }
        "reverse" => { t.reverse(); }
        "restart" => { t.restart(); }
        "complete" => { t.complete(); }
        "kill" => { t.kill(); }
        "seek" => { t.seek(value); }
        "progress" => { t.set_progress(value); }
        "totalProgress" => { t.set_total_progress(value); }
        "timeScale" => { t.set_time_scale(value); }
        _ => {}
    }
    // values a jump set show in the next frame
    cx.notify_all();
}

/// A query of motion `id` (NaN when there's no such motion or query).
pub fn query(cx: &mut App, id: u32, what: &str) -> f64 {
    let Some(h) = motions(cx).held.get(&id) else { return f64::NAN };
    let t = &h.tween;
    let flag = |b: bool| f64::from(u8::from(b));
    match what {
        "time" => t.time(),
        "totalTime" => t.total_time(),
        "duration" => t.duration(),
        "totalDuration" => t.total_duration(),
        "progress" => t.progress(),
        "timeScale" => t.time_scale(),
        "isActive" => flag(t.is_active()),
        "paused" => flag(t.paused()),
        "reversed" => flag(t.reversed()),
        label => Timeline(t.clone()).label_time(label).unwrap_or(f64::NAN),
    }
}

/// JavaScript dropped motion `id` (it runs on; what it owned goes with it).
pub fn release(cx: &mut App, id: u32) {
    if let Some(h) = motions(cx).held.remove(&id) {
        for mv in h.owned {
            mv.dispose();
        }
    }
}

/// `motionValue(init)` as value `id`.
pub fn value_new(cx: &mut App, id: u32, init: &Any) {
    let mv = MotionValue::new(value_of(init));
    motions(cx).values.insert(id, mv);
}

/// `mv.set(v)`: windows that bind it draw it.
pub fn value_set(cx: &mut App, id: u32, v: &Any) {
    if let Some(mv) = motions(cx).value(id) {
        mv.set(value_of(v));
    }
}

/// `mv.get()` (NaN for none), and `getVelocity()` (`velocity`).
pub fn value_get(cx: &mut App, id: u32, velocity: bool) -> f64 {
    motions(cx).value(id).map_or(f64::NAN, |mv| f64::from(if velocity { mv.velocity() } else { mv.get() }))
}

pub fn value_dispose(cx: &mut App, id: u32) {
    if let Some(mv) = motions(cx).values.remove(&id) {
        mv.dispose();
    }
}

fn value_of(v: &Any) -> Value {
    match v {
        Any::Num(n) => Value::Num(*n as f32),
        Any::Str(s) => Value::from(s.as_str()),
        _ => Value::Num(0.0),
    }
}

/// `CustomEase.create(name, svgPath)` / `gsap.registerEase`: false for a
/// path the engine can't read.
pub fn register_ease(name: &str, path: &str) -> bool {
    let (Ok(n), Ok(p)) = (std::ffi::CString::new(name), std::ffi::CString::new(path)) else { return false };
    blenderui::motion::Ease::path(&p).is_some_and(|e| e.register(&n))
}

// ==========================================================================
// Elements' Motion props

/// One Motion prop of an element, as it crosses (op `MOTION`).
#[derive(Debug)]
pub enum MotionProp {
    Target(When, DynTarget),
    Transition(DynVars),
    Style(DynVars),
    Variants(Vec<(&'static str, DynVars)>),
    Viewport(InView),
    Drag(Drag),
    DragConstraints([f32; 4]),
    DragElastic(f32),
    DragMomentum(bool),
    DragSnapToOrigin(bool),
    Layout(Layout),
    /// the prop is gone (`which` as in [`MotionProp::parse`])
    Clear(i32),
}

/// The targets by `which`: 0 .. 5, then 9 whileInView, 10 whileDrag.
const WHEN: [When; 6] = [When::Initial, When::Animate, When::Exit, When::WhileHover, When::WhileTap, When::WhileFocus];

fn num(v: &Any) -> Option<f32> {
    match v {
        Any::Num(n) => Some(*n as f32),
        Any::Bool(b) => Some(f32::from(u8::from(*b))),
        _ => None,
    }
}

fn flag(v: &Any, name: &str) -> Result<bool, String> {
    match v {
        Any::Bool(b) => Ok(*b),
        _ => Err(format!("`{name}` takes true or false")),
    }
}

impl MotionProp {
    /// Prop `which` (0 initial, 1 animate, 2 exit, 3 whileHover, 4 whileTap,
    /// 5 whileFocus, 6 transition, 7 style, 8 variants: `bindings/node/dom.js`'s
    /// MOTION_PROPS) from its value; `null` removes it.
    pub fn parse(which: i32, v: &Any) -> Result<MotionProp, String> {
        if *v == Any::Null {
            return Ok(MotionProp::Clear(which));
        }
        Ok(match which {
            0..=5 => MotionProp::Target(WHEN[which as usize], DynTarget::parse(v)?),
            6 => MotionProp::Transition(match v {
                Any::Map(m) => {
                    let mut d = DynVars::default();
                    d.add_transition(m)?;
                    d
                }
                _ => return Err("`transition` takes an object".into()),
            }),
            7 => MotionProp::Style(match v {
                Any::Map(m) => DynVars::parse(m)?,
                _ => return Err("a motion `style` takes an object".into()),
            }),
            8 => MotionProp::Variants(blenderui::motion::variants(v)?),
            9 => MotionProp::Target(When::WhileInView, DynTarget::parse(v)?),
            10 => MotionProp::Target(When::WhileDrag, DynTarget::parse(v)?),
            11 => MotionProp::Viewport(match v {
                Any::Map(m) => {
                    let mut r = InView::default();
                    for (k, x) in m {
                        match (k.as_str(), x) {
                            ("once", x) => r.once = flag(x, "viewport.once")?,
                            ("amount", Any::Str(s)) if s == "all" => r.amount = 1.0,
                            ("amount", Any::Str(s)) if s == "some" => r.amount = 0.0,
                            ("amount", x) => r.amount = num(x).ok_or("`viewport.amount` takes a number, \"some\" or \"all\"")?.clamp(0.0, 1.0),
                            ("margin" | "root", _) => return Err(format!("`viewport.{k}` isn't supported")),
                            _ => return Err(format!("no viewport option `{k}`")),
                        }
                    }
                    r
                }
                _ => return Err("`viewport` takes an object".into()),
            }),
            12 => MotionProp::Drag(match v {
                Any::Bool(b) => Drag::from(*b),
                Any::Str(s) if s == "x" || s == "y" => Drag::from(s.as_str()),
                _ => return Err("`drag` takes true, \"x\" or \"y\"".into()),
            }),
            13 => MotionProp::DragConstraints(match v {
                Any::Map(m) => {
                    let mut c = [f32::NEG_INFINITY, f32::INFINITY, f32::NEG_INFINITY, f32::INFINITY];
                    for (k, x) in m {
                        let i = match k.as_str() {
                            "left" => 0,
                            "right" => 1,
                            "top" => 2,
                            "bottom" => 3,
                            _ => return Err(format!("no dragConstraints side `{k}`")),
                        };
                        c[i] = num(x).ok_or_else(|| format!("dragConstraints.{k} takes a number"))?;
                    }
                    c
                }
                _ => return Err("`dragConstraints` takes { left, right, top, bottom } (pixels)".into()),
            }),
            14 => MotionProp::DragElastic(num(v).ok_or("`dragElastic` takes a number")?.clamp(0.0, 1.0)),
            15 => MotionProp::DragMomentum(flag(v, "dragMomentum")?),
            16 => MotionProp::DragSnapToOrigin(flag(v, "dragSnapToOrigin")?),
            17 => MotionProp::Layout(match v {
                Any::Bool(true) => Layout::Both,
                Any::Str(s) if s == "position" || s == "size" => Layout::from(s.as_str()),
                _ => return Err("`layout` takes true, \"position\" or \"size\"".into()),
            }),
            _ => return Err(format!("no Motion prop {which}")),
        })
    }

    fn which(&self) -> i32 {
        match self {
            MotionProp::Target(When::WhileInView, _) => 9,
            MotionProp::Target(When::WhileDrag, _) => 10,
            MotionProp::Target(w, _) => WHEN.iter().position(|x| x == w).map_or(-1, |i| i as i32),
            MotionProp::Transition(_) => 6,
            MotionProp::Style(_) => 7,
            MotionProp::Variants(_) => 8,
            MotionProp::Viewport(_) => 11,
            MotionProp::Drag(_) => 12,
            MotionProp::DragConstraints(_) => 13,
            MotionProp::DragElastic(_) => 14,
            MotionProp::DragMomentum(_) => 15,
            MotionProp::DragSnapToOrigin(_) => 16,
            MotionProp::Layout(_) => 17,
            MotionProp::Clear(w) => *w,
        }
    }

    /// Its motion values and eases (UI thread, once).
    pub(crate) fn resolve(&mut self, m: &Motions) {
        let find = |h| m.value(h);
        match self {
            MotionProp::Target(_, t) => t.resolve(find),
            MotionProp::Transition(v) | MotionProp::Style(v) => v.resolve(find),
            MotionProp::Variants(vs) => {
                for (_, v) in vs {
                    v.resolve(find);
                }
            }
            MotionProp::Viewport(_) | MotionProp::Drag(_) | MotionProp::DragConstraints(_) | MotionProp::DragElastic(_)
            | MotionProp::DragMomentum(_) | MotionProp::DragSnapToOrigin(_) | MotionProp::Layout(_)
            | MotionProp::Clear(_) => {}
        }
    }
}

/// An element's Motion props (only those given).
#[derive(Debug, Default)]
pub(crate) struct NodeMotion(SmallVec<[MotionProp; 2]>);

impl NodeMotion {
    pub(crate) fn set(&mut self, p: MotionProp) {
        let w = p.which();
        self.0.retain(|x| x.which() != w);
        if !matches!(p, MotionProp::Clear(_)) {
            self.0.push(p);
        }
    }

    /// The props on the element (each render makes its own copies).
    /// Whether it has an `exit` (it animates out before it goes).
    pub(crate) fn has_exit(&self) -> bool {
        self.0.iter().any(|p| matches!(p, MotionProp::Target(When::Exit, _)))
    }

    pub(crate) fn apply(&self, mut d: Div) -> Div {
        for p in &self.0 {
            d = match p {
                MotionProp::Target(w, t) => match w {
                    When::Initial => d.initial(t.target()),
                    When::Animate => d.animate(t.target()),
                    When::Exit => d.exit(t.target()),
                    When::WhileHover => d.while_hover(t.target()),
                    When::WhileTap => d.while_tap(t.target()),
                    When::WhileFocus => d.while_focus(t.target()),
                    When::WhileInView => d.while_in_view(t.target()),
                    When::WhileDrag => d.while_drag(t.target()),
                },
                MotionProp::Viewport(v) => d.viewport(*v),
                MotionProp::Drag(g) => d.drag(*g),
                MotionProp::DragConstraints(c) => d.drag_constraints(*c),
                MotionProp::DragElastic(e) => d.drag_elastic(*e),
                MotionProp::DragMomentum(m) => d.drag_momentum(*m),
                MotionProp::DragSnapToOrigin(s) => d.drag_snap_to_origin(*s),
                MotionProp::Layout(l) => d.layout(*l),
                MotionProp::Transition(v) => d.transition(v.vars()),
                MotionProp::Style(v) => d.motion_style(v.vars()),
                MotionProp::Variants(vs) => d.variants(vs.iter().map(|(n, v)| (*n, v.vars())).collect::<Vec<_>>()),
                MotionProp::Clear(_) => d,
            };
        }
        d
    }
}

/// Move every window's clock by `ms` (headless: the virtual clock) and, when
/// `draw`, render them (tests step animations frame-exactly). A `clock`
/// event follows the step's events.
pub fn advance_clock(cx: &mut App, ms: f64, draw: bool) {
    cx.advance_clock(std::time::Duration::from_secs_f64(ms.max(0.0) / 1000.0));
    if draw {
        let ids: Vec<WindowId> = cx.global::<crate::UiState>().windows.values().map(|h| h.id).collect();
        for id in ids {
            cx.draw(id);
        }
    }
    cx.global::<WindowSink>().0(Event { kind: "clock".into(), ..Event::default() });
}
