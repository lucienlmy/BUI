//! `bui-runtime`: blenderui with an embedded QuickJS engine.
//!
//! A standalone app is this executable with a JavaScript bundle appended
//! (see `bui build` in bindings/node): no Node.js, no Bun, no browser. The
//! JavaScript packages (`@blender-ui/native`, `@blender-ui/react`) run on it
//! unchanged: `prelude.js` gives them the addon's API (`__BUI_NATIVE__`),
//! timers, `console` and `process`.
//!
//! JavaScript runs on the UI thread, as in a browser. A "pump" runs it: it
//! delivers window events and request results, runs due timers and promise
//! jobs, then applies what JavaScript asked for (open a window, commit ops,
//! request a frame ...) through `blenderui-jsview`, shared with the Node.js
//! addon. Events and timers schedule the next pump; nothing runs when idle.
//!
//!   bui-runtime app.js [args]       run a script (development)
//!   app.exe [args]                  an app built by `bui build`
//!   BUI_HEADLESS=1 / --headless     off-screen windows (tests)
//!
//! SPDX-License-Identifier: GPL-2.0-or-later

use std::cell::Cell;
use std::sync::{Arc, mpsc};
use std::time::{Duration, Instant};

use blenderui::prelude::*;
use blenderui_jsview as view;
use blenderui::motion::Any;
use rquickjs::function::Opt;
use rquickjs::{Array, Context, Ctx, Exception, Function, Object, Runtime, Type, TypedArray, Value};

/// The end of an app executable: `MAGIC`, then the bundle's length (u64 LE).
const MAGIC: &[u8; 8] = b"BUIAPP01";
const PRELUDE: &str = include_str!("prelude.js");

// ==========================================================================
// State shared between the host functions and the pump (UI thread only).
// Each cell's value is moved out while used: no RefCell.

/// What JavaScript asked for, applied by the pump with `&mut App`.
enum Command {
    Open(u32, view::WindowConfig),
    Commit(u32, Vec<i32>, Vec<String>, bool, Vec<Option<view::motion::MotionProp>>),
    Frame(u32),
    Close(u32),
    Slug(bool),
    Quit,
    Screenshot(u32, u32),
    Bounds(u32, u32, u32),
    Click(u32, f64, f64),
}

/// A finished request (screenshot, bounds) for its promise.
enum Answer {
    Screenshot(u32, u32, Vec<u8>),
    Bounds(Option<view::Rect>),
    Error(String),
}

struct Js {
    runtime: Runtime,
    context: Context,
}

thread_local! {
    static JS: Cell<Option<Js>> = const { Cell::new(None) };
    static COMMANDS: Cell<Vec<Command>> = const { Cell::new(Vec::new()) };
    static EVENTS: Cell<Vec<view::Event>> = const { Cell::new(Vec::new()) };
    static ANSWERS: Cell<Vec<(u32, Answer)>> = const { Cell::new(Vec::new()) };
    static WAKE: Cell<Option<Wake>> = const { Cell::new(None) };
    static QUIT: Cell<bool> = const { Cell::new(false) };
    static SCHEDULED: Cell<bool> = const { Cell::new(false) };
    static HEADLESS: Cell<bool> = const { Cell::new(false) };
}

fn push<T>(cell: &'static std::thread::LocalKey<Cell<Vec<T>>>, item: T) {
    cell.with(|c| {
        let mut v = c.take();
        v.push(item);
        c.set(v);
    });
}

fn take<T>(cell: &'static std::thread::LocalKey<Cell<Vec<T>>>) -> Vec<T> {
    cell.with(|c| c.take())
}

fn command(c: Command) {
    push(&COMMANDS, c);
}

// ==========================================================================
// Waking the pump

/// How the next pump gets scheduled: through the native loop (and a timer
/// thread for deadlines), or, headless, by the loop in `main` itself.
#[derive(Clone)]
struct Wake {
    ui: Option<UiSender>,
    timer: Option<mpsc::Sender<Option<Instant>>>,
}

