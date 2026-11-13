// Types for @blender-ui/vue: Vue's own, plus what draws into BUI windows.
import type { App as VueApp, Component, ComponentPublicInstance, DefineComponent } from 'vue'
import type { App, Window } from '@blender-ui/native'
import type { RenderOptions } from '@blender-ui/native/host'

export * from 'vue'

export interface VueRenderOptions extends RenderOptions {
  /** The root component's props. */
  props?: Record<string, unknown>
}

export interface Root {
  /** Mount a component (or an app from `createApp`), replacing what this root showed; returns the root instance. */
  render (component: Component | VueApp, options?: { props?: Record<string, unknown> }): ComponentPublicInstance
  /** Unmount what the root shows (closing the window does it too). */
  unmount (): void
}

/** Vue's `createApp`, for BUI windows: `createApp(App).use(plugin)`, then `render(app, options)`. */
export function createApp (rootComponent: Component, rootProps?: Record<string, unknown> | null): VueApp

/** A root mounting a Vue app (or a component) into `window`'s body; unmounted when the window closes. */
export function createRoot (window: Window): Root

/**
 * Open a window (and an App, unless `options.app` is given) and mount a component, or an app made
 * with `createApp`, in it.
 */
export function render (component: Component | VueApp, options?: VueRenderOptions): {
  app: App
  window: Window
  root: Root
  /** The root component's instance (what it exposes). */
  component: ComponentPublicInstance
}

/** Called by compiled single-file components that have a `<style>` block. */
export function addStyle (key: string, css: string): void

// Blender's widgets as components; each is an HTML element underneath and takes its attributes
// and events, and `v-model`.
export const Button: DefineComponent<Record<string, unknown>>
export const Checkbox: DefineComponent<Record<string, unknown>>
export const Radio: DefineComponent<Record<string, unknown>>
export const NumberField: DefineComponent<Record<string, unknown>>
export const NumberSlider: DefineComponent<Record<string, unknown>>
export const TextField: DefineComponent<Record<string, unknown>>
export const TextArea: DefineComponent<Record<string, unknown>>
export const ColorSwatch: DefineComponent<Record<string, unknown>>
export const Dropdown: DefineComponent<Record<string, unknown>>
export const ProgressBar: DefineComponent<Record<string, unknown>>

// BUI's own signals (`effect` stays Vue's).
export { signal, batch, untrack, createSignal, isSignal, Signal, Memo } from '@blender-ui/native'
/** A derived, cached BUI signal (`computed` stays Vue's). */
export { memo } from '@blender-ui/native'
