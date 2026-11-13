// Types for @blender-ui/native/host: what framework bindings share.
import type { App, AppOptions, Element, Window, WindowOptions } from './index'

export type RenderOptions = WindowOptions & AppOptions & { app?: App }

/** The window `render(..., options)` draws into: `options.app`'s, or a new App's. */
export function open (options?: RenderOptions): { app: App, window: Window }

/** `{ Button: wrap('Button'), ... }`: each Blender widget as a component. */
export function blenderWidgets<C> (wrap: (name: string) => C): Record<
  'Button' | 'Checkbox' | 'Radio' | 'NumberField' | 'NumberSlider' | 'TextField' | 'TextArea' | 'ColorSwatch' |
  'Dropdown' | 'ProgressBar', C>

/** A 53-bit hash of `text` (cyrb53) in hex: a compiled component's cache file name or scope id. */
export function hash (text: string): string

/** Scoped `<style>` blocks of compiled components: `add(key, css)` when a component's module loads,
 *  `attach(window)` to give a window those (and the ones added later) as `<style>` elements, once each. */
export function styleRegistry (): { add (key: string, css: string): void, attach (window: Window): void }

/** The props of an element that had none (`applyProps(el, NO_PROPS, props)`). */
export const NO_PROPS: Record<string, unknown>
/** JSX-style prop -> BUI event (`onClick` -> `click`). */
export const EVENTS: Record<string, string>
/** Apply the difference between two JSX-style prop objects to an element:
 *  className, style (string, object or signal), events, signals as values,
 *  text children. */
export function applyProps (el: Element, prev: Record<string, unknown>, next: Record<string, unknown>): void
/** Text children (a string or number) rather than elements. */
export function isText (children: unknown): children is string | number | bigint
