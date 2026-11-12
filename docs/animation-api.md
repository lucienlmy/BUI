# Animation API

> **Status: built.** This page was the proposal; the API below is what shipped, with the
> differences listed here. The guides are on the website: [Rust](https://bui.preetham.top/docs/rust/motion/),
> [JavaScript](https://bui.preetham.top/docs/javascript/animation/), [Python](https://bui.preetham.top/docs/python/animation/)
> and the Lua overview.
>
> Differences from the proposal:
> - `variants` in `rsx!` take a list of `("name", vars! { ... })` pairs, not a nested object, and
>   `staggerChildren` / `delayChildren` aren't there (give children a `delay`, or use `gsap::to` with `stagger`).
> - JavaScript has no `layoutId` yet; its `exit` runs whenever an element is removed, so `AnimatePresence` only passes its children through. Rust has `presence` and `layout`.
> - Widgets (kit nodes) have no draw transform or opacity: `x`, `y`, `scale`, `rotate` and `opacity` apply to
>   HTML elements, and a widget animates its own values.

> **Reading the rest of this page:** it is the original proposal. Where a sample below uses something that was
> not built, it says so in a comment (`// not built`): nested-object `variants` and `staggerChildren` in Rust,
> and `AnimatePresence` modes, `LayoutGroup`, `layoutId` and `MotionConfig` in React.

GSAP's and Motion's APIs, both, on one animation core in the kit (C), so Rust, JavaScript,
Python, Lua and C all animate the same way and none of them carries its own engine. Code
written for GSAP or Motion should port by changing the import; where BUI can't do
something, the call says so (an error naming the option), it never silently does nothing.

## Before this

- Rust and JS `<div>`s: CSS `@keyframes`, `animation-*`, `transition-*` (`anim.rs`).
- Python Qt syntax: `QPropertyAnimation` with 4 easings.
- `BK_set_animations(0)`: reduced motion, today only for images and video.
- The JS DOM's `el.animate()` (Web Animations) is a stub that finishes at once.

## The core (C, `kit/intern/anim/`)

Everything below is built from five pieces in the kit, each from Blender where Blender
has it:

| Piece | What it is | From |
|---|---|---|
| **Easing** | every named ease, `cubic-bezier()`, `steps()`, `linear()`, custom curves | `BLI_easing.h` (Penner: sine, quad..quint, expo, circ, back, elastic, bounce), CSS timing from `anim.rs` |
| **Curve** | keyframes with bezier handles, per-key interpolation and easing, extrapolation | Blender's FCurve evaluation (`BKE_fcurve`) |
| **Tween** | one property from A to B (or through keyframes) over time, or by a spring | new; Blender's FCurve per property |
| **Timeline** | tweens placed on a time line: offsets, labels, nesting, repeat, yoyo, time scale | Blender's NLA strips (start, scale, repeat, reverse, blend) |
| **Spring** | physical spring (stiffness, damping, mass, velocity), and inertia (decay) | new (Blender has none); Motion's equations |

One clock per window, ticked by the window's frame timer (vsync), only while something
animates; a deterministic virtual clock for headless tests. Reduced motion (the OS setting,
or `BK_set_animations(0)`) makes tweens jump to their end, as GSAP's `matchMedia` and
Motion's `reducedMotion: "user"` do.

### What can be animated

| Target | Properties |
|---|---|
| HTML elements (Rust, JS) | every CSS property that interpolates (lengths, %, colors, shadows, filters, transforms, ...), through the same override layer CSS animations use |
| Any element or widget | the transform shorthands **`x`, `y`, `scale`, `scaleX`, `scaleY`, `rotate`, `opacity`** (GSAP's and Motion's own names) |
| Blender widgets (every language) | `value` (number fields, sliders, progress), colors (`bg`, `textColor`), sizes, padding, spacing, `scroll` |
| Anything | a **motion value** (a number, color or list you read), or a plain object's numbers (GSAP tweens any object) |

New in the kit for this: a draw-time **transform and opacity per node** (translate, scale,
rotate, alpha applied when it is drawn, not to layout), which kit widgets don't have today.

Values take the usual forms: numbers, `"100px"`, `"50%"`, `"2rem"`, `"45deg"`,
`"#ff8800"`, `"rgb(...)"`, relative `"+=50"`/`"-=50"`, arrays for keyframes, and
`null`/`None` keyframes meaning "the current value" (Motion's wildcard).

### Ease names

GSAP's and Motion's names both work everywhere:

- GSAP: `none`, `power1`..`power4` (`.in`, `.out`, `.inOut`), `sine`, `expo`, `circ`,
  `back(1.7)`, `elastic(1, 0.3)`, `bounce`, `steps(5)`, `"custom"` (a registered curve).
- Motion: `linear`, `easeIn`, `easeOut`, `easeInOut`, `circIn`.., `backIn`.., `anticipate`,
  a `[x1, y1, x2, y2]` cubic bezier, or a function.
- CSS: `ease`, `ease-in`, ..., `cubic-bezier()`, `steps()`, `linear()`.
- Blender: every F-Curve interpolation by name (`"bezier"`, `"constant"`, `"bounce"`,
  `"elastic"`, ...), and a curve drawn with Blender's curve widget as a custom ease.

---

## Rust

Rust's main API is `#[component]` functions and `rsx!`, so Motion and GSAP live there first:
`<motion.div>` and its props work as in Motion for React, the hooks are Motion's and
`@gsap/react`'s in snake_case, and `{{ ... }}` (double braces, as in JSX) is an object of
values that `rsx!` reads at compile time (outside `rsx!` it is spelled `vars! { ... }`). The
functions underneath (`motion::animate`,
`gsap::to`, ...) are also there for code outside components, and typed builders for those
who'd rather not use macros.

### Components and `rsx!` (the main API)

```rust
use blenderui::prelude::*;

#[component]
fn card(title: &str) {
    rsx! {
        <motion.div p-4 rounded-xl bg-zinc-800
            initial={{ opacity: 0, y: 20 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, scale: 0.95 }}
            whileHover={{ scale: 1.03 }}
            whileTap={{ scale: 0.97 }}
            transition={{ type: "spring", stiffness: 400, damping: 30 }}>
            {title.to_string()}
        </motion.div>
    }
}

#[component]
fn list(items: Vec<Item>) {
    let (open, set_open) = use_state(true);
    rsx! {
        // not built: `variants` in rsx! take [("name", vars! { .. })] pairs, and there is no staggerChildren
        <motion.ul variants={{
                hidden: { opacity: 0 },
                show:   { opacity: 1, transition: { staggerChildren: 0.05, delayChildren: 0.1 } },
            }}
            initial="hidden" animate={if open() { "show" } else { "hidden" }}>
            {items.iter().map(|it| rsx! {
                <motion.li key={it.id} layout
                    variants={{ hidden: { x: -20, opacity: 0 }, show: { x: 0, opacity: 1 } }}>
                    {it.name.clone()}
                </motion.li>
            })}
        </motion.ul>
        <AnimatePresence mode="wait">
            {open().then(|| rsx! {
                <motion.div key="panel" initial={{ height: 0 }} animate={{ height: "auto" }} exit={{ height: 0 }} />
            })}
        </AnimatePresence>
    }
}
```

- **Every tag**: `<motion.div>`, `<motion.span>`, `<motion.button>`, `<motion.svg>`,
  `<motion.path>`, ... and Blender's widgets: `<motion.NumberSlider animate={{ value: 0.8 }}>`,
  `<motion.Button whileTap={{ scale: 0.95 }}>`.
- **Your own components**: `<motion.card title="Hi" animate={{ x: 100 }} />` animates the
  root element that `card` renders (Motion's `motion.create(Card)`), nothing to change in
  `card`.
- **The props, Motion's names**: `initial`, `animate`, `exit`, `transition`, `variants`,
  `custom`, `whileHover`, `whileTap`, `whileFocus`, `whileDrag`, `whileInView`, `viewport`,
  `layout`, `layoutId`, `layoutDependency`, `drag`, `dragConstraints`, `dragElastic`,
  `dragMomentum`, `dragSnapToOrigin`, `dragControls`, `onAnimationStart`,
  `onAnimationComplete`, `onUpdate`, `onHoverStart`, `onTap`, `onDrag`, `onDragEnd`,
  `onViewportEnter`, `style={{ x: mv, opacity: mv }}` (motion values bound to style).
- **Values in `{{ }}`**: numbers, `"100px"`, `"50%"`, `"#f80"`, arrays for keyframes
  (`{{ x: [0, 100, 0] }}`), `None` for "where it is now", nested objects (`transition`,
  variants), and any Rust expression in braces (`{{ x: {offset * 2.0} }}`). A misspelled
  property or ease is a compile error naming the close matches, as Tailwind attributes are
  today.
- **Plain elements too**: the same props on `<div>` work (every BUI element can animate);
  `motion.` is there so Motion code reads the same.

### Hooks

```rust
#[component]
fn progress_bar() {
    let progress = use_motion_value(0.0);                     // MotionValue<f32>
    let smooth = use_spring(progress, vars! { stiffness: 200, damping: 30 });
    let width = use_transform(smooth, [0.0, 1.0], ["0%", "100%"]);
    let velocity = use_velocity(smooth);
    use_motion_value_event(progress, "change", |v| println!("{v}"));
    let (scope, animate) = use_animate();                     // Motion's useAnimate
    let in_view = use_in_view(scope, vars! { once: true, amount: 0.5 });
    let ScrollInfo { y, .. } = use_scroll(vars! { container: scope });
    let reduced = use_reduced_motion();
    let controls = use_animation_controls();                  // controls.start("show")
    let drag = use_drag_controls();
    use_animation_frame(|time, delta| { /* every frame */ });
    let time = use_time();

    use_effect(move || { if in_view { animate(".bar", vars! { opacity: 1 }, vars! { duration: 0.4 }); } }, in_view);
    rsx! { <div ref={scope}><motion.div class="bar" style={{ width: width, scaleY: y.progress }} /></div> }
}
```

### GSAP in components (`@gsap/react`'s `useGSAP`)

```rust
#[component]
fn hero() {
    let scope = use_ref();
    // runs after mount, scoped to `scope` (selectors match inside it), everything it made is
    // reverted on unmount or when the deps change
    let ctx = use_gsap(move || {
        let tl = gsap::timeline(vars! { defaults: { duration: 0.6, ease: "power2.out" } });
        tl.from(".title", vars! { y: 40, opacity: 0 })
          .from(".subtitle", vars! { y: 20, opacity: 0 }, "-=0.3")
          .from(".cta", vars! { scale: 0.8, opacity: 0, stagger: 0.1 });
        gsap::to(".bg", vars! { y: -100, scrollTrigger: { trigger: ".hero", scrub: true } });
    }, vars! { scope, deps: () });
    let on_click = ctx.context_safe(|| gsap::to(".cta", {{ rotate: 360 }})); // GSAP's contextSafe
    rsx! {
        <section ref={scope} class="hero">
            <h1 class="title">"BUI"</h1>
            <p class="subtitle">"Blender's UI, everywhere"</p>
            <button class="cta" onClick={on_click}>"Start"</button>
        </section>
    }
}
```

Outside `rsx!` the same object is `vars! { ... }` (Rust reads a bare `{{ }}` as a block): one
grammar, so a value moves between the two by changing its braces. `AnimatePresence`,
`LayoutGroup` and `MotionConfig` are components:

```rust
rsx! {
    <MotionConfig reducedMotion="user" transition={{ duration: 0.3 }}>
        <LayoutGroup id="tabs">
            {tabs.iter().map(|t| rsx! {
                <div key={t.id} onClick={select(t.id)}>
                    {t.label.clone()}
                    {(selected() == t.id).then(|| rsx! { <motion.div layoutId="underline" class="underline" /> })}
                </div>
            })}
        </LayoutGroup>
    </MotionConfig>
}
```

### Functions (outside components)

```rust
use blenderui::prelude::*;

// GSAP
gsap::to("#box", vars! { x: 200, rotate: 360, opacity: 0.5, duration: 1.0, ease: "power2.out" });
gsap::from(".card", vars! { y: 40, opacity: 0, stagger: 0.1 });
gsap::from_to("#bar", vars! { width: "0%" }, vars! { width: "100%", duration: 2.0 });
gsap::set("#box", vars! { x: 0 });
let tl = gsap::timeline(vars! { repeat: 2, yoyo: true, defaults: { duration: 0.5 } });
tl.to("#a", vars! { x: 100 }).to("#b", vars! { x: 100 }, "<").add_label("mid").call(|| (), "mid");
let t = gsap::to("#box", vars! { x: 100, stagger: { each: 0.05, from: "center", grid: [8, 8] } });
t.pause(); t.resume(); t.reverse(); t.restart(); t.seek(0.5); t.progress(0.25); t.time_scale(2.0);
t.kill(); t.finished().await;
let move_x = gsap::quick_to("#cursor", "x", vars! { duration: 0.3 }); move_x(120.0);
gsap::delayed_call(2.0, || ()); gsap::register_ease("wobble", Ease::curve(&blender_curve));
gsap::match_media().add("(prefers-reduced-motion: no-preference)", || ());
gsap::utils::map_range(0.0, 1.0, 0.0, 100.0, 0.5); gsap::ticker::add(|time, delta| ());

// Motion
let c = motion::animate("#box", vars! { x: 100, opacity: [0, 1] }, vars! { duration: 0.8, ease: "easeOut" });
motion::animate(".item", vars! { scale: [1, 1.2, 1] }, vars! { delay: stagger(0.05) });
motion::animate("#ball", vars! { y: 300 }, vars! { type: "spring", bounce: 0.4, visualDuration: 0.5 });
motion::animate_sequence([("#a", vars! { x: 100 }, vars! {}), ("#b", vars! { x: 100 }, vars! { at: "<" })]);
c.pause(); c.play(); c.stop(); c.complete(); c.set_time(0.3); c.set_speed(0.5); c.finished().await;
motion::scroll(motion::animate("#bar", vars! { scaleX: [0, 1] }, vars! {}), vars! { container: "#list" });
motion::in_view(".card", |el| { motion::animate(el, vars! { opacity: 1 }, vars! {}); });
motion::hover("#btn", |el| ..); motion::press("#btn", |el| ..);

// typed builders, the same things without macros
Tween::to("#box").x(200.0).duration(1.0).ease(Ease::Power2Out).repeat(-1).yoyo(true).play();
motion::animate("#ball", Values::new().y(300.0), Spring::new().stiffness(300.0).damping(20.0));
div().initial(Values::new().opacity(0.0)).animate(Values::new().opacity(1.0)).while_hover(Values::new().scale(1.05));
// and gpui's own `with_animation(id, Animation::new(..).with_easing(..), |el, t| ..)` keeps working
```

---

## JavaScript

`@blender-ui/native/gsap` and `@blender-ui/native/motion` export GSAP's and Motion's own
names, so `import { gsap } from 'gsap'` becomes `import { gsap } from '@blender-ui/native/gsap'`
and the rest of the code stays. React, Svelte and Vue get Motion's components.

### GSAP style

```js
import { gsap, ScrollTrigger, Flip, Draggable, Observer, CustomEase } from '@blender-ui/native/gsap'

gsap.to('#box', { x: 200, rotate: 360, duration: 1, ease: 'power2.out', repeat: -1, yoyo: true })
gsap.from('.card', { y: 40, opacity: 0, stagger: { each: 0.1, from: 'center' } })
gsap.fromTo('#bar', { width: '0%' }, { width: '100%', duration: 2 })
gsap.set('#box', { x: 0 })
const tl = gsap.timeline({ defaults: { duration: 0.5 }, repeat: 2 })
tl.to('#a', { x: 100 }).to('#b', { x: 100 }, '<').addLabel('mid').call(fn, null, 'mid')
const t = gsap.to(obj, { value: 100, onUpdate: () => render(obj.value) }) // any object
t.pause(); t.reverse(); t.progress(0.5); t.timeScale(2); await t

gsap.quickTo('#cursor', 'x', { duration: 0.3 })
gsap.matchMedia().add('(prefers-reduced-motion: no-preference)', () => { ... })
gsap.context(() => { ... }).revert()
gsap.utils.mapRange(0, 1, 0, 100, 0.5); gsap.ticker.add(fn); gsap.delayedCall(1, fn)
CustomEase.create('wobble', 'M0,0 C0.2,0.8 0.4,1.2 1,1')

// plugins
gsap.registerPlugin(ScrollTrigger, Flip, Draggable, Observer)
gsap.to('#bar', { scaleX: 1, scrollTrigger: { trigger: '#section', start: 'top center', end: 'bottom top', scrub: true, pin: true } })
const state = Flip.getState('.item'); reorder(); Flip.from(state, { duration: 0.5, ease: 'power1.inOut' })
Draggable.create('#knob', { type: 'rotation', inertia: true, bounds: '#dial' })
Observer.create({ target: window, type: 'wheel,touch', onUp: prev, onDown: next })
```

### Motion style

```js
import { animate, stagger, spring, scroll, inView, hover, press, motionValue,
         transform, mix, frame, cancelFrame } from '@blender-ui/native/motion'

const c = animate('#box', { x: 100, opacity: [0, 1] }, { duration: 0.8, ease: 'easeOut' })
animate('.item', { scale: [1, 1.2, 1] }, { delay: stagger(0.05, { from: 'last' }) })
animate('#ball', { y: 300 }, { type: 'spring', stiffness: 300, damping: 20 })
animate('#ball', { y: 300 }, { type: spring, bounce: 0.4, visualDuration: 0.5 })
animate([['#a', { x: 100 }], ['#b', { x: 100 }, { at: '<' }]])            // sequence
animate(0, 100, { onUpdate: v => label.textContent = Math.round(v) })   // a number
c.pause(); c.time = 0.3; c.speed = 0.5; await c

scroll(animate('#bar', { scaleX: [0, 1] }), { container: list })
inView('.card', el => { animate(el, { opacity: 1 }); return () => animate(el, { opacity: 0 }) })
hover('#btn', el => { animate(el, { scale: 1.1 }); return () => animate(el, { scale: 1 }) })
el.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 300, easing: 'ease-out' }) // Web Animations, real
```

### React (`@blender-ui/react/motion`), Vue, Svelte

```jsx
import { motion, AnimatePresence, LayoutGroup, MotionConfig, useAnimate, useMotionValue,
         useSpring, useTransform, useScroll, useInView, useVelocity, useAnimationFrame,
         useReducedMotion, useDragControls, useAnimationControls } from '@blender-ui/react/motion'

<motion.div initial={{ opacity: 0, y: 20 }} animate={{ opacity: 1, y: 0 }} exit={{ opacity: 0 }}
            whileHover={{ scale: 1.05 }} whileTap={{ scale: 0.95 }} whileInView={{ opacity: 1 }}
            transition={{ type: 'spring', stiffness: 400 }} layout layoutId="tab"
            drag="x" dragConstraints={{ left: 0, right: 300 }} />
{/* not built: AnimatePresence only passes its children through (an exit runs on any removal), and has no `mode` */}
<AnimatePresence mode="wait">{open && <motion.div key="m" exit={{ opacity: 0 }} />}</AnimatePresence>
<motion.NumberSlider animate={{ value: 0.8 }} />   {/* Blender widgets animate too */}
```

Vue: `<Motion :initial :animate :exit :while-hover ...>` and `<AnimatePresence>` from
`@blender-ui/vue/motion` (Motion for Vue's API). Svelte: `use:motion={{ animate, transition }}`
and the stores `motionValue`, `spring`, `tweened` from `@blender-ui/svelte/motion`.

---

## Python

`bui.gsap` and `bui.motion`; keyword arguments where JS has objects, snake_case names.
Targets: a component, a list of them, a `MotionValue`, or any object with attributes.

### GSAP style

```python
from bui import gsap
from bui.gsap import Tween, stagger, ScrollTrigger, Flip, Draggable

gsap.to(box, x=200, rotate=360, duration=1, ease="power2.out", repeat=-1, yoyo=True)
gsap.from_(cards, y=40, opacity=0, stagger=0.1)           # `from` is a keyword in Python
gsap.from_to(bar, {"width": "0%"}, {"width": "100%", "duration": 2})
gsap.set(box, x=0)
slider_tween = gsap.to(volume_slider, value=0.8, duration=0.4)   # a Blender widget's value

tl = gsap.timeline(repeat=2, defaults={"duration": 0.5})
tl.to(a, x=100).to(b, x=100, position="<").add_label("mid").call(print, "halfway", position="mid")
with gsap.timeline() as tl:          # or as a context: tweens made inside join it in order
    gsap.to(a, x=100)
    gsap.to(b, x=100)

t = gsap.to(state, value=100, on_update=lambda: label.set("text", f"{state.value:.0f}"))
t.pause(); t.reverse(); t.progress(0.5); t.time_scale(2); await t   # awaitable, or t.then(fn)
move = gsap.quick_to(cursor, "x", duration=0.3); move(120)
gsap.delayed_call(1, fn); gsap.ticker.add(fn); gsap.utils.map_range(0, 1, 0, 100, 0.5)
with gsap.context() as ctx: ...; ctx.revert()
gsap.register_ease("wobble", curve_widget)       # a Blender CurveMapping as an ease
gsap.to(bar, scale_x=1, scroll_trigger={"trigger": section, "start": "top center", "scrub": True})
state = Flip.get_state(items); reorder(); Flip.from_(state, duration=0.5)
Draggable.create(knob, type="rotation", inertia=True)
```

### Motion style

```python
from bui import motion
from bui.motion import animate, spring, stagger, MotionValue, scroll, in_view, hover, press

c = animate(box, {"x": 100, "opacity": [0, 1]}, duration=0.8, ease="easeOut")
animate(items, {"scale": [1, 1.2, 1]}, delay=stagger(0.05, from_="last"))
animate(ball, {"y": 300}, type="spring", stiffness=300, damping=20)
animate([(a, {"x": 100}), (b, {"x": 100}, {"at": "<"})])     # sequence
c.pause(); c.time = 0.3; c.speed = 0.5; await c

progress = MotionValue(0)
smooth = progress.spring(stiffness=200)
width = progress.transform([0, 1], ["0%", "100%"])
progress.on_change(lambda v: ...)

# on components: keyword arguments, the same names
Panel("Settings", ..., initial={"opacity": 0, "y": 20}, animate={"opacity": 1, "y": 0},
      exit={"opacity": 0}, while_hover={"scale": 1.02}, transition={"type": "spring"},
      layout=True)
Button("Save", on_click=save, while_tap={"scale": 0.95})
NumberSlider("Volume", animate={"value": 0.8}, transition={"duration": 0.3})
```

Qt syntax gets real `QPropertyAnimation`, `QSequentialAnimationGroup`,
`QParallelAnimationGroup`, `QVariantAnimation` and every `QEasingCurve` type on the same
core; tkinter has no animation API to port.

---

## C (the core every binding calls)

```c
BKTween *t = BK_tween_to(target, "x", 200.0, &(BKTweenVars){.duration = 1, .ease = BK_ease("power2.out")});
BK_tween_set_keyframes(t, values, times, n);    BK_tween_set_spring(t, stiffness, damping, mass);
BK_tween_play(t); BK_tween_pause(t); BK_tween_reverse(t); BK_tween_seek(t, s); BK_tween_set_speed(t, k);
BK_tween_on(t, BK_TWEEN_COMPLETE, fn, data);   BK_tween_kill(t);
BKTimeline *tl = BK_timeline_new(...); BK_timeline_add(tl, tween, "<+0.2"); BK_timeline_add_label(tl, "mid");
BKMotionValue *mv = BK_motion_value_new(0.0);  /* targets: nodes, motion values, a callback */
float y = BK_ease_eval(BK_ease("elastic.out(1, 0.3)"), x);
```

---

## GSAP → BUI

| GSAP | BUI | |
|---|---|---|
| `to`, `from`, `fromTo`, `set`, `timeline`, `delayedCall`, `quickTo`, `quickSetter` | all three languages | phase 1 |
| tween/timeline controls, callbacks, `then`/`await` | all | phase 1 |
| `stagger` (each, amount, from, grid, axis, ease) | all | phase 1 |
| `keyframes` (array, percent, simple) | all | phase 1 |
| eases, `CustomEase`, `registerEase` | all, plus Blender curves | phase 1 |
| `context`, `matchMedia`, `utils.*`, `ticker` | all | phase 2 |
| `ScrollTrigger` (scrub, pin, snap, markers) | all | phase 3 |
| `Flip` | all | phase 3 (the layout snapshot) |
| `Draggable`, `InertiaPlugin`, `Observer` | all | phase 3 |
| `MotionPathPlugin`, `DrawSVGPlugin`, `MorphSVGPlugin` | Rust and JS SVG | phase 4 |
| `SplitText`, `ScrambleText`, `TextPlugin` | phase 4 (needs text runs per element) | |
| `CSSRulePlugin`, `EaselPlugin`, `PixiPlugin`, `ScrollToPlugin` | ScrollTo yes; the others don't apply | |

## Motion → BUI

| Motion | BUI | |
|---|---|---|
| `animate` (elements, values, objects, sequences), controls | all | phase 1 |
| tween, `spring` (stiffness/damping/mass, bounce/visualDuration), `inertia` | all | phase 1 |
| `stagger`, keyframes, `times` | all | phase 1 |
| motion values: `motionValue`, `useSpring`, `useTransform`, `useVelocity`, `useMotionValueEvent` | all (hooks in `#[component]`, React, Vue, Svelte) | phase 2 |
| `motion.div` and the props (`initial`/`animate`/`exit`, `while*`, `variants`, `transition`) | all (`<motion.div>` in `rsx!`) | phase 2 |
| `AnimatePresence` (modes, `custom`), `MotionConfig`, `useReducedMotion` | all | phase 2 |
| `scroll`, `inView`, `useScroll`, `useInView`, `hover`, `press` | all | phase 3 |
| `layout`, `layoutId`, `LayoutGroup` | all | phase 3 |
| `drag` (constraints, elastic, momentum, `useDragControls`) | all | phase 3 |
| `frame`/`cancelFrame`, `useAnimationFrame`, `useTime` | all | phase 2 |
| `motion.svg` path length, `useTransform` on SVG attributes | Rust and JS SVG | phase 4 |

## Phases

1. **Core**: easing (Blender's, Motion's, GSAP's, CSS), curves, tweens, springs, timelines,
   stagger, keyframes; the per-node draw transform and opacity in the kit; the three
   language APIs above for tweens and timelines; Web Animations `el.animate()` for real.
2. **Declarative**: motion values, `initial`/`animate`/`exit`/`while*`, variants, presence,
   `context`/`matchMedia`, the React/Vue/Svelte components and hooks.
3. **Interaction and layout**: scroll and in-view, gestures and drag with inertia, layout
   (FLIP: the kit records each node's box before and after a layout), `Flip`,
   `ScrollTrigger`, `Draggable`, `Observer`.
4. **SVG and text**: motion paths, path drawing and morphing, split text.

Each phase lands with its tests (a virtual clock: frame-exact values), the website's
animation pages, and examples in all three languages.

## Choices to make now

- **`rsx!` syntax**: `<motion.div>` needs `.` in tag names (a small parser change; `-` and
  `:` already work), and `{{ ... }}` an object grammar `rsx!` checks at compile time; outside
  it, `vars! { ... }`.
- **Names**: two namespaces (`gsap`, `motion`) as above, or one BUI API with GSAP and Motion
  as thin aliases over it. Proposal: the two namespaces, because the point is that existing
  code ports unchanged; both are thin over the C core, so nothing is duplicated.
- **The CSS engine** (`anim.rs`) moves onto the core's easing and timing once phase 1 lands,
  so CSS, GSAP and Motion share one clock and one interpolation per property.
- **Licensing**: GSAP is free (including the plugins) but not open source, and Motion is MIT.
  This ports their **APIs**, written fresh; none of their code is copied.
