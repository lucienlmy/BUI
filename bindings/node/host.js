// What every framework binding shares, so each one is a thin adapter over
// the DOM-like tree (dom.js):
//
//   open(options)            the App and window a `render(..., options)` opens
//   blenderWidgets(wrap)     Blender's widgets as the framework's components
//   applyProps(el, prev, next)
//                            JSX-style props (className, style, onClick ...)
//                            onto an element: for prop-based renderers (React;
//                            a Vue or Solid universal renderer the same way)
//
// DOM-based frameworks (Svelte) need none of the props: their compiled code
// calls the DOM, which `@blender-ui/native/globals` provides.
//
// SPDX-License-Identifier: GPL-2.0-or-later
'use strict'

const { App } = require('./index')
const { BLENDER_COMPONENTS, MOTION_PROPS } = require('./dom')
const { isSignal } = require('./signals')

/**
 * The window `render(..., options)` draws into: `options.app`'s, or a new App's.
 * @param {{ app?: App, gpu?: boolean, slug?: boolean, headless?: boolean, theme?: 'dark' | 'light',
 *           title?: string, width?: number, height?: number, resizable?: boolean }} options
 */
function open (options = {}) {
  const app = options.app ?? new App({ gpu: options.gpu, slug: options.slug, headless: options.headless, theme: options.theme })
  return { app, window: app.window(options) }
}

/** `{ Button: wrap('Button'), ... }`: each Blender widget as a component. */
function blenderWidgets (wrap) {
  const out = {}
  for (const name of Object.keys(BLENDER_COMPONENTS)) {
    const C = wrap(name)
    Object.defineProperty(C, 'name', { value: name })
    out[name] = C
  }
  return out
}

// ---- JSX-style props ----------------------------------------------------------

// prop -> BUI event; onChange is per keystroke on text fields, as in React DOM.
const EVENTS = {
  onClick: 'click', onMouseDown: 'mousedown', onMouseUp: 'mouseup', onMouseMove: 'mousemove',
  onMouseEnter: 'mouseenter', onMouseLeave: 'mouseleave', onWheel: 'wheel',
  onKeyDown: 'keydown', onKeyUp: 'keyup', onInput: 'input', onChange: 'change',
}
const TEXT_INPUTS = new Set(['textarea'])
const ATTR_ALIASES = { className: 'class', htmlFor: 'for', tabIndex: 'tabindex' }
const SKIP = new Set(['children', 'key', 'ref', 'dangerouslySetInnerHTML', 'suppressHydrationWarning',
  'suppressContentEditableWarning'])
/** The props of an element that had none. */
const NO_PROPS = {}

function eventName (el, prop) {
  if (prop === 'onChange') {
    // text fields report every edit as `input`; checkboxes, selects: `change`
    const type = el.getAttribute('type')
    if (TEXT_INPUTS.has(el.localName) || (el.localName === 'input' &&
        !['checkbox', 'radio', 'range', 'color', 'file'].includes(type))) return 'input'
  }
  return EVENTS[prop]
}

/** Listen with `fn` (or stop, when it isn't a function) for prop `k`.
 *  The DOM event is remembered with it: onChange's depends on `type`. */
function listen (el, k, fn) {
  const props = (el._props ??= {})
  const cur = props[k]
  const type = typeof fn === 'function' ? eventName(el, k) : null
  if (cur !== undefined && cur.fn === fn && cur.type === type) return
  if (cur !== undefined) el.removeEventListener(cur.type, cur.fn)
  if (type === null) delete props[k]
  else {
    el.addEventListener(type, fn)
    props[k] = { type, fn }
  }
}

/** Text children (`<b>{n}</b>`) rather than elements. */
function isText (children) {
  return typeof children === 'string' || typeof children === 'number' || typeof children === 'bigint'
}

/** Motion's props (`animate`, `whileHover` ...) on any element; `style` is the style's own. */
function isMotionProp (k) { return k in MOTION_PROPS && k !== 'style' }

/** Apply the difference between two prop objects to an element. Hot: React
 *  calls it for every element of a re-rendered list, mostly with equal props. */
function applyProps (el, prev, next) {
  if (prev !== NO_PROPS) {
    for (const k in prev) {
      if (k in next || SKIP.has(k)) continue
      if (k in EVENTS) {
        listen(el, k, null)
      } else if (isMotionProp(k)) {
        el.setMotion(k, null)
      } else if (k === 'style') {
        el.style = null // the setter also lets go of the Motion values in it
      } else {
        el.removeAttribute(ATTR_ALIASES[k] ?? k)
      }
    }
  }
  let events = false
  for (const k in next) {
    const v = next[k]
    if (prev[k] === v || SKIP.has(k)) continue
    if (k in EVENTS) {
      events = true // after the attributes: onChange's event depends on `type`
    } else if (isMotionProp(k)) {
      el.setMotion(k, v) // read by the engine once, when the tick's changes are sent
    } else if (k === 'style') {
      el.style = v // a string, an object (motion values too), a signal of either, or null
    } else if (v == null || v === false) {
      el.removeAttribute(ATTR_ALIASES[k] ?? k)
    } else if (isSignal(v)) {
      el.setAttribute(ATTR_ALIASES[k] ?? k, v) // bound: updates without a re-render
    } else {
      el.setAttribute(ATTR_ALIASES[k] ?? k, v === true ? '' : v)
    }
  }
  if (events || (next.onChange !== undefined && prev.type !== next.type)) {
    for (const k in next) if (k in EVENTS) listen(el, k, next[k])
  }
  // text children; text -> elements is the renderer's to clear (React's
  // resetTextContent, before it inserts them)
  const c = next.children
  if (c !== prev.children && isText(c)) el.textContent = String(c)
}

module.exports = { open, blenderWidgets, applyProps, isText, NO_PROPS, EVENTS }
