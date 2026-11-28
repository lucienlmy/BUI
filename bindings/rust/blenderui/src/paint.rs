//! Painting: a frame of laid-out boxes becomes one display list on the kit
//! canvas. [`Painter`] is also what `canvas(|bounds, painter| ...)` draws with.

use std::collections::HashMap;
use std::hash::Hasher;
use std::sync::Arc;

use taffy::prelude::*;

use crate::color::Rgba;
use crate::effects::{Side, Tile};
use crate::geometry::{rect, Bounds, Length, Units};
use crate::shader::{RenderTarget, ShaderEffect};
use crate::layout::{BoxNode, Content, Frame, Key, Replaced, StyleStore};
use crate::style::{key_hasher, BorderStyle, BoxEdge, Computed, DecoStyle, DisplayKind, TextAlign, WhiteSpace};
use crate::text::{self, TextCache};
use blenderui_sys as sys;

/// A decoded image, owned by the kit's image module (a `BKImBuf`: one
/// allocation of premultiplied pixels). Every format and every binding goes
/// through the same C decoders: PNG, JPEG, WebP, GIF, BMP, TGA, QOI, Radiance
/// HDR, PNM, SVG and TIFF, plus OpenEXR, AVIF and JPEG XL when built with their
/// features and HEIC through the OS's decoder. Cheap to share (`Arc<Image>`) and to pass every frame:
/// the canvas borrows it, nothing is copied or converted per frame.
pub struct Image {
    pub width: u32,
    pub height: u32,
    ib: std::ptr::NonNull<sys::BKImBuf>,
}

// SAFETY: a BKImBuf is never written after it is made, and only Drop frees it.
unsafe impl Send for Image {}
// SAFETY: as above: shared references only read it.
unsafe impl Sync for Image {}

impl Image {
    fn wrap(ib: *mut sys::BKImBuf) -> Option<Self> {
        let ib = std::ptr::NonNull::new(ib)?;
        let (mut w, mut h) = (0, 0);
        // SAFETY: `ib` is a live image the kit just made.
        unsafe { sys::BK_imbuf_size(ib.as_ptr(), &raw mut w, &raw mut h) };
        Some(Self { width: w as u32, height: h as u32, ib })
    }

    /// Straight-alpha RGBA pixels, `width * height * 4` bytes (copied once).
    #[must_use]
    pub fn from_rgba(width: u32, height: u32, rgba: &[u8]) -> Option<Self> {
        if width == 0 || height == 0 || rgba.len() < width as usize * height as usize * 4 {
            return None;
        }
        // SAFETY: `rgba` holds the `width * height * 4` bytes the kit reads.
        Self::wrap(unsafe { sys::BK_imbuf_from_rgba(rgba.as_ptr(), width as i32, height as i32, 0) })
    }

    /// An image of premultiplied ARGB words (a layer's), copied as they are.
    pub(crate) fn from_argb(a: &crate::effects::Argb) -> Option<Self> {
        if a.w == 0 || a.h == 0 || a.px.len() < a.w * a.h {
            return None;
        }
        // SAFETY: `a.px` holds the `w * h` words the kit reads.
        Self::wrap(unsafe { sys::BK_imbuf_from_argb(a.px.as_ptr(), a.w as i32, a.h as i32) })
    }

    /// Decode an encoded image, fitted within `max_w` x `max_h` (0: no limit;
    /// never upscaled). JPEGs decode smaller for free (DCT scaling).
    #[must_use]
    pub fn decode(bytes: &[u8], max_w: u32, max_h: u32) -> Option<Self> {
        // SAFETY: the kit reads `bytes.len()` bytes and keeps nothing.
        Self::wrap(unsafe { sys::BK_imbuf_decode(bytes.as_ptr().cast(), bytes.len(), max_w as i32, max_h as i32) })
    }

    /// Decode an image file (any format the kit reads), at full size.
    #[must_use]
    pub fn open(path: &str) -> Option<Self> {
        let path = std::ffi::CString::new(path).ok()?;
        // SAFETY: `path` is NUL-terminated and outlives the call.
        Self::wrap(unsafe { sys::BK_imbuf_load(path.as_ptr(), 0, 0) })
    }

    /// Decode a PNG file. Kept for compatibility: every format works now.
    #[deprecated(since = "0.4.0", note = "every format decodes: use Image::open")]
    #[must_use]
    pub fn from_png(path: &str) -> Option<Self> {
        Self::open(path)
    }

    /// The pixels as straight-alpha RGBA (a copy).
    #[must_use]
    pub fn to_rgba(&self) -> Vec<u8> {
        let mut out = vec![0u8; self.width as usize * self.height as usize * 4];
        // SAFETY: `out` holds width * height * 4 bytes, what the kit writes.
        unsafe { sys::BK_imbuf_rgba(self.ib.as_ptr(), 0, out.as_mut_ptr()) };
        out
    }

    /// A BUI texture of the pixels (a Dear ImGui `ImTextureID`); free it
    /// with `blenderui::sys::BUI_texture_free`. 0 on failure.
    #[must_use]
    pub fn texture(&self) -> i32 {
        // SAFETY: a live image.
        unsafe { sys::BK_imbuf_texture(self.ib.as_ptr(), 0) }
    }

    /// Puts these pixels into BUI texture `tex` of the same size (from
    /// [`Image::texture`]): the next frame of an [`Animation`] in a Dear
    /// ImGui texture. `false` when the sizes differ.
    #[must_use]
    pub fn update_texture(&self, tex: i32) -> bool {
        // SAFETY: a live image; the kit checks `tex`.
        unsafe { sys::BK_imbuf_texture_update(self.ib.as_ptr(), 0, tex) != 0 }
    }

    pub(crate) const fn as_ptr(&self) -> *const sys::BKImBuf {
        self.ib.as_ptr()
    }
}

impl Drop for Image {
    fn drop(&mut self) {
        // SAFETY: the image is ours; nothing uses it after this (the canvas
        // that borrowed it gets its next display list before it draws again).
        unsafe { sys::BK_imbuf_free(self.ib.as_ptr()) };
    }
}

impl std::fmt::Debug for Image {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Image").field("width", &self.width).field("height", &self.height).finish_non_exhaustive()
    }
}

/// The image cache shared by every window (and every binding in the
/// process): decoded bytes held, the budget, decodes pending, totals.
#[must_use]
pub fn image_stats() -> sys::BKImageStats {
    let mut st = sys::BKImageStats::default();
    // SAFETY: the kit writes the struct.
    unsafe { sys::BK_image_stats(&raw mut st) };
    st
}

/// The image cache's memory budget in bytes (default 256 MiB). Images on
/// screen stay even past it; the least recently drawn go first.
pub fn set_image_budget(bytes: usize) {
    // SAFETY: a plain setting.
    unsafe { sys::BK_image_set_budget(bytes) };
}

/// An animation decoded one frame at a time at the size asked (GIF,
/// animated WebP): for drawing it yourself, e.g. into a Dear ImGui texture
/// with [`Image::update_texture`]. `<img>` and `Painter::image_file` play
/// animations by themselves.
pub struct Animation {
    anim: std::ptr::NonNull<sys::BKAnim>,
    pub width: u32,
    pub height: u32,
    /// Frames in the file and how many times it plays (0: forever).
    pub frames: u32,
    pub loops: u32,
}

// SAFETY: the decoder is owned here and only used through `&mut self`.
unsafe impl Send for Animation {}

impl Animation {
    /// Decode `bytes` frame by frame, fitted within `max_w` x `max_h` (0: no
    /// limit). `None` when it is not an animation the kit reads.
    #[must_use]
    pub fn new(bytes: &[u8], max_w: u32, max_h: u32) -> Option<Self> {
        // SAFETY: the kit copies `bytes`.
        let anim = std::ptr::NonNull::new(unsafe {
            sys::BK_anim_open(bytes.as_ptr().cast(), bytes.len(), max_w as i32, max_h as i32)
        })?;
        let (mut w, mut h, mut loops) = (0, 0, 0);
        // SAFETY: a live animation; the out-parameters are written.
        let frames = unsafe { sys::BK_anim_info(anim.as_ptr(), &raw mut w, &raw mut h, &raw mut loops) };
        Some(Self { anim, width: w as u32, height: h as u32, frames: frames as u32, loops: loops as u32 })
    }

    /// The next frame (looping after the last) and how long to show it.
    pub fn next_frame(&mut self) -> Option<(Image, std::time::Duration)> {
        let mut delay = 0;
        // SAFETY: a live animation; the frame is ours.
        let img = Image::wrap(unsafe { sys::BK_anim_next(self.anim.as_ptr(), &raw mut delay) })?;
        Some((img, std::time::Duration::from_millis(delay.max(0) as u64)))
    }
}

impl Drop for Animation {
    fn drop(&mut self) {
        // SAFETY: ours, freed once.
        unsafe { sys::BK_anim_free(self.anim.as_ptr()) };
    }
}

/// An image file shown by path: its intrinsic size comes from the file's
/// header (read once); the pixels decode on the kit's worker threads at the
/// size they are drawn, only while visible.
#[derive(Clone, Debug)]
pub(crate) struct ImageFile {
    pub src: Arc<str>,
    /// The natural size, or what stands in for it (`flags`).
    pub width: u32,
    pub height: u32,
    /// `sys::BK_IMAGE_INFO_*`: which of the size is natural.
    flags: i32,
}

impl ImageFile {
    fn probe(src: &str) -> Option<Self> {
        let path = std::ffi::CString::new(src).ok()?;
        let mut info = sys::BKImageInfo::default();
        // SAFETY: `path` is NUL-terminated; `info` is written.
        let ok = unsafe { sys::BK_image_info(path.as_ptr(), &raw mut info) } != 0;
        (ok && info.width > 0 && info.height > 0).then(|| Self {
            src: src.into(), width: info.width as u32, height: info.height as u32, flags: info.flags,
        })
    }

    /// Its natural size: an SVG's may lack a width, a height, or a ratio.
    pub(crate) fn natural(&self) -> crate::style::Natural {
        let n = crate::style::Natural::of(self.width as f32, self.height as f32);
        let lacks = |f: i32| self.flags & f != 0;
        crate::style::Natural {
            width: n.width.filter(|_| !lacks(sys::BK_IMAGE_INFO_NO_WIDTH)),
            height: n.height.filter(|_| !lacks(sys::BK_IMAGE_INFO_NO_HEIGHT)),
            ratio: n.ratio.filter(|_| !lacks(sys::BK_IMAGE_INFO_NO_RATIO)),
        }
    }
}

/// `<img src>` files, kept by the window (each header is read once).
#[derive(Default)]
pub(crate) struct ImageCache {
    files: HashMap<String, Option<ImageFile>>,
    /// Inline `<svg>` pictures, decoded once per markup (by its hash).
    svgs: HashMap<u64, Option<Arc<Image>>>,
    /// `<video src>` players
    pub(crate) videos: crate::video::VideoCache,
}

impl ImageCache {
    /// `src` of an `<img>`: any format the kit reads.
    pub(crate) fn load(&mut self, src: &str) -> Option<ImageFile> {
        if let Some(i) = self.files.get(src) {
            return i.clone();
        }
        let file = ImageFile::probe(src);
        self.files.insert(src.to_owned(), file.clone());
        file
    }

    /// An inline `<svg>`'s picture, from its markup (the kit's SVG reader rasterizes it).
    pub(crate) fn svg(&mut self, markup: &str) -> Option<Arc<Image>> {
        use std::hash::{Hash, Hasher};
        let mut h = std::collections::hash_map::DefaultHasher::new();
        markup.hash(&mut h);
        let key = h.finish();
        if let Some(i) = self.svgs.get(&key) {
            return i.clone();
        }
        if self.svgs.len() > 256 {
            self.svgs.clear();
        }
        let img = Image::decode(markup.as_bytes(), 0, 0).map(Arc::new);
        self.svgs.insert(key, img.clone());
        img
    }
}

#[derive(Clone)]
enum Kind {
    Rect { radius: f32, outline: Rgba, width: f32 },
    Oval { outline: Rgba, width: f32 },
    Line { width: f32 },
    Polygon,
    Arc { start: f32, extent: f32 },
    /// `start..end` of the painter's text arena (NUL-terminated there).
    Text { start: u32, size: f32, flags: i32 },
    Image(Arc<Image>),
    /// A file by path (`start..` of the text arena, NUL-terminated), fitted
    /// into the box by `fit` (`sys::BK_FIT_*`); animations play (`frame` 0)
    /// or hold frame `frame - 1`.
    File { start: u32, fit: i32, frame: i32 },
    /// A video's current frame, fitted by `fit`.
    Video(std::rc::Rc<crate::video::Video>, i32),
    /// A GPU shader over the box (see [`crate::shader`]).
    Effect(Box<EffectItem>),
    /// A GPU pass into a render target (see [`Painter::effect_to`]): not drawn into the canvas.
    Pass(Box<PassItem>),
    /// CSS `box-shadow` (an outer one): the coords are the shadow's box, then the box casting it (the
    /// shadow shows only outside that); `radius` is the shadow's corner, `cast_radius` the caster's,
    /// `sigma` the blur's (a Gaussian; 0 a hard edge).
    Shadow { radius: f32, cast_radius: f32, sigma: f32 },
}

/// What [`Painter::effect_to`] records: the render target, the effect (0: a fill), its textures and floats.
#[derive(Clone)]
struct PassItem {
    target: i32,
    effect: i32,
    inputs: [i32; sys::BK_EFFECT_MAX_INPUTS],
    floats: Vec<f32>,
}

/// What [`Painter::effect`] records: the effect, its textures and floats, and the image drawn where
/// the GPU cannot run it.
#[derive(Clone)]
struct EffectItem {
    id: i32,
    inputs: [i32; sys::BK_EFFECT_MAX_INPUTS],
    floats: Vec<f32>,
    fallback: Option<Arc<Image>>,
    /// A texture of the painter's own among the inputs (a turned layer's): held for as long as
    /// a display list draws it, not only while the layer cache keeps it (another window's
    /// paints can drop it from the cache while this one's list is still shown). Never read.
    #[allow(dead_code)]
    texture: Option<Arc<Texture>>,
}

/// Coordinates: up to four inline (rects, ovals, text, images), more on the heap.
#[derive(Clone)]
enum Coords {
    Inline([f32; 4], u8),
    Heap(Vec<f32>),
}

impl Coords {
    /// Points flattened to x, y pairs; up to two stay inline (no allocation).
    fn of_points(points: &[(f32, f32)]) -> Coords {
        if points.len() <= 2 {
            let mut a = [0.0; 4];
            for (k, &(x, y)) in points.iter().enumerate() {
                a[k * 2] = x;
                a[k * 2 + 1] = y;
            }
            Coords::Inline(a, (points.len() * 2) as u8)
        } else {
            Coords::Heap(points.iter().flat_map(|&(x, y)| [x, y]).collect())
        }
    }

    fn as_slice(&self) -> &[f32] {
        match self {
            Coords::Inline(a, n) => &a[..*n as usize],
            Coords::Heap(v) => v,
        }
    }
}

#[derive(Clone)]
struct Item {
    kind: Kind,
    coords: Coords,
    fill: Rgba,
    clip: Option<Bounds<f32>>,
    /// Its element has `pointer-events: none`: the pointer goes through it.
    through: bool,
    /// A CSS box's rectangle (background, border, image): its edges snap to device pixels, as
    /// browsers paint boxes laid out at fractional positions.
    snap: bool,
}

/// Draws into a canvas display list, in logical pixels.
///
/// ```ignore
/// canvas(|bounds, p| {
///     p.fill(bounds, rgb(0x4772b3));
///     p.text(bounds.origin.x + 4.0, bounds.origin.y + 4.0, "hi", 13.0, white());
/// })
/// ```
#[derive(Default)]
pub struct Painter {
    items: Vec<Item>,
    /// Every text item's bytes, NUL-terminated, back to back.
    text: Vec<u8>,
    clip: Option<Bounds<f32>>,
    /// The box being painted has `pointer-events: none`.
    through: bool,
    /// What is being painted is a CSS box (not a canvas's own drawing): see `Item::snap`.
    pub(crate) snap: bool,
    opacity: f32,
    /// Retained painting (frames where only moving values changed): each
    /// box's items (start, end; u32::MAX when it painted none), each item's
    /// index in the canvas (-1: none), the text size after the full paint,
    /// and the items repainted since (only they go to the canvas).
    ranges: Vec<(u32, u32)>,
    slots: Vec<i32>,
    text_full: usize,
    changed: Vec<u32>,
    retained: bool,
    partial: bool,
    /// Layers over widgets: widgets are kit nodes drawn after the canvas
    /// they sit on, so an item that overlaps a widget painted before it
    /// starts a new segment (its own canvas, drawn over that widget).
    /// `segments`: where each segment after the first starts in `items`;
    /// `widgets`: each painted widget's segment and area; `over`: the areas
    /// of the widgets in the segment being painted.
    segments: Vec<u32>,
    widgets: Vec<(Key, u32, Bounds<f32>)>,
    over: Vec<Bounds<f32>>,
    /// The window draws on the GPU (set before each paint).
    gpu: bool,
    /// The text selection to highlight (set before each paint; None: nothing is selected).
    pub(crate) sel: Option<crate::selection::SelPaint>,
    /// Device pixels per logical one of what this paints (0 before a frame sets it: 1), so that
    /// clips cut along device rows ([`rounded_strips`]).
    scale: f32,
}

impl Painter {
    pub(crate) fn new() -> Painter {
        Painter { opacity: 1.0, ..Painter::default() }
    }

    /// Empty for the next frame, keeping the buffers.
    pub(crate) fn reset(&mut self) {
        self.items.clear();
        self.text.clear();
        self.clip = None;
        self.opacity = 1.0;
        self.ranges.clear();
        self.retained = false;
        self.partial = false;
        self.segments.clear();
        self.widgets.clear();
        self.over.clear();
    }

    /// How many canvases the items need (1 unless something overlaps a widget).
    pub(crate) fn segment_count(&self) -> usize {
        self.segments.len() + 1
    }

    /// Each painted widget's segment and its place in paint order.
    pub(crate) fn widget_order(&self) -> HashMap<Key, (u32, u32)> {
        self.widgets.iter().enumerate().map(|(rank, &(k, seg, _))| (k, (seg, rank as u32))).collect()
    }

    /// A widget is painted here (in paint order): it draws on top of its
    /// segment's items, and what overlaps it from now on goes in a later one.
    pub(crate) fn note_widget(&mut self, key: Key, area: Bounds<f32>) {
        self.widgets.push((key, self.segments.len() as u32, area));
        self.over.push(area);
    }

    /// Where an item is, generously (text: a full em per byte): `None` when it has no place.
    fn item_area(&self, kind: &Kind, coords: &Coords) -> Option<[f32; 4]> {
        let c = coords.as_slice();
        if c.len() < 2 {
            return None;
        }
        if let Kind::Text { start, size, .. } = kind {
            let len = self.text[*start as usize..].iter().position(|&b| b == 0).unwrap_or(0);
            return Some([c[0], c[1], c[0] + len as f32 * size, c[1] + size * 1.5]);
        }
        if let (Kind::Shadow { sigma, .. }, [x0, y0, x1, y1, ..]) = (kind, c) {
            // the shadow's box (the caster's is under it) as far as the blur reaches
            let g = 3.0 * sigma;
            return Some([x0.min(*x1) - g, y0.min(*y1) - g, x0.max(*x1) + g, y0.max(*y1) + g]);
        }
        let (mut x0, mut y0, mut x1, mut y1) = (c[0], c[1], c[0], c[1]);
        for xy in c.as_chunks::<2>().0 {
            x0 = x0.min(xy[0]);
            x1 = x1.max(xy[0]);
            y0 = y0.min(xy[1]);
            y1 = y1.max(xy[1]);
        }
        Some([x0 - 2.0, y0 - 2.0, x1 + 2.0, y1 + 2.0]) // edges, antialiasing
    }

    /// Whether an item overlaps a widget painted before it in this segment.
    fn overlaps_widget(&self, kind: &Kind, coords: &Coords) -> bool {
        !self.over.is_empty()
            && self.item_area(kind, coords).is_some_and(|[x0, y0, x1, y1]| {
                self.over.iter().any(|w| {
                    x0 < w.origin.x + w.size.width && w.origin.x < x1 && y0 < w.origin.y + w.size.height
                        && w.origin.y < y1
                })
            })
    }

    /// Paint again only the boxes marked in `dirty` (with what they paint
    /// over: nothing, their items stay in place). False when that can't be
    /// done (the last paint had layers, a box paints a different number of
    /// items now, the text grew too much): paint the frame in full instead.
    pub(crate) fn repaint(&mut self, frame: &Frame, styles: &StyleStore, tc: &mut TextCache, dirty: &[bool],
                          vp: (f32, f32), app: &crate::window::App) -> bool {
        if !self.retained || self.ranges.len() != frame.nodes.len() || self.text.len() > 2 * self.text_full + 4096 {
            return false;
        }
        // a scale that moved on is a new matrix for the box and all under it
        with_scratch(|s| self.repaint_with(&mut s.mats, frame, styles, tc, dirty, vp, app))
    }

    #[allow(clippy::too_many_arguments)]
    fn repaint_with(&mut self, mats: &mut Mats, frame: &Frame, styles: &StyleStore, tc: &mut TextCache,
                    dirty: &[bool], vp: (f32, f32), app: &crate::window::App) -> bool {
        vector_mats(frame, styles, vp, mats);
        if mats.layered {
            return false;
        }
        self.changed.clear();
        for (i, _) in dirty.iter().enumerate().filter(|(_, d)| **d) {
            let (start, end) = self.ranges[i];
            let n = &frame.nodes[i];
            let at = self.items.len();
            if n.visible && n.opacity > 0.0 && self.in_view(mats, frame, styles, i, vp) {
                self.opacity = n.opacity;
                self.clip = n.clip;
                paint_node(frame, styles, tc, i, self, vp, app);
                mats.map(self, at, i);
            }
            let painted = (self.items.len() - at) as u32;
            let had = if start == u32::MAX { 0 } else { end - start };
            if painted != had {
                return false;
            }
            for k in 0..painted {
                self.items.swap((start + k) as usize, at + k as usize);
                self.changed.push(start + k);
            }
            self.items.truncate(at);
        }
        self.opacity = 1.0;
        self.clip = None;
        self.partial = true;
        end_layers_frame();
        true
    }

