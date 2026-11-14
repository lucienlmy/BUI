//! Shared by both benchmark apps: the scene parameters and the measurement.
//!
//! A background thread waits for the warm-up, snapshots process CPU time,
//! memory and the frame counter, waits for the measured window, snapshots
//! again, prints one JSON line and exits the process. Nothing here depends on
//! the UI framework, so both apps are measured the same way. Windows and Linux.

#![allow(dead_code)]

use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

pub static FRAMES: AtomicU64 = AtomicU64::new(0);

/// Where the frame count comes from: `frame()` calls (one per render), or a
/// framework's own counter (frames it draws without rendering the view).
static FRAME_SOURCE: std::sync::OnceLock<fn() -> u64> = std::sync::OnceLock::new();

pub fn set_frame_source(f: fn() -> u64) {
    let _ = FRAME_SOURCE.set(f);
}

fn frames() -> u64 {
    FRAME_SOURCE.get().map_or_else(|| FRAMES.load(Ordering::Relaxed), |f| f())
}

pub struct Config {
    pub cells: usize,
    pub animate: bool,
    /// `--mode one`: every frame re-renders, but only cell 0 changes.
    pub one: bool,
    /// `--mode motion`: every cell's colour and opacity loop forever through
    /// the framework's own animation system (gpui's `with_animation`,
    /// blenderui's `<motion.div>`): no frame-by-frame code in the app.
    pub motion: bool,
    /// `--mode spin`: the same loop on one cell, the rest still (a spinner in
    /// a big UI: what a frame costs when little moves).
    pub spin: bool,
    pub warmup: f64,
    pub seconds: f64,
    pub width: f32,
    pub height: f32,
}

impl Config {
    pub fn from_args() -> Config {
        let args: Vec<String> = std::env::args().collect();
        let get = |k: &str, d: &str| {
            args.iter().position(|a| a == k).and_then(|i| args.get(i + 1)).cloned()
                .unwrap_or_else(|| d.to_owned())
        };
        Config {
            cells: get("--cells", "100").parse().unwrap(),
            animate: matches!(get("--mode", "anim").as_str(), "anim" | "one"),
            one: get("--mode", "anim") == "one",
            motion: get("--mode", "anim") == "motion",
            spin: get("--mode", "anim") == "spin",
            warmup: get("--warmup", "3").parse().unwrap(),
            seconds: get("--seconds", "10").parse().unwrap(),
            width: 1000.0,
            height: 800.0,
        }
    }

    /// Grid of cells filling the window: (columns, cell width, cell height, font size).
    pub fn grid(&self) -> (usize, f32, f32, f32) {
        let gap = 2.0;
        let cols = ((self.cells as f32 * self.width / self.height).sqrt().ceil() as usize).max(1);
        let rows = self.cells.div_ceil(cols);
        let w = (self.width - gap) / cols as f32 - gap;
        let h = (self.height - gap) / rows as f32 - gap;
        (cols, w.floor(), h.floor(), (h * 0.55).clamp(8.0, 14.0).floor())
    }
}

/// The colour of cell `i` at frame `t` (0xRRGGBB), the same in both apps.
pub fn cell_color(i: usize, t: u64) -> u32 {
    let h = ((i as u64 * 7 + t * 3) % 360) as f32 / 60.0;
    let (s, v) = (0.55f32, 0.75f32);
    let c = v * s;
    let x = c * (1.0 - (h % 2.0 - 1.0).abs());
    let (r, g, b) = match h as u32 {
        0 => (c, x, 0.0),
        1 => (x, c, 0.0),
        2 => (0.0, c, x),
        3 => (0.0, x, c),
        4 => (x, 0.0, c),
        _ => (c, 0.0, x),
    };
    let m = v - c;
    let byte = |f: f32| ((f + m) * 255.0).round() as u32;
    (byte(r) << 16) | (byte(g) << 8) | byte(b)
}

/// The label of cell `i` at frame `t`.
pub fn cell_label(i: usize, t: u64) -> String {
    format!("{}", (i as u64 * 13 + t) % 1000)
}

impl Config {
    /// The frame number cell `i` shows: all cells follow `t`, or only cell 0 (`--mode one`).
    pub fn t_for(&self, i: usize, t: u64) -> u64 {
        if self.one && i != 0 { 0 } else { t }
    }
}

pub fn frame() -> u64 {
    FRAMES.fetch_add(1, Ordering::Relaxed)
}

#[repr(C)]
#[derive(Default)]
pub struct MemCounters {
    cb: u32,
    page_faults: u32,
    pub peak_working_set: usize,
    pub working_set: usize,
    quota_peak_paged: usize,
    quota_paged: usize,
    quota_peak_nonpaged: usize,
    quota_nonpaged: usize,
    pagefile: usize,
    peak_pagefile: usize,
    pub private_bytes: usize,
}

