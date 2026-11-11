'use strict'
// Motion for React: <motion.div> props and the motion-value hooks, on the
// virtual clock of a headless window (`app.advanceClock(ms)`).
const test = require('node:test')
const assert = require('node:assert')
const React = require('react')
const { render, act } = require('..')
const { motion, useMotionValue, useTransform } = require('../motion')
const { gsap } = require('@blender-ui/native/gsap')
const h = React.createElement

const near = (a, b, eps = 0.6) => Math.abs(a - b) <= eps
const tick = (ms = 30) => new Promise((resolve) => setTimeout(resolve, ms))
const byId = (window, id) => [...window._nodes.values()].find((n) => n.getAttribute?.('id') === id)
const width = async (window, id) => (await byId(window, id).getBoundingClientRect()).width

test('motion.div: initial -> animate, a new animate target, variants', async (t) => {
  let setWide
  function Card () {
    const [wide, set] = React.useState(false)
    setWide = set
    return h(React.Fragment, null,
      h(motion.div, {
        id: 'card',
        style: { height: 10, width: 10 },
        initial: { width: 200 },
        animate: { width: wide ? 300 : 100 },
        transition: { duration: 1, ease: 'linear' },
      }),
      h(motion.div, {
        id: 'v',
        style: { height: 10, width: 10 },
        variants: { small: { width: 20 }, big: { width: 80 } },
        initial: 'small',
        animate: wide ? 'big' : 'small',
        transition: { duration: 0.5, ease: 'linear' },
      }))
  }
  const { app, window } = render(h(Card), { headless: true, width: 400, height: 200 })
  t.after(() => app.quit())
  gsap.ticker.lagSmoothing(0)
  await tick()
  assert.ok(near(await width(window, 'card'), 200), `from initial: ${await width(window, 'card')}`)
  await app.advanceClock(500)
  assert.ok(near(await width(window, 'card'), 150), `halfway: ${await width(window, 'card')}`)
  await app.advanceClock(600)
  assert.ok(near(await width(window, 'card'), 100))
  assert.ok(near(await width(window, 'v'), 20), 'initial variant')
  act(() => setWide(true))
  await tick()
  await width(window, 'card')
  await app.advanceClock(250)
  assert.ok(near(await width(window, 'v'), 50), `variant halfway: ${await width(window, 'v')}`)
  assert.ok(near(await width(window, 'card'), 150), `retargeted from where it was: ${await width(window, 'card')}`)
})

test('motion values: useMotionValue in style, useTransform', async (t) => {
  let x, w
  function Bar () {
    x = useMotionValue(0)
    w = useTransform(x, [0, 1], [10, 110])
    return h(motion.div, { id: 'bar', style: { height: 10, width: w } })
  }
  const { app, window } = render(h(Bar), { headless: true, width: 400, height: 200 })
  t.after(() => app.quit())
  await tick()
  assert.ok(near(await width(window, 'bar'), 10), `bound to the derived value: ${await width(window, 'bar')}`)
  x.set(0.5)
  assert.ok(near(w.get(), 60), `derived: ${w.get()}`)
  assert.ok(near(await width(window, 'bar'), 60), 'the element follows it without re-rendering')
  assert.strictEqual(motion.Button, require('..').Button, 'motion.<Widget> is the Blender widget')
  assert.strictEqual(motion.span, 'span')
})