/// Run a pump soon (native: once, however many events ask for it).
fn wake() {
    let w = WAKE.with(|c| {
        let w = c.take();
        c.set(w.clone());
        w
    });
    if let Some(ui) = w.and_then(|w| w.ui) {
        if !SCHEDULED.with(|s| s.replace(true)) {
            ui.send(|cx| { pump(cx); });
        }
    }
}

/// A thread that sends a pump to the UI thread at the next timer deadline.
fn timer_thread(ui: UiSender) -> mpsc::Sender<Option<Instant>> {
    let (tx, rx) = mpsc::channel::<Option<Instant>>();
    std::thread::Builder::new().name("bui-timers".into()).spawn(move || {
        let mut next: Option<Instant> = None;
        loop {
            let got = match next {
                Some(t) => {
                    let now = Instant::now();
                    if t <= now {
                        next = None;
                        ui.send(|cx| { pump(cx); });
                        continue;
                    }
                    rx.recv_timeout(t - now)
                }
                None => rx.recv().map_err(|_| mpsc::RecvTimeoutError::Disconnected),
            };
            match got {
                Ok(t) => next = t,
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            }
        }
    }).expect("starting the timer thread");
    tx
}

/// Window events, from the UI thread.
fn sink() -> view::Sink {
    Arc::new(|e: view::Event| {
        push(&EVENTS, e);
        wake();
    })
}

// ==========================================================================
// The pump

static START: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();

fn now_ms() -> f64 {
    START.get_or_init(Instant::now).elapsed().as_secs_f64() * 1000.0
}

/// Run JavaScript until it settles, apply what it asked for; returns the
/// next timer deadline (ms on the `now_ms` clock).
fn pump(cx: &mut App) -> Option<f64> {
    SCHEDULED.with(|s| s.set(false));
    let Some(js) = JS.with(|c| c.take()) else { return None };
    let mut next = None;
    // a bounded number of rounds, so the native loop keeps running (input,
    // painting) even if JavaScript keeps itself busy
    for _ in 0..64 {
        let events = take(&EVENTS);
        let answers = take(&ANSWERS);
        js.context.with(|ctx| {
            let g = ctx.globals();
            if !events.is_empty() {
                if let Ok(dispatch) = g.get::<_, Function>("__bui_dispatch") {
                    for e in events {
                        if let Ok(o) = event_object(&ctx, e) {
                            report(&ctx, dispatch.call::<_, ()>((o,)));
                        }
                    }
                }
            }
            if !answers.is_empty() {
                if let Ok(resolve) = g.get::<_, Function>("__bui_resolve") {
                    for (id, a) in answers {
                        let r = answer_value(&ctx, a).and_then(|(ok, v)| resolve.call::<_, ()>((id, ok, v)));
                        report(&ctx, r);
                    }
                }
            }
            if let Ok(tick) = g.get::<_, Function>("__bui_tick") {
                let t: rquickjs::Result<f64> = tick.call(());
                next = match t {
                    Ok(t) if t >= 0.0 => Some(t),
                    Ok(_) => None,
                    Err(e) => {
                        report::<()>(&ctx, Err(e));
                        None
                    }
                };
            }
        });
        run_jobs(&js);
        let commands = take(&COMMANDS);
        let busy = !commands.is_empty();
        for c in commands {
            apply(cx, c);
        }
        if HEADLESS.with(|h| h.get()) {
            cx.run_pending(); // render: frames, events
        }
        let more = busy
            || EVENTS.with(|c| { let v = c.take(); let n = !v.is_empty(); c.set(v); n })
            || ANSWERS.with(|c| { let v = c.take(); let n = !v.is_empty(); c.set(v); n })
            || next.is_some_and(|t| t <= now_ms());
        if !more || QUIT.with(|q| q.get()) {
            break;
        }
    }
    let busy = js.context.with(|ctx| {
        ctx.globals().get::<_, Function>("__bui_busy").and_then(|f| f.call::<_, bool>(())).unwrap_or(false)
    });
    JS.with(|c| c.set(Some(js)));
    // nothing left to wait for: no window, no timer, no request in flight
    if view::window_count(cx) == 0 && next.is_none() && !busy && !QUIT.with(|q| q.get()) {
        command(Command::Quit);
        for c in take(&COMMANDS) {
            apply(cx, c);
        }
    }
    // the next timer
    let w = WAKE.with(|c| {
        let w = c.take();
        c.set(w.clone());
        w
    });
    if let Some(timer) = w.and_then(|w| w.timer) {
        let at = next.map(|t| START.get_or_init(Instant::now).checked_add(Duration::from_secs_f64(t / 1000.0)));
        let _ = timer.send(at.flatten());
    }
    next
}

