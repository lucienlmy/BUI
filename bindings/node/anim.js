// Animation from JavaScript: what `gsap.js`, `motion.js`, `el.animate()` and
// the Motion props (`dom.js`) share. Every motion runs in Rust
// (`blenderui::motion`, on the C engine): a call hands its objects to the
// addon once, which reads them there (errors throw here, naming the option),
// and the UI thread runs the frames. JavaScript holds handles (ids it
// chooses) and hears only the events its callbacks want, and a motion's end.
//
// SPDX-License-Identifier: GPL-2.0-or-later
'use strict'

const { OP, isMotionValue } = require('./dom')

// The engine's events (kit/include/BUI_anim.h, BK_EV_MOTION_*) and a
// timeline's call() (blenderui-jsview/src/motion.rs, EV_CALL).
const EV = { START: 1, UPDATE: 2, COMPLETE: 4, REPEAT: 8, REVERSE_COMPLETE: 16, INTERRUPT: 32, CALL: 256 }
// The callbacks of each event, by their GSAP and Motion names.
const CALLBACKS = [
  [EV.START, ['onStart', 'onAnimationStart']],
  [EV.UPDATE, ['onUpdate']],
  [EV.COMPLETE, ['onComplete', 'onAnimationComplete']],
  [EV.REPEAT, ['onRepeat']],
  [EV.REVERSE_COMPLETE, ['onReverseComplete']],
  [EV.INTERRUPT, ['onInterrupt']],
]

/** The most recent App: the one selectors and new motion values belong to. */
let current = null

/** The motions of one App (one UI thread, one engine). */
class Motions {
  constructor (app) {
    this.app = app
    this.native = app.native
    this._next = 1
    /** ids -> handles that wait for events (running, or a timeline's children) */
    this._live = new Map()
    // a handle JavaScript dropped: the UI thread lets go of it (it runs on)
    this._gone = typeof FinalizationRegistry !== 'function' ? { register () {}, unregister () {} } : new FinalizationRegistry(({ id, children, value }) => {
      if (this.app._quit) return
      if (value) this.native.motionValueDispose(id)
      else this.native.motionRelease(id)
      for (const c of children) this._live.delete(c)
    })
    current = this
  }

  close () { if (current === this) current = null }

  /** The UI thread must see the tree (elements, their attributes) first. */
  _flush () { for (const w of this.app.windows.values()) w.flush() }

  _id () { return this._next++ }

  _event (raw) {
    this._live.get(raw.target)?._event(raw.button, raw)
  }
}

/** The motions of `app`, or of the newest App. */
function motionsOf (app) {
  const m = app?.motions ?? current
  if (!m) throw new Error('blenderui: no App is running (create one before animating)')
  return m
}

/** `"<"`, `"+=1"`, `1.5` ... as the engine's position text (`""`: the end). */
function position (p) { return p == null ? '' : String(p) }

/** An element as a selector: a hidden attribute only the native tree sees. */
function elementSelector (el) {
  const w = el.ownerWindow
  if (w === null) throw new Error('blenderui: animating an element that is in no window')
  const key = `${w.id}:${el.id}`
  w._op(OP.SET_ATTR, el.id, w._s('data-bui-motion'), w._s(key))
  return `[data-bui-motion="${key}"]`
}

/** What a call animates: `{ app, selector }`, `{ values }` (motion values)
 *  or `{ object }` (a plain object's numbers). */
function targetsOf (targets) {
  if (typeof targets === 'string') return { selector: targets }
  if (isMotionValue(targets)) return { values: [targets] }
  const list = Array.isArray(targets) || (targets && typeof targets.length === 'number' && typeof targets !== 'function')
    ? Array.from(targets)
    : [targets]
  if (list.length > 0 && list.every(isMotionValue)) return { values: list }
  if (list.every((t) => typeof t === 'string' || (t && t.nodeType === 1))) {
    const el = list.find((t) => typeof t !== 'string')
    return { app: el?.ownerWindow?.app, selector: list.map((t) => typeof t === 'string' ? t : elementSelector(t)).join(', ') }
  }
  if (list.length === 1 && targets !== null && typeof targets === 'object') return { object: targets }
  throw new TypeError('blenderui: targets are selectors, elements, motion values or one plain object')
}

/** The callback for event `bit`, from `sources` (vars, a transition, options). */
function callback (sources, bit) {
  const names = CALLBACKS.find(([b]) => b === bit)?.[1] ?? []
  for (const s of sources) {
    if (!s) continue
    for (const n of names) if (typeof s[n] === 'function') return [s[n], s[`${n}Params`]]
  }
  return null
}

/**
 * A motion JavaScript holds: GSAP's tween / timeline, Motion's controls and
 * the Web Animations `Animation` are views of it.
 */
class Handle {
  constructor (m, id, sources, flavor) {
    this._m = m
    this.id = id
    this._sources = sources
    this._flavor = flavor
    this._object = null // a plain object target: [object, property names]
    this._values = null // motion value targets
    this._waiters = null
    this._done = false
  }

  _ctl (op, value) {
    this._m._live.set(this.id, this) // its events matter again
    this._done = false
    this._m.native.motionControl(this.id, op, value)
    return this
  }

  _q (what) { return this._m.native.motionQuery(this.id, what) }

