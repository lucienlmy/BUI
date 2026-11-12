// @blender-ui/native: Blender's UI from JavaScript.
//
// A DOM-like retained tree (createElement, setAttribute, appendChild,
// addEventListener) rendered by BUI in native windows: HTML elements, CSS,
// Tailwind classes. BUI runs on its own thread; each window collects the
// tree changes of a tick and sends them in one call (an Int32Array of ops
// and a string table), so only what changed crosses into Rust. Events come
// back asynchronously.
//
// SPDX-License-Identifier: GPL-2.0-or-later
'use strict'

// The native layer: the QuickJS runtime's built-in one (bui-runtime, apps
// made by `bui build`), or the napi addon (Node.js, Bun).
const native = globalThis.__BUI_NATIVE__ ?? loadAddon()

function loadAddon () {
  const path = require('node:path')
  const { TAG, packageName } = require('./platform')
  const pkg = packageName()
  // a relative path starts at the working directory, not at this package
  const env = process.env.BUI_NODE_ADDON && path.resolve(process.env.BUI_NODE_ADDON)
  const built = path.join(__dirname, `blenderui.${TAG}.node`) // npm run build in a clone
  try {
    if (env) return require(env)
    if (require('node:fs').existsSync(built)) return require(built)
    // a literal name: `bun build --compile` (and other bundlers) can then embed the addon
    if (TAG === 'win32-x64') return require('@blender-ui/native-win32-x64')
    return require(pkg)
  } catch (err) {
    throw new Error(`@blender-ui/native: no native addon for ${TAG}. ` +
      `The prebuilt one is the optional dependency ${pkg} (Windows x64, Linux x64 with glibc 2.35 ` +
      'or later, macOS): install without --omit=optional / --no-optional. Elsewhere, build it from a clone of ' +
      'https://github.com/ppmpreetham/BUI with `npm run build` in bindings/node (needs Rust, CMake and a C compiler), ' +
      'then use that package, or point BUI_NODE_ADDON at the built blenderui.<platform>-<arch>.node. ' +
      `(${err.message.split('\n')[0]})`,
    { cause: err })
  }
}
const signals = require('./signals')
const dom = require('./dom')
const { OP, HTMLElement, TextNode, Comment, DocumentFragment, createElement } = dom
const { Motions } = require('./anim')

const LAYERS = ['background', 'bottom', 'top', 'overlay']
const EDGES = ['top', 'bottom', 'left', 'right']
const KEYBOARD = ['none', 'exclusive', 'on-demand']

function oneOf (what, value, names) {
  if (value != null && !names.includes(value)) {
    throw new TypeError(`${what}: '${value}' is not one of ${names.map((n) => `'${n}'`).join(', ')}`)
  }
}

/** The window options as the addon takes them; unknown layer-shell names throw here. */
function windowConfig (o) {
  const l = o.layerShell
  if (l) {
    oneOf('layerShell.layer', l.layer, LAYERS)
    oneOf('layerShell.keyboard', l.keyboard, KEYBOARD)
    for (const e of l.anchor ?? []) oneOf('layerShell.anchor', e, EDGES)
  }
  return {
    title: o.title, width: o.width, height: o.height, resizable: o.resizable, gpu: o.gpu,
    transparent: o.transparent, alwaysOnTop: o.alwaysOnTop, frame: o.frame, layerShell: l,
  }
}

class Window {
  constructor (app, id, options = {}) {
    this.app = app
    this.id = id
    this.closed = false
    this._next = 1
    this._nodes = new Map()
    this._ops = []
    this._strings = []
    this._stringIds = new Map()
    this._motion = [] // [which, value, ...]: the Motion props of this tick's MOTION ops
    this._scheduled = false
    this._on = { close: new Set(), focus: new Set(), blur: new Set() }
    this._frameCallbacks = new Map()
    this._nextFrameId = 1
    this._framePending = false
    this._inFrame = false
    this._orphans = []
    /** The root element: node 0. */
    this.body = new HTMLElement('body')
    this.body.ownerWindow = this
    this.body.id = 0
    this._nodes.set(0, this.body)
    app.native.openWindow(id, windowConfig(options))
  }

  /** An element. Capitalized tags are Blender's widgets (`Button`, `NumberSlider` ...). */
  createElement (tag) { return createElement(this, tag) }

  /** A text node; `text` may be a signal (the text then follows it). */
  createTextNode (text) {
    const t = new TextNode('')
    t.data = text
    t._attach(this)
    return t
  }

