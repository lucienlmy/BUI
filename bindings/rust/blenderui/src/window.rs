//! Application, windows, entities and event dispatch, shaped like gpui:
//!
//! ```ignore
//! Application::new().run(|cx: &mut App| {
//!     cx.open_window(WindowOptions::default(), |_, cx| cx.new(|_| Counter { n: 0 })).unwrap();
//! });
//! ```

use std::any::{Any, TypeId};
use std::cell::Cell;
use std::collections::{HashMap, VecDeque};
use std::ffi::{c_char, c_double, c_int, c_void, CStr, CString};
use std::marker::PhantomData;
use std::ops::{Deref, DerefMut};

use crate::element::{AnyElement, IntoElement, Listeners, SharedString};
use crate::events::*;
use crate::geometry::{point, px, size, Bounds, Pixels, Size};
use crate::layout::{id_key, layout, Content, Frame, Key, LayoutCaches, UiState, ROOT_KEY};
use crate::prof::{self, span, Phase};
use crate::style::AppRegion;
use crate::window_kind::{WindowControlArea, WindowDecorations, WindowKind};
use crate::paint::{paint_frame, Painter};
use crate::selection::{Cx, Selector};
use crate::style::KeySet;
use crate::widgets::{WKind, Widgets};
use blenderui_sys as sys;

// ==========================================================================
// Entities

/// State owned by the app (gpui's `Entity<T>`): a `Copy` handle (slot index
/// and generation) into the app's entity slots. No reference counting.
pub struct Entity<T> {
    id: u32,
    generation: u32,
    _t: PhantomData<fn() -> T>,
}

impl<T> Clone for Entity<T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for Entity<T> {}

/// One entity's storage. The value is moved out while it is being updated.
struct Slot {
    generation: u32,
    value: Option<Box<dyn Any>>,
}

impl<T: 'static> Entity<T> {
    /// Read the value.
    pub fn read<'a>(&self, cx: &'a App) -> &'a T {
        let slot = &cx.entities[self.id as usize];
        assert!(slot.generation == self.generation, "entity was released");
        slot.value.as_ref().expect("entity is being built or updated")
            .downcast_ref::<T>().expect("entity type")
    }

    /// Mutate the value with a [`Context`] for it. The value is taken out of
    /// its slot meanwhile, so `cx` stays fully usable (except for updating
    /// this same entity again, which panics).
    pub fn update<R>(&self, cx: &mut App, f: impl FnOnce(&mut T, &mut Context<T>) -> R) -> R {
        let slot = &mut cx.entities[self.id as usize];
        assert!(slot.generation == self.generation, "entity was released");
        let mut value = slot.value.take().expect("entity is being built or already being updated");
        let r = {
            let v = value.downcast_mut::<T>().expect("entity type");
            let mut ctx = Context { app: cx, entity: *self };
            f(v, &mut ctx)
        };
        // restore the value unless the callback released the entity (a window it closed and freed with
        // `run_pending`): its handles are dead then, and the value goes with this scope
        let slot = &mut cx.entities[self.id as usize];
        if slot.generation == self.generation {
            slot.value = Some(value);
        }
        r
    }

    /// Same entity?
    pub fn entity_id(&self) -> usize {
        self.id as usize
    }

    pub fn downgrade(&self) -> WeakEntity<T> {
        WeakEntity { entity: *self }
    }
}

/// A handle that may outlive its entity (gpui's `WeakEntity<T>`).
pub struct WeakEntity<T> {
    entity: Entity<T>,
}

impl<T> Clone for WeakEntity<T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for WeakEntity<T> {}

impl<T: 'static> WeakEntity<T> {
    /// The entity, if it hasn't been released.
    pub fn upgrade_in(&self, cx: &App) -> Option<Entity<T>> {
        let slot = cx.entities.get(self.entity.id as usize)?;
        (slot.generation == self.entity.generation).then_some(self.entity)
    }

    /// Update if still alive.
    pub fn update<R>(&self, cx: &mut App, f: impl FnOnce(&mut T, &mut Context<T>) -> R) -> Option<R> {
        self.upgrade_in(cx).map(|e| e.update(cx, f))
    }

    /// An element listener that updates this entity (what [`Context::listener`] makes), for code
    /// that builds elements away from `render`'s context: a screen's editor factories, say.
    pub fn listener<E: ?Sized>(
        self,
        f: impl Fn(&mut T, &E, &mut Window, &mut Context<T>) + 'static,
    ) -> impl Fn(&E, &mut Window, &mut App) + 'static {
        move |e: &E, window: &mut Window, cx: &mut App| {
            self.update(cx, |this, cx| f(this, e, window, cx));
        }
    }
}

/// The app, seen from inside an entity (gpui's `Context<T>`); derefs to [`App`].
pub struct Context<'a, T> {
    app: &'a mut App,
    entity: Entity<T>,
}

impl<T> Deref for Context<'_, T> {
    type Target = App;
    fn deref(&self) -> &App {
        self.app
    }
}
impl<T> DerefMut for Context<'_, T> {
    fn deref_mut(&mut self) -> &mut App {
        self.app
    }
}

impl<T: 'static> Context<'_, T> {
    /// This entity.
    pub fn entity(&self) -> Entity<T> {
        self.entity
    }

    pub fn weak_entity(&self) -> WeakEntity<T> {
        self.entity.downgrade()
    }

    /// Re-render the windows showing this entity.
    pub fn notify(&mut self) {
        self.app.notify_all();
    }

    /// Turn a method-like closure into an element listener (gpui's `cx.listener`).
    pub fn listener<E: ?Sized>(
        &self,
        f: impl Fn(&mut T, &E, &mut Window, &mut Context<T>) + 'static,
    ) -> impl Fn(&E, &mut Window, &mut App) + 'static {
        self.entity.downgrade().listener(f)
    }
}

/// A view: something that renders an element tree (gpui's `Render`).
pub trait Render: 'static + Sized {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement;
}

// ==========================================================================
// The application

/// Window identity.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct WindowId(pub usize);

/// A typed window handle (gpui's `WindowHandle<V>`).
pub struct WindowHandle<V> {
    pub id: WindowId,
    entity: Entity<V>,
}

impl<V> Clone for WindowHandle<V> {
    fn clone(&self) -> Self {
        WindowHandle { id: self.id, entity: self.entity }
    }
}

impl<V: 'static> WindowHandle<V> {
    /// The root view of the window.
    pub fn root(&self) -> Entity<V> {
        self.entity
    }

    /// Update the root view with its window.
    pub fn update<R>(&self, cx: &mut App,
                     f: impl FnOnce(&mut V, &mut Window, &mut Context<V>) -> R) -> Option<R> {
        cx.with_window(self.id.0, |w, cx| self.entity.update(cx, |v, cx| f(v, w, cx)))
    }
}

/// How a window opens (gpui's `WindowBounds`).
#[derive(Clone, Copy, Debug)]
pub enum WindowBounds {
    Windowed(Bounds<Pixels>),
    Maximized(Bounds<Pixels>),
    Fullscreen(Bounds<Pixels>),
}

/// How the window's background shows (gpui's `WindowBackgroundAppearance`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum WindowBackgroundAppearance {
    #[default]
    Opaque,
    /// Per-pixel alpha: only the elements show. On Windows the window has no frame.
    Transparent,
}

#[derive(Clone, Debug, Default)]
pub struct TitlebarOptions {
    pub title: Option<SharedString>,
}

/// Options for [`App::open_window`] (gpui's `WindowOptions`).
#[derive(Clone, Debug, Default)]
pub struct WindowOptions {
    pub window_bounds: Option<WindowBounds>,
    pub titlebar: Option<TitlebarOptions>,
    pub is_resizable: Option<bool>,
    pub window_min_size: Option<Size<Pixels>>,
    /// Draw this window on the GPU (Direct3D 11 / OpenGL 3.3) instead of the software
    /// raster; `None` follows [`Application::gpu`] / the `BUI_GPU` variable.
    pub gpu: Option<bool>,
    pub window_background: WindowBackgroundAppearance,
    pub kind: WindowKind,
    pub window_decorations: WindowDecorations,
}

/// The GPU renderer's graphics API on Windows (Linux has OpenGL only: there this is ignored).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum GpuApi {
    /// Direct3D 11, the default.
    D3d11,
    /// Direct3D 12. A device that cannot start on it starts on Direct3D 11.
    D3d12,
}

impl GpuApi {
    const fn raw(self) -> c_int {
        match self {
            Self::D3d11 => sys::BUI_GPU_API_D3D11,
            Self::D3d12 => sys::BUI_GPU_API_D3D12,
        }
    }
}

/// Which GPU renders on a machine with two, as a hybrid laptop's integrated and discrete ones
/// (Windows; elsewhere the GPU is the display's and this is ignored).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum GpuAdapter {
    /// The display's GPU, the default: the integrated one on most hybrid laptops, which costs the
    /// least power and needs no copy of each frame between GPUs.
    Default,
    /// The GPU Windows picks for high performance: the discrete one (and the one vendor profilers
    /// such as NVIDIA Nsight see).
    HighPerformance,
}

impl GpuAdapter {
    const fn raw(self) -> c_int {
        match self {
            Self::Default => sys::BUI_GPU_ADAPTER_DEFAULT,
            Self::HighPerformance => sys::BUI_GPU_ADAPTER_HIGH_PERFORMANCE,
        }
    }
}

/// The application entry point (gpui's `Application`).
pub struct Application {
    headless: bool,
    gpu: Option<bool>,
    gpu_api: Option<GpuApi>,
    gpu_adapter: Option<GpuAdapter>,
    slug: Option<bool>,
    keep_alive: bool,
    theme: Option<String>,
}

impl Default for Application {
    fn default() -> Self {
        Application::new()
    }
}

impl Application {
    /// Native windows.
    pub fn new() -> Application {
        Application { headless: false, gpu: None, gpu_api: None, gpu_adapter: None, slug: None, keep_alive: false, theme: None }
    }

    /// Draw every window on the GPU (Direct3D 11 on Windows, or 12: [`Application::gpu_api`]): the same
    /// pixels as the software raster, with rasterization off the CPU. Falls
    /// back to software where no device exists. The `BUI_GPU` environment
    /// variable (1/0) overrides this.
    pub fn gpu(mut self, on: bool) -> Application {
        self.gpu = Some(on);
        self
    }

    /// The graphics API the GPU renderer uses on Windows: [`GpuApi::D3d11`] (the default) or
    /// [`GpuApi::D3d12`], which falls back to Direct3D 11 where it cannot start. The
    /// `BUI_GPU_API` environment variable (`d3d11`/`d3d12`) overrides this; without either, a
    /// default baked into the executable (`bui` CLI) applies, then Direct3D 11.
    pub fn gpu_api(mut self, api: GpuApi) -> Application {
        self.gpu_api = Some(api);
        self
    }

    /// Which GPU the GPU renderer uses on a machine with two (Windows): [`GpuAdapter::Default`],
    /// the display's (the integrated GPU of most hybrid laptops), or
    /// [`GpuAdapter::HighPerformance`], the discrete one. The `BUI_GPU_ADAPTER` environment variable
    /// (`high-performance`/`default`) overrides this.
    pub fn gpu_adapter(mut self, adapter: GpuAdapter) -> Application {
        self.gpu_adapter = Some(adapter);
        self
    }

    /// How the GPU renderer draws text: `true` for Slug (glyph outlines in
    /// the pixel shader, sharp at any size), `false` for a per-glyph bitmap
    /// atlas like Blender's (less GPU work). Defaults to the `slug` feature;
    /// `BUI_SLUG=1`/`0` overrides. See also [`crate::set_slug`].
    pub fn slug(mut self, on: bool) -> Application {
        self.slug = Some(on);
        self
    }

    /// Blender's `"dark"` (default) or `"light"` theme for every window (see
    /// [`crate::set_theme`]); the `BUI_THEME` variable applies otherwise.
    pub fn theme(mut self, name: &str) -> Application {
        self.theme = Some(name.to_string());
        self
    }

    /// Keep the event loop running with no windows open, until
    /// [`App::quit`]: for apps driven from another thread ([`App::ui_sender`]).
    pub fn keep_alive(mut self, on: bool) -> Application {
        self.keep_alive = on;
        self
    }

    /// Off-screen windows (tests, screenshots, CI): nothing is shown, the
    /// `simulate_*` and `draw` methods drive it.
    pub fn headless() -> Application {
        Application { headless: true, gpu: None, gpu_api: None, gpu_adapter: None, slug: None, keep_alive: false, theme: None }
    }

    /// The kit's global settings for this application (renderer, text, theme, off-screen scale).
    fn configure(&self, headless: bool) {
        if let Some(on) = self.gpu {
            unsafe { sys::BUI_gpu_set_default(on as c_int) };
        }
        if let Some(api) = self.gpu_api {
            // SAFETY: sets a global choice, read when a device is created.
            unsafe { sys::BUI_gpu_set_api(api.raw()) };
        }
        if let Some(adapter) = self.gpu_adapter {
            // SAFETY: as above.
            unsafe { sys::BUI_gpu_set_adapter(adapter.raw()) };
        }
        if let Some(on) = self.slug {
            crate::set_slug(on);
        }
        if let Some(t) = &self.theme {
            crate::set_theme(t);
        }
        if headless {
            // off-screen windows have no monitor DPI: BUI_UI_SCALE (e.g. 2 for
            // retina-sharp screenshots) sets the resolution scale
            if let Some(s) = std::env::var("BUI_UI_SCALE").ok().and_then(|v| v.parse::<f32>().ok()) {
                unsafe { sys::BK_set_scale(s) };
            }
        }
    }

    /// Run `f` and hand back the application, off-screen and with a real clock, for a host that has no
    /// event loop for BUI to run (a web page, which calls back on every animation frame): it feeds the
    /// events (`simulate_*`) and takes the frames ([`App::frame_if_needed`]). The kit keeps global
    /// state: one application per process, and the host must not start a second.
    pub fn start(self, f: impl FnOnce(&mut App)) -> App {
        self.configure(true);
        // a page's animations follow its clock, not the virtual one tests step by hand
        let mut app = App::create(true, true);
        f(&mut app);
        app.flush();
        app
    }

    /// Run `f`, then the event loop until every window is closed.
    pub fn run(self, f: impl FnOnce(&mut App)) {
        // the C kit keeps global state: one application at a time per process
        static KIT: std::sync::Mutex<()> = std::sync::Mutex::new(());
        let _guard = KIT.lock().unwrap_or_else(|e| e.into_inner());
        let headless = self.headless || unsafe { sys::BK_window_supported() } == 0;
        self.configure(headless);
        let mut app = App::create(headless, !headless);
        f(&mut app);
        app.flush();
        if headless {
            app.shutdown();
            return;
        }
        // the app stays on this stack frame; kit callbacks reach it through APP
        APP.with(|a| a.set(&mut app as *mut App));
        unsafe { sys::BK_window_set_keep_alive(self.keep_alive as c_int) };
        // SAFETY: plain message loop; `app` isn't touched here until it returns.
        unsafe { sys::BK_window_run() };
        APP.with(|a| a.set(std::ptr::null_mut()));
        app.shutdown();
    }
}

thread_local! {
    /// The app while the native loop runs (it lives on `Application::run`'s stack).
    static APP: Cell<*mut App> = const { Cell::new(std::ptr::null_mut()) };
    /// Set while a callback holds the app (a re-entrant callback must not).
    static BUSY: Cell<bool> = const { Cell::new(false) };
    /// Kit events, handled in order once the app is free.
    static QUEUE: Cell<VecDeque<RawEvent>> = const { Cell::new(VecDeque::new()) };
}

fn queue_push(ev: RawEvent) {
    QUEUE.with(|q| {
        let mut v = q.take();
        v.push_back(ev);
        q.set(v);
    });
}

fn queue_pop() -> Option<RawEvent> {
    QUEUE.with(|q| {
        let mut v = q.take();
        let ev = v.pop_front();
        q.set(v);
        ev
    })
}

struct RawEvent {
    window: usize,
    node: *mut sys::BKNode,
    kind: c_int,
    text: String,
    a: f64,
    b: f64,
    /// `BK_EV_A11Y_ACTION`: the element a screen reader's request is for (a: the action, b and
    /// text: its value); 0 for every other kind.
    #[cfg_attr(not(feature = "a11y"), allow(dead_code))]
    key: Key,
}

/// The application state (gpui's `App`).
pub struct App {
    /// By id (never reused): `None` while the window is lent to a callback (`with_window`) and once it
    /// has closed and been freed (`free_closed`).
    windows: Vec<Option<Box<Window>>>,
    /// How many windows are lent to callbacks right now.
    lent: usize,
    /// A simulated input is inside the kit (`input`): its events reach the listeners as they come, the
    /// windows render once the kit returns (as a native window's frame comes after its events).
    in_kit: bool,
    entities: Vec<Slot>,
    headless: bool,
    real_clock: bool,
    globals: HashMap<TypeId, Box<dyn Any>>,
    pub(crate) propagate: bool,
    /// The theme generation the windows were last rendered for: a theme change re-renders them
    /// (`light-dark()`, system colors, `prefers-color-scheme`, the `styled` sheet follow it).
    theme_seen: i32,
    /// A key listener acted on the key ([`App::prevent_default`]).
    default_prevented: bool,
    /// Work sent from other threads ([`UiSender`]).
    remote: Option<(std::sync::mpsc::Sender<RemoteJob>, std::sync::mpsc::Receiver<RemoteJob>)>,
    /// Called when the user closes a window.
    close_hooks: Vec<Box<dyn FnMut(WindowId, &mut App)>>,
    /// While the close hooks run: windows a hook closed, whose hooks run next.
    closed_later: Option<VecDeque<usize>>,
    /// Called when a window gets or loses the keyboard focus.
    activation_hooks: Vec<Box<dyn FnMut(WindowId, bool, &mut App)>>,
    /// Called with the desktop shell's events (`crate::shell`).
    pub(crate) shell_hooks: Vec<Box<dyn FnMut(&crate::shell::ShellEvent, &mut App)>>,
    /// Counts `notify_all` calls (see `WindowHandle::update`).
    notified: u64,
    /// [`shutdown`](App::shutdown) was asked while a window was lent to a
    /// callback: it runs once every window is back.
    shutdown_later: bool,
}