  _event (bit, raw) {
    if (bit === EV.UPDATE || bit === EV.COMPLETE) {
      if (this._object) {
        const [o, names] = this._object
        const vs = raw.values ?? [raw.number]
        for (let i = 0; i < names.length; i++) o[names[i]] = vs[i]
      } else if (this._values) {
        this._values[0]._changed(raw.number)
      }
    }
    const cb = callback(this._sources, bit)
    if (cb) {
      const [fn, params] = cb
      const args = this._flavor === 'motion' && bit === EV.UPDATE
        ? [this._object ? this._object[0][this._object[1][0]] : raw.number]
        : (params ?? [])
      try { fn.apply(this, args) } catch (err) { queueMicrotask(() => { throw err }) }
    }
    if (bit === EV.COMPLETE) {
      this._done = true
      if (!this._child) this._m._live.delete(this.id)
      const w = this._waiters
      this._waiters = null
      if (w) for (const r of w) r(this._settled())
    }
  }

  /** What `finished` resolves with: this handle, unless it is a thenable
   *  (which a promise would wait on forever). */
  _settled () { return typeof this.then === 'function' ? undefined : this }

  /** Resolves when it completes (GSAP's and Motion's `then`). */
  _finished () {
    if (this._done) return Promise.resolve(this._settled())
    return new Promise((resolve) => (this._waiters ??= []).push(resolve))
  }

  then (resolve, reject) {
    // resolving with itself, a thenable, would wait forever: it isn't one meanwhile
    return this._finished().then(() => {
      this.then = undefined
      try { return resolve ? resolve(this) : this } finally { delete this.then }
    }, reject)
  }
}
Handle.prototype._child = false

/** Start `verb` ('to', 'from', 'fromTo', 'set', 'animate') of `vars` on
 *  `targets`; `extra`: fromTo's from-vars or animate's options; in
 *  `timeline` at `pos`, else a handle of class `Kind`. */
function start (verb, targets, vars, extra, { timeline = null, pos, Kind = null, flavor = 'gsap', events = 0 } = {}) {
  if (vars === null || typeof vars !== 'object') throw new TypeError('blenderui: vars must be an object')
  const t = targetsOf(targets)
  const m = timeline ? timeline._m : motionsOf(t.app)
  const id = m._id()
  const sources = [vars, vars.transition, verb === 'animate' ? extra : null]
  const h = new (Kind ?? Handle)(m, id, sources, flavor)
  // a plain object, and a value with listeners, hear every frame
  if (t.object || t.values?.some((v) => v._on !== null)) events |= EV.UPDATE
  m._flush()
  const names = m.native.motionTween(id, verb, t.selector, t.values?.map((v) => v.id), t.object, vars, extra,
    timeline?.id, timeline ? position(pos) : undefined, events | (Kind ? EV.COMPLETE : 0))
  if (names) h._object = [t.object, names]
  if (t.values) h._values = t.values
  if (Kind) {
    m._live.set(id, h)
    m._gone.register(h, { id, children: h._children ?? [], value: false })
  } else if (names || t.values || CALLBACKS.some(([bit]) => callback(sources, bit))) {
    // a timeline's child: its callbacks live as long as the timeline
    h._child = true
    m._live.set(id, h)
    timeline._children.push(id)
  }
  return Kind ? h : null
}

/** A motion value: a number (or CSS value) the engine animates, bound to
 *  elements through `style={{ x: mv }}` without re-rendering. */
class MotionValue {
  constructor (init = 0, app) {
    const m = motionsOf(app)
    this._m = m
    this.id = m._id()
    // the addon finds a value inside objects by this
    this.__buiMotionValue = this.id
    this._v = init
    this._on = null
    m.native.motionValue(this.id, init)
    m._gone.register(this, { id: this.id, children: [], value: true })
  }

  /** Its value now (asked of the UI thread). */
  get () {
    const v = this._m.native.motionValueGet(this.id, false)
    return Number.isNaN(v) ? this._v : v
  }

  /** Its speed now, units per second. */
  getVelocity () {
    const v = this._m.native.motionValueGet(this.id, true)
    return Number.isNaN(v) ? 0 : v
  }

  /** Set it now (stopping what animates it). */
  set (v) {
    this._m.native.motionValueSet(this.id, v)
    this._changed(v)
  }

  jump (v) { this.set(v) }

  /** `on('change', fn)`: fn(value) on every change JavaScript sees (its
   *  `set()`s, and each frame of motions started after this); returns the
   *  unsubscribe function. */
  on (event, fn) {
    if (event !== 'change') throw new TypeError(`blenderui: a motion value has no '${event}' event (only 'change')`)
    const set = (this._on ??= new Set())
    set.add(fn)
    return () => { set.delete(fn); if (set.size === 0) this._on = null }
  }

  _changed (v) {
    this._v = v
    if (this._on) for (const fn of [...this._on]) fn(v)
  }

  /** Free it now (else when it is garbage). */
  destroy () {
    this._m._gone.unregister(this)
    this._m.native.motionValueDispose(this.id)
  }
}

function motionValue (init, app) { return new MotionValue(init, app) }

module.exports = { EV, Motions, Handle, MotionValue, motionValue, isMotionValue, motionsOf, start, position }