    /// Map the items from `from` on through `m`, a move and a scale (see
    /// [`Effects::vector`](crate::effects::Effects::vector)): their points,
    /// text sizes, corner radii and line widths, and their clips ([`clip_through`]).
    fn map_items(&mut self, from: usize, m: &crate::effects::Affine, outer: OuterClip) {
        let (sx, sy) = (m[0], m[3]);
        let mean = (sx + sy) * 0.5;
        for it in &mut self.items[from..] {
            match &mut it.coords {
                Coords::Inline(a, n) => map_points(&mut a[..*n as usize], m),
                Coords::Heap(v) => map_points(v, m),
            }
            match &mut it.kind {
                Kind::Rect { radius, width, .. } => {
                    *radius *= sx.min(sy);
                    *width *= mean;
                }
                Kind::Oval { width, .. } | Kind::Line { width } => *width *= mean,
                Kind::Shadow { radius, cast_radius, sigma } => {
                    *radius *= sx.min(sy);
                    *cast_radius *= sx.min(sy);
                    *sigma *= mean;
                }
                // anchored at its top left: the size follows the height
                Kind::Text { size, .. } => *size *= sy,
                _ => {}
            }
            it.clip = clip_through(it.clip, m, outer);
            it.snap = false; // a moved, scaled or turned box: its corners are not a pixel grid's
        }
    }

    /// Leave out of the items the clip `outer` (a layer's own, applied when
    /// its image is drawn).
    fn unclip(&mut self, outer: Option<Bounds<f32>>) {
        for it in self.items.iter_mut().filter(|it| outer.is_some() && it.clip == outer) {
            it.clip = None;
        }
    }

    /// The area the items cover: their points (with half a stroke and a pixel
    /// of antialiasing around a stroked one), and text by its measured width
    /// and line (`None`: no items). What a layer must rasterize: not the boxes
    /// it sits in, which may be wider than anything drawn.
    fn extent(&self, tc: &mut TextCache) -> Option<Bounds<f32>> {
        let (mut x0, mut y0, mut x1, mut y1) = (f32::MAX, f32::MAX, f32::MIN, f32::MIN);
        for it in &self.items {
            let xy = it.coords.as_slice();
            if let (Kind::Text { start, size, flags }, [x, y, ..]) = (&it.kind, xy) {
                let t = &self.text[*start as usize..];
                let t = std::str::from_utf8(&t[..t.iter().position(|&b| b == 0).unwrap_or(t.len())]).unwrap_or("");
                // a quarter of the size around: italic overhang, synthetic bold, descenders
                let pad = size * TEXT_INK_PAD;
                let w = tc.width(t, *size, *flags);
                (x0, y0, x1, y1) = (x0.min(x - pad), y0.min(y - pad), x1.max(x + w + pad), y1.max(y + size * TEXT_LINE + pad));
                continue;
            }
            let stroke = match it.kind {
                Kind::Line { width } | Kind::Oval { width, .. } | Kind::Rect { width, .. } if width > 0.0 => {
                    width * 0.5 + 1.0
                }
                // a Gaussian's reach
                Kind::Shadow { sigma, .. } => 3.0 * sigma + 1.0,
                _ => 0.0,
            };
            for p in xy.as_chunks::<2>().0 {
                (x0, y0, x1, y1) = (x0.min(p[0] - stroke), y0.min(p[1] - stroke), x1.max(p[0] + stroke), y1.max(p[1] + stroke));
            }
        }
        (x0 <= x1).then(|| rect(x0, y0, x1 - x0, y1 - y0))
    }

    /// Whether box `i` is painted: a selection paints everything, a box under
    /// a vector transform paints where the transform puts it (not where its
    /// bounds are), else only what the view shows.
    fn in_view(&self, mats: &Mats, frame: &Frame, styles: &StyleStore, i: usize, vp: (f32, f32)) -> bool {
        self.sel.is_some() || mats.m[i] != crate::effects::IDENTITY || !offscreen(frame, styles, i, vp)
    }

    fn push(&mut self, kind: Kind, coords: Coords, fill: Rgba) {
        if self.overlaps_widget(&kind, &coords) {
            self.segments.push(self.items.len() as u32); // drawn over that widget: a canvas above it
            self.over.clear();
        }
        let fill = fill.opacity(self.opacity);
        let kind = match kind {
            Kind::Rect { radius, outline, width } => {
                Kind::Rect { radius, outline: outline.opacity(self.opacity), width }
            }
            Kind::Oval { outline, width } => Kind::Oval { outline: outline.opacity(self.opacity), width },
            k => k,
        };
        self.items.push(Item { kind, coords, fill, clip: self.clip, through: self.through, snap: self.snap });
    }

    fn corners(b: Bounds<f32>) -> Coords {
        Coords::Inline([b.origin.x, b.origin.y, b.origin.x + b.size.width, b.origin.y + b.size.height], 4)
    }

    /// A filled rectangle (gpui's `paint_quad(fill(bounds, color))`).
    pub fn fill(&mut self, bounds: Bounds<f32>, color: impl Into<Rgba>) {
        let c = color.into();
        if !c.is_transparent() && bounds.size.width > 0.0 && bounds.size.height > 0.0 {
            self.push(Kind::Rect { radius: 0.0, outline: Rgba::TRANSPARENT, width: 0.0 },
                      Self::corners(bounds), c);
        }
    }

    /// A selection highlight: a rectangle that is pushed even when transparent (see `paint_selection`).
    fn highlight(&mut self, bounds: Bounds<f32>, color: Rgba) {
        self.push(Kind::Rect { radius: 0.0, outline: Rgba::TRANSPARENT, width: 0.0 }, Self::corners(bounds), color);
    }

    /// A rounded rectangle with an optional border (gpui's `quad`).
    pub fn quad(&mut self, bounds: Bounds<f32>, radius: f32, fill: impl Into<Rgba>,
                border: impl Into<Rgba>, border_width: f32) {
        let (fill, border) = (fill.into(), border.into());
        if fill.is_transparent() && (border.is_transparent() || border_width <= 0.0) {
            return;
        }
        let max_r = bounds.size.width.min(bounds.size.height) / 2.0;
        self.push(Kind::Rect { radius: radius.clamp(0.0, max_r.max(0.0)), outline: border,
                               width: border_width },
                  Self::corners(bounds), fill);
    }

    /// CSS `box-shadow` (an outer one): the rounded box `shadow` (corner `radius`) blurred by a
    /// Gaussian of `sigma`, in `color`, showing only outside the rounded box `cast` (corner
    /// `cast_radius`) that casts it.
    fn shadow(&mut self, (shadow, radius): (Bounds<f32>, f32), (cast, cast_radius): (Bounds<f32>, f32), sigma: f32,
              color: Rgba) {
        if color.is_transparent() || shadow.size.width <= 0.0 || shadow.size.height <= 0.0 {
            return;
        }
        let corners = |b: Bounds<f32>| [b.origin.x, b.origin.y, b.origin.x + b.size.width, b.origin.y + b.size.height];
        let coords: Vec<f32> = corners(shadow).into_iter().chain(corners(cast)).collect();
        self.push(Kind::Shadow { radius: radius.max(0.0), cast_radius: cast_radius.max(0.0), sigma: sigma.max(0.0) },
                  Coords::Heap(coords), color);
    }

    /// An ellipse inscribed in `bounds`.
    pub fn oval(&mut self, bounds: Bounds<f32>, fill: impl Into<Rgba>, outline: impl Into<Rgba>,
                width: f32) {
        self.push(Kind::Oval { outline: outline.into(), width }, Self::corners(bounds), fill.into());
    }

    /// A polyline through `points`.
    pub fn line(&mut self, points: &[(f32, f32)], color: impl Into<Rgba>, width: f32) {
        self.push(Kind::Line { width }, Coords::of_points(points), color.into());
    }

    /// A filled polygon (gpui's `paint_path`).
    pub fn polygon(&mut self, points: &[(f32, f32)], color: impl Into<Rgba>) {
        self.push(Kind::Polygon, Coords::of_points(points), color.into());
    }

    /// A pie slice: degrees counter-clockwise from 3 o'clock.
    pub fn arc(&mut self, bounds: Bounds<f32>, start: f32, extent: f32, color: impl Into<Rgba>) {
        self.push(Kind::Arc { start, extent }, Self::corners(bounds), color.into());
    }

    /// Text with its top-left at (x, y).
    pub fn text(&mut self, x: f32, y: f32, text: &str, size: f32, color: impl Into<Rgba>) {
        self.styled_text(x, y, text, size, 0, color.into());
    }

    pub(crate) fn styled_text(&mut self, x: f32, y: f32, text: &str, size: f32, flags: i32, color: Rgba) {
        if !text.trim().is_empty() {
            let start = self.text.len() as u32;
            self.text.extend(text.bytes().filter(|&b| b != 0));
            self.text.push(0);
            self.push(Kind::Text { start, size, flags }, Coords::Inline([x, y, 0.0, 0.0], 2), color);
        }
    }

    /// An image scaled to `bounds` (at the painter's opacity: its fill's alpha).
    pub fn image(&mut self, bounds: Bounds<f32>, image: &Arc<Image>) {
        self.push(Kind::Image(image.clone()), Self::corners(bounds), Rgba { r: 0.0, g: 0.0, b: 0.0, a: 1.0 });
    }

    /// An image over everything painted so far, at `opacity`, unclipped (a view transition's pictures).
    pub(crate) fn overlay(&mut self, bounds: Bounds<f32>, image: &Arc<Image>, opacity: f32) {
        if opacity <= 0.0 {
            return;
        }
        let (o, c) = (self.opacity, self.clip);
        (self.opacity, self.clip) = (opacity.min(1.0), None);
        self.image(bounds, image);
        (self.opacity, self.clip) = (o, c);
    }

    /// An image file (any format the kit reads) scaled to `bounds`. It
    /// decodes on the kit's worker threads at the size drawn and appears when
    /// ready; nothing loads while it is off screen.
    pub fn image_file(&mut self, bounds: Bounds<f32>, src: &str) {
        self.file(bounds, src, sys::BK_FIT_FILL, None);
    }

    /// An image file covering `bounds` (CSS `object-fit: cover`: thumbnails).
    pub fn image_file_cover(&mut self, bounds: Bounds<f32>, src: &str) {
        self.file(bounds, src, sys::BK_FIT_COVER, None);
    }

    /// An image file scaled to `bounds`, an animation (GIF, animated WebP)
    /// held on `frame`; `None` plays it (what `image_file` does: each frame
    /// for its delay, looping as the file says, paused while off screen).
    pub fn image_file_frame(&mut self, bounds: Bounds<f32>, src: &str, frame: Option<u32>) {
        self.file(bounds, src, sys::BK_FIT_FILL, frame);
    }

    /// A video's current frame fitted into `bounds` (`fit`: `sys::BK_FIT_*`); it
    /// plays while drawn, and nothing decodes while it isn't.
    pub fn video(&mut self, bounds: Bounds<f32>, video: &std::rc::Rc<crate::video::Video>, fit: i32) {
        self.push(Kind::Video(video.clone(), fit), Self::corners(bounds), Rgba::TRANSPARENT);
    }

    /// Whether the window being painted is drawn by a GPU renderer right now (the first frames
    /// of a GPU window use the software raster while the device starts; off-screen drawing is
    /// software). A paint callback that has both a GPU effect and a CPU way to draw picks one.
    #[must_use]
    pub const fn is_gpu(&self) -> bool {
        self.gpu
    }

    /// Set by the window before it paints.
    pub(crate) const fn set_gpu(&mut self, on: bool) {
        self.gpu = on;
    }

    /// A GPU effect over `bounds` (see [`crate::shader`]): `inputs` are the BUI textures it samples
    /// (`Image::texture`; at most [`ShaderEffect::MAX_INPUTS`], the rest are ignored), `floats` its
    /// `U[..]` (at most [`ShaderEffect::MAX_FLOATS`]). The shader sees device pixels. Where the GPU
    /// cannot run it, `fallback` is drawn fitted into `bounds` (nothing when `None`). Floats that
    /// change every frame repaint the canvas every frame.
    pub fn effect(&mut self, bounds: Bounds<f32>, effect: &ShaderEffect, inputs: &[i32], floats: &[f32],
                  fallback: Option<&Arc<Image>>) {
        self.effect_holding(bounds, effect, inputs, floats, fallback, None);
    }

    /// [`Painter::effect`], the item holding `texture` (one of `inputs`) while it is drawn.
    fn effect_holding(&mut self, bounds: Bounds<f32>, effect: &ShaderEffect, inputs: &[i32], floats: &[f32],
                      fallback: Option<&Arc<Image>>, texture: Option<Arc<Texture>>) {
        let mut slots = [0; sys::BK_EFFECT_MAX_INPUTS];
        for (slot, id) in slots.iter_mut().zip(inputs) {
            *slot = *id;
        }
        let floats = floats[..floats.len().min(ShaderEffect::MAX_FLOATS)].to_vec();
        let item = EffectItem { id: effect.id(), inputs: slots, floats, fallback: fallback.cloned(), texture };
        self.push(Kind::Effect(Box::new(item)), Self::corners(bounds), Rgba::TRANSPARENT);
    }

    /// A pass of a GPU effect into `target` instead of the window (see [`RenderTarget`]): `effect` over
    /// all of it, sampling `inputs` with `floats` as [`effect`](Self::effect) does. `bounds` is the
    /// canvas's own box. The pass runs when the whole canvas is redrawn, once, in the order the passes
    /// are recorded and before the items after it (which may sample what it drew). Where the GPU cannot
    /// run it nothing happens.
    pub fn effect_to(&mut self, bounds: Bounds<f32>, target: &RenderTarget, effect: &ShaderEffect, inputs: &[i32],
                     floats: &[f32]) {
        let mut slots = [0; sys::BK_EFFECT_MAX_INPUTS];
        for (slot, id) in slots.iter_mut().zip(inputs) {
            *slot = *id;
        }
        let floats = floats[..floats.len().min(ShaderEffect::MAX_FLOATS)].to_vec();
        let item = PassItem { target: target.texture(), effect: effect.id(), inputs: slots, floats };
        self.push(Kind::Pass(Box::new(item)), Self::corners(bounds), Rgba::TRANSPARENT);
    }

    /// Fill all of `target` with `rgba` as given (a pass: see [`effect_to`](Self::effect_to)).
    pub fn clear_target(&mut self, bounds: Bounds<f32>, target: &RenderTarget, rgba: [f32; 4]) {
        let item = PassItem { target: target.texture(), effect: 0, inputs: [0; sys::BK_EFFECT_MAX_INPUTS], floats: rgba.to_vec() };
        self.push(Kind::Pass(Box::new(item)), Self::corners(bounds), Rgba::TRANSPARENT);
    }

    fn file(&mut self, bounds: Bounds<f32>, src: &str, fit: i32, frame: Option<u32>) {
        if !src.is_empty() {
            let start = self.text.len() as u32;
            self.text.extend(src.bytes().filter(|&b| b != 0));
            self.text.push(0);
            let frame = frame.map_or(0, |f| f.saturating_add(1).min(i32::MAX as u32) as i32);
            self.push(Kind::File { start, fit, frame }, Self::corners(bounds), Rgba::TRANSPARENT);
        }
    }

    /// NUL-terminated bytes at `start` of the text arena (without the NUL).
    fn arena_str(&self, start: u32) -> &[u8] {
        let t = &self.text[start as usize..];
        &t[..t.iter().position(|&b| b == 0).unwrap_or(t.len())]
    }

    /// Restrict the following drawing to `bounds` (intersected with any current clip).
    pub fn with_clip(&mut self, bounds: Bounds<f32>, f: impl FnOnce(&mut Painter)) {
        let saved = self.clip;
        self.clip = Some(match saved {
            Some(c) => c.intersect(&bounds),
            None => bounds,
        });
        f(self);
        self.clip = saved;
    }

    /// The kit item for `it`, scaled to device pixels; its coordinates go
    /// into `coords` (point `ci.coords` at them once filled). Images are
    /// borrowed by the kit: the painter keeps them alive until its next
    /// flush. With `sync` (offscreen layers, drawn once) files decode now
    /// and `sync` keeps them for the call. None: nothing to draw.
    fn build_item(&self, it: &Item, scale: f32, coords: &mut Vec<f32>, sync: Option<&mut Vec<Arc<Image>>>)
                  -> Option<sys::BKCanvasItem> {
        coords.clear();
        coords.extend(it.coords.as_slice().iter().map(|v| v * scale));
        let mut ci = sys::BKCanvasItem { fill: it.fill.to_array(), no_hit: i32::from(it.through), ..Default::default() };
        match &it.kind {
            Kind::Rect { radius, outline, width } => {
                ci.type_ = if *radius > 0.0 { sys::BK_ITEM_ROUNDRECT } else { sys::BK_ITEM_RECT };
                ci.radius = radius * scale;
                if *width > 0.0 {
                    ci.outline = outline.to_array();
                    ci.width = (width * scale).max(1.0);
                }
            }
            Kind::Oval { outline, width } => {
                ci.type_ = sys::BK_ITEM_OVAL;
                if *width > 0.0 {
                    ci.outline = outline.to_array();
                    ci.width = (width * scale).max(1.0);
                }
            }
            Kind::Line { width } => {
                ci.type_ = sys::BK_ITEM_LINE;
                ci.width = (width * scale).max(1.0);
            }
            Kind::Polygon => ci.type_ = sys::BK_ITEM_POLYGON,
            Kind::Shadow { radius, cast_radius, sigma } => {
                ci.type_ = sys::BK_ITEM_SHADOW;
                ci.radius = radius * scale;
                ci.size = cast_radius * scale;
                ci.width = sigma * scale;
                ci.no_hit = 1;
            }
            Kind::Arc { start, extent } => {
                ci.type_ = sys::BK_ITEM_ARC;
                ci.start = *start;
                ci.extent = *extent;
            }
            Kind::Text { start, size, flags } => {
                ci.type_ = sys::BK_ITEM_TEXT;
                ci.size = size * scale;
                (ci.family, ci.text_flags) = crate::text::family(*flags);
                ci.anchor = sys::BK_ANCHOR_N | sys::BK_ANCHOR_W;
                ci.text = self.text[*start as usize..].as_ptr().cast();
            }
            Kind::Image(img) => {
                ci.type_ = sys::BK_ITEM_IMAGE;
                ci.imbuf = img.as_ptr();
                ci.fit = sys::BK_FIT_FILL;
                // the fill carries the image's opacity, which it is drawn at
                ci.fade = 1.0 - it.fill.a;
                ci.fill = [0.0; 4];
            }
            Kind::Video(v, fit) => {
                ci.type_ = sys::BK_ITEM_IMAGE;
                ci.video = v.as_ptr();
                ci.fit = *fit;
            }
            Kind::Effect(e) => {
                ci.type_ = sys::BK_ITEM_EFFECT;
                ci.effect = e.id;
                ci.effect_inputs = e.inputs;
                ci.effect_floats = e.floats.as_ptr();
                ci.n_effect_floats = e.floats.len() as i32;
                ci.fit = sys::BK_FIT_FILL;
                ci.imbuf = e.fallback.as_ref().map_or(std::ptr::null(), |img| img.as_ptr());
            }
            Kind::Pass(p) => {
                ci.type_ = sys::BK_ITEM_PASS;
                ci.effect = p.effect;
                ci.effect_inputs = p.inputs;
                ci.effect_floats = p.floats.as_ptr();
                ci.n_effect_floats = p.floats.len() as i32;
                ci.effect_target = p.target;
            }
            Kind::File { start, fit, frame } => {
                ci.type_ = sys::BK_ITEM_IMAGE;
                ci.fit = *fit;
                ci.frame = *frame;
                match sync {
                    Some(keep) => {
                        // offscreen layers are drawn once: an animation shows its first frame
                        let img = file_pixels(std::str::from_utf8(self.arena_str(*start)).ok()?)?;
                        ci.imbuf = img.as_ptr();
                        keep.push(img);
                    }
                    None => ci.src = self.text[*start as usize..].as_ptr().cast(),
                }
            }
        }
        // a CSS box laid out at fractional positions: each edge on its nearest device pixel (so
        // neighbours still meet), not blended across two
        let snap = it.snap && matches!(it.kind, Kind::Rect { .. } | Kind::Image(_) | Kind::File { .. } | Kind::Video(..));
        if snap {
            coords.iter_mut().for_each(|v| *v = v.round());
        }
        if let Some(c) = it.clip {
            ci.clip_on = 1;
            let (x, y) = (c.origin.x * scale, c.origin.y * scale);
            let (r, b) = (x + c.size.width * scale, y + c.size.height * scale);
            ci.clip = if snap {
                [x.round(), y.round(), r.round() - x.round(), b.round() - y.round()]
            } else {
                [x, y, r - x, b - y]
            };
        }
        ci.ncoords = coords.len() as i32;
        Some(ci)
    }

    /// Replace the canvas display list with these items, scaled to device
    /// pixels (after a [`repaint`](Self::repaint): only the items it changed).
    pub(crate) fn flush(&mut self, canvases: &[*mut sys::BKNode], scale: f32) {
        let canvas = canvases[0];
        let mut coords: Vec<f32> = Vec::new();
        if std::mem::take(&mut self.partial) {
            let partial_ok = self.changed.iter().all(|&k| {
                let slot = self.slots[k as usize];
                let Some(mut ci) = self.build_item(&self.items[k as usize], scale, &mut coords, None) else {
                    return slot < 0;
                };
                if slot < 0 {
                    return false; // it shows now: indices shift, send everything
                }
                ci.coords = coords.as_ptr();
                // SAFETY: as below
                unsafe { sys::BK_canvas_put_item(canvas, slot, &ci) };
                true
            });
            if partial_ok {
                return;
            }
        }
        self.slots.clear();
        // segment k is items[lo..hi] on canvas k (canvases past the last one empty)
        for (k, &canvas) in canvases.iter().enumerate() {
            let lo = if k == 0 { 0 } else { self.segments.get(k - 1).map_or(self.items.len(), |&s| s as usize) };
            let hi = self.segments.get(k).map_or(self.items.len(), |&s| s as usize);
            let mut count: i32 = 0;
            for it in &self.items[lo..hi.max(lo)] {
                let Some(mut ci) = self.build_item(it, scale, &mut coords, None) else {
                    self.slots.push(-1);
                    continue;
                };
                ci.coords = coords.as_ptr();
                // SAFETY: every pointer in `ci` outlives the call; the kit copies
                // them into the slot's existing buffers.
                unsafe { sys::BK_canvas_put_item(canvas, count, &ci) };
                self.slots.push(count);
                count += 1;
            }
            // SAFETY: `canvas` is a live kit canvas node owned by the window.
            unsafe { sys::BK_canvas_truncate(canvas, count) };
        }
    }

