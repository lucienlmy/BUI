//! `text-overflow: ellipsis` against the container's content box, and text
//! never clipped inside its own line box (the chat-list item of a messenger).

use blenderui::prelude::*;

type Img = (usize, usize, Vec<u32>);

fn open<V: Render>(cx: &mut App, w: f32, h: f32, v: V) -> WindowId {
    let opts = WindowOptions {
        window_bounds: Some(WindowBounds::Windowed(Bounds {
            origin: point(px(0.0), px(0.0)),
            size: size(px(w), px(h)),
        })),
        ..Default::default()
    };
    cx.open_window(opts, |_, cx| cx.new(|_| v)).unwrap().id
}

/// Rows and columns holding light ink (text on a dark ground) inside `b`:
/// (top, bottom, left, right) in device pixels.
fn ink(img: &Img, b: Bounds<f32>) -> Option<(usize, usize, usize, usize)> {
    let s = img.0 as f32 / 400.0;
    let (x0, y0) = ((b.origin.x * s).floor() as usize, (b.origin.y * s).floor() as usize);
    let (x1, y1) = (((b.origin.x + b.size.width) * s).ceil() as usize,
                    ((b.origin.y + b.size.height) * s).ceil() as usize);
    let mut r: Option<(usize, usize, usize, usize)> = None;
    for y in y0..y1.min(img.1) {
        for x in x0..x1.min(img.0) {
            if (img.2[y * img.0 + x] >> 16) & 0xff > 110 {
                r = Some(r.map_or((y, y, x, x), |(t, bo, l, ri)| (t.min(y), bo.max(y), l.min(x), ri.max(x))));
            }
        }
    }
    r
}

/// The rows of ink in the last `cols` columns of the ink: a `…` is dots on
/// the baseline, a cut letter is tall.
fn tail_height(img: &Img, b: Bounds<f32>) -> usize {
    let (top, bottom, _, right) = ink(img, b).expect("no text");
    let (mut t, mut bo) = (usize::MAX, 0);
    for y in top..=bottom {
        if (right.saturating_sub(2)..=right).any(|x| (img.2[y * img.0 + x] >> 16) & 0xff > 110) {
            t = t.min(y);
            bo = bo.max(y);
        }
    }
    bo - t + 1
}

struct Row {
    text: &'static str,
    style: &'static str,
}

impl Render for Row {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body style="margin: 0; background: #18181b; color: #a1a1aa; font-size: 14px">
                <div id="box" style="width: 120px; display: flex">
                    <div id="t" style={self.style}>{self.text}</div>
                </div>
            </body>
        }
    }
}

const LONG: &str = "A very long message that should get truncated with an ellipsis at the edge";
const CLIP: &str = "flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis";

fn render_row(cx: &mut App, text: &'static str, style: &'static str) -> (Img, Bounds<f32>, Bounds<f32>) {
    let id = open(cx, 400.0, 60.0, Row { text, style });
    let img = cx.draw(id);
    (img, cx.element_bounds(id, "box").unwrap(), cx.element_bounds(id, "t").unwrap())
}

#[test]
fn long_nowrap_text_ends_with_an_ellipsis_at_the_content_width() {
    Application::headless().run(|cx| {
        let (img, boxb, t) = render_row(cx, LONG, CLIP);
        assert_eq!(t.size.width, boxb.size.width, "the item is the container's width");
        let (_, _, _, right) = ink(&img, grow(t, 60.0)).unwrap();
        let s = img.0 as f32 / 400.0;
        assert!(right as f32 <= (boxb.origin.x + boxb.size.width) * s, "ink past the container");
        assert!(tail_height(&img, t) <= (7.0 * s) as usize, "no `…`: the text ends on a cut glyph");
    });
}

#[test]
fn short_text_has_no_ellipsis() {
    Application::headless().run(|cx| {
        let (img, _, t) = render_row(cx, "Hi!", CLIP);
        // `!` is a full-height stem, a `…` would be three low dots
        assert!(tail_height(&img, t) > 8, "short text must be left alone");
    });
}

#[test]
fn clip_cuts_without_an_ellipsis_and_visible_overflow_is_not_cut() {
    Application::headless().run(|cx| {
        let clip = "flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: clip";
        let (img, boxb, t) = render_row(cx, LONG, clip);
        let s = img.0 as f32 / 400.0;
        let (_, _, _, right) = ink(&img, t).unwrap();
        assert!(right as f32 <= (boxb.origin.x + boxb.size.width) * s, "clip keeps the text inside");
        // text-overflow only applies when overflow clips: this overflows the box, no `…`
        let visible = "white-space: nowrap; text-overflow: ellipsis";
        let (img, boxb, _) = render_row(cx, LONG, visible);
        let (_, _, _, right) = ink(&img, grow(t, 60.0)).unwrap();
        assert!(right as f32 > (boxb.origin.x + boxb.size.width) * s, "visible overflow spills out");
    });
}

// -- the messenger's chat-list item ----------------------------------------------------------