fn run_jobs(js: &Js) {
    loop {
        match js.runtime.execute_pending_job() {
            Ok(true) => {}
            Ok(false) => break,
            Err(e) => {
                e.0.with(|ctx| {
                    let v = ctx.catch();
                    eprintln!("{}", describe(&v));
                });
            }
        }
    }
}

fn apply(cx: &mut App, c: Command) {
    match c {
        Command::Open(id, config) => view::open_window(cx, id, config, sink()),
        Command::Commit(w, ops, strings, animated, props) => view::commit(cx, w, &ops, &strings, props, animated),
        Command::Frame(w) => view::request_frame(cx, w),
        Command::Close(w) => view::close_window(cx, w),
        Command::Slug(on) => {
            blenderui::set_slug(on);
            cx.notify_all();
        }
        Command::Quit => {
            if !QUIT.with(|q| q.replace(true)) {
                view::close_all(cx);
                if !HEADLESS.with(|h| h.get()) {
                    // done: exit now, as Node and Bun do. Tearing the app down
                    // (GPU device, windows) can take seconds and nothing needs it.
                    exit(0);
                }
                cx.quit();
            }
        }
        Command::Screenshot(w, id) => {
            let a = match view::screenshot(cx, w) {
                Some((width, height, data)) => Answer::Screenshot(width, height, data),
                None => Answer::Error("no such window".into()),
            };
            push(&ANSWERS, (id, a));
        }
        Command::Bounds(w, node, id) => push(&ANSWERS, (id, Answer::Bounds(view::bounds(cx, w, node)))),
        Command::Click(w, x, y) => view::simulate_click(cx, w, x, y),
    }
}

fn event_object<'js>(ctx: &Ctx<'js>, e: view::Event) -> rquickjs::Result<Object<'js>> {
    let o = Object::new(ctx.clone())?;
    o.set("kind", e.kind)?;
    o.set("window", e.window)?;
    o.set("target", e.target)?;
    o.set("x", e.x)?;
    o.set("y", e.y)?;
    o.set("button", e.button)?;
    o.set("clickCount", e.click_count)?;
    o.set("delta", e.delta)?;
    o.set("key", e.key)?;
    o.set("keyChar", e.key_char)?;
    o.set("value", e.value)?;
    o.set("checked", e.checked)?;
    o.set("number", e.number)?;
    o.set("shift", e.shift)?;
    o.set("ctrl", e.ctrl)?;
    o.set("alt", e.alt)?;
    Ok(o)
}

fn answer_value<'js>(ctx: &Ctx<'js>, a: Answer) -> rquickjs::Result<(bool, Value<'js>)> {
    Ok(match a {
        Answer::Screenshot(w, h, data) => {
            let o = Object::new(ctx.clone())?;
            o.set("width", w)?;
            o.set("height", h)?;
            o.set("data", TypedArray::<u8>::new(ctx.clone(), data)?)?;
            (true, o.into_value())
        }
        Answer::Bounds(Some(r)) => {
            let o = Object::new(ctx.clone())?;
            o.set("x", r.x)?;
            o.set("y", r.y)?;
            o.set("width", r.width)?;
            o.set("height", r.height)?;
            (true, o.into_value())
        }
        Answer::Bounds(None) => (true, Value::new_null(ctx.clone())),
        Answer::Error(m) => (false, rquickjs::String::from_str(ctx.clone(), &m)?.into_value()),
    })
}