  createComment (data = '') {
    const c = new Comment(String(data))
    c._attach(this)
    return c
  }

  createDocumentFragment () { return new DocumentFragment() }

  /** Changes are sent once per tick; `flush()` sends them now. */
  flush () {
    this._scheduled = false
    if (this._orphans.length > 0) this._sweep()
    if (this._ops.length === 0) return
    const ops = this.closed ? null : Int32Array.from(this._ops)
    const strings = this._strings
    const motion = this._motion.length > 0 ? this._motion : undefined
    try {
      // while animating, changes render on the next vsync (one rebuild per frame). A Motion prop
      // the addon can't read throws here, after it applied the rest of the batch (without it): the
      // elements JavaScript made are in the native tree either way
      if (ops) this.app.native.commit(this.id, ops, strings, this._inFrame || this._framePending, motion)
    } finally {
      this._ops.length = 0 // the array keeps its capacity for the next tick
      this._strings = []
      this._stringIds.clear()
      if (motion) this._motion = []
    }
  }

  /**
   * Run `cb(timestamp)` before the next frame renders, paced by the display
   * (like the browser's). Changes made in it render in that frame.
   */
  requestAnimationFrame (cb) {
    const id = this._nextFrameId++
    this._frameCallbacks.set(id, cb)
    if (!this._framePending) {
      this._framePending = true
      this.flush()
      this.app.native.requestFrame(this.id)
    }
    return id
  }

  cancelAnimationFrame (id) { this._frameCallbacks.delete(id) }

  _frame () {
    this._framePending = false
    const cbs = this._frameCallbacks // callbacks requested in a callback run in the next frame
    this._frameCallbacks = new Map()
    this._inFrame = true
    const now = performance.now()
    try {
      for (const cb of cbs.values()) {
        try { cb(now) } catch (err) { queueMicrotask(() => { throw err }) }
      }
    } finally {
      this._inFrame = false
    }
    this.flush()
  }

  /** Render and read the pixels: `{ width, height, data }` (RGBA). */
  async screenshot () {
    this.flush()
    return this.app.native.screenshot(this.id)
  }

  /** Click at (x, y) logical pixels (tests). */
  click (x, y) {
    this.flush()
    this.app.native.simulateClick(this.id, x, y)
  }

  close () {
    this.flush()
    this.app.native.closeWindow(this.id)
    this._closed()
  }

  on (event, fn) {
    const set = this._on[event]
    if (!set) throw new Error(`blenderui: unsupported window event "${event}"`)
    set.add(fn)
    return this
  }

  _emit (event) {
    for (const fn of this._on[event]) {
      try { fn() } catch (err) { queueMicrotask(() => { throw err }) }
    }
  }

  _closed () {
    if (this.closed) return
    this.closed = true
    this._emit('close')
    // the native window is gone: free what follows signals or holds listeners
    for (const n of this._nodes.values()) {
      n._unbindAll()
      n._listeners?.clear()
    }
    this._frameCallbacks.clear()
    this.flush()
    this.app._windowClosed(this)
  }

  _op (...args) {
    this._ops.push(...args)
    this._schedule()
  }

  /** Flush once, at the end of this tick. */
  _schedule () {
    if (this._scheduled) return
    this._scheduled = true
    queueMicrotask(() => this.flush())
  }

  /** A Motion prop of node `id` (read by the addon when this tick is sent). */
  _motionOp (id, which, value) {
    this._op(OP.MOTION, id, which, this._motion.length / 2)
    this._motion.push(which, value)
  }

  _s (str) {
    let i = this._stringIds.get(str)
    if (i === undefined) {
      i = this._strings.push(str) - 1
      this._stringIds.set(str, i)
    }
    return i
  }

  /** A node of `document` was removed: freed at the next flush unless reinserted. */
  _orphan (node) {
    this._orphans.push(node)
    this._schedule()
  }

  _sweep () {
    const orphans = this._orphans
    this._orphans = []
    for (const n of orphans) if (n.parentNode === null) n.destroy()
  }

  _free (node) {
    node._unbindAll()
    if (node.id === 0 || !this._nodes.delete(node.id)) return // the body, or freed already
    this._op(OP.FREE, node.id)
  }
}

