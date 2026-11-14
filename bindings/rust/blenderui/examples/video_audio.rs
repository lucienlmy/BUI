//! A video with sound: it plays its audio on the default output, the buttons
//! mute it, change the volume and pause it. `cargo run --release --example video_audio [clip.mp4]`
//! (the default is the 2 s test clip, a 440 Hz tone, looping).

use std::rc::Rc;

use blenderui::prelude::*;
use blenderui::Video;

struct Player(Rc<Video>);

impl Render for Player {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let (mute, quieter, louder, pause) =
            (self.0.clone(), self.0.clone(), self.0.clone(), self.0.clone());
        let button = |label: &'static str| {
            div().px_3().py_1().rounded_md().bg(rgb(0x0369a1)).text_color(rgb(0xffffff)).cursor_pointer().child(label)
        };
        div()
            .flex()
            .flex_col()
            .gap_3()
            .p_4()
            .bg(rgb(0x18181b))
            .child(video().video(self.0.clone()).w(px(320.0)).h(px(180.0)))
            .child(
                div()
                    .flex()
                    .gap_2()
                    .child(button("Mute / unmute").on_click(move |_, _, _| mute.set_muted(!mute.muted())))
                    .child(button("Quieter").on_click(move |_, _, _| quieter.set_volume(quieter.volume() - 0.2)))
                    .child(button("Louder").on_click(move |_, _, _| louder.set_volume(louder.volume() + 0.2)))
                    .child(button("Pause / play").on_click(move |_, _, _| {
                        if pause.state() == blenderui::VideoState::Playing {
                            pause.pause();
                        } else {
                            pause.play();
                        }
                    })),
            )
    }
}

fn main() {
    let clip = std::env::args().nth(1).unwrap_or_else(|| {
        format!("{}/../../../tests/videos/clip_audio.mp4", env!("CARGO_MANIFEST_DIR")).replace('\\', "/")
    });
    let video = Rc::new(Video::new(&clip).expect("a video file"));
    video.set_loop(true);
    Application::new().run(move |cx| {
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds {
                origin: point(px(0.0), px(0.0)),
                size: size(px(360.0), px(300.0)),
            })),
            ..Default::default()
        };
        cx.open_window(opts, |_, cx| cx.new(|_| Player(video))).unwrap();
    });
}
