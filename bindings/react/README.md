# @blender-ui/react

[![npm](https://img.shields.io/npm/v/@blender-ui/react)](https://www.npmjs.com/package/@blender-ui/react)
[![license](https://img.shields.io/npm/l/@blender-ui/react)](https://github.com/ppmpreetham/BUI/blob/main/LICENSE)

React 19 for the desktop, drawn by [BUI](https://github.com/ppmpreetham/BUI),
Blender's UI toolkit. You write JSX with HTML elements, CSS and Tailwind
classes, and BUI renders it in native windows on the CPU or the GPU. There's
no browser, no webview and no Electron.
Prefer Svelte? [`@blender-ui/svelte`](https://www.npmjs.com/package/@blender-ui/svelte)
is the same engine for Svelte 5.

- **React's hooks, by React's names, holding signals.** A change updates the
  one node that shows it, without re-rendering the component.
- **Node.js and Bun.**
- **One small executable:** `bui build` makes an app of about 5 MB that
  reaches its first frame in about 70 ms.

```sh
npm install @blender-ui/react react        # or: bun add @blender-ui/react react
```

```jsx
/** @jsxImportSource @blender-ui/react */
import { render, useState } from '@blender-ui/react'

function Counter () {
  const [count, setCount] = useState(0)
  return (
    <main className="p-6 flex flex-col gap-3 bg-[#2b2b2b] text-[#e6e6e6]">
      <span>Clicks: {count}</span>
      <button onClick={() => setCount(c => c + 1)}>Add one</button>
    </main>
  )
}

render(<Counter />, { title: 'Counter', width: 360, height: 200, gpu: true })
```

```sh
bun app.jsx                 # run it (Bun reads JSX as is)
npx bui build app.jsx       # ship it: app.exe, one ~5 MB file
```

Put `"jsxImportSource": "@blender-ui/react"` in your `tsconfig.json`, or in
your bundler's JSX options; the `/** @jsxImportSource */` comment works per
file. Without JSX, use this package's `createElement`.

## Hooks

The hooks have React's names, arguments and timing, but the state they hold
is a **signal**:

- **Passed into JSX** (`<span>{count}</span>`, `style={styleMemo}`,
  `style={{ color: colorSignal }}`): a change updates that node. The
  component doesn't re-render.
- **Read while rendering** (`count.value`; in plain JavaScript also
  `count + 1` or `count > 5`): the component re-renders when it changes, as
  with React.
- **Conditions and `===` need `.value`.** A signal is an object, so it's
  always truthy: write `{isPending.value && <Spinner />}` and
  `count.value === 0`. TypeScript also needs `.value` for arithmetic.

| Hook | Returns |
|---|---|
| `useState(init)` | `[signal, setState]` |
| `useReducer(reducer, arg, init?)` | `[signal, dispatch]` |
| `useMemo(fn, deps?)` | a memo, recomputed when the signals it reads change |
| `useRef(v)` | `{ current }`, observable by effects and memos |
| `useCallback(fn, deps?)` / `useEffectEvent(fn)` | a function whose calls are batched |
| `useEffect` / `useLayoutEffect` / `useInsertionEffect` | React's timing and deps, plus a re-run when the signals the effect read change |
| `useSyncExternalStore(subscribe, get)` | a signal following the store |
| `useTransition()` | `[isPending, startTransition]`; signal updates run after urgent ones |
| `useDeferredValue(value, initial?)` | a signal that follows `value` at low priority |
| `useActionState(action, initial)` | `[state, dispatch, isPending]` |
| `useOptimistic(state, reducer?)` | `[optimistic, addOptimistic]` |
| `use`, `useContext`, `useId`, `useDebugValue`, `useImperativeHandle` | React's own |

Also exported:

- **Rust-style names:** `useSignal(v)` and `useComputed(fn)`.
- **`useSignalValue(sig)`:** re-renders its component when `sig` changes.
- **Plain signals:** `signal`, `computed`, `effect`, `batch`, `untrack`,
  `startTransition`.

React's plain-value hooks still work: import them from `react`.

Every hook is built from React's public hooks. A conformance suite runs each
one beside React's original, in development and production builds and under
StrictMode. It fails when a React release adds a hook this package doesn't
cover yet.

## Motion

`@blender-ui/react/motion` is Motion for React's API on BUI's animation
engine (Rust and C; nothing runs per frame in JavaScript):

```jsx
import { motion, useMotionValue, useSpring, useTransform } from '@blender-ui/react/motion'

<motion.div initial={{ opacity: 0, y: 20 }} animate={{ opacity: 1, y: 0 }}
            whileHover={{ scale: 1.05 }} transition={{ type: 'spring', stiffness: 400 }} />
```

- **`motion.div`, `motion.span` ...** are the host elements (every element
  takes Motion's props: `initial`, `animate`, `exit`, `whileHover`,
  `whileTap`, `whileFocus`, `transition`, `variants`); `motion.Button`,
  `motion.NumberSlider` ... are Blender's widgets; `motion.create(C)` is `C`.
- **Motion values:** `useMotionValue`, `useSpring`, `useTransform`,
  `useMotionValueEvent`; `style={{ width: mv }}` follows a value without
  re-rendering.
- **Functions:** `animate`, `stagger`, `motionValue`; GSAP's API is
  `@blender-ui/native/gsap`.
- **Also on every element:** `whileInView` (with `viewport`), `whileDrag`, `drag`
  (`true`, `"x"`, `"y"`) with `dragConstraints`, `dragElastic`, `dragMomentum` and
  `dragSnapToOrigin`, and `layout` (`true`, `"position"`, `"size"`).
- Not there yet: `AnimatePresence` and `layoutId`.

## The renderer

- **Host components are HTML elements.** `className` takes Tailwind classes,
  `style` takes strings, objects or signals, and the mouse, keyboard and form
  events work (`onClick`, `onChange` ...). Events bubble, and
  `stopPropagation` works.
- **One native batch per commit:** only what changed crosses to BUI's UI
  thread.
- **React 19.3:** `<Suspense>`, `<Activity>`, transitions, actions and
  `<ViewTransition>`, which commits without animating.
- **API:** `render(element, options)` opens a window. `createRoot(window)`
  renders into one you opened with
  [`@blender-ui/native`](https://www.npmjs.com/package/@blender-ui/native),
  and `act(fn)` is for tests.
  - Window options: `title`, `width`, `height`, `resizable`, `gpu`,
    `slug`, `headless`, and `theme` (`'dark'`, the default, or `'light'`:
    Blender's Light theme; `BUI_THEME` applies otherwise).

## Standalone apps

`bui` comes with this package, through `@blender-ui/native`:

```sh
npx bui build app.jsx              # app.exe: BUI + embedded QuickJS, ~5 MB
npx bui build app.jsx --bun        # app.exe: Bun, with Node's APIs and a JIT, ~115 MB
npx bui run app.jsx                # run on the QuickJS runtime, no build
```

Options:

- `-o <file>`: the output file.
- `--console`: keep a console window for logs.
- `--dev`: React's development build.

Bundling uses [Bun](https://bun.sh), so it must be installed.

The default build runs your app on QuickJS, a small JavaScript engine embedded
in BUI. It has no Node modules (`fs` and the like) and no JIT, so keep large
lists updating through signals, or use `--bun`.

## Benchmarks

Ryzen 7 5800U laptop, Windows 11, GPU renderer, React's production build,
measured before the latest features landed.
The full method is in the [guide](https://github.com/ppmpreetham/BUI/blob/main/docs/javascript.md#performance).

**Startup:** a small React app that quits at its first frame; medians of 12
launches.

| | First frame | Launch to exit | RAM | Size |
|---|---:|---:|---:|---:|
| **`bui build`** (QuickJS) | **68 ms** | **104 ms** | **19 MB** | **4.9 MB** |
| `bui build --bun` | 79 ms | 147 ms | 35 MB | 114 MB |
| `bun app.js` | 80 ms | 152 ms | 35 MB | |
| `node app.js` | 108 ms | 187 ms | 45 MB | |

**Animation:** 3,000 cells, one changing per frame.

- **With signals:** the changing values are signals, so the grid renders once.
- **With state:** the grid re-renders every frame.

| 3,000 cells | FPS | CPU per frame | RAM |
|---|---:|---:|---:|
| Bun: with signals / with state | 58.4 / 55.8 | 13.3 / 20.8 ms | 85 / 118 MB |
| Node: with signals / with state | 56.7 / 54.8 | 14.0 / 18.9 ms | 93 / 121 MB |
| QuickJS: with signals / with state | 58.7 / 19.7 | 14.9 / 51.0 ms | 61 / 67 MB |
| *Rust, for reference* | 59.6 | 12.0 ms | 43 MB |

## Requirements

- **Windows x64** prebuilt. On **Linux**, build the `@blender-ui/native`
  addon from source (`npm run build` in `bindings/node`) and windows are
  native X11 windows. On **macOS** it's headless for now. `headless: true`
  renders off-screen anywhere.
- **Node.js 18** or newer, or Bun.
- **React 19.3.**

Guide: [docs/javascript.md](https://github.com/ppmpreetham/BUI/blob/main/docs/javascript.md) ·
Website: [bui.preetham.top](https://bui.preetham.top) ·
Source: [github.com/ppmpreetham/BUI](https://github.com/ppmpreetham/BUI) ·
License: GPL-3.0-or-later
