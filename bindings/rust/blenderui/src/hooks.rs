//! Hooks and signals: zopra / React / Leptos-style state for components.
//!
//! ```ignore
//! #[component]
//! fn counter(start: i32) {
//!     let (count, set_count) = use_state(start);             // zopra / React
//!     let doubled = Memo::new(move || count() * 2);          // Leptos (auto-tracked)
//!     Effect::new(move || println!("count = {}", count()));  // Leptos (auto-tracked)
//!     use_effect(move || println!("mounted"), ());           // React / zopra (deps)
//!     rsx! { <button onClick={move || set_count(count() + 1)}>{doubled.get()}</button> }
//! }
//! ```
//!
//! Signals are `Copy` handles into a per-thread store: closures capture them
//! freely and they need no `cx`. Writing one re-renders the window that owns
//! it. Hook state is keyed by call site (and order, like React), and freed
//! when its component stops rendering. No `Rc` / `RefCell`: the store is
//! moved out of its `Cell` while in use, and user code never runs inside it.

use std::any::{Any, TypeId};
use std::cell::Cell;
use std::marker::PhantomData;
use std::panic::Location;

use crate::style::KeyMap;

struct Slot {
    value: Option<Box<dyn Any>>,
    generation: u32,
    version: u32,
    window: usize,
    /// rendered this frame (hook slots only; plain signals are 'static)
    seen: bool,
    hook: bool,
}

/// A deferred effect: runs after the render (with the store not borrowed).
type Job = Box<dyn FnOnce()>;

#[derive(Default)]
struct Store {
    slots: Vec<Slot>,
    free: Vec<u32>,
    /// (window, call site, key scope, occurrence) -> slot
    keys: KeyMap<(usize, usize, u64, u32), u32>,
    /// occurrences of each (call site, key scope) in the current render
    occ: KeyMap<(usize, u64), u32>,
    /// the keys of the keyed components being rendered, hashed together
    scope: u64,
    window: Option<usize>,
    dirty: Vec<usize>,
    jobs: Vec<Job>,
    /// signals read while tracking (Effect / Memo)
    tracking: Option<Vec<(u32, u32)>>,
    /// context values (TypeId) for the components being rendered
    ctx: Vec<(TypeId, Box<dyn Any>)>,
}

thread_local! {
    static STORE: Cell<Store> = Cell::new(Store::default());
}

/// Run `f` on the store (moved out of its cell). `f` must not call user code.
fn with<R>(f: impl FnOnce(&mut Store) -> R) -> R {
    STORE.with(|c| {
        let mut s = c.take();
        let r = f(&mut s);
        c.set(s);
        r
    })
}

impl Store {
    fn alloc(&mut self, value: Box<dyn Any>, hook: bool) -> (u32, u32) {
        let window = self.window.unwrap_or(usize::MAX);
        let slot = Slot { value: Some(value), generation: 0, version: 0, window, seen: true, hook };
        match self.free.pop() {
            Some(i) => {
                let s = &mut self.slots[i as usize];
                let generation = s.generation + 1;
                *s = Slot { generation, ..slot };
                (i, generation)
            }
            None => {
                self.slots.push(slot);
                ((self.slots.len() - 1) as u32, 0)
            }
        }
    }

    /// The slot for the hook at `site` (same call site and order as last render).
    fn hook_slot(&mut self, site: &'static Location<'static>, init: impl FnOnce() -> Box<dyn Any>) -> (u32, u32) {
        let window = self.window.unwrap_or(usize::MAX);
        let site = site as *const Location as usize;
        let n = self.occ.entry((site, self.scope)).or_insert(0);
        let key = (window, site, self.scope, *n);
        *n += 1;
        if let Some(&i) = self.keys.get(&key) {
            let s = &mut self.slots[i as usize];
            s.seen = true;
            return (i, s.generation);
        }
        let (i, generation) = self.alloc(init(), true);
        self.keys.insert(key, i);
        (i, generation)
    }
}