#[component]
fn chat(title: String, content: String, stamp: String, pinned: bool, unread: bool) {
    rsx! {
        <div flex items-center p-3 border-b border-white/10>
            <div size-12 mr-4 shrink-0 rounded-full bg-zinc-600 overflow-hidden flex items-center justify-center>
                <span text-lg font-medium text-white>{title.clone()}</span>
            </div>
            <div flex-1 min-w-0>
                <div flex justify-between items-center mb-1>
                    <div id={format!("{title}-title")} flex-1 min-w-0 font-medium text-white truncate>{title.clone()}</div>
                    <span text-xs text-zinc-400>{stamp}</span>
                </div>
                <div flex justify-between items-center gap-2>
                    <div id={format!("{title}-sub")} flex-1 min-w-0 h-5 text-sm text-zinc-400 truncate>{content.clone()}</div>
                    <div flex items-center gap-2 shrink-0>
                        {pinned.then(|| rsx! { <div text-xs text-zinc-400>"pinned"</div> })}
                        {unread.then(|| rsx! { <div w-2 h-2 rounded-full bg-emerald-500 /> })}
                    </div>
                </div>
            </div>
        </div>
    }
}

struct List;

impl Render for List {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body m-0 flex flex-col bg-zinc-900 text-white>
                <chat title={"Alice".to_string()} content={"See you tomorrow!".to_string()} stamp={"09:41".to_string()} pinned={true} unread={true} />
                <chat title={"Bob".to_string()} content={LONG.to_string()} stamp={"18:05".to_string()} pinned={false} unread={false} />
            </body>
        }
    }
}

#[test]
fn chat_list_item_cuts_long_subtitle_and_shows_whole_glyphs() {
    Application::headless().run(|cx| {
        let id = open(cx, 400.0, 160.0, List);
        let img = cx.draw(id);
        let s = img.0 as f32 / 400.0;
        // the second row's subtitle is long: it ends in a `…` inside its container
        let subs = [cx.element_bounds(id, "Alice-sub").unwrap(), cx.element_bounds(id, "Bob-sub").unwrap()];
        let (_, _, _, right) = ink(&img, grow(subs[1], 40.0)).expect("subtitle drawn");
        assert!(right as f32 <= (subs[1].origin.x + subs[1].size.width) * s, "subtitle spills");
        assert!(tail_height(&img, subs[1]) <= (7.0 * s) as usize, "Bob's subtitle has no `…`");
        // Alice's fits: no `…`, and her line boxes hold their glyphs entirely
        assert!(tail_height(&img, subs[0]) > 8, "'See you tomorrow!' was cut");
        // and its glyphs lie inside its box (they sat 9px high, cut by the box's top)
        let (top, bottom, _, _) = ink(&img, subs[0]).unwrap();
        let (y0, y1) = (subs[0].origin.y * s, (subs[0].origin.y + subs[0].size.height) * s);
        assert!(top as f32 >= y0 && (bottom + 1) as f32 <= y1, "glyph rows {top}..{bottom} vs box {y0}..{y1}");
        assert!(top as f32 <= y0 + 6.0 * s, "text sits at the box's top, not floating below it");
    });
}

/// `b` widened to the right: to see text spilling out of its box.
fn grow(b: Bounds<f32>, dx: f32) -> Bounds<f32> {
    Bounds { origin: b.origin, size: size(b.size.width + dx, b.size.height) }
}

struct Sizes;

impl Render for Sizes {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body m-0>
                <div id="xs" text-xs>x</div>
                <div id="sm" text-sm>x</div>
                <div id="base" text-base>x</div>
                <div id="lg" text-lg>x</div>
                <div id="xl" text-xl>x</div>
            </body>
        }
    }
}

/// Tailwind's `text-sm` sets `line-height: calc(1.25 / 0.875)`: a number, a
/// factor of the font size (20px here), not 1.43 px (which put the text 9px
/// above its box, where the parent's `overflow: hidden` cut it).
#[test]
fn unitless_calc_line_height_is_a_factor() {
    Application::headless().run(|cx| {
        let id = open(cx, 200.0, 200.0, Sizes);
        cx.draw(id);
        for (n, h) in [("xs", 16.0), ("sm", 20.0), ("base", 24.0), ("lg", 28.0), ("xl", 28.0)] {
            assert_eq!(cx.element_bounds(id, n).unwrap().size.height, h, "{n}");
        }
    });
}

/// The same text with and without the parent's `overflow: hidden` has the
/// same ink: nothing is cut inside its own line box.
#[test]
fn overflow_hidden_does_not_cut_the_glyphs() {
    Application::headless().run(|cx| {
        const TEXT: &str = "Alice Qgy";
        for (plain, hidden) in [
            ("font-size: 14px; line-height: calc(1.25 / 0.875)", "font-size: 14px; line-height: calc(1.25 / 0.875); overflow: hidden"),
            ("font-size: 16px", "font-size: 16px; overflow: hidden"),
            ("font-size: 12px; line-height: 1.3", "font-size: 12px; line-height: 1.3; overflow: hidden"),
        ] {
            let (a, _, ta) = render_row(cx, TEXT, plain);
            let (b, _, tb) = render_row(cx, TEXT, hidden);
            let (ia, ib) = (ink(&a, ta).unwrap(), ink(&b, tb).unwrap());
            assert_eq!(ia, ib, "`{plain}`: ink {ib:?} with overflow hidden, {ia:?} without");
            let s = a.0 as f32 / 400.0;
            assert!(ib.0 as f32 >= tb.origin.y * s && (ib.1 + 1) as f32 <= (tb.origin.y + tb.size.height) * s,
                    "`{plain}`: glyph rows {ib:?} leave the line box {tb:?}");
        }
    });
}