class App {
  /**
   * @param {{ gpu?: boolean, slug?: boolean, headless?: boolean, theme?: 'dark' | 'light',
   *           quitOnLastWindowClosed?: boolean }} [options]
   */
  constructor (options = {}) {
    this.windows = new Map()
    this._nextWindow = 1
    this.quitOnLastWindowClosed = options.quitOnLastWindowClosed ?? true
    this.native = new native.NativeApp(
      { gpu: options.gpu, slug: options.slug, headless: options.headless, theme: options.theme },
      raw => this._event(raw))
    /** Its animations (gsap, motion, `el.animate`): the newest App's take selectors. */
    this.motions = new Motions(this)
  }

  get headless () { return this.native.headless }

  /** Move the animation clock `ms` forward; headless, every window renders
   *  that frame (tests step animations frame-exactly). Resolves once the
   *  step's callbacks (onUpdate ...) have run. */
  advanceClock (ms) {
    for (const w of this.windows.values()) w.flush()
    const step = new Promise((resolve) => (this._clock ??= []).push(resolve))
    this.native.advanceClock(ms)
    return step
  }

  /** Open a window: `{ title, width, height, resizable, gpu }`. */
  window (options) {
    const w = new Window(this, this._nextWindow++, options)
    this.windows.set(w.id, w)
    return w
  }

  /** GPU text: Slug (glyph outlines) or the glyph atlas. */
  setSlug (on) { this.native.setSlug(!!on) }

  /**
   * Blender's 'dark' (default) or 'light' theme, or a theme loaded with `loadTheme`; open windows
   * redraw. A built-in name switches at once; a loaded theme's name returns a promise (the theme
   * lives on the UI thread) that rejects for an unknown name.
   */
  setTheme (name) {
    name = String(name)
    if (this.native.setTheme(name)) return Promise.resolve()
    return this.native.setCustomTheme(name).then((ok) => {
      if (!ok) throw new RangeError(`unknown theme ${name} (expected 'dark', 'light' or a theme loaded with loadTheme)`)
    })
  }

  /**
   * Register the themes of a theme file (a path) or of theme JSON text (gpui-kit's format with
   * Blender's color names, docs/theming.md) and resolve with how many were added. Choose one with
   * `setTheme(name)`. Rejects with the line and column of a mistake; a file with one adds no themes.
   */
  loadTheme (source) {
    const text = String(source)
    return this.native.loadTheme(text, !text.trimStart().startsWith('{'))
  }

  /** The names of the themes loaded with `loadTheme`. */
  themes () { return this.native.themes() }

  /** One color of the active theme: a key such as 'button.background' and a '#rrggbb[aa]' string or [r, g, b, a] in 0..1. */
  async setThemeColor (key, color) {
    let c = color
    if (typeof color === 'string') {
      let h = color.replace('#', '')
      if (h.length <= 4) h = [...h].map((ch) => ch + ch).join('')
      if (!/^[0-9a-f]{6}([0-9a-f]{2})?$/i.test(h)) throw new RangeError(`not a color: ${color}`)
      c = [0, 2, 4, 6].slice(0, h.length / 2).map((i) => parseInt(h.slice(i, i + 2), 16) / 255)
    }
    const [r, g, b, a = 1] = c
    if (!(await this.native.setThemeColor(String(key), r, g, b, a))) throw new RangeError(`no theme color ${key}`)
  }

  /** One number of the active theme: 'button.roundness', 'button.border.width', 'button.corners.top_left' (false keeps the corner square), ... */
  async setThemeValue (key, value) {
    if (!(await this.native.setThemeValue(String(key), Number(value)))) throw new RangeError(`no theme value ${key}`)
  }

  /** Close every window and end the UI thread (Node may then exit). */
  quit () {
    if (this._quit) return
    this._quit = true
    this.motions.close()
    for (const w of [...this.windows.values()]) {
      w.flush()
      w._closed()
    }
    this.native.quit()
  }

  _event (raw) {
    if (raw.kind === 'motion') return this.motions._event(raw)
    if (raw.kind === 'clock') return this._clock?.shift()?.()
    const win = this.windows.get(raw.window)
    if (!win) return
    if (raw.kind === 'close') {
      win._closed()
      return
    }
    if (raw.kind === 'frame') {
      win._frame()
      return
    }
    if (raw.kind === 'focus' || raw.kind === 'blur') {
      win._emit(raw.kind)
      return
    }
    win._nodes.get(raw.target)?._dispatch(raw)
  }

  _windowClosed (win) {
    this.windows.delete(win.id)
    if (this.quitOnLastWindowClosed && this.windows.size === 0 && !this._quit) {
      this._quit = true
      this.motions.close()
      this.native.quit()
    }
  }
}

module.exports = { ...dom, App, Window, native, ...signals }