type RemoteJob = Box<dyn FnOnce(&mut App) + Send>;

/// Runs closures on the UI thread from any thread ([`App::ui_sender`]):
/// the UI loop wakes, runs them in order, then re-renders what they changed.
#[derive(Clone)]
pub struct UiSender {
    tx: std::sync::mpsc::Sender<RemoteJob>,
}

impl UiSender {
    /// Queue `f` for the UI thread. False when the app has ended.
    pub fn send(&self, f: impl FnOnce(&mut App) + Send + 'static) -> bool {
        let ok = self.tx.send(Box::new(f)).is_ok();
        // SAFETY: posts a message; callable from any thread.
        unsafe { sys::BK_window_wake() };
        ok
    }
}

unsafe extern "C" fn on_wake() {
    let ran = with_running_app(|app| {
        app.drain();
        app.flush();
    });
    if !ran {
        // a callback further up holds the app: try again on the next turn
        unsafe { sys::BK_window_wake() };
    }
}

impl Drop for App {
    fn drop(&mut self) {
        self.shutdown();
    }
}

impl App {
    /// `real_clock`: animations follow the wall clock (native windows, [`Application::start`]); else they
    /// follow the virtual one tests step with [`App::advance_clock`].
    fn create(headless: bool, real_clock: bool) -> App {
        crate::motion::start_clock(real_clock);
        App { windows: Vec::new(), lent: 0, in_kit: false, entities: Vec::new(), headless, real_clock, globals: HashMap::new(), propagate: true,
              default_prevented: false, remote: None, close_hooks: Vec::new(), closed_later: None,
              activation_hooks: Vec::new(), shell_hooks: Vec::new(), notified: 0, shutdown_later: false,
              theme_seen: crate::theme_generation() }
    }

    /// Create an entity (gpui's `cx.new`).
    pub fn new<T: 'static>(&mut self, build: impl FnOnce(&mut Context<T>) -> T) -> Entity<T> {
        let id = self.entities.len() as u32;
        self.entities.push(Slot { generation: 0, value: None });
        let entity = Entity { id, generation: 0, _t: PhantomData };
        let v = build(&mut Context { app: self, entity });
        self.entities[id as usize].value = Some(Box::new(v));
        entity
    }

    /// Drop an entity's value. Its handles become dead: `WeakEntity::update`
    /// skips them, `Entity::read` / `update` panic.
    pub fn release<T: 'static>(&mut self, entity: Entity<T>) {
        self.release_slot(entity.id, entity.generation);
    }

    /// Drop the value of entity `id` while it is generation `generation`.
    fn release_slot(&mut self, id: u32, generation: u32) {
        if let Some(slot) = self.entities.get_mut(id as usize).filter(|s| s.generation == generation) {
            slot.value = None;
            slot.generation += 1;
        }
    }

    /// Open a window whose root view is built by `build`.
    pub fn open_window<V: Render>(
        &mut self,
        options: WindowOptions,
        build: impl FnOnce(&mut Window, &mut App) -> Entity<V>,
    ) -> Result<WindowHandle<V>, String> {
        let bounds = match options.window_bounds {
            Some(WindowBounds::Windowed(b) | WindowBounds::Maximized(b) | WindowBounds::Fullscreen(b)) => b,
            None => Bounds { origin: point(px(0.0), px(0.0)), size: size(px(1024.0), px(700.0)) },
        };
        let title = options.titlebar.as_ref().and_then(|t| t.title.clone())
            .map(|t| t.to_string()).unwrap_or_else(|| "BUI".into());
        let id = self.windows.len();
        let mut w = Window::create(id, &title, bounds.size.width.0, bounds.size.height.0, self.headless, self.real_clock)?;
        if let Some(n) = &w.native {
            unsafe {
                if let Some(WindowBounds::Maximized(_)) = options.window_bounds {
                    sys::BK_window_wm(*n, c"state".as_ptr(), 3.0, 0.0);
                }
                if options.is_resizable == Some(false) {
                    sys::BK_window_wm(*n, c"resizable".as_ptr(), 0.0, 0.0);
                }
                if let Some(m) = options.window_min_size {
                    sys::BK_window_wm(*n, c"minsize".as_ptr(), (m.width.0 * w.scale) as f64,
                                      (m.height.0 * w.scale) as f64);
                }
                if options.window_background == WindowBackgroundAppearance::Transparent {
                    sys::BK_window_wm(*n, c"transparent".as_ptr(), 1.0, 0.0);
                }
                for (key, a, b) in options.kind.requests(options.window_decorations) {
                    sys::BK_window_wm(*n, key.as_ptr(), a, b);
                }
                if options.gpu == Some(true) {
                    sys::BK_window_set_gpu(*n, 1);
                }
            }
        }
        let entity = build(&mut w, self);
        w.view_entity = (entity.id, entity.generation);
        let view = entity;
        w.view = Some(Box::new(move |window: &mut Window, cx: &mut App| {
            view.update(cx, |v, cx| v.render(window, cx).into_any_element())
        }));
        w.dirty = true;
        self.windows.push(Some(Box::new(w)));
        if !self.headless {
            self.with_window(id, |w, app| w.rebuild(app));
            if let Some(n) = self.window(WindowId(id)).and_then(|w| w.native) {
                unsafe { sys::BK_window_show(n) };
            }
        }
        Ok(WindowHandle { id: WindowId(id), entity })
    }

    /// Mark every window for re-render.
    pub fn notify_all(&mut self) {
        self.notified = self.notified.wrapping_add(1);
        for w in self.windows.iter_mut().flatten() {
            w.refresh();
        }
    }

    /// A handle other threads use to run code on this (the UI) thread.
    pub fn ui_sender(&mut self) -> UiSender {
        if self.remote.is_none() {
            self.remote = Some(std::sync::mpsc::channel());
            if !self.headless {
                unsafe { sys::BK_window_wake_init(Some(on_wake)) };
            }
        }
        UiSender { tx: self.remote.as_ref().map(|(tx, _)| tx.clone()).unwrap() }
    }

    /// Run work queued by [`UiSender`]s and re-render what changed (for a
    /// loop that isn't the native one, e.g. headless apps driven by
    /// another thread).
    pub fn run_pending(&mut self) {
        self.drain();
        self.flush();
    }

    /// The windows that are open.
    pub fn window_ids(&self) -> Vec<WindowId> {
        self.windows.iter().enumerate().filter(|(_, w)| w.as_deref().is_some_and(|w| w.life == Life::Open)).map(|(i, _)| WindowId(i)).collect()
    }

    /// The window's next frame, when something changed in it or it is animating (else `None`: nothing to
    /// show): the work a host does on each animation frame ([`Application::start`]).
    pub fn frame_if_needed(&mut self, id: WindowId) -> Option<(usize, usize, Vec<u32>)> {
        self.run_pending();
        let ctx = self.windows.get(id.0).and_then(|w| w.as_deref()).map(|w| w.ctx)?;
        // SAFETY: the window's own context, alive while the window is
        let due = unsafe { sys::BK_needs_render(ctx) != 0 || sys::BK_animating(ctx) != 0 };
        due.then(|| self.draw(id))
    }

    /// Resize an off-screen window to `width` x `height` device pixels at `scale` device pixels per
    /// logical pixel (a host whose canvas changed size or pixel ratio).
    pub fn set_canvas_size(&mut self, id: WindowId, width: usize, height: usize, scale: f32) {
        if let Some(w) = self.windows.get_mut(id.0).and_then(|w| w.as_deref_mut()) {
            w.set_device_size((width as i32, height as i32), scale);
        }
    }

    /// Call `f` whenever a window closes: by the user, its title bar's Close control or
    /// [`close_window`](Self::close_window).
    pub fn on_window_closed(&mut self, f: impl FnMut(WindowId, &mut App) + 'static) {
        self.close_hooks.push(Box::new(f));
    }

    /// Call `f` with `true` when a window gets the keyboard focus and `false` when it
    /// loses it (gpui's `observe_window_activation`).
    pub fn on_window_activation(&mut self, f: impl FnMut(WindowId, bool, &mut App) + 'static) {
        self.activation_hooks.push(Box::new(f));
    }

    /// Resize a window to `size` (logical pixels), as the user dragging its
    /// edge does: a native window asks the system, a headless one takes the
    /// size at once (tests and benchmarks of resizing).
    pub fn resize_window(&mut self, id: WindowId, size: Size<Pixels>) {
        let Some(w) = self.windows.get_mut(id.0).and_then(|w| w.as_deref_mut()) else { return };
        let (lw, lh) = (f32::from(size.width), f32::from(size.height));
        match w.native {
            // SAFETY: a live native window; the kit reports the new size back (BK_EV_CONFIGURE).
            Some(n) => unsafe { sys::BK_window_set_size(n, lw.round() as c_int, lh.round() as c_int) },
            None => {
                let phys = ((lw * w.scale).round() as i32, (lh * w.scale).round() as i32);
                w.set_device_size(phys, w.scale);
            }
        }
        self.flush();
    }

    /// Close a window (as if the user had).
    pub fn close_window(&mut self, id: WindowId) {
        self.with_window(id.0, |w, _| w.remove_window());
    }

    /// The title-bar part under (x, y) (logical pixels) of a window whose title bar the app draws.
    pub fn window_control_area(&self, window: WindowId, x: f32, y: f32) -> Option<WindowControlArea> {
        self.window(window)?.control_area_at(x, y)
    }

    /// Stop the event loop.
    pub fn quit(&mut self) {
        unsafe { sys::BK_window_quit() };
    }

    /// Stop the current event from reaching ancestors.
    pub fn stop_propagation(&mut self) {
        self.propagate = false;
    }

    /// In a key listener: the app acted on the key, so the system doesn't (as a browser's
    /// `preventDefault`). On Android, Back is the key `Escape`: a listener that closes something on
    /// it calls this, and only a Back nothing took finishes the app.
    pub fn prevent_default(&mut self) {
        self.default_prevented = true;
    }

    /// Activate the app (no-op: windows come to the front when shown).
    pub fn activate(&mut self, _ignoring_other_apps: bool) {}

    pub fn set_global<G: 'static>(&mut self, g: G) {
        self.globals.insert(TypeId::of::<G>(), Box::new(g));
    }
    pub fn global<G: 'static>(&self) -> &G {
        self.try_global().expect("global not set")
    }
    pub fn try_global<G: 'static>(&self) -> Option<&G> {
        self.globals.get(&TypeId::of::<G>()).and_then(|g| g.downcast_ref())
    }
    pub fn global_mut<G: 'static>(&mut self) -> &mut G {
        self.globals.get_mut(&TypeId::of::<G>()).and_then(|g| g.downcast_mut()).expect("global not set")
    }

    pub fn window(&self, id: WindowId) -> Option<&Window> {
        self.windows.get(id.0)?.as_deref()
    }

    fn with_window<R>(&mut self, id: usize, f: impl FnOnce(&mut Window, &mut App) -> R) -> Option<R> {
        let mut w = self.windows.get_mut(id)?.take()?;
        self.lent += 1;
        let notified = self.notified;
        let r = f(&mut w, self);
        self.lent -= 1;
        // the window is out of the list while it runs: a `notify` in what it called missed it
        if self.notified != notified {
            w.refresh();
        }
        // closed by the app (`remove_window`): its close hooks, now that it is back
        let closing = w.life == Life::Closing;
        if closing {
            w.life = Life::Closed;
        }
        self.windows[id] = Some(w);
        if closing {
            self.window_closed(id);
        }
        if self.shutdown_later {
            self.shutdown();
        }
        Some(r)
    }

    /// Window `id` has closed: the close hooks. A window a hook closes (the hooks are out of
    /// the list then) has its hooks run after this round, once the list is back.
    fn window_closed(&mut self, id: usize) {
        if let Some(later) = &mut self.closed_later {
            later.push_back(id);
            return;
        }
        self.closed_later = Some(VecDeque::new());
        let mut next = Some(id);
        while let Some(id) = next {
            run_hooks(self, |a| &mut a.close_hooks, |h, app| h(WindowId(id), app));
            next = self.closed_later.as_mut().and_then(VecDeque::pop_front);
        }
        self.closed_later = None;
    }

    /// Free the windows that have closed: what they hold of the kit, their raster and their root view
    /// (the window owns it, as gpui's does). A window waits while close hooks are running (a hook
    /// still to come may look at it) and, a native one, until the system has destroyed it (its close
    /// is on the stack until then).
    fn free_closed(&mut self) {
        // also not inside a kit call that emits into a window (its context is still in use after the
        // listener returns: the cursor, the input entry's cleanup) and not while a window is lent to a
        // callback (it may share this one's root view): the next flush frees them
        if self.closed_later.is_some() || self.lent > 0 || kit_emitting() {
            return;
        }
        for id in 0..self.windows.len() {
            let done = self.windows[id].as_deref().is_some_and(|w| {
                // SAFETY: the window's own native window, alive until `destroy`.
                w.life == Life::Closed && w.native.is_none_or(|n| unsafe { sys::BK_window_is_open(n) } == 0)
            });
            if let Some(mut w) = self.windows[id].take_if(|_| done) {
                // it never renders again: its hook state goes too
                crate::hooks::drop_window(id);
                w.destroy();
                // a root view that other windows show (`open_window` may return an entity that exists)
                // stays until the last of them is freed
                let view = w.view_entity;
                if !self.windows.iter().flatten().any(|other| other.view_entity == view) {
                    self.release_slot(view.0, view.1);
                }
            }
        }
    }

    /// Handle queued kit events, then re-render what changed.
    fn drain(&mut self) {
        crate::shell::drain(self);
        while let Some(ev) = queue_pop() {
            // a closed window's queued events go with it (its handlers and hooks are done)
            if self.window(WindowId(ev.window)).is_some_and(|w| w.life == Life::Open) {
                self.with_window(ev.window, |w, app| w.dispatch(ev, app));
            }
        }
        // work from other threads (UiSender)
        loop {
            let job = match &self.remote {
                Some((_, rx)) => rx.try_recv().ok(),
                None => None,
            };
            match job {
                Some(f) => f(self),
                None => break,
            }
        }
    }

    /// Re-render dirty windows now (headless) or soon (native: next loop turn).
    fn flush(&mut self) {
        self.drain();
        // a `shutdown` that waited for a callback or a kit call to return (`shutdown` asks again)
        if self.shutdown_later {
            self.shutdown();
        }
        self.free_closed();
        let theme = crate::theme_generation();
        if theme != self.theme_seen {
            self.theme_seen = theme;
            self.notify_all();
        }
        // windows whose signals were written (hooks)
        for id in crate::hooks::take_dirty() {
            if id == usize::MAX {
                self.notify_all();
            } else if let Some(w) = self.windows.get_mut(id).and_then(|w| w.as_deref_mut()) {
                w.refresh();
            }
        }
        for id in 0..self.windows.len() {
            // a render may shut the app down (the windows go once it returns)
            let Some(slot) = self.windows.get_mut(id) else { break };
            let Some(w) = slot.as_deref_mut() else { continue };
            // a closed window draws no more frames (its animations stop with it)
            if w.life != Life::Open || (!w.dirty && !w.scrolled && !w.motion_frame && !w.sel_repaint && w.glides.is_empty()) {
                continue;
            }
            match w.native {
                // animation: rebuild right before the next (vsync-paced) render
                Some(n) if w.animation_frame => {
                    w.animation_frame = false;
                    w.frame_pending = true;
                    unsafe { sys::BK_window_request_frame(n, c_int::from(w.vsync)) };
                }
                // the requested frame rebuilds the window at its paced time
                Some(n) if !w.timer_pending && !w.frame_pending => {
                    w.timer_pending = true;
                    unsafe { sys::BK_window_timer(n, 0) };
                }
                Some(_) => {}
                // inside the kit: not under its feet (a render can free the widget it is handling)
                None if self.in_kit => {}
                None => {
                    self.with_window(id, |w, app| w.update(app));
                }
            }
        }
    }

    /// Free every window's native and raster resources. Safe to call again (and done on drop): a host
    /// that took the app from [`Application::start`] calls it when the page is done with it. Called from
    /// a window's callback (an event handler, a render), it runs once that callback has returned.
    pub fn shutdown(&mut self) {
        // a window lent to a callback is out of its slot, and one a kit call is emitting into is still in
        // use when the listener returns (the kit reads its context): free them all once they are back
        self.shutdown_later = self.lent > 0 || kit_emitting();
        if self.shutdown_later {
            return;
        }
        for w in self.windows.iter_mut().flatten() {
            w.destroy();
        }
        self.windows.clear();
    }

    // -- automation (tests, screenshots) --------------------------------------------------

    /// Render a window off-screen: (width, height, 0x00RRGGBB pixels) in device pixels.
    /// Images it shows are decoded first (the frame waits for the kit's
    /// image workers), so the pixels are deterministic.
    pub fn draw(&mut self, id: WindowId) -> (usize, usize, Vec<u32>) {
        self.flush();
        let Some(w) = self.windows.get_mut(id.0).and_then(|w| w.as_deref_mut()) else {
            return (0, 0, Vec::new());
        };
        let (pw, ph) = w.phys;
        // A native window's raster is what it presents next, and the pixels the kit compares frames with
        // belong to it: drawing into it here would take the damage of frames nobody presented, and
        // leave their old pixels on the screen. It gets its own raster, and the window repaints all.
        let own = w.native.is_some();
        // SAFETY: the raster is sized to the window; pixels are copied out.
        let px = unsafe {
            if !own && w.raster.is_null() {
                w.raster = sys::BUI_raster_create(pw, ph);
            }
            let target = if own { sys::BUI_raster_create(pw, ph) } else { w.raster };
            let _s = span(Phase::HeadlessRaster);
            sys::BK_render(w.ctx, target, pw, ph, 1);
            // decodes this frame asked for: wait, then draw what arrived
            // (WebAssembly has no worker threads and no sleeping: there is nothing to wait for)
            #[cfg(not(target_os = "wasi"))]
            {
                let mut st = sys::BKImageStats::default();
                sys::BK_image_stats(&raw mut st);
                for _ in 0..10_000 {
                    if st.pending == 0 {
                        break;
                    }
                    std::thread::sleep(std::time::Duration::from_micros(200));
                    sys::BK_image_stats(&raw mut st);
                }
            }
            if sys::BK_image_poll() > 0 {
                sys::BK_render(w.ctx, target, pw, ph, 0);
            }
            let p = sys::BUI_raster_pixels(target);
            let px = std::slice::from_raw_parts(p, (pw * ph) as usize).to_vec();
            if own {
                sys::BUI_raster_free(target);
                sys::BK_node_relayout(w.root); // the window's own next render covers everything again
            }
            px
        };
        self.flush(); // configure events from the render
        (pw as usize, ph as usize, px)
    }

    /// A frame the way a native window draws it, and the same frame in full: what the kit repaints
    /// where something changed (on the pixels of the last frame), and a fresh render of everything.
    /// They must be equal; a difference is a damaged region that was lost. (Tests: draw once first.)
    #[doc(hidden)]
    pub fn draw_partial_and_full(&mut self, id: WindowId) -> (usize, usize, Vec<u32>, Vec<u32>, Vec<[i32; 4]>) {
        self.flush();
        let Some(w) = self.windows.get_mut(id.0).and_then(|w| w.as_deref_mut()) else {
            return (0, 0, Vec::new(), Vec::new(), Vec::new());
        };
        let (pw, ph) = w.phys;
        // SAFETY: the rasters are sized to the window; pixels are copied out.
        let (partial, full, damage) = unsafe {
            if w.raster.is_null() {
                w.raster = sys::BUI_raster_create(pw, ph);
            }
            sys::BK_render(w.ctx, w.raster, pw, ph, 0);
            let p = sys::BUI_raster_pixels(w.raster);
            let partial = std::slice::from_raw_parts(p, (pw * ph) as usize).to_vec();
            // what a window's present would copy to the screen
            let mut rects: *const [c_int; 4] = std::ptr::null();
            let n = sys::BUI_raster_damage(w.raster, &raw mut rects);
            let damage = if rects.is_null() { Vec::new() } else { std::slice::from_raw_parts(rects, n as usize).to_vec() };
            let fresh = sys::BUI_raster_create(pw, ph);
            sys::BK_render(w.ctx, fresh, pw, ph, 1);
            let p = sys::BUI_raster_pixels(fresh);
            let full = std::slice::from_raw_parts(p, (pw * ph) as usize).to_vec();
            sys::BUI_raster_free(fresh);
            (partial, full, damage)
        };
        self.flush();
        (pw as usize, ph as usize, partial, full, damage)
    }

    /// Bring a window's layout up to date (what `element_bounds` reads) without drawing it: a native
    /// window draws itself on its next frame, and [`draw`](Self::draw) would be a second, unpresented one.
    ///
    /// An off-screen window has no loop of its own: it is drawn, which is also what lays the kit out.
    pub fn layout(&mut self, id: WindowId) {
        let native = self.windows.get(id.0).and_then(|w| w.as_deref()).is_some_and(|w| w.native.is_some());
        if native {
            self.flush();
            self.with_window(id.0, |w, app| w.update(app));
        } else {
            self.draw(id);
        }
    }

    /// Move every window's CSS animation clock forward by `d`. Headless
    /// windows' clocks stand still otherwise, so tests step animations and
    /// transitions deterministically; native windows add `d` to real time.
    pub fn advance_clock(&mut self, d: std::time::Duration) {
        crate::motion::advance(d.as_secs_f64());
        for w in self.windows.iter_mut().flatten() {
            if w.life != Life::Open {
                continue;
            }
            w.clock.advance(d);
            // as a native window's next frame: a redraw (a full frame when a
            // CSS value is more than paint)
            if w.caches.anim.css_running() {
                w.request_css_frame();
            }
            if crate::motion::active() {
                w.request_motion_frame();
            }
        }
        self.flush();
    }

    /// Bounds (logical pixels) of the element with `.id(id)`.
    pub fn element_bounds(&mut self, window: WindowId, id: &str) -> Option<Bounds<f32>> {
        self.flush();
        let w = self.window(window)?;
        let f = w.frame.as_ref()?;
        f.by_key().get(&id_key(id)).map(|&i| f.nodes[i].bounds)
    }

    fn input(&mut self, window: WindowId, f: impl FnOnce(*mut sys::BKContext, f32)) {
        self.flush();
        let Some((ctx, scale)) = self.window(window).filter(|w| w.frame.is_some()).map(|w| (w.ctx, w.scale)) else {
            return;
        };
        // as the native loop delivers them (`Application::run`): the kit's events reach the listeners
        // during the call, so a key listener's `prevent_default` takes the key from Blender's own
        // handling, as it does in a window. `self` isn't touched until the call returns.
        let outer = APP.with(|a| a.replace(std::ptr::from_mut(self)));
        let was = std::mem::replace(&mut self.in_kit, true);
        f(ctx, scale);
        self.in_kit = was;
        APP.with(|a| a.set(outer));
        self.flush();
    }

    /// Move the pointer to (x, y) in logical pixels.
    pub fn simulate_mouse_move(&mut self, window: WindowId, x: f32, y: f32) {
        self.input(window, |ctx, s| unsafe {
            sys::BK_motion(ctx, (x * s) as c_int, (y * s) as c_int, 0);
        });
    }

    /// Press and release the left button at (x, y).
    pub fn simulate_click(&mut self, window: WindowId, x: f32, y: f32) {
        self.simulate_mouse_move(window, x, y);
        self.input(window, |ctx, s| unsafe {
            sys::BK_press(ctx, (x * s) as c_int, (y * s) as c_int, 0);
        });
        self.input(window, |ctx, s| unsafe {
            sys::BK_release(ctx, (x * s) as c_int, (y * s) as c_int, sys::BK_BUTTON1);
        });
    }

    /// Click at (x, y) with modifier keys held (`ctrl`, `shift`).
    pub fn simulate_click_with(&mut self, window: WindowId, x: f32, y: f32, ctrl: bool, shift: bool) {
        let mods = c_int::from(shift) | (c_int::from(ctrl) << 1);
        self.simulate_mouse_move(window, x, y);
        self.input(window, |ctx, s| unsafe {
            sys::BK_press(ctx, (x * s) as c_int, (y * s) as c_int, mods);
        });
        self.input(window, |ctx, s| unsafe {
            sys::BK_release(ctx, (x * s) as c_int, (y * s) as c_int, sys::BK_BUTTON1 | mods);
        });
    }

    /// Press the left button at (x, y) (and hold it: see [`simulate_release`](Self::simulate_release)).
    pub fn simulate_press(&mut self, window: WindowId, x: f32, y: f32) {
        self.simulate_mouse_move(window, x, y);
        self.input(window, |ctx, s| unsafe {
            sys::BK_press(ctx, (x * s) as c_int, (y * s) as c_int, 0);
        });
    }

    /// Let go of the left button at (x, y).
    pub fn simulate_release(&mut self, window: WindowId, x: f32, y: f32) {
        self.input(window, |ctx, s| unsafe {
            sys::BK_release(ctx, (x * s) as c_int, (y * s) as c_int, sys::BK_BUTTON1);
        });
    }

    /// The host lost the pointer mid-press (a cancelled touch, a lost pointer capture): a drag goes back
    /// as Escape puts it back, and the press ends with no motion, no mouse-up and no click.
    pub fn simulate_cancel(&mut self, window: WindowId) {
        self.with_window(window.0, |w, _| {
            w.end_press(w.press_button);
        });
        self.input(window, |ctx, _| unsafe {
            sys::BK_cancel(ctx, 0);
        });
    }

    /// Click with the middle or the secondary button at (x, y) (as a window sends it: the secondary
    /// button's click arrives as its release alone). The left button is [`simulate_click`](Self::simulate_click).
    pub fn simulate_button(&mut self, window: WindowId, x: f32, y: f32, button: MouseButton) {
        let code = match button {
            MouseButton::Left => return self.simulate_click(window, x, y),
            MouseButton::Middle => 2,
            MouseButton::Right => 3,
        };
        self.simulate_mouse_move(window, x, y);
        self.input(window, |ctx, s| unsafe {
            let (x, y) = ((x * s) as c_int, (y * s) as c_int);
            if button == MouseButton::Middle {
                sys::BK_button(ctx, x, y, code, 0, 0);
            }
            sys::BK_button(ctx, x, y, code, 1, 0);
        });
    }

    /// Press at `from`, drag to `to` and release there.
    pub fn simulate_drag(&mut self, window: WindowId, from: (f32, f32), to: (f32, f32)) {
        self.simulate_mouse_move(window, from.0, from.1);
        self.input(window, |ctx, s| unsafe {
            sys::BK_press(ctx, (from.0 * s) as c_int, (from.1 * s) as c_int, 0);
        });
        for k in 1..=4 {
            let t = k as f32 / 4.0;
            let (x, y) = (from.0 + (to.0 - from.0) * t, from.1 + (to.1 - from.1) * t);
            self.input(window, |ctx, s| unsafe {
                sys::BK_motion(ctx, (x * s) as c_int, (y * s) as c_int, sys::BK_BUTTON1);
            });
        }
        self.input(window, |ctx, s| unsafe {
            sys::BK_release(ctx, (to.0 * s) as c_int, (to.1 * s) as c_int, sys::BK_BUTTON1);
        });
    }

    /// A key with modifier keys held.
    pub fn simulate_key_with(&mut self, window: WindowId, text: &str, keysym: &str, ctrl: bool, shift: bool) {
        let mods = c_int::from(shift) | (c_int::from(ctrl) << 1);
        let (t, k) = (CString::new(text).unwrap_or_default(), CString::new(keysym).unwrap_or_default());
        self.input(window, |ctx, _| unsafe {
            sys::BK_key(ctx, t.as_ptr(), k.as_ptr(), mods);
        });
    }

    /// Scroll the wheel at (x, y): positive `lines` scroll the content up.
    pub fn simulate_scroll(&mut self, window: WindowId, x: f32, y: f32, lines: i32) {
        self.simulate_mouse_move(window, x, y);
        self.input(window, |ctx, s| unsafe {
            sys::BK_wheel(ctx, (x * s) as c_int, (y * s) as c_int, lines, 0);
        });
    }

    /// A touch drag at (x, y): the content moves `dy` logical pixels down (negative: up), exactly.
    pub fn simulate_scroll_px(&mut self, window: WindowId, x: f32, y: f32, dy: f32) {
        self.simulate_mouse_move(window, x, y);
        self.input(window, |ctx, s| unsafe {
            sys::BK_scroll_px(ctx, (x * s) as c_int, (y * s) as c_int, 0.0, dy * s, 0);
        });
    }

    /// A touch screen contact at (x, y), logical pixels: `id` tells the fingers apart. The kit's
    /// touch model makes of the contacts what it makes of a real screen's (taps, scrolls and
    /// flings, long presses, two-finger pans, pinches and turns).
    pub fn simulate_touch(&mut self, window: WindowId, id: i32, phase: TouchPhase, x: f32, y: f32) {
        self.input(window, |ctx, s| unsafe {
            sys::BK_touch(ctx, id, phase.to_kit(), x * s, y * s, 0.0, 0);
        });
    }

    /// A trackpad pinch at (x, y): `delta` is the scale change (0.1 = 10% larger).
    pub fn simulate_magnify(&mut self, window: WindowId, x: f32, y: f32, delta: f32) {
        self.input(window, |ctx, s| unsafe {
            sys::BK_magnify(ctx, (x * s) as c_int, (y * s) as c_int, delta, 0);
        });
    }

    /// A trackpad turn at (x, y): degrees, positive counter-clockwise.
    pub fn simulate_rotate(&mut self, window: WindowId, x: f32, y: f32, degrees: f32) {
        self.input(window, |ctx, s| unsafe {
            sys::BK_rotate(ctx, (x * s) as c_int, (y * s) as c_int, degrees, 0);
        });
    }

    /// Type text / press a key (`keysym` as in Tk: "Return", "BackSpace", "a" ...).
    pub fn simulate_key(&mut self, window: WindowId, text: &str, keysym: &str) {
        let (t, k) = (CString::new(text).unwrap_or_default(), CString::new(keysym).unwrap_or_default());
        self.input(window, |ctx, _| unsafe {
            sys::BK_key(ctx, t.as_ptr(), k.as_ptr(), 0);
        });
    }
}

