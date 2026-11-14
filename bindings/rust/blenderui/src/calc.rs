//! CSS math: `calc()`, `min()`, `max()`, `clamp()`, `round()`, `mod()`,
//! `rem()`, `abs()`, `sign()`, trigonometry, `pow()`, `sqrt()`, `hypot()`,
//! `log()`, `exp()` and the constants `e`, `pi`, `infinity`, `NaN`, over
//! every length unit and angles.
//!
//! Expressions are interned: each distinct one is parsed once and lives for
//! the rest of the process, so lengths can hold a `&'static Calc` (they stay
//! `Copy`) and taffy can carry a resolved copy as its opaque calc pointer
//! (`tree.rs` evaluates it against the percentage basis during layout).

use crate::css::Tok;
use crate::geometry::{Units, ROOT_FONT_SIZE};
use crate::style::{key_hasher, KeyMap};
use std::hash::{Hash, Hasher};
use std::sync::{Mutex, OnceLock};

/// A math expression. Leaves keep their unit until [`Calc::resolve`]
/// bakes the font size and viewport in, leaving only px and %.
#[derive(Debug, PartialEq)]
#[repr(align(8))]
pub enum Calc {
    Num(f32),
    Px(f32),
    Pct(f32),
    Em(f32),
    Rem(f32),
    Vw(f32),
    Vh(f32),
    Vmin(f32),
    Vmax(f32),
    Add(Box<Calc>, Box<Calc>),
    Sub(Box<Calc>, Box<Calc>),
    Mul(Box<Calc>, Box<Calc>),
    Div(Box<Calc>, Box<Calc>),
    Min(Vec<Calc>),
    Max(Vec<Calc>),
    Clamp(Box<Calc>, Box<Calc>, Box<Calc>),
    /// round(strategy, value, step)
    Round(RoundMode, Box<Calc>, Box<Calc>),
    Mod(Box<Calc>, Box<Calc>),
    Rem2(Box<Calc>, Box<Calc>),
    Fn1(Fn1, Box<Calc>),
    Pow(Box<Calc>, Box<Calc>),
    Atan2(Box<Calc>, Box<Calc>),
    Log(Box<Calc>, Option<Box<Calc>>),
    Hypot(Vec<Calc>),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RoundMode {
    Nearest,
    Up,
    Down,
    ToZero,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Fn1 {
    Abs,
    Sign,
    Sqrt,
    Exp,
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
}

/// Names of functions that start a math expression.
pub fn is_math_fn(name: &str) -> bool {
    matches!(name, "calc" | "min" | "max" | "clamp" | "round" | "mod" | "rem" | "abs" | "sign"
        | "sqrt" | "exp" | "sin" | "cos" | "tan" | "asin" | "acos" | "atan" | "atan2" | "pow"
        | "hypot" | "log" | "-webkit-calc" | "-moz-calc")
}

impl Calc {
    /// Parse a math function (`calc(...)`, `min(...)` ...) into an interned expression.
    pub fn parse_fn(name: &str, args: &[Vec<Tok>]) -> Option<&'static Calc> {
        let c = Parser::function(name, args)?;
        Some(intern(c))
    }

    /// Whether the value depends on the percentage basis.
    pub fn has_pct(&self) -> bool {
        use Calc::*;
        match self {
            Pct(_) => true,
            Num(_) | Px(_) | Em(_) | Rem(_) | Vw(_) | Vh(_) | Vmin(_) | Vmax(_) => false,
            Add(a, b) | Sub(a, b) | Mul(a, b) | Div(a, b) | Mod(a, b) | Rem2(a, b) | Pow(a, b)
            | Atan2(a, b) | Round(_, a, b) => a.has_pct() || b.has_pct(),
            Min(v) | Max(v) | Hypot(v) => v.iter().any(Calc::has_pct),
            Clamp(a, b, c) => a.has_pct() || b.has_pct() || c.has_pct(),
            Fn1(_, a) => a.has_pct(),
            Log(a, b) => a.has_pct() || b.as_ref().is_some_and(|b| b.has_pct()),
        }
    }

    /// Whether the expression is a plain number (no length or percentage
    /// leaf): `calc(1.25 / 0.875)`, which `line-height` takes as a factor.
    pub fn is_number(&self) -> bool {
        use Calc::*;
        match self {
            Num(_) => true,
            Px(_) | Pct(_) | Em(_) | Rem(_) | Vw(_) | Vh(_) | Vmin(_) | Vmax(_) => false,
            Add(a, b) | Sub(a, b) | Mul(a, b) | Div(a, b) | Mod(a, b) | Rem2(a, b) | Pow(a, b)
            | Atan2(a, b) | Round(_, a, b) => a.is_number() && b.is_number(),
            Min(v) | Max(v) | Hypot(v) => v.iter().all(Calc::is_number),
            Clamp(a, b, c) => a.is_number() && b.is_number() && c.is_number(),
            Fn1(_, a) => a.is_number(),
            Log(a, b) => a.is_number() && b.as_ref().is_none_or(|b| b.is_number()),
        }
    }

    /// The value in px, percentages taken of `basis`.
    pub fn eval_units(&self, u: &Units, basis: f32) -> f32 {
        use Calc::*;
        let e = |c: &Calc| c.eval_units(u, basis);
        match self {
            Num(v) | Px(v) => *v,
            Pct(p) => p * basis / 100.0,
            Em(v) => v * u.font_size,
            Rem(v) => v * ROOT_FONT_SIZE,
            Vw(v) => v * u.viewport.0 / 100.0,
            Vh(v) => v * u.viewport.1 / 100.0,
            Vmin(v) => v * u.viewport.0.min(u.viewport.1) / 100.0,
            Vmax(v) => v * u.viewport.0.max(u.viewport.1) / 100.0,
            Add(a, b) => e(a) + e(b),
            Sub(a, b) => e(a) - e(b),
            Mul(a, b) => e(a) * e(b),
            Div(a, b) => e(a) / e(b),
            Min(v) => v.iter().map(e).fold(f32::INFINITY, f32::min),
            Max(v) => v.iter().map(e).fold(f32::NEG_INFINITY, f32::max),
            // clamp(MIN, VAL, MAX) = max(MIN, min(VAL, MAX))
            Clamp(lo, v, hi) => e(lo).max(e(v).min(e(hi))),
            Round(m, v, s) => round(*m, e(v), e(s)),
            Mod(a, b) => {
                let (a, b) = (e(a), e(b));
                a - b * (a / b).floor()
            }
            Rem2(a, b) => e(a) % e(b),
            Fn1(f, a) => {
                let a = e(a);
                match f {
                    self::Fn1::Abs => a.abs(),
                    self::Fn1::Sign => if a == 0.0 { 0.0 } else { a.signum() },
                    self::Fn1::Sqrt => a.sqrt(),
                    self::Fn1::Exp => a.exp(),
                    self::Fn1::Sin => a.sin(),
                    self::Fn1::Cos => a.cos(),
                    self::Fn1::Tan => a.tan(),
                    self::Fn1::Asin => a.asin(),
                    self::Fn1::Acos => a.acos(),
                    self::Fn1::Atan => a.atan(),
                }
            }
            Pow(a, b) => e(a).powf(e(b)),
            Atan2(a, b) => e(a).atan2(e(b)),
            Log(a, b) => match b {
                Some(b) => e(a).ln() / e(b).ln(),
                None => e(a).ln(),
            },
            Hypot(v) => v.iter().map(|c| e(c).powi(2)).sum::<f32>().sqrt(),
        }
    }

    /// Pixels when the value doesn't need a percentage basis.
    pub fn to_px(&self, u: &Units) -> Option<f32> {
        (!self.has_pct()).then(|| self.eval_units(u, 0.0))
    }

    /// A copy with font-relative and viewport units baked in (px and %
    /// remain), interned: what taffy evaluates during layout.
    pub fn resolve(&self, u: &Units) -> &'static Resolved {
        Resolved::new(intern(self.bake(u)))
    }