    /// Draw the items offscreen into a `w` x `h` device-pixel region whose
    /// top left is (`x0`, `y0`) (CSS effects), as floats for the effects
    /// that work on them.
    pub(crate) fn rasterize(&self, scale: f32, x0: f32, y0: f32, w: usize, h: usize)
                            -> Option<crate::effects::Pixels> {
        self.rasterize_argb(scale, x0, y0, w, h).map(|a| crate::effects::Pixels::from_argb(&a))
    }

    /// [`rasterize`](Self::rasterize) as premultiplied ARGB words: the kit's
    /// image format, kept end to end by a layer that only blurs.
    pub(crate) fn rasterize_argb(&self, scale: f32, x0: f32, y0: f32, w: usize, h: usize)
                                 -> Option<crate::effects::Argb> {
        if w == 0 || h == 0 || w > crate::effects::MAX_RASTER || h > crate::effects::MAX_RASTER {
            return None;
        }
        let mut coords: Vec<Vec<f32>> = Vec::with_capacity(self.items.len());
        let mut keep: Vec<Arc<Image>> = Vec::new();
        let mut items: Vec<sys::BKCanvasItem> = Vec::with_capacity(self.items.len());
        for it in &self.items {
            let mut c = Vec::new();
            if let Some(ci) = self.build_item(it, scale, &mut c, Some(&mut keep)) {
                items.push(ci);
                coords.push(c);
            }
        }
        for (ci, c) in items.iter_mut().zip(&coords) {
            ci.coords = c.as_ptr();
        }
        let mut px = vec![0u32; w * h];
        // SAFETY: `items` point into `coords`, `keep` and `self.text`, all alive here.
        let ok = unsafe {
            sys::BK_canvas_rasterize(items.as_ptr(), items.len() as i32, x0, y0, w as i32, h as i32,
                                     px.as_mut_ptr())
        };
        drop(keep);
        (ok != 0).then_some(crate::effects::Argb { w, h, px })
    }

    /// A hash of everything drawn into a layer over `area` (layer caching),
    /// placed relative to (`dx`, `dy`) to 1/64 px: the same content moved
    /// hashes the same. A clip that holds all of `area` cuts nothing, so it
    /// is left out (a layer moving under a still clip stays the same).
    fn content_hash(&self, h: &mut impl std::hash::Hasher, area: Bounds<f32>, dx: f32, dy: f32) {
        use std::hash::Hash;
        for it in &self.items {
            for xy in it.coords.as_slice().chunks(2) {
                quantize(xy[0] - dx).hash(h);
                xy.get(1).map(|y| quantize(y - dy)).hash(h);
            }
            it.fill.to_array().map(f32::to_bits).hash(h);
            match &it.kind {
                Kind::Rect { radius, outline, width } => {
                    (0u8, radius.to_bits(), outline.to_array().map(f32::to_bits), width.to_bits()).hash(h)
                }
                Kind::Oval { outline, width } => (1u8, outline.to_array().map(f32::to_bits), width.to_bits()).hash(h),
                Kind::Line { width } => (2u8, width.to_bits()).hash(h),
                Kind::Polygon => 3u8.hash(h),
                Kind::Shadow { radius, cast_radius, sigma } => {
                    (11u8, radius.to_bits(), cast_radius.to_bits(), sigma.to_bits()).hash(h)
                }
                Kind::Arc { start, extent } => (4u8, start.to_bits(), extent.to_bits()).hash(h),
                Kind::Text { start, size, flags } => {
                    let t = &self.text[*start as usize..];
                    let end = t.iter().position(|&b| b == 0).unwrap_or(t.len());
                    (5u8, &t[..end], size.to_bits(), flags).hash(h)
                }
                Kind::Image(img) => (6u8, Arc::as_ptr(img) as usize).hash(h),
                Kind::File { start, fit, frame } => (7u8, self.arena_str(*start), fit, frame).hash(h),
                Kind::Video(v, fit) => (8u8, std::rc::Rc::as_ptr(v) as usize, fit).hash(h),
                Kind::Pass(p) => {
                    (10u8, p.target, p.effect, p.inputs).hash(h);
                    p.floats.iter().for_each(|v| v.to_bits().hash(h));
                }
                Kind::Effect(e) => {
                    // the floats move every frame: a changed value is a changed layer
                    let fallback = e.fallback.as_ref().map_or(0, |img| Arc::as_ptr(img) as usize);
                    (9u8, e.id, e.inputs, fallback).hash(h);
                    e.floats.iter().for_each(|v| v.to_bits().hash(h));
                }
            }
            if let Some(c) = it.clip.filter(|c| c.intersect(&area) != area) {
                (quantize(c.origin.x - dx), quantize(c.origin.y - dy), quantize(c.size.width),
                 quantize(c.size.height)).hash(h);
            }
        }
    }
}

// ==========================================================================
// Painting a frame

fn lp(v: LengthPercentage, basis: f32) -> f32 {
    use taffy::style::ExpandedLengthPercentage as E;
    match v.expand() {
        E::Length(p) => p,
        E::Percent(f) => f * basis,
        // SAFETY: calc pointers in styles come from crate::calc's interner
        E::Calc(ptr) => unsafe { crate::calc::Resolved::from_ptr(ptr) }.eval(basis),
    }
}

/// Border widths (top, right, bottom, left).
pub(crate) fn border_widths(c: &Computed, w: f32) -> [f32; 4] {
    let b = c.layout.border;
    [lp(b.top, w), lp(b.right, w), lp(b.bottom, w), lp(b.left, w)]
}

/// The box's largest corner radius, at most half its shorter side: one radius for what draws a
/// single one (shadows, outlines, layers' masks).
fn radius_px(c: &Computed, b: Bounds<f32>, vp: (f32, f32)) -> f32 {
    corner_radii(c, b, vp).into_iter().fold(0.0, f32::max).min(b.size.width.min(b.size.height) / 2.0)
}

/// The corner radii of box `b` (top-left, top-right, bottom-right, bottom-left), shrunk together
/// where two on one side would overlap (CSS Backgrounds 3 §5.5).
fn corner_radii(c: &Computed, b: Bounds<f32>, vp: (f32, f32)) -> [f32; 4] {
    if c.radius.iter().all(|r| *r == crate::geometry::Length::Px(0.0)) {
        return [0.0; 4];
    }
    let u = c.units(vp);
    let (w, h) = (b.size.width, b.size.height);
    let r: [f32; 4] = std::array::from_fn(|k| c.radius[k].resolve(&u, w.min(h)).clamp(0.0, 1.0e6));
    let f = [w / (r[0] + r[1]), h / (r[1] + r[2]), w / (r[2] + r[3]), h / (r[3] + r[0])]
        .into_iter()
        .filter(|f| f.is_finite())
        .fold(1.0f32, f32::min)
        .max(0.0);
    r.map(|v| v * f)
}

/// Whether all four radii are one (the kit's rounded box draws those).
fn one_radius(r: &[f32; 4]) -> bool {
    r.iter().all(|v| (v - r[0]).abs() < 0.01)
}

/// The radii of box `b`'s corners on a box `d` inside it on every side (a border, padding or
/// content box): each smaller by how far in it is, never below 0.
fn radii_inside(r: [f32; 4], b: Bounds<f32>, d: Bounds<f32>) -> [f32; 4] {
    let (l, t) = (d.origin.x - b.origin.x, d.origin.y - b.origin.y);
    let rt = b.origin.x + b.size.width - d.origin.x - d.size.width;
    let bm = b.origin.y + b.size.height - d.origin.y - d.size.height;
    [(r[0] - l.max(t)).max(0.0), (r[1] - rt.max(t)).max(0.0), (r[2] - rt.max(bm)).max(0.0), (r[3] - l.max(bm)).max(0.0)]
}

/// Paint every box of `frame`, in stacking order. `scale`: device pixels
/// per logical pixel (for elements whose effects work on pixels).
pub(crate) fn paint_frame(frame: &Frame, styles: &StyleStore, tc: &mut TextCache, p: &mut Painter,
                          vp: (f32, f32), scale: f32, app: &crate::window::App) {
    with_scratch(|s| paint_frame_with(s, frame, styles, tc, p, vp, scale, app));
    p.opacity = 1.0;
    p.clip = None;
}

/// The boxes of `frame` that `keep` selects, painted alone into one picture of `area` (a view
/// transition's snapshot). Layer effects inside (filters, blend modes) are left out, and kit
/// widgets (form controls) aren't in it.
#[allow(clippy::too_many_arguments)]
pub(crate) fn snapshot(frame: &Frame, styles: &StyleStore, tc: &mut TextCache, keep: &dyn Fn(usize) -> bool,
                       area: Bounds<f32>, vp: (f32, f32), scale: f32, app: &crate::window::App) -> Option<Arc<Image>> {
    let mut p = Painter::new();
    p.scale = scale;
    with_scratch(|s| {
        let PaintScratch { order, mats, .. } = s;
        order.clear();
        order.extend((0..frame.nodes.len()).filter(|&i| keep(i)));
        if frame.nodes.iter().any(|n| styles[n.style].z_index.is_some()) {
            order.sort_by_cached_key(|&i| frame.z_of(i, styles));
        }
        vector_mats(frame, styles, vp, mats);
        for &i in order.iter() {
            let n = &frame.nodes[i];
            if !n.visible || n.opacity <= 0.0 {
                continue;
            }
            p.opacity = n.opacity;
            p.clip = n.clip;
            let start = p.items.len();
            paint_node(frame, styles, tc, i, &mut p, vp, app);
            mats.map(&mut p, start, i);
        }
    });
    let (x0, y0, w, h) = device_rect(area, scale);
    if w == 0 || h == 0 {
        return None;
    }
    let px = p.rasterize_argb(scale, x0, y0, w.min(crate::effects::MAX_RASTER), h.min(crate::effects::MAX_RASTER))?;
    Image::from_argb(&px).map(Arc::new)
}

/// [`paint_frame`] with the paint's buffers.
#[allow(clippy::too_many_arguments)]
fn paint_frame_with(s: &mut PaintScratch, frame: &Frame, styles: &StyleStore, tc: &mut TextCache, p: &mut Painter,
                    vp: (f32, f32), scale: f32, app: &crate::window::App) {
    let PaintScratch { order, mats, done, tour, subs } = s;
    p.scale = scale;
    order.clear();
    match frame.order_modified(styles) {
        Some(o) => order.extend(o),
        None => order.extend(0..frame.nodes.len()),
    }
    if frame.nodes.iter().any(|n| styles[n.style].z_index.is_some()) {
        order.sort_by_cached_key(|&i| frame.z_of(i, styles)); // stable: tree order within a level
    }
    vector_mats(frame, styles, vp, mats);
    // the canvas's background, under every box
    let (_, canvas) = canvas_background(frame, styles);
    if let Some(root) = frame.nodes.first() {
        let r = root.bounds;
        let (w, h) = (vp.0.max(r.origin.x + r.size.width), vp.1.max(r.origin.y + r.size.height));
        p.opacity = 1.0;
        p.clip = None;
        p.quad(rect(0.0, 0.0, w, h), 0.0, canvas, Rgba::TRANSPARENT, 0.0);
    }
    if !mats.layered {
        // each box's items, for redraws that paint only what moved
        p.ranges.clear();
        p.ranges.resize(frame.nodes.len(), (u32::MAX, 0));
        for &i in order.iter() {
            let n = &frame.nodes[i];
            if !n.visible || n.opacity <= 0.0 {
                continue;
            }
            p.opacity = n.opacity;
            p.clip = n.clip;
            if matches!(n.content, Content::Widget(_)) {
                p.note_widget(n.key, n.clip.map_or(n.bounds, |c| c.intersect(&n.bounds)));
            }
            let start = p.items.len();
            if p.in_view(mats, frame, styles, i, vp) {
                paint_node(frame, styles, tc, i, p, vp, app);
            }
            mats.map(p, start, i);
            p.ranges[i] = (start as u32, p.items.len() as u32);
        }
        // redraws that paint only what moved: one canvas only (items map to canvas slots)
        p.retained = p.segments.is_empty();
        p.text_full = p.text.len();
    } else {
        done.clear();
        done.resize(frame.nodes.len(), false);
        tour.build(frame);
        let mut cx = LayerCx { frame, styles, tc, vp, scale, app, done, mats, tour, subs };
        paint_list(&mut cx, order, p, None, 1.0);
    }
    end_layers_frame();
}

/// Buffers a paint fills, kept between paints so a frame allocates none of them.
#[derive(Default)]
struct PaintScratch {
    order: Vec<usize>,
    mats: Mats,
    done: Vec<bool>,
    tour: Tour,
    /// The boxes of the layers being painted (one list per nesting level).
    subs: Vec<Vec<usize>>,
}

thread_local! {
    static SCRATCH: std::cell::RefCell<PaintScratch> = std::cell::RefCell::new(PaintScratch::default());
}

/// `f` with the paint buffers (taken out while it runs: a paint inside a
/// paint gets fresh ones).
fn with_scratch<R>(f: impl FnOnce(&mut PaintScratch) -> R) -> R {
    let mut s = SCRATCH.with(|c| std::mem::take(&mut *c.borrow_mut()));
    let r = f(&mut s);
    SCRATCH.with(|c| *c.borrow_mut() = s);
    r
}

struct LayerCx<'a> {
    frame: &'a Frame,
    styles: &'a StyleStore,
    tc: &'a mut TextCache,
    vp: (f32, f32),
    scale: f32,
    app: &'a crate::window::App,
    done: &'a mut Vec<bool>,
    mats: &'a Mats,
    tour: &'a Tour,
    subs: &'a mut Vec<Vec<usize>>,
}

/// Each box's vector transform: its own move and scale and its ancestors'
/// ([`Effects::vector`](crate::effects::Effects::vector)), applied to what it
/// paints; the clip from outside its nearest transformed ancestor (that one
/// stays put); and whether some box needs a pixel layer.
#[derive(Default)]
struct Mats {
    m: Vec<crate::effects::Affine>,
    outer: Vec<OuterClip>,
    /// Boxes whose move and scale goes through a pixel layer all the same: a
    /// non-uniform scale over text (a text item scales by one size), or one
    /// over effects on pixels (a mask, a clip path, a filter, a backdrop:
    /// they work at the boxes' own bounds, not where the scale puts them).
    pixel: Vec<bool>,
    /// What each box's subtree holds (the buffer of [`vector_mats`]).
    under: Vec<Under>,
    layered: bool,
}

/// What a box's subtree holds that a vector scale over it can't carry.
#[derive(Clone, Copy, Default)]
struct Under {
    /// It draws text (the box included).
    text: bool,
    /// A box under it (not the box) has effects on pixels.
    pixels: bool,
}

/// The clip from outside a box's nearest transformed ancestor: as layout gave
/// it (`.0`, what its items carry) and where it draws (`.1`: through the
/// transforms above that ancestor, not through the ancestor's own).
type OuterClip = (Option<Bounds<f32>>, Option<Bounds<f32>>);

/// Where clip `c` of an item drawn through `m` draws: the outer clip where it
/// draws; a clip made inside the transformed boxes through `m`, within it.
fn clip_through(c: Option<Bounds<f32>>, m: &crate::effects::Affine, outer: OuterClip) -> Option<Bounds<f32>> {
    if c == outer.0 {
        return outer.1;
    }
    let mapped = map_rect(c?, m);
    Some(outer.1.map_or(mapped, |o| mapped.intersect(&o)))
}

impl Mats {
    /// Map what box `i` painted (the items from `from` on).
    fn map(&self, p: &mut Painter, from: usize, i: usize) {
        if self.m[i] != crate::effects::IDENTITY {
            p.map_items(from, &self.m[i], self.outer[i]);
        }
    }

    /// Where clip `c` of box `i`'s items draws, as [`map`](Self::map) leaves it.
    fn clip(&self, c: Option<Bounds<f32>>, i: usize) -> Option<Bounds<f32>> {
        if self.m[i] == crate::effects::IDENTITY { c } else { clip_through(c, &self.m[i], self.outer[i]) }
    }
}

/// The effects of box `i`, when it owns them (see [`owns_style`](crate::layout::owns_style)).
fn effects_of<'s>(frame: &Frame, styles: &'s StyleStore, i: usize) -> Option<&'s crate::effects::Effects> {
    let n = &frame.nodes[i];
    if crate::layout::owns_style(&n.content) { styles[n.style].effects.as_deref() } else { None }
}

fn vector_mats(frame: &Frame, styles: &StyleStore, vp: (f32, f32), out: &mut Mats) {
    use crate::effects::{mul, IDENTITY};
    let nodes = &frame.nodes;
    out.m.clear();
    out.outer.clear();
    out.pixel.clear();
    out.layered = false;
    // children come after their parents: backwards, what a subtree holds reaches its root
    out.under.clear();
    out.under.resize(nodes.len(), Under::default());
    for (i, n) in nodes.iter().enumerate().rev() {
        let text = out.under[i].text || matches!(n.content, Content::Word { .. } | Content::Text(_));
        out.under[i].text = text;
        if let Some(p) = n.parent {
            let pixels = out.under[i].pixels
                || effects_of(frame, styles, i).is_some_and(|e| e.needs_pixels() || !e.backdrop.is_empty());
            let up = &mut out.under[p];
            up.text |= text;
            up.pixels |= pixels;
        }
    }
    for (i, n) in nodes.iter().enumerate() {
        // parents come before their children
        let (mut m, mut outer) = n.parent.and_then(|p| Some((*out.m.get(p)?, out.outer[p]))).unwrap_or((IDENTITY, (None, None)));
        if let Some(e) = effects_of(frame, styles, i) {
            let u = styles[n.style].units(vp);
            out.layered |= !e.backdrop.is_empty() || e.needs_layer(n.bounds, &u);
            let vector = e.vector(n.bounds, &u);
            let under = out.under[i];
            let pixel = vector.is_some_and(|v| under.pixels || (under.text && (v[0] - v[3]).abs() > crate::effects::AXIS_EPSILON));
            out.layered |= pixel;
            out.pixel.push(pixel);
            if let Some(own) = vector.filter(|_| !pixel) {
                // its clip draws through the transforms above it, not its own
                outer = (n.clip, clip_through(n.clip, &m, outer));
                m = mul(&m, &own);
            }
        }
        else {
            out.pixel.push(false);
        }
        out.m.push(m);
        out.outer.push(outer);
    }
}

/// Points (x, y pairs) through `m`.
fn map_points(xy: &mut [f32], m: &crate::effects::Affine) {
    for p in xy.as_chunks_mut::<2>().0 {
        (p[0], p[1]) = crate::effects::apply(m, p[0], p[1]);
    }
}

/// A rectangle through `m` (a move and a scale: it stays a rectangle).
fn map_rect(b: Bounds<f32>, m: &crate::effects::Affine) -> Bounds<f32> {
    let (x0, y0) = crate::effects::apply(m, b.origin.x, b.origin.y);
    let (x1, y1) = crate::effects::apply(m, b.origin.x + b.size.width, b.origin.y + b.size.height);
    rect(x0.min(x1), y0.min(y1), (x1 - x0).abs(), (y1 - y0).abs())
}

/// How many paints a layer result is kept without being used. More than one:
/// a frame can be painted more than once (a hover, a selection), and a paint
/// that finds a layer's image never asks for its raster, which the next
/// frame of a turn or a fade needs again.
const KEEP_PAINTS: u8 = 3;

/// Layer results by key, each with the paints since it was last used; one
/// unused for [`KEEP_PAINTS`] paints is dropped (see [`Caches::end_frame`]).
struct Cache<T: Clone> {
    map: HashMap<u64, (T, u8)>,
}

impl<T: Clone> Default for Cache<T> {
    fn default() -> Self {
        Cache { map: HashMap::new() }
    }
}

impl<T: Clone> Cache<T> {
    fn get(&mut self, k: u64) -> Option<T> {
        self.map.get_mut(&k).map(|e| {
            e.1 = 0;
            e.0.clone()
        })
    }
    fn put(&mut self, k: u64, v: T) {
        self.map.insert(k, (v, 0));
    }
    fn end_frame(&mut self) {
        self.map.retain(|_, e| {
            e.1 += 1;
            e.1 <= KEEP_PAINTS
        });
    }
}

/// A layer's three stages, each cached by what it depends on, so a frame
/// redoes only the stages whose inputs changed:
/// - `rasters`: its content, rasterized (a blur radius that moves reuses it);
/// - `filtered`: then filtered, clipped and masked (a turning or fading
///   layer reuses it: only the turn is done again, and a fade is the
///   image's opacity);
/// - `images`: then transformed, as the image drawn, placed relative to the
///   layer's area (a layer that only moves reuses it: nothing is done again).
///
/// A stage found in the cache costs nothing; a static page soon keeps only
/// images.
#[derive(Default)]
struct Caches {
    rasters: Cache<Arc<crate::effects::Argb>>,
    filtered: Cache<Arc<crate::effects::Argb>>,
    images: Cache<(Arc<Image>, Bounds<f32>)>,
    /// Filtered layers as GPU textures, for the GPU to turn (see [`gpu_turn`]).
    textures: Cache<Arc<Texture>>,
}

/// A paint is done: count it for every cached layer result, dropping those
/// unused for [`KEEP_PAINTS`] paints. Every paint counts, layered or not, so
/// a page that stops using layers frees their buffers.
fn end_layers_frame() {
    LAYERS.with(|l| l.borrow_mut().end_frame());
}

impl Caches {
    fn end_frame(&mut self) {
        self.rasters.end_frame();
        self.filtered.end_frame();
        self.images.end_frame();
        self.textures.end_frame();
    }
}

thread_local! {
    static LAYERS: std::cell::RefCell<Caches> = std::cell::RefCell::new(Caches::default());
    /// The shader that turns a layer on the GPU (None: it can't be declared).
    static TURN: Option<ShaderEffect> = ShaderEffect::new("bui-turn", crate::effects::TURN_SHADER, 1, 7).ok();
}

/// A BUI texture of a layer's pixels, freed with it: the layer cache and every
/// display list that draws it ([`EffectItem::texture`]) hold it.
struct Texture(i32);

impl Drop for Texture {
    fn drop(&mut self) {
        // SAFETY: ours, freed once, when nothing that draws it is left.
        unsafe { sys::BUI_texture_free(self.0) };
    }
}

