//! What gets animated ([`Key`]: a property, interned once per call site) and
//! to what ([`Value`]: a number, a CSS value as text, a color, keyframes).
//!
//! Text values (`"50%"`, `"+=100"`, `"#f80"`, `"rgb(0 0 0 / 50%)"`) are read
//! by the engine when a motion starts, never per frame.

use std::borrow::Cow;
use std::ffi::{CStr, CString};
use std::sync::atomic::{AtomicI32, Ordering};

use blenderui_sys as sys;

use crate::color::Rgba;

/// A property by name (`x`, `scale`, `backgroundColor`, `border-radius` ...):
/// one per call site (`vars!` makes them `static`), its engine id looked up
/// the first time it's used.
#[derive(Debug)]
pub struct Key {
    name: &'static CStr,
    id: AtomicI32,
}

impl Key {
    /// A property by its name (GSAP's / Motion's camelCase or CSS's kebab-case).
    #[must_use]
    pub const fn new(name: &'static CStr) -> Key {
        Key { name, id: AtomicI32::new(0) }
    }

    /// The engine's id for it.
    #[inline]
    pub fn id(&self) -> i32 {
        let id = self.id.load(Ordering::Relaxed);
        if id != 0 {
            return id;
        }
        // SAFETY: a NUL-terminated name.
        let id = unsafe { sys::BK_anim_prop(self.name.as_ptr()) };
        self.id.store(id, Ordering::Relaxed);
        id
    }

    /// Its name as written.
    #[must_use]
    pub fn name(&self) -> &'static CStr {
        self.name
    }
}

/// The transform shorthands and the common properties, already resolved.
pub mod keys {
    use super::Key;
    use std::sync::atomic::AtomicI32;

    macro_rules! fixed {
        ($($name:ident = $id:ident, $s:literal;)*) => {$(
            pub static $name: Key = Key { name: $s, id: AtomicI32::new(blenderui_sys::$id) };
        )*};
    }
    fixed! {
        X = BK_PROP_X, c"x";
        Y = BK_PROP_Y, c"y";
        Z = BK_PROP_Z, c"z";
        SCALE = BK_PROP_SCALE, c"scale";
        SCALE_X = BK_PROP_SCALE_X, c"scaleX";
        SCALE_Y = BK_PROP_SCALE_Y, c"scaleY";
        ROTATE = BK_PROP_ROTATE, c"rotate";
        ROTATE_X = BK_PROP_ROTATE_X, c"rotateX";
        ROTATE_Y = BK_PROP_ROTATE_Y, c"rotateY";
        SKEW_X = BK_PROP_SKEW_X, c"skewX";
        SKEW_Y = BK_PROP_SKEW_Y, c"skewY";
        OPACITY = BK_PROP_OPACITY, c"opacity";
        WIDTH = BK_PROP_WIDTH, c"width";
        HEIGHT = BK_PROP_HEIGHT, c"height";
        COLOR = BK_PROP_COLOR, c"color";
        BACKGROUND_COLOR = BK_PROP_BACKGROUND_COLOR, c"backgroundColor";
        VALUE = BK_PROP_VALUE, c"value";
    }
}

/// A value to animate to (or a keyframe).
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    /// A number: pixels for lengths, degrees for angles, as is otherwise.
    Num(f32),
    /// A CSS value: `"50%"`, `"2rem"`, `"45deg"`, `"+=100"`, `"#f80"`, `"red"`.
    Text(Cow<'static, CStr>),
    Color(Rgba),
    /// Keyframes (Motion's arrays, GSAP's keyframe values).
    Keys(Cow<'static, [Value]>),
    /// The value it has when the motion starts (Motion's `null` keyframe).
    Current,
    /// A [`MotionValue`](super::MotionValue)'s value, read every frame (`style={{ x: mv }}`).
    Motion(super::MotionValue),
}

impl From<f32> for Value {
    fn from(v: f32) -> Value {
        Value::Num(v)
    }
}

impl From<f64> for Value {
    fn from(v: f64) -> Value {
        Value::Num(v as f32)
    }
}

impl From<i32> for Value {
    fn from(v: i32) -> Value {
        Value::Num(v as f32)
    }
}

impl From<Rgba> for Value {
    fn from(c: Rgba) -> Value {
        Value::Color(c)
    }
}

impl From<&'static CStr> for Value {
    fn from(s: &'static CStr) -> Value {
        Value::Text(Cow::Borrowed(s))
    }
}

