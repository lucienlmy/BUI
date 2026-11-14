// @blender-ui/native/gsap: GSAP's API on BUI's animation engine.
//
//   import { gsap } from '@blender-ui/native/gsap'   // was: from 'gsap'
//   gsap.to('#box', { x: 200, rotate: 360, duration: 1, ease: 'power2.out' })
//   const tl = gsap.timeline({ defaults: { duration: 0.5 } })
//   tl.to('#a', { x: 100 }).to('#b', { x: 100 }, '<').addLabel('mid').call(fn, null, 'mid')
//
// Every tween and timeline runs in Rust (`blenderui::motion`); these objects
// are handles. Targets: selectors ('#id', '.class', 'tag', '[attr=v]'),
// elements, motion values, or one plain object (its numbers, written back
// each frame). Every ease name GSAP, Motion, CSS and Blender have works.
//
// SPDX-License-Identifier: GPL-2.0-or-later
'use strict'

const { EV, Handle, motionsOf, start } = require('./anim')
const check = require('./validate')

/** A tween: GSAP's controls (getters without an argument, setters with one). */
class Tween extends Handle {
  play (from) { if (from != null) this.seek(from); return this._ctl('play') }
  pause (atTime) { if (atTime != null) this.seek(atTime); return this._ctl('pause') }
  resume () { return this._ctl('resume') }
  reverse (from) { if (from != null) this.seek(from); return this._ctl('reverse') }
  restart () { return this._ctl('restart') }
  /** Stop it where it is, for good. */
  kill () {
    this._ctl('kill')
    this._m._live.delete(this.id)
    return this
  }

  /** Jump to a time (seconds) or, in a timeline, a label. */
  seek (time) {
    const t = typeof time === 'string' ? this._q(time) : Number(time)
    if (Number.isNaN(t)) throw new RangeError(`gsap: no label '${time}'`)
    return this._ctl('seek', t)
  }

  progress (value) { return value === undefined ? this._q('progress') : this._ctl('progress', value) }
  totalProgress (value) {
    if (value !== undefined) return this._ctl('totalProgress', value)
    const d = this._q('totalDuration')
    return d > 0 ? this._q('totalTime') / d : 1
  }

  time (value) { return value === undefined ? this._q('time') : this.seek(value) }
  totalTime () { return this._q('totalTime') }
  timeScale (value) { return value === undefined ? this._q('timeScale') : this._ctl('timeScale', value) }
  duration (value) {
    if (value !== undefined) throw new TypeError('gsap: setting duration() isn\'t supported (give `duration` in the vars)')
    return this._q('duration')
  }

  totalDuration () { return this._q('totalDuration') }
  isActive () { return this._q('isActive') === 1 }
  paused (value) {
    if (value === undefined) return this._q('paused') === 1
    return value ? this.pause() : this.resume()
  }

  reversed (value) {
    if (value === undefined) return this._q('reversed') === 1
    return !!value !== this.reversed() ? this.reverse() : this
  }
}

/** A timeline: tweens placed in time by GSAP's position parameter (`"<"`,
 *  `">-0.2"`, `"+=1"`, `"label"`, `"label+=0.5"`, `"50%"`, a number). */
class Timeline extends Tween {
  constructor (m, id, sources, flavor) {
    super(m, id, sources, flavor)
    this._children = []
  }

  to (targets, vars, pos) { start('to', targets, vars, undefined, { timeline: this, pos }); return this }
  from (targets, vars, pos) { start('from', targets, vars, undefined, { timeline: this, pos }); return this }
  fromTo (targets, fromVars, toVars, pos) { start('fromTo', targets, toVars, fromVars, { timeline: this, pos }); return this }
  set (targets, vars, pos) { start('set', targets, vars, undefined, { timeline: this, pos }); return this }

  /** A tween or timeline (made with gsap.to / gsap.timeline), a label, a
   *  function (as `call`), or an array of them, at `pos`. */
  add (child, pos) {
    if (Array.isArray(child)) {
      for (const c of child) this.add(c, pos)
    } else if (typeof child === 'string') {
      this.addLabel(child, pos)
    } else if (typeof child === 'function') {
      this.call(child, null, pos)
    } else if (child instanceof Tween && child._m === this._m) {
      this._m.native.motionAdd(this.id, child.id, check.position(pos))
    } else {
      throw new TypeError('gsap: add() takes a tween, a timeline, a label or a function')
    }
    return this
  }

  addLabel (name, pos) {
    this._m.native.motionLabel(this.id, String(name), check.position(pos))
    return this
  }

  /** Run `fn(...params)` when the playhead passes `pos`. */
  call (fn, params, pos) {
    const id = this._m._id()
    const tl = this
    this._m._live.set(id, { _event () { try { fn.apply(tl, params ?? []) } catch (err) { queueMicrotask(() => { throw err }) } } })
    this._children.push(id)
    this._m.native.motionCall(this.id, id, check.position(pos))
    return this
  }
}

function timeline (vars = {}) {
  vars = check.vars(vars)
  const m = motionsOf()
  const id = m._id()
  const tl = new Timeline(m, id, [vars], 'gsap')
  m.native.motionTimeline(id, vars, EV.COMPLETE)
  m._live.set(id, tl)
  m._gone.register(tl, { id, children: tl._children, value: false })
  return tl
}

const gsap = {
  /** Animate `targets` from where they are to `vars`. */
  to: (targets, vars) => start('to', targets, vars, undefined, { Kind: Tween }),
  /** Animate `targets` from `vars` to where they are. */
  from: (targets, vars) => start('from', targets, vars, undefined, { Kind: Tween }),
  /** From `fromVars` to `toVars` (the timing is `toVars`'). */
  fromTo: (targets, fromVars, toVars) => start('fromTo', targets, toVars, fromVars, { Kind: Tween }),
  /** Set `vars` now. */
  set: (targets, vars) => start('set', targets, vars, undefined, { Kind: Tween }),
  timeline,
  /** `fn(...params)` after `delay` seconds. */
  delayedCall: (delay, fn, params) => timeline().call(fn, params, delay),
  ticker: {
    /** A frame later than `threshold` ms counts as `adjusted` ms (500, 33), so a
     *  stall doesn't jump animations to their end; `lagSmoothing(0)` turns it off. */
    lagSmoothing (threshold = 500, adjusted = 33) {
      motionsOf().native.motionLagSmoothing(threshold / 1000, Math.min(adjusted, threshold) / 1000)
    },
  },
  /** Plugins this module has (CustomEase) need no registering; others aren't here. */
  registerPlugin (...plugins) {
    for (const p of plugins) if (p !== CustomEase) throw new TypeError(`gsap: plugin ${p?.name ?? p} isn't supported`)
  },
}

/** GSAP's CustomEase: an SVG path ("M0,0 C0.2,0.8 0.4,1 1,1") as a named
 *  ease; returns the name, which `ease` then takes. */
const CustomEase = {
  create (name, path) {
    if (!motionsOf().native.registerEase(String(name), String(path))) throw new SyntaxError(`CustomEase: can't read the path '${path}'`)
    return String(name)
  },
}

module.exports = { gsap, default: gsap, Tween, Timeline, CustomEase }
