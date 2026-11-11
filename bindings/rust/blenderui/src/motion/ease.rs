//! Eases: every GSAP, Motion, CSS and Blender ease by name, parsed once into a
//! [`Ease`] (a few bytes, `Copy`); evaluating one is a function call.

use std::ffi::CStr;

use blenderui_sys as sys;

/// A parsed ease. Make one with [`ease`] / [`Ease::parse`], [`Ease::bezier`],
/// [`Ease::points`] or [`Ease::path`]; the `vars!` / rsx `{{ }}` objects parse
/// theirs once per call site.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(transparent)]
pub struct Ease(pub(crate) sys::BKEase);

impl Ease {
    /// Linear (GSAP's `"none"`).
    pub const LINEAR: Ease = Ease(sys::BKEase { kind: sys::BK_EASE_LINEAR, mode: 0, table: 0, p: [0.0; 4] });

    /// An ease by name: `"power2.out"`, `"elastic.out(1, 0.3)"`, `"easeInOut"`,
    /// `"cubic-bezier(.17,.67,.83,.67)"`, `"steps(4, jump-start)"`,
    /// `"linear(0, 0.25 75%, 1)"`, `"bezier"` (Blender's) ... `None` when unknown.
    #[must_use]
    pub fn parse(name: &CStr) -> Option<Ease> {
        let mut e = sys::BKEase::default();
        // SAFETY: a NUL-terminated name and a valid out pointer.
        (unsafe { sys::BK_ease_parse(name.as_ptr(), &raw mut e) } != 0).then_some(Ease(e))
    }

    /// Motion's `[x1, y1, x2, y2]` and CSS's `cubic-bezier()`.
    #[must_use]
    pub fn bezier(x1: f32, y1: f32, x2: f32, y2: f32) -> Ease {
        // SAFETY: plain values.
        Ease(unsafe { sys::BK_ease_bezier(x1, y1, x2, y2) })
    }

    /// A curve through `(x, y)` points, x ascending from 0 to 1.
    #[must_use]
    pub fn points(xy: &[[f32; 2]]) -> Option<Ease> {
        let mut e = sys::BKEase::default();
        let n = i32::try_from(xy.len()).ok()?;
        // SAFETY: `n` points read from a live slice.
        (unsafe { sys::BK_ease_points(xy.as_ptr(), n, &raw mut e) } != 0).then_some(Ease(e))
    }

    /// GSAP's `CustomEase`: an SVG path normalized to 0..1.
    #[must_use]
    pub fn path(svg: &CStr) -> Option<Ease> {
        let mut e = sys::BKEase::default();
        // SAFETY: a NUL-terminated path and a valid out pointer.
        (unsafe { sys::BK_ease_path(svg.as_ptr(), &raw mut e) } != 0).then_some(Ease(e))
    }

    /// Give this ease a name [`Ease::parse`] finds (`CustomEase.create`,
    /// `gsap.registerEase`).
    pub fn register(self, name: &CStr) -> bool {
        // SAFETY: a NUL-terminated name; the ease is copied.
        unsafe { sys::BK_ease_register(name.as_ptr(), &raw const self.0) != 0 }
    }

    /// The eased value of `t` (0..1; outside, the curve goes on).
    #[inline]
    #[must_use]
    pub fn eval(&self, t: f32) -> f32 {
        // SAFETY: a valid ease.
        unsafe { sys::BK_ease_eval(&raw const self.0, t) }
    }
}

/// [`Ease::parse`], linear for an unknown name (GSAP falls back the same way).
#[must_use]
pub fn ease(name: &CStr) -> Ease {
    Ease::parse(name).unwrap_or(Ease::LINEAR)
}