impl From<&str> for Value {
    fn from(s: &str) -> Value {
        Value::Text(Cow::Owned(CString::new(s).unwrap_or_default()))
    }
}

impl From<String> for Value {
    fn from(s: String) -> Value {
        Value::Text(Cow::Owned(CString::new(s).unwrap_or_default()))
    }
}

impl<T: Into<Value>, const N: usize> From<[T; N]> for Value {
    fn from(keys: [T; N]) -> Value {
        Value::Keys(Cow::Owned(keys.into_iter().map(Into::into).collect()))
    }
}

impl<T: Into<Value>> From<Vec<T>> for Value {
    fn from(keys: Vec<T>) -> Value {
        Value::Keys(Cow::Owned(keys.into_iter().map(Into::into).collect()))
    }
}

impl<T: Into<Value>> From<Option<T>> for Value {
    /// `None`: the current value (Motion's `null` keyframe).
    fn from(v: Option<T>) -> Value {
        v.map_or(Value::Current, Into::into)
    }
}

/// One value read for the engine: up to 4 numbers (a color: 4), its unit and
/// whether it is relative (`+=` / `-=`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct Read {
    pub v: [f32; 4],
    pub n: u8,
    pub unit: i32,
    pub rel: i8,
    pub current: bool,
}

impl Read {
    pub const CURRENT: Read = Read { v: [0.0; 4], n: 1, unit: sys::BK_UNIT_NONE, rel: 0, current: true };
}

impl Value {
    /// One (non-keyframe) value as numbers; `None` for text that isn't a value.
    pub(crate) fn read(&self) -> Option<Read> {
        match self {
            Value::Num(v) => Some(Read { v: [*v, 0.0, 0.0, 0.0], n: 1, unit: sys::BK_UNIT_NONE, ..Read::default() }),
            Value::Color(c) => Some(Read { v: [c.r, c.g, c.b, c.a], n: 4, unit: sys::BK_UNIT_COLOR, ..Read::default() }),
            Value::Text(s) => {
                let (mut r, mut unit, mut rel) = (Read::default(), 0, 0);
                // SAFETY: a NUL-terminated string and valid out pointers.
                let n = unsafe { sys::BK_anim_parse_value(s.as_ptr(), r.v.as_mut_ptr(), &raw mut unit, &raw mut rel) };
                (n > 0).then(|| Read { n: n as u8, unit, rel: rel as i8, ..r })
            }
            Value::Current => Some(Read::CURRENT),
            Value::Motion(m) => Some(m.read()),
            Value::Keys(k) => k.last().and_then(Value::read),
        }
    }

    /// Equal values: the same `static` (a literal object's, every render) at
    /// once, else by value.
    pub(crate) fn same(&self, o: &Value) -> bool {
        match (self, o) {
            (Value::Keys(Cow::Borrowed(a)), Value::Keys(Cow::Borrowed(b))) if std::ptr::eq(*a, *b) => true,
            (Value::Text(Cow::Borrowed(a)), Value::Text(Cow::Borrowed(b))) if std::ptr::eq(*a, *b) => true,
            _ => self == o,
        }
    }

    /// The keyframes (one for a plain value).
    pub(crate) fn keys(&self) -> &[Value] {
        match self {
            Value::Keys(k) => k,
            v => std::slice::from_ref(v),
        }
    }
}
