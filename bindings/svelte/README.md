# @blender-ui/svelte

[![npm](https://img.shields.io/npm/v/@blender-ui/svelte)](https://www.npmjs.com/package/@blender-ui/svelte)
[![license](https://img.shields.io/npm/l/@blender-ui/svelte)](https://github.com/ppmpreetham/BUI/blob/main/LICENSE)

Svelte 5 for the desktop, drawn by [BUI](https://github.com/ppmpreetham/BUI),
Blender's UI toolkit. You write components with runes, snippets, HTML
elements, CSS and Tailwind classes, and BUI renders them in native windows on
the CPU or the GPU. There's no browser, no webview and no Electron.

- **Plain Svelte.** Runes, snippets, bindings, `{#each}` / `{#if}` /
  `{#await}` / `{#key}`, context, lifecycle and scoped `<style>` blocks work
  as on the web; a `$state` change updates the one node that shows it.
- **Blender's widgets** as components: `Button`, `NumberSlider` and eight more.
- **Node.js and Bun**, and `bui build` makes one small executable.

```sh
npm install @blender-ui/svelte svelte        # or: bun add @blender-ui/svelte svelte
```

```svelte
<!-- Counter.svelte -->
<script>
  import { Button } from '@blender-ui/svelte'
  let count = $state(0)
</script>

<main class="p-6 flex flex-col gap-3 bg-[#2b2b2b] text-[#e6e6e6]">
  <span>Clicks: {count}</span>
  <Button onclick={() => count++}>Add one</Button>
</main>
```

```js
// app.js
import { render } from '@blender-ui/svelte'
import Counter from './Counter.svelte'

render(Counter, { title: 'Counter', width: 360, height: 200, gpu: true })
```

```sh
node --import @blender-ui/svelte/register app.js     # Node.js 22.15+
bun --preload @blender-ui/svelte/register app.js     # Bun
npx bui build app.js                                 # ship it: one small executable
```

`register` compiles `.svelte` (and `.svelte.js`) files on import and resolves
`svelte` to its browser build. It caches both in
`node_modules/.cache/@blender-ui/svelte`: the compiled components, and (when
Bun is installed) Svelte's runtime bundled on the first run, so it loads like
one module. `bui build` finds this package's bundler plugin by itself.

## API

| Export | What it does |
|---|---|
| `render(Component, options)` | Opens a window and mounts `Component`; returns `{ app, window, root, component }` |
| `createRoot(window)` | `{ render(Component, { props, context }), unmount() }` for a window you opened with [`@blender-ui/native`](https://www.npmjs.com/package/@blender-ui/native); closing the window unmounts it |
| `act(fn)` | Runs `fn` and applies Svelte's pending updates now (tests) |
| `Button`, `Checkbox`, `Radio`, `NumberField`, `NumberSlider`, `TextField`, `TextArea`, `ColorSwatch`, `Dropdown`, `ProgressBar` | Blender's widgets, always drawn and handled like Blender |
| `signal`, `computed`, `effect`, `batch`, `untrack`, `createSignal`, `ref`, `startTransition` | BUI's signals, for state outside components. As a value (`{label}`, `style={style}`) a signal binds its node, so a write updates it without Svelte running; as a store (`$count`) the component follows it |

Options: `props`, `context`, and the window's `title`, `width`, `height`,
`resizable`, `gpu`, `slug`, `headless`, `theme` (`'dark'` or `'light'`) and
`app` (an `App` to open the window in).

## How it works

Svelte has no custom-renderer API: compiled components call the DOM.
[`@blender-ui/native`](https://www.npmjs.com/package/@blender-ui/native)
provides that DOM: `document` makes detached nodes that join a window's
retained tree when inserted, comments are anchors that take no space, and only
the changes of each tick cross to BUI's UI thread. Components compile with
`fragments: 'tree'` (templates built with `createElement`, no HTML parser).
Svelte's event delegation works: an element with an `onclick` listens
natively, so BUI sends it its clicks. The functions Svelte's templates call on
each update (`set_text`, `set_attribute`, `set_class`, `set_style`, `clsx`)
are served by `internal.js` and write straight into BUI's nodes; each is
used only while Svelte's has the shape it was written for (a test compares
them), otherwise Svelte's own runs.

Transitions (`transition:fade` ...) run on BUI's animation engine through
`el.animate()` (an outro keeps its element until it ends; see the animation guide). Not there yet: `use:motion` and the motion stores; `{@html}` (no HTML parser); `bind:clientWidth`
and other size bindings (no `ResizeObserver`); transitions that read
`getComputedStyle` (fade's opacity) start from the value the element has.

## Requirements

- **Windows x64** prebuilt. On **Linux**, build the `@blender-ui/native`
  addon from source; on **macOS** it's headless for now.
- **Node.js 22.15** or newer (`module.registerHooks`), or Bun.
- **Svelte 5.57** or newer.

Guide: [docs/javascript.md](https://github.com/ppmpreetham/BUI/blob/main/docs/javascript.md) ·
Website: [bui.preetham.top](https://bui.preetham.top) ·
Source: [github.com/ppmpreetham/BUI](https://github.com/ppmpreetham/BUI) ·
License: GPL-3.0-or-later