    fn bake(&self, u: &Units) -> Calc {
        self.bake_with(&|leaf| match leaf {
            Calc::Num(x) => Calc::Num(*x),
            Calc::Pct(x) => Calc::Pct(*x),
            other => Calc::Px(other.eval_units(u, 0.0)),
        })
    }

    /// A copy with every leaf mapped by `f`.
    fn bake_with(&self, f: &dyn Fn(&Calc) -> Calc) -> Calc {
        use Calc::*;
        let b = |c: &Calc| Box::new(c.bake_with(f));
        let v = |l: &[Calc]| l.iter().map(|c| c.bake_with(f)).collect();
        match self {
            Num(_) | Pct(_) | Px(_) | Em(_) | Rem(_) | Vw(_) | Vh(_) | Vmin(_) | Vmax(_) => f(self),
            Add(a, c) => Add(b(a), b(c)),
            Sub(a, c) => Sub(b(a), b(c)),
            Mul(a, c) => Mul(b(a), b(c)),
            Div(a, c) => Div(b(a), b(c)),
            Min(l) => Min(v(l)),
            Max(l) => Max(v(l)),
            Clamp(a, c, d) => Clamp(b(a), b(c), b(d)),
            Round(m, a, c) => Round(*m, b(a), b(c)),
            Mod(a, c) => Mod(b(a), b(c)),
            Rem2(a, c) => Rem2(b(a), b(c)),
            Fn1(f, a) => Fn1(*f, b(a)),
            Pow(a, c) => Pow(b(a), b(c)),
            Atan2(a, c) => Atan2(b(a), b(c)),
            Log(a, c) => Log(b(a), c.as_ref().map(|c| b(c))),
            Hypot(l) => Hypot(v(l)),
        }
    }

