// The benchmark scene on gpui, shared by gpui-bench and gpui-fast-bench
// (included by each; `APP_NAME` labels the results).

#[path = "../../common/measure.rs"]
mod measure;

use gpui::{
    Animation, AnimationExt, App, AppContext, Bounds, Context, IntoElement, ParentElement, Render, Styled,
    Window, WindowBounds, WindowOptions, bounce, div, ease_in_out, px, rgb, rgba, size,
};

struct Scene {
    cells: usize,
    animate: bool,
    t: u64,
}

impl Render for Scene {
    fn render(&mut self, window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        measure::frame();
        let cfg = measure::Config::from_args();
        let (_, w, h, font) = cfg.grid();
        let t = self.t;
        if self.animate {
            self.t += 1;
            window.request_animation_frame();
        }
        div()
            .size_full()
            .bg(rgb(0x303030))
            .flex()
            .flex_wrap()
            .content_start()
            .gap(px(2.0))
            .p(px(2.0))
            .children((0..self.cells).map(|i| {
                let cell = div()
                    .w(px(w))
                    .h(px(h))
                    .bg(rgb(measure::cell_color(i, cfg.t_for(i, t))))
                    .rounded(px(3.0))
                    .flex()
                    .items_center()
                    .justify_center()
                    .text_size(px(font))
                    .text_color(rgb(0xffffff))
                    .child(measure::cell_label(i, cfg.t_for(i, t)));
                if !cfg.animates(i) {
                    return cell.into_any_element();
                }
                // colour and opacity, there and back, eased, forever (gpui has no
                // keyframes or start offsets: the offset shifts the colour phase)
                let off = measure::motion_offset(i);
                cell.with_animation(
                    ("cell", i),
                    Animation::new(std::time::Duration::from_secs_f32(measure::MOTION_SECONDS * 2.0))
                        .repeat()
                        .with_easing(bounce(ease_in_out)),
                    move |el, d| {
                        let d = (d + off) % 1.0;
                        let mix = |a: u32, b: u32, s: u32| {
                            let (x, y) = (((a >> s) & 0xff) as f32, ((b >> s) & 0xff) as f32);
                            ((x + (y - x) * d) as u32) << s
                        };
                        let c = mix(measure::MOTION_FROM, measure::MOTION_TO, 16)
                            | mix(measure::MOTION_FROM, measure::MOTION_TO, 8)
                            | mix(measure::MOTION_FROM, measure::MOTION_TO, 0);
                        el.bg(rgba((c << 8) | 0xff)).opacity(1.0 - 0.7 * d)
                    },
                )
                .into_any_element()
            }))
    }
}

fn main() {
    let cfg = measure::Config::from_args();
    measure::start(APP_NAME, &cfg);
    new_app().run(move |cx: &mut App| {
        let bounds = Bounds::centered(None, size(px(cfg.width), px(cfg.height)), cx);
        cx.open_window(
            WindowOptions { window_bounds: Some(WindowBounds::Windowed(bounds)), ..Default::default() },
            |_, cx| cx.new(|_| Scene { cells: cfg.cells, animate: cfg.animate, t: 0 }),
        )
        .unwrap();
        cx.activate(true);
    });
}
