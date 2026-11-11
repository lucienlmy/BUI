//! Animation from JavaScript (`@blender-ui/native/gsap`, `/motion`): the
//! boundary. Each call reads its JavaScript objects once, here (errors throw
//! at the caller, naming the option), and hands them to the UI thread
//! (`blenderui_jsview::motion`), which runs them on `blenderui::motion`.
//! Nothing crosses per frame but the events a motion's callbacks want.
//!
//! SPDX-License-Identifier: GPL-2.0-or-later

use blenderui::motion::{Any, DynVars};
use blenderui_jsview::motion as m;
use napi::bindgen_prelude::*;
use napi::{JsBoolean, JsNumber, JsObject, JsString, JsUnknown, ValueType};
use napi_derive::napi;

use crate::NativeApp;

/// The property a JavaScript `MotionValue` carries its handle in.
const MOTION_VALUE: &str = "__buiMotionValue";

/// How deep objects nest (a cycle stops here).
const DEPTH: u32 = 16;

fn err(e: impl Into<String>) -> Error {
    Error::new(Status::InvalidArg, e.into())
}

/// A JavaScript value as the vars grammar reads it (`None`: undefined).
fn any(v: JsUnknown, depth: u32) -> Result<Option<Any>> {
    // SAFETY (the casts): each is to the type `get_type` reported.
    Ok(Some(match v.get_type()? {
        ValueType::Undefined => return Ok(None),
        ValueType::Null => Any::Null,
        ValueType::Boolean => Any::Bool(unsafe { v.cast::<JsBoolean>() }.get_value()?),
        ValueType::Number => Any::Num(unsafe { v.cast::<JsNumber>() }.get_double()?),
        ValueType::String => Any::Str(unsafe { v.cast::<JsString>() }.into_utf8()?.into_owned()?),
        ValueType::Function => Any::Func,
        ValueType::Object => {
            let depth = depth.checked_sub(1).ok_or_else(|| err("motion: objects nest too deeply (a cycle?)"))?;
            let o: JsObject = unsafe { v.cast() };
            if o.is_array()? {
                let n = o.get_array_length()?;
                let mut l = Vec::with_capacity(n as usize);
                for i in 0..n {
                    l.push(any(o.get_element::<JsUnknown>(i)?, depth)?.unwrap_or(Any::Null));
                }
                Any::List(l)
            } else {
                let mv = o.get_named_property::<JsUnknown>(MOTION_VALUE)?;
                if mv.get_type()? == ValueType::Number {
                    Any::Motion(unsafe { mv.cast::<JsNumber>() }.get_uint32()?)
                } else {
                    Any::Map(entries(&o, depth)?)
                }
            }
        }
        t => return Err(err(format!("motion: a {t} isn't a value"))),
    }))
}

/// An object's own and inherited enumerable entries (undefined ones left out).
fn entries(o: &JsObject, depth: u32) -> Result<Vec<(String, Any)>> {
    let names = o.get_property_names()?;
    let n = names.get_array_length()?;
    let mut out = Vec::with_capacity(n as usize);
    for i in 0..n {
        let k = names.get_element::<JsString>(i)?.into_utf8()?.into_owned()?;
        if let Some(v) = any(o.get_named_property::<JsUnknown>(&k)?, depth)? {
            out.push((k, v));
        }
    }
    Ok(out)
}

fn vars(o: &JsObject) -> Result<DynVars> {
    DynVars::parse(&entries(o, DEPTH)?).map_err(err)
}

/// Commit's Motion props: `[which, value, which, value ...]`.
pub(crate) fn props(list: Option<JsObject>) -> Result<Vec<Option<m::MotionProp>>> {
    let Some(list) = list else { return Ok(Vec::new()) };
    let n = list.get_array_length()?;
    let mut out = Vec::with_capacity(n as usize / 2);
    for i in (0..n.saturating_sub(1)).step_by(2) {
        let which = list.get_element::<JsNumber>(i)?.get_int32()?;
        let v = any(list.get_element::<JsUnknown>(i + 1)?, DEPTH)?.unwrap_or(Any::Null);
        out.push(Some(m::MotionProp::parse(which, &v).map_err(err)?));
    }
    Ok(out)
}