    fn hash_into<H: Hasher>(&self, h: &mut H) {
        use Calc::*;
        std::mem::discriminant(self).hash(h);
        match self {
            Num(x) | Px(x) | Pct(x) | Em(x) | Rem(x) | Vw(x) | Vh(x) | Vmin(x) | Vmax(x) => {
                x.to_bits().hash(h)
            }
            Add(a, b) | Sub(a, b) | Mul(a, b) | Div(a, b) | Mod(a, b) | Rem2(a, b) | Pow(a, b)
            | Atan2(a, b) => {
                a.hash_into(h);
                b.hash_into(h);
            }
            Round(m, a, b) => {
                m.hash(h);
                a.hash_into(h);
                b.hash_into(h);
            }
            Min(v) | Max(v) | Hypot(v) => {
                v.len().hash(h);
                v.iter().for_each(|c| c.hash_into(h));
            }
            Clamp(a, b, c) => {
                a.hash_into(h);
                b.hash_into(h);
                c.hash_into(h);
            }
            Fn1(f, a) => {
                f.hash(h);
                a.hash_into(h);
            }
            Log(a, b) => {
                a.hash_into(h);
                if let Some(b) = b {
                    b.hash_into(h);
                }
            }
        }
    }
}

fn clone_calc(c: &Calc) -> Calc {
    c.bake_with(&|leaf| match *leaf {
        Calc::Num(x) => Calc::Num(x),
        Calc::Px(x) => Calc::Px(x),
        Calc::Pct(x) => Calc::Pct(x),
        Calc::Em(x) => Calc::Em(x),
        Calc::Rem(x) => Calc::Rem(x),
        Calc::Vw(x) => Calc::Vw(x),
        Calc::Vh(x) => Calc::Vh(x),
        Calc::Vmin(x) => Calc::Vmin(x),
        Calc::Vmax(x) => Calc::Vmax(x),
        _ => Calc::Num(0.0), // bake_with only passes leaves
    })
}

fn round(m: RoundMode, v: f32, step: f32) -> f32 {
    if step == 0.0 || !step.is_finite() {
        return if step == 0.0 { f32::NAN } else { v };
    }
    let q = v / step;
    let r = match m {
        RoundMode::Nearest => (q + 0.5).floor(),
        RoundMode::Up => q.ceil(),
        RoundMode::Down => q.floor(),
        RoundMode::ToZero => q.trunc(),
    };
    r * step
}

/// A baked expression in taffy's calc slot: px and % only.
#[repr(transparent)]
pub struct Resolved(Calc);

