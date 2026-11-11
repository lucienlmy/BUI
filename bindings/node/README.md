# @blender-ui/native

[![npm](https://img.shields.io/npm/v/@blender-ui/native)](https://www.npmjs.com/package/@blender-ui/native)
[![license](https://img.shields.io/npm/l/@blender-ui/native)](https://github.com/ppmpreetham/BUI/blob/main/LICENSE)

[BUI](https://github.com/ppmpreetham/BUI), Blender's UI toolkit, from
JavaScript. It gives you native windows drawn on the CPU or the GPU
(Direct3D 11 on Windows, OpenGL 3.3 on Linux), HTML elements, CSS and Tailwind
classes, Blender components (`NumberSlider`, `Button` ...), and signals. It runs on Node.js and
Bun through a napi-rs addon, and `bui build` turns an app into a single
executable of about 5 MB. There's no browser and no webview.

For React, use [`@blender-ui/react`](https://www.npmjs.com/package/@blender-ui/react);
for Svelte, [`@blender-ui/svelte`](https://www.npmjs.com/package/@blender-ui/svelte).
Both are built on this package.

```sh
npm install @blender-ui/native        # or: bun add @blender-ui/native
```

```js
const { App, signal } = require('@blender-ui/native')

const app = new App({ gpu: true })
const win = app.window({ title: 'Hello', width: 360, height: 200 })
win.body.className = 'm-0 p-6 flex flex-col gap-3 bg-[#2b2b2b] text-[#e6e6e6]'

const clicks = signal(0)
const label = win.createElement('span')
label.textContent = clicks            // follows the signal
const button = win.createElement('button')
button.textContent = 'Add one'
button.addEventListener('click', () => clicks.value++)
win.body.appendChild(label)
win.body.appendChild(button)
```

## API

- **A DOM-like tree:** `createElement`, `createTextNode`, `appendChild`,
  `insertBefore`, `removeChild`, `setAttribute`, `className` (Tailwind
  classes included), `style` (a string or an object), `textContent`.
  - Moving nodes and the error cases behave as in the DOM; siblings are a
    linked list.
  - `before` / `after` / `append` / `replaceWith`, `cloneNode`,
    `dispatchEvent`, `el.style.setProperty`, `el.classList`, and DOM
    properties that reflect attributes (`disabled`, `value`, `checked` ...).
  - `destroy()` frees a removed subtree natively.
- **`document`:** a window-less document whose nodes are detached until
  inserted into a window's tree; fragments, and comments as anchors. A removed
  node made by `document` is freed natively at the end of the tick unless it
  was reinserted.
- **For framework bindings:** `@blender-ui/native/host` (`open`,
  `blenderWidgets`, `applyProps`: what React's and Svelte's bindings share),
  `@blender-ui/native/globals` (`document`, `window` and the node classes as
  globals, for DOM-based frameworks), and `bui build` bundling with the Bun
  plugin a `@blender-ui/*` package declares (`"bui": { "bunPlugin" }`).
- **Events:** `addEventListener` for `click`, `mousedown`, `mouseup`,
  `mousemove`, `mouseenter`, `mouseleave`, `wheel`, `keydown`, `keyup`, `input`
  and `change` arrive from the UI thread; other types fire through
  `dispatchEvent`.
  - They bubble through the tree, and `stopPropagation()` works.
  - They arrive after BUI has handled them, so `preventDefault()` only marks
    the event.
- **Frames and measurement:** `win.requestAnimationFrame(cb)` is paced by the
  display. `el.getBoundingClientRect()` and `win.screenshot()` return
  promises.
- **Signals:** `signal`, `memo`, `effect`, `batch`, `untrack`,
  `startTransition`.
  - Pass a signal to `setAttribute`, `style` (or inside a style object),
    `textContent` or `createTextNode`, and its changes update that node
    alone.
  - Memos and effects track what they read, are glitch-free, and need no
    dependency arrays.
  - Without the addon: `require('@blender-ui/native/signals')`.
- **Animation:** GSAP's and Motion's APIs, run by BUI's engine (Rust and C:
  JavaScript hands over each call's objects once and hears only the
  callbacks it asked for).
  - `@blender-ui/native/gsap`: `gsap.to / from / fromTo / set / timeline /
    delayedCall`, timelines with GSAP's positions (`'<'`, `'+=1'`, labels),
    the tween controls, callbacks, `await tween`, `CustomEase`, every ease
    name of GSAP, Motion, CSS and Blender. Targets: selectors, elements,
    motion values, a plain object's numbers.
  - `@blender-ui/native/motion`: `animate` (elements, motion values,
    `animate(0, 100, { onUpdate })`), `motionValue`, `stagger`.
  - Every element takes Motion's props: `el.setMotion('animate', { x: 100 })`
    (`initial`, `animate`, `exit`, `whileHover`, `whileTap`, `whileFocus`,
    `transition`, `variants`), and `style = { x: motionValue }`.
  - `el.animate(keyframes, options)` (Web Animations) runs for real.
- **Tests:** `new App({ headless: true })` renders off-screen, and
  `win.click(x, y)` simulates input. Its clock stands still:
  `await app.advanceClock(ms)` steps animations frame-exactly.
- **Looks and theme:** plain HTML looks like a browser's; the `styled`
  attribute gives an element and its subtree Blender's look; the Blender
  components (`win.createElement('NumberSlider')`, `'Button'`, `'Checkbox'`
  ...) are Blender's widgets anywhere. `new App({ theme: 'light' })` or
  `app.setTheme('light')` (or `BUI_THEME=light`) switches Blender's theme.

## Standalone apps

```sh
npx bui build app.js           # app.exe: BUI + embedded QuickJS, ~5 MB
npx bui build app.js --bun     # app.exe: Bun, with Node's APIs and a JIT, ~115 MB
npx bui run app.js             # run on the QuickJS runtime, no build
```

Options:

- `-o <file>`: the output file.
- `--console`: keep a console window for logs (Windows builds open none).
- `--dev`: development builds (React's, Svelte's dev mode).

Bundling uses [Bun](https://bun.sh), so it must be installed.

**How the default build works.** It appends your bundled app to `bui-runtime`,
BUI with the QuickJS engine embedded. That takes under a second and compiles
nothing.

- **It's fast to start:** the result reaches its first frame in about 70 ms
  and uses 19 MB of RAM there.
- **It's limited:** no Node modules (`fs` and the like) and no JIT. Keep
  frequent updates on signals, or use `--bun`.

## How it's fast

- **Its own UI thread:** BUI renders on a thread of its own, so it never
  blocks JavaScript, and an idle window uses no CPU.
- **Only changes cross:** each tick, a window sends one batch of operations,
  an `Int32Array` and a string table.
- **Styles parsed once:** a `style` value is parsed when it's set, not every
  frame.

## Benchmarks

Ryzen 7 5800U laptop, Windows 11, GPU renderer, measured before the latest
features landed. Method:
[guide](https://github.com/ppmpreetham/BUI/blob/main/docs/javascript.md#performance).

| Startup (small app, quits at first frame) | First frame | Launch to exit | RAM | Size |
|---|---:|---:|---:|---:|
| **`bui build`** (QuickJS) | **68 ms** | **104 ms** | **19 MB** | **4.9 MB** |
| `bui build --bun` | 79 ms | 147 ms | 35 MB | 114 MB |
| `bun app.js` | 80 ms | 152 ms | 35 MB | |
| `node app.js` | 108 ms | 187 ms | 45 MB | |

| 3,000 cells, one changing per frame | FPS | CPU per frame | RAM |
|---|---:|---:|---:|
| Bun | 58.8 | 13.1 ms | 80 MB |
| Node.js | 55.8 | 14.4 ms | 88 MB |
| QuickJS (`bui build`) | 59.1 | 16.3 ms | 56 MB |
| *Rust, for reference* | 59.6 | 12.0 ms | 43 MB |

## Requirements

- **Prebuilt for Windows x64.** Elsewhere, build from a clone of the
  [repository](https://github.com/ppmpreetham/BUI) with `npm run build` in
  `bindings/node` (you need Rust, CMake and a C compiler).
  - **Linux:** built from source, windows are native X11 windows (Wayland
    through XWayland), on the CPU or OpenGL. Building needs the X11 headers.
  - **macOS:** headless only for now (`headless: true`): BUI runs on its own
    UI thread under Node, and macOS only allows windows on the main thread.
- **Runtimes:** Node.js 18 or newer, or Bun.

Guide: [docs/javascript.md](https://github.com/ppmpreetham/BUI/blob/main/docs/javascript.md) ·
Website: [bui.preetham.top](https://bui.preetham.top) ·
Source: [github.com/ppmpreetham/BUI](https://github.com/ppmpreetham/BUI) ·
License: GPL-3.0-or-later
