// @blender-ui/svelte: Svelte 5 components in BUI's native windows.
//
//   import { render } from '@blender-ui/svelte'
//   import App from './App.svelte'
//   render(App, { props: { name: 'BUI' }, title: 'App', width: 400, height: 300 })
//
// Svelte has no custom-renderer API: its compiled components call the DOM.
// @blender-ui/native provides that DOM (`@blender-ui/native/globals`): the
// nodes Svelte creates join a window's retained tree when inserted, and only
// the changes of each tick cross to the native UI thread. Runes update the
// nodes that show them, as in a browser.
//
// SPDX-License-Identifier: GPL-2.0-or-later
import '@blender-ui/native/globals'
import './events.js'
import { mount, unmount, flushSync } from 'svelte'
import native from '@blender-ui/native'
import host from '@blender-ui/native/host'
import Widget from './Widget.svelte'

// Components' scoped stylesheets (their <style> blocks)
const styles = host.styleRegistry()

/** A root rendering Svelte components into `window`'s body (unmounted when
 *  the window closes). */
export function createRoot (window) {
  let component = null
  styles.attach(window)
  const root = {
    /** Mount `Component` (replacing what this root showed); returns its exports. */
    render (Component, options = {}) {
      root.unmount()
      styles.attach(window)
      component = mount(Component, { target: window.body, props: options.props, context: options.context })
      return component
    },
    unmount () {
      if (component === null) return
      unmount(component)
      component = null
    },
  }
  window.on('close', () => root.unmount())
  return root
}

/** Called by compiled components that have a <style> block (compile.js). */
export function addStyle (key, css) {
  styles.add(key, css)
}

/** Open a window (and an App, unless `options.app` is given) and mount `Component` in it. */
export function render (Component, options = {}) {
  const { app, window } = host.open(options)
  const root = createRoot(window)
  const component = root.render(Component, options)
  return { app, window, root, component }
}

/** Run `fn` and apply Svelte's pending updates now (tests). */
export function act (fn) {
  return flushSync(fn)
}

// Blender's widgets as components: `<NumberSlider min={0} max={10} />`,
// `<Button onclick={...}>Apply</Button>`. Always Blender widgets; their
// lowercase HTML counterparts look like a browser's (Blender's inside `styled`).
export const { Button, Checkbox, Radio, NumberField, NumberSlider, TextField, TextArea, ColorSwatch, Dropdown,
  ProgressBar } = host.blenderWidgets((tag) => (anchor, props) => Widget(anchor, withTag(props, tag)))

/** `props` with `tag` added; reads stay live (Svelte's props are getters). */
function withTag (props, tag) {
  return new Proxy(props, {
    get: (t, k) => k === 'tag' ? tag : Reflect.get(t, k),
    has: (t, k) => k === 'tag' || Reflect.has(t, k),
    ownKeys: (t) => [...Reflect.ownKeys(t), 'tag'],
    getOwnPropertyDescriptor: (t, k) => k === 'tag'
      ? { value: tag, enumerable: true, configurable: true }
      : Reflect.getOwnPropertyDescriptor(t, k),
  })
}

// Signals: BUI's own, for state shared outside components. Each is a Svelte
// store too (`$count` in a component follows it).
export const { signal, effect, batch, untrack, createSignal, ref, isSignal, startTransition, Signal, Memo } = native
/** A derived, cached signal (`memo` in @blender-ui/native). */
export const computed = native.memo