/// Print an uncaught JavaScript error.
fn report<T>(ctx: &Ctx<'_>, r: rquickjs::Result<T>) {
    if let Err(e) = r {
        if matches!(e, rquickjs::Error::Exception) {
            eprintln!("{}", describe(&ctx.catch()));
        } else {
            eprintln!("bui-runtime: {e}");
        }
    }
}

fn describe(v: &Value<'_>) -> String {
    if let Some(ex) = v.as_exception() {
        let msg = ex.message().unwrap_or_default();
        let stack = ex.stack().unwrap_or_default();
        return format!("Uncaught {msg}\n{stack}");
    }
    match v.get::<String>() {
        Ok(s) => format!("Uncaught {s}"),
        Err(_) => "Uncaught exception".into(),
    }
}

// ==========================================================================
// Host functions (`host` in prelude.js)

/// The window options object of `index.d.ts`.
/// How deep a Motion prop's value may nest.
const MOTION_DEPTH: u32 = 12;

/// A Motion prop's value (`{ x: 100, ease: 'linear', onComplete () {} }`, a motion value by its
/// handle ...) as the engine's `Any`: read once, here.
fn motion_any(v: &Value<'_>, depth: u32) -> rquickjs::Result<Any> {
    if depth == 0 {
        return Err(rquickjs::Error::new_from_js_message("value", "Motion prop", "nested too deeply"));
    }
    Ok(match v.type_of() {
        Type::Bool => Any::Bool(v.as_bool().unwrap_or(false)),
        Type::Int | Type::Float => Any::Num(v.as_number().unwrap_or(0.0)),
        Type::String => Any::Str(v.as_string().map(|s| s.to_string()).transpose()?.unwrap_or_default()),
        Type::Function | Type::Constructor => Any::Func,
        Type::Array => {
            let mut list = Vec::new();
            if let Some(a) = v.as_array() {
                for item in a.iter::<Value>() {
                    list.push(motion_any(&item?, depth - 1)?);
                }
            }
            Any::List(list)
        }
        Type::Object => {
            let Some(o) = v.as_object() else { return Ok(Any::Null) };
            // a motion value: its handle (what the Node addon finds the same way)
            if let Some(id) = o.get::<_, Option<u32>>("__buiMotionValue")? {
                return Ok(Any::Motion(id));
            }
            let mut map = Vec::new();
            for kv in o.props::<String, Value>() {
                let (k, x) = kv?;
                map.push((k, motion_any(&x, depth - 1)?));
            }
            Any::Map(map)
        }
        _ => Any::Null,
    })
}

/// Commit's Motion props `[which, value, which, value ...]`, and the first error among them. A
/// prop that can't be read becomes the removal of that prop: the batch refers to the list by
/// position, and is applied either way (the error is reported after it).
fn motion_props(list: Option<Array<'_>>) -> rquickjs::Result<(Vec<Option<view::motion::MotionProp>>, Option<String>)> {
    let Some(list) = list else { return Ok((Vec::new(), None)) };
    let items: Vec<Value> = list.iter::<Value>().collect::<rquickjs::Result<_>>()?;
    let mut out = Vec::with_capacity(items.len() / 2);
    let mut first = None;
    for pair in items.chunks_exact(2) {
        let which = pair[0].as_number().map_or(-1, |n| n as i32);
        let value = motion_any(&pair[1], MOTION_DEPTH)?;
        out.push(Some(match view::motion::MotionProp::parse(which, &value) {
            Ok(p) => p,
            Err(e) => {
                first.get_or_insert(e);
                view::motion::MotionProp::Clear(which)
            }
        }));
    }
    Ok((out, first))
}

