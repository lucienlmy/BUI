// Types for @blender-ui/native.
import type { MotionProps, MotionValue } from './motion'
export type { MotionProps } from './motion'

export interface AppOptions {
  /** Draw on the GPU: Direct3D 11 on Windows, OpenGL 3.3 on Linux. */
  gpu?: boolean
  /** GPU text from glyph outlines (Slug, default) or a per-glyph atlas. */
  slug?: boolean
  /** No native windows: tests, screenshots, CI. */
  headless?: boolean
  /** Blender's 'dark' (default) or 'light' theme (BUI_THEME otherwise). */
  theme?: 'dark' | 'light'
  /** Quit when the last window closes (default true). */
  quitOnLastWindowClosed?: boolean
}

export interface WindowOptions {
  title?: string
  width?: number
  height?: number
  resizable?: boolean
  gpu?: boolean
  /** Per-pixel alpha: only the elements show (no frame on Windows). */
  transparent?: boolean
  /** Stay above other windows: Windows, macOS and X11. A Wayland toplevel cannot; use `layerShell`. */
  alwaysOnTop?: boolean
  /** `false`: no frame (Electron's `frame`). */
  frame?: boolean
  /** A wlr-layer-shell surface (Wayland compositors with layer shell: KDE Plasma, wlroots);
   *  a normal window elsewhere. */
  layerShell?: LayerShellOptions
}

export interface LayerShellOptions {
  /** Default `'overlay'`. */
  layer?: 'background' | 'bottom' | 'top' | 'overlay'
  /** Edges the surface sticks to; both edges of an axis fill it. */
  anchor?: Array<'top' | 'bottom' | 'left' | 'right'>
  /** Space the compositor keeps free for the surface (a panel), in pixels. */
  exclusiveZone?: number
  /** Default `'none'`. */
  keyboard?: 'none' | 'exclusive' | 'on-demand'
}

export type EventType = 'click' | 'mousedown' | 'mouseup' | 'mousemove' | 'mouseenter' | 'mouseleave' |
  'wheel' | 'keydown' | 'keyup' | 'change' | 'input'

export class BuiEvent {
  type: EventType
  target: Element
  currentTarget: Element
  clientX: number
  clientY: number
  x: number
  y: number
  button: number
  detail: number
  deltaY: number
  key?: string
  char?: string
  shiftKey: boolean
  ctrlKey: boolean
  altKey: boolean
  metaKey: boolean
  timeStamp: number
  preventDefault (): void
  stopPropagation (): void
  stopImmediatePropagation (): void
  /** Set by stopPropagation(). */
  cancelBubble: boolean
  /** The target and its ancestors, innermost first. */
  composedPath (): Element[]
}

/** A node lives in a window (`ownerWindow`) or is detached (made by
 *  `document` or `cloneNode`): it is created natively, with its subtree, when
 *  inserted into a window's tree. */
export class Node {
  readonly ownerWindow: Window | null
  /** The native id (-1 while detached). */
  readonly id: number
  readonly nodeType: number
  readonly nodeName: string
  readonly parentNode: Element | DocumentFragment | null
  readonly parentElement: Element | DocumentFragment | null
  /** The children (a new array each time). */
  readonly childNodes: Node[]
  readonly firstChild: Node | null
  readonly lastChild: Node | null
  readonly nextSibling: Node | null
  readonly previousSibling: Node | null
  /** Under a window's body. */
  readonly isConnected: boolean
  readonly ownerDocument: Document
  textContent: string
  appendChild<T extends Node> (child: T): T
  insertBefore<T extends Node> (child: T, before: Node | null): T
  removeChild<T extends Node> (child: T): T
  replaceChild<T extends Node> (child: Node, old: T): T
  remove (): void
  before (...nodes: (Node | string)[]): void
  after (...nodes: (Node | string)[]): void
  append (...nodes: (Node | string)[]): void
  prepend (...nodes: (Node | string)[]): void
  replaceWith (...nodes: (Node | string)[]): void
  contains (other: Node | null): boolean
  hasChildNodes (): boolean
  /** A detached copy (and its subtree when `deep`); listeners aren't copied. */
  cloneNode (deep?: boolean): this
  /** Free the node and its subtree natively (after removing it). Nodes made
   *  by `document` are freed for you once removed and dropped. */
  destroy (): void
}

export class TextNode extends Node {
  /** A string, or a signal (the text then follows it). */
  data: string | ReadSignal<unknown>
  nodeValue: string
}

/** A comment (an anchor for DOM frameworks): natively an empty text node. */
export class Comment extends Node {
  data: string
  nodeValue: string
}

/** Nodes inserted together: inserting the fragment moves its children. */
export class DocumentFragment extends Node {}

/** `el.style`: the inline style. */
export interface Style {
  cssText: string
  getPropertyValue (name: string): string
  setProperty (name: string, value: string | null, priority?: string): void
  removeProperty (name: string): string
}