impl Resolved {
    fn new(c: &'static Calc) -> &'static Resolved {
        // SAFETY: Resolved is a transparent wrapper around Calc.
        unsafe { &*(c as *const Calc as *const Resolved) }
    }

    /// The opaque pointer taffy stores (8-byte aligned, as it requires).
    pub fn as_ptr(&'static self) -> *const () {
        self as *const Resolved as *const ()
    }

    /// # Safety
    /// `ptr` must come from [`Resolved::as_ptr`].
    pub unsafe fn from_ptr(ptr: *const ()) -> &'static Resolved {
        unsafe { &*(ptr as *const Resolved) }
    }

    pub fn eval(&self, basis: f32) -> f32 {
        let u = Units { font_size: 16.0, viewport: (0.0, 0.0) };
        let v = self.0.eval_units(&u, basis);
        if v.is_nan() { 0.0 } else { v }
    }
}

/// Every distinct expression, kept for the life of the process (they are
/// few: the stylesheets' and styles' calc() values).
fn intern(c: Calc) -> &'static Calc {
    static MAP: OnceLock<Mutex<KeyMap<u64, Vec<&'static Calc>>>> = OnceLock::new();
    let mut h = key_hasher();
    c.hash_into(&mut h);
    let key = h.finish();
    let mut map = MAP.get_or_init(Default::default).lock().unwrap_or_else(|e| e.into_inner());
    let bucket = map.entry(key).or_default();
    if let Some(found) = bucket.iter().find(|x| ***x == c) {
        return found;
    }
    let leaked: &'static Calc = Box::leak(Box::new(c));
    bucket.push(leaked);
    leaked
}

/// Recursive-descent parser over a function's argument tokens.
struct Parser<'a> {
    toks: &'a [Tok],
    i: usize,
}

impl<'a> Parser<'a> {
    fn function(name: &str, args: &[Vec<Tok>]) -> Option<Calc> {
        let sum = |a: &Vec<Tok>| Parser::sum_all(a);
        let two = |f: fn(Box<Calc>, Box<Calc>) -> Calc| -> Option<Calc> {
            Some(f(Box::new(sum(args.first()?)?), Box::new(sum(args.get(1)?)?)))
        };
        Some(match name {
            "calc" | "-webkit-calc" | "-moz-calc" => sum(args.first()?)?,
            "min" => Calc::Min(args.iter().map(sum).collect::<Option<_>>()?),
            "max" => Calc::Max(args.iter().map(sum).collect::<Option<_>>()?),
            "hypot" => Calc::Hypot(args.iter().map(sum).collect::<Option<_>>()?),
            "clamp" if args.len() == 3 => Calc::Clamp(Box::new(sum(&args[0])?), Box::new(sum(&args[1])?),
                                                      Box::new(sum(&args[2])?)),
            "round" => {
                let (mode, rest) = match args.first()?.as_slice() {
                    [Tok::Ident(m)] => (match m.as_ref() {
                        "nearest" => RoundMode::Nearest,
                        "up" => RoundMode::Up,
                        "down" => RoundMode::Down,
                        "to-zero" => RoundMode::ToZero,
                        _ => return None,
                    }, &args[1..]),
                    _ => (RoundMode::Nearest, args),
                };
                let v = sum(rest.first()?)?;
                let step = match rest.get(1) {
                    Some(s) => sum(s)?,
                    None => Calc::Num(1.0),
                };
                Calc::Round(mode, Box::new(v), Box::new(step))
            }
            "mod" => two(Calc::Mod)?,
            "rem" => two(Calc::Rem2)?,
            "pow" => two(Calc::Pow)?,
            "atan2" => two(Calc::Atan2)?,
            "log" => Calc::Log(Box::new(sum(args.first()?)?),
                               match args.get(1) { Some(b) => Some(Box::new(sum(b)?)), None => None }),
            f => {
                let op = match f {
                    "abs" => Fn1::Abs,
                    "sign" => Fn1::Sign,
                    "sqrt" => Fn1::Sqrt,
                    "exp" => Fn1::Exp,
                    "sin" => Fn1::Sin,
                    "cos" => Fn1::Cos,
                    "tan" => Fn1::Tan,
                    "asin" => Fn1::Asin,
                    "acos" => Fn1::Acos,
                    "atan" => Fn1::Atan,
                    _ => return None,
                };
                Calc::Fn1(op, Box::new(sum(args.first()?)?))
            }
        })
    }

