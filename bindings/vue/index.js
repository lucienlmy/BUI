// @blender-ui/vue: Vue 3 components in BUI's native windows.
//
//   import { render, ref } from '@blender-ui/vue'
//   import App from './App.vue'
//   render(App, { props: { name: 'BUI' }, title: 'App', width: 400, height: 300 })
//
// Everything `vue` exports (`ref`, `computed`, `h`, `defineComponent` ...), and what BUI adds
// (own.js): `createApp` and `render`, which draw into BUI windows and take the place of Vue's
// browser ones, `createRoot`, Blender's widgets and BUI's signals. `require()` gets index.cjs.
//
// SPDX-License-Identifier: GPL-2.0-or-later
export * from 'vue'
export {
  createApp, createRoot, render, addStyle,
  Button, Checkbox, Radio, NumberField, NumberSlider, TextField, TextArea, ColorSwatch, Dropdown, ProgressBar,
  signal, batch, untrack, createSignal, isSignal, Signal, Memo, memo,
} from './own.js'