/// Draws a turned layer as the GPU's: its filtered pixels `px` (cached as a
/// texture under `key`), whose top left is at `origin` in layer device pixels
/// (`layer_scale` per logical pixel), through `m_dev` (layer device pixels),
/// in a window of `scale` device pixels per logical one. No pixels are turned
/// on the CPU and nothing is uploaded while the content stays the same: a
/// turn or skew animation costs a quad a frame. False when the GPU can't
/// (no shader, it failed to compile, no texture, a degenerate matrix).
#[allow(clippy::too_many_arguments)]
fn gpu_turn(p: &mut Painter, px: &crate::effects::Argb, key: u64, origin: (f32, f32), m_dev: &crate::effects::Affine,
            (layer_scale, scale): (f32, f32), clip: Option<Bounds<f32>>, opacity: f32) -> bool {
    let size = (px.w as f32, px.h as f32);
    let Some((at, m)) = crate::effects::turn_box(origin, size, m_dev, layer_scale, scale) else { return false };
    TURN.with(|turn| {
        let Some(turn) = turn.as_ref().filter(|t| t.error().is_empty()) else { return false };
        let texture = LAYERS.with(|l| l.borrow_mut().textures.get(key)).or_else(|| {
            let id = crate::effects::turn_texture(px);
            (id > 0).then(|| {
                let t = Arc::new(Texture(id));
                LAYERS.with(|l| l.borrow_mut().textures.put(key, t.clone()));
                t
            })
        });
        let Some(texture) = texture else { return false };
        p.opacity = 1.0;
        p.clip = clip;
        let id = texture.0;
        p.effect_holding(at, turn, &[id], &[m[0], m[1], m[2], m[3], m[4], m[5], opacity], None, Some(texture));
        true
    })
}

/// Where each box enters and leaves a depth-first walk of the tree (an
/// Euler tour): box `k` is inside box `root` when its span is in `root`'s.
/// One walk a paint, then each test is two comparisons (walking up from each
/// box was quadratic in a page with many layers: a long list of blurred rows).
#[derive(Default)]
struct Tour {
    enter: Vec<u32>,
    leave: Vec<u32>,
    /// The walk's own buffers: children by parent (compressed rows) and the path.
    start: Vec<u32>,
    fill: Vec<u32>,
    kids: Vec<u32>,
    stack: Vec<(usize, u32)>,
}

/// `v` as `n` zeros, in the room it has.
fn zeroed(v: &mut Vec<u32>, n: usize) {
    v.clear();
    v.resize(n, 0);
}

impl Tour {
    fn build(&mut self, frame: &Frame) {
        let nodes = &frame.nodes;
        let n = nodes.len();
        let Tour { enter, leave, start, fill, kids, stack } = self;
        // children by parent, in arena order (compressed rows)
        zeroed(start, n + 1);
        for p in nodes.iter().filter_map(|nd| nd.parent) {
            start[p + 1] += 1;
        }
        for i in 0..n {
            start[i + 1] += start[i];
        }
        fill.clear();
        fill.extend_from_slice(start);
        zeroed(kids, n);
        for (k, nd) in nodes.iter().enumerate() {
            if let Some(p) = nd.parent {
                kids[fill[p] as usize] = k as u32;
                fill[p] += 1;
            }
        }
        zeroed(enter, n);
        zeroed(leave, n);
        stack.clear();
        let mut clock = 0u32;
        for root in (0..n).filter(|&i| nodes[i].parent.is_none()) {
            stack.push((root, start[root]));
            enter[root] = clock;
            clock += 1;
            while let Some((node, next)) = stack.last_mut() {
                if *next < start[*node + 1] {
                    let child = kids[*next as usize] as usize;
                    *next += 1;
                    enter[child] = clock;
                    clock += 1;
                    stack.push((child, start[child]));
                } else {
                    leave[*node] = clock;
                    stack.pop();
                }
            }
        }
    }

    /// Whether box `k` is `root` or under it.
    fn inside(&self, k: usize, root: usize) -> bool {
        self.enter[root] <= self.enter[k] && self.leave[k] <= self.leave[root]
    }
}

/// Paint `list` (in stacking order) into `p`; elements whose effects need
/// pixels are painted with their subtree into a layer. `root`: the layer
/// being painted (its own effects were applied by the caller); `base`: its
/// opacity (descendants paint relative to it: the layer takes it as a group).
fn paint_list(cx: &mut LayerCx, list: &[usize], p: &mut Painter, root: Option<usize>, base: f32) {
    for (idx, &i) in list.iter().enumerate() {
        if cx.done[i] {
            continue;
        }
        let n = &cx.frame.nodes[i];
        if !n.visible || n.opacity <= 0.0 {
            continue;
        }
        let fx = effects_of(cx.frame, cx.styles, i);
        if fx.is_some_and(|e| !e.backdrop.is_empty()) && Some(i) != root {
            paint_backdrop(cx, i, p);
        }
        let u = cx.styles[n.style].units(cx.vp);
        if (cx.mats.pixel[i] || fx.is_some_and(|e| e.needs_layer(n.bounds, &u))) && Some(i) != root {
            // a list kept from an earlier layer (a nested layer takes the next)
            let mut sub = cx.subs.pop().unwrap_or_default();
            sub.clear();
            sub.extend(list[idx..].iter().copied().filter(|&k| !cx.done[k] && cx.tour.inside(k, i)));
            // the layer's own pass paints these; mark them after
            paint_layer(cx, i, &sub, p, base);
            for &k in &sub {
                cx.done[k] = true;
            }
            cx.subs.push(sub);
            continue;
        }
        cx.done[i] = true;
        p.opacity = (n.opacity / base.max(1e-6)).min(1.0);
        p.clip = n.clip;
        if matches!(n.content, Content::Widget(_)) {
            p.note_widget(n.key, n.clip.map_or(n.bounds, |c| c.intersect(&n.bounds)));
        }
        let (frame, styles, vp) = (cx.frame, cx.styles, cx.vp);
        let start = p.items.len();
        paint_node(frame, styles, cx.tc, i, p, vp, cx.app);
        cx.mats.map(p, start, i);
    }
}

/// How far a box paints past its bounds: its outline, shadows and border image.
fn paint_reach(c: &Computed) -> f32 {
    let outline = c.outline.0 + c.outline.2.max(0.0);
    // a blur is a Gaussian of half its radius: it reaches three of those (see `Painter::shadow`)
    let shadows = c.shadows.iter().filter(|s| !s.inset).map(|s| s.x.abs().max(s.y.abs()) + 1.5 * s.blur + s.spread.max(0.0));
    let image = c.effects.as_ref().and_then(|e| e.border_image.as_ref()).into_iter().flat_map(|bi| bi.outset).filter_map(|o| match o {
        Side::Len(Length::Px(v)) => Some(v),
        _ => None,
    });
    shadows.chain(image).fold(outline, f32::max)
}

/// How far a text's decorations reach past its line, down or up: an explicit
/// `text-underline-offset` and `text-decoration-thickness` can put them anywhere.
fn decoration_reach(c: &Computed) -> f32 {
    if !(c.underline || c.line_through || c.overline) {
        return 0.0;
    }
    let offset = if c.underline { c.underline_offset.map_or(0.0, f32::abs) } else { 0.0 };
    offset + decoration_extent(c.decoration_style, decoration_thickness(c))
}

/// How far box `i` can paint past its bounds, (across, down); `None` when that is not known.
/// Text can spill past its box: one em per byte across, one line per byte down, plus its decorations.
fn spill(n: &BoxNode, c: &Computed) -> Option<(f32, f32)> {
    match &n.content {
        // a placeholder draws its label as wide as the label is: no bound
        Content::Replaced(Replaced::Placeholder(_)) => None,
        Content::Element | Content::Replaced(_) if n.taffy.is_some() && n.marker.is_none() => {
            Some((paint_reach(c), paint_reach(c)))
        }
        Content::Word { .. } if c.text_shadow.is_empty() => Some((c.font_size, c.font_size + decoration_reach(c))),
        Content::Text(t) if (n.taffy.is_some() || n.folded) && c.text_shadow.is_empty() => {
            // each char advances by its width and the letter spacing (paint_text_line)
            let bytes = t.len() as f32 + 1.0;
            Some((bytes * (c.font_size + c.letter_spacing.abs()), bytes * c.line_px().max(c.font_size) + decoration_reach(c)))
        }
        _ => None,
    }
}

/// Whether box `i` paints nothing the view shows: all it can reach lies
/// outside the viewport or its clip.
fn offscreen(frame: &Frame, styles: &StyleStore, i: usize, vp: (f32, f32)) -> bool {
    let n = &frame.nodes[i];
    let Some((mx, my)) = spill(n, &styles[n.style]) else { return false };
    let screen = rect(0.0, 0.0, vp.0, vp.1);
    let view = n.clip.map_or(screen, |clip| clip.intersect(&screen));
    let (b, at) = (n.bounds, view.origin);
    b.origin.x + b.size.width + mx < at.x
        || b.origin.x - mx > at.x + view.size.width
        || b.origin.y + b.size.height + my < at.y
        || b.origin.y - my > at.y + view.size.height
}

/// The logical-pixel area a layer's content covers: what it painted (text by
/// its measured width: not the boxes it sits in, which a resize widens and
/// narrows with nothing drawn in them), and the boxes of its widgets. Its
/// filters spread past it ([`crate::effects::filter_outset`]).
fn layer_area(cx: &mut LayerCx, list: &[usize], sub: &Painter) -> Bounds<f32> {
    // widgets are the kit's own nodes, not items: their boxes count
    let widgets = list.iter().copied().filter(|&k| matches!(cx.frame.nodes[k].content, Content::Widget(_)));
    match (boxes_area(cx, widgets), sub.extent(cx.tc)) {
        (Some(a), Some(b)) => union(a, b),
        (Some(a), None) | (None, Some(a)) => a,
        (None, None) => rect(0.0, 0.0, 0.0, 0.0),
    }
}

/// The area of the boxes of `list` (through their vector transforms), with
/// their outlines and shadows; `None` when none has an area.
fn boxes_area(cx: &LayerCx, list: impl IntoIterator<Item = usize>) -> Option<Bounds<f32>> {
    let mut area: Option<Bounds<f32>> = None;
    for k in list {
        let n = &cx.frame.nodes[k];
        // a box with no area (a line break, an empty run: placed at 0, 0)
        // would stretch the layer to the page's corner; what it paints is in
        // the painted extent
        if !n.visible || n.bounds.size.width <= 0.0 || n.bounds.size.height <= 0.0 {
            continue;
        }
        let grow = paint_reach(&cx.styles[n.style]);
        let b = n.bounds;
        let grown = rect(b.origin.x - grow, b.origin.y - grow, b.size.width + 2.0 * grow, b.size.height + 2.0 * grow);
        let mapped = map_rect(grown, &cx.mats.m[k]);
        area = Some(area.map_or(mapped, |a| union(a, mapped)));
    }
    area
}

/// The smallest rectangle holding both.
fn union(a: Bounds<f32>, b: Bounds<f32>) -> Bounds<f32> {
    let (x0, y0) = (a.origin.x.min(b.origin.x), a.origin.y.min(b.origin.y));
    let x1 = (a.origin.x + a.size.width).max(b.origin.x + b.size.width);
    let y1 = (a.origin.y + a.size.height).max(b.origin.y + b.size.height);
    rect(x0, y0, x1 - x0, y1 - y0)
}

/// Whether layer `root` (its subtree `list`) is out of view (off the window,
/// or outside its clip), told from its boxes before its content is painted:
/// a long list's blurred rows cost nothing below the fold. Not when a turn
/// or a pixel transform in it may carry what it paints past its boxes.
fn out_of_view(cx: &LayerCx, root: usize, list: &[usize], e: &crate::effects::Effects) -> bool {
    let turned = |k: usize| effects_of(cx.frame, cx.styles, k).is_some_and(|f| f.has_transform());
    if list.iter().any(|&k| turned(k)) {
        return false;
    }
    let Some(boxes) = boxes_area(cx, list.iter().copied()) else { return true };
    // what its filters spread, and those of the layers in it (nested: they add up)
    let g = list.iter().filter(|&&k| k != root).filter_map(|&k| effects_of(cx.frame, cx.styles, k))
        .fold(crate::effects::filter_outset(&e.filter), |g, f| g + crate::effects::filter_outset(&f.filter));
    let area = rect(boxes.origin.x - g, boxes.origin.y - g, boxes.size.width + 2.0 * g, boxes.size.height + 2.0 * g);
    let window = rect(0.0, 0.0, cx.vp.0, cx.vp.1);
    let seen = cx.mats.clip(cx.frame.nodes[root].clip, root).map_or(window, |c| c.intersect(&window));
    let visible = area.intersect(&seen);
    visible.size.width * visible.size.height <= 0.0
}

/// Device-pixel rectangle covering `b`: (x0, y0, w, h).
fn device_rect(b: Bounds<f32>, scale: f32) -> (f32, f32, usize, usize) {
    let x0 = (b.origin.x * scale).floor();
    let y0 = (b.origin.y * scale).floor();
    let x1 = ((b.origin.x + b.size.width) * scale).ceil();
    let y1 = ((b.origin.y + b.size.height) * scale).ceil();
    (x0, y0, (x1 - x0).max(0.0) as usize, (y1 - y0).max(0.0) as usize)
}

/// Paint element `root` and its subtree `list` through its effects.
fn paint_layer(cx: &mut LayerCx, root: usize, list: &[usize], p: &mut Painter, base: f32) {
    let mut inside = Vec::new();
    paint_layer_inner(cx, root, list, p, base, &mut inside);
    // widgets in a layer are kit nodes (no effects): on top of the layer's image
    for (key, _, area) in inside {
        p.note_widget(key, area);
    }
}

fn paint_layer_inner(cx: &mut LayerCx, root: usize, list: &[usize], p: &mut Painter, base: f32,
                     inside: &mut Vec<(Key, u32, Bounds<f32>)>) {
    use crate::effects as fx;
    use std::hash::Hash;
    let n = &cx.frame.nodes[root];
    let c = &cx.styles[n.style];
    let Some(e) = c.effects.as_deref().cloned() else { return };
    // a layer no one can see is not drawn at all (it is made when it comes
    // into view)
    if out_of_view(cx, root, list, &e) {
        return;
    }
    let rel_opacity = (n.opacity / base.max(1e-6)).min(1.0);
    // the content, painted at full opacity relative to the root
    let mut sub = Painter::new();
    sub.scale = cx.scale;
    paint_list(cx, list, &mut sub, Some(root), n.opacity);
    inside.append(&mut sub.widgets);
    // the clip the layer is under is applied when its image is drawn: its
    // pixels don't depend on where it sits under it (a line gliding past the
    // edge of a list is drawn once). Where it draws: through the transforms
    // above, as its items carry it
    let n = &cx.frame.nodes[root];
    let clip = cx.mats.clip(n.clip, root);
    sub.unclip(clip);
    let c = &cx.styles[n.style];
    let screen = rect(-cx.vp.0, -cx.vp.1, cx.vp.0 * 3.0, cx.vp.1 * 3.0);
    // the content's own area, and the layer's: as far as its filters spread
    let inner = layer_area(cx, list, &sub).intersect(&screen);
    let g = fx::filter_outset(&e.filter);
    let area = rect(inner.origin.x - g, inner.origin.y - g, inner.size.width + 2.0 * g, inner.size.height + 2.0 * g)
        .intersect(&screen);
    // a wide blur keeps no detail: the layer works at a fraction of the
    // resolution, and its image is drawn stretched (bilinear, by the renderer)
    let scale = cx.scale / low_res(&e, cx.scale) as f32;
    let (x0, y0, w, h) = device_rect(area, scale);
    let (ix0, iy0, ..) = device_rect(inner, scale);
    // the raster's size from the content's alone, a pixel over its span (it fits at any phase
    // of the device grid): a raster found by its key is the size asked for, and a layer
    // gliding or scrolling a fraction of a pixel a frame reuses it
    // (of its size as the key has it: a moving layer's size wavers in the last bits)
    // (no more than a raster holds: a span of the whole limit is not dropped for its pixel over)
    let span = |v: f32| ((quantize(v) as f32 / 64.0 * scale).ceil() as usize + 1).min(crate::effects::MAX_RASTER);
    let (iw, ih) = (span(inner.size.width), span(inner.size.height));
    if w == 0 || h == 0 || iw == 1 || ih == 1 {
        return;
    }
    // where the raster sits in the layer (both on the same device grid), which holds it whole
    // (no more than a raster holds either: past the limit the layer is cut, its raster clipped to it)
    let at_inner = ((ix0 - x0) as usize, (iy0 - y0) as usize);
    let side = |s: usize, at: usize, i: usize| s.max(at + i).min(crate::effects::MAX_RASTER);
    let (w, h) = (side(w, at_inner.0, iw), side(h, at_inner.1, ih));
    let b = n.bounds;
    let u = c.units(cx.vp);
    // the root's own transform (a layer's: more than a move and a scale), about
    // its origin as its ancestors' vector transforms placed the content
    let outside = cx.mats.m[root];
    let m = match fx::invert(&outside) {
        Some(back) if outside != fx::IDENTITY => fx::mul(&fx::mul(&outside, &e.matrix(b, &u)), &back),
        _ => e.matrix(b, &u),
    };
    let transformed = m != fx::IDENTITY;
    let m_dev = [m[0], m[1], m[2], m[3], m[4] * scale, m[5] * scale];
    // every stage depends on the content relative to its area only: a layer
    // that moved is the same layer, drawn elsewhere; the raster depends on
    // the content alone, not on how far the filters spread
    let (ax, ay) = (area.origin.x, area.origin.y);
    let q = quantize;
    let content = {
        let mut hh = key_hasher();
        sub.content_hash(&mut hh, inner, inner.origin.x, inner.origin.y);
        // its size and resolution make the raster's size (whatever the grid phase)
        (scale.to_bits(), q(inner.size.width), q(inner.size.height)).hash(&mut hh);
        hh.finish()
    };
    let filtered = {
        let mut hh = key_hasher();
        content.hash(&mut hh);
        hash_effects(&mut hh, &e, scale, &u);
        (q(b.origin.x - ax), q(b.origin.y - ay), q(b.size.width), q(b.size.height)).hash(&mut hh);
        if transformed {
            // a turned layer is turned from the device grid it sits on: where its
            // content falls between that grid's pixels (to a quarter) is its own
            let frac = |v: f32, at: f32| ((v * scale - at) * 4.0).round() as i32;
            (frac(inner.origin.x, ix0), frac(inner.origin.y, iy0)).hash(&mut hh);
        }
        hh.finish()
    };
    // a transformed image is placed where the transform puts it (absolute)
    let (ox, oy) = if transformed { (0.0, 0.0) } else { (ax, ay) };
    let placed = {
        let mut hh = key_hasher();
        filtered.hash(&mut hh);
        if transformed {
            (x0.to_bits(), y0.to_bits(), m.map(f32::to_bits)).hash(&mut hh);
        }
        hh.finish()
    };
    // a blend mode mixes with what is under it, which changes: never cached
    let cached = e.blend == fx::Blend::Normal;
    let emit = |p: &mut Painter, img: &Arc<Image>, at: Bounds<f32>| {
        // the layer's own opacity is the image's, not its pixels': a fade
        // draws the same layer (a blend has it in its pixels already)
        p.opacity = if cached { rel_opacity } else { 1.0 };
        p.clip = clip;
        p.image(rect(at.origin.x + ox, at.origin.y + oy, at.size.width, at.size.height), img);
    };
    if cached && let Some((img, at)) = LAYERS.with(|l| l.borrow_mut().images.get(placed)) {
        return emit(p, &img, at);
    }
    let from_cache = |pick: fn(&mut Caches) -> &mut Cache<Arc<fx::Argb>>, k: u64| {
        if cached { LAYERS.with(|l| pick(&mut l.borrow_mut()).get(k)) } else { None }
    };
    let keep = |pick: fn(&mut Caches) -> &mut Cache<Arc<fx::Argb>>, k: u64, v: &Arc<fx::Argb>| {
        if cached {
            LAYERS.with(|l| pick(&mut l.borrow_mut()).put(k, v.clone()));
        }
    };
    let px = match from_cache(|l| &mut l.filtered, filtered) {
        Some(f) => f,
        None => {
            let raster = match from_cache(|l| &mut l.rasters, content) {
                Some(r) => r,
                None => {
                    let Some(r) = sub.rasterize_argb(scale, ix0, iy0, iw, ih).map(Arc::new) else { return };
                    fx::count_layer();
                    keep(|l| &mut l.rasters, content, &r);
                    r
                }
            };
            // the filters spread into a transparent margin around the content
            let padded = raster.padded(w, h, at_inner);
            let filters = drawn_filters(&e.filter, scale);
            let f = Arc::new(layer_effects(padded, &filters, &e, c, b, &u, (scale, x0, y0)));
            keep(|l| &mut l.filtered, filtered, &f);
            f
        }
    };
    // a GPU window turns it on the GPU: the cached layer as a texture, drawn
    // through the matrix (nothing turned on the CPU, nothing uploaded again)
    if transformed && cached && p.is_gpu()
        && gpu_turn(p, &px, filtered, (x0, y0), &m_dev, (scale, cx.scale), clip, rel_opacity)
    {
        return;
    }
    let (mut out, (mut tx, mut ty)) = (None, (x0, y0));
    if transformed {
        // a degenerate matrix draws nothing
        let Some((t, at)) = fx::transform(&fx::Pixels::from_argb(&px), (x0, y0), &m_dev) else { return };
        out = Some(t.to_argb());
        (tx, ty) = at;
    }
    if !cached {
        // a blend mixes the faded layer with what is under it
        let mut faded = out.clone().unwrap_or_else(|| (*px).clone());
        faded.fade(rel_opacity);
        let mine = fx::Pixels::from_argb(&faded);
        if let Some(back) = p.rasterize(scale, tx, ty, mine.w, mine.h) {
            out = Some(fx::composite(&back, &mine, e.blend).to_argb());
        }
    }
    let shown = out.as_ref().unwrap_or(&px);
    let Some(img) = Image::from_argb(shown).map(Arc::new) else { return };
    let at = rect(tx / scale - ox, ty / scale - oy, shown.w as f32 / scale, shown.h as f32 / scale);
    if cached {
        LAYERS.with(|l| l.borrow_mut().images.put(placed, (img.clone(), at)));
    }
    emit(p, &img, at);
}

/// How many times coarser than the screen a layer can be drawn: its widest
/// blur hides detail finer than a third of its radius in device pixels (1:
/// full resolution). Not with a clip path, a mask or a blend mode, whose
/// edges or backdrop must stay sharp.
fn low_res(e: &crate::effects::Effects, scale: f32) -> usize {
    use crate::effects::{Blend, Filter, DOWNSAMPLE_FROM, SMALL_SIGMA};
    if e.clip_path.is_some() || e.mask.as_ref().is_some_and(|m| m.image.is_some()) || e.blend != Blend::Normal {
        return 1;
    }
    let sigma = e.filter.iter().filter_map(|f| match f {
        Filter::Blur(r) => Some(r * scale),
        _ => None,
    }).fold(0.0, f32::max);
    if sigma < DOWNSAMPLE_FROM { 1 } else { (sigma / SMALL_SIGMA).floor().max(1.0) as usize }
}

