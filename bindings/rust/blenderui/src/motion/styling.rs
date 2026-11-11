//! Writing moving values onto computed styles (and putting the style's own
//! back): transforms on top of the style's, the common fields directly,
//! any other CSS property through its fields.

use blenderui_sys as sys;

use super::element::PropState;
use super::value::Read;
use crate::anim::{Field, copy_field, field_numbers, fields_of, set_field_numbers};
use crate::geometry::{Length, Units};
use crate::style::Computed;

/// Whether property `p` only changes how things look, not where they are
/// (transforms, opacity, colors, shadows, filters): a redraw can do it.
pub(super) fn paint_only(p: &PropState) -> bool {
    use Field::{Accent, Backdrop, Background, BorderColor, BoxShadow, Color, DecorationColor, Filter, Opacity,
                OutlineColor, Rotate, Scale, TextShadow, Transform, Translate, Visibility};
    match p.id {
        sys::BK_PROP_X | sys::BK_PROP_Y | sys::BK_PROP_Z | sys::BK_PROP_SCALE | sys::BK_PROP_SCALE_X
        | sys::BK_PROP_SCALE_Y | sys::BK_PROP_ROTATE | sys::BK_PROP_ROTATE_X | sys::BK_PROP_ROTATE_Y
        | sys::BK_PROP_SKEW_X | sys::BK_PROP_SKEW_Y => true,
        _ if p.layout != 0 => true,
        _ => p.fields.iter().all(|f| matches!(f, Opacity | Color | Background | BorderColor(_) | OutlineColor
            | DecorationColor | Accent | BoxShadow | TextShadow | Filter | Backdrop | Rotate | Scale | Transform
            | Translate | Visibility)),
    }
}

/// The style fields a non-transform property writes (by its CSS name).
pub(super) fn fields_for(id: i32) -> &'static [Field] {
    match id {
        sys::BK_PROP_OPACITY => &[Field::Opacity],
        sys::BK_PROP_WIDTH => &[Field::Width],
        sys::BK_PROP_HEIGHT => &[Field::Height],
        sys::BK_PROP_COLOR => &[Field::Color],
        sys::BK_PROP_BACKGROUND_COLOR => &[Field::Background],
        id if id >= sys::BK_PROP__FIXED => {
            // SAFETY: the engine's name for an id it gave (or NULL).
            let name = unsafe { sys::BK_anim_prop_name(id) };
            if name.is_null() {
                return &[];
            }
            let camel = unsafe { std::ffi::CStr::from_ptr(name) }.to_string_lossy();
            let mut kebab = String::with_capacity(camel.len() + 4);
            for c in camel.chars() {
                if c.is_ascii_uppercase() {
                    kebab.push('-');
                    kebab.push(c.to_ascii_lowercase());
                } else {
                    kebab.push(c);
                }
            }
            fields_of(crate::css::Prop::lookup(&kebab))
        }
        _ => &[],
    }
}

/// What property `p` is in style `c` (transforms: none, Motion's x / scale /
/// rotate are on top of the style's own).
pub(super) fn base_of(p: &PropState, c: &Computed, u: &Units) -> Read {
    let one = |v: f32| Read { v: [v, 0.0, 0.0, 0.0], n: 1, ..Read::default() };
    match p.id {
        sys::BK_PROP_SCALE | sys::BK_PROP_SCALE_X | sys::BK_PROP_SCALE_Y => one(1.0),
        sys::BK_PROP_X | sys::BK_PROP_Y | sys::BK_PROP_Z | sys::BK_PROP_ROTATE | sys::BK_PROP_ROTATE_X
        | sys::BK_PROP_ROTATE_Y | sys::BK_PROP_SKEW_X | sys::BK_PROP_SKEW_Y => one(0.0),
        _ if p.layout != 0 => one(if p.layout >= 3 { 1.0 } else { 0.0 }),
        _ => p.fields.first().and_then(|&f| field_numbers(f, c, u)).map_or(one(0.0), |(v, n, pct)| Read {
            v,
            n: n as u8,
            unit: if n == 4 { sys::BK_UNIT_COLOR } else if pct { sys::BK_UNIT_PERCENT } else { sys::BK_UNIT_PX },
            ..Read::default()
        }),
    }
}

fn length(v: f32, unit: i32) -> Length {
    match unit {
        sys::BK_UNIT_PERCENT => Length::Percent(v),
        sys::BK_UNIT_EM => Length::Em(v),
        sys::BK_UNIT_REM => Length::Rem(v),
        sys::BK_UNIT_VW => Length::Vw(v),
        sys::BK_UNIT_VH => Length::Vh(v),
        _ => Length::Px(v),
    }
}

/// `a` moved by `v` in `unit` (pixels add; another unit replaces a zero `a`).
fn offset(a: Length, v: f32, unit: i32, u: &Units) -> Length {
    match (a, length(v, unit)) {
        (a, Length::Px(d)) => a.to_px(u).map_or(Length::Px(d), |x| Length::Px(x + d)),
        (a, l) if a.to_px(u) == Some(0.0) => l,
        (a, _) => a,
    }
}