    fn sum_all(toks: &'a [Tok]) -> Option<Calc> {
        let mut p = Parser { toks, i: 0 };
        let v = p.sum()?;
        (p.i == toks.len()).then_some(v)
    }

    fn peek(&self) -> Option<&'a Tok> {
        self.toks.get(self.i)
    }

    fn sum(&mut self) -> Option<Calc> {
        let mut acc = self.product()?;
        loop {
            match self.peek() {
                Some(Tok::Delim('+')) => {
                    self.i += 1;
                    acc = Calc::Add(Box::new(acc), Box::new(self.product()?));
                }
                Some(t) if is_minus(t) => {
                    self.i += 1;
                    acc = Calc::Sub(Box::new(acc), Box::new(self.product()?));
                }
                // `100%-2rem` (no spaces, as Tailwind writes it): the tokenizer
                // reads a signed number; treat it as a sum
                Some(Tok::Num(v) | Tok::Dim(v, _) | Tok::Pct(v)) if *v < 0.0 => {
                    acc = Calc::Add(Box::new(acc), Box::new(self.product()?));
                }
                _ => return Some(acc),
            }
        }
    }

    fn product(&mut self) -> Option<Calc> {
        let mut acc = self.unary()?;
        loop {
            match self.peek() {
                Some(Tok::Delim('*')) => {
                    self.i += 1;
                    acc = Calc::Mul(Box::new(acc), Box::new(self.unary()?));
                }
                Some(Tok::Slash) => {
                    self.i += 1;
                    acc = Calc::Div(Box::new(acc), Box::new(self.unary()?));
                }
                _ => return Some(acc),
            }
        }
    }

    fn unary(&mut self) -> Option<Calc> {
        let t = self.peek()?;
        self.i += 1;
        Some(match t {
            Tok::Delim('(') => {
                let v = self.sum()?;
                match self.peek() {
                    Some(Tok::Delim(')')) => self.i += 1,
                    _ => return None,
                }
                v
            }
            t if is_minus(t) => Calc::Mul(Box::new(Calc::Num(-1.0)), Box::new(self.unary()?)),
            Tok::Delim('+') => self.unary()?,
            Tok::Num(v) => Calc::Num(*v),
            Tok::Pct(v) => Calc::Pct(*v),
            Tok::Dim(v, unit) => leaf(*v, unit)?,
            Tok::Ident(s) => match s.as_ref() {
                "e" => Calc::Num(std::f32::consts::E),
                "pi" => Calc::Num(std::f32::consts::PI),
                "infinity" => Calc::Num(f32::INFINITY),
                "-infinity" => Calc::Num(f32::NEG_INFINITY),
                "nan" => Calc::Num(f32::NAN),
                "-e" => Calc::Num(-std::f32::consts::E),
                "-pi" => Calc::Num(-std::f32::consts::PI),
                _ => return None,
            },
            Tok::Func(fx) => Parser::function(&fx.name, &fx.args)?,
            Tok::Calc(c) => clone_calc(c),
            _ => return None,
        })
    }
}

/// `-` between spaces tokenizes as an identifier (it can start one).
fn is_minus(t: &Tok) -> bool {
    matches!(t, Tok::Delim('-')) || matches!(t, Tok::Ident(s) if s == "-")
}