/// The filters as drawn at `scale`: blur radii to a quarter of a device
/// pixel (a difference no one sees), so a radius that moves gives the same
/// filtered layer for several frames.
fn drawn_filters(filters: &[crate::effects::Filter], scale: f32) -> Vec<crate::effects::Filter> {
    use crate::effects::Filter;
    filters.iter().map(|f| match *f {
        Filter::Blur(r) => Filter::Blur((r * scale * 4.0).round() / (4.0 * scale)),
        ref other => other.clone(),
    }).collect()
}

/// Feeds formatted text into a hasher: a key from `Debug` with no string made.
struct HashWrite<'h, H: Hasher>(&'h mut H);

impl<H: Hasher> std::fmt::Write for HashWrite<'_, H> {
    fn write_str(&mut self, s: &str) -> std::fmt::Result {
        self.0.write(s.as_bytes());
        Ok(())
    }
}

/// What of `e` its filtered layer depends on: the filters as drawn (see
/// [`drawn_filters`]), the clip path and the mask, and the units those
/// resolve with (`circle(1em)` is another circle at another font size).
/// Allocates nothing.
fn hash_effects<H: Hasher>(hh: &mut H, e: &crate::effects::Effects, scale: f32, u: &Units) {
    use crate::effects::Filter;
    use std::hash::Hash;
    use std::fmt::Write;
    let mut w = HashWrite(hh);
    for f in &e.filter {
        // writing to a hasher can't fail
        let _ = match *f {
            Filter::Blur(r) => write!(w, "blur{};", (r * scale * 4.0).round() as i32),
            ref other => write!(w, "{other:?};"),
        };
    }
    let _ = write!(w, "{:?}{:?}", e.clip_path, e.mask);
    if e.clip_path.is_some() || e.mask.is_some() {
        (u.font_size.to_bits(), u.viewport.0.to_bits(), u.viewport.1.to_bits()).hash(hh);
    }
}

/// A layer's raster (with its margin) through `filters` (see
/// [`drawn_filters`]), its clip path and mask, at `(scale, x0, y0)` (device
/// pixels per logical one, and its top left). Its opacity is not in the
/// pixels: the image is drawn at it. Blurs alone stay in ARGB, in integers;
/// anything else works on floats.
fn layer_effects(raster: crate::effects::Argb, filters: &[crate::effects::Filter], e: &crate::effects::Effects,
                 c: &Computed, b: Bounds<f32>, u: &Units, (scale, x0, y0): (f32, f32, f32))
                 -> crate::effects::Argb {
    use crate::effects as fx;
    let blurs_only = filters.iter().all(|f| matches!(f, fx::Filter::Blur(_))) && e.clip_path.is_none()
        && e.mask.as_ref().is_none_or(|m| m.image.is_none());
    if blurs_only {
        let mut a = raster;
        for f in filters {
            if let fx::Filter::Blur(r) = f {
                fx::blur_argb(&mut a, r * scale);
            }
        }
        return a;
    }
    let (w, h) = (raster.w, raster.h);
    let mut px = fx::Pixels::from_argb(&raster);
    for f in filters {
        fx::apply_filter(&mut px, f, scale);
    }
    if let Some((shape, edge)) = &e.clip_path {
        let mut mp = Painter::new();
        clip_shape(&mut mp, shape, *edge, c, b, u);
        if let Some(mask) = mp.rasterize(scale, x0, y0, w, h) {
            fx::apply_mask(&mut px, &mask, false);
        }
    }
    if let Some(mask) = e.mask.as_ref().filter(|m| m.image.is_some()) {
        let mut mp = Painter::new();
        paint_source(&mut mp, mask.image.as_ref().unwrap(), b, Some(mask), u.viewport);
        if let Some(mk) = mp.rasterize(scale, x0, y0, w, h) {
            fx::apply_mask(&mut px, &mk, mask.luminance);
        }
    }
    px.to_argb()
}

/// A text item's line, as a share of its size (ascent and descent).
const TEXT_LINE: f32 = 1.25;
/// Room left around measured text, as a share of its size: an italic's
/// overhang, a synthetic bold, glyphs past their advance.
const TEXT_INK_PAD: f32 = 0.25;

/// A logical-pixel value to 1/64 px, as bits: positions relative to a
/// layer that moved round off the same.
fn quantize(v: f32) -> i32 {
    (v * 64.0).round() as i32
}

/// `backdrop-filter`: what's already painted under the element's border
/// box, filtered and clipped to its rounded box.
fn paint_backdrop(cx: &mut LayerCx, i: usize, p: &mut Painter) {
    use crate::effects as fx;
    let n = &cx.frame.nodes[i];
    let c = &cx.styles[n.style];
    let Some(e) = c.effects.as_deref() else { return };
    let b = n.bounds;
    let g = fx::filter_outset(&e.backdrop);
    let grown = rect(b.origin.x - g, b.origin.y - g, b.size.width + 2.0 * g, b.size.height + 2.0 * g);
    let (x0, y0, w, h) = device_rect(grown, cx.scale);
    let Some(mut px) = p.rasterize(cx.scale, x0, y0, w, h) else { return };
    for f in &e.backdrop {
        fx::apply_filter(&mut px, f, cx.scale);
    }
    let mut mp = Painter::new();
    mp.quad(b, radius_px(c, b, cx.vp), Rgba { r: 1.0, g: 1.0, b: 1.0, a: 1.0 }, Rgba::TRANSPARENT, 0.0);
    if let Some(mask) = mp.rasterize(cx.scale, x0, y0, w, h) {
        fx::apply_mask(&mut px, &mask, false);
    }
    let Some(img) = Image::from_argb(&px.to_argb()).map(Arc::new) else { return };
    p.opacity = 1.0;
    p.clip = n.clip;
    p.image(rect(x0 / cx.scale, y0 / cx.scale, w as f32 / cx.scale, h as f32 / cx.scale), &img);
}

/// The coverage of a `clip-path` shape, painted white.
fn clip_shape(p: &mut Painter, shape: &crate::effects::ClipShape, edge: crate::style::BoxEdge, c: &Computed,
              b: Bounds<f32>, u: &Units) {
    use crate::effects::{ClipShape as S, ShapeRadius as R};
    let white = Rgba { r: 1.0, g: 1.0, b: 1.0, a: 1.0 };
    let bw = border_widths(c, b.size.width);
    let rb = match edge {
        crate::style::BoxEdge::Border => b,
        crate::style::BoxEdge::Padding => rect(b.origin.x + bw[3], b.origin.y + bw[0],
                                                (b.size.width - bw[1] - bw[3]).max(0.0),
                                                (b.size.height - bw[0] - bw[2]).max(0.0)),
        crate::style::BoxEdge::Content => content_box(c, b),
    };
    let (x, y, w, h) = (rb.origin.x, rb.origin.y, rb.size.width, rb.size.height);
    let radius = |r: &R, cx: f32, cy: f32, horizontal: Option<bool>| -> f32 {
        let (dx, dy) = (cx.min(w - cx), cy.min(h - cy));
        let (fx_, fy) = (cx.max(w - cx), cy.max(h - cy));
        match (r, horizontal) {
            (R::Len(l), Some(true)) => l.resolve(u, w),
            (R::Len(l), Some(false)) => l.resolve(u, h),
            // circle: percentages of the box's normalised diagonal
            (R::Len(l), None) => l.resolve(u, (w * w + h * h).sqrt() / std::f32::consts::SQRT_2),
            (R::Closest, Some(true)) => dx,
            (R::Closest, Some(false)) => dy,
            (R::Closest, None) => dx.min(dy),
            (R::Farthest, Some(true)) => fx_,
            (R::Farthest, Some(false)) => fy,
            (R::Farthest, None) => fx_.max(fy),
        }
    };
    match shape {
        S::Box => p.fill(rb, white),
        S::Inset(s, r) => {
            let (t, rr, bb, l) = (s[0].resolve(u, h), s[1].resolve(u, w), s[2].resolve(u, h), s[3].resolve(u, w));
            let ib = rect(x + l, y + t, (w - l - rr).max(0.0), (h - t - bb).max(0.0));
            p.quad(ib, r.resolve(u, ib.size.width.min(ib.size.height)), white, Rgba::TRANSPARENT, 0.0);
        }
        S::Circle(r, at) => {
            let (cx_, cy) = (at[0].at(w), at[1].at(h));
            let rad = radius(r, cx_, cy, None);
            p.oval(rect(x + cx_ - rad, y + cy - rad, 2.0 * rad, 2.0 * rad), white, Rgba::TRANSPARENT, 0.0);
        }
        S::Ellipse(rx, ry, at) => {
            let (cx_, cy) = (at[0].at(w), at[1].at(h));
            let (a, bb) = (radius(rx, cx_, cy, Some(true)), radius(ry, cx_, cy, Some(false)));
            p.oval(rect(x + cx_ - a, y + cy - bb, 2.0 * a, 2.0 * bb), white, Rgba::TRANSPARENT, 0.0);
        }
        S::Polygon(pts) => {
            let pts: Vec<(f32, f32)> = pts.iter().map(|(px_, py)| (x + px_.resolve(u, w), y + py.resolve(u, h))).collect();
            p.polygon(&pts, white);
        }
    }
}

/// A mask or border-image source painted over `area` (sized, positioned and
/// tiled like a background when `mask` gives the placement).
fn paint_source(p: &mut Painter, src: &crate::effects::ImageSource, area: Bounds<f32>,
                mask: Option<&crate::effects::Mask>, _vp: (f32, f32)) {
    match src {
        crate::effects::ImageSource::Gradient(g) => p.with_clip(area, |p| paint_gradient(p, g, area)),
        crate::effects::ImageSource::Url(u) => {
            let mut st = crate::style::ImageStyle { bg_url: Some(u.clone()), ..Default::default() };
            if let Some(m) = mask {
                st.bg_size = m.size;
                st.bg_position = m.position;
                st.bg_repeat = m.repeat;
            }
            paint_background_image(p, &st, area, area);
        }
    }
}

/// A background that repaints exactly the pixels already under it: an opaque
/// plain fill with the colour and bounds of the nearest painted ancestor, with
/// only undecorated same-sized boxes in between (so nothing else can show).
/// The canvas's background (CSS Backgrounds 2.11.2): the root element's colour, or `<body>`'s
/// when the root has none, or white; and the box it came from (which doesn't paint it again).
pub(crate) fn canvas_background(frame: &Frame, styles: &StyleStore) -> (Option<usize>, Rgba) {
    let Some(root) = frame.nodes.first() else { return (None, Rgba::WHITE) };
    let own = |c: &Computed| !c.background.is_transparent();
    let plain = |c: &Computed| c.gradient.is_none() && c.images.as_ref().is_none_or(|i| i.bg_url.is_none());
    let rc = &styles[root.style];
    if own(rc) {
        return (Some(0), rc.background);
    }
    if plain(rc) {
        let body = (1..frame.nodes.len()).find(|&i| frame.nodes[i].parent == Some(0) && frame.el(i).is_some_and(|e| e.tag == "body"));
        // (a body without a box, `display: contents`, has nothing to give)
        if let Some(b) = body.filter(|&b| frame.nodes[b].taffy.is_some() && own(&styles[frame.nodes[b].style])) {
            return (Some(b), styles[frame.nodes[b].style].background);
        }
    }
    (None, Rgba::WHITE)
}

fn redundant_background(frame: &Frame, styles: &StyleStore, n: &BoxNode) -> bool {
    let c = &styles[n.style];
    if c.background.a < 1.0 || c.gradient.is_some() || !c.shadows.is_empty()
        || c.images.as_ref().is_some_and(|i| i.bg_url.is_some()) {
        return false;
    }
    let mut cur = n.parent;
    while let Some(i) = cur {
        let a = &frame.nodes[i];
        let ac = &styles[a.style];
        if a.bounds != n.bounds || a.taffy.is_none() || !ac.shadows.is_empty() || ac.gradient.is_some()
            || a.opacity < 1.0 || a.clip.is_some() != n.clip.is_some()
        {
            return false;
        }
        if !ac.background.is_transparent() {
            return ac.background == c.background && n.opacity >= 1.0;
        }
        if border_widths(ac, a.bounds.size.width).iter().any(|w| *w > 0.0) {
            return false;
        }
        cur = a.parent;
    }
    false
}

/// The inline boxes around line item `i` (a word or an inline box's edge), outermost first: each
/// paints the piece of its fragment the item covers, its background and its top and bottom borders
/// round its content area (with its padding), its start and end borders where its edges are (CSS
/// Inline 3: `box-decoration-break: slice`).
fn paint_inline_boxes(frame: &Frame, styles: &StyleStore, i: usize, p: &mut Painter) {
    let n = &frame.nodes[i];
    let b = n.bounds;
    let chain = inline_boxes(frame, styles, i);
    // the item's baseline: where its own style's text sits in its box
    let c = &styles[n.style];
    let base = text::baseline(b.origin.y, b.size.height, c.font_size, c.font_id);
    for &k in chain.iter().rev() {
        let s = &styles[frame.nodes[k].style];
        let w = b.size.width;
        let (top, bottom) = text::content_box(base, s.font_size, s.font_id);
        let bw = border_widths(s, w);
        let pad = |v: taffy::style::LengthPercentage| lp(v, 0.0);
        let (top, bottom) = (top - pad(s.layout.padding.top) - bw[0], bottom + pad(s.layout.padding.bottom) + bw[2]);
        // its own edge: the part past its margin
        let margin = |v: taffy::style::LengthPercentageAuto| taffy::ResolveOrZero::resolve_or_zero(v, Some(0.0), crate::tree::resolve_calc);
        let (mut x0, mut x1) = (b.origin.x, b.origin.x + w);
        let edge = match n.content {
            Content::Edge(start) if n.owner == k => Some(start),
            _ => None,
        };
        match edge {
            Some(true) => x0 += margin(s.layout.margin.left),
            Some(false) => x1 -= margin(s.layout.margin.right),
            None => {}
        }
        let r = rect(x0, top, (x1 - x0).max(0.0), (bottom - top).max(0.0));
        if !s.background.is_transparent() {
            p.fill(r, s.background);
        }
        let side = |p: &mut Painter, r: Bounds<f32>, c: Rgba| if r.size.width > 0.0 && r.size.height > 0.0 && c.a > 0.0 { p.fill(r, c) };
        side(p, rect(x0, top, r.size.width, bw[0]), s.border_color[0]);
        side(p, rect(x0, bottom - bw[2], r.size.width, bw[2]), s.border_color[2]);
        match edge {
            Some(true) => side(p, rect(x0, top, bw[3], r.size.height), s.border_color[3]),
            Some(false) => side(p, rect(x1 - bw[1], top, bw[1], r.size.height), s.border_color[1]),
            None => {}
        }
    }
}

/// Word box `i`'s text node, and where its word starts and ends in it.
fn word_span(frame: &Frame, i: usize) -> Option<(usize, usize, usize)> {
    match frame.nodes.get(i)?.content {
        Content::Word { src, start, end, .. } => Some((src as usize, start as usize, end as usize)),
        _ => None,
    }
}

/// The text between word box `i - 1`'s word and word box `i`'s: of one text node, or the end of
/// one and the start of the next when that is white space only.
fn between_words(frame: &Frame, i: usize) -> Option<&str> {
    let (src, start, _) = word_span(frame, i)?;
    let (psrc, _, pend) = word_span(frame, i.checked_sub(1)?)?;
    let text = |s: usize| match &frame.nodes[s].content {
        Content::Text(t) => Some(t.as_ref()),
        _ => None,
    };
    if psrc == src {
        return text(src)?.get(pend..start);
    }
    let (tail, head) = (text(psrc)?.get(pend..)?, text(src)?.get(..start)?);
    let blank = |s: &str| s.chars().all(|c| c == ' ');
    // (one gap only: the space at the end of the first, or the start of the second)
    (blank(tail) && blank(head)).then_some(if tail.is_empty() { head } else { tail })
}

/// When word box `i` continues the run of the word box before it, the text between them: text that
/// looks the same on the same line, where that text puts it (no justification, spacing, tab or
/// soft hyphen between them; a gap of discarded white space, `white-space-trim`, is none).
fn run_gap<'f>(frame: &'f Frame, styles: &StyleStore, tc: &mut TextCache, i: usize) -> Option<&'f str> {
    let k = i.checked_sub(1)?;
    word_span(frame, k)?;
    let between = between_words(frame, i)?;
    let (n, m) = (&frame.nodes[i], &frame.nodes[k]);
    let (c, d) = (&styles[n.style], &styles[m.style]);
    let looks = n.style == m.style
        || (c.font_size == d.font_size && text::flags(c) == text::flags(d) && c.color == d.color
            && c.underline == d.underline && c.line_through == d.line_through && c.overline == d.overline
            && c.text_shadow.is_empty() && d.text_shadow.is_empty() && c.opacity == d.opacity);
    // (letter spacing paints per character, the same in a run: alike on both sides it joins)
    if !looks || (n.bounds.origin.y - m.bounds.origin.y).abs() > 0.01 || c.letter_spacing != d.letter_spacing
        || c.word_spacing != 0.0 || d.word_spacing != 0.0 {
        return None;
    }
    // a word that paints a fragment of its inline box (a highlight) paints it after the run's first
    // word drew the run: over the run's text, so it starts a run of its own
    if inline_boxes(frame, styles, i).iter().any(|&b| paints_fragment(&styles[frame.nodes[b].style])) {
        return None;
    }
    let (prev, word) = (frame.text(k), frame.text(i));
    if [prev, between, word].iter().any(|s| s.contains(['\t', crate::linebreak::SOFT_HYPHEN])) {
        return None;
    }
    let fits = |tc: &mut TextCache, gap: &str| {
        let joined = format!("{prev}{gap}");
        let w = tc.width(&joined, c.font_size, text::flags(c)) + c.letter_spacing * joined.chars().count() as f32;
        (m.bounds.origin.x + w - n.bounds.origin.x).abs() < 0.5
    };
    if fits(tc, between) {
        Some(between)
    } else if !between.is_empty() && between.chars().all(|ch| ch == ' ') && fits(tc, "") {
        Some("")
    } else {
        None
    }
}

/// The inline boxes line item `i` is in, innermost first (up to the first that isn't one).
fn inline_boxes(frame: &Frame, styles: &StyleStore, i: usize) -> smallvec::SmallVec<[usize; 4]> {
    let mut chain = smallvec::SmallVec::new();
    let mut o = Some(frame.nodes[i].owner);
    while let Some(k) = o.filter(|&k| k < frame.nodes.len()) {
        let m = &frame.nodes[k];
        if m.taffy.is_some() || !matches!(m.content, Content::Element) || styles[m.style].display != DisplayKind::Inline {
            break;
        }
        chain.push(k);
        o = m.parent;
    }
    chain
}

/// Whether an inline box paints a fragment under its words: a background or an image, borders, a
/// shadow or an outline (see [`paint_inline_boxes`]).
fn paints_fragment(c: &Computed) -> bool {
    !c.background.is_transparent() || c.gradient.is_some() || c.images.is_some() || !c.shadows.is_empty()
        || c.outline.0 > 0.0
        || (0..4).any(|k| !c.border_color[k].is_transparent() && border_widths(c, 0.0)[k] > 0.0)
}

/// The last word box of the run word box `i` starts (`i` itself when none continues it).
fn run_after(frame: &Frame, styles: &StyleStore, tc: &mut TextCache, i: usize) -> usize {
    let mut j = i;
    while j + 1 < frame.nodes.len() && run_gap(frame, styles, tc, j + 1).is_some() {
        j += 1;
    }
    j
}

fn paint_node(frame: &Frame, styles: &StyleStore, tc: &mut TextCache, i: usize, p: &mut Painter,
              vp: (f32, f32), app: &crate::window::App) {
    let n = &frame.nodes[i];
    // under a box whose `overflow` rounds its corners: what this paints there is cut to them after
    let rounded = (n.round != crate::layout::NONE).then_some((n.round as usize, p.items.len()));
    paint_node_items(frame, styles, tc, i, p, vp, app);
    if let Some((round, from)) = rounded {
        clip_to_rounded(p, from, frame, styles, round, vp);
    }
}