fn window_config(c: &Object<'_>) -> rquickjs::Result<view::WindowConfig> {
    let layer_shell = c
        .get::<_, Option<Object>>("layerShell")?
        .map(|l| -> rquickjs::Result<view::LayerShellConfig> {
            Ok(view::LayerShellConfig {
                layer: l.get("layer")?,
                anchor: l.get("anchor")?,
                exclusive_zone: l.get("exclusiveZone")?,
                keyboard: l.get("keyboard")?,
            })
        })
        .transpose()?;
    Ok(view::WindowConfig {
        title: c.get("title")?,
        width: c.get("width")?,
        height: c.get("height")?,
        resizable: c.get("resizable")?,
        gpu: c.get("gpu")?,
        transparent: c.get("transparent")?,
        always_on_top: c.get("alwaysOnTop")?,
        frame: c.get("frame")?,
        layer_shell,
    })
}

fn host_object<'js>(ctx: &Ctx<'js>, argv: Vec<String>) -> rquickjs::Result<Object<'js>> {
    let h = Object::new(ctx.clone())?;
    macro_rules! f {
        ($name:literal, $f:expr) => {
            h.set($name, Function::new(ctx.clone(), $f)?)?;
        };
    }
    f!("print", |level: u32, msg: String| {
        if level == 0 {
            println!("{msg}");
        } else {
            eprintln!("{msg}");
        }
    });
    f!("now", now_ms);
    f!("headless", || HEADLESS.with(|h| h.get()));
    f!("platform", || match std::env::consts::OS {
        "windows" => "win32".to_string(),
        "macos" => "darwin".to_string(),
        os => os.to_string(),
    });
    f!("arch", || match std::env::consts::ARCH {
        "x86_64" => "x64".to_string(),
        "aarch64" => "arm64".to_string(),
        a => a.to_string(),
    });
    let args = argv.clone();
    f!("argv", move || args.clone());
    f!("env", |ctx: Ctx<'js>| -> rquickjs::Result<Object<'js>> {
        let o = Object::new(ctx)?;
        for (k, v) in std::env::vars() {
            o.set(k, v)?;
        }
        Ok(o)
    });
    f!("exit", |code: i32| -> () {
        exit(code);
    });
    f!("rss", rss);
    f!("cpuCycles", view::process_cpu_cycles);
    f!("tsc", view::timestamp_counter);
    f!("openWindow", |id: u32, c: Object<'js>| -> rquickjs::Result<()> {
        command(Command::Open(id, window_config(&c)?));
        Ok(())
    });
    f!("commit", |ctx: Ctx<'js>, w: u32, ops: TypedArray<'js, i32>, strings: Vec<String>, animated: bool,
                  motion: Opt<Option<Array<'js>>>| -> rquickjs::Result<()> {
        // SAFETY: copied right away, before JavaScript can detach or resize the buffer
        let ops = unsafe { ops.as_slice() }.to_vec();
        // absent, `undefined` and `null` all mean no motion props
        let (props, bad) = motion_props(motion.0.flatten())?;
        command(Command::Commit(w, ops, strings, animated, props));
        // the batch is applied; the prop that couldn't be read is reported
        bad.map_or(Ok(()), |e| Err(Exception::throw_message(&ctx, &e)))
    });
    f!("requestFrame", |w: u32| command(Command::Frame(w)));
    f!("closeWindow", |w: u32| command(Command::Close(w)));
    f!("setSlug", |on: bool| command(Command::Slug(on)));
    f!("quit", || command(Command::Quit));
    f!("screenshot", |w: u32, id: u32| command(Command::Screenshot(w, id)));
    f!("bounds", |w: u32, node: u32, id: u32| command(Command::Bounds(w, node, id)));
    f!("simulateClick", |w: u32, x: f64, y: f64| command(Command::Click(w, x, y)));
    Ok(h)
}

