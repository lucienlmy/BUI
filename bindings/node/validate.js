// The boundary checks every animation entry (gsap, motion, el.animate, the
// Motion props) shares, one contract with the C engine (docs/animation-api.md,
// "Numbers and positions"): a time or a count that is NaN or infinite throws
// a RangeError, a negative duration, delay or repeat delay is 0, a repeat is
// at most MAX_REPEAT (-1 and Infinity: forever).
//
// SPDX-License-Identifier: GPL-2.0-or-later
'use strict'

const MAX_REPEAT = 1000000

/** `v` as a finite number, or a RangeError (a TypeError for a non-number) naming `what`. */
function finite (what, v) {
  if (typeof v !== 'number') throw new TypeError(`blenderui: ${what} must be a number, not ${typeof v}`)
  if (!Number.isFinite(v)) throw new RangeError(`blenderui: ${what} must be a finite number, not ${v}`)
  return v
}

/** A repeat count: -1 (and Infinity) forever, else 0 .. MAX_REPEAT whole repeats. */
function repeatCount (v) {
  if (typeof v !== 'number') throw new TypeError(`blenderui: repeat must be a number, not ${typeof v}`)
  if (Number.isNaN(v)) throw new RangeError('blenderui: repeat must not be NaN')
  if (v === Infinity || v === -1) return -1
  return Math.min(Math.max(Math.trunc(v), 0), MAX_REPEAT)
}

// the vars that are times (seconds): a negative one is 0
const TIMES = ['duration', 'delay', 'repeatDelay', 'stagger']

/** `vars` with its times and repeat checked (also in `transition` and `defaults`): the same
 *  object when nothing changes, else a copy. */
function vars (v) {
  if (v === null || typeof v !== 'object') return v
  let out = v
  const set = (k, x) => { if (out === v) out = { ...v }; out[k] = x }
  for (const k of TIMES) {
    if (v[k] == null || (k === 'stagger' && typeof v[k] === 'object')) continue // stagger: a time, or GSAP's { each, from }
    if (finite(k, v[k]) < 0) set(k, 0)
  }
  if (v.repeat != null) {
    const r = repeatCount(v.repeat)
    if (r !== v.repeat) set('repeat', r)
  }
  if (v.timeScale != null) finite('timeScale', v.timeScale)
  for (const k of ['transition', 'defaults']) {
    if (v[k] != null && typeof v[k] === 'object') {
      const x = vars(v[k])
      if (x !== v[k]) set(k, x)
    }
  }
  return out
}

const NUM = '[+-]?(?:\\d+\\.?\\d*|\\.\\d+)(?:e[+-]?\\d+)?'
const OFFSET = `(?:[+-]=)?${NUM}%?` // "1", "-0.5", "+=1", "-=0.2", "50%", "+=25%"
const POSITION = [
  new RegExp(`^${OFFSET}$`, 'i'), // a time, an offset from the end, a percent of the duration
  new RegExp(`^[<>](?:${OFFSET})?$`, 'i'), // "<", ">", "<0.5", ">-0.2", "<+=1"
  new RegExp(`^[A-Za-z_][\\w.-]*?(?:[+-]=${NUM}%?)?$`, 'i'), // "intro", "intro+=0.5", "intro-=25%"
]

/** GSAP's position parameter as the engine's text (`""`: the end): a finite number, or a
 *  string of GSAP's grammar; anything else throws. */
function position (p) {
  if (p == null) return ''
  if (typeof p === 'number') return String(finite('position', p))
  if (typeof p !== 'string') throw new TypeError(`blenderui: a position is a number or a string, not ${typeof p}`)
  const s = p.trim()
  if (!POSITION.some((re) => re.test(s))) throw new RangeError(`blenderui: '${p}' is not a position (a time, '+=n', '<', '>', '<n', 'label', 'label+=n', 'n%')`)
  return s
}

module.exports = { MAX_REPEAT, finite, repeatCount, vars, position }