/// Accessibility (the `a11y` feature): what a screen reader gets of a window, and its requests.
#[cfg(feature = "a11y")]
impl App {
    /// The window's accessibility tree as a screen reader gets it: the window, the kit's widgets
    /// and the page (see [`crate::a11y`]). The first call starts it, as a reader attaching does.
    pub fn accessibility_tree(&mut self, window: WindowId) -> crate::a11y::AccessTree {
        let mut tree = crate::a11y::AccessTree::default();
        // a reader's first look: the page hears of it and gives its subtree, then the tree is read
        for _ in 0..2 {
            self.input(window, |ctx, _| {
                let (mut nodes, mut focus) = (std::ptr::null(), 0u64);
                // SAFETY: the live context; the nodes live until the next call, and are copied now.
                unsafe {
                    let n = sys::BK_a11y_tree(ctx, std::ptr::null(), &raw mut nodes, &raw mut focus);
                    tree = crate::a11y::AccessTree { nodes: crate::a11y::snapshot(nodes, n), focus };
                }
            });
        }
        tree
    }

    /// Ask for `action` on node `id` of [`accessibility_tree`](Self::accessibility_tree), as a
    /// screen reader does (`value`: `SetValue`'s text; `num`: its number). True when it was done.
    pub fn accessibility_action(&mut self, window: WindowId, id: u64, action: crate::a11y::Action,
                                value: Option<&str>, num: f64) -> bool {
        let text = value.map(|v| CString::new(v.replace('\0', "")).unwrap_or_default());
        let req = sys::BKA11yRequest { id, action: action as i32, num,
                                       text: text.as_deref().map_or(std::ptr::null(), CStr::as_ptr) };
        let mut done = false;
        // SAFETY: the live context and a request valid for the call.
        self.input(window, |ctx, _| done = unsafe { sys::BK_a11y_action(ctx, &raw const req) } != 0);
        done
    }
}

// ==========================================================================
// Kit callbacks

thread_local! {
    /// How many kit calls are emitting into a listener right now (`on_emit` is on the stack).
    static EMITTING: Cell<u32> = const { Cell::new(0) };
}

/// Whether a kit call is emitting into the app: the context of the window it is for must outlive it,
/// so a window a listener closed is freed once the call has returned.
fn kit_emitting() -> bool {
    EMITTING.with(|e| e.get() > 0)
}

/// Marks `on_emit` as running for as long as it lives (also when a listener unwinds).
struct Emitting;

impl Emitting {
    fn enter() -> Self {
        EMITTING.with(|e| e.set(e.get() + 1));
        Self
    }
}

impl Drop for Emitting {
    fn drop(&mut self) {
        EMITTING.with(|e| e.set(e.get() - 1));
    }
}

unsafe extern "C" fn on_emit(data: *mut c_void, node: *mut sys::BKNode, kind: c_int, ref_: *mut c_void,
                             a: c_double, b: c_double) {
    let _emitting = Emitting::enter();
    crate::screen::kit_event(node, kind, a, b); // areas made and freed: known at once
    #[cfg(feature = "a11y")]
    if kind == sys::BK_EV_A11Y_ACTION {
        // SAFETY: the kit's request, valid for this call: copied now.
        if let Some(r) = unsafe { crate::a11y::Request::from_kit(ref_.cast()) } {
            queue_push(RawEvent { window: data as usize, node, kind, text: r.text.unwrap_or_default(),
                                  a: f64::from(r.action as i32), b: r.num, key: r.key });
            with_running_app(App::flush);
        }
        return;
    }
    let text = if ref_.is_null() || kind < sys::BK_EV_PRESS {
        String::new()
    } else {
        // SAFETY: raw events carry a C string in `ref`.
        unsafe { CStr::from_ptr(ref_ as *const c_char) }.to_string_lossy().into_owned()
    };
    queue_push(RawEvent { window: data as usize, node, kind, text, a, b, key: 0 });
    with_running_app(|app| app.flush());
}

static FRAMES_PRESENTED: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// Frames drawn by every window so far (full renders and redraws of moving
/// values alike): a frame counter for benchmarks and overlays, readable from
/// any thread.
pub fn frames_presented() -> u64 {
    FRAMES_PRESENTED.load(std::sync::atomic::Ordering::Relaxed)
}

unsafe extern "C" fn on_timer(data: *mut c_void, _win: *mut sys::BKWindow, _id: c_int) {
    let id = data as usize;
    let ran = with_running_app(|app| {
        app.drain();
        app.with_window(id, |w, app| {
            w.timer_pending = false;
            w.update(app);
        });
        app.flush();
    });
    if !ran {
        // the app is busy (re-entrant callback): try again next turn
        unsafe { sys::BK_window_timer(_win, 1) };
    }
}