/// What box `i` paints (see [`paint_node`]).
fn paint_node_items(frame: &Frame, styles: &StyleStore, tc: &mut TextCache, i: usize, p: &mut Painter,
                    vp: (f32, f32), app: &crate::window::App) {
    let n = &frame.nodes[i];
    let c = &styles[n.style];
    p.through = !c.pointer_events;
    p.snap = true; // a CSS box (a canvas's own drawing turns it off)
    let b = n.bounds;
    match &n.content {
        Content::Edge(_) => paint_inline_boxes(frame, styles, i, p),
        Content::Word { .. } => {
            // the inline boxes it is in paint their fragments of this line under it
            paint_inline_boxes(frame, styles, i, p);
            let text = frame.text(i);
            if let Some(sel) = p.sel {
                let line = RunLine { text, x: b.origin.x, y: b.origin.y, lh: b.size.height, w: b.size.width, at: 0 };
                paint_selection(p, tc, c, &sel, n.seq, text.len(), &line, true);
            }
            // Han in a Japanese or Korean paragraph keeps its faces
            let hint = match n.content {
                Content::Word { lang: 1, .. } => sys::BUI_FONT_HAN_JA,
                Content::Word { lang: 2, .. } => sys::BUI_FONT_HAN_KO,
                _ => 0,
            };
            // words of one text at their natural spacing on a line draw as one run, as a browser
            // draws a text run (its glyphs placed by the run, not word by word): the first paints
            // them all
            let run = run_after(frame, styles, tc, i);
            if run_gap(frame, styles, tc, i).is_some() {
                // drawn by the run's first word
            } else if run > i {
                let mut whole = String::from(frame.text(i));
                for j in i + 1..=run {
                    whole.push_str(run_gap(frame, styles, tc, j).unwrap_or(""));
                    whole.push_str(frame.text(j));
                }
                paint_text_line_as(p, tc, c, &whole, (b.origin.x, b.origin.y, b.size.height), None, hint);
            } else if text == "\t" {
                // a soft hyphen draws nothing, but a hyphen where a line ends at it (its box grew to hold one)
                // a tab: its advance is its box
            } else if text.contains(crate::linebreak::SOFT_HYPHEN) {
                let mut shown: String = text.chars().filter(|&ch| ch != crate::linebreak::SOFT_HYPHEN).collect();
                let w = tc.width(&shown, c.font_size, text::flags(c)) + c.letter_spacing * shown.chars().count() as f32;
                if text.ends_with(crate::linebreak::SOFT_HYPHEN) && b.size.width > w + 0.5 {
                    shown.push('\u{2010}');
                }
                paint_text_line_as(p, tc, c, &shown, (b.origin.x, b.origin.y, b.size.height), None, hint);
            } else {
                paint_text_line_as(p, tc, c, text, (b.origin.x, b.origin.y, b.size.height), None, hint);
            }
        }
        // a text run as a flex/grid item (in line boxes its words paint instead)
        Content::Text(t) if n.taffy.is_some() || n.folded => {
            each_run_line(tc, c, t, b, p.sel.is_some(), |tc, line| {
                if let Some(sel) = p.sel {
                    paint_selection(p, tc, c, &sel, n.seq, t.len(), line, false);
                }
                paint_text_line(p, tc, c, line.text, line.x, line.y, line.lh, Some(line.w));
            });
        }
        Content::Element | Content::Replaced(_) if n.taffy.is_some() => {
            let hidden_cell = c.empty_cells_hide && c.display == DisplayKind::TableCell
                && !c.border_collapse && frame.is_empty(i);
            if !hidden_cell {
                let canvas = (i == 0 || n.parent == Some(0)) && canvas_background(frame, styles).0 == Some(i);
                paint_box(n, c, p, vp, canvas || redundant_background(frame, styles, n));
            }
            let marker_img = n.marker.as_ref().and(c.list_image.as_deref()).and_then(background_file);
            if let Some(img) = marker_img.as_ref() {
                // `list-style-image`: the image, at most a line tall, left of the box
                let bw = border_widths(c, b.size.width);
                let pad = lp(c.layout.padding.top, b.size.width);
                let lh = c.line_px();
                let (iw, ih) = (img.width as f32, img.height as f32);
                let s = (lh / ih.max(1.0)).min(1.0);
                let (w, h) = (iw * s, ih * s);
                p.image_file(rect(b.origin.x - w - 6.0, b.origin.y + bw[0] + pad + (lh - h) / 2.0, w, h), &img.src);
            } else if let Some(m) = n.marker.as_ref().filter(|m| !m.is_empty()) {
                let bw = border_widths(c, b.size.width);
                let pad = lp(c.layout.padding.top, b.size.width);
                let flags = text::flags(c);
                let mw = tc.width(m, c.font_size, flags);
                paint_text_line(p, tc, c, m, b.origin.x - mw - 6.0, b.origin.y + bw[0] + pad, c.line_px(),
                                Some(mw));
            }
            if let Content::Replaced(r) = &n.content {
                let inner = content_box(c, b);
                match r {
                    Replaced::Image(img) => match &c.images {
                        Some(st) if st.object_fit != crate::style::ObjectFit::Fill
                            || st.object_position != [crate::style::Pos::CENTER; 2] => {
                            let (x, y, w, h) = st.object_rect(
                                (inner.origin.x, inner.origin.y, inner.size.width, inner.size.height),
                                (img.width as f32, img.height as f32));
                            p.with_clip(inner, |p| p.image(rect(x, y, w, h), img));
                        }
                        _ => p.image(inner, img),
                    },
                    Replaced::File(file, frame) => match &c.images {
                        Some(st) if st.object_fit != crate::style::ObjectFit::Fill
                            || st.object_position != [crate::style::Pos::CENTER; 2] => {
                            let (x, y, w, h) = st.object_rect(
                                (inner.origin.x, inner.origin.y, inner.size.width, inner.size.height),
                                (file.width as f32, file.height as f32));
                            p.with_clip(inner, |p| p.image_file_frame(rect(x, y, w, h), &file.src, *frame));
                        }
                        _ => p.image_file_frame(inner, &file.src, *frame),
                    },
                    Replaced::Video(v) => p.with_clip(inner, |p| p.video(inner, v, c.images.as_ref().map_or(
                        sys::BK_FIT_CONTAIN, |st| st.object_fit.kit_video()))),
                    Replaced::Painted(f) => {
                        p.snap = false; // the canvas draws as it says, anti-aliased (as a browser canvas)
                        p.with_clip(inner, |p| f(inner, p, app));
                    }
                    // a canvas nothing draws on is transparent, as is an <svg> (its shapes aren't drawn)
                    Replaced::Placeholder("canvas" | "svg") => {}
                    Replaced::Placeholder(tag) => {
                        p.fill(inner, crate::color::rgb(0x282828));
                        let label = frame.attr(i, "alt").unwrap_or(tag);
                        let w = tc.width(label, c.font_size, 0);
                        let base = text::baseline(inner.origin.y, inner.size.height, c.font_size, 0);
                        p.styled_text(inner.origin.x + (inner.size.width - w) / 2.0,
                                      text::item_top(base, c.font_size),
                                      label, c.font_size, 0, crate::color::rgb(0x8a8a8a));
                    }
                }
            }
            // `scrollbar-width: none`: it scrolls, with no bar
            if c.layout.scrollbar_width > 0.0 {
                paint_scrollbars(n, p);
            }
        }
        _ => {}
    }
}

/// One line of a text run (a flex / grid item, or text folded into its
/// box) as it is drawn: what paint draws, and what text selection measures.
pub(crate) struct RunLine<'a> {
    pub text: &'a str,
    pub x: f32,
    pub y: f32,
    pub lh: f32,
    pub w: f32,
    /// Byte offset of `text` in the run (only when asked for: 0 otherwise).
    pub at: usize,
}

/// Call `f` with each line of text run `t` laid out in `b`: wrapped, cut with
/// an ellipsis, aligned. `offsets`: find where each line starts in `t`.
pub(crate) fn each_run_line(tc: &mut TextCache, c: &Computed, t: &str, b: Bounds<f32>, offsets: bool,
                            mut f: impl FnMut(&mut TextCache, &RunLine)) {
    let flags = text::flags(c);
    let ellipsis = c.ellipsizes();
    let align = |lw: f32| match c.text_align.physical(c.layout.direction == taffy::style::Direction::Rtl) {
        TextAlign::Center => b.origin.x + (b.size.width - lw) / 2.0,
        TextAlign::Right => b.origin.x + b.size.width - lw,
        _ => b.origin.x,
    };
    if !ellipsis && !t.contains([' ', '\n']) && text::break_mode(c) == 0 {
        // one word: one line (the common label case)
        let lw = tc.width(t, c.font_size, flags);
        f(tc, &RunLine { text: t, x: align(lw), y: b.origin.y, lh: c.line_px(), w: lw, at: 0 });
        return;
    }
    let nowrap = matches!(c.white_space, WhiteSpace::NoWrap | WhiteSpace::Pre);
    let (wrapped, wrapped_run);
    let lines: &[String] = if nowrap {
        wrapped = [t.to_owned()];
        &wrapped
    } else {
        wrapped_run = tc.wrap(t, c.font_size, flags, b.size.width + 0.5, text::break_mode(c));
        &wrapped_run.lines
    };
    let lh = c.line_px();
    let mut from = 0;
    for (k, line) in lines.iter().enumerate() {
        let line: std::borrow::Cow<str> = if ellipsis {
            tc.ellipsize(line, c.font_size, flags, b.size.width + 0.5).into()
        } else {
            line.as_str().into()
        };
        let lw = tc.width(&line, c.font_size, flags);
        let mut at = 0;
        if offsets {
            // lines are pieces of the run, whitespace between them dropped
            let shown = line.strip_suffix('\u{2026}').filter(|_| ellipsis).unwrap_or(&line);
            at = from + (t[from..].len() - t[from..].trim_start().len());
            if !t[at..].starts_with(shown) {
                at = t[from..].find(shown).map_or(at, |k| from + k);
            }
            from = (at + shown.len()).min(t.len());
        }
        f(tc, &RunLine { text: &line, x: align(lw), y: b.origin.y + lh * k as f32, lh, w: lw, at });
    }
}

/// The selection highlight behind one line of text: the selected part of
/// the line (`word`: through the space after the word when the selection
/// goes on). While anything is selected every line pushes one rectangle
/// (transparent when not selected), so items keep their canvas slots and a
/// selection change damages only the lines whose state changed.
#[allow(clippy::too_many_arguments)]
fn paint_selection(p: &mut Painter, tc: &mut TextCache, c: &Computed, sel: &crate::selection::SelPaint, seq: u32,
                   len: usize, line: &RunLine, word: bool) {
    let none = rect(line.x, line.y, 0.0, 0.0);
    let Some((s, e, goes_on)) = sel.range(seq, len) else {
        return p.highlight(none, Rgba::TRANSPARENT);
    };
    let (rs, re) = (s.saturating_sub(line.at).min(line.text.len()), e.saturating_sub(line.at).min(line.text.len()));
    if re <= rs {
        return p.highlight(none, Rgba::TRANSPARENT);
    }
    let x0 = line.x + crate::selection::advance(tc, c, line.text, rs);
    let x1 = if word && goes_on && re == line.text.len() { line.x + line.w }
             else { line.x + crate::selection::advance(tc, c, line.text, re) };
    p.highlight(rect(x0, line.y, (x1 - x0).max(0.0), line.lh), sel.color);
}

pub(crate) fn content_box(c: &Computed, b: Bounds<f32>) -> Bounds<f32> {
    let w = b.size.width;
    let bw = border_widths(c, w);
    let pd = c.layout.padding;
    let (pt, pr, pb, pl) = (lp(pd.top, w), lp(pd.right, w), lp(pd.bottom, w), lp(pd.left, w));
    rect(b.origin.x + bw[3] + pl, b.origin.y + bw[0] + pt,
         (w - bw[1] - bw[3] - pl - pr).max(0.0), (b.size.height - bw[0] - bw[2] - pt - pb).max(0.0))
}

/// One line of text in a line box of height `lh`, with decorations.
#[allow(clippy::too_many_arguments)]
fn paint_text_line(p: &mut Painter, tc: &mut TextCache, c: &Computed, s: &str, x: f32, y: f32, lh: f32,
                   width: Option<f32>) {
    paint_text_line_as(p, tc, c, s, (x, y, lh), width, 0);
}

/// [`paint_text_line`] with extra font flags (`text::han_hint`).
fn paint_text_line_as(p: &mut Painter, tc: &mut TextCache, c: &Computed, s: &str, (x, y, lh): (f32, f32, f32),
                      width: Option<f32>, hint: i32) {
    let flags = text::flags(c) | hint;
    let size = c.font_size;
    let base = text::baseline(y, lh, size, c.font_id);
    let s: std::borrow::Cow<str> = if c.small_caps { s.to_uppercase().into() } else { s.into() };
    let size_draw = if c.small_caps { size * 0.8 } else { size };
    let top = text::item_top(base, size_draw);
    // shadows under the text, the first on top: each the run itself, moved (and blurred)
    for sh in c.text_shadow.iter().rev() {
        if sh.blur > 0.0 {
            blurred_run(p, tc, (&s, c.letter_spacing), (x + sh.x, top + sh.y), (size_draw, flags), sh.color, sh.blur / 2.0);
        } else {
            text_run(p, tc, (&s, c.letter_spacing), (x + sh.x, top + sh.y), (size_draw, flags), sh.color);
        }
    }
    text_run(p, tc, (&s, c.letter_spacing), (x, top), (size_draw, flags), c.color);
    if c.underline || c.line_through || c.overline {
        let w = width.unwrap_or_else(|| tc.width(s.trim_end(), size_draw, flags));
        let thick = decoration_thickness(c);
        let dc = c.decoration_color.unwrap_or(c.color);
        if c.underline {
            // `text-underline-offset` is measured from the baseline
            let y = base + c.underline_offset.unwrap_or(size * text::UNDERLINE);
            decoration(p, c.decoration_style, x, y, w, thick, dc);
        }
        if c.line_through {
            decoration(p, c.decoration_style, x, base - size * text::STRIKEOUT - thick / 2.0, w, thick, dc);
        }
        if c.overline {
            decoration(p, c.decoration_style, x, text::ascent_top(base, size, c.font_id), w, thick, dc);
        }
    }
}

/// A line of text `s` at (`x`, `top`): one run, or with `letter-spacing` each character spaced
/// out (the text and each of its shadows place their glyphs alike).
fn text_run(p: &mut Painter, tc: &mut TextCache, (s, spacing): (&str, f32), (x, top): (f32, f32), (size, flags): (f32, i32),
            color: Rgba) {
    if spacing == 0.0 {
        p.styled_text(x, top, s, size, flags, color);
        return;
    }
    let mut cx = x;
    let mut buf = [0u8; 4];
    for ch in s.chars() {
        let t: &str = ch.encode_utf8(&mut buf);
        p.styled_text(cx, top, t, size, flags, color);
        cx += tc.width(t, size, flags) + spacing;
    }
}

/// [`text_run`] blurred by a Gaussian of `sigma` (a `text-shadow`'s blur): rasterized once,
/// blurred, and kept as an image by what it looks like (a line that moves draws the same one).
#[allow(clippy::too_many_arguments)]
fn blurred_run(p: &mut Painter, tc: &mut TextCache, run: (&str, f32), at: (f32, f32), font: (f32, i32), color: Rgba,
               sigma: f32) {
    use std::hash::{Hash, Hasher};
    thread_local! {
        static BLURRED: std::cell::RefCell<HashMap<u64, Arc<Image>>> = std::cell::RefCell::new(HashMap::new());
    }
    let mut sub = Painter::new();
    let scale = if p.scale > 0.0 { p.scale } else { 1.0 };
    sub.scale = scale;
    text_run(&mut sub, tc, run, at, font, color);
    let Some(ink) = sub.extent(tc) else { return };
    // as far as the blur reaches
    let g = 3.0 * sigma;
    let area = rect(ink.origin.x - g, ink.origin.y - g, ink.size.width + 2.0 * g, ink.size.height + 2.0 * g);
    let (x0, y0, w, h) = device_rect(area, scale);
    if w == 0 || h == 0 || w > crate::effects::MAX_RASTER || h > crate::effects::MAX_RASTER {
        return;
    }
    let placed = rect(x0 / scale, y0 / scale, w as f32 / scale, h as f32 / scale);
    let key = {
        let mut hh = key_hasher();
        sub.content_hash(&mut hh, placed, placed.origin.x, placed.origin.y);
        (sigma.to_bits(), scale.to_bits(), w, h).hash(&mut hh);
        hh.finish()
    };
    let img = BLURRED.with(|m| m.borrow().get(&key).cloned()).or_else(|| {
        let mut px = sub.rasterize_argb(scale, x0, y0, w, h)?;
        crate::effects::blur_argb(&mut px, sigma * scale);
        let img = Arc::new(Image::from_argb(&px)?);
        BLURRED.with(|m| {
            let mut m = m.borrow_mut();
            if m.len() > 128 {
                m.clear();
            }
            m.insert(key, img.clone());
        });
        Some(img)
    });
    if let Some(img) = img {
        let snap = std::mem::replace(&mut p.snap, false);
        p.image(placed, &img);
        p.snap = snap;
    }
}

/// The thickness a decoration line draws at: `text-decoration-thickness`, else 1/14 em.
fn decoration_thickness(c: &Computed) -> f32 {
    c.decoration_thickness.unwrap_or(c.font_size / 14.0).max(1.0)
}

/// How far from its `y` a `decoration` line of thickness `t` paints, either way: a double line's
/// second strip ends at 3t, a wave swings 1.5t each way with a t/2 half stroke.
const fn decoration_extent(style: DecoStyle, t: f32) -> f32 {
    match style {
        DecoStyle::Double => 3.0 * t,
        DecoStyle::Wavy => 2.0 * t,
        DecoStyle::Solid | DecoStyle::Dotted | DecoStyle::Dashed => t,
    }
}

/// One text decoration line of `text-decoration-style` from `x` to `x + w`.
fn decoration(p: &mut Painter, style: DecoStyle, x: f32, y: f32, w: f32, t: f32, col: Rgba) {
    match style {
        DecoStyle::Solid => p.fill(rect(x, y, w, t), col),
        DecoStyle::Double => {
            p.fill(rect(x, y, w, t), col);
            p.fill(rect(x, y + t * 2.0, w, t), col);
        }
        DecoStyle::Dotted | DecoStyle::Dashed => {
            let (on, gap) = if style == DecoStyle::Dotted { (t, t) } else { (t * 3.0, t * 2.0) };
            let mut cx = x;
            while cx < x + w {
                p.fill(rect(cx, y, on.min(x + w - cx), t), col);
                cx += on + gap;
            }
        }
        DecoStyle::Wavy => {
            let (amp, period) = (t * 1.5, t * 6.0);
            let steps = ((w / (period / 8.0)).ceil() as usize).clamp(2, 4096);
            let pts: Vec<(f32, f32)> = (0..=steps).map(|k| {
                let px = x + w * k as f32 / steps as f32;
                (px, y + amp * (std::f32::consts::TAU * (px - x) / period).sin())
            }).collect();
            p.line(&pts, col, t);
        }
    }
}

/// Shadows, background, borders and outline of an element box.
fn paint_box(n: &BoxNode, c: &Computed, p: &mut Painter, vp: (f32, f32), skip_background: bool) {
    let b = n.bounds;
    if matches!(c.display, DisplayKind::Contents) {
        return;
    }
    let r = radius_px(c, b, vp);
    // each corner's own radius (`border-radius: 56px 56px 6px 6px`): drawn as one rounded box when
    // they are one, else as a polygon
    let radii = corner_radii(c, b, vp);
    // outer shadows, the first on top (CSS Backgrounds 6.1); an empty box's spread still shows. Each
    // is the border box moved and spread, blurred by a Gaussian of half the blur radius (7.2), and
    // shows only outside the border box (7.1.1)
    for s in c.shadows.iter().rev().filter(|s| !s.inset) {
        let (w, h) = (b.size.width + 2.0 * s.spread, b.size.height + 2.0 * s.spread);
        if w <= 0.0 || h <= 0.0 {
            continue;
        }
        let sb = rect(b.origin.x + s.x - s.spread, b.origin.y + s.y - s.spread, w, h);
        // square corners stay square under spread
        let corner = if r > 0.0 { (r + s.spread).max(0.0) } else { 0.0 };
        p.shadow((sb, corner), (b, r), s.blur / 2.0, s.color);
    }
    if b.size.width <= 0.0 && b.size.height <= 0.0 {
        return;
    }
    // `background-clip`: the painting area; its radius shrinks with it
    let bw = border_widths(c, b.size.width);
    // `background-attachment: local` on a scroll container: painted in its scrollport only, placed
    // on its scrolled content (CSS Backgrounds 3.5)
    let ov = c.layout.overflow;
    let local = c.images.as_deref().is_some_and(|s| s.bg_local)
        && (ov.x != taffy::style::Overflow::Visible || ov.y != taffy::style::Overflow::Visible);
    let clip_box = edge_box(c, b, c.bg_clip, &bw);
    let clip_box = if local { clip_box.intersect(&edge_box(c, b, BoxEdge::Padding, &bw)) } else { clip_box };
    let clip_r = (r - (clip_box.origin.x - b.origin.x)).max(0.0);
    let clip_radii = radii_inside(radii, b, clip_box);
    let origin_area = |st: &crate::style::ImageStyle| {
        if st.bg_fixed {
            return rect(0.0, 0.0, vp.0, vp.1);
        }
        let a = edge_box(c, b, c.bg_origin, &bw);
        if local {
            let ((sx, sy), (rx, ry)) = (n.scroll_pos, n.scroll_range);
            rect(a.origin.x - sx, a.origin.y - sy, a.size.width + rx, a.size.height + ry)
        } else {
            a
        }
    };
    if !(skip_background && c.bg_clip == BoxEdge::Border) && !c.background.is_transparent() {
        if one_radius(&clip_radii) {
            p.quad(clip_box, clip_r, c.background, Rgba::TRANSPARENT, 0.0);
        } else {
            p.polygon(&rounded_points(clip_box, clip_radii), c.background);
        }
    }
    if let Some(g) = &c.gradient {
        // a gradient is an image with no natural size: sized, positioned and repeated as one
        let plain;
        let st = match c.images.as_deref() {
            Some(st) => st,
            None => {
                plain = crate::style::ImageStyle::default();
                &plain
            }
        };
        let area = origin_area(st);
        // one tile is painted as shapes; repeated ones draw it rasterized once
        let one = !st.bg_repeat.0 && !st.bg_repeat.1;
        let tiles = |p: &mut Painter| tile_background(p, st, area, clip_box, crate::style::Natural::default(), |p, t| {
            match gradient_image(g, t.size.width, t.size.height).filter(|_| !one) {
                Some(img) => p.image(t, &img),
                None => p.with_clip(t, |p| paint_gradient(p, g, t)),
            }
        });
        if clip_radii.iter().any(|r| *r > 0.0) { with_rounded_clip(p, clip_box, clip_radii, tiles) } else { tiles(p) }
    }
    if let Some(st) = c.images.as_deref().filter(|st| st.bg_url.is_some()) {
        // `background-origin` positions the image, `-attachment: fixed` against the viewport
        let area = origin_area(st);
        let clip_to = if st.bg_fixed { clip_box } else { clip_box.intersect(&b) };
        paint_background_image(p, st, area, clip_to);
    }
    // inset shadows: the padding box outside its offset, spread-shrunk copy (the hole), painted as
    // a ring wide enough to cover the box around the hole
    let pad = rect(b.origin.x + bw[3], b.origin.y + bw[0], (b.size.width - bw[1] - bw[3]).max(0.0),
                   (b.size.height - bw[0] - bw[2]).max(0.0));
    let pad_r = (r - bw[0].max(bw[3])).max(0.0);
    for s in c.shadows.iter().rev().filter(|s| s.inset) {
        let ring = b.size.width + b.size.height + s.x.abs() + s.y.abs() + s.spread.abs() + s.blur;
        let steps = (s.blur / 2.0).ceil().clamp(1.0, 8.0) as usize;
        let paint = |p: &mut Painter| {
            for k in 0..steps {
                let shrink = s.spread + s.blur * ((k as f32 + 1.0) / (steps as f32 + 1.0) - 0.5);
                let a = s.color.a / steps as f32 * if s.blur > 0.0 { 1.0 } else { steps as f32 };
                let hole = rect(pad.origin.x + s.x + shrink, pad.origin.y + s.y + shrink,
                                (pad.size.width - 2.0 * shrink).max(0.0), (pad.size.height - 2.0 * shrink).max(0.0));
                let outer = rect(hole.origin.x - ring, hole.origin.y - ring, hole.size.width + 2.0 * ring, hole.size.height + 2.0 * ring);
                let hr = if pad_r > 0.0 { (pad_r - shrink).max(0.0) } else { 0.0 };
                p.quad(outer, hr + ring, Rgba::TRANSPARENT, s.color.alpha(a), ring);
            }
        };
        if pad_r > 0.0 {
            with_rounded_clip(p, pad, radii_inside(radii, b, pad), paint);
        } else {
            p.with_clip(pad, paint);
        }
    }
    let visible: Vec<usize> = (0..4).filter(|&k| bw[k] > 0.0 && c.border_drawn[k]
                                            && !c.border_color[k].is_transparent()).collect();
    let uniform = visible.len() == 4 && bw.iter().all(|w| *w == bw[0])
        && c.border_color.iter().all(|x| *x == c.border_color[0]);
    let styled = visible.iter().any(|&k| c.border_style[k] != BorderStyle::Solid);
    let border_image = c.effects.as_ref().and_then(|e| e.border_image.as_ref()).filter(|bi| bi.source.is_some());
    if let Some(bi) = border_image {
        paint_border_image(p, bi, b, &bw, &c.units(vp));
    } else if styled && uniform && r > 0.0 && c.border_style.iter().all(|s| *s == c.border_style[0]) {
        paint_rounded_styled(p, b, r, bw[0], c.border_color[0], c.border_style[0]);
    } else if styled {
        let styles: [BorderStyle; 4] = std::array::from_fn(|k| c.border_style[k]);
        let widths: [f32; 4] = std::array::from_fn(|k| if visible.contains(&k) { bw[k] } else { 0.0 });
        paint_edges(p, b, widths, c.border_color, styles);
    } else if uniform && !one_radius(&radii) {
        stroke_rounded4(p, b, radii, bw[0], c.border_color[0]);
    } else if uniform {
        // the kit draws an outline inside its rectangle, as a border is inside the border box
        p.quad(b, r, Rgba::TRANSPARENT, c.border_color[0], bw[0]);
    } else {
        let (x, y, w, h) = (b.origin.x, b.origin.y, b.size.width, b.size.height);
        for k in visible {
            let side = match k {
                0 => rect(x, y, w, bw[0]),
                1 => rect(x + w - bw[1], y, bw[1], h),
                2 => rect(x, y + h - bw[2], w, bw[2]),
                _ => rect(x, y, bw[3], h),
            };
            p.fill(side, c.border_color[k]);
        }
    }
    let (ow, oc, off) = c.outline;
    if ow > 0.0 && !oc.is_transparent() && c.outline_style != BorderStyle::Solid {
        let g = off + ow;
        let ob = rect(b.origin.x - g, b.origin.y - g, b.size.width + 2.0 * g, b.size.height + 2.0 * g);
        paint_edges(p, ob, [ow; 4], [oc; 4], [c.outline_style; 4]);
    } else if ow > 0.0 && !oc.is_transparent() {
        // drawn inside the rectangle `outline-offset` + its width out from the border box
        let g = off + ow;
        p.quad(rect(b.origin.x - g, b.origin.y - g, b.size.width + 2.0 * g, b.size.height + 2.0 * g),
               r + g, Rgba::TRANSPARENT, oc, ow);
    }
}

