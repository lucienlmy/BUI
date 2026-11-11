// `el.animate(keyframes, options)`: the Web Animations API, run by BUI's
// animation engine. The keyframes and timing become one tween's vars here
// (once); the engine runs it.
//
// SPDX-License-Identifier: GPL-2.0-or-later
'use strict'

const { Handle, EV, start } = require('./anim')

const NOT_PROPS = new Set(['offset', 'easing', 'composite'])

/** Keyframes (an array of frames, or an object of arrays) and timing as vars. */
function vars (keyframes, options) {
  const o = typeof options === 'number' ? { duration: options } : (options ?? {})
  if (o.endDelay) throw new TypeError('animate(): endDelay isn\'t supported')
  if (o.composite && o.composite !== 'replace') throw new TypeError('animate(): only composite \'replace\' is supported')
  const v = {}
  if (Array.isArray(keyframes)) {
    const n = keyframes.length
    for (const k of keyframes) for (const p in k) if (!NOT_PROPS.has(p) && !(p in v)) v[p] = keyframes.map((f) => f[p] ?? null)
    if (keyframes.some((f) => f.offset != null)) v.times = keyframes.map((f, i) => f.offset ?? (n === 1 ? 1 : i / (n - 1)))
    if (keyframes.some((f) => f.easing != null)) v.ease = keyframes.slice(0, -1).map((f) => f.easing ?? o.easing ?? 'linear')
  } else if (keyframes) {
    for (const p in keyframes) if (!NOT_PROPS.has(p)) v[p] = keyframes[p]
    if (keyframes.offset != null) v.times = [].concat(keyframes.offset)
    if (Array.isArray(keyframes.easing)) v.ease = keyframes.easing
  }
  v.ease ??= o.easing ?? 'linear'
  v.duration = (typeof o.duration === 'number' ? o.duration : 0) / 1000
  if (o.delay) v.delay = o.delay / 1000
  const it = o.iterations ?? 1
  if (it !== 1) v.repeat = it === Infinity ? -1 : Math.max(0, Math.ceil(it) - 1)
  if (o.direction === 'alternate' || o.direction === 'alternate-reverse') v.yoyo = true
  if (o.direction === 'reverse' || o.direction === 'alternate-reverse') v.reversed = true
  if (o.playbackRate != null) v.timeScale = o.playbackRate
  return v
}

/** An `Animation` (Web Animations): its state, controls and `finished`.
 *  The values stay where it ends (as `fill: 'forwards'`). */
class Animation extends Handle {
  constructor (m, id, sources, flavor) {
    super(m, id, sources, flavor)
    this.onfinish = null
    this.oncancel = null
    this._state = 'running'
    this.finished = this._finished()
  }

  get playState () { return this._done ? 'finished' : this._state }
  get pending () { return false }
  get currentTime () { return this._q('time') * 1000 }
  set currentTime (ms) { this._ctl('seek', ms / 1000) }
  get playbackRate () { return this._q('timeScale') }
  set playbackRate (r) { this._ctl('timeScale', r) }
  updatePlaybackRate (r) { this.playbackRate = r }

  play () {
    const done = this._done
    this._state = 'running'
    this._ctl(done ? 'restart' : 'play')
    if (done) this.finished = this._finished()
  }

  pause () { this._state = 'paused'; this._ctl('pause') }
  reverse () { this._state = 'running'; this._ctl('reverse') }
  finish () { this._ctl('complete') }
  cancel () {
    this._ctl('seek', 0)
    this._ctl('kill')
    this._state = 'idle'
    this.oncancel?.({ type: 'cancel', target: this })
  }

  _event (bit, raw) {
    super._event(bit, raw)
    if (bit === EV.COMPLETE) this.onfinish?.({ type: 'finish', target: this })
  }
}
// not a thenable: `finished` is the promise
Animation.prototype.then = undefined

function animate (el, keyframes, options) {
  return start('to', el, vars(keyframes, options), undefined, { Kind: Animation })
}

module.exports = { animate, Animation }
