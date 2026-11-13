# @blender-ui/vue

[![npm](https://img.shields.io/npm/v/@blender-ui/vue)](https://www.npmjs.com/package/@blender-ui/vue)
[![license](https://img.shields.io/npm/l/@blender-ui/vue)](https://github.com/ppmpreetham/BUI/blob/main/LICENSE)

Vue 3 for the desktop, drawn by [BUI](https://github.com/ppmpreetham/BUI),
Blender's UI toolkit. You write single-file components with refs, `v-model`,
slots and scoped styles, and BUI renders them in native windows on the CPU or
the GPU. There's no browser, no webview and no Electron.

- **Vue's own runtime.** This is a custom renderer (`createRenderer`): reactivity,
  the virtual DOM diff, components, slots, `provide` / `inject`, `v-if` / `v-for`,
  `<script setup>` and TypeScript work as on the web, and plugins install with
  `app.use()`. Vue's compiler compiles your `.vue` files.
- **Blender's widgets** as components: `Button`, `NumberSlider` and eight more,
  with `v-model`.
- **Node.js and Bun**, and `bui build` makes one small executable.

```sh
npm install @blender-ui/vue vue        # or: bun add @blender-ui/vue vue
```

```vue
<!-- Counter.vue -->
<script setup>
import { ref } from 'vue'
import { Button } from '@blender-ui/vue'
const count = ref(0)
</script>

<template>
  <main class="p-6 flex flex-col gap-3">
    <span>Clicks: {{ count }}</span>
    <Button @click="count++">Add one</Button>
  </main>
</template>
```

```js
// app.js
import { render } from '@blender-ui/vue'
import Counter from './Counter.vue'

const { window } = render(Counter, { title: 'Counter', width: 360, height: 200, gpu: true })
// the body is the page: `min-h-screen` makes its background fill the window, not only its content
window.body.className = 'm-0 min-h-screen bg-[#2b2b2b] text-[#e6e6e6]'
```

The window's `body` is the page. A background on a component's root only paints that element's
box (the `body` has an 8 px margin by default), so put the page color on `window.body`, with
`m-0 min-h-screen` to make it fill the whole window.

```sh
node --import @blender-ui/vue/register app.js
bun --preload @blender-ui/vue/register app.js     # or preload = ["@blender-ui/vue/register"] in bunfig.toml
npx bui build app.js                              # one executable (add --bun for a Bun one)
```

`register` compiles `.vue` files with Vue's own compiler and caches the result in
`node_modules/.cache/@blender-ui/vue`: a cached start never loads the compiler (Node.js reaches its
first frame in about 125 ms, Bun in about 100). Every `import 'vue'` resolves to the one Vue this
package uses, wherever the importing file is, so refs are tracked by one runtime. `require()` works
too (`const { render } = require('@blender-ui/vue')`).

Node.js 22.15 or newer (module hooks), or Bun. The native addon is prebuilt for
Windows x64; elsewhere, build it from source (see the main README).

## API

Everything `vue` exports is exported here too (`ref`, `computed`, `h`,
`defineComponent`, `nextTick` ...), so one import is enough. The BUI parts:

| | |
| --- | --- |
| `render(component, options)` | Opens a window and mounts a component, or an app from `createApp`, into it. Returns `{ app, window, root, component }`. |
| `createApp(component, props)` | Vue's `createApp`, for BUI windows: `createApp(App).use(pinia)`, then `render(app, options)`. |
| `createRoot(window)` | A root for a window you opened yourself: `root.render(component \| app, { props })`, `root.unmount()`. Closing the window unmounts it (`onUnmounted` runs). |
| `Button`, `Checkbox`, `Radio`, `NumberField`, `NumberSlider`, `TextField`, `TextArea`, `ColorSwatch`, `Dropdown`, `ProgressBar` | Blender's widgets, always drawn the Blender way. Each is an HTML element underneath (`NumberSlider` is an `<input type="range">`). |
| `signal`, `memo`, `batch`, `untrack`, `isSignal` | BUI's own signals: a signal as the value of an attribute or style binds it, and the node follows it without a re-render. |

`render` options: `props`, the window (`title`, `width`, `height`, `resizable`),
`gpu`, `slug`, `headless` (off-screen, for tests and CI), `theme` (`'dark'` or
`'light'`) and `app` (reuse an `App`, to open several windows).

## What works

- **Elements** are HTML elements. `class` takes Tailwind classes, objects and
  arrays; `:style` takes strings and objects; `v-show` works.
- **Events:** `@click`, `@mousedown`, `@mouseup`, `@mousemove`, `@mouseenter`,
  `@mouseleave`, `@wheel`, `@keydown`, `@keyup`, `@input` and `@change`, with the
  modifiers `.stop`, `.prevent`, `.self`, `.once`, `.enter` and the other key
  and mouse-button ones.
- **Errors in event handlers** go through Vue's error handling (`onErrorCaptured`, then
  `app.config.errorHandler`), not straight out of the event loop.
- **`v-model`** on `<input>` (text, number, range, checkbox, radio), `<textarea>`,
  `<select>` and the Blender widgets. A checkbox can be bound to an
  array or a `Set` (and takes `true-value` / `false-value`), and `:value` keeps its type: a
  number stays a number. `number` and `range` inputs give numbers.
- **Styles:** a component's `<style>` and `<style scoped>` blocks (plain CSS) work;
  BUI adds each component's stylesheet to a window once.
- **Teleport** to an element (not a selector: there is no page to search).

## What doesn't (yet)

- **`<Transition>`, `<TransitionGroup>` and `<KeepAlive>`'s animations:** they need a
  browser's CSS transitions and layout APIs.
- **`v-html`** and `innerHTML`: build the content with elements.
- **`v-model.trim` / `.number` / `.lazy` on a plain `<input>`:** the template compiler drops
  modifiers there. They work on the Blender widgets (`<TextField v-model.trim="t">`).
  `type="number"` and `type="range"` inputs give numbers anyway.
- **`<style lang="scss">`** and the like: only CSS. `v-bind()` in `<style>`, `<style module>`,
  `src="…"` blocks and non-HTML templates (`<template lang="pug">`) are compile errors, not
  silently ignored.
- **TypeScript enums and namespaces on Node.js:** it strips types only, as `node file.ts`
  does. Use a const object instead of an enum, or run on Bun, which compiles all of TypeScript.
- **`<select multiple>`.**
- **Browser APIs** (`window`, `localStorage`, `fetch`'s DOM-side uses, `getComputedStyle`)
  and **libraries built on the DOM and on CSS** (component libraries such as Vuetify or
  Element Plus): not available. Libraries that are plain JavaScript on top of Vue (a store,
  a router in memory mode) don't touch the DOM, but none is tested with BUI yet.
- Events BUI doesn't report (`focus`, `blur`, `dblclick`, `submit` ...) are accepted but
  never fire.

## Testing

```js
import { render, nextTick } from '@blender-ui/vue'
const { app, window } = render(Counter, { headless: true, width: 200, height: 120 })
await window.screenshot()         // one rendered frame: clicks are hit-tested against it
window.click(10, 10)
await nextTick()
```

`window.screenshot()` returns the pixels; `@blender-ui/native`'s window has the
rest (`click`, `getBoundingClientRect` ...).

## License

GPL-3.0-or-later, as BUI. Vue is MIT.