unsafe extern "C" fn on_frame(data: *mut c_void, _win: *mut sys::BKWindow) {
    // BUI_PROFILE: wall time between frames, i.e. rebuild + kit render + present
    thread_local! { static LAST: std::cell::Cell<Option<std::time::Instant>> = const { std::cell::Cell::new(None) }; }
    if prof::on() {
        let now = std::time::Instant::now();
        if let Some(t) = LAST.with(|l| l.replace(Some(now))) {
            prof::add(Phase::FrameInterval, (now - t).as_secs_f64() * 1000.0);
        }
    }
    let id = data as usize;
    with_running_app(|app| {
        app.drain();
        app.with_window(id, |w, app| {
            w.frame_pending = false;
            w.update(app);
        });
        app.flush();
    });
}

unsafe extern "C" fn on_close(data: *mut c_void, _win: *mut sys::BKWindow) -> c_int {
    let id = data as usize;
    with_running_app(|app| {
        if let Some(w) = app.windows.get_mut(id).and_then(|w| w.as_deref_mut()) {
            w.life = Life::Closed;
        }
        app.window_closed(id);
    });
    1
}

unsafe extern "C" fn on_focus(data: *mut c_void, _win: *mut sys::BKWindow, active: c_int) {
    let id = data as usize;
    with_running_app(|app| {
        run_hooks(app, |a| &mut a.activation_hooks, |h, app| h(WindowId(id), active != 0, app));
    });
}

unsafe extern "C" fn on_hit_test(data: *mut c_void, _win: *mut sys::BKWindow, x: c_int, y: c_int) -> c_int {
    let mut area = sys::BK_WINDOW_AREA_CLIENT;
    // busy (asked from inside a callback, as a move does): the app's, until the next ask
    with_running_app(|app| {
        if let Some(w) = app.window(WindowId(data as usize)) {
            area = match w.control_area_at(x as f32 / w.scale, y as f32 / w.scale) {
                None => sys::BK_WINDOW_AREA_CLIENT,
                Some(WindowControlArea::Drag) => sys::BK_WINDOW_AREA_DRAG,
                Some(WindowControlArea::Min) => sys::BK_WINDOW_AREA_MIN,
                Some(WindowControlArea::Max) => sys::BK_WINDOW_AREA_MAX,
                Some(WindowControlArea::Close) => sys::BK_WINDOW_AREA_CLOSE,
            };
        }
    });
    area
}

/// Call each hook; hooks a hook adds run from the next call.
pub(crate) fn run_hooks<H: ?Sized>(app: &mut App, list: fn(&mut App) -> &mut Vec<Box<H>>,
                        mut call: impl FnMut(&mut H, &mut App)) {
    let mut hooks = std::mem::take(list(app));
    hooks.iter_mut().for_each(|h| call(h, app));
    hooks.append(list(app));
    *list(app) = hooks;
}

/// Run `f` with the app, unless there is none (before the loop, headless)
/// or a callback further up the stack already holds it.
pub(crate) fn with_running_app(f: impl FnOnce(&mut App)) -> bool {
    let app = APP.with(Cell::get);
    if app.is_null() || BUSY.with(Cell::get) {
        return false;
    }
    BUSY.with(|b| b.set(true));
    // SAFETY: `app` points at the live App on `Application::run`'s stack,
    // which isn't touched while the loop runs; BUSY keeps this `&mut` unique.
    f(unsafe { &mut *app });
    BUSY.with(|b| b.set(false));
    true
}

unsafe extern "C" fn noop(_: *mut c_void) {}

unsafe extern "C" {
    /// The C runtime's `free`, for strings the kit allocates (the clipboard).
    fn free(p: *mut c_void);
}

// ==========================================================================
// Windows

/// Where a window is in its life.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Life {
    Open,
    /// Closed by the app; its close hooks have not run yet.
    Closing,
    Closed,
}

type ViewFn = Box<dyn Fn(&mut Window, &mut App) -> AnyElement>;

/// An element asked into view: where in its scroll container, and how.
struct Reveal {
    key: Key,
    at: f32,
    glide: Option<f32>,
}

/// A scroll container gliding from one offset to another (seconds).
struct Glide {
    container: Key,
    from: (f32, f32),
    to: (f32, f32),
    start: f64,
    duration: f32,
}

/// `Window::state_read` bits: the view read the hover (`is_hovered`) or the focus (`focused_id`).
const READ_HOVER: u8 = 1;
const READ_FOCUS: u8 = 2;

/// A window (gpui's `Window`): its kit context, layout and interaction state.
pub struct Window {
    id: usize,
    ctx: *mut sys::BKContext,
    canvas: *mut sys::BKNode,
    /// The canvases over the first one (see `Painter`: items that overlap a
    /// widget are drawn on a canvas above it), made as needed.
    overlays: Vec<*mut sys::BKNode>,
    /// `canvas`, then `overlays`: what `Painter::flush` fills.
    canvases: Vec<*mut sys::BKNode>,
    root: *mut sys::BKNode,
    opaque: bool,
    native: Option<*mut sys::BKWindow>,
    raster: *mut sys::BuiRaster,
    phys: (i32, i32),
    scale: f32,
    view: Option<ViewFn>,
    /// The root view's entity (id, generation): released with the window.
    view_entity: (u32, u32),
    frame: Option<Frame>,
    /// A view transition starting or running (`crate::transition`).
    transition: Option<crate::transition::Transition>,
    /// While `emit` lends the frame to listeners: the bounds of the event's
    /// path (the target and its ancestors), so `bounds_of` still answers for
    /// them. Reused, so an event allocates nothing once it has grown.
    lent_bounds: Vec<(Key, Bounds<f32>)>,
    /// Styles, text widths, images, the author sheet and the taffy tree:
    /// kept across frames.
    caches: LayoutCaches,
    /// The display list, reused every frame.
    painter: Painter,
    state: UiState,
    widgets: Widgets,
    /// Blender screens (`blenderui::screen`): their kit nodes, by element key.
    screens: crate::screen::Screens,
    dirty: bool,
    /// Only scroll offsets changed since the last frame: it is placed again, not rendered
    /// (when [`scroll_free`](Self::scroll_free)).
    scrolled: bool,
    /// The last render did not depend on scroll positions: nothing read them (`scroll_offset`,
    /// `bounds_of`), nothing animates and no container sticks to its end.
    scroll_free: bool,
    /// A view read a scroll position or a placement this render (it must render again to follow it).
    scroll_read: std::cell::Cell<bool>,
    timer_pending: bool,
    frame_pending: bool,
    animation_frame: bool,
    /// The next frame is Motion's values alone: a redraw (no render or layout).
    motion_frame: bool,
    /// The redraw moves CSS animations and transitions too.
    css_frame: bool,
    /// The element asked into view this render ([`Window::scroll_into_view`]).
    reveal: Option<Reveal>,
    /// Scrolls gliding to an element.
    glides: Vec<Glide>,
    /// CSS animation time (virtual in headless windows).
    clock: crate::anim::Clock,
    vsync: bool,
    life: Life,
    title: String,
    cursor: i32,
    pointer: (f32, f32),
    pressed: Vec<Key>,
    press_button: MouseButton,
    /// How many presses in a row the last one was (a double click's second is 2).
    press_count: usize,
    /// Where the button went down, and whether its press-drag has come (Blender's `KM_PRESS_DRAG`, once per press).
    press_start: (f32, f32),
    press_drag_sent: bool,
    /// The element that holds the pointer (HTML's pointer capture): while one does, the pointer's events go
    /// to it, wherever the pointer is.
    capture: Option<Key>,
    /// The element whose listener is running (what [`Window::set_pointer_capture`] holds the pointer for).
    current: Option<Key>,
    /// The elements under the pointer, outermost first: the order of what it enters and leaves.
    hover_path: Vec<Key>,
    /// Scratch for the hit test of a pointer move (kept so a move allocates nothing).
    path_buf: Vec<Key>,
    /// Capture events raised by a listener: fired when the event it is in has gone through (the frame is
    /// lent to that one meanwhile).
    later: Vec<(EventKind, Key, PointerEvent)>,
    /// The interaction states the view read while it rendered (`READ_HOVER`: `is_hovered`, `READ_FOCUS`:
    /// `focused_id`): it renders again when they change, as it does for a style that depends on them.
    state_read: Cell<u8>,
    /// Where each scroll container was when its `scroll` event last went out (a scroll from any source).
    scroll_seen: HashMap<Key, (f32, f32)>,
    /// A scrollbar thumb being dragged: the container and where on the thumb it was grabbed.
    sb_drag: Option<(Key, f32)>,
    /// `data-stick="bottom"` containers: the scroll range last frame (to see when content grew).
    stuck: HashMap<Key, f32>,
    /// The text selection (see `selection`).
    selector: Selector,
    /// The press in progress changed only the text selection: nothing answers to it, so it and its
    /// release render nothing (the selection is drawn on the frame as it is). No press: true (its
    /// end renders nothing).
    press_quiet: bool,
    /// The next `present` shows the frame it already showed (a selection change): its text boxes are
    /// the same, so the selection's index of them stays.
    same_frame: bool,
    /// The selection changed: the next frame paints it again (no render, no layout).
    sel_repaint: bool,
    /// Called when the selection changes, with the selected text if any.
    selection_hooks: Vec<Box<dyn FnMut(Option<&str>, &mut Window, &mut App)>>,
    /// The page's accessibility subtree, kept while a screen reader reads the window (`a11y`).
    #[cfg(feature = "a11y")]
    a11y: Option<Box<crate::a11y::Accessibility>>,
}

impl Window {
    fn create(id: usize, title: &str, w: f32, h: f32, headless: bool, real_clock: bool) -> Result<Window, String> {
        // SAFETY: building a fresh kit tree; pointers stay owned by this window.
        unsafe {
            let root = sys::BK_node_new(c"frame".as_ptr());
            let canvas = sys::BK_node_new(c"canvas".as_ptr());
            if root.is_null() || canvas.is_null() {
                return Err("kit nodes unavailable".into());
            }
            sys::BK_node_add(root, canvas, -1);
            sys::BK_geom_set_manager(canvas, sys::BK_GEOM_PLACE);
            sys::BK_geom_set(canvas, c"relwidth".as_ptr(), 1.0);
            sys::BK_geom_set(canvas, c"relheight".as_ptr(), 1.0);
            sys::BK_set_num(canvas, c"events".as_ptr(), 1.0);
            let ctx = sys::BK_context_new(root);
            let host = sys::BKHost {
                data: id as *mut c_void,
                request: Some(noop),
                set_cursor: None,
                warp: None,
                clipboard_get: None,
                clipboard_set: None,
                schedule: None,
                emit: Some(on_emit),
            };
            sys::BK_context_set_host(ctx, &host);
            let mut win = Window {
                id,
                ctx,
                canvas,
                overlays: Vec::new(),
                canvases: vec![canvas],
                root,
                opaque: false,
                native: None,
                raster: std::ptr::null_mut(),
                phys: (w.round() as i32, h.round() as i32),
                scale: 1.0,
                view: None,
                view_entity: (u32::MAX, 0),
                frame: None,
                transition: None,
                lent_bounds: Vec::new(),
                caches: LayoutCaches::default(),
                painter: Painter::new(),
                state: UiState::default(),
                widgets: Widgets::new(),
                screens: crate::screen::Screens::default(),
                dirty: true,
                scrolled: false,
                scroll_free: false,
                scroll_read: std::cell::Cell::new(false),
                timer_pending: false,
                frame_pending: false,
                animation_frame: false,
                motion_frame: false,
                css_frame: false,
                reveal: None,
                glides: Vec::new(),
                clock: crate::anim::Clock::new(real_clock),
                vsync: true,
                life: Life::Open,
                title: title.to_owned(),
                cursor: 0,
                pointer: (-1.0, -1.0),
                pressed: Vec::new(),
                press_button: MouseButton::Left,
                press_count: 1,
                press_start: (0.0, 0.0),
                press_drag_sent: false,
                capture: None,
                current: None,
                hover_path: Vec::new(),
                path_buf: Vec::new(),
                later: Vec::new(),
                state_read: Cell::new(0),
                scroll_seen: HashMap::new(),
                sb_drag: None,
                stuck: HashMap::new(),
                selector: Selector::default(),
                press_quiet: true,
                same_frame: false,
                sel_repaint: false,
                selection_hooks: Vec::new(),
                #[cfg(feature = "a11y")]
                a11y: None,
            };
            if headless {
                win.scale = sys::BK_scale().max(0.25);
                win.phys = ((w * win.scale).round() as i32, (h * win.scale).round() as i32);
            } else {
                let t = CString::new(title).unwrap_or_default();
                let n = sys::BK_window_new(ctx, t.as_ptr(), w.round() as c_int, h.round() as c_int);
                if n.is_null() {
                    win.destroy();
                    return Err("could not create a native window".into());
                }
                let cb = sys::BKWindowCallbacks { data: id as *mut c_void, on_timer: Some(on_timer),
                                                  on_close: Some(on_close), on_frame: Some(on_frame),
                                                  on_focus: Some(on_focus), hit_test: Some(on_hit_test) };
                sys::BK_window_set_callbacks(n, &cb);
                win.scale = sys::BK_scale().max(0.5);
                win.phys = ((w * win.scale).round() as i32, (h * win.scale).round() as i32);
                win.native = Some(n);
            }
            win.caches.text.set_scale(win.scale);
            Ok(win)
        }
    }

    /// Free what the window holds of the kit. The order is the kit's: the
    /// native window gives the context its host back, the context lets go of
    /// its root (and of the raster it rendered last), then the tree goes
    /// (with the canvases), then the raster.
    fn destroy(&mut self) {
        self.widgets.free_all();
        self.screens.free_all();
        // SAFETY: each pointer is this window's and freed once (nulled after).
        unsafe {
            if let Some(n) = self.native.take() {
                sys::BK_window_free(n);
            }
            sys::BK_context_free(std::mem::replace(&mut self.ctx, std::ptr::null_mut()));
            sys::BK_node_free(std::mem::replace(&mut self.root, std::ptr::null_mut()));
            self.overlays.clear();
            self.canvases.clear();
            if !self.raster.is_null() {
                sys::BUI_raster_free(self.raster);
                self.raster = std::ptr::null_mut();
            }
        }
    }

    /// Window id.
    pub fn window_handle(&self) -> WindowId {
        WindowId(self.id)
    }

    /// The kit's native window, `None` off screen.
    pub(crate) const fn native(&self) -> Option<*mut sys::BKWindow> {
        self.native
    }

    /// Starts a view transition (CSS View Transitions): the page as it is now is pictured, and the
    /// state the next render shows is animated to: boxes with a `view-transition-name` move and
    /// resize from where they were while their old pictures fade out, and the rest of the page
    /// cross-fades. Call it before changing the state (in the same event handler). A window that
    /// hasn't rendered yet has nothing to animate from: it does nothing.
    pub fn start_view_transition(&mut self, cx: &App) {
        let vp = self.viewport();
        let Some(frame) = self.frame.as_ref() else { return };
        let mut shot = crate::transition::Shot { frame, styles: &self.caches.styles, text: &mut self.caches.text,
                                                 vp, scale: self.scale, app: cx };
        self.transition = Some(crate::transition::Transition::capture(&mut shot));
    }

    /// Re-render on the next turn.
    /// Render again on the next display refresh (gpui's
    /// `request_animation_frame`): call it from `render` to animate.
    pub fn request_animation_frame(&mut self) {
        self.animation_frame = true;
        self.dirty = true;
    }

    /// Pace animation frames to the display (default `true`). With `false`
    /// frames render as fast as the CPU allows (benchmarks).
    pub fn set_vsync(&mut self, on: bool) {
        self.vsync = on;
    }

    /// Whether CSS animations or transitions were running at the last
    /// render (the window keeps requesting frames while they are).
    pub fn is_animating(&self) -> bool {
        self.caches.anim.running()
    }

    /// Whether the window will render again (something changed or is
    /// animating). False for an idle window.
    pub fn needs_render(&self) -> bool {
        self.dirty || self.scrolled
    }

    /// Frames presented to the native window so far.
    pub fn presented_frames(&self) -> u64 {
        self.native.map_or(0, |n| unsafe { sys::BK_window_frames(n) } as u64)
    }

    pub fn refresh(&mut self) {
        self.dirty = true;
    }

    /// Paint the last frame again (no render, no layout): for state a painted element reads, like a hover highlight.
    pub fn repaint(&mut self) {
        self.sel_repaint = true;
    }

    /// Content size in logical pixels.
    pub fn viewport_size(&self) -> Size<Pixels> {
        size(px(self.phys.0 as f32 / self.scale), px(self.phys.1 as f32 / self.scale))
    }

    pub fn bounds(&self) -> Bounds<Pixels> {
        Bounds { origin: point(px(0.0), px(0.0)), size: self.viewport_size() }
    }

    /// Device pixels per logical pixel.
    pub fn scale_factor(&self) -> f32 {
        self.scale
    }

    /// Whether frames are drawn on the GPU right now (it turns on a few
    /// frames after start, once the device is ready).
    pub fn is_gpu(&self) -> bool {
        // SAFETY: `native` is a live kit window owned by this Window.
        self.native.map(|n| unsafe { sys::BK_window_gpu(n) } != 0).unwrap_or(false)
    }

    pub fn set_window_title(&mut self, title: &str) {
        if self.title != title {
            self.title = title.to_owned();
            if let Some(n) = self.native {
                let t = CString::new(title).unwrap_or_default();
                unsafe { sys::BK_window_set_title(n, t.as_ptr()) };
            }
        }
    }