use platform::cpu_time;
pub use platform::{cpu_cycles, memory, tsc_hz};

/// Windows: CPU cycles of every thread and the working set, from kernel32.
#[cfg(windows)]
mod platform {
    use std::time::{Duration, Instant};

    use super::MemCounters;

    #[repr(C)]
    #[derive(Default)]
    struct FileTime {
        lo: u32,
        hi: u32,
    }

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetCurrentProcess() -> isize;
        fn GetProcessTimes(
            p: isize,
            c: *mut FileTime,
            e: *mut FileTime,
            k: *mut FileTime,
            u: *mut FileTime,
        ) -> i32;
        fn K32GetProcessMemoryInfo(p: isize, m: *mut MemCounters, cb: u32) -> i32;
        fn QueryProcessCycleTime(p: isize, cycles: *mut u64) -> i32;
    }

    /// CPU cycles used by every thread of the process. Unlike GetProcessTimes
    /// (charged per ~15.6 ms clock tick, which aliases with a 60 Hz frame loop)
    /// this is exact; `tsc_hz` turns it into time.
    pub fn cpu_cycles() -> u64 {
        let mut c = 0u64;
        // SAFETY: the pseudo handle of the current process and a valid out pointer.
        unsafe { QueryProcessCycleTime(GetCurrentProcess(), &mut c) };
        c
    }

    /// The cycle counter's frequency, measured against the wall clock.
    pub fn tsc_hz(over: Duration) -> f64 {
        // SAFETY: rdtsc reads a counter and has no preconditions on x86_64.
        let (t0, c0) = (Instant::now(), unsafe { core::arch::x86_64::_rdtsc() });
        std::thread::sleep(over);
        // SAFETY: as above.
        let (t1, c1) = (Instant::now(), unsafe { core::arch::x86_64::_rdtsc() });
        (c1 - c0) as f64 / (t1 - t0).as_secs_f64()
    }

    /// Process CPU time (user + kernel), all threads.
    pub(super) fn cpu_time() -> Duration {
        let (mut c, mut e, mut k, mut u) = Default::default();
        // SAFETY: the pseudo handle of the current process and four valid out pointers.
        unsafe { GetProcessTimes(GetCurrentProcess(), &mut c, &mut e, &mut k, &mut u) };
        let t = |f: &FileTime| (u64::from(f.hi) << 32 | u64::from(f.lo)) * 100;
        Duration::from_nanos(t(&k) + t(&u))
    }

    pub fn memory() -> MemCounters {
        let mut m = MemCounters {
            cb: std::mem::size_of::<MemCounters>() as u32,
            ..Default::default()
        };
        // SAFETY: `m` is a writable PROCESS_MEMORY_COUNTERS_EX and `cb` gives its size.
        unsafe { K32GetProcessMemoryInfo(GetCurrentProcess(), &mut m, m.cb) };
        m
    }
}

/// Linux: `CLOCK_PROCESS_CPUTIME_ID` is the exact CPU time of every thread, in
/// nanoseconds, so "cycles" are nanoseconds and `tsc_hz` is 1e9. Memory comes
/// from `/proc/self/status` (`VmRSS`, `VmHWM`, `RssAnon`), read into a stack buffer.
#[cfg(target_os = "linux")]
mod platform {
    use std::fs::File;
    use std::io::Read;
    use std::time::Duration;

    use super::MemCounters;

    const CLOCK_PROCESS_CPUTIME_ID: i32 = 2;

    #[repr(C)]
    struct Timespec {
        sec: i64,
        nsec: i64,
    }

    unsafe extern "C" {
        fn clock_gettime(clock: i32, ts: *mut Timespec) -> i32;
    }

    pub fn cpu_cycles() -> u64 {
        let mut ts = Timespec { sec: 0, nsec: 0 };
        // SAFETY: a valid clock id and a valid out pointer.
        unsafe { clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &raw mut ts) };
        u64::try_from(ts.sec).unwrap_or(0) * 1_000_000_000 + u64::try_from(ts.nsec).unwrap_or(0)
    }

    pub fn tsc_hz(over: Duration) -> f64 {
        std::thread::sleep(over); // the warm-up, as on Windows
        1e9
    }

    pub(super) fn cpu_time() -> Duration {
        Duration::from_nanos(cpu_cycles())
    }

    pub fn memory() -> MemCounters {
        let mut buf = [0u8; 4096];
        let n = File::open("/proc/self/status")
            .and_then(|mut f| f.read(&mut buf))
            .unwrap_or(0);
        let text = str::from_utf8(&buf[..n]).unwrap_or("");
        let kb = |key: &str| {
            text.lines()
                .find_map(|l| l.strip_prefix(key))
                .and_then(|v| v.split_whitespace().next()?.parse::<usize>().ok())
                .map_or(0, |v| v * 1024)
        };
        MemCounters {
            working_set: kb("VmRSS:"),
            peak_working_set: kb("VmHWM:"),
            private_bytes: kb("RssAnon:"),
            ..MemCounters::default()
        }
    }
}

