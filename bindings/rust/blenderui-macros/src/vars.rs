//! `vars!{ x: 100, opacity: 0.5, duration: 1, ease: "power2.out" }` (and rsx's
//! `{{ .. }}`): a GSAP vars / Motion target object, sorted out when the program
//! compiles.
//!
//! - Keys that are timing options (`duration`, `ease`, `type`, `stiffness`,
//!   `repeat`, `onComplete` ...) set the [`Transition`]; `transition: {..}`
//!   nests one (with per-property ones, Motion's `transition: { x: {..} }`).
//! - Any other key is a property: a `static Key` per call site, its engine
//!   id looked up once.
//! - Values of literals (numbers, `"50%"`, `"#f80"`, `null`, `[keyframes]`)
//!   are a `static` array: no allocation, ever. Others are expressions,
//!   converted with `Value::from`.
//! - An ease given by name is parsed once per call site (`static OnceLock`);
//!   `type` must be "spring", "tween", "keyframes", "inertia" or "decay".
//!
//! [`Transition`]: ../blenderui/motion/struct.Transition.html

use proc_macro::{Delimiter, Group, TokenStream, TokenTree};

type R<T> = Result<T, String>;

const M: &str = "::blenderui::motion";

/// `vars!{..}`.
pub fn vars(input: TokenStream) -> TokenStream {
    match object(input) {
        Ok(ts) => ts,
        Err(e) => format!("::core::compile_error!({e:?})").parse().unwrap(),
    }
}

fn ts(s: &str) -> TokenStream {
    s.parse().expect("vars!: scaffold")
}

/// One value as written.
enum V {
    /// a number literal (with its sign)
    Num(String),
    /// a string literal, as a C string literal
    Str(String),
    Null,
    Bool(bool),
    Arr(Vec<V>),
    Obj(TokenStream),
    Expr(TokenStream),
}

impl V {
    fn constant(&self) -> bool {
        match self {
            V::Num(_) | V::Str(_) | V::Null => true,
            V::Arr(a) => a.iter().all(V::constant),
            _ => false,
        }
    }

    /// As a `motion::Value` expression.
    fn value(&self) -> String {
        match self {
            V::Num(n) => format!("{M}::Value::Num(({n}) as f32)"),
            V::Str(s) => format!("{M}::Value::Text(::std::borrow::Cow::Borrowed({s}))"),
            V::Null => format!("{M}::Value::Current"),
            V::Arr(a) if self.constant() => {
                let items: Vec<String> = a.iter().map(V::value).collect();
                format!("{M}::Value::Keys(::std::borrow::Cow::Borrowed(&[{}]))", items.join(", "))
            }
            V::Arr(a) => {
                let items: Vec<String> = a.iter().map(V::value).collect();
                format!("{M}::Value::Keys(::std::borrow::Cow::Owned(::std::vec![{}]))", items.join(", "))
            }
            V::Bool(b) => format!("{M}::Value::Num(({}) as i32 as f32)", b),
            V::Obj(_) | V::Expr(_) => format!("{M}::Value::from({})", self.expr()),
        }
    }

    /// As a plain Rust expression (numbers, strings, closures ...).
    fn expr(&self) -> String {
        match self {
            V::Num(n) => n.clone(),
            V::Str(s) => s.trim_start_matches('c').to_string(),
            V::Null => "::core::option::Option::None::<f32>".into(),
            V::Bool(b) => b.to_string(),
            V::Arr(a) => format!("[{}]", a.iter().map(V::expr).collect::<Vec<_>>().join(", ")),
            V::Obj(t) | V::Expr(t) => format!("({t})"),
        }
    }

    fn text(&self) -> Option<String> {
        match self {
            V::Str(s) => {
                let lit = s.trim_start_matches('c');
                Some(lit.trim_start_matches('r').trim_matches('#').trim_matches('"').to_string())
            }
            _ => None,
        }
    }
}

/// Split at top-level commas.
fn split(stream: TokenStream) -> Vec<Vec<TokenTree>> {
    let mut out = vec![Vec::new()];
    for t in stream {
        match &t {
            TokenTree::Punct(p) if p.as_char() == ',' => out.push(Vec::new()),
            _ => out.last_mut().expect("one").push(t),
        }
    }
    out.retain(|e| !e.is_empty());
    out
}

