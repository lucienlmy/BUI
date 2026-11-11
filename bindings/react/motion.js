// @blender-ui/react/motion: Motion for React's API on BUI's animation engine.
//
//   import { motion, useMotionValue, useSpring } from '@blender-ui/react/motion'
//   <motion.div initial={{ opacity: 0, y: 20 }} animate={{ opacity: 1, y: 0 }}
//               whileHover={{ scale: 1.05 }} transition={{ type: 'spring', stiffness: 400 }} />
//
// Every element takes Motion's props (the host config hands them to the
// engine), so `motion.div` is the `div` host element and `motion.Button`
// Blender's Button: no wrapper component, nothing per render but the props
// that changed. The engine (Rust, `blenderui::motion`) runs every frame.
//
// SPDX-License-Identifier: GPL-2.0-or-later
'use strict'

const React = require('react')
const { animate, stagger, motionValue, MotionValue, isMotionValue } = require('@blender-ui/native/motion')
const ui = require('./index')

/** `motion.create(Component)`: a component that renders an element passes
 *  the props on itself; the element animates. */
const create = (Component) => Component

/** `motion.div`, `motion.span` ... (the host elements), `motion.Button`,
 *  `motion.NumberSlider` ... (Blender's widgets), `motion.create(C)`. */
const motion = new Proxy(create, {
  get (target, tag) {
    if (tag === 'create') return create
    if (typeof tag !== 'string') return target[tag]
    return /^[A-Z]/.test(tag) && typeof ui[tag] === 'function' ? ui[tag] : tag
  },
})

/** Motion's `useMotionValue(init)`: the same value every render (freed when
 *  the component's last reference to it goes). */
function useMotionValue (init) {
  return React.useState(() => motionValue(init))[0]
}

/** Motion's `useSpring(source, options)`: follows `source` (a number, or a
 *  motion value's changes) with a spring. */
function useSpring (source, options) {
  const mv = useMotionValue(isMotionValue(source) ? source.get() : source)
  const opts = React.useRef(options)
  opts.current = options
  const follow = (to) => animate(mv, to, { type: 'spring', ...opts.current })
  React.useEffect(() => {
    if (isMotionValue(source)) return source.on('change', follow)
    follow(source)
  }, [source])
  return mv
}

/** Motion's `transform(input, output)`: a value in `input`'s ranges mapped
 *  linearly into `output`'s (clamped at the ends). */
function transform (input, output) {
  return (v) => {
    const n = Math.min(input.length, output.length)
    if (v <= input[0]) return output[0]
    if (v >= input[n - 1]) return output[n - 1]
    let i = 1
    while (i < n - 1 && v > input[i]) i++
    const t = (v - input[i - 1]) / (input[i] - input[i - 1])
    return output[i - 1] + (output[i] - output[i - 1]) * t
  }
}

/** Motion's `useTransform(source, input, output)` or `(source, fn)`: a value
 *  derived from `source` each time JavaScript sees it change. */
function useTransform (source, input, output) {
  const map = typeof input === 'function' ? input : transform(input, output)
  const mv = useMotionValue(map(source.get()))
  React.useEffect(() => source.on('change', (v) => mv.set(map(v))), [source])
  return mv
}

/** Motion's `useMotionValueEvent(value, 'change', fn)`. */
function useMotionValueEvent (value, event, fn) {
  const cb = React.useRef(fn)
  cb.current = fn
  React.useEffect(() => value.on(event, (v) => cb.current(v)), [value, event])
}

module.exports = {
  motion, useMotionValue, useSpring, useTransform, useMotionValueEvent, transform,
  animate, stagger, motionValue, MotionValue, isMotionValue,
}