// ==========================================================================
// Render scopes (called by the window around its view)

pub(crate) fn begin(window: usize) {
    with(|s| {
        s.window = Some(window);
        s.occ.clear();
        s.ctx.clear();
        s.scope = 0;
        for slot in s.slots.iter_mut().filter(|x| x.window == window && x.hook) {
            slot.seen = false;
        }
    });
}

/// End of a window's render: free the state of components that weren't
/// rendered, then run the effects queued during the render.
pub(crate) fn end() {
    let jobs = with(|s| {
        let w = s.window.take();
        let mut gone = Vec::new();
        for (i, slot) in s.slots.iter_mut().enumerate() {
            if slot.hook && Some(slot.window) == w && !slot.seen && slot.value.is_some() {
                gone.push(slot.value.take());
                s.free.push(i as u32);
            }
        }
        s.keys.retain(|_, i| s.slots[*i as usize].value.is_some());
        s.ctx.clear();
        let jobs = std::mem::take(&mut s.jobs);
        (jobs, gone)
    });
    drop(jobs.1); // user values dropped outside the store
    for job in jobs.0 {
        job();
    }
}

/// Every window draws again (an animation started outside a render).
pub(crate) fn wake_all() {
    with(|s| {
        if !s.dirty.contains(&usize::MAX) {
            s.dirty.push(usize::MAX);
        }
    });
}

/// Windows whose signals were written since the last call.
pub(crate) fn take_dirty() -> Vec<usize> {
    with(|s| std::mem::take(&mut s.dirty))
}

// ==========================================================================
// Signals

/// A `Copy` handle to a value: read with [`get`](Self::get) /
/// [`with`](Self::with), write with [`set`](Self::set) /
/// [`update`](Self::update) (which re-renders its window). Leptos' `RwSignal`.
pub struct Signal<T> {
    i: u32,
    generation: u32,
    _t: PhantomData<fn() -> T>,
}

impl<T> Clone for Signal<T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for Signal<T> {}

/// Leptos' name for a read-write signal.
pub type RwSignal<T> = Signal<T>;

impl<T: 'static> Signal<T> {
    /// A signal that lives until [`dispose`](Self::dispose) (inside a
    /// component, prefer [`use_signal`]: its state follows the component).
    pub fn new(value: T) -> Signal<T> {
        let (i, generation) = with(|s| s.alloc(Box::new(value), false));
        Signal { i, generation, _t: PhantomData }
    }

    fn take(self) -> Option<Box<T>> {
        with(|s| {
            let slot = s.slots.get_mut(self.i as usize).filter(|x| x.generation == self.generation)?;
            if let Some(t) = &mut s.tracking {
                t.push((self.i, slot.version));
            }
            slot.value.take()?.downcast::<T>().ok()
        })
    }

    fn put(self, v: Box<T>, changed: bool) {
        with(|s| {
            if let Some(slot) = s.slots.get_mut(self.i as usize).filter(|x| x.generation == self.generation) {
                slot.value = Some(v);
                if changed {
                    slot.version = slot.version.wrapping_add(1);
                    let w = slot.window;
                    if w != usize::MAX && !s.dirty.contains(&w) {
                        s.dirty.push(w);
                    } else if w == usize::MAX {
                        s.dirty.push(usize::MAX); // global signal: every window
                    }
                }
            }
        })
    }

    /// Look at the value without cloning it.
    pub fn with<R>(self, f: impl FnOnce(&T) -> R) -> R {
        let v = self.take().expect("signal used after its component was dropped");
        let r = f(&v);
        self.put(v, false);
        r
    }

    pub fn get(self) -> T
    where
        T: Clone,
    {
        self.with(T::clone)
    }

    /// Change the value in place and re-render.
    pub fn update(self, f: impl FnOnce(&mut T)) {
        let mut v = self.take().expect("signal used after its component was dropped");
        f(&mut v);
        self.put(v, true);
    }

    pub fn set(self, value: T) {
        self.update(|v| *v = value);
    }

    /// Change the value in place *without* re-rendering: for what a render
    /// itself refreshes (a handle to a widget's state, the latest callbacks).
    pub(crate) fn update_quiet<R>(self, f: impl FnOnce(&mut T) -> R) -> R {
        let mut v = self.take().expect("signal used after its component was dropped");
        let r = f(&mut v);
        self.put(v, false);
        r
    }

    /// Leptos' read / write halves of this signal.
    pub fn split(self) -> (ReadSignal<T>, WriteSignal<T>) {
        (ReadSignal(self), WriteSignal(self))
    }

    pub fn dispose(self) {
        let v = with(|s| {
            let slot = s.slots.get_mut(self.i as usize).filter(|x| x.generation == self.generation)?;
            let v = slot.value.take();
            s.free.push(self.i);
            v
        });
        drop(v);
    }
}