/// Blender's rounded-box corner tessellation (`cornervec`,
/// interface_widgets.cc): 9 points from the start of a quarter circle to its
/// end, as fractions of the radius.
const CORNERVEC: [[f32; 2]; 9] = [
    [0.0, 0.0], [0.195, 0.02], [0.383, 0.067], [0.55, 0.169], [0.707, 0.293], [0.831, 0.45], [0.924, 0.617],
    [0.98, 0.805], [1.0, 1.0],
];

/// A closed rounded rectangle outline as points (clockwise from the top
/// edge's start), with each point's outward normal direction: 0 top,
/// 1 right, 2 bottom, 3 left (corners split at their middle).
fn rounded_path(b: Bounds<f32>, r: f32) -> Vec<(f32, f32, usize)> {
    let r = r.min(b.size.width / 2.0).min(b.size.height / 2.0).max(0.0);
    rounded_path4(b, [r; 4])
}

/// [`rounded_path`] with a radius a corner (top-left, top-right, bottom-right, bottom-left; each
/// fits its sides already, see [`corner_radii`]).
fn rounded_path4(b: Bounds<f32>, r: [f32; 4]) -> Vec<(f32, f32, usize)> {
    let (x0, y0, x1, y1) = (b.origin.x, b.origin.y, b.origin.x + b.size.width, b.origin.y + b.size.height);
    let [tl, tr, br, bl] = r.map(|v| v.max(0.0));
    let mut pts = Vec::with_capacity(40);
    // top-right, bottom-right, bottom-left, top-left corners, like Blender's round_box edges
    for (k, v) in CORNERVEC.iter().enumerate() {
        pts.push((x1 - tr + v[0] * tr, y0 + tr - (1.0 - v[1]) * tr, if k < 5 { 0 } else { 1 }));
    }
    for (k, v) in CORNERVEC.iter().enumerate() {
        pts.push((x1 - br + (1.0 - v[1]) * br, y1 - br + v[0] * br, if k < 5 { 1 } else { 2 }));
    }
    for (k, v) in CORNERVEC.iter().enumerate() {
        pts.push((x0 + bl - v[0] * bl, y1 - bl + (1.0 - v[1]) * bl, if k < 5 { 2 } else { 3 }));
    }
    for (k, v) in CORNERVEC.iter().enumerate() {
        pts.push((x0 + tl - (1.0 - v[1]) * tl, y0 + tl - v[0] * tl, if k < 5 { 3 } else { 0 }));
    }
    pts
}

/// The points of a rounded box's outline ([`rounded_path4`]).
fn rounded_points(b: Bounds<f32>, r: [f32; 4]) -> Vec<(f32, f32)> {
    rounded_path4(b, r).into_iter().map(|(x, y, _)| (x, y)).collect()
}

/// The ring between the rounded box `b` (radii `r`) and the one `w` inside it: one polygon of
/// both outlines (even-odd fills the ring), joined where each starts.
fn stroke_rounded4(p: &mut Painter, b: Bounds<f32>, r: [f32; 4], w: f32, color: Rgba) {
    let inner = rect(b.origin.x + w, b.origin.y + w, (b.size.width - 2.0 * w).max(0.0), (b.size.height - 2.0 * w).max(0.0));
    let mut pts = rounded_points(b, r);
    let hole = rounded_points(inner, r.map(|v| (v - w).max(0.0)));
    pts.push(pts[0]);
    pts.extend(hole.iter().copied());
    pts.push(hole[0]);
    p.polygon(&pts, color);
}

/// Non-solid uniform borders on a rounded box: dashes and dots follow the
/// curve, `double` is two rounded lines, and groove / ridge / inset / outset
/// shade each side's part of the path.
fn paint_rounded_styled(p: &mut Painter, b: Bounds<f32>, r: f32, w: f32, col: Rgba, style: BorderStyle) {
    let inset = |d: f32| rect(b.origin.x + d, b.origin.y + d, (b.size.width - 2.0 * d).max(0.0),
                              (b.size.height - 2.0 * d).max(0.0));
    match style {
        BorderStyle::Double if w >= 3.0 => {
            let t = w / 3.0;
            p.quad(inset(t / 2.0), (r - t / 2.0).max(0.0), Rgba::TRANSPARENT, col, t);
            p.quad(inset(w - t / 2.0), (r - w + t / 2.0).max(0.0), Rgba::TRANSPARENT, col, t);
        }
        BorderStyle::Dashed | BorderStyle::Dotted => {
            let mut path = rounded_path(inset(w / 2.0), (r - w / 2.0).max(0.0));
            path.push(path[0]);
            let total: f32 = path.windows(2).map(|q| ((q[1].0 - q[0].0).powi(2) + (q[1].1 - q[0].1).powi(2)).sqrt()).sum();
            let dotted = style == BorderStyle::Dotted;
            let (on, gap) = if dotted { (w, w) } else { (w * 3.0, w * 3.0) };
            let n = ((total / (on + gap)).floor()).max(1.0);
            let period = total / n; // an even number of segments around the box
            let on = on.min(period);
            // walk the path, emitting [start, start + on) of every period
            let point_at = |d: f32| -> (f32, f32) {
                let mut acc = 0.0;
                for q in path.windows(2) {
                    let len = ((q[1].0 - q[0].0).powi(2) + (q[1].1 - q[0].1).powi(2)).sqrt();
                    if acc + len >= d && len > 0.0 {
                        let t = (d - acc) / len;
                        return (q[0].0 + (q[1].0 - q[0].0) * t, q[0].1 + (q[1].1 - q[0].1) * t);
                    }
                    acc += len;
                }
                (path[0].0, path[0].1)
            };
            for k in 0..n as usize {
                let start = k as f32 * period;
                if dotted {
                    let (cx, cy) = point_at(start + on / 2.0);
                    p.oval(rect(cx - w / 2.0, cy - w / 2.0, w, w), col, Rgba::TRANSPARENT, 0.0);
                } else {
                    let steps = 6;
                    let pts: Vec<(f32, f32)> = (0..=steps).map(|j| point_at(start + on * j as f32 / steps as f32)).collect();
                    p.line(&pts, col, w);
                }
            }
        }
        _ => {
            // shaded styles: the path in two halves of the width, coloured per side
            let dark = shade(col, 0.5);
            let side_col = |side: usize, outer: bool| -> Rgba {
                let lit = side == 0 || side == 3;
                match style {
                    BorderStyle::Inset => if lit { dark } else { col },
                    BorderStyle::Outset => if lit { col } else { dark },
                    BorderStyle::Groove => if lit == outer { dark } else { col },
                    BorderStyle::Ridge => if lit == outer { col } else { dark },
                    _ => col,
                }
            };
            let halves: &[(f32, bool)] = if matches!(style, BorderStyle::Groove | BorderStyle::Ridge) {
                &[(w / 4.0, true), (w * 3.0 / 4.0, false)]
            } else {
                &[(w / 2.0, true)]
            };
            let lw = if halves.len() == 2 { w / 2.0 } else { w };
            for &(d, outer) in halves {
                let mut path = rounded_path(inset(d), (r - d).max(0.0));
                path.push(path[0]);
                for q in path.windows(2) {
                    p.line(&[(q[0].0, q[0].1), (q[1].0, q[1].1)], side_col(q[0].2, outer), lw);
                }
            }
        }
    }
}

/// Border-image source pixels, cached per source and size.
fn border_image_source(src: &crate::effects::ImageSource, w: f32, h: f32) -> Option<Arc<Image>> {
    match src {
        crate::effects::ImageSource::Url(u) => file_pixels(u),
        crate::effects::ImageSource::Gradient(g) => gradient_image(g, w, h),
    }
}

/// Gradient `g` rasterized at `w` x `h` (whole pixels), cached: drawn once for many tiles.
fn gradient_image(g: &crate::style::Gradient, w: f32, h: f32) -> Option<Arc<Image>> {
    thread_local! {
        static GRADIENTS: std::cell::RefCell<HashMap<String, Arc<Image>>> =
            std::cell::RefCell::new(HashMap::new());
    }
    let (iw, ih) = (w.ceil().max(1.0) as usize, h.ceil().max(1.0) as usize);
    let key = format!("{g:?}{iw}x{ih}");
    if let Some(i) = GRADIENTS.with(|m| m.borrow().get(&key).cloned()) {
        return Some(i);
    }
    let img = Arc::new(match g.shape {
        crate::style::GradientShape::Radial(r) => Image::from_argb(&radial_pixels(g, &r, iw, ih))?,
        crate::style::GradientShape::Linear { .. } => {
            let mut gp = Painter::new();
            let area = rect(0.0, 0.0, iw as f32, ih as f32);
            gp.with_clip(area, |gp| paint_gradient(gp, g, area));
            Image::from_argb(&gp.rasterize(1.0, 0.0, 0.0, iw, ih)?.to_argb())?
        }
    });
    GRADIENTS.with(|m| {
        let mut m = m.borrow_mut();
        if m.len() > 64 {
            m.clear();
        }
        m.insert(key, img.clone());
    });
    Some(img)
}

/// A radial gradient over a `w` x `h` box, a colour per pixel (premultiplied ARGB): its ending
/// shape sized and placed in the box (CSS Images 3 §3.2.2), the stops along its x radius.
fn radial_pixels(g: &crate::style::Gradient, r: &crate::style::Radial, w: usize, h: usize) -> crate::effects::Argb {
    use crate::style::RadialSize;
    let (bw, bh) = (w as f32, h as f32);
    let (cx, cy) = (r.at[0].at(bw), r.at[1].at(bh));
    // the distances to the sides, and the corner farthest / closest
    let (dx_near, dx_far) = (cx.abs().min((bw - cx).abs()), cx.abs().max((bw - cx).abs()));
    let (dy_near, dy_far) = (cy.abs().min((bh - cy).abs()), cy.abs().max((bh - cy).abs()));
    let (rx, ry) = match r.size {
        RadialSize::Radii(x, y) => (x.at(bw), y.at(bh)),
        RadialSize::Extent { corner, farthest } => {
            let (sx, sy) = if farthest { (dx_far, dy_far) } else { (dx_near, dy_near) };
            match (r.circle, corner) {
                (true, false) => if farthest { (sx.max(sy), sx.max(sy)) } else { (sx.min(sy), sx.min(sy)) },
                (true, true) => {
                    let d = sx.hypot(sy);
                    (d, d)
                }
                (false, false) => (sx, sy),
                // the side's aspect, grown until it passes through the corner (that is, by √2)
                (false, true) => (sx * std::f32::consts::SQRT_2, sy * std::f32::consts::SQRT_2),
            }
        }
    };
    let (rx, ry) = (rx.max(1e-3), ry.max(1e-3));
    let stops = g.resolved(rx);
    let mut px = vec![0u32; w * h];
    for (j, row) in px.chunks_exact_mut(w).enumerate() {
        let dy = (j as f32 + 0.5 - cy) / ry;
        for (i, out) in row.iter_mut().enumerate() {
            let dx = (i as f32 + 0.5 - cx) / rx;
            let c = gradient_at(&stops, dx.hypot(dy));
            let a = c.a.clamp(0.0, 1.0);
            let ch = |v: f32| (v.clamp(0.0, 1.0) * a * 255.0 + 0.5) as u32;
            *out = ((a * 255.0 + 0.5) as u32) << 24 | ch(c.r) << 16 | ch(c.g) << 8 | ch(c.b);
        }
    }
    crate::effects::Argb { w, h, px }
}

/// A sub-rectangle of an image (whole pixels), cached.
fn crop(img: &Arc<Image>, x: u32, y: u32, w: u32, h: u32) -> Option<Arc<Image>> {
    if w == 0 || h == 0 || x + w > img.width || y + h > img.height {
        return None;
    }
    // keyed by the source's address, with a weak reference to it: the address is not reused while
    // the entry holds it, and a source that has gone (no strong reference) is a miss
    type Crops = HashMap<(usize, [u32; 4]), (std::sync::Weak<Image>, Arc<Image>)>;
    thread_local! {
        static CROPS: std::cell::RefCell<Crops> = std::cell::RefCell::new(HashMap::new());
    }
    let key = (Arc::as_ptr(img) as usize, [x, y, w, h]);
    let hit = CROPS.with(|m| m.borrow().get(&key).filter(|(src, _)| src.strong_count() > 0).map(|(_, c)| c.clone()));
    if hit.is_some() {
        return hit;
    }
    let all = img.to_rgba();
    let mut rgba = Vec::with_capacity((w * h * 4) as usize);
    for row in y..y + h {
        let s = ((row * img.width + x) * 4) as usize;
        rgba.extend_from_slice(&all[s..s + (w * 4) as usize]);
    }
    let c = Arc::new(Image::from_rgba(w, h, &rgba)?);
    CROPS.with(|m| {
        let mut m = m.borrow_mut();
        if m.len() > 512 {
            m.clear();
        }
        m.insert(key, (Arc::downgrade(img), c.clone()));
    });
    Some(c)
}

/// `border-image`: the source cut into nine parts by `slice`; corners go in
/// the corners of the border-image area, edges are stretched or tiled along
/// the sides, and the middle is drawn with `fill`.
fn paint_border_image(p: &mut Painter, bi: &crate::effects::BorderImage, b: Bounds<f32>, bw: &[f32; 4], u: &Units) {
    let resolve = |v: Side, k: usize, basis: f32| -> f32 {
        match v {
            Side::Num(n) => n * bw[k],
            Side::Len(l) => l.resolve(u, basis),
            Side::Auto => bw[k],
        }
    };
    let out: [f32; 4] = std::array::from_fn(|k| resolve(bi.outset[k], k, 0.0));
    let area = rect(b.origin.x - out[3], b.origin.y - out[0], b.size.width + out[1] + out[3],
                    b.size.height + out[0] + out[2]);
    let Some(src) = bi.source.as_ref().and_then(|s| border_image_source(s, area.size.width, area.size.height)) else {
        return;
    };
    let (iw, ih) = (src.width as f32, src.height as f32);
    // slices in image pixels (numbers are pixels; percentages of the image)
    let sl: [f32; 4] = std::array::from_fn(|k| {
        let basis = if k % 2 == 0 { ih } else { iw };
        match bi.slice[k] {
            Side::Num(n) => n,
            Side::Len(Length::Percent(pc)) => basis * pc / 100.0,
            Side::Len(l) => l.resolve(u, basis),
            Side::Auto => 0.0,
        }.clamp(0.0, basis)
    });
    // widths in CSS px (numbers: multiples of the border width; auto: the slice)
    let mut wd: [f32; 4] = std::array::from_fn(|k| match bi.width[k] {
        Side::Auto => sl[k],
        v => resolve(v, k, if k % 2 == 0 { area.size.height } else { area.size.width }),
    });
    // scale down when opposite widths overlap
    let f = (area.size.width / (wd[1] + wd[3]).max(1e-6)).min(area.size.height / (wd[0] + wd[2]).max(1e-6)).min(1.0);
    wd.iter_mut().for_each(|v| *v *= f);
    let (ax, ay, aw, ah) = (area.origin.x, area.origin.y, area.size.width, area.size.height);
    let (sx, sy) = ([0.0, sl[3], iw - sl[1], iw], [0.0, sl[0], ih - sl[2], ih]);
    let (dx, dy) = ([ax, ax + wd[3], ax + aw - wd[1], ax + aw], [ay, ay + wd[0], ay + ah - wd[2], ay + ah]);
    for j in 0..3 {
        for i in 0..3 {
            if i == 1 && j == 1 && !bi.fill {
                continue;
            }
            let (cx, cy) = (sx[i].round() as u32, sy[j].round() as u32);
            let (cw, ch) = ((sx[i + 1] - sx[i]).round() as u32, (sy[j + 1] - sy[j]).round() as u32);
            let dest = rect(dx[i], dy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j]);
            if dest.size.width <= 0.0 || dest.size.height <= 0.0 {
                continue;
            }
            let Some(part) = crop(&src, cx, cy, cw, ch) else { continue };
            // edges tile along their length; corners always stretch
            let tile_x = if j != 1 || i == 1 { if i == 1 { bi.repeat.0 } else { Tile::Stretch } } else { Tile::Stretch };
            let tile_y = if i != 1 || j == 1 { if j == 1 { bi.repeat.1 } else { Tile::Stretch } } else { Tile::Stretch };
            // the tile's size: scaled to the edge's thickness
            let (mut tw, mut th) = (dest.size.width, dest.size.height);
            if tile_x != Tile::Stretch && i == 1 {
                tw = cw as f32 * (dest.size.height / ch.max(1) as f32);
            }
            if tile_y != Tile::Stretch && j == 1 {
                th = ch as f32 * (dest.size.width / cw.max(1) as f32);
            }
            let fit = |len: f32, t: f32, mode: Tile| -> (f32, usize, f32) {
                if mode == Tile::Stretch || t <= 0.0 {
                    return (len, 1, 0.0);
                }
                match mode {
                    Tile::Round => {
                        let n = (len / t).round().max(1.0);
                        (len / n, n as usize, 0.0)
                    }
                    Tile::Space => {
                        let n = (len / t).floor();
                        if n < 1.0 { (t, 0, 0.0) } else { (t, n as usize, (len - n * t) / (n + 1.0)) }
                    }
                    _ => {
                        // repeat: centred, partial tiles at both ends
                        let n = (len / t).ceil() as usize + 1;
                        (t, n, 0.0)
                    }
                }
            };
            let (tw2, nx, gx) = fit(dest.size.width, tw, if i == 1 { tile_x } else { Tile::Stretch });
            let (th2, ny, gy) = fit(dest.size.height, th, if j == 1 { tile_y } else { Tile::Stretch });
            let start = |len: f32, t: f32, n: usize, g: f32, mode: Tile| -> f32 {
                if mode == Tile::Repeat { (len - n as f32 * t) / 2.0 } else { g }
            };
            let x0 = dest.origin.x + start(dest.size.width, tw2, nx, gx, if i == 1 { tile_x } else { Tile::Stretch });
            let y0 = dest.origin.y + start(dest.size.height, th2, ny, gy, if j == 1 { tile_y } else { Tile::Stretch });
            p.with_clip(dest, |p| {
                for yy in 0..ny {
                    for xx in 0..nx {
                        let at = rect(x0 + xx as f32 * (tw2 + gx), y0 + yy as f32 * (th2 + gy), tw2, th2);
                        p.image(at, &part);
                    }
                }
            });
        }
    }
}

/// The border, padding or content box of `b`.
fn edge_box(c: &Computed, b: Bounds<f32>, edge: BoxEdge, bw: &[f32; 4]) -> Bounds<f32> {
    match edge {
        BoxEdge::Border => b,
        BoxEdge::Padding => rect(b.origin.x + bw[3], b.origin.y + bw[0], (b.size.width - bw[1] - bw[3]).max(0.0),
                                 (b.size.height - bw[0] - bw[2]).max(0.0)),
        BoxEdge::Content => content_box(c, b),
    }
}

/// A colour darkened (`f` < 1) or lightened toward white (`f` > 1), for 3D borders.
fn shade(c: Rgba, f: f32) -> Rgba {
    if f <= 1.0 {
        Rgba { r: c.r * f, g: c.g * f, b: c.b * f, a: c.a }
    } else {
        mix(c, Rgba { r: 1.0, g: 1.0, b: 1.0, a: c.a }, f - 1.0)
    }
}