    /// Close this window, as if the user had (gpui's `remove_window`). The close hooks run once
    /// the event that asked has been handled (`BK_window_close` does not ask `on_close`).
    pub fn remove_window(&mut self) {
        if self.life != Life::Open {
            return;
        }
        self.life = Life::Closing;
        if let Some(n) = self.native {
            unsafe { sys::BK_window_close(n) };
        }
    }

    pub fn minimize_window(&self) {
        self.wm(c"state", 1.0);
    }

    /// Maximize the window, or restore it when it is maximized.
    pub fn zoom_window(&self) {
        self.wm(c"state", if self.is_maximized() { 0.0 } else { 3.0 });
    }

    pub fn is_maximized(&self) -> bool {
        let (mut a, mut b) = (0.0, 0.0);
        self.native.is_some_and(|n| unsafe { sys::BK_window_wm_get(n, c"state".as_ptr(), &mut a, &mut b) } != 0
                                    && a == 3.0)
    }

    /// Let the window manager move the window with the pointer, from a press (what a drag area
    /// of a title bar the app draws does by itself). The window manager takes the pointer, so the
    /// press ends here: its release never comes.
    pub fn start_window_move(&mut self) {
        self.end_press(self.press_button);
        self.wm(c"move", 0.0);
    }

    /// An off-screen window is `phys` device pixels at `scale` device pixels per logical pixel: what
    /// a headless resize and a host's canvas resize both come to. Any positive finite ratio is kept
    /// (page zoom out gives 0.75).
    fn set_device_size(&mut self, phys: (i32, i32), scale: f32) {
        if !scale.is_finite() || scale <= 0.0 || phys.0 <= 0 || phys.1 <= 0 || (phys == self.phys && scale == self.scale) {
            return;
        }
        self.phys = phys;
        self.dirty = true;
        if scale != self.scale {
            // the kit's resolution scale and the text measured at it follow, as when the window is made
            self.scale = scale;
            self.caches.text.set_scale(scale);
            // SAFETY: a plain setter; the kit has one scale
            unsafe { sys::BK_set_scale(scale) };
        }
        // SAFETY: the raster was made for the old size and belongs to this window; `draw` makes a new one
        unsafe {
            if !self.raster.is_null() {
                sys::BUI_raster_free(self.raster);
                self.raster = std::ptr::null_mut();
            }
            sys::BK_node_relayout(self.root);
        }
    }

    fn wm(&self, key: &CStr, a: f64) {
        if let Some(n) = self.native {
            unsafe { sys::BK_window_wm(n, key.as_ptr(), a, 0.0) };
        }
    }

    /// The title-bar part (`app-region`) of box `hit` and the elements around it: the nearest one
    /// set, unless an element that answers to presses (a listener, a widget) or `no-drag` comes
    /// first, as a button inside a drag area does.
    fn control_area(&self, hit: Option<usize>) -> Option<WindowControlArea> {
        let f = self.frame.as_ref()?;
        let mut cur = hit.map(|b| f.nodes[b].owner);
        while let Some(i) = cur {
            let n = &f.nodes[i];
            match self.caches.styles[n.style].app_region {
                AppRegion::Area(a) => return Some(a),
                AppRegion::NoDrag => return None,
                AppRegion::Auto => {}
            }
            let pressable = f.el(i).and_then(|e| e.listeners.as_deref()).is_some_and(Listeners::take_presses);
            if pressable || matches!(n.content, Content::Widget(_)) {
                return None;
            }
            cur = n.parent;
        }
        None
    }

    /// A click on element `key`: what the title-bar control it is part of does.
    fn run_control_area(&mut self, key: Key) {
        let hit = self.frame.as_ref().and_then(|f| f.by_key().get(&key).copied());
        match self.control_area(hit) {
            Some(WindowControlArea::Min) => self.minimize_window(),
            Some(WindowControlArea::Max) => self.zoom_window(),
            Some(WindowControlArea::Close) => self.remove_window(),
            Some(WindowControlArea::Drag) | None => {}
        }
    }

    /// The title-bar part under (x, y), in logical pixels.
    fn control_area_at(&self, x: f32, y: f32) -> Option<WindowControlArea> {
        self.control_area(self.frame.as_ref()?.hit_box(x, y, &self.caches.styles))
    }

    /// Pointer position in logical pixels.
    pub fn mouse_position(&self) -> crate::geometry::Point<Pixels> {
        point(px(self.pointer.0), px(self.pointer.1))
    }

    /// The `.id()` of the focused element, if any.
    pub fn focused_id(&self) -> Option<&str> {
        self.state_read.set(self.state_read.get() | READ_FOCUS);
        self.state.focused_id.as_deref()
    }

    /// Focus the element with `.id(id)` (keyboard events go to it).
    /// Its ancestors in the last frame are within focus (`:focus-within`), as a press makes them.
    pub fn focus_id(&mut self, id: &str) {
        let key = id_key(id);
        let mut path = self.path_to_key(key);
        if path.is_empty() {
            path.push(key); // not drawn yet
        }
        self.focus_path(&path);
        self.state.focused_id = Some(id.to_owned());
        self.dirty = true;
    }

    /// Give the text field with `.id(id)` keyboard focus and start editing, as a click on it does
    /// ([`focus_id`](Self::focus_id) only moves the document's focus, not the widget's caret).
    pub fn focus_input(&mut self, id: &str) {
        self.focus_id(id);
        if let Some(w) = self.widgets.nodes.get(&id_key(id)) {
            // SAFETY: a live context and one of its live widget nodes.
            unsafe { sys::BK_focus_node(self.ctx, w.node) };
        }
    }

    /// Show (or hide) the popover with `.id(id)` (an element with a `popover` attribute): it
    /// matches `:popover-open`, which the user-agent sheet shows in the top layer. An open
    /// `<dialog>` shown so is modal (`:modal`).
    pub fn show_popover(&mut self, id: &str, show: bool) {
        let key = id_key(id);
        let changed = if show { self.state.popover_open.insert(key) } else { self.state.popover_open.remove(&key) };
        self.dirty |= changed;
    }

    /// Is the element with `.id(id)` hovered?
    pub fn is_hovered(&self, id: &str) -> bool {
        self.state_read.set(self.state_read.get() | READ_HOVER);
        self.state.hovered.contains(&id_key(id))
    }

    /// Scroll offset of a scroll container with `.id(id)`.
    pub fn scroll_offset(&self, id: &str) -> (f32, f32) {
        self.scroll_read.set(true);
        self.state.scroll.get(&id_key(id)).copied().unwrap_or((0.0, 0.0))
    }

    /// Scroll the scroll container around the element `.id(id)` so the
    /// element's top sits `at` of the way down it (0: at its top, 0.5: in its
    /// middle), as `scrollIntoView` does. It is worked out after this frame's
    /// layout, so it follows the layout the render makes: ask for it in
    /// `render`, each render, to keep an element in place as the page changes
    /// or the window is resized; nothing reads a placement and nothing renders
    /// twice. `glide`: the seconds the scroll takes to get there (eased out;
    /// its frames are scroll frames: no render), `None` jumps. A wheel or a
    /// scrollbar drag on the container stops a glide.
    pub fn scroll_into_view(&mut self, id: &str, at: f32, glide: Option<f32>) {
        // validated here, at the boundary: a NaN would reach the scroll state
        let at = if at.is_finite() { at.clamp(0.0, 1.0) } else { 0.0 };
        let glide = glide.filter(|d| d.is_finite() && *d > 0.0);
        self.reveal = Some(Reveal { key: id_key(id), at, glide });
    }

    /// Carry out the [`scroll_into_view`](Self::scroll_into_view) asked for
    /// while `frame` was rendered: true when its container jumped (place the
    /// frame again); a glide moves on in the next frames.
    fn reveal(&mut self, frame: &crate::layout::Frame) -> bool {
        let Some(r) = self.reveal.take() else { return false };
        let Some(&i) = frame.by_key().get(&r.key) else { return false };
        let nodes = &frame.nodes;
        // the nearest container that scrolls vertically (a horizontal-only one can't place it)
        let mut up = nodes[i].parent;
        while let Some(k) = up.filter(|&k| nodes[k].scroll_range.1 <= 0.0) {
            up = nodes[k].parent;
        }
        let Some(c) = up else { return false };
        let (n, el) = (&nodes[c], &nodes[i]);
        let (sx, sy) = n.scroll_pos;
        let to = (sy + el.bounds.origin.y - n.bounds.origin.y - r.at * n.bounds.size.height).clamp(0.0, n.scroll_range.1);
        let gliding = self.glides.iter().position(|g| g.container == n.key);
        if gliding.is_some_and(|g| (self.glides[g].to.1 - to).abs() < 0.5) || (gliding.is_none() && (sy - to).abs() < 0.5) {
            return false; // there, or on its way there
        }
        if let Some(g) = gliding {
            self.glides.remove(g);
        }
        match r.glide {
            Some(duration) => {
                // from where it is now (a glide that was on its way included)
                self.glides.push(Glide { container: n.key, from: (sx, sy), to: (sx, to), start: self.clock.now(),
                                         duration });
                self.scrolled = true;
                self.animation_frame = true;
                false
            }
            None => {
                self.state.scroll.insert(n.key, (sx, to));
                true
            }
        }
    }

    pub fn set_scroll_offset(&mut self, id: &str, x: f32, y: f32) {
        self.state.scroll.insert(id_key(id), (x, y));
        self.dirty = true;
    }

    /// Bounds (logical pixels) of the element with `.id(id)` as of the last
    /// frame. Works while rendering (views read where they were last
    /// placed); inside an event listener the frame is lent out, so a view
    /// keeps what it read while rendering.
    pub fn bounds_of(&self, id: &str) -> Option<Bounds<f32>> {
        self.scroll_read.set(true); // placements move with scroll
        let key = id_key(id);
        match self.frame.as_ref() {
            Some(f) => f.by_key().get(&key).map(|&i| f.nodes[i].bounds),
            // inside a listener the frame is lent out: the event's path still answers
            None => self.lent_bounds.iter().find(|(k, _)| *k == key).map(|&(_, b)| b),
        }
    }

    /// Put `text` on the clipboard.
    pub fn write_clipboard(&mut self, text: &str) {
        let c = CString::new(text.replace('\0', "")).unwrap_or_default();
        // SAFETY: a live context and a NUL-terminated string the kit copies.
        unsafe { sys::BK_clipboard_set(self.ctx, c.as_ptr()) };
    }

    /// The clipboard's text (what [`write_clipboard`](Self::write_clipboard) and the user's copies put there).
    pub fn read_clipboard(&self) -> Option<String> {
        // SAFETY: a live context; the kit returns a malloc'd NUL-terminated string (or NULL) we free.
        unsafe {
            let p = sys::BK_clipboard_get(self.ctx);
            if p.is_null() {
                return None;
            }
            let s = CStr::from_ptr(p).to_string_lossy().into_owned();
            free(p.cast());
            Some(s)
        }
    }

    // -- text selection ------------------------------------------------------------------------

    /// The text selected in the window's elements (press and drag, double-click, `Ctrl+A`), if any:
    /// block boundaries are newlines, whitespace is as rendered. Text fields keep their own selection.
    pub fn selected_text(&mut self) -> Option<String> {
        let f = self.frame.as_ref()?;
        self.selector.text(f, &self.caches.styles)
    }

    /// Select all the selectable text (like `Ctrl+A` on a page).
    pub fn select_all(&mut self) {
        if let Some(f) = &self.frame {
            if self.selector.select_all(f, &self.caches.styles) {
                self.dirty = true;
            }
        }
    }

    /// Drop the selection (like `Escape`).
    pub fn clear_selection(&mut self) {
        if self.selector.clear() {
            self.dirty = true;
        }
    }

    /// Call `f` with the selected text (None: nothing selected) whenever the user changes the selection:
    /// pressing, dragging, `Ctrl+A`, `Escape`, clicking away. Not for [`select_all`](Self::select_all)
    /// and [`clear_selection`](Self::clear_selection): their caller knows.
    pub fn on_selection_change(&mut self, f: impl FnMut(Option<&str>, &mut Window, &mut App) + 'static) {
        self.selection_hooks.push(Box::new(f));
    }

    /// The cursor the window shows over its elements now (the kit's cursor id: 0 arrow, 3 I-beam ...).
    #[doc(hidden)]
    pub const fn cursor_style(&self) -> i32 {
        self.cursor
    }

    /// A text field (one of Blender's text widgets) is being typed in. Keys typed there still reach
    /// the page's key listeners, as a browser's do: check this to keep single-key shortcuts from
    /// firing while the user types.
    pub fn text_field_focused(&self) -> bool {
        // SAFETY: a live context.
        unsafe { !sys::BK_focus(self.ctx).is_null() }
    }

    /// The selection changed by the user: draw it (the frame again, no render) and tell the hooks.
    fn selection_changed(&mut self, app: &mut App, repaint: bool) {
        if repaint {
            // at the next frame, as a redraw of Motion's values is: a native window draws what is
            // presented from its frame callback
            self.sel_repaint = true;
            self.animation_frame = true;
        }
        if self.selection_hooks.is_empty() {
            return;
        }
        let text = self.selected_text();
        let mut hooks = std::mem::take(&mut self.selection_hooks);
        for h in &mut hooks {
            h(text.as_deref(), self, app);
        }
        hooks.append(&mut self.selection_hooks);
        self.selection_hooks = hooks;
    }

    /// Nothing on screen answers to a press on `path` or to the focus moving to it: no listeners,
    /// `.active()` / `.focus()` refinements or Motion props on it, its ancestors or the element that
    /// loses the focus, and no `:active` / `:focus` rule in the sheet.
    fn press_is_quiet(&self, path: &[Key]) -> bool {
        let Some(f) = &self.frame else { return false };
        let plain = |k: &Key| f.by_key().get(k).and_then(|&i| f.el(i)).is_none_or(|e| !e.reactive);
        !self.caches.state_rules && self.state_read.get() & READ_FOCUS == 0 && path.iter().all(plain) && self.state.focused.as_ref().is_none_or(plain)
    }

    fn select_press(&mut self, x: f32, y: f32, shift: bool) -> bool {
        let Some(f) = &self.frame else { return false };
        let hit = f.hit_box(x, y, &self.caches.styles);
        let mut cx = Cx { frame: f, styles: &self.caches.styles, text: &mut self.caches.text };
        self.selector.press(&mut cx, x, y, hit, shift)
    }

    fn select_drag(&mut self, x: f32, y: f32) -> bool {
        let Some(f) = &self.frame else { return false };
        let mut cx = Cx { frame: f, styles: &self.caches.styles, text: &mut self.caches.text };
        self.selector.drag_to(&mut cx, x, y)
    }

    /// `Ctrl+A`, `Ctrl+C` and `Escape` on the document's selection: not while a text field has the
    /// keyboard (it owns them). (`Ctrl` is the Command key on macOS: the kit maps it.)
    fn selection_key(&mut self, ks: &Keystroke, app: &mut App) {
        if self.text_field_focused() {
            return;
        }
        let ctrl = ks.modifiers.control && !ks.modifiers.alt;
        let changed = match ks.key.to_ascii_lowercase().as_str() {
            "escape" => self.selector.clear(),
            "a" if ctrl => self.frame.as_ref().is_some_and(|f| self.selector.select_all(f, &self.caches.styles)),
            "c" if ctrl => {
                if let Some(t) = self.selected_text() {
                    self.write_clipboard(&t);
                }
                false
            }
            _ => false,
        };
        if changed {
            self.selection_changed(app, true);
        }
    }

    /// One UI unit (Blender's `UI_UNIT_Y`, a row of widgets) in logical pixels.
    pub fn ui_unit(&self) -> f32 {
        // SAFETY: a constant query.
        unsafe { sys::BK_unit() as f32 / self.scale }
    }

    fn viewport(&self) -> (f32, f32) {
        (self.phys.0 as f32 / self.scale, self.phys.1 as f32 / self.scale)
    }

    // -- render -----------------------------------------------------------------------------

