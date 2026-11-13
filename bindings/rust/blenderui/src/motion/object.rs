//! Vars from a runtime object (JavaScript, Python, Lua): the grammar `vars!`
//! reads when a program compiles, read here once when a binding's call
//! crosses into Rust.
//!
//! Reading touches no engine state, so a binding checks its caller's object
//! where the call is made (on any thread, errors naming the option);
//! [`DynVars::resolve`] then parses the eases on the UI thread, once. The
//! option names are `vars!`'s (`blenderui-macros/src/vars.rs`): the two
//! tables move together.

use std::borrow::Cow;
use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::sync::{Mutex, OnceLock, PoisonError};

use blenderui_sys as sys;

use super::MotionValue;
use super::ease::{Ease, ease};
use super::spec::Target;
use super::value::{Key, Value};
use super::vars::{Events, Kind, Transition, Vars};

/// A value of a runtime object, as a binding reads it.
#[derive(Clone, Debug, PartialEq)]
pub enum Any {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    List(Vec<Self>),
    Map(Vec<(String, Self)>),
    /// A function (`onComplete` ...): the binding keeps it; the vars note
    /// which events it wants.
    Func,
    /// A motion value, by the binding's handle (found on the UI thread).
    Motion(u32),
}

/// An ease as written: parsed on the UI thread (the ease pool is its).
#[derive(Clone, Debug)]
enum EaseSrc {
    Name(CString),
    Bezier([f32; 4]),
    /// one per keyframe segment
    Names(Vec<CString>),
}

/// Which transition an ease belongs to.
#[derive(Clone, Copy, Debug)]
enum Slot {
    Main,
    Per(usize),
    Defaults,
}