/// One dimension as an expression leaf (angles become radians, times seconds).
pub fn leaf(v: f32, unit: &str) -> Option<Calc> {
    use std::f32::consts::PI;
    Some(match unit {
        "px" => Calc::Px(v),
        "rem" => Calc::Rem(v),
        "em" => Calc::Em(v),
        "ex" | "rex" => Calc::Em(v * 0.5),
        "ch" | "rch" => Calc::Em(v * 0.55),
        "cap" | "rcap" => Calc::Em(v * 0.7),
        "ic" | "ric" => Calc::Em(v),
        "lh" => Calc::Em(v * 1.3),
        "rlh" => Calc::Px(v * ROOT_FONT_SIZE * 1.3),
        "vw" | "svw" | "lvw" | "dvw" | "vi" | "svi" | "lvi" | "dvi" | "cqw" | "cqi" => Calc::Vw(v),
        "vh" | "svh" | "lvh" | "dvh" | "vb" | "svb" | "lvb" | "dvb" | "cqh" | "cqb" => Calc::Vh(v),
        "vmin" | "svmin" | "lvmin" | "dvmin" | "cqmin" => Calc::Vmin(v),
        "vmax" | "svmax" | "lvmax" | "dvmax" | "cqmax" => Calc::Vmax(v),
        "pt" => Calc::Px(v * 4.0 / 3.0),
        "pc" => Calc::Px(v * 16.0),
        "in" => Calc::Px(v * 96.0),
        "cm" => Calc::Px(v * 96.0 / 2.54),
        "mm" => Calc::Px(v * 96.0 / 25.4),
        "q" => Calc::Px(v * 96.0 / 101.6),
        "deg" => Calc::Num(v * PI / 180.0),
        "grad" => Calc::Num(v * PI / 200.0),
        "rad" => Calc::Num(v),
        "turn" => Calc::Num(v * 2.0 * PI),
        "s" => Calc::Num(v),
        "ms" => Calc::Num(v / 1000.0),
        "x" | "dppx" => Calc::Num(v),
        "dpi" => Calc::Num(v / 96.0),
        "dpcm" => Calc::Num(v * 2.54 / 96.0),
        "fr" => return None,
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::tokenize;

    fn eval(s: &str, basis: f32) -> f32 {
        let t = tokenize(s);
        let Tok::Func(fx) = &t[0] else { panic!("{s}") };
        let c = Calc::parse_fn(&fx.name, &fx.args).unwrap_or_else(|| panic!("parse {s}"));
        c.eval_units(&Units { font_size: 10.0, viewport: (1000.0, 500.0) }, basis)
    }

    #[test]
    fn arithmetic_and_units() {
        assert_eq!(eval("calc(100% - 2rem)", 200.0), 168.0);
        assert_eq!(eval("calc(100%-2rem)", 200.0), 168.0);
        assert_eq!(eval("calc((1px + 2px) * 3)", 0.0), 9.0);
        assert_eq!(eval("calc(10px * 2 / 4)", 0.0), 5.0);
        assert_eq!(eval("calc(2em + 10vw)", 0.0), 120.0);
        assert_eq!(eval("calc(-1 * 5px)", 0.0), -5.0);
        assert_eq!(eval("calc(50vmin)", 0.0), 250.0);
    }

    #[test]
    fn functions() {
        assert_eq!(eval("min(10px, 5px, 20px)", 0.0), 5.0);
        assert_eq!(eval("max(10%, 30px)", 100.0), 30.0);
        assert_eq!(eval("clamp(10px, 50%, 40px)", 200.0), 40.0);
        assert_eq!(eval("round(up, 11px, 5px)", 0.0), 15.0);
        assert_eq!(eval("round(12.4px, 1px)", 0.0), 12.0);
        assert_eq!(eval("mod(-7px, 3px)", 0.0), 2.0);
        assert_eq!(eval("rem(-7px, 3px)", 0.0), -1.0);
        assert_eq!(eval("abs(-4px)", 0.0), 4.0);
        assert_eq!(eval("calc(sign(-3px) * 2px)", 0.0), -2.0);
        assert!((eval("calc(sin(90deg) * 10px)", 0.0) - 10.0).abs() < 1e-4);
        assert_eq!(eval("calc(pow(2, 3) * 1px)", 0.0), 8.0);
        assert_eq!(eval("hypot(3px, 4px)", 0.0), 5.0);
        assert!((eval("calc(pi * 1px)", 0.0) - std::f32::consts::PI).abs() < 1e-6);
    }

    #[test]
    fn interned_and_resolved() {
        let t = tokenize("calc(100% - 1em)");
        let Tok::Func(fx) = &t[0] else { panic!() };
        let a = Calc::parse_fn(&fx.name, &fx.args).unwrap();
        let b = Calc::parse_fn(&fx.name, &fx.args).unwrap();
        assert!(std::ptr::eq(a, b));
        let r = a.resolve(&Units { font_size: 20.0, viewport: (0.0, 0.0) });
        assert_eq!(r.eval(100.0), 80.0);
        assert_eq!(r.as_ptr() as usize % 8, 0);
    }
}