    fn rebuild(&mut self, app: &mut App) {
        // the view is moved out while it renders (it gets `&mut Window`)
        let Some(view) = self.view.take() else { return };
        self.dirty = false;
        self.scrolled = false;
        self.scroll_read.set(false);
        self.state_read.set(0);
        let root = {
            let _s = span(Phase::Render);
            crate::hooks::begin(self.id);
            let mut root = view(self, app);
            crate::element::resolve_deferred(&mut root, self, app);
            root
        };
        if self.view.is_none() {
            self.view = Some(view);
        }
        let vp = self.viewport();
        let (canvas, scale) = (self.canvas, self.scale);
        let widgets = &mut self.widgets;
        let recycle = self.frame.take().unwrap_or_default();
        self.caches.anim.begin(self.clock.now());
        let mut frame = layout(root, vp, &self.state, &mut self.caches, recycle,
                           &mut |key, ask| match ask {
                               crate::widgets::WidgetAsk::Ensure(spec) => widgets.ensure(canvas, key, spec, scale),
                               crate::widgets::WidgetAsk::Height(w) => (0.0, widgets.height_for(key, w, scale)),
                           });
        // a checkbox / radio the app gave another `checked` since the user changed it is the app's
        // again, for good (see `UiState::checked`)
        let widgets = &self.widgets;
        self.state.checked.retain(|k, &mut (_, at)| widgets.nodes.get(k).is_none_or(|w| w.spec.attr_checked == at));
        crate::vlist::run_after_layout(&frame, self, app);
        // `data-stick="bottom"`: a scroll container that grew goes to its end
        // (`"bottom"`: only if it was at the end; `"always"`: whatever it showed)
        let mut stuck = false;
        let mut sticks = false;
        for i in 0..frame.nodes.len() {
            let Some(mode) = frame.attr(i, "data-stick") else { continue };
            sticks = true;
            let always = mode == "always";
            let n = &frame.nodes[i];
            let (key, range, pos) = (n.key, n.scroll_range.1, n.scroll_pos.1);
            let prev = self.stuck.insert(key, range).unwrap_or(0.0);
            if range > prev + 0.5 && (always || pos >= prev - 2.0) && pos < range - 0.5 {
                self.state.scroll.insert(key, (n.scroll_pos.0, range));
                stuck = true;
            }
        }
        // an element asked into view this render: its container scrolls, in
        // this very frame for a jump (placed before what follows placement:
        // in-view states, scroll-linked values, screens)
        if self.reveal(&frame) {
            crate::layout::replace(&mut frame, vp, &self.state, &mut self.caches);
        }
        // CSS animations / transitions still moving: render again next frame;
        // Motion's values alone: redraw (nothing running: no frames, so an
        // idle window costs no CPU)
        let (css, motion) = self.caches.anim.end();
        // `@container`: sizes changed under a query, or an element came into
        // view (`whileInView`): render once more
        let (mut in_view, flipped) = self.caches.anim.after_layout(&frame, &self.caches.styles, vp);
        // useScroll: the values follow the scroll; what they move draws again
        let scrolled = crate::motion::update_scroll(&frame);
        if flipped {
            // a layout animation starts where the element was seen: this frame too
            in_view |= self.redraw_motion_placed(&mut frame, vp) == Some(true);
        }
        let container_changed = self.caches.after_layout(&frame) | in_view;
        // screens: their kit nodes go where their elements are (the kit reports what moves)
        self.screens.after_layout(&frame, self.scale);
        // a scroll is a placement alone unless the view reads placements or a
        // container sticks to its end (animations follow it: see scroll_frame)
        self.scroll_free = !sticks && !self.scroll_read.get();
        self.present(app, frame, None);
        self.forget_removed(app);
        if stuck {
            self.dirty = true;
        }
        // state of components that didn't render is freed; effects run now
        crate::hooks::end();
        if container_changed {
            self.dirty = true;
        }
        // time alone moves what animates: redraws, not renders (a CSS value
        // that is more than paint makes its redraw a full frame)
        if css {
            self.request_css_frame();
        }
        if motion || scrolled {
            self.request_motion_frame();
        }
    }

    /// A frame of moving values alone (CSS animations', then Motion's): the last frame's elements, with
    /// their values set again and painted (no render, cascade or layout).
    fn redraw(&mut self, app: &mut App) {
        let Some(mut frame) = self.frame.take() else { return };
        let Some((css_dirty, css_running)) = self.sample_values(&mut frame) else {
            self.frame = Some(frame);
            self.dirty = true;
            return self.rebuild(app);
        };
        // no layout ran: the widgets are the last frame's
        self.widgets.keep();
        // the boxes the values changed: only they are painted again
        let mut dirty = self.caches.anim.take_redrawn();
        if dirty.len() != frame.nodes.len() {
            dirty.clear();
            dirty.resize(frame.nodes.len(), false);
        }
        dirty.iter_mut().zip(&css_dirty).for_each(|(d, c)| *d |= *c);
        self.present(app, frame, Some(&dirty));
        // the marks Motion keeps are its own: drop the CSS ones
        dirty.iter_mut().zip(&css_dirty).filter(|(_, c)| **c).for_each(|(d, _)| *d = false);
        self.caches.anim.give_redrawn(dirty);
        self.caches.anim.give_css_dirty(css_dirty);
        self.request_more_values(css_running);
    }

    /// The moving values of `frame` set again for now: CSS animations' first,
    /// Motion's on top (as at a full frame). The boxes CSS changed and whether
    /// its animations run on; `None` when a value changes layout (a render then).
    /// A box they moved into view or out of it (`whileInView`) renders next; the
    /// frame is a redraw still.
    fn sample_values(&mut self, frame: &mut Frame) -> Option<(Vec<bool>, bool)> {
        self.motion_frame = false;
        let css = std::mem::take(&mut self.css_frame);
        let vp = self.viewport();
        let moved_css = if css {
            self.caches.anim.redraw_css(self.clock.now(), frame, &mut self.caches.styles, vp)
        } else {
            Some((Vec::new(), false))
        }?;
        self.dirty |= self.redraw_motion_placed(frame, vp)?;
        Some(moved_css)
    }

    /// Motion's redraw of `frame`, placed again when a redraw moved a clip under one that stays:
    /// the moved values are in the styles, and the in-view states follow the new clips. `None`: it
    /// can't be redrawn (render); else whether anything changed.
    fn redraw_motion_placed(&mut self, frame: &mut Frame, vp: (f32, f32)) -> Option<bool> {
        let mut changed = self.caches.anim.redraw_motion(frame, &mut self.caches.styles, vp)?;
        if self.caches.anim.take_reclip() {
            crate::layout::replace(frame, vp, &self.state, &mut self.caches);
            changed |= self.caches.anim.in_view(frame, vp);
        }
        Some(changed)
    }

    /// Another frame of values when CSS animations (`css_running`) or Motion's run on.
    fn request_more_values(&mut self, css_running: bool) {
        if css_running {
            self.request_css_frame();
        }
        if crate::motion::active() {
            self.request_motion_frame();
        }
    }

    /// A frame after scrolling alone: the last frame placed again with the new offsets and painted
    /// (no render, cascade or layout), unless something follows the scroll positions: then a render.
    fn scroll_frame(&mut self, app: &mut App) {
        self.scrolled = false;
        let vp = self.viewport();
        let placed = self.scroll_free && match self.frame.take() {
            Some(mut frame) => {
                let ok = crate::layout::replace(&mut frame, vp, &self.state, &mut self.caches);
                self.frame = Some(frame);
                ok
            }
            None => false,
        };
        if !placed {
            self.dirty = true;
            return self.rebuild(app);
        }
        let Some(mut frame) = self.frame.take() else { return };
        self.screens.replace(&frame, self.scale); // a screen inside a scrolled box moved with it
        // what follows placement: Motion's in-view states (a change renders
        // next), its layout animations and the scroll-linked values (redraws).
        // Animations alone don't: their values are in the styles placement reads
        let (mut in_view, flipped) = self.caches.anim.after_layout(&frame, &self.caches.styles, vp);
        if flipped {
            in_view |= self.redraw_motion_placed(&mut frame, vp) == Some(true);
        }
        if crate::motion::update_scroll(&frame) {
            self.request_motion_frame();
        }
        if in_view {
            self.dirty = true;
        }
        // what animates moves on under the scroll (a glide scrolls every frame for
        // as long as it runs): this frame's values, over the new placement
        let mut sampled = None;
        if self.motion_frame {
            let Some((css_dirty, css_running)) = self.sample_values(&mut frame) else {
                self.frame = Some(frame);
                self.dirty = true;
                return self.rebuild(app);
            };
            self.caches.anim.give_css_dirty(css_dirty);
            sampled = Some(css_running);
        }
        self.widgets.keep(); // no layout ran: the widgets are the last frame's
        self.same_frame = true; // the same boxes: the selection's index of them stays
        self.present(app, frame, None);
        if let Some(css_running) = sampled {
            self.request_more_values(css_running);
        }
    }

    /// Draw again on the next display refresh, for Motion's values alone.
    fn request_motion_frame(&mut self) {
        self.motion_frame = true;
        self.animation_frame = true;
    }

    /// Draw again on the next display refresh, for CSS animations and
    /// transitions: their values move in a redraw ([`Animations::redraw_css`](crate::anim::Animations)).
    fn request_css_frame(&mut self) {
        self.css_frame = true;
        self.request_motion_frame();
    }

    /// Move the gliding scrolls on (see [`scroll_into_view`](Self::scroll_into_view)):
    /// each frame of a glide is a scroll frame.
    fn step_glides(&mut self) {
        if self.glides.is_empty() {
            return;
        }
        let now = self.clock.now();
        let ease = crate::anim::Timing::Bezier(0.25, 1.0, 0.5, 1.0);
        for g in &self.glides {
            let t = if g.duration > 0.0 { ((now - g.start) as f32 / g.duration).clamp(0.0, 1.0) } else { 1.0 };
            let k = ease.apply(t, false);
            let at = (g.from.0 + (g.to.0 - g.from.0) * k, g.from.1 + (g.to.1 - g.from.1) * k);
            self.state.scroll.insert(g.container, at);
        }
        self.glides.retain(|g| ((now - g.start) as f32) < g.duration);
        self.scrolled = true;
        if !self.glides.is_empty() {
            self.animation_frame = true;
        }
    }

    /// Render the window when it needs it: in full, or a redraw.
    fn update(&mut self, app: &mut App) {
        self.step_glides();
        if self.dirty {
            self.rebuild(app);
        } else if self.scrolled {
            self.scroll_frame(app);
        } else if self.sel_repaint {
            // the selection alone: the frame again, painted
            if let Some(frame) = self.frame.take() {
                self.widgets.keep(); // no layout ran: the widgets are the last frame's
                self.same_frame = true;
                self.present(app, frame, None);
            }
        } else if self.motion_frame {
            self.redraw(app);
        }
    }

    /// Paint `frame` and show it (the widgets placed, the hover updated).
    /// `dirty`: a redraw's boxes to paint again (the rest kept), when it can.
    fn present(&mut self, app: &mut App, mut frame: Frame, dirty: Option<&[bool]>) {
        FRAMES_PRESENTED.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        self.sel_repaint = false; // whatever is painted has the selection
        let vp = self.viewport();
        let scale = self.scale;
        // a selection follows its text into the new frame (and goes with it)
        if !std::mem::take(&mut self.same_frame) {
            self.selector.invalidate();
        }
        self.painter.sel = self.selector.paint(&frame, &self.caches.styles);
        {
            let _s = span(Phase::Paint);
            let gpu = self.is_gpu();
            let p = &mut self.painter;
            p.set_gpu(gpu);
            // a view transition paints the frame whole, its named boxes hidden under its pictures
            let mut hide = Vec::new();
            let now = self.clock.now();
            let moving = self.transition.as_mut().is_some_and(|t| {
                let mut shot = crate::transition::Shot { frame: &frame, styles: &self.caches.styles,
                                                         text: &mut self.caches.text, vp, scale, app };
                t.paint(&mut shot, &self.caches.author.sheet.transitions, now, &mut hide)
            });
            if !moving {
                self.transition = None;
            }
            for &i in &hide {
                frame.nodes[i].visible = false;
            }
            let kept = !moving
                && dirty.is_some_and(|d| p.repaint(&frame, &self.caches.styles, &mut self.caches.text, d, vp, app));
            if !kept {
                p.reset();
                paint_frame(&frame, &self.caches.styles, &mut self.caches.text, p, vp, self.scale, app);
            }
            if let Some(t) = self.transition.as_ref().filter(|_| moving) {
                t.draw(p, now);
            }
            for &i in &hide {
                frame.nodes[i].visible = true;
            }
            if moving {
                // the next frame of the transition: the same frame, painted again
                self.sel_repaint = true;
                self.animation_frame = true;
            }
        }
        {
            let _s = span(Phase::Flush);
            self.grow_canvases(self.painter.segment_count());
            self.painter.flush(&self.canvases, scale);
        }
        let _rest = span(Phase::Widgets);
        let mut at = HashMap::new(); // empty (no allocation) without form widgets
        if frame.nodes.iter().any(|n| matches!(n.content, Content::Widget(_))) {
            let order = self.painter.widget_order();
            for n in &frame.nodes {
                if matches!(n.content, Content::Widget(_)) {
                    let (seg, rank) = order.get(&n.key).copied().unwrap_or((0, u32::MAX));
                    at.insert(n.key, (n.bounds, n.clip, n.visible, seg, rank));
                }
            }
        }
        self.widgets.place(&at, &self.canvases, scale);
        // an opaque canvas background covers the window: the kit can skip its clear
        let styles = &self.caches.styles;
        let opaque = frame.nodes.first().is_some_and(|h| styles[h.style].opacity >= 1.0)
            && crate::paint::canvas_background(&frame, styles).1.a >= 1.0;
        if opaque != self.opaque {
            self.opaque = opaque;
            unsafe { sys::BK_set_num(self.root, c"opaque".as_ptr(), opaque as i32 as f64) };
        }
        if let Some(t) = frame.title.clone() {
            self.set_window_title(&t);
        }
        #[cfg(feature = "a11y")]
        self.publish_a11y(&frame);
        self.frame = Some(frame);
        // what the layout moved under a pointer that stayed still is entered or left
        self.update_hover(app, false);
        self.fire_scrolls(app);
        drop(_rest);
        prof::frame_done();
    }

    // -- events ------------------------------------------------------------------------------

    /// At least `n` canvases (the ones over the first are made here, above
    /// the rest, and take the pointer only where they have items).
    fn grow_canvases(&mut self, n: usize) {
        while self.canvases.len() < n {
            // SAFETY: a fresh canvas added to our root, like the first one.
            let c = unsafe {
                let c = sys::BK_node_new(c"canvas".as_ptr());
                sys::BK_node_add(self.root, c, -1);
                sys::BK_geom_set_manager(c, sys::BK_GEOM_PLACE);
                sys::BK_geom_set(c, c"relwidth".as_ptr(), 1.0);
                sys::BK_geom_set(c, c"relheight".as_ptr(), 1.0);
                sys::BK_set_num(c, c"events".as_ptr(), 1.0);
                sys::BK_set_num(c, c"hititems".as_ptr(), 1.0);
                sys::BK_set_num(c, c"cursor".as_ptr(), f64::from(self.cursor));
                c
            };
            self.overlays.push(c);
            self.canvases.push(c);
        }
    }

    /// Element keys from the root to the deepest element under the pointer.
    fn path_at(&self, x: f32, y: f32) -> Vec<Key> {
        let Some(f) = &self.frame else { return Vec::new() };
        f.hit_path(x, y, &self.caches.styles).into_iter().map(|i| f.nodes[i].key).collect()
    }

    /// The element the event its listeners are running for fired on (`None` outside one).
    pub(crate) fn event_target(&self) -> Option<Key> {
        self.lent_bounds.first().map(|&(k, _)| k)
    }