/// A vars object read from a runtime value: [`Vars`] once its eases and
/// motion values are resolved on the UI thread. `Send`: it crosses threads.
#[derive(Clone, Debug, Default)]
pub struct DynVars {
    props: Vec<(&'static Key, Value)>,
    transition: Transition,
    per: Vec<(&'static Key, Transition)>,
    defaults: Option<Transition>,
    eases: Vec<(Slot, EaseSrc)>,
    /// `props[i]` is the binding's motion value `handle`
    motions: Vec<(usize, u32)>,
    events: i32,
}

impl DynVars {
    /// GSAP's vars / Motion's target: properties, options, `transition: {..}`
    /// (with per-property ones), `defaults: {..}` (a timeline's), callbacks.
    pub fn parse(entries: &[(String, Any)]) -> Result<Self, String> {
        let mut d = Self::default();
        for (k, v) in entries {
            match norm(k).as_str() {
                "transition" => d.add_transition(map(k, v)?)?,
                "defaults" => {
                    let mut t = Transition::default();
                    for (dk, dv) in map(k, v)? {
                        if !option(Slot::Defaults, &mut t, &mut d.eases, &mut d.events, dk, dv)? {
                            return Err(format!("defaults: `{dk}` isn't an option"));
                        }
                    }
                    d.defaults = Some(t);
                }
                _ => {
                    if !option(Slot::Main, &mut d.transition, &mut d.eases, &mut d.events, k, v)? {
                        d.prop(k, v)?;
                    }
                }
            }
        }
        Ok(d)
    }

    /// Motion's `transition` object (and `animate()`'s options): options, a
    /// `default: {..}`, and per-property transitions, added to these vars.
    pub fn add_transition(&mut self, entries: &[(String, Any)]) -> Result<(), String> {
        for (k, v) in entries {
            if norm(k) == "default" {
                for (dk, dv) in map(k, v)? {
                    if !option(Slot::Main, &mut self.transition, &mut self.eases, &mut self.events, dk, dv)? {
                        return Err(format!("transition.default: `{dk}` isn't an option"));
                    }
                }
            } else if !option(Slot::Main, &mut self.transition, &mut self.eases, &mut self.events, k, v)? {
                // a property's own transition
                let slot = Slot::Per(self.per.len());
                let mut t = Transition::default();
                for (pk, pv) in map(k, v)? {
                    if !option(slot, &mut t, &mut self.eases, &mut self.events, pk, pv)? {
                        return Err(format!("transition.{k}: `{pk}` isn't an option"));
                    }
                }
                self.per.push((intern_key(k)?, t));
            }
        }
        Ok(())
    }

    fn prop(&mut self, k: &str, v: &Any) -> Result<(), String> {
        if let Any::Motion(h) = v {
            self.motions.push((self.props.len(), *h));
        }
        self.props.push((intern_key(k)?, value(k, v)?));
        Ok(())
    }

    /// Parse the eases and find the motion values (UI thread, once).
    pub fn resolve(&mut self, motion: impl Fn(u32) -> Option<MotionValue>) {
        for (slot, src) in std::mem::take(&mut self.eases) {
            let t = match slot {
                Slot::Main => &mut self.transition,
                Slot::Per(i) => &mut self.per[i].1,
                Slot::Defaults => self.defaults.get_or_insert_with(Transition::default),
            };
            match src {
                EaseSrc::Name(n) => t.ease = Some(ease(&n)),
                EaseSrc::Bezier([x1, y1, x2, y2]) => t.ease = Some(Ease::bezier(x1, y1, x2, y2)),
                EaseSrc::Names(n) => t.eases = Some(Cow::Owned(n.iter().map(|n| ease(n)).collect())),
            }
        }
        for (i, h) in std::mem::take(&mut self.motions) {
            if let Some(mv) = motion(h) {
                self.props[i].1 = Value::Motion(mv);
            }
        }
    }

    /// The engine events its callbacks want (`BK_EV_MOTION_*` bits).
    #[must_use]
    pub const fn events(&self) -> i32 {
        self.events
    }

    /// A timeline's `defaults`: its children's options where theirs are unset.
    #[must_use]
    pub const fn defaults(&self) -> Option<&Transition> {
        self.defaults.as_ref()
    }

    /// The properties, in order.
    #[must_use]
    pub fn props(&self) -> &[(&'static Key, Value)] {
        &self.props
    }

    /// The vars (a copy: an element's props make theirs each render).
    #[must_use]
    pub fn vars(&self) -> Vars {
        Vars {
            props: Cow::Owned(self.props.clone()),
            transition: self.transition.clone(),
            per: Cow::Owned(self.per.clone()),
            events: None,
        }
    }

    /// The vars, these moved in (an imperative motion's, used once).
    #[must_use]
    pub fn into_vars(self) -> Vars {
        Vars { props: Cow::Owned(self.props), transition: self.transition, per: Cow::Owned(self.per), events: None }
    }
}

/// The callbacks of the event bits `mask` ([`DynVars::events`]), each made by
/// `make(bit)` (a binding's: they tell its runtime).
pub fn events(mask: i32, mut make: impl FnMut(i32) -> Box<dyn FnMut()>) -> Option<Box<Events>> {
    if mask == 0 {
        return None;
    }
    let mut on = |bit: i32| (mask & bit != 0).then(|| make(bit));
    Some(Box::new(Events {
        on_start: on(sys::BK_EV_MOTION_START),
        on_update: on(sys::BK_EV_MOTION_UPDATE),
        on_complete: on(sys::BK_EV_MOTION_COMPLETE),
        on_repeat: on(sys::BK_EV_MOTION_REPEAT),
        on_reverse_complete: on(sys::BK_EV_MOTION_REVERSE_COMPLETE),
        on_interrupt: on(sys::BK_EV_MOTION_INTERRUPT),
    }))
}

/// A Motion target prop (`initial`, `animate`, `while*`, `exit`): values, a
/// variant's name, or `false`.
#[derive(Clone, Debug)]
pub enum DynTarget {
    Vars(DynVars),
    Variant(String),
    Off,
}

impl DynTarget {
    pub fn parse(v: &Any) -> Result<Self, String> {
        Ok(match v {
            Any::Map(m) => Self::Vars(DynVars::parse(m)?),
            Any::Str(s) => Self::Variant(s.clone()),
            Any::Bool(false) => Self::Off,
            Any::Bool(true) | Any::Null => Self::Variant(String::new()),
            _ => return Err("a target is an object, a variant's name or false".into()),
        })
    }

    /// Its motion values (UI thread, once).
    pub fn resolve(&mut self, motion: impl Fn(u32) -> Option<MotionValue>) {
        if let Self::Vars(v) = self {
            v.resolve(motion);
        }
    }

    /// The element's target (made each render).
    #[must_use]
    pub fn target(&self) -> Target {
        match self {
            Self::Vars(v) => Target::Vars(v.vars()),
            Self::Variant(s) => Target::Variant(Cow::Owned(s.clone())),
            Self::Off => Target::Off,
        }
    }
}

/// Motion's `variants`: names (interned once) to vars.
pub fn variants(v: &Any) -> Result<Vec<(&'static str, DynVars)>, String> {
    map("variants", v)?.iter().map(|(name, vars)| Ok((intern_str(name)?, DynVars::parse(map(name, vars)?)?))).collect()
}

/// Spelling-insensitive: `repeatDelay`, `repeat_delay`, `repeat-delay`.
fn norm(k: &str) -> String {
    k.chars().filter(|c| *c != '_' && *c != '-').map(|c| c.to_ascii_lowercase()).collect()
}

fn map<'a>(k: &str, v: &'a Any) -> Result<&'a [(String, Any)], String> {
    match v {
        Any::Map(m) => Ok(m),
        _ => Err(format!("`{k}` takes an object")),
    }
}

fn num(k: &str, v: &Any) -> Result<f64, String> {
    match v {
        Any::Num(n) => Ok(*n),
        _ => Err(format!("`{k}` takes a number")),
    }
}

/// A time in seconds: a number (NaN is an error, a negative one is none).
/// Switch to the C engine's own check when it exposes one.
fn time(k: &str, v: &Any) -> Result<f64, String> {
    super::tween::clamp_time(num(k, v)?).ok_or_else(|| format!("`{k}` takes a number of seconds, not NaN"))
}

fn flag(k: &str, v: &Any) -> Result<bool, String> {
    match v {
        Any::Bool(b) => Ok(*b),
        _ => Err(format!("`{k}` takes true or false")),
    }
}

fn text(k: &str, s: &str) -> Result<CString, String> {
    CString::new(s).map_err(|_| format!("`{k}`: a NUL character in \"{s}\""))
}

/// The engine event of a callback key.
fn event(norm_key: &str) -> Option<i32> {
    Some(match norm_key {
        "onstart" | "onanimationstart" => sys::BK_EV_MOTION_START,
        "onupdate" => sys::BK_EV_MOTION_UPDATE,
        "oncomplete" | "onanimationcomplete" => sys::BK_EV_MOTION_COMPLETE,
        "onrepeat" => sys::BK_EV_MOTION_REPEAT,
        "onreversecomplete" => sys::BK_EV_MOTION_REVERSE_COMPLETE,
        "oninterrupt" => sys::BK_EV_MOTION_INTERRUPT,
        _ => return None,
    })
}

/// Set option `k` of `t` from `v`; false when `k` isn't an option (a property).
fn option(slot: Slot, t: &mut Transition, eases: &mut Vec<(Slot, EaseSrc)>, events: &mut i32, k: &str, v: &Any)
          -> Result<bool, String> {
    let n = norm(k);
    if let Some(bit) = event(&n) {
        match v {
            Any::Func => *events |= bit,
            Any::Null => {}
            _ => return Err(format!("`{k}` takes a function")),
        }
        return Ok(true);
    }
    // what the binding reads itself (callbacks' arguments and `this`, GSAP's names for a motion)
    if (n.starts_with("on") && n.ends_with("params")) || matches!(n.as_str(), "callbackscope" | "id" | "data") {
        return Ok(true);
    }
    macro_rules! f32s { ($($name:literal => $field:ident),*) => {
        match n.as_str() {
            $($name => { t.$field = Some(num(k, v)? as f32); return Ok(true); })*
            _ => {}
        }
    }; }
    f32s!("stiffness" => stiffness, "damping" => damping, "mass" => mass, "bounce" => bounce,
          "visualduration" => visual_duration, "velocity" => velocity, "restdelta" => rest_delta,
          "restspeed" => rest_speed, "power" => power, "timeconstant" => time_constant, "min" => min, "max" => max,
          "bouncestiffness" => bounce_stiffness, "bouncedamping" => bounce_damping);
    match n.as_str() {
        "duration" => t.duration = Some(time(k, v)?),
        "delay" => t.delay = Some(time(k, v)?),
        "repeatdelay" => t.repeat_delay = Some(time(k, v)?),
        "timescale" => t.time_scale = Some(num(k, v)?),
        // Motion's Infinity and GSAP's -1: forever
        "repeat" => t.repeat = Some(num(k, v).map(|r| if r.is_finite() && r >= 0.0 { r as i32 } else { -1 })?),
        "yoyo" => t.yoyo = Some(flag(k, v)?),
        "paused" => t.paused = Some(flag(k, v)?),
        "reversed" => t.reversed = Some(flag(k, v)?),
        "immediaterender" | "immediate" => t.immediate = Some(flag(k, v)?),
        "overwrite" => t.overwrite = Some(match v {
            Any::Bool(b) => *b,
            Any::Str(s) if s == "auto" => true,
            _ => return Err("`overwrite` takes true, false or \"auto\"".into()),
        }),
        "stagger" => t.stagger = Some(match v {
            Any::Num(each) => *each,
            Any::Map(m) => {
                let mut each = 0.0;
                for (sk, sv) in m {
                    match norm(sk).as_str() {
                        "each" => each = num(sk, sv)?,
                        _ => return Err(format!("stagger: `{sk}` isn't supported (a number, or {{ each }})")),
                    }
                }
                each
            }
            _ => return Err("`stagger` takes a number or { each }".into()),
        }),
        "type" => t.kind = match v {
            Any::Str(s) => match s.as_str() {
                "spring" => Kind::Spring,
                "tween" | "keyframes" => Kind::Tween,
                "inertia" | "decay" => Kind::Inertia,
                o => return Err(format!("unknown type \"{o}\" (spring, tween, keyframes, inertia, decay)")),
            },
            _ => return Err("`type` takes a string".into()),
        },
        "repeattype" => t.yoyo = Some(match v {
            Any::Str(s) if s == "reverse" || s == "mirror" => true,
            Any::Str(s) if s == "loop" => false,
            _ => return Err("repeatType is \"loop\", \"reverse\" or \"mirror\"".into()),
        }),
        "ease" => eases.push((slot, match v {
            Any::Str(s) => EaseSrc::Name(text(k, s)?),
            Any::List(l) if l.len() == 4 && l.iter().all(|x| matches!(x, Any::Num(_))) => {
                let p = |i: usize| num(k, &l[i]).map(|x| x as f32);
                EaseSrc::Bezier([p(0)?, p(1)?, p(2)?, p(3)?])
            }
            Any::List(l) => EaseSrc::Names(l.iter().map(|x| match x {
                Any::Str(s) => text(k, s),
                _ => Err("`ease` takes a name, [x1, y1, x2, y2] or names per keyframe".into()),
            }).collect::<Result<_, _>>()?),
            _ => return Err("`ease` takes a name, [x1, y1, x2, y2] or names per keyframe".into()),
        })),
        "times" => t.times = Some(match v {
            Any::List(l) => Cow::Owned(l.iter().map(|x| num(k, x).map(|x| x as f32)).collect::<Result<_, _>>()?),
            _ => return Err("`times` takes numbers".into()),
        }),
        _ => return Ok(false),
    }
    Ok(true)
}

/// A property's value (or keyframes).
fn value(k: &str, v: &Any) -> Result<Value, String> {
    Ok(match v {
        Any::Num(n) => Value::Num(*n as f32),
        Any::Bool(b) => Value::Num(f32::from(u8::from(*b))),
        Any::Str(s) => Value::Text(Cow::Owned(text(k, s)?)),
        // Motion's `null` keyframe; a motion value until it is resolved
        Any::Null | Any::Motion(_) => Value::Current,
        Any::List(l) => Value::Keys(Cow::Owned(l.iter().map(|x| match x {
            Any::Num(_) | Any::Str(_) | Any::Null | Any::Bool(_) => value(k, x),
            _ => Err(format!("`{k}`: keyframes are numbers, strings or null")),
        }).collect::<Result<_, _>>()?)),
        Any::Map(_) | Any::Func => return Err(format!("`{k}` isn't an option, and an object isn't a value to animate it to")),
    })
}

/// How many distinct names each table keeps. A name interned stays for the
/// run (the keys and variants that hold it are `&'static`, and nothing says
/// when the last one goes), so the table is bounded instead: a program that
/// makes up names without end is told so, not left to leak. CSS has a few
/// hundred properties and an app a few dozen variants.
const MAX_NAMES: usize = 4096;

type Table<T> = OnceLock<Mutex<HashMap<Box<str>, T>>>;

/// `name`'s entry in `table`, made by `make` the first time.
fn interned<T: Copy>(name: &str, table: &Table<T>, make: impl FnOnce() -> Result<T, String>) -> Result<T, String> {
    let mut m = table.get_or_init(Mutex::default).lock().unwrap_or_else(PoisonError::into_inner);
    if let Some(&t) = m.get(name) {
        return Ok(t);
    }
    if m.len() >= MAX_NAMES {
        return Err(format!("`{name}`: more than {MAX_NAMES} distinct names (property and variant names are fixed at build time, not made up)"));
    }
    let t = make()?;
    m.insert(name.into(), t);
    Ok(t)
}

/// A property key by its runtime name: one per distinct name, for good
/// (like `vars!`'s statics), at most [`MAX_NAMES`].
fn intern_key(name: &str) -> Result<&'static Key, String> {
    static KEYS: Table<&'static Key> = OnceLock::new();
    interned(name, &KEYS, || {
        let c: &'static CStr = Box::leak(text(name, name)?.into_boxed_c_str());
        Ok(&*Box::leak(Box::new(Key::new(c))))
    })
}