/// Leptos' read half.
pub struct ReadSignal<T>(Signal<T>);
/// Leptos' write half.
pub struct WriteSignal<T>(Signal<T>);
impl<T> Clone for ReadSignal<T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for ReadSignal<T> {}
impl<T> Clone for WriteSignal<T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for WriteSignal<T> {}

impl<T: 'static> ReadSignal<T> {
    pub fn get(self) -> T
    where
        T: Clone,
    {
        self.0.get()
    }
    pub fn with<R>(self, f: impl FnOnce(&T) -> R) -> R {
        self.0.with(f)
    }
}

impl<T: 'static> WriteSignal<T> {
    pub fn set(self, v: T) {
        self.0.set(v)
    }
    pub fn update(self, f: impl FnOnce(&mut T)) {
        self.0.update(f)
    }
}

// ==========================================================================
// Hooks

/// A signal owned by the calling component (the same one on every render).
#[track_caller]
pub fn use_signal<T: 'static>(init: T) -> Signal<T> {
    let site = Location::caller();
    let mut init = Some(init);
    let (i, generation) = with(|s| s.hook_slot(site, || Box::new(init.take().expect("init"))));
    Signal { i, generation, _t: PhantomData }
}

/// [`use_signal`] whose first value is made only on the first render (state
/// with a cost to make, or that must not be made and dropped every render).
#[track_caller]
pub(crate) fn use_signal_with<T: 'static>(init: impl FnOnce() -> T) -> Signal<T> {
    let site = Location::caller();
    let (i, generation) = with(|s| s.hook_slot(site, || Box::new(init())));
    Signal { i, generation, _t: PhantomData }
}

/// zopra / React state: `let (count, set_count) = use_state(0);` then
/// `count()` and `set_count(count() + 1)`. Both halves are `Copy` closures.
#[track_caller]
pub fn use_state<T: Clone + 'static>(init: T) -> (impl Fn() -> T + Copy, impl Fn(T) + Copy) {
    let s = use_signal(init);
    (move || s.get(), move |v| s.set(v))
}

/// Leptos `signal(v)` / `create_signal(v)`: read and write halves, owned
/// by the calling component.
#[track_caller]
pub fn signal<T: 'static>(init: T) -> (ReadSignal<T>, WriteSignal<T>) {
    use_signal(init).split()
}

/// Leptos 0.6 name of [`signal`].
#[track_caller]
pub fn create_signal<T: 'static>(init: T) -> (ReadSignal<T>, WriteSignal<T>) {
    use_signal(init).split()
}

/// React / zopra effect: runs after the render, and again whenever `deps`
/// differ from the previous render's (`()` = once, after the first render).
#[track_caller]
pub fn use_effect<D: PartialEq + 'static>(effect: impl FnOnce() + 'static, deps: D) {
    let site = Location::caller();
    let prev: Signal<Option<D>> = {
        let (i, generation) = with(|s| s.hook_slot(site, || Box::new(None::<D>)));
        Signal { i, generation, _t: PhantomData }
    };
    let changed = prev.with(|p| p.as_ref() != Some(&deps));
    if changed {
        let v = prev.take();
        if let Some(mut v) = v {
            *v = Some(deps);
            prev.put(v, false);
        }
        with(|s| s.jobs.push(Box::new(effect)));
    }
}