/// Resident memory (the working set on Windows), bytes.
fn rss() -> f64 {
    #[cfg(windows)]
    {
        #[repr(C)]
        #[derive(Default)]
        struct MemCounters {
            cb: u32,
            page_faults: u32,
            peak_working_set: usize,
            working_set: usize,
            quota_peak_paged: usize,
            quota_paged: usize,
            quota_peak_nonpaged: usize,
            quota_nonpaged: usize,
            pagefile: usize,
            peak_pagefile: usize,
        }
        #[link(name = "kernel32")]
        unsafe extern "system" {
            fn GetCurrentProcess() -> isize;
            fn K32GetProcessMemoryInfo(p: isize, m: *mut MemCounters, cb: u32) -> i32;
        }
        let mut m = MemCounters { cb: std::mem::size_of::<MemCounters>() as u32, ..Default::default() };
        // SAFETY: the current-process pseudo handle and a sized out struct.
        unsafe { K32GetProcessMemoryInfo(GetCurrentProcess(), &mut m, m.cb) };
        m.working_set as f64
    }
    #[cfg(not(windows))]
    {
        0.0
    }
}

// ==========================================================================
// Startup

/// The app's JavaScript: the bundle appended to this executable, or the
/// script named on the command line. Returns (name, source, script args).
fn load() -> Result<(String, Vec<u8>, Vec<String>), String> {
    let mut args: Vec<String> = std::env::args().skip(1).filter(|a| a != "--headless").collect();
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    if let Ok(bytes) = std::fs::read(&exe) {
        if bytes.len() > 16 && &bytes[bytes.len() - 16..bytes.len() - 8] == MAGIC {
            let n = u64::from_le_bytes(bytes[bytes.len() - 8..].try_into().unwrap()) as usize;
            if n <= bytes.len() - 16 {
                let start = bytes.len() - 16 - n;
                let name = exe.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
                return Ok((name, bytes[start..bytes.len() - 16].to_vec(), args));
            }
        }
    }
    if args.is_empty() {
        return Err("usage: bui-runtime <app.js> [args]   (or build an app with `bui build`)".into());
    }
    let path = args.remove(0);
    let src = std::fs::read(&path).map_err(|e| format!("{path}: {e}"))?;
    Ok((path, src, args))
}

fn start(cx: &mut App, name: String, source: Vec<u8>, args: Vec<String>) {
    let runtime = Runtime::new().expect("QuickJS runtime");
    runtime.set_max_stack_size(48 << 20);
    let context = Context::full(&runtime).expect("QuickJS context");
    view::init(cx, sink());
    let exe = std::env::current_exe().map(|p| p.to_string_lossy().into_owned()).unwrap_or_default();
    let mut argv = vec![exe, name.clone()];
    argv.extend(args);
    let ok = context.with(|ctx| {
        let host = match host_object(&ctx, argv) {
            Ok(h) => h,
            Err(e) => {
                report::<()>(&ctx, Err(e));
                return false;
            }
        };
        let prelude: rquickjs::Result<Function> = ctx.eval(PRELUDE);
        let r = prelude.and_then(|p| p.call::<_, ()>((host,)));
        if r.is_err() {
            report(&ctx, r);
            return false;
        }
        let mut opts = rquickjs::context::EvalOptions::default();
        opts.filename = Some(name.clone());
        opts.strict = false;
        let r: rquickjs::Result<()> = ctx.eval_with_options(source, opts);
        if r.is_err() {
            report(&ctx, r);
            return false;
        }
        true
    });
    JS.with(|c| c.set(Some(Js { runtime, context })));
    if !ok {
        std::process::exit(1);
    }
}