    /// Deliver `e` of `kind` to element `target` as HTML does: down from the root to it (the capture pass),
    /// then back up (the bubbling pass, when `kind` bubbles), with the window-wide listeners heard before and
    /// after both. A listener that calls `stop_propagation` ends the pass it is in. The frame is moved out
    /// of the window meanwhile, so listeners can take `&mut Window` while the frame lends them out: no
    /// copies, no `Rc` (the bounds of the event's path stay readable: `bounds_of`). True when a listener
    /// stopped the propagation (it took the event).
    fn fire<E: 'static>(&mut self, app: &mut App, kind: EventKind, target: Key, e: &E) -> bool {
        let Some(frame) = self.frame.take() else { return false };
        if frame.listen & kind.bit() == 0 {
            self.frame = Some(frame);
            return false;
        }
        app.propagate = true;
        let entries = |i: usize| frame.el(i).and_then(|el| el.listeners.as_deref()).map(|l| &l.entries[..]);
        let path = frame.path_to(frame.by_key().get(&target).copied());
        // reused, so an event allocates nothing once it has grown
        self.lent_bounds.clear();
        self.lent_bounds.extend(path.iter().rev().map(|&i| (frame.nodes[i].key, frame.nodes[i].bounds)));
        let any = e as &dyn Any;
        // every element of the window, for the listeners that hear all of it
        let wide = |this: &mut Self, app: &mut App, phase: DispatchPhase| {
            for &i in &frame.wide {
                let i = i as usize;
                for en in entries(i).into_iter().flatten().filter(|en| en.wide && en.kind == kind) {
                    this.current = Some(frame.nodes[i].key);
                    (en.call)(any, phase, this, app);
                    if !app.propagate {
                        return;
                    }
                }
            }
        };
        wide(self, app, DispatchPhase::Capture);
        'pass: {
            if !app.propagate {
                break 'pass;
            }
            for &i in &path {
                for en in entries(i).into_iter().flatten().filter(|en| en.capture && !en.wide && en.kind == kind) {
                    self.current = Some(frame.nodes[i].key);
                    (en.call)(any, DispatchPhase::Capture, self, app);
                    if !app.propagate {
                        break 'pass;
                    }
                }
            }
            let up = if kind.bubbles() { path.len() } else { path.len().min(1) };
            for &i in path.iter().rev().take(up) {
                for en in entries(i).into_iter().flatten().filter(|en| !en.capture && !en.wide && en.kind == kind) {
                    self.current = Some(frame.nodes[i].key);
                    (en.call)(any, DispatchPhase::Bubble, self, app);
                    if !app.propagate {
                        break 'pass;
                    }
                }
            }
            wide(self, app, DispatchPhase::Bubble);
        }
        let stopped = !app.propagate;
        app.propagate = true;
        self.current = None;
        self.lent_bounds.clear();
        if self.frame.is_none() {
            self.frame = Some(frame);
        }
        self.fire_later(app);
        stopped
    }

    /// The capture events that listeners raised, once the event they were in is done.
    fn fire_later(&mut self, app: &mut App) {
        while self.frame.is_some() && !self.later.is_empty() {
            let (kind, key, e) = self.later.remove(0);
            self.fire(app, kind, key, &e);
        }
    }

    /// Hold the pointer for the element whose listener is running (HTML's `setPointerCapture`): its
    /// pointer and mouse events go to that element, also after the pointer left it or the window, until the
    /// button is released or [`release_pointer_capture`](Self::release_pointer_capture). A drag starts here.
    pub fn set_pointer_capture(&mut self, app: &mut App) {
        if let Some(key) = self.current {
            self.capture_key(app, key);
        }
    }

    /// [`set_pointer_capture`](Self::set_pointer_capture) for the element `id` (its `.id(..)`), from outside
    /// its listener (a binding that learns of the request after the event).
    pub fn capture_pointer_for(&mut self, app: &mut App, id: &str) {
        self.capture_key(app, id_key(id));
    }

    fn capture_key(&mut self, app: &mut App, key: Key) {
        if self.capture != Some(key) {
            self.release_pointer_capture(app);
            self.capture = Some(key);
            let e = self.pointer_event(None, self.held(), 0);
            self.later.push((EventKind::GotPointerCapture, key, e));
            self.fire_later(app);
        }
    }

    /// Let go of the pointer (HTML's `releasePointerCapture`).
    pub fn release_pointer_capture(&mut self, app: &mut App) {
        if let Some(key) = self.capture.take() {
            let e = self.pointer_event(None, self.held(), 0);
            self.later.push((EventKind::LostPointerCapture, key, e));
            self.fire_later(app);
        }
    }

    /// Whether the element `id` (its `.id(..)`) holds the pointer.
    #[must_use]
    pub fn has_pointer_capture(&self, id: &str) -> bool {
        self.capture == Some(id_key(id))
    }

    /// The button held down, if any.
    fn held(&self) -> Option<MouseButton> {
        (!self.pressed.is_empty()).then_some(self.press_button)
    }

    /// A pointer event at the pointer's place.
    fn pointer_event(&self, button: Option<MouseButton>, held: Option<MouseButton>, clicks: usize) -> PointerEvent {
        PointerEvent::mouse(point(px(self.pointer.0), px(self.pointer.1)), button, held, Modifiers::default(), clicks)
    }

    /// Where the pointer's events go: the element that holds it, else the one under it (else the root,
    /// which the window-wide listeners hear through).
    fn pointer_target(&self) -> Key {
        self.capture.or_else(|| self.hover_path.last().copied()).unwrap_or(ROOT_KEY)
    }

    /// The element keys under the pointer (into `path`), and (when the pointer moved: `measure`) whether
    /// it is over text that can be selected.
    fn hover_target(&mut self, measure: bool, path: &mut Vec<Key>) -> Option<bool> {
        path.clear();
        let Some(f) = &self.frame else { return Some(false) };
        let (x, y) = self.pointer;
        let hit = f.hit_box(x, y, &self.caches.styles);
        path.extend(f.path_to(hit).into_iter().map(|i| f.nodes[i].key));
        measure.then(|| hit.is_some_and(|i| {
            let mut cx = Cx { frame: f, styles: &self.caches.styles, text: &mut self.caches.text };
            crate::selection::is_text(&mut cx, i, x, y)
        }))
    }

    /// The pointer's cursor: that of the deepest element under it that sets one
    /// (`cursor: auto` sets none), else the text cursor over selectable text.
    /// `over_text` None: not measured (a frame, not a pointer move): keep the I-beam if it showed.
    fn set_cursor(&mut self, path: &[Key], over_text: Option<bool>) {
        let Some(f) = &self.frame else { return };
        let text = over_text.unwrap_or(self.cursor == crate::style::CURSOR_TEXT);
        let cursor = path.iter().rev()
            .find_map(|k| f.by_key().get(k).and_then(|&i| self.caches.styles[f.nodes[i].style].cursor))
            .unwrap_or(if text { crate::style::CURSOR_TEXT } else { 0 });
        if cursor != self.cursor {
            self.cursor = cursor;
            for &c in &self.canvases {
                unsafe { sys::BK_set_num(c, c"cursor".as_ptr(), cursor as f64) };
            }
        }
    }

    /// What is under the pointer: its cursor, `:hover`, and the elements it entered and left hear so
    /// (`measure`: it moved, so whether it is over text is measured again).
    fn update_hover(&mut self, app: &mut App, measure: bool) {
        // the elements under the pointer, into a buffer that stays: most moves end where this returns
        let mut path = std::mem::take(&mut self.path_buf);
        let over_text = self.hover_target(measure, &mut path);
        self.set_cursor(&path, over_text);
        if path == self.hover_path {
            self.path_buf = path;
            return;
        }
        let now: KeySet<Key> = path.iter().copied().collect();
        let entered: Vec<Key> = path.iter().copied().filter(|k| !self.state.hovered.contains(k)).collect();
        let left: Vec<Key> = self.hover_path.iter().rev().copied().filter(|k| !now.contains(k)).collect();
        let (old_deepest, new_deepest) = (self.hover_path.last().copied(), path.last().copied());
        // a restyle (a render) only where something answers to the pointer: a `:hover` rule in the
        // sheet, or a `.hover()` refinement / Motion props on an element it entered or left, or a view
        // that read `is_hovered`
        let restyle = self.state_read.get() & READ_HOVER != 0 || self.frame.as_ref().is_some_and(|f| {
            entered.iter().chain(&left).any(|k| {
                f.by_key().get(k).is_some_and(|&i| self.caches.state_rules || f.el(i).is_none_or(|e| e.reactive))
            })
        });
        self.state.hovered = now;
        // the path becomes the hover path; the old one's buffer is the next scratch
        self.path_buf = std::mem::replace(&mut self.hover_path, path);
        // out, leave, over, enter (HTML's order); enter and leave go to each element on its own, so a
        // child doesn't make its parent leave
        let held = self.held();
        let pe = self.pointer_event(None, held, 0);
        let me = MouseMoveEvent { position: pe.position, pressed_button: held, modifiers: Modifiers::default() };
        if let Some(k) = old_deepest.filter(|_| new_deepest != old_deepest) {
            self.fire(app, EventKind::PointerOut, k, &pe);
            self.fire(app, EventKind::MouseOut, k, &me);
        }
        for &k in &left {
            self.fire(app, EventKind::PointerLeave, k, &pe);
            self.fire(app, EventKind::MouseLeave, k, &me);
            self.fire(app, EventKind::Hover, k, &false);
        }
        if let Some(k) = new_deepest.filter(|_| new_deepest != old_deepest) {
            self.fire(app, EventKind::PointerOver, k, &pe);
            self.fire(app, EventKind::MouseOver, k, &me);
        }
        for &k in &entered {
            self.fire(app, EventKind::PointerEnter, k, &pe);
            self.fire(app, EventKind::MouseEnter, k, &me);
            self.fire(app, EventKind::Hover, k, &true);
        }
        if restyle {
            self.dirty = true;
        }
    }

    /// The modifiers and the button of a pointer event's text.
    fn parse_pointer(&self, text: &str) -> (Modifiers, MouseButton) {
        let mut it = text.split_whitespace().map(|v| v.parse::<i32>().unwrap_or(0));
        let mods = it.next().unwrap_or(0);
        let button = match it.next().unwrap_or(1) {
            2 => MouseButton::Middle,
            3 => MouseButton::Right,
            _ => MouseButton::Left,
        };
        (Modifiers::from_kit(mods), button)
    }

    /// The press of `button` ends: released, cancelled by the host or taken by the window manager for
    /// a move (Blender's `button_activate_exit`). Nothing is pressed or `:active`, a drag (Motion's, a
    /// selection's, a scrollbar's, which stays where it is) lets go, and a press that rendered renders
    /// again. True when a left press selected text (which is not a click).
    fn end_press(&mut self, button: MouseButton) -> bool {
        self.sb_drag = None;
        self.pressed.clear();
        self.state.active.clear();
        self.dirty |= !std::mem::replace(&mut self.press_quiet, true);
        self.caches.anim.pointer_up();
        button == MouseButton::Left && self.selector.release()
    }

    /// Move the focus to the element at the end of `path` (its ancestors, for `:focus-within`) without
    /// events; the element that had it.
    fn focus_path(&mut self, path: &[Key]) -> Option<Key> {
        let old = std::mem::replace(&mut self.state.focused, path.last().copied());
        self.state.focus_path = path.iter().copied().collect();
        self.state.focused_id = self.frame.as_ref().and_then(|f| {
            let i = *f.by_key().get(path.last()?)?;
            f.el(i)?.id.as_ref().map(|s| s.to_string())
        });
        old
    }

    /// Focus the element at the end of `path` (its ancestors, for `:focus-within`): `blur` and `focusout` for
    /// the one that had it, `focus` and `focusin` for the one that has it now.
    fn set_focus(&mut self, app: &mut App, path: &[Key]) {
        let (old, new) = (self.focus_path(path), path.last().copied());
        if old != new {
            if let Some(k) = old {
                self.fire(app, EventKind::Blur, k, &FocusEvent);
                self.fire(app, EventKind::FocusOut, k, &FocusEvent);
            }
            if let Some(k) = new {
                self.fire(app, EventKind::Focus, k, &FocusEvent);
                self.fire(app, EventKind::FocusIn, k, &FocusEvent);
            }
        }
    }

    fn dispatch(&mut self, ev: RawEvent, app: &mut App) {
        #[cfg(feature = "a11y")]
        match ev.kind {
            sys::BK_EV_A11Y_TREE => {
                // a screen reader reads the window: the page's subtree from now on, this frame's first
                self.a11y.get_or_insert_default();
                if let Some(frame) = self.frame.take() {
                    self.publish_a11y(&frame);
                    self.frame = Some(frame);
                }
                return;
            }
            sys::BK_EV_A11Y_ACTION => return self.a11y_request(&ev, app),
            _ => {}
        }
        if !self.canvases.contains(&ev.node) {
            if let Some(target) = self.screens.target(ev.node) {
                self.screen_event(&target, &ev, app);
                return;
            }
            self.widget_event(ev, app);
            return;
        }
        // a text field took the focus: its own selection counts, the document's goes
        if self.selector.has_selection() && self.text_field_focused() && self.selector.clear() {
            self.selection_changed(app, true);
        }
        let (x, y) = (ev.a as f32 / self.scale, ev.b as f32 / self.scale);
        match ev.kind {
            sys::BK_EV_CONFIGURE => {
                let phys = (ev.a as i32, ev.b as i32);
                if phys != self.phys && phys.0 > 0 && phys.1 > 0 {
                    self.phys = phys;
                    self.dirty = true;
                }
            }
            // an enter carries no position (the kit's `set_hover`): the motion after it does
            sys::BK_EV_ENTER => {}
            sys::BK_EV_MOTION => {
                self.pointer = (x, y);
                if self.sb_drag.is_some() {
                    self.drag_scrollbar(y);
                    return;
                }
                if self.caches.anim.pointer_move(x, y) {
                    self.request_motion_frame(); // a drag: a redraw, no render
                }
                self.update_hover(app, true);
                if self.select_drag(x, y) {
                    self.selection_changed(app, true);
                }
                let (modifiers, _) = self.parse_pointer(&ev.text);
                let held = self.held();
                let target = self.pointer_target();
                let mut pe = self.pointer_event(None, held, 0);
                pe.modifiers = modifiers;
                self.fire(app, EventKind::PointerMove, target, &pe);
                let e = MouseMoveEvent { position: point(px(x), px(y)), pressed_button: held, modifiers };
                self.fire(app, EventKind::MouseMove, target, &e);
                // Blender's press-drag: the first move past the drag threshold of a held button
                if let Some(button) = held.filter(|_| !self.press_drag_sent) {
                    let (sx, sy) = self.press_start;
                    if (x - sx).abs() > DRAG_THRESHOLD || (y - sy).abs() > DRAG_THRESHOLD {
                        self.press_drag_sent = true;
                        let (start, at) = (point(px(sx), px(sy)), point(px(x), px(y)));
                        let e = PressDragEvent { button, start, position: at, modifiers, direction: DragDirection::of(start, at) };
                        self.fire(app, EventKind::PressDrag, target, &e);
                    }
                }
            }
            sys::BK_EV_LEAVE => {
                self.pointer = (-1.0, -1.0);
                self.update_hover(app, true);
            }
            sys::BK_EV_PRESS | sys::BK_EV_DOUBLE => {
                self.pointer = (x, y);
                let (modifiers, button) = self.parse_pointer(&ev.text);
                // a title bar's drag area: the window manager moves the window (Windows does it
                // without the press reaching the app)
                if button == MouseButton::Left && self.control_area_at(x, y) == Some(WindowControlArea::Drag) {
                    if ev.kind == sys::BK_EV_DOUBLE {
                        self.zoom_window();
                    } else {
                        self.start_window_move();
                    }
                    return;
                }
                let path = self.path_at(x, y);
                if button == MouseButton::Left && self.grab_scrollbar(&path, x, y) {
                    return;
                }
                let selected = button == MouseButton::Left && self.select_press(x, y, modifiers.shift);
                let quiet = self.press_is_quiet(&path);
                self.press_quiet = quiet;
                self.press_button = button;
                self.press_count = if ev.kind == sys::BK_EV_DOUBLE { 2 } else { 1 };
                self.press_start = (x, y);
                self.press_drag_sent = false;
                self.state.active = path.iter().copied().collect();
                self.set_focus(app, &path);
                self.pressed.clone_from(&path);
                self.dirty |= !quiet;
                self.caches.anim.pointer_down(&path, x, y);
                let target = self.capture.or(path.last().copied()).unwrap_or(ROOT_KEY);
                let mut pe = self.pointer_event(Some(button), Some(button), self.press_count);
                pe.modifiers = modifiers;
                self.fire(app, EventKind::PointerDown, target, &pe);
                let e = MouseDownEvent { button, position: point(px(x), px(y)), modifiers, click_count: self.press_count };
                self.fire(app, EventKind::MouseDown, target, &e);
                if selected {
                    // a press that renders draws the selection with it; a quiet one draws it now
                    self.selection_changed(app, quiet);
                }
            }
            sys::BK_EV_RELEASE => {
                if self.sb_drag.take().is_some() {
                    return;
                }
                self.pointer = (x, y);
                let (modifiers, button) = self.parse_pointer(&ev.text);
                let path = self.capture.map_or_else(|| self.path_at(x, y), |k| self.path_to_key(k));
                let target = path.last().copied().unwrap_or(ROOT_KEY);
                let mut pe = self.pointer_event(Some(button), None, self.press_count);
                pe.modifiers = modifiers;
                self.fire(app, EventKind::PointerUp, target, &pe);
                let up = MouseUpEvent { button, position: point(px(x), px(y)), modifiers };
                self.fire(app, EventKind::MouseUp, target, &up);
                // click: the deepest element under both press and release
                let common = if self.pressed.is_empty() && button != MouseButton::Left {
                    path.last().copied() // a button a window reports by its release alone
                } else {
                    self.pressed.iter().zip(&path).take_while(|(a, b)| a == b).map(|(a, _)| *a).last()
                };
                // a press that selected text is a selection, not a click (as in a browser)
                let dragged = self.end_press(button);
                if let (Some(target), false) = (common, dragged) {
                    let down = MouseDownEvent { button, position: point(px(x), px(y)), modifiers, click_count: self.press_count };
                    let e = ClickEvent { down, up };
                    if button == MouseButton::Left {
                        self.click(app, target, &e);
                    } else {
                        self.fire(app, EventKind::AuxClick, target, &e);
                        if button == MouseButton::Right {
                            let at = point(px(x), px(y));
                            self.fire(app, EventKind::ContextMenu, target, &ContextMenuEvent { position: at, modifiers });
                        }
                    }
                }
                // the pointer is let go when its button is
                self.release_pointer_capture(app);
            }
            sys::BK_EV_WHEEL => {
                let (modifiers, _) = self.parse_pointer(&ev.text);
                // after "mods button": how the scroll came (BK_scroll_phase), then where, relative
                // to the canvas (a touch scroll moves no pointer there)
                let mut rest = ev.text.split_whitespace().skip(2).map(|v| v.parse::<i32>().unwrap_or(0));
                let phase = ScrollPhase::from_kit(rest.next().unwrap_or(0));
                let (px_, py_) = match (rest.next(), rest.next()) {
                    (Some(ex), Some(ey)) => (ex as f32 / self.scale, ey as f32 / self.scale),
                    _ => self.pointer,
                };
                // `b`: the exact device pixels of a touch drag (BK_scroll_px), 0 for a wheel notch
                let pixels = (ev.b != 0.0).then(|| ev.b as f32 / self.scale);
                let position = point(px(px_), px(py_));
                let path = self.path_at(px_, py_);
                let target = path.last().copied().unwrap_or(ROOT_KEY);
                if phase != ScrollPhase::Wheel {
                    // a finger drags the content: `on_pan` first, which may take it (a sideways
                    // touch scroll comes as Shift + wheel)
                    let d = px(pixels.unwrap_or(0.0));
                    let delta = if modifiers.shift { point(d, px(0.0)) } else { point(px(0.0), d) };
                    if self.fire(app, EventKind::Pan, target, &PanEvent { position, delta, phase, modifiers }) {
                        return;
                    }
                }
                let e = ScrollWheelEvent { position, delta: ev.a as f32, pixels, phase, modifiers };
                self.fire(app, EventKind::Wheel, target, &e);
                // `scroll` follows the frame that moves it; a touch drag follows the finger: it snaps no step (browsers snap such a gesture at its end)
                self.scroll_by(&path, -pixels.unwrap_or(ev.a as f32 * 40.0), modifiers.shift, pixels.is_none());
            }
            sys::BK_EV_GESTURE => self.gesture(app, &ev),
            sys::BK_EV_KEY | sys::BK_EV_KEYRELEASE => {
                let mut parts = ev.text.splitn(2, '\t');
                let ch = parts.next().unwrap_or("").to_owned();
                let key = parts.next().unwrap_or("").to_owned();
                let ks = Keystroke { key, key_char: (!ch.is_empty()).then_some(ch),
                                     modifiers: Modifiers::from_kit(ev.a as i32) };
                let target = self.state.focused.or_else(|| self.state_deepest()).unwrap_or(ROOT_KEY);
                if ev.kind == sys::BK_EV_KEY {
                    self.selection_key(&ks, app);
                    let page = match ks.key.to_ascii_lowercase().as_str() {
                        "pagedown" => 1.0,
                        "pageup" => -1.0,
                        _ => 0.0,
                    };
                    // `keypress`: a key that types a character
                    let types = ks.key_char.is_some() && !ks.modifiers.control && !ks.modifiers.alt;
                    let down = KeyDownEvent { keystroke: ks };
                    app.default_prevented = false;
                    self.fire(app, EventKind::KeyDown, target, &down);
                    // as in a browser, a `keydown` whose default was prevented types nothing: no `keypress`
                    if types && !app.default_prevented {
                        self.fire(app, EventKind::KeyPress, target, &down);
                    }
                    if std::mem::take(&mut app.default_prevented) && !self.ctx.is_null() {
                        // SAFETY: the live context; the kit is delivering this key (BK_key) now.
                        unsafe { sys::BK_key_taken(self.ctx) };
                    }
                    if page != 0.0 {
                        self.scroll_page(page);
                    }
                } else {
                    self.fire(app, EventKind::KeyUp, target, &KeyUpEvent { keystroke: ks });
                }
            }
            _ => {}
        }
    }

    /// A pinch or a turn (`BK_EV_GESTURE`: text "mods kind x y", device pixels from the canvas; `a`
    /// the amount): `on_pinch` / `on_rotate` from the element under its centre up.
    fn gesture(&mut self, app: &mut App, ev: &RawEvent) {
        let mut it = ev.text.split_whitespace().map(|v| v.parse::<i32>().unwrap_or(0));
        let (mods, kind) = (it.next().unwrap_or(0), it.next().unwrap_or(0));
        let (x, y) = (it.next().unwrap_or(0) as f32 / self.scale, it.next().unwrap_or(0) as f32 / self.scale);
        let target = self.path_at(x, y).last().copied().unwrap_or(ROOT_KEY);
        let (position, modifiers, touch) = (point(px(x), px(y)), Modifiers::from_kit(mods), mods & sys::BK_FROM_TOUCH != 0);
        let amount = ev.a as f32;
        match kind {
            sys::BK_GESTURE_MAGNIFY => {
                self.fire(app, EventKind::Pinch, target, &PinchEvent { position, delta: amount, modifiers, touch });
            }
            sys::BK_GESTURE_ROTATE => {
                self.fire(app, EventKind::Rotate, target, &RotateEvent { position, degrees: amount, modifiers, touch });
            }
            _ => {}
        }
    }

    /// A click on element `target`: its `<details>` opens or closes, its listeners run (`dblclick` too
    /// for the second press), its window control acts.
    fn click(&mut self, app: &mut App, target: Key, e: &ClickEvent) {
        self.toggle_details(target);
        self.fire(app, EventKind::Click, target, e);
        if e.down.click_count == 2 {
            self.fire(app, EventKind::DoubleClick, target, e);
        }
        self.run_control_area(target);
    }

    /// The page as the canvas's accessibility subtree, while a screen reader reads the window.
    #[cfg(feature = "a11y")]
    fn publish_a11y(&mut self, frame: &Frame) {
        let Some(a) = self.a11y.as_deref_mut() else { return };
        let (widgets, screens) = (&self.widgets, &self.screens);
        let kit = |k: Key| widgets.nodes.get(&k).map(|w| w.node).or_else(|| screens.node(k));
        let page = crate::a11y::Page { frame, state: &self.state, kit: &kit, scale: self.scale };
        a.update(&page, self.canvas);
    }

    /// A screen reader's request for an element of the page, done as the input it stands for.
    #[cfg(feature = "a11y")]
    fn a11y_request(&mut self, ev: &RawEvent, app: &mut App) {
        use crate::a11y::Action;
        let Some(f) = &self.frame else { return };
        let Some(&i) = f.by_key().get(&ev.key) else { return };
        let b = f.nodes[i].bounds;
        let at = point(px(b.origin.x + b.size.width / 2.0), px(b.origin.y + b.size.height / 2.0));
        match Action::from_raw(ev.a as i32) {
            Some(Action::Click) => {
                let down = MouseDownEvent { button: MouseButton::Left, position: at, modifiers: Modifiers::default(),
                                            click_count: 1 };
                let up = MouseUpEvent { button: MouseButton::Left, position: at, modifiers: Modifiers::default() };
                self.click(app, ev.key, &ClickEvent { down, up });
            }
            Some(Action::Focus) => {
                let path: Vec<Key> = f.path_to(Some(i)).into_iter().map(|j| f.nodes[j].key).collect();
                self.set_focus(app, &path);
            }
            Some(Action::Expand | Action::Collapse) => self.toggle_details(ev.key),
            Some(Action::ScrollIntoView) => self.reveal = Some(Reveal { key: ev.key, at: 0.0, glide: None }),
            _ => return,
        }
        self.dirty = true;
    }

    /// What left the tree no longer holds the pointer or the focus: the pointer is cancelled (`pointercancel`
    /// and `lostpointercapture` go to the root), and key events go back to where the pointer is.
    fn forget_removed(&mut self, app: &mut App) {
        let Some(frame) = &self.frame else { return };
        let gone = |k: Option<Key>| k.is_some_and(|k| !frame.by_key().contains_key(&k));
        let (capture_gone, focus_gone) = (gone(self.capture), gone(self.state.focused));
        if capture_gone {
            self.capture = None;
            let e = self.pointer_event(None, self.held(), 0);
            self.fire(app, EventKind::PointerCancel, ROOT_KEY, &e);
            self.fire(app, EventKind::LostPointerCapture, ROOT_KEY, &e);
        }
        if focus_gone {
            self.state.focused = None;
            self.state.focus_path.clear();
            self.state.focused_id = None;
        }
    }

    /// `scroll` for every scroll container that moved since its last one, whatever moved it (the wheel,
    /// `set_scroll_offset`, a glide, a layout that changed its range). Costs nothing unless something listens.
    fn fire_scrolls(&mut self, app: &mut App) {
        let Some(frame) = &self.frame else { return };
        if frame.listen & EventKind::Scroll.bit() == 0 {
            return;
        }
        let mut moved: smallvec::SmallVec<[(Key, (f32, f32)); 4]> = smallvec::SmallVec::new();
        for n in frame.nodes.iter().filter(|n| n.scroll_range != (0.0, 0.0)) {
            match self.scroll_seen.insert(n.key, n.scroll_pos) {
                Some(was) if was != n.scroll_pos => moved.push((n.key, n.scroll_pos)),
                _ => {}
            }
        }
        for (key, at) in moved {
            self.fire(app, EventKind::Scroll, key, &ScrollEvent { offset: point(px(at.0), px(at.1)) });
        }
    }

    /// Element keys from the root to the element `key` (empty when it isn't in this frame).
    fn path_to_key(&self, key: Key) -> Vec<Key> {
        let Some(f) = &self.frame else { return Vec::new() };
        f.path_to(f.by_key().get(&key).copied()).into_iter().map(|i| f.nodes[i].key).collect()
    }

    fn state_deepest(&self) -> Option<Key> {
        self.path_at(self.pointer.0, self.pointer.1).last().copied()
    }

    /// `<summary>` clicks open and close their `<details>`.
    fn toggle_details(&mut self, target: Key) {
        let Some(f) = &self.frame else { return };
        let mut cur = f.by_key().get(&target).copied();
        while let Some(i) = cur {
            if f.el(i).is_some_and(|e| e.tag == "summary") {
                let details = f.nodes[i].parent.filter(|&p| f.el(p).is_some_and(|e| e.tag == "details"));
                if let Some(p) = details {
                    let key = f.nodes[p].key;
                    let open = self.state.details_open.get(&key).copied()
                        .unwrap_or(f.attr(p, "open").is_some());
                    self.state.details_open.insert(key, !open);
                    self.dirty = true;
                }
                return;
            }
            cur = f.nodes[i].parent;
        }
    }

    /// PageUp / PageDown: a page of the innermost scroll container under the pointer.
    fn scroll_page(&mut self, dir: f32) {
        let path = self.path_at(self.pointer.0, self.pointer.1);
        let Some(f) = &self.frame else { return };
        let page = path.iter().rev()
            .filter_map(|k| f.by_key().get(k))
            .map(|&i| &f.nodes[i])
            .find(|n| n.scroll_range.1 > 0.0)
            .map(|n| n.bounds.size.height * 0.9);
        if let Some(page) = page {
            self.scroll_by(&path, dir * page, false, true);
        }
    }

    /// The vertical scrollbar of a container along `path`: (key, thumb top, thumb height, track top, track height).
    fn scrollbar_at(&self, path: &[Key], x: f32) -> Option<(Key, f32, f32, f32, f32)> {
        let f = self.frame.as_ref()?;
        for key in path.iter().rev() {
            let Some(&i) = f.by_key().get(key) else { continue };
            let n = &f.nodes[i];
            // `scrollbar-width: none`: no bar to grab
            if self.caches.styles[n.style].layout.scrollbar_width <= 0.0 {
                continue;
            }
            let ry = n.scroll_range.1;
            let b = n.bounds;
            if ry <= 0.0 || x < b.origin.x + b.size.width - 10.0 || x > b.origin.x + b.size.width {
                continue;
            }
            let h = b.size.height;
            let th = (h * h / (h + ry)).max(16.0);
            let ty = b.origin.y + (h - th) * (n.scroll_pos.1 / ry);
            return Some((*key, ty, th, b.origin.y, h));
        }
        None
    }

    /// A press on a scrollbar starts a drag (on the track it first centers the thumb there).
    fn grab_scrollbar(&mut self, path: &[Key], x: f32, y: f32) -> bool {
        let Some((key, ty, th, _, _)) = self.scrollbar_at(path, x) else { return false };
        let grab = if y >= ty && y <= ty + th { y - ty } else { th / 2.0 };
        self.sb_drag = Some((key, grab));
        self.drag_scrollbar(y);
        true
    }

    fn drag_scrollbar(&mut self, y: f32) {
        let Some((key, grab)) = self.sb_drag else { return };
        let Some(f) = &self.frame else { return };
        let Some(&i) = f.by_key().get(&key) else { return };
        let n = &f.nodes[i];
        let (rx, ry) = n.scroll_range;
        let h = n.bounds.size.height;
        let th = (h * h / (h + ry)).max(16.0);
        let free = (h - th).max(1.0);
        let sy = (((y - grab - n.bounds.origin.y) / free) * ry).clamp(0.0, ry);
        let sx = n.scroll_pos.0.clamp(0.0, rx);
        self.glides.retain(|g| g.container != key);
        self.state.scroll.insert(key, (sx, sy));
        self.scrolled = true;
    }

    /// Scroll the innermost scroll container along `path` that can move; a
    /// discrete step (`snaps`: a wheel notch, a page) lands on its snap positions.
    fn scroll_by(&mut self, path: &[Key], dy: f32, horizontal: bool, snaps: bool) {
        let Some(f) = &self.frame else { return };
        for key in path.iter().rev() {
            let Some(&i) = f.by_key().get(key) else { continue };
            let n = &f.nodes[i];
            let (rx, ry) = n.scroll_range;
            // where an earlier scroll of this turn put it (a diagonal touch scroll comes as two
            // events before the next layout), else where it was drawn
            let (sx, sy) = self.state.scroll.get(key).copied().unwrap_or(n.scroll_pos);
            // a wheel over content with no vertical range scrolls it sideways; a finger keeps to
            // the axis it moved along
            let (nx, ny) = if horizontal || (ry <= 0.0 && snaps) {
                ((sx + dy).clamp(0.0, rx), sy)
            } else {
                (sx, (sy + dy).clamp(0.0, ry))
            };
            let (nx, ny) = if snaps { snap(f, &self.caches.styles, i, (sx, sy), (nx, ny)) } else { (nx, ny) };
            if (nx, ny) != (sx, sy) {
                // the user scrolls: a glide there stops
                self.glides.retain(|g| g.container != *key);
                self.state.scroll.insert(*key, (nx, ny));
                self.scrolled = true;
                return;
            }
        }
    }

    /// A screen's kit event: the layout changed, an area wants a window, a region was resized
    /// (a `change` on its element), or areas and regions moved (the content follows: a render).
    fn screen_event(&mut self, target: &crate::screen::Target, ev: &RawEvent, app: &mut App) {
        if let Some((key, e)) = crate::screen::change_event(target, &self.screens, ev.node, ev.kind, ev.a, ev.b) {
            self.fire(app, EventKind::Change, key, &e);
        }
        self.dirty = true;
    }

    /// Syncs a `<bui-screen>` with its kit nodes and places its areas' elements (`screen.rs`).
    pub(crate) fn sync_screen(&mut self, div: &mut crate::element::Div, spec: &crate::screen::Spec) {
        if let Some(made) = self.screens.sync(self.canvas, self.scale, div, spec) {
            // a new screen: its layout reaches `change` listeners as every later one does (the
            // areas a JavaScript screen renders come from it)
            queue_push(RawEvent { window: self.id, node: made, kind: sys::BK_EV_CHANGE, text: String::new(),
                                  a: 0.0, b: 0.0, key: 0 });
        }
    }

    fn widget_event(&mut self, ev: RawEvent, app: &mut App) {
        if self.widgets.syncing {
            return;
        }
        let Some(key) = self.widgets.key_of(ev.node) else { return };
        match ev.kind {
            sys::BK_EV_CHANGE => {
                let e = self.widgets.read(key);
                let kind = self.widgets.nodes.get(&key).map(|w| w.spec.kind);
                // the user's state, kept against the app's `checked` they changed it from
                let app_checked = |w: &crate::widgets::Widgets, k: Key| w.nodes.get(&k).is_some_and(|n| n.spec.attr_checked);
                match kind {
                    Some(WKind::Checkbox) => {
                        self.state.checked.insert(key, (e.checked, app_checked(&self.widgets, key)));
                    }
                    Some(WKind::Radio) if e.checked => {
                        self.state.checked.insert(key, (true, app_checked(&self.widgets, key)));
                        for other in self.widgets.radio_group(key) {
                            self.state.checked.insert(other, (false, app_checked(&self.widgets, other)));
                            self.widgets.set_value(other, 0.0);
                        }
                    }
                    _ => {}
                }
                self.fire(app, EventKind::Input, key, &e);
                self.fire(app, EventKind::Change, key, &e);
                self.dirty = true;
            }
            sys::BK_EV_CLICK => {
                let (x, y) = self.pointer;
                let down = MouseDownEvent { button: MouseButton::Left, position: point(px(x), px(y)),
                                            modifiers: Modifiers::default(), click_count: 1 };
                let up = MouseUpEvent { button: MouseButton::Left, position: point(px(x), px(y)),
                                        modifiers: Modifiers::default() };
                self.fire(app, EventKind::Click, key, &ClickEvent { down, up });
                self.run_control_area(key);
                self.dirty = true;
            }
            _ => {}
        }
    }
}