/// `--mode motion`: the two colours each cell loops between, and the loop.
pub const MOTION_FROM: u32 = 0x4772b3;
pub const MOTION_TO: u32 = 0xe06c75;
pub const MOTION_SECONDS: f32 = 1.0;
/// Whether cell `i` animates through the framework (`motion`: all; `spin`: the first).
impl Config {
    pub fn animates(&self, i: usize) -> bool {
        self.motion || (self.spin && i == 0)
    }
}

/// A cell's start offset in the loop (so the cells aren't in step).
pub fn motion_offset(i: usize) -> f32 {
    (i % 17) as f32 / 17.0
}

/// Start the measuring thread. `name` labels the JSON line.
pub fn start(name: &'static str, cfg: &Config) {
    let (warmup, secs, cells, animate) = (cfg.warmup, cfg.seconds, cfg.cells, cfg.animate);
    let mode = if cfg.spin { "spin" } else if cfg.motion { "motion" } else if cfg.one { "one" } else if animate { "anim" } else { "idle" };
    let launched = Instant::now();
    std::thread::spawn(move || {
        // time to first frame
        let first = loop {
            if frames() > 0 {
                break launched.elapsed();
            }
            if launched.elapsed() > Duration::from_secs(30) {
                break Duration::ZERO;
            }
            std::thread::sleep(Duration::from_millis(1));
        };
        let hz = tsc_hz(Duration::from_secs_f64(warmup));
        let (w0, c0, f0) = (Instant::now(), cpu_cycles(), frames());
        let t0 = cpu_time();
        let mut peak_ws = 0usize;
        let mut ws_sum = 0f64;
        let mut samples = 0u32;
        while w0.elapsed().as_secs_f64() < secs {
            std::thread::sleep(Duration::from_millis(100));
            let m = memory();
            peak_ws = peak_ws.max(m.working_set);
            ws_sum += m.working_set as f64;
            samples += 1;
        }
        let (wall, frames) = (w0.elapsed(), frames() - f0);
        let cpu = Duration::from_secs_f64((cpu_cycles() - c0) as f64 / hz);
        let _tick_based = cpu_time() - t0; // GetProcessTimes, kept for comparison when debugging
        let m = memory();
        let mb = |b: f64| b / (1024.0 * 1024.0);
        println!(
            "{{\"app\":\"{name}\",\"cells\":{cells},\"mode\":\"{}\",\"fps\":{:.1},\"cpu_pct\":{:.1},\
             \"cpu_ms_per_frame\":{:.3},\"ws_mb\":{:.1},\"peak_ws_mb\":{:.1},\"private_mb\":{:.1},\
             \"first_frame_ms\":{:.0}}}",
            mode,
            frames as f64 / wall.as_secs_f64(),
            cpu.as_secs_f64() / wall.as_secs_f64() * 100.0,
            if frames > 0 { cpu.as_secs_f64() * 1000.0 / frames as f64 } else { 0.0 },
            mb(ws_sum / samples.max(1) as f64),
            mb(peak_ws as f64),
            mb(m.private_bytes as f64),
            first.as_secs_f64() * 1000.0,
        );
        std::process::exit(0);
    });
}

#[cfg(test)]
mod tests {
    use std::hint::black_box;
    use std::time::Duration;

    use super::{cpu_cycles, cpu_time, memory, tsc_hz};

    #[test]
    fn cpu_time_grows_with_work() {
        let (c0, t0) = (cpu_cycles(), cpu_time());
        let sum = (0..20_000_000u64).fold(0u64, |a, i| black_box(a.wrapping_add(i)));
        black_box(sum);
        assert!(cpu_cycles() > c0, "cycles did not grow");
        assert!(cpu_time() > t0, "CPU time did not grow");
    }

    #[test]
    fn cycle_rate_is_positive() {
        let hz = tsc_hz(Duration::from_millis(10));
        assert!(hz.is_finite() && hz > 1e6, "rate {hz}");
    }

    #[test]
    fn memory_reports_resident_set() {
        let m = memory();
        assert!(m.working_set > 0, "working set is 0");
        assert!(m.peak_working_set >= m.working_set, "peak below current");
        assert!(m.private_bytes > 0, "private bytes is 0");
    }
}