/// zopra's `use_effect!(effect, [a, b])` form.
#[macro_export]
macro_rules! use_effect {
    ($effect:expr, [$($dep:expr),* $(,)?] $(,)?) => {
        $crate::hooks::use_effect($effect, ($($dep.clone(),)*))
    };
}

/// Signals read by `f` (and their versions).
fn tracked<R>(f: impl FnOnce() -> R) -> (R, Vec<(u32, u32)>) {
    let outer = with(|s| s.tracking.replace(Vec::new()));
    let r = f();
    let deps = with(|s| std::mem::replace(&mut s.tracking, outer)).unwrap_or_default();
    (r, deps)
}

fn changed(deps: &[(u32, u32)]) -> bool {
    with(|s| deps.iter().any(|&(i, v)| s.slots.get(i as usize).is_none_or(|x| x.version != v)))
}

struct EffectState {
    f: Option<Box<dyn FnMut()>>,
    deps: Vec<(u32, u32)>,
    ran: bool,
}

/// Leptos effects: run after the render, and again after any render in
/// which a signal they read has changed.
pub struct Effect;

impl Effect {
    #[track_caller]
    #[allow(clippy::new_ret_no_self)]
    pub fn new(f: impl FnMut() + 'static) {
        let site = Location::caller();
        let mut f = Some(f);
        let (i, generation) = with(|s| s.hook_slot(site, || Box::new(EffectState { f: None, deps: Vec::new(), ran: false })));
        let st: Signal<EffectState> = Signal { i, generation, _t: PhantomData };
        let run = st.with(|e| !e.ran || changed(&e.deps));
        if let Some(f) = f.take() {
            if let Some(mut e) = st.take() {
                if e.f.is_none() {
                    e.f = Some(Box::new(f));
                }
                st.put(e, false);
            }
        }
        if run {
            with(|s| s.jobs.push(Box::new(move || {
                let Some(mut e) = st.take() else { return };
                let mut f = e.f.take();
                st.put(e, false);
                let (_, deps) = tracked(|| {
                    if let Some(f) = f.as_mut() {
                        f();
                    }
                });
                if let Some(mut e) = st.take() {
                    e.f = f;
                    e.deps = deps;
                    e.ran = true;
                    st.put(e, false);
                }
            })));
        }
    }
}

/// Leptos memos: a derived value, recomputed only when a signal it read
/// has changed. `Copy`; read with [`get`](Self::get).
pub struct Memo<T>(Signal<(Option<T>, Vec<(u32, u32)>)>);

impl<T> Clone for Memo<T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for Memo<T> {}

impl<T: Clone + 'static> Memo<T> {
    #[track_caller]
    pub fn new(f: impl Fn() -> T + 'static) -> Memo<T> {
        let site = Location::caller();
        let (i, generation) = with(|s| s.hook_slot(site, || Box::new((None::<T>, Vec::<(u32, u32)>::new()))));
        let m = Memo(Signal { i, generation, _t: PhantomData });
        let stale = m.0.with(|(v, deps)| v.is_none() || changed(deps));
        if stale {
            let (v, deps) = tracked(f);
            if let Some(mut slot) = m.0.take() {
                *slot = (Some(v), deps);
                m.0.put(slot, false);
            }
        }
        m
    }

    pub fn get(self) -> T {
        self.0.with(|(v, _)| v.clone().expect("memo value"))
    }
}