/// Borders (or an outline) with non-solid styles, side by side inside
/// `b`: dashes and dots run along each side, `double` is two lines, and
/// groove / ridge / inset / outset shade the sides. Corners are square.
fn paint_edges(p: &mut Painter, b: Bounds<f32>, w: [f32; 4], col: [Rgba; 4], style: [BorderStyle; 4]) {
    let (x, y, bw, bh) = (b.origin.x, b.origin.y, b.size.width, b.size.height);
    // the band of side k, inset by `from` and `to` (fractions of its width)
    let band = |k: usize, from: f32, to: f32| -> Bounds<f32> {
        let (a, t) = (w[k] * from, w[k] * (to - from));
        match k {
            0 => rect(x, y + a, bw, t),
            1 => rect(x + bw - w[1] + a, y, t, bh),
            2 => rect(x, y + bh - w[2] + a, bw, t),
            _ => rect(x + a, y, t, bh),
        }
    };
    for k in 0..4 {
        if w[k] <= 0.0 || col[k].is_transparent() {
            continue;
        }
        let c = col[k];
        // top and left are the "light source" sides for inset / outset
        let lit = k == 0 || k == 3;
        match style[k] {
            BorderStyle::Solid => p.fill(band(k, 0.0, 1.0), c),
            BorderStyle::Double if w[k] >= 3.0 => {
                p.fill(band(k, 0.0, 1.0 / 3.0), c);
                p.fill(band(k, 2.0 / 3.0, 1.0), c);
            }
            BorderStyle::Double => p.fill(band(k, 0.0, 1.0), c),
            BorderStyle::Inset => p.fill(band(k, 0.0, 1.0), if lit { shade(c, 0.5) } else { c }),
            BorderStyle::Outset => p.fill(band(k, 0.0, 1.0), if lit { c } else { shade(c, 0.5) }),
            BorderStyle::Groove | BorderStyle::Ridge => {
                let (dark, light) = (shade(c, 0.5), c);
                let outer_dark = (style[k] == BorderStyle::Groove) == lit;
                p.fill(band(k, 0.0, 0.5), if outer_dark { dark } else { light });
                p.fill(band(k, 0.5, 1.0), if outer_dark { light } else { dark });
            }
            BorderStyle::Dashed | BorderStyle::Dotted => {
                let horizontal = k == 0 || k == 2;
                let full = band(k, 0.0, 1.0);
                let len = if horizontal { full.size.width } else { full.size.height };
                let t = w[k];
                let (on, gap) = if style[k] == BorderStyle::Dotted { (t, t) } else { (t * 3.0, t * 3.0) };
                // spread the segments so both ends start and end with one
                let n = ((len + gap) / (on + gap)).floor().max(1.0);
                let gap = if n > 1.0 { (len - n * on) / (n - 1.0) } else { 0.0 };
                for j in 0..n as usize {
                    let o = j as f32 * (on + gap);
                    let seg = if horizontal {
                        rect(full.origin.x + o, full.origin.y, on.min(len - o), t)
                    } else {
                        rect(full.origin.x, full.origin.y + o, t, on.min(len - o))
                    };
                    if style[k] == BorderStyle::Dotted && t >= 3.0 {
                        p.oval(seg, c, Rgba::TRANSPARENT, 0.0);
                    } else {
                        p.fill(seg, c);
                    }
                }
            }
        }
    }
}

fn mix(a: Rgba, b: Rgba, t: f32) -> Rgba {
    Rgba { r: a.r + (b.r - a.r) * t, g: a.g + (b.g - a.g) * t, b: a.b + (b.b - a.b) * t,
           a: a.a + (b.a - a.a) * t }
}

/// Runs `f` clipped to the rounded box `b` (radii as [`corner_radii`]): what it paints is cut to
/// the corners after ([`round_items`]; gradients have no rounded fill of their own).
fn with_rounded_clip(p: &mut Painter, b: Bounds<f32>, r: [f32; 4], f: impl FnOnce(&mut Painter)) {
    let from = p.items.len();
    p.with_clip(b, f);
    round_items(p, from, b, r);
}

/// The rounded box `b` (radii `r`) as rectangles that cover it once: rows one device pixel tall
/// across the corners (`scale` device pixels per logical one), each as wide as the rounded outline
/// at its height, and the straight middle. Each sits inside its device rows (a clip is cut to whole
/// pixels, so neighbours never share one), and rows with the same edges in device pixels are one.
fn rounded_strips(b: Bounds<f32>, r: [f32; 4], scale: f32) -> Vec<Bounds<f32>> {
    let s = if scale > 0.0 { scale } else { 1.0 };
    let (x, y, w, h) = (b.origin.x, b.origin.y, b.size.width, b.size.height);
    let [tl, tr, br, bl] = r;
    let (top, bottom) = (tl.max(tr), bl.max(br));
    if (top * s < 0.5 && bottom * s < 0.5) || w <= 0.0 || h <= 0.0 {
        return vec![b];
    }
    // whole device rows from `k0` to `k1` (exclusive), as a clip
    let rows = |k0: i64, k1: i64, l: f32, rr: f32| rect(l, (k0 as f32 + 0.02) / s, rr - l, (k1 - k0) as f32 / s - 0.04 / s);
    // how far in the outline is at depth `d` into a corner of radius `r`
    let inset = |r: f32, d: f32| if d >= r { 0.0 } else { r - (r * r - (r - d.max(0.0)).powi(2)).max(0.0).sqrt() };
    // the top band's device rows, the straight middle's, the bottom band's
    let t0 = (y * s).floor() as i64;
    let t1 = ((y + top) * s).ceil() as i64;
    let b1 = ((y + h) * s).ceil() as i64;
    let b0 = (((y + h - bottom) * s).floor() as i64).max(t1).min(b1);
    let t1 = t1.min(b1);
    let mut out: Vec<Bounds<f32>> = Vec::new();
    // a run of rows with the same edges in device pixels: (first row, left, right)
    let mut run: Option<(i64, i64, i64)> = None;
    let mut corner_rows = |ks: std::ops::Range<i64>, out: &mut Vec<Bounds<f32>>| {
        for k in ks.clone() {
            let yc = (k as f32 + 0.5) / s;
            let (dt, db) = (yc - y, y + h - yc);
            let left = inset(tl, dt).max(inset(bl, db));
            let right = inset(tr, dt).max(inset(br, db));
            let (l, rr) = (((x + left) * s).floor() as i64, ((x + w - right) * s).ceil() as i64);
            match run {
                Some((_, pl, pr)) if pl == l && pr == rr => continue,
                Some((k0, pl, pr)) if pr > pl => out.push(rows(k0, k, (pl as f32 + 0.02) / s, (pr as f32 - 0.02) / s)),
                _ => {}
            }
            run = Some((k, l, rr));
        }
        if let Some((k0, pl, pr)) = run.take()
            && pr > pl
        {
            out.push(rows(k0, ks.end, (pl as f32 + 0.02) / s, (pr as f32 - 0.02) / s));
        }
    };
    corner_rows(t0..t1, &mut out);
    if b0 > t1 {
        out.push(rows(t1, b0, x, x + w));
    }
    corner_rows(b0..b1, &mut out);
    out
}

/// The items from `from` on (what one box painted) cut to the rounded corners of box `round`, whose
/// `overflow` clips them (its padding box: see [`round_items`]).
fn clip_to_rounded(p: &mut Painter, from: usize, frame: &Frame, styles: &StyleStore, round: usize, vp: (f32, f32)) {
    // items over a widget start a canvas of their own at their index: leave those as they are
    if p.segments.last().is_some_and(|&seg| seg as usize > from) {
        return;
    }
    let n = &frame.nodes[round];
    let c = &styles[n.style];
    let b = n.bounds;
    let bw = border_widths(c, b.size.width);
    let pad = rect(b.origin.x + bw[3], b.origin.y + bw[0], (b.size.width - bw[1] - bw[3]).max(0.0),
                   (b.size.height - bw[0] - bw[2]).max(0.0));
    round_items(p, from, pad, radii_inside(corner_radii(c, b, vp), b, pad));
}

/// The items from `from` on cut to the rounded box `b` (radii `r`): only those reaching into a
/// corner (the rest are inside it already, as far as their rectangular clip lets them). A filled
/// box or a convex polygon (a gradient's band) becomes its part inside, anti-aliased (the rounded
/// box is convex too); anything else is drawn once in each of [`rounded_strips`].
fn round_items(p: &mut Painter, from: usize, b: Bounds<f32>, r: [f32; 4]) {
    if r.iter().all(|v| *v * p.scale.max(1.0) < 0.5) || p.items.len() <= from {
        return;
    }
    let (x0, y0, x1, y1) = (b.origin.x, b.origin.y, b.origin.x + b.size.width, b.origin.y + b.size.height);
    let corners = [rect(x0, y0, r[0], r[0]), rect(x1 - r[1], y0, r[1], r[1]), rect(x1 - r[2], y1 - r[2], r[2], r[2]),
                   rect(x0, y1 - r[3], r[3], r[3])];
    let mut strips: Option<Vec<Bounds<f32>>> = None;
    let mut outline: Option<Vec<(f32, f32)>> = None;
    for it in p.items.split_off(from) {
        let Some([ax0, ay0, ax1, ay1]) = p.item_area(&it.kind, &it.coords) else {
            p.items.push(it);
            continue;
        };
        let area = rect(ax0, ay0, ax1 - ax0, ay1 - ay0);
        let area = it.clip.map_or(area, |c| c.intersect(&area));
        let reaches = corners.iter().any(|k| {
            let o = k.intersect(&area);
            k.size.width > 0.0 && o.size.width > 0.0 && o.size.height > 0.0
        });
        if !reaches {
            p.items.push(it);
            continue;
        }
        // its shape, when it is a convex one filled
        let shape: Option<Vec<(f32, f32)>> = match (&it.kind, it.coords.as_slice()) {
            (Kind::Rect { radius, width, .. }, [x, y, xe, ye, ..]) if *width <= 0.0 => {
                let own = rect(x.min(*xe), y.min(*ye), (xe - x).abs(), (ye - y).abs());
                Some(rounded_points(own, [radius.min(own.size.width.min(own.size.height) / 2.0); 4]))
            }
            (Kind::Polygon, xy) => {
                let pts: Vec<(f32, f32)> = xy.as_chunks::<2>().0.iter().map(|q| (q[0], q[1])).collect();
                convex(&pts).then_some(pts)
            }
            _ => None,
        };
        if let Some(shape) = shape {
            let cut = clip_convex(&shape, outline.get_or_insert_with(|| rounded_points(b, r)));
            if cut.len() >= 3 {
                p.items.push(Item { kind: Kind::Polygon, coords: Coords::of_points(&cut), snap: false, ..it });
            }
            continue;
        }
        for strip in strips.get_or_insert_with(|| rounded_strips(b, r, p.scale)).iter() {
            let cut = it.clip.map_or(*strip, |c| c.intersect(strip));
            let o = cut.intersect(&area);
            if o.size.width > 0.0 && o.size.height > 0.0 {
                p.items.push(Item { clip: Some(cut), ..it.clone() });
            }
        }
    }
}

/// Whether the polygon `pts` is convex (every turn the same way; a straight run turns neither).
fn convex(pts: &[(f32, f32)]) -> bool {
    let n = pts.len();
    if n < 3 {
        return false;
    }
    let mut sign = 0.0f32;
    for k in 0..n {
        let (a, b, c) = (pts[k], pts[(k + 1) % n], pts[(k + 2) % n]);
        let turn = (b.0 - a.0) * (c.1 - b.1) - (b.1 - a.1) * (c.0 - b.0);
        if turn.abs() < 1e-6 {
            continue;
        }
        if sign == 0.0 {
            sign = turn.signum();
        } else if turn.signum() != sign {
            return false;
        }
    }
    sign != 0.0
}

/// The convex polygon `subject` cut to the convex polygon `clip` (Sutherland-Hodgman).
fn clip_convex(subject: &[(f32, f32)], clip: &[(f32, f32)]) -> Vec<(f32, f32)> {
    // which side is inside: the clip's winding
    let area: f32 = clip.iter().zip(clip.iter().cycle().skip(1)).map(|(a, b)| a.0 * b.1 - b.0 * a.1).sum();
    let sign = if area >= 0.0 { 1.0 } else { -1.0 };
    let mut out = subject.to_vec();
    for k in 0..clip.len() {
        let (a, b) = (clip[k], clip[(k + 1) % clip.len()]);
        if (b.0 - a.0).abs() < 1e-6 && (b.1 - a.1).abs() < 1e-6 {
            continue;
        }
        let side = |q: (f32, f32)| sign * ((b.0 - a.0) * (q.1 - a.1) - (b.1 - a.1) * (q.0 - a.0));
        let input = std::mem::take(&mut out);
        for (j, &cur) in input.iter().enumerate() {
            let prev = input[(j + input.len() - 1) % input.len()];
            let (sc, sp) = (side(cur), side(prev));
            if sc >= 0.0 {
                if sp < 0.0 {
                    let t = sp / (sp - sc);
                    out.push((prev.0 + (cur.0 - prev.0) * t, prev.1 + (cur.1 - prev.1) * t));
                }
                out.push(cur);
            } else if sp >= 0.0 {
                let t = sp / (sp - sc);
                out.push((prev.0 + (cur.0 - prev.0) * t, prev.1 + (cur.1 - prev.1) * t));
            }
        }
        if out.is_empty() {
            break;
        }
    }
    out
}

/// Colour at position `t` (0..1) along a gradient's resolved `stops`, mixed in premultiplied
/// alpha (CSS Images 4 §3.5.4: a fade to `transparent` is not darkened by its black).
fn gradient_at(stops: &[(Rgba, f32)], t: f32) -> Rgba {
    let Some(&(first, p0)) = stops.first() else { return Rgba::TRANSPARENT };
    if t <= p0 {
        return first;
    }
    for w in stops.windows(2) {
        let ((c0, a), (c1, b)) = (w[0], w[1]);
        if t <= b {
            return if b - a < 1e-6 { c1 } else { mix_premultiplied(c0, c1, (t - a) / (b - a)) };
        }
    }
    stops[stops.len() - 1].0
}

/// `a` to `b` by `t` with their colours weighted by their alpha.
fn mix_premultiplied(a: Rgba, b: Rgba, t: f32) -> Rgba {
    let alpha = a.a + (b.a - a.a) * t;
    if alpha <= 1e-6 {
        return Rgba::TRANSPARENT;
    }
    let ch = |x: f32, y: f32| (x * a.a + (y * b.a - x * a.a) * t) / alpha;
    Rgba { r: ch(a.r, b.r), g: ch(a.g, b.g), b: ch(a.b, b.b), a: alpha }
}

/// Gradients as bands: horizontal/vertical strips, or rings for radial ones.
fn paint_gradient(p: &mut Painter, g: &crate::style::Gradient, b: Bounds<f32>) {
    const BANDS: usize = 48;
    let (x, y, w, h) = (b.origin.x, b.origin.y, b.size.width, b.size.height);
    let angle_deg = match g.shape {
        crate::style::GradientShape::Radial(_) => {
            // every pixel its own colour: rings of a colour each band where the stops fade, and
            // stacked translucent ones add up
            if let Some(img) = gradient_image(g, w, h) {
                p.image(b, &img);
            }
            return;
        }
        crate::style::GradientShape::Linear { angle_deg } => angle_deg,
    };
    // angle: 180deg = to bottom, 90deg = to right
    let a = angle_deg.to_radians();
    let (dx, dy) = (a.sin(), -a.cos());
    let len = (w * dx.abs() + h * dy.abs()).max(1.0);
    let (cx, cy) = (x + w / 2.0, y + h / 2.0);
    let stops = g.resolved(len);
    // a band perpendicular to the gradient line from `t0` to `t1` (fractions of it), long enough to
    // cover the box
    let mut band = |t0: f32, t1: f32, col: Rgba| {
        let (s0, s1) = ((t0 - 0.5) * len, (t1 - 0.5) * len + 0.6);
        let (px, py) = (-dy, dx);
        let span = w + h;
        let pts = [
            (cx + dx * s0 + px * span, cy + dy * s0 + py * span),
            (cx + dx * s1 + px * span, cy + dy * s1 + py * span),
            (cx + dx * s1 - px * span, cy + dy * s1 - py * span),
            (cx + dx * s0 - px * span, cy + dy * s0 - py * span),
        ];
        p.polygon(&pts, col);
    };
    let Some(&(first, start)) = stops.first() else { return };
    let (last, end) = stops[stops.len() - 1];
    // the first and last colours fill the line before and after the stops; each segment between
    // two stops is one band when they share a colour (a hard stop's edge stays hard), else bands
    // 2px wide or more
    if start > 0.0 {
        band(0.0, start, first);
    }
    for pair in stops.windows(2) {
        let ((c0, t0), (c1, t1)) = (pair[0], pair[1]);
        let t1 = t1.min(1.0);
        if t1 <= t0.max(0.0) {
            continue;
        }
        let t0 = t0.max(0.0);
        if c0 == c1 {
            band(t0, t1, c0);
            continue;
        }
        // bands on a 2px grid along the line (wider for long segments), so stops a hair apart paint alike
        let step = 2.0f32.max((t1 - t0) * len / BANDS as f32);
        let mut at = (t0 * len / step).floor() * step;
        while at < t1 * len {
            let (a, b) = ((at / len).max(t0), ((at + step) / len).min(t1));
            band(a, b, gradient_at(&stops, (a + b) / 2.0));
            at += step;
        }
    }
    if end < 1.0 {
        band(end.max(0.0), 1.0, last);
    }
}

/// Overlay scrollbars on scroll containers that overflow.
fn paint_scrollbars(n: &BoxNode, p: &mut Painter) {
    let (rx, ry) = n.scroll_range;
    let b = n.bounds;
    let (sx, sy) = n.scroll_pos;
    let thumb = crate::color::rgba(0x8a8a8aa0);
    if ry > 0.0 {
        let h = b.size.height;
        let th = (h * h / (h + ry)).max(16.0);
        let ty = b.origin.y + (h - th) * (sy / ry);
        p.quad(rect(b.origin.x + b.size.width - 6.0, ty + 1.0, 4.0, th - 2.0), 2.0, thumb,
               Rgba::TRANSPARENT, 0.0);
    }
    if rx > 0.0 {
        let w = b.size.width;
        let tw = (w * w / (w + rx)).max(16.0);
        let tx = b.origin.x + (w - tw) * (sx / rx);
        p.quad(rect(tx + 1.0, b.origin.y + b.size.height - 6.0, tw - 2.0, 4.0), 2.0, thumb,
               Rgba::TRANSPARENT, 0.0);
    }
}

/// `background-image: url()` files (PNG), decoded once per process.
fn background_file(url: &str) -> Option<ImageFile> {
    static FILES: std::sync::Mutex<Option<HashMap<String, Option<ImageFile>>>> = std::sync::Mutex::new(None);
    let mut files = FILES.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
    let files = files.get_or_insert_with(HashMap::new);
    if let Some(i) = files.get(url) {
        return i.clone();
    }
    let file = ImageFile::probe(url);
    files.insert(url.to_owned(), file.clone());
    file
}

/// A file's pixels decoded now (border images and offscreen layers, which
/// need them in the frame that asks), cached by path.
fn file_pixels(url: &str) -> Option<Arc<Image>> {
    thread_local! {
        static PIXELS: std::cell::RefCell<HashMap<String, Option<Arc<Image>>>> =
            std::cell::RefCell::new(HashMap::new());
    }
    if let Some(i) = PIXELS.with(|m| m.borrow().get(url).cloned()) {
        return i;
    }
    let img = Image::open(url).map(Arc::new);
    PIXELS.with(|m| {
        let mut m = m.borrow_mut();
        if m.len() >= 64 {
            m.clear();
        }
        m.insert(url.to_owned(), img.clone());
    });
    img
}

/// A background image in its positioning `area` (`background-origin`, or the
/// viewport when fixed): sized, positioned and tiled, clipped to `clip_to`.
fn paint_background_image(p: &mut Painter, st: &crate::style::ImageStyle, area: Bounds<f32>,
                          clip_to: Bounds<f32>) {
    let Some(img) = st.bg_url.as_deref().and_then(background_file) else { return };
    tile_background(p, st, area, clip_to, img.natural(), |p, t| p.image_file(t, &img.src));
}

/// The tiles of a background image of natural size `natural` (CSS Backgrounds 3.6 to 3.9): sized by
/// `background-size` and placed by `background-position` in positioning area `area`, repeated by
/// `background-repeat` over painting area `clip_to`; `draw` paints one.
fn tile_background(p: &mut Painter, st: &crate::style::ImageStyle, area: Bounds<f32>, clip_to: Bounds<f32>,
                   natural: crate::style::Natural, draw: impl Fn(&mut Painter, Bounds<f32>)) {
    use crate::style::{REPEAT_ROUND, REPEAT_SPACE};
    let (ax, ay, aw, ah) = (area.origin.x, area.origin.y, area.size.width, area.size.height);
    let (mut tw, mut th) = st.tile((aw, ah), natural);
    if tw < 0.5 || th < 0.5 || aw <= 0.0 || ah <= 0.0 {
        return;
    }
    // `round`: a whole number of tiles fills the area (CSS Backgrounds 3.4)
    if st.bg_repeat.0 && st.bg_spacing[0] == REPEAT_ROUND {
        tw = aw / (aw / tw).round().max(1.0);
    }
    if st.bg_repeat.1 && st.bg_spacing[1] == REPEAT_ROUND {
        th = ah / (ah / th).round().max(1.0);
    }
    // each axis: where the tiles start, their pitch, how many: one where `background-position` puts
    // it; repeated (from before the painting area's start to past its end); or `space`d, as many
    // whole tiles as fit with the rest shared between them (one alone where it is placed)
    let axis = |repeat: bool, spacing: u8, t: f32, a0: f32, alen: f32, pos: crate::style::Pos, c0: f32, clen: f32| {
        let at = a0 + pos.at(alen - t);
        if !repeat {
            return (at, t, 1usize);
        }
        let (start, pitch) = if spacing == REPEAT_SPACE {
            let n = (alen / t).floor();
            if n < 2.0 {
                return (at, t, usize::from(n >= 1.0));
            }
            (a0, t + (alen - n * t) / (n - 1.0))
        } else {
            (at, t)
        };
        let first = start - ((start - a0.min(c0)) / pitch).ceil() * pitch;
        let n = (((c0 + clen).max(a0 + alen) - first) / pitch).ceil() as usize;
        (first, pitch, n)
    };
    let (sx, px, nx) = axis(st.bg_repeat.0, st.bg_spacing[0], tw, ax, aw, st.bg_position[0], clip_to.origin.x, clip_to.size.width);
    let (sy, py, ny) = axis(st.bg_repeat.1, st.bg_spacing[1], th, ay, ah, st.bg_position[1], clip_to.origin.y, clip_to.size.height);
    if nx.saturating_mul(ny) > 4096 {
        return; // tiny tiles over a huge box: not worth thousands of quads
    }
    p.with_clip(clip_to, |p| {
        for j in 0..ny {
            for i in 0..nx {
                draw(p, rect(sx + i as f32 * px, sy + j as f32 * py, tw, th));
            }
        }
    });
}