/// A variant's name, interned (the element's variants take `&'static str`).
fn intern_str(name: &str) -> Result<&'static str, String> {
    static NAMES: Table<&'static str> = OnceLock::new();
    interned(name, &NAMES, || Ok(Box::leak(name.into())))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn made_up_names_are_bounded() {
        // a table of its own: the real ones are the process's
        static TABLE: Table<&'static str> = OnceLock::new();
        let made = (0..100_000).filter(|i| {
            let name = format!("prop{i}");
            interned(&name, &TABLE, || Ok(Box::leak(name.clone().into()))).is_ok()
        }).count();
        assert_eq!(made, MAX_NAMES, "no more names than the bound are kept (and leaked)");
        assert!(interned("prop0", &TABLE, || Err("known".into())).is_ok(), "known names still answer");
        assert!(interned("prop99999", &TABLE, || Err("new".into())).is_err(), "a name past the bound is an error");
        assert!(intern_key("opacity").is_ok() && intern_str("hidden").is_ok());
    }

    #[test]
    fn times_are_checked() {
        assert_eq!(time("duration", &Any::Num(-1.0)), Ok(0.0));
        assert!(time("duration", &Any::Num(f64::NAN)).is_err());
        assert!(time("duration", &Any::Str("1".into())).is_err());
        assert!(DynVars::parse(&[("delay".into(), Any::Num(f64::NAN))]).is_err());
    }
}