/// Marker so `Window` is not `Send` (it holds raw kit pointers).
#[allow(dead_code)]
struct NotSend(PhantomData<*const ()>);

/// `scroll-snap-type`: the scroll position `to` (moving from `from`) of
/// container `i` moved to its children's snap positions: the next one in
/// the direction of travel (`mandatory`), or the nearest when it's within a
/// third of the view (`proximity`).
fn snap(f: &crate::layout::Frame, styles: &crate::layout::StyleStore, i: usize, from: (f32, f32), to: (f32, f32))
        -> (f32, f32) {
    let n = &f.nodes[i];
    let c = &styles[n.style];
    if c.snap_axes == 0 {
        return to;
    }
    let (rx, ry) = n.scroll_range;
    let view = n.bounds;
    let mut out = to;
    for axis in 0..2 {
        if c.snap_axes & (1 << axis) == 0 {
            continue;
        }
        let (cur, target, range, view_len) = if axis == 0 {
            (from.0, to.0, rx, view.size.width)
        } else {
            (from.1, to.1, ry, view.size.height)
        };
        if target == cur {
            continue;
        }
        // snap positions of the children, in scroll coordinates
        let mut points: Vec<f32> = Vec::new();
        for k in f.children_of(i) {
            let kc = &styles[f.nodes[k].style];
            let align = if axis == 0 { kc.snap_align.0 } else { kc.snap_align.1 };
            if align == 0 {
                continue;
            }
            let b = f.nodes[k].bounds;
            let (start, len, vstart) = if axis == 0 {
                (b.origin.x + cur, b.size.width, view.origin.x)
            } else {
                (b.origin.y + cur, b.size.height, view.origin.y)
            };
            let rel = start - vstart;
            let p = match align {
                1 => rel,
                2 => rel + len / 2.0 - view_len / 2.0,
                _ => rel + len - view_len,
            };
            points.push(p.clamp(0.0, range));
        }
        if points.is_empty() {
            continue;
        }
        let forward = target > cur;
        let pick = if c.snap_mandatory {
            if forward {
                points.iter().copied().filter(|p| *p > cur + 0.5).fold(f32::INFINITY, f32::min)
            } else {
                points.iter().copied().filter(|p| *p < cur - 0.5).fold(f32::NEG_INFINITY, f32::max)
            }
        } else {
            let near = points.iter().copied().min_by(|a, b| (a - target).abs().total_cmp(&(b - target).abs()));
            near.filter(|p| (p - target).abs() <= view_len / 3.0).unwrap_or(target)
        };
        let v = if pick.is_finite() { pick } else { target };
        if axis == 0 {
            out.0 = v;
        } else {
            out.1 = v;
        }
    }
    out
}
