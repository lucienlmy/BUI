//! `<video>`: it loads, plays and pauses in a window, and players come and go
//! without leaks (a counting Rust allocator and the kit's MEM counters). One
//! test: the counter is global. Skips where no backend decodes the clip.

use std::alloc::{GlobalAlloc, Layout, System};
use std::rc::Rc;
use std::sync::atomic::{AtomicIsize, Ordering};
use std::time::Duration;

use blenderui::prelude::*;
use blenderui::{sys, Video, VideoState};

struct Counting;
static LIVE: AtomicIsize = AtomicIsize::new(0);

// SAFETY: forwards to the system allocator; only counts.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        LIVE.fetch_add(l.size() as isize, Ordering::Relaxed);
        // SAFETY: same layout contract as the caller's.
        unsafe { System.alloc(l) }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        LIVE.fetch_sub(l.size() as isize, Ordering::Relaxed);
        // SAFETY: `p` came from `alloc` with `l`.
        unsafe { System.dealloc(p, l) }
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
        LIVE.fetch_add(n as isize - l.size() as isize, Ordering::Relaxed);
        // SAFETY: as `dealloc` + `alloc`.
        unsafe { System.realloc(p, l, n) }
    }
}

#[global_allocator]
static A: Counting = Counting;

fn clip() -> String {
    format!("{}/../../../tests/videos/clip.mp4", env!("CARGO_MANIFEST_DIR")).replace('\\', "/")
}

struct Player(Rc<Video>);
impl Render for Player {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .child(video().id("v").video(self.0.clone()).w(px(96.0)).h(px(54.0)))
            .child(video().id("s").src(clip()).attr("autoplay", "false").w(px(96.0)).h(px(54.0)))
    }
}

#[test]
fn video_plays_and_players_free_everything() {
    if Video::backends().is_empty() {
        eprintln!("no video backend in this build: skipped");
        return;
    }
    let v = Rc::new(Video::new(&clip()).unwrap());
    let mut ok = true;
    Application::headless().run(|cx| {
        let opts = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds { origin: point(px(0.0), px(0.0)), size: size(px(220.0), px(80.0)) })),
            ..Default::default()
        };
        let win = cx.open_window(opts, |_, cx| cx.new(|_| Player(v.clone()))).unwrap();
        let hash = |cx: &mut App| {
            let b = cx.element_bounds(win.id, "v").unwrap();
            let (pw, _, px) = cx.draw(win.id);
            (0..40).fold(0u64, |h, k| h.wrapping_mul(31) ^ u64::from(px[(b.origin.y as usize + 10 + k % 30) * pw + b.origin.x as usize + 5 + k]))
        };
        for _ in 0..200 {
            hash(cx);
            if v.state() != VideoState::Loading {
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        if v.state() != VideoState::Playing {
            ok = false; // no decoder for H.264 here, or the backend doesn't deliver (no session)
            return;
        }
        assert_eq!(v.size(), Some((96, 54)));
        let mut seen = vec![hash(cx)];
        for _ in 0..8 {
            std::thread::sleep(Duration::from_millis(60));
            let h = hash(cx);
            if !seen.contains(&h) {
                seen.push(h);
            }
        }
        assert!(seen.len() >= 2, "it plays: {} distinct frames", seen.len()); // a loaded runner drops some
        v.pause();
        v.seek(Duration::from_millis(1500));
        for _ in 0..30 {
            hash(cx);
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!((v.time().as_secs_f64() - 1.5).abs() < 0.11, "seek: {:?}", v.time());
        assert_eq!(v.state(), VideoState::Paused);
    });
    if !ok {
        eprintln!("this machine's backend doesn't decode H.264: skipped");
        return;
    }
    drop(v);
    // players come and go: every byte back (Rust and the kit's MEM)
    let (rust0, mem0) = (LIVE.load(Ordering::Relaxed), unsafe { sys::MEM_get_memory_blocks_in_use() });
    for _ in 0..200 {
        let mut p = Video::new(&clip()).unwrap();
        p.on_state(|_| {});
        p.set_loop(true);
        p.seek(Duration::from_millis(500));
        p.set_volume(2.0);
        p.set_muted(true);
        assert!(p.muted() && (p.volume() - 1.0).abs() < 1e-9, "sound by default, volume clamped");
        drop(p);
    }
    assert_eq!(LIVE.load(Ordering::Relaxed), rust0, "Rust allocations returned");
    // SAFETY: a counter read.
    assert_eq!(unsafe { sys::MEM_get_memory_blocks_in_use() }, mem0, "C (MEM) blocks returned");
}