export interface ClassList {
  readonly value: string
  readonly length: number
  add (...names: string[]): void
  remove (...names: string[]): void
  toggle (name: string, force?: boolean): boolean
  contains (name: string): boolean
}

/** A style object's values; motion values (`{ x: mv }`) are read by the engine each frame. */
type StyleValue = string | Record<string, string | number | MotionValue<any> | null | undefined>

/** A Web Animations keyframe: property values, `offset` (0..1) and `easing`. */
export interface Keyframe { offset?: number | null, easing?: string, [property: string]: string | number | null | undefined }
export interface KeyframeAnimationOptions {
  /** ms */
  duration?: number
  /** ms */
  delay?: number
  /** any ease name (CSS, GSAP, Motion, Blender) */
  easing?: string
  iterations?: number
  direction?: 'normal' | 'reverse' | 'alternate' | 'alternate-reverse'
  fill?: 'none' | 'forwards' | 'backwards' | 'both' | 'auto'
  playbackRate?: number
}
/** A Web Animations `Animation`, run by the engine. */
export interface Animation {
  readonly playState: 'running' | 'paused' | 'finished' | 'idle'
  readonly pending: boolean
  /** ms */
  currentTime: number
  playbackRate: number
  readonly finished: Promise<Animation>
  onfinish: ((event: { type: 'finish', target: Animation }) => void) | null
  oncancel: ((event: { type: 'cancel', target: Animation }) => void) | null
  play (): void
  pause (): void
  reverse (): void
  finish (): void
  cancel (): void
  updatePlaybackRate (rate: number): void
}

export class Element extends Node {
  readonly tagName: string
  readonly localName: string
  readonly namespaceURI: string
  readonly children: Element[]
  className: string
  readonly classList: ClassList
  /** Reads the inline style; takes CSS text, an object such as
   *  `{ backgroundColor: 'red', width: 40 }`, or a signal of either. */
  get style (): Style
  set style (v: StyleValue | ReadSignal<StyleValue> | null)
  /** A value, or a signal: the attribute then follows it (null / false removes it). */
  setAttribute (name: string, value: unknown): void
  getAttribute (name: string): string | null
  hasAttribute (name: string): boolean
  removeAttribute (name: string): void
  toggleAttribute (name: string, force?: boolean): boolean
  getAttributeNames (): string[]
  /** Any type; BUI's own (`EventType`) arrive from the UI thread. */
  addEventListener (type: EventType | (string & {}), listener: (event: BuiEvent) => void, options?: unknown): void
  removeEventListener (type: EventType | (string & {}), listener: (event: BuiEvent) => void, options?: unknown): void
  /** Run the listeners for `event.type` here (and up, if it bubbles). */
  dispatchEvent (event: { type: string, bubbles?: boolean }): boolean
  /** Web Animations, run by the engine: `animate([{ opacity: 0 }, { opacity: 1 }], { duration: 300, easing: 'ease-out' })`.
   *  The values stay where it ends (as `fill: 'forwards'`). */
  animate (keyframes: Keyframe[] | Record<string, unknown> | null, options?: number | KeyframeAnimationOptions): Animation
  /** One of Motion's props (`MotionProps`); null removes it. */
  setMotion<K extends keyof MotionProps> (name: K, value: MotionProps[K] | null): void
  getBoundingClientRect (): Promise<{ x: number, y: number, width: number, height: number } | null>
}

/** An HTML element: its DOM properties reflect attributes. */
export class HTMLElement extends Element {
  /** Form controls: the latest value from input / change events, else the attribute. */
  value: string
  checked: boolean
  disabled: boolean
  hidden: boolean
  type: string
}

/** The window-less document: its nodes are detached until inserted into a
 *  window's tree. DOM frameworks use it through `@blender-ui/native/globals`. */
export class Document {
  readonly head: HTMLElement
  /** Capitalized tags are Blender's widgets. */
  createElement (tag: string): HTMLElement
  createElementNS (ns: string, tag: string): HTMLElement
  createTextNode (text: string | ReadSignal<unknown>): TextNode
  createComment (data?: string): Comment
  createDocumentFragment (): DocumentFragment
  importNode<T extends Node> (node: T, deep?: boolean): T
}
export const document: Document

/** BUI's event types and their native bits. */
export const EVENT_BITS: Record<EventType, number>
/** Capitalized Blender widgets: [HTML tag, input type]. */
export const BLENDER_COMPONENTS: Record<string, [string, string?]>