fn radians(v: f32, unit: i32) -> f32 {
    match unit {
        sys::BK_UNIT_RAD => v,
        sys::BK_UNIT_TURN => v * std::f32::consts::TAU,
        _ => v.to_radians(),
    }
}

/// Put back in `st` what property `p` writes, from `base`; true for the
/// transform functions (restored together by [`restore_transforms`]).
pub(super) fn restore(p: &PropState, base: &Computed, st: &mut Computed) -> bool {
    match p.id {
        sys::BK_PROP_X => st.translate.0 = base.translate.0,
        sys::BK_PROP_Y => st.translate.1 = base.translate.1,
        sys::BK_PROP_OPACITY => st.opacity = base.opacity,
        sys::BK_PROP_COLOR => st.color = base.color,
        sys::BK_PROP_BACKGROUND_COLOR => st.background = base.background,
        sys::BK_PROP_SCALE | sys::BK_PROP_SCALE_X | sys::BK_PROP_SCALE_Y | sys::BK_PROP_ROTATE | sys::BK_PROP_SKEW_X
        | sys::BK_PROP_SKEW_Y => return true,
        _ => match p.layout {
            1 => st.translate.0 = base.translate.0,
            2 => st.translate.1 = base.translate.1,
            3 | 4 => return true,
            _ => {
                for &f in p.fields {
                    copy_field(f, base, st);
                }
            }
        },
    }
    false
}

/// The transform parts Motion writes (scale, rotate, skews), as in `base`.
pub(super) fn restore_transforms(base: &Computed, st: &mut Computed) {
    match (&base.effects, &mut st.effects) {
        (Some(b), Some(s)) => {
            s.scale2 = b.scale2;
            s.rotate = b.rotate;
            s.transform.clone_from(&b.transform);
        }
        (None, Some(s)) => {
            s.scale2 = None;
            s.rotate = 0.0;
            s.transform.clear();
        }
        _ => {}
    }
}

fn rgba(v: &[f32; 4]) -> crate::color::Rgba {
    let c = |x: f32| x.clamp(0.0, 1.0);
    crate::color::Rgba { r: c(v[0]), g: c(v[1]), b: c(v[2]), a: c(v[3]) }
}

/// Lay value `r` of property `p` over style `c`.
pub(super) fn write(p: &PropState, r: &Read, c: &mut Computed, u: &Units) {
    let v = r.v[0];
    match p.id {
        sys::BK_PROP_X => c.translate.0 = offset(c.translate.0, v, r.unit, u),
        sys::BK_PROP_Y => c.translate.1 = offset(c.translate.1, v, r.unit, u),
        sys::BK_PROP_SCALE | sys::BK_PROP_SCALE_X | sys::BK_PROP_SCALE_Y => {
            let e = c.effects_mut();
            let (sx, sy) = e.scale2.unwrap_or((1.0, 1.0));
            e.scale2 = Some(match p.id {
                sys::BK_PROP_SCALE_X => (sx * v, sy),
                sys::BK_PROP_SCALE_Y => (sx, sy * v),
                _ => (sx * v, sy * v),
            });
        }
        sys::BK_PROP_ROTATE => c.effects_mut().rotate += radians(v, r.unit),
        sys::BK_PROP_SKEW_X => c.effects_mut().transform.push(crate::effects::TransformFn::Skew(radians(v, r.unit), 0.0)),
        sys::BK_PROP_SKEW_Y => c.effects_mut().transform.push(crate::effects::TransformFn::Skew(0.0, radians(v, r.unit))),
        // the common ones straight into their field (the generic way builds a value first)
        sys::BK_PROP_OPACITY => c.opacity = v.clamp(0.0, 1.0),
        sys::BK_PROP_COLOR if r.n == 4 => c.color = rgba(&r.v),
        sys::BK_PROP_BACKGROUND_COLOR if r.n == 4 => c.background = rgba(&r.v),
        // `layout` offsets: in pixels, and scales, on top of the rest
        _ if p.layout == 1 => c.translate.0 = offset(c.translate.0, v, sys::BK_UNIT_PX, u),
        _ if p.layout == 2 => c.translate.1 = offset(c.translate.1, v, sys::BK_UNIT_PX, u),
        _ if p.layout >= 3 => {
            let e = c.effects_mut();
            let (sx, sy) = e.scale2.unwrap_or((1.0, 1.0));
            e.scale2 = Some(if p.layout == 3 { (sx * v, sy) } else { (sx, sy * v) });
        }
        _ => {
            let pct = r.unit == sys::BK_UNIT_PERCENT;
            for &f in p.fields {
                set_field_numbers(f, c, &r.v[..r.n as usize], pct);
            }
        }
    }
}