fn main() {
    let (name, source, args) = match load() {
        Ok(x) => x,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(2);
        }
    };
    let headless = std::env::args().any(|a| a == "--headless")
        || std::env::var("BUI_HEADLESS").is_ok_and(|v| v == "1")
        || !blenderui::native_windows();
    START.get_or_init(Instant::now);
    // a large stack: JavaScript recursion runs on the C stack
    let ui = std::thread::Builder::new().name("bui-main".into()).stack_size(64 << 20).spawn(move || {
        HEADLESS.with(|h| h.set(headless));
        if headless {
            Application::headless().run(move |cx| {
                WAKE.with(|c| c.set(Some(Wake { ui: None, timer: None })));
                start(cx, name, source, args);
                loop {
                    let next = pump(cx);
                    if QUIT.with(|q| q.get()) {
                        break;
                    }
                    let pending = EVENTS.with(|c| { let v = c.take(); let n = !v.is_empty(); c.set(v); n });
                    if pending {
                        continue;
                    }
                    match next {
                        Some(t) => {
                            let wait = t - now_ms();
                            if wait > 0.0 {
                                std::thread::sleep(Duration::from_secs_f64(wait / 1000.0));
                            }
                        }
                        // headless, only timers can bring new work: none, and no
                        // window left means done; windows alone keep it waiting
                        None if view::window_count(cx) == 0 => break,
                        None => std::thread::sleep(Duration::from_millis(5)),
                    }
                }
            });
        } else {
            Application::new().keep_alive(true).run(move |cx| {
                let ui = cx.ui_sender();
                WAKE.with(|c| c.set(Some(Wake { ui: Some(ui.clone()), timer: Some(timer_thread(ui)) })));
                start(cx, name, source, args);
                pump(cx);
            });
        }
    }).expect("starting the UI thread");
    let _ = ui.join();
}

/// End the process with `code`, flushing what JavaScript printed.
fn exit(code: i32) -> ! {
    use std::io::Write;
    let _ = std::io::stdout().flush();
    let _ = std::io::stderr().flush();
    std::process::exit(code)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Motion props as the JavaScript runtime hands them to `commit`: `[which, value, ...]`.
    fn props_of(js: &str) -> (usize, Option<String>, Vec<String>) {
        let rt = Runtime::new().expect("runtime");
        let cx = Context::full(&rt).expect("context");
        cx.with(|ctx| {
            let list: Array = ctx.eval(js).expect("a list");
            let (props, bad) = motion_props(Some(list)).expect("read");
            let kinds = props.iter().flatten().map(|p| format!("{p:?}").split('(').next().unwrap_or("").to_owned()).collect();
            (props.len(), bad, kinds)
        })
    }

    #[test]
    fn motion_props_are_forwarded() {
        // drag "x", constraints, a whileHover target with a value and a motion value by handle
        let (n, bad, kinds) = props_of("[12, 'x', 13, { left: 0, right: 100 }, 3, { width: 80, ease: 'linear' }, 7, { x: { __buiMotionValue: 4 } }]");
        assert_eq!((n, bad), (4, None));
        assert_eq!(kinds, ["Drag", "DragConstraints", "Target", "Style"]);
    }

    /// `commit` is called with `undefined` for no motion props (the prelude always passes the argument).
    #[test]
    fn commit_takes_undefined_for_no_motion_props() {
        let rt = Runtime::new().expect("runtime");
        let cx = Context::full(&rt).expect("context");
        cx.with(|ctx| {
            let f = Function::new(ctx.clone(), |motion: Opt<Option<Array<'_>>>| motion.0.flatten().is_some()).expect("fn");
            ctx.globals().set("f", f).expect("global");
            let got: Vec<bool> = ctx.eval("[f(), f(undefined), f(null), f([])]").expect("calls");
            assert_eq!(got, [false, false, false, true]);
        });
    }

    #[test]
    fn a_prop_that_cannot_be_read_still_applies_the_rest() {
        let (n, bad, kinds) = props_of("[12, 'diagonal', 14, 0.5]");
        assert_eq!(n, 2, "positions are kept: the batch refers to them");
        assert!(bad.is_some_and(|e| e.contains("drag")), "the first error is reported");
        assert_eq!(kinds, ["Clear", "DragElastic"], "the bad one becomes a removal; the other is kept");
    }
}