export class Window {
  readonly app: App
  readonly id: number
  readonly body: HTMLElement
  closed: boolean
  /** Capitalized tags are Blender's widgets (`Button`, `NumberSlider` ...). */
  createElement (tag: string): HTMLElement
  createTextNode (text: string | ReadSignal<unknown>): TextNode
  createComment (data?: string): Comment
  createDocumentFragment (): DocumentFragment
  /** Send pending changes now (they are otherwise sent once per tick). */
  flush (): void
  /** Run `cb` before the next frame, paced by the display. */
  requestAnimationFrame (cb: (time: number) => void): number
  cancelAnimationFrame (id: number): void
  screenshot (): Promise<{ width: number, height: number, data: Buffer }>
  click (x: number, y: number): void
  close (): void
  /** `focus` / `blur`: the window got or lost the keyboard focus. */
  on (event: 'close' | 'focus' | 'blur', listener: () => void): this
}

export class App {
  constructor (options?: AppOptions)
  readonly headless: boolean
  readonly windows: Map<number, Window>
  quitOnLastWindowClosed: boolean
  window (options?: WindowOptions): Window
  setSlug (on: boolean): void
  /** Blender's 'dark' (default) or 'light' theme, or a theme loaded with `loadTheme` (that returns a promise); open windows redraw. */
  setTheme (name: 'dark' | 'light'): void
  setTheme (name: string): Promise<void>
  /** Register the themes of a theme file (a path) or of theme JSON text; resolves with how many were added. */
  loadTheme (source: string): Promise<number>
  /** The names of the themes loaded with `loadTheme`. */
  themes (): Promise<string[]>
  /** One color of the active theme: a key such as 'button.background', '#rrggbb[aa]' or [r, g, b, a] in 0..1. */
  setThemeColor (key: string, color: string | [number, number, number, number?]): Promise<void>
  /** One number of the active theme: 'button.roundness', 'button.border.width', 'button.corners.top_left', ... */
  setThemeValue (key: string, value: number | boolean): Promise<void>
  /** Move the animation clock `ms` forward; headless, every window renders that
   *  frame (tests step animations frame-exactly). Resolves once the step's
   *  callbacks have run. */
  advanceClock (ms: number): Promise<void>
  quit (): void
}

export function styleText (style: Record<string, string | number | null | undefined>): string

// Signals: BUI's hooks (the Rust crate's Signal / Memo / Effect).
// Bind one to an attribute, `style` or text and that node alone updates.

export interface ReadSignal<T> {
  /** Read, subscribing the running memo / effect. */
  readonly value: T
  get (): T
  /** Read without subscribing. */
  peek (): T
  with<R> (fn: (value: T) => R): R
  /** Call `fn` now and on every change; returns the unsubscribe function. */
  subscribe (fn: (value: T) => void): () => void
  /** `count + 1`, `count > 5` read the value (`===` and truthiness don't: use `.value`). */
  valueOf (): T
}

export class Signal<T> implements ReadSignal<T> {
  constructor (value: T, options?: { equals?: (a: T, b: T) => boolean })
  value: T
  get (): T
  peek (): T
  set (value: T): void
  update (fn: (value: T) => T): void
  with<R> (fn: (value: T) => R): R
  subscribe (fn: (value: T) => void): () => void
  valueOf (): T
}

export class Memo<T> implements ReadSignal<T> {
  readonly value: T
  get (): T
  peek (): T
  with<R> (fn: (value: T) => R): R
  subscribe (fn: (value: T) => void): () => void
  valueOf (): T
}

export function signal<T> (value: T, options?: { equals?: (a: T, b: T) => boolean }): Signal<T>
/** A lazily derived, cached value, recomputed when the signals it read change. */
export function memo<T> (fn: () => T, options?: { equals?: (a: T, b: T) => boolean }): Memo<T>
export const computed: typeof memo
/** Runs `fn` now and when what it read changes; `fn` may return a cleanup. Returns `dispose`. */
export function effect (fn: () => void | (() => void)): () => void
/** Several writes, one update. */
export function batch<R> (fn: () => R): R
export function untrack<R> (fn: () => R): R
/** Leptos / Solid style getter and setter. */
export function createSignal<T> (value: T): [() => T, (value: T | ((prev: T) => T)) => void]
export function isSignal (x: unknown): x is ReadSignal<unknown>
/** Writes in `fn` update what they affect after urgent work, in slices. Settles
 *  when done; rejects with the error of an async `fn`. */
export function startTransition (fn: () => void | Promise<void>): Promise<void>
/** Run `fn` at urgent priority, even inside a transition. */
export function urgent<R> (fn: () => R): R

/** A ref whose `.current` effects and memos can follow (reading it while a
 *  React component renders doesn't subscribe). */
export class Ref<T> extends Signal<T> { current: T }
export function ref<T> (value: T): Ref<T>
/** Run `fn` outside any owning effect: effects it creates live until disposed. */
export function unowned<R> (fn: () => R): R

/** The addon (internal), plus counters for benchmarks. */
export const native: {
  /** CPU cycles of every thread of this process (Windows), or null. */
  processCpuCycles (): number | null
  /** The x86-64 time-stamp counter, or null. */
  timestampCounter (): number | null
  [key: string]: unknown
}