fn parse_value(toks: &[TokenTree]) -> R<V> {
    match toks {
        [TokenTree::Literal(l)] => {
            let s = l.to_string();
            if s.starts_with('"') || s.starts_with("r\"") || s.starts_with("r#") {
                Ok(V::Str(format!("c{s}")))
            } else if s.starts_with(|c: char| c.is_ascii_digit()) {
                Ok(V::Num(s))
            } else {
                Ok(V::Expr(toks.iter().cloned().collect()))
            }
        }
        [TokenTree::Punct(p), TokenTree::Literal(l)] if p.as_char() == '-'
            && l.to_string().starts_with(|c: char| c.is_ascii_digit()) => Ok(V::Num(format!("-{l}"))),
        [TokenTree::Ident(i)] if i.to_string() == "null" => Ok(V::Null),
        [TokenTree::Ident(i)] if i.to_string() == "true" => Ok(V::Bool(true)),
        [TokenTree::Ident(i)] if i.to_string() == "false" => Ok(V::Bool(false)),
        [TokenTree::Group(g)] if g.delimiter() == Delimiter::Bracket => {
            Ok(V::Arr(split(g.stream()).iter().map(|e| parse_value(e)).collect::<R<_>>()?))
        }
        [TokenTree::Group(g)] if g.delimiter() == Delimiter::Brace => Ok(V::Obj(g.stream())),
        [] => Err("vars!: a key without a value".into()),
        _ => Ok(V::Expr(toks.iter().cloned().collect())),
    }
}

/// `key: value` entries.
fn entries(stream: TokenStream) -> R<Vec<(String, V)>> {
    let mut out = Vec::new();
    for e in split(stream) {
        let colon = e.iter().position(|t| matches!(t, TokenTree::Punct(p) if p.as_char() == ':'))
            .ok_or_else(|| format!("vars!: expected `key: value`, found `{}`",
                                   e.iter().map(ToString::to_string).collect::<String>()))?;
        let key = match &e[..colon] {
            [TokenTree::Ident(i)] => i.to_string().trim_start_matches("r#").to_string(),
            [TokenTree::Literal(l)] if l.to_string().starts_with('"') => l.to_string().trim_matches('"').to_string(),
            k => return Err(format!("vars!: bad key `{}`", k.iter().map(ToString::to_string).collect::<String>())),
        };
        out.push((key, parse_value(&e[colon + 1..])?));
    }
    Ok(out)
}

/// Spelling-insensitive key: `repeatDelay`, `repeat_delay`, `repeat-delay`.
fn norm(k: &str) -> String {
    k.chars().filter(|c| *c != '_' && *c != '-').map(|c| c.to_ascii_lowercase()).collect()
}

/// A transition option: the field and how its value converts.
fn option(key: &str) -> Option<(&'static str, &'static str)> {
    Some(match norm(key).as_str() {
        "duration" => ("duration", "f64"),
        "delay" => ("delay", "f64"),
        "repeat" => ("repeat", "i32"),
        "repeatdelay" => ("repeat_delay", "f64"),
        "yoyo" => ("yoyo", "bool"),
        "timescale" => ("time_scale", "f64"),
        "paused" => ("paused", "bool"),
        "reversed" => ("reversed", "bool"),
        "immediaterender" | "immediate" => ("immediate", "bool"),
        "stagger" => ("stagger", "f64"),
        "stiffness" => ("stiffness", "f32"),
        "damping" => ("damping", "f32"),
        "mass" => ("mass", "f32"),
        "bounce" => ("bounce", "f32"),
        "visualduration" => ("visual_duration", "f32"),
        "velocity" => ("velocity", "f32"),
        "restdelta" => ("rest_delta", "f32"),
        "restspeed" => ("rest_speed", "f32"),
        "power" => ("power", "f32"),
        "timeconstant" => ("time_constant", "f32"),
        "min" => ("min", "f32"),
        "max" => ("max", "f32"),
        "bouncestiffness" => ("bounce_stiffness", "f32"),
        "bouncedamping" => ("bounce_damping", "f32"),
        "type" | "ease" | "times" | "repeattype" | "overwrite" | "transition" | "default" => ("", ""),
        k if event(k).is_some() => ("", ""),
        _ => return None,
    })
}

