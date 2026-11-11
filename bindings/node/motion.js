// @blender-ui/native/motion: Motion's (Framer Motion's) functions on BUI's
// animation engine.
//
//   import { animate, motionValue } from '@blender-ui/native/motion'
//   const c = animate('#box', { x: 100, opacity: [0, 1] }, { duration: 0.8, ease: 'easeOut' })
//   animate('#ball', { y: 300 }, { type: 'spring', stiffness: 300, damping: 20 })
//   animate(0, 100, { onUpdate: (v) => { label.textContent = Math.round(v) } })
//
// Motion's defaults: springs for x / y / scale / rotate, eased tweens for the
// rest. Every motion runs in Rust (`blenderui::motion`); elements' Motion
// props (initial, animate, whileHover ...) are in `@blender-ui/native`'s DOM
// (and `motion.div` in @blender-ui/react/motion).
//
// SPDX-License-Identifier: GPL-2.0-or-later
'use strict'

const { Handle, MotionValue, motionValue, isMotionValue, start } = require('./anim')

/** Motion's playback controls. */
class AnimationPlaybackControls extends Handle {
  play () { this._ctl('play') }
  pause () { this._ctl('pause') }
  /** Stop where it is. */
  stop () { this._ctl('kill') }
  /** To its end now. */
  complete () { this._ctl('complete') }
  /** Back to its start, stopped. */
  cancel () { this._ctl('seek', 0); this._ctl('kill') }
  get time () { return this._q('time') }
  set time (t) { this._ctl('seek', t) }
  get speed () { return this._q('timeScale') }
  set speed (s) { this._ctl('timeScale', s) }
  get duration () { return this._q('duration') }
  get state () { return this._done ? 'finished' : this._q('paused') === 1 ? 'paused' : 'running' }
  get finished () { return this._finished() }
}

const STAGGER = Symbol('stagger')

/** `delay: stagger(0.05)`: each target starts `each` seconds after the last. */
function stagger (each, options) {
  if (options && Object.keys(options).length) throw new TypeError('motion: stagger options (from, startDelay, ease) aren\'t supported')
  return { [STAGGER]: Number(each) }
}

/** Motion's options, with `delay: stagger(n)` as the engine's `stagger`. */
function options (o) {
  if (o?.delay?.[STAGGER] === undefined) return o ?? {}
  return { ...o, delay: undefined, stagger: o.delay[STAGGER] }
}

/**
 * Motion's `animate`:
 *   animate(targets, keyframes, options)   elements, selectors, motion values' vars, an object
 *   animate(motionValue, to, options)
 *   animate(from, to, options)             a number; `onUpdate(latest)`
 */
function animate (target, keyframes, opts) {
  const flavor = { Kind: AnimationPlaybackControls, flavor: 'motion' }
  if (Array.isArray(target) && Array.isArray(target[0])) throw new TypeError('motion: animation sequences aren\'t supported yet')
  if (typeof target === 'number') return start('animate', { value: target }, { value: keyframes }, options(opts), flavor)
  if (isMotionValue(target)) return start('animate', target, { value: keyframes }, options(opts), flavor)
  return start('animate', target, keyframes, options(opts), flavor)
}

module.exports = { animate, stagger, motionValue, MotionValue, isMotionValue, AnimationPlaybackControls }