#[napi]
impl NativeApp {
    /// `gsap.to / from / fromTo / set`, Motion's `animate` (`verb`) as motion
    /// `id`: on elements (`selector`), motion values (`values`, handles) or a
    /// plain `object`'s numbers. `extra`: fromTo's from-vars, animate's
    /// options. In `timeline` at `position`, else held for JavaScript.
    /// `events`: event bits wanted besides the callbacks' (`then`, cleanup).
    /// Returns an object target's animated properties, in the order its
    /// `motion` events give their values.
    #[napi]
    #[allow(clippy::too_many_arguments)]
    pub fn motion_tween(&self, id: u32, verb: String, selector: Option<String>, values: Option<Vec<u32>>,
                        object: Option<JsObject>, vars_obj: JsObject, extra: Option<JsObject>, timeline: Option<u32>,
                        position: Option<String>, events: i32) -> Result<Option<Vec<String>>> {
        let verb = match verb.as_str() {
            "to" => m::Verb::To,
            "from" => m::Verb::From,
            "fromTo" => m::Verb::FromTo,
            "set" => m::Verb::Set,
            "animate" => m::Verb::Animate,
            v => return Err(err(format!("motion: no verb {v}"))),
        };
        let mut d = vars(&vars_obj)?;
        let mut from = None;
        if let Some(x) = &extra {
            match verb {
                m::Verb::FromTo => from = Some(vars(x)?),
                m::Verb::Animate => d.add_transition(&entries(x, DEPTH)?).map_err(err)?,
                _ => {}
            }
        }
        let target = match (selector, values, object) {
            (Some(s), _, _) => m::Target::Selector(s),
            (_, Some(v), _) => m::Target::Values(v),
            (_, _, Some(o)) => {
                // where each property starts: the object's number now
                let mut start = Vec::with_capacity(d.props().len());
                for (k, _) in d.props() {
                    let name = k.name().to_str().map_err(|_| err("motion: a property name isn't UTF-8"))?;
                    let v = o.get_named_property::<JsUnknown>(name)?;
                    let n = if v.get_type()? == ValueType::Number { unsafe { v.cast::<JsNumber>() }.get_double()? } else { 0.0 };
                    start.push((name.to_string(), n as f32));
                }
                m::Target::Object(start)
            }
            _ => return Err(err("motion: no targets")),
        };
        let names = match &target {
            m::Target::Object(p) => Some(p.iter().map(|(k, _)| k.clone()).collect()),
            _ => None,
        };
        let mask = d.events() | events;
        let into = timeline.map(|t| (t, position.unwrap_or_default()));
        self.link.send(move |cx| m::tween(cx, id, verb, target, d, from, into, mask));
        Ok(names)
    }

    /// `gsap.timeline(vars)` as motion `id`.
    #[napi]
    pub fn motion_timeline(&self, id: u32, vars_obj: Option<JsObject>, events: i32) -> Result<()> {
        let d = match &vars_obj {
            Some(o) => vars(o)?,
            None => DynVars::default(),
        };
        let mask = d.events() | events;
        self.link.send(move |cx| m::timeline(cx, id, d, mask));
        Ok(())
    }

    /// `tl.add(child, position)` (both held).
    #[napi]
    pub fn motion_add(&self, timeline: u32, child: u32, position: String) {
        self.link.send(move |cx| m::add(cx, timeline, child, &position));
    }

    /// `tl.addLabel(name, position)`.
    #[napi]
    pub fn motion_label(&self, timeline: u32, name: String, position: String) {
        self.link.send(move |cx| m::label(cx, timeline, &name, &position));
    }

    /// `tl.call(fn, params, position)`: a `motion` event for `id` there.
    #[napi]
    pub fn motion_call(&self, timeline: u32, id: u32, position: String) {
        self.link.send(move |cx| m::call(cx, timeline, id, &position));
    }

    /// A control: play, pause, resume, reverse, restart, complete, kill,
    /// seek, progress, totalProgress, timeScale.
    #[napi]
    pub fn motion_control(&self, id: u32, op: String, value: Option<f64>) {
        self.link.send(move |cx| m::control(cx, id, &op, value.unwrap_or(0.0)));
    }

    /// A query (time, totalTime, duration, totalDuration, progress, timeScale,
    /// isActive, paused, reversed, or a label's time): NaN for none.
    #[napi]
    pub fn motion_query(&self, id: u32, what: String) -> f64 {
        self.ask(move |cx| m::query(cx, id, &what)).unwrap_or(f64::NAN)
    }

    /// JavaScript dropped motion `id`.
    #[napi]
    pub fn motion_release(&self, id: u32) {
        self.link.send(move |cx| m::release(cx, id));
    }

    /// A motion value `id` starting at `init` (a number or a CSS value).
    #[napi]
    pub fn motion_value(&self, id: u32, init: JsUnknown) -> Result<()> {
        let v = any(init, 0)?.unwrap_or(Any::Num(0.0));
        self.link.send(move |cx| m::value_new(cx, id, &v));
        Ok(())
    }

    #[napi]
    pub fn motion_value_set(&self, id: u32, value: JsUnknown) -> Result<()> {
        let v = any(value, 0)?.unwrap_or(Any::Num(0.0));
        self.link.send(move |cx| m::value_set(cx, id, &v));
        Ok(())
    }

    /// Its value now (or, `velocity`, its speed per second).
    #[napi]
    pub fn motion_value_get(&self, id: u32, velocity: bool) -> f64 {
        self.ask(move |cx| m::value_get(cx, id, velocity)).unwrap_or(f64::NAN)
    }

    #[napi]
    pub fn motion_value_dispose(&self, id: u32) {
        self.link.send(move |cx| m::value_dispose(cx, id));
    }

    /// `CustomEase.create(name, svgPath)`: whether the path was read.
    #[napi]
    pub fn register_ease(&self, name: String, path: String) -> bool {
        self.ask(move |_| m::register_ease(&name, &path)).unwrap_or(false)
    }

    /// GSAP's `ticker.lagSmoothing(threshold, adjusted)` (seconds; 0: off).
    #[napi]
    pub fn motion_lag_smoothing(&self, threshold: f64, adjusted: f64) {
        self.link.send(move |_| blenderui::motion::lag_smoothing(threshold, adjusted));
    }

    /// Move the animation clock `ms` forward; headless, render every window
    /// (tests: frame-exact values). Returns once done.
    #[napi]
    pub fn advance_clock(&self, ms: f64) {
        let draw = self.headless;
        self.ask(move |cx| m::advance_clock(cx, ms, draw));
    }
}