fn event(norm_key: &str) -> Option<&'static str> {
    Some(match norm_key {
        "onstart" | "onanimationstart" => "on_start",
        "onupdate" => "on_update",
        "oncomplete" | "onanimationcomplete" => "on_complete",
        "onrepeat" => "on_repeat",
        "onreversecomplete" => "on_reverse_complete",
        "oninterrupt" => "on_interrupt",
        _ => return None,
    })
}

/// A string literal's ease, parsed once per call site.
fn ease_cell(s: &V) -> String {
    let V::Str(lit) = s else { unreachable!() };
    format!("{{ static __E: ::std::sync::OnceLock<{M}::Ease> = ::std::sync::OnceLock::new(); \
             *__E.get_or_init(|| {M}::ease({lit})) }}")
}

/// Statements setting transition `t` from option `key: v`.
fn set_option(t: &str, key: &str, v: &V, events: &mut Vec<String>) -> R<String> {
    let k = norm(key);
    if let Some(ev) = event(&k) {
        events.push(format!("__ev.{ev} = ::core::option::Option::Some(::std::boxed::Box::new({}));", v.expr()));
        return Ok(String::new());
    }
    Ok(match k.as_str() {
        "type" => {
            let kind = match v.text().as_deref() {
                Some("spring") => "Spring",
                Some("tween" | "keyframes") => "Tween",
                Some("inertia" | "decay") => "Inertia",
                Some(o) => return Err(format!("vars!: unknown type \"{o}\" (spring, tween, keyframes, inertia, decay)")),
                None => return Err("vars!: `type` takes a string".into()),
            };
            format!("{t}.kind = {M}::Kind::{kind};")
        }
        "repeattype" => match v.text().as_deref() {
            Some("reverse" | "mirror") => format!("{t}.yoyo = ::core::option::Option::Some(true);"),
            Some("loop") => format!("{t}.yoyo = ::core::option::Option::Some(false);"),
            _ => return Err("vars!: repeatType is \"loop\", \"reverse\" or \"mirror\"".into()),
        },
        "overwrite" => match v {
            V::Bool(b) => format!("{t}.overwrite = ::core::option::Option::Some({b});"),
            V::Str(_) => format!("{t}.overwrite = ::core::option::Option::Some(true);"), // "auto"
            _ => format!("{t}.overwrite = ::core::option::Option::Some({});", v.expr()),
        },
        "ease" => match v {
            V::Str(_) => format!("{t}.ease = ::core::option::Option::Some({});", ease_cell(v)),
            V::Arr(a) if a.len() == 4 && a.iter().all(|x| matches!(x, V::Num(_))) => {
                let n: Vec<String> = a.iter().map(|x| format!("({}) as f32", x.expr())).collect();
                format!("{t}.ease = ::core::option::Option::Some({M}::Ease::bezier({}));", n.join(", "))
            }
            V::Arr(a) if a.iter().all(|x| matches!(x, V::Str(_))) => {
                // one ease per keyframe segment, parsed once
                let names: Vec<String> = a.iter().map(|x| match x { V::Str(s) => format!("{M}::ease({s})"), _ => unreachable!() }).collect();
                format!("{t}.eases = ::core::option::Option::Some(::std::borrow::Cow::Borrowed({{ \
                         static __E: ::std::sync::OnceLock<[{M}::Ease; {n}]> = ::std::sync::OnceLock::new(); \
                         &__E.get_or_init(|| [{}])[..] }}));", names.join(", "), n = a.len())
            }
            _ => format!("{t}.ease = ::core::option::Option::Some(::core::convert::Into::into({}));", v.expr()),
        },
        "times" => match v {
            V::Arr(a) if v.constant() => {
                let n: Vec<String> = a.iter().map(|x| format!("({}) as f32", x.expr())).collect();
                format!("{t}.times = ::core::option::Option::Some(::std::borrow::Cow::Borrowed(&[{}]));", n.join(", "))
            }
            _ => format!("{t}.times = ::core::option::Option::Some(::std::borrow::Cow::Owned(\
                          ::std::iter::IntoIterator::into_iter({}).map(|x| x as f32).collect()));", v.expr()),
        },
        _ => {
            let (field, ty) = option(key).ok_or_else(|| format!("vars!: `{key}` isn't an option"))?;
            let conv = match (ty, v) {
                ("bool", _) => v.expr(),
                (_, _) => format!("({}) as {ty}", v.expr()),
            };
            format!("{t}.{field} = ::core::option::Option::Some({conv});")
        }
    })
}