/// React-style memo with explicit dependencies.
#[track_caller]
pub fn use_memo<T: Clone + 'static, D: PartialEq + 'static>(f: impl FnOnce() -> T, deps: D) -> T {
    let site = Location::caller();
    let (i, generation) = with(|s| s.hook_slot(site, || Box::new(None::<(D, T)>)));
    let slot: Signal<Option<(D, T)>> = Signal { i, generation, _t: PhantomData };
    let hit = slot.with(|v| v.as_ref().filter(|(d, _)| *d == deps).map(|(_, t)| t.clone()));
    match hit {
        Some(t) => t,
        None => {
            let t = f();
            if let Some(mut b) = slot.take() {
                *b = Some((deps, t.clone()));
                slot.put(b, false);
            }
            t
        }
    }
}

// ==========================================================================
// Context

/// Make `value` available to the components rendered inside this one
/// (looked up by type; the innermost wins).
pub fn provide_context<T: 'static>(value: T) {
    with(|s| s.ctx.push((TypeId::of::<T>(), Box::new(value))));
}

pub fn try_use_context<T: Clone + 'static>() -> Option<T> {
    // take the matching value out to clone it outside the store (Clone is user code)
    let found = with(|s| {
        let k = s.ctx.iter().rposition(|(t, _)| *t == TypeId::of::<T>())?;
        Some((k, std::mem::replace(&mut s.ctx[k].1, Box::new(()))))
    });
    let (k, b) = found?;
    let v = b.downcast_ref::<T>().cloned();
    with(|s| s.ctx[k].1 = b);
    v
}

#[track_caller]
pub fn use_context<T: Clone + 'static>() -> T {
    try_use_context().unwrap_or_else(|| {
        panic!("use_context::<{}>(): no parent component provided one", std::any::type_name::<T>())
    })
}

/// Runs a component body: context it provides is visible to its children
/// only (used by `#[component]`).
#[doc(hidden)]
pub fn component<R>(body: impl FnOnce() -> R) -> R {
    let mark = with(|s| s.ctx.len());
    let r = body();
    let dropped: Vec<Box<dyn Any>> = with(|s| s.ctx.drain(mark..).map(|(_, v)| v).collect());
    drop(dropped);
    r
}

/// Children of a component (`#[component] fn card(children: Children)`).
pub type Children = Vec<crate::element::AnyElement>;

/// `<Item key={id} ../>`: a component whose hook state follows its key, not
/// its position among its siblings (React's `key`). Without keys, removing
/// the first item of a list hands its state to the next one.
pub struct Keyed<E> {
    key: u64,
    inner: E,
}

/// Wraps `inner` so the hooks it runs are identified by `key` (`rsx!` does
/// this for `key=` attributes).
pub fn keyed<K: std::hash::Hash, E>(key: K, inner: E) -> Keyed<E> {
    use std::hash::{BuildHasher, Hasher};
    let mut h = std::hash::BuildHasherDefault::<std::collections::hash_map::DefaultHasher>::default().build_hasher();
    key.hash(&mut h);
    Keyed { key: h.finish(), inner }
}

impl<E: crate::element::IntoElement> crate::element::IntoElement for Keyed<E> {
    fn into_any_element(self) -> crate::element::AnyElement {
        let outer = with(|s| {
            let outer = s.scope;
            // nested keys combine: a keyed row inside a keyed list
            s.scope = (outer ^ self.key).wrapping_mul(0x9e37_79b9_7f4a_7c15).rotate_left(29) | 1;
            outer
        });
        let el = self.inner.into_any_element();
        with(|s| s.scope = outer);
        el
    }
}

/// A component's props, built field by field (`rsx!` calls the setters, then
/// [`build`]). Implemented by `#[component]` and `#[derive(Props)]`
/// builders once every required prop is set.
#[diagnostic::on_unimplemented(
    message = "a required prop is missing: `{Self}`",
    label = "set the props named `missing_<prop>` in this builder's type",
    note = "optional props are `Option<T>` (or `children`); every other prop must be given"
)]
pub trait Complete {
    type Props;
    fn finish(self) -> Self::Props;
}

/// A required prop that has been set (props builders' type state).
#[doc(hidden)]
pub struct Set;

#[doc(hidden)]
pub fn build<B: Complete>(builder: B) -> B::Props {
    builder.finish()
}