/// The `Vars` expression for an object's contents.
pub fn object(stream: TokenStream) -> R<TokenStream> {
    let items = entries(stream)?;
    let mut statics = String::new(); // property keys
    let mut props: Vec<(usize, &V)> = Vec::new();
    let mut body = String::from("#[allow(unused_mut)] let mut __t = <" );
    body.push_str(M);
    body.push_str("::Transition as ::core::default::Default>::default();");
    let mut per = Vec::new();
    let mut events = Vec::new();
    let mut nkeys = 0;
    let mut key_static = |name: &str, statics: &mut String| {
        let id = nkeys;
        nkeys += 1;
        statics.push_str(&format!("static __K{id}: {M}::Key = {M}::Key::new(c{name:?});"));
        id
    };
    for (key, v) in &items {
        let k = norm(key);
        if k == "transition" {
            let V::Obj(inner) = v else { return Err("vars!: `transition` takes an object".into()) };
            for (ik, iv) in entries(inner.clone())? {
                if norm(&ik) == "default" {
                    let V::Obj(d) = &iv else { return Err("vars!: `default` takes an object".into()) };
                    for (dk, dv) in entries(d.clone())? {
                        body.push_str(&set_option("__t", &dk, &dv, &mut events)?);
                    }
                } else if option(&ik).is_some() {
                    body.push_str(&set_option("__t", &ik, &iv, &mut events)?);
                } else {
                    // a property's own transition
                    let V::Obj(p) = &iv else { return Err(format!("vars!: transition `{ik}` takes an object")) };
                    let id = key_static(&ik, &mut statics);
                    let mut pt = format!("#[allow(unused_mut)] let mut __p = <{M}::Transition as ::core::default::Default>::default();");
                    for (pk, pv) in entries(p.clone())? {
                        pt.push_str(&set_option("__p", &pk, &pv, &mut events)?);
                    }
                    per.push(format!("{{ {pt} (&__K{id}, __p) }}"));
                }
            }
        } else if option(key).is_some() {
            body.push_str(&set_option("__t", key, v, &mut events)?);
        } else {
            props.push((key_static(key, &mut statics), v));
        }
    }
    let all_const = props.iter().all(|(_, v)| v.constant());
    let pairs: Vec<String> = props.iter().map(|(id, v)| format!("(&__K{id}, {})", v.value())).collect();
    let props_expr = if props.is_empty() {
        "::std::borrow::Cow::Borrowed(&[])".to_string()
    } else if all_const {
        statics.push_str(&format!("static __PROPS: [(&{M}::Key, {M}::Value); {}] = [{}];", pairs.len(), pairs.join(", ")));
        "::std::borrow::Cow::Borrowed(&__PROPS)".to_string()
    } else {
        format!("::std::borrow::Cow::Owned(::std::vec![{}])", pairs.join(", "))
    };
    let per_expr = if per.is_empty() {
        "::std::borrow::Cow::Borrowed(&[])".to_string()
    } else {
        format!("::std::borrow::Cow::Owned(::std::vec![{}])", per.join(", "))
    };
    let events_expr = if events.is_empty() {
        "::core::option::Option::None".to_string()
    } else {
        format!("{{ let mut __ev = <{M}::Events as ::core::default::Default>::default(); {} \
                 ::core::option::Option::Some(::std::boxed::Box::new(__ev)) }}", events.join(" "))
    };
    let code = format!("{{ {statics} {body} {M}::Vars {{ props: {props_expr}, transition: __t, per: {per_expr}, \
                        events: {events_expr} }} }}");
    Ok(TokenStream::from(TokenTree::Group(Group::new(Delimiter::Brace, ts(&code)))))
}
