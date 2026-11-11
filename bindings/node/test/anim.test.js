'use strict'
// gsap, Motion and Web Animations on the engine, on headless windows' virtual
// clock: `app.advanceClock(ms)` moves it and renders that frame.
const test = require('node:test')
const assert = require('node:assert')
const { App } = require('..')
const { gsap, CustomEase } = require('../gsap')
const { animate, motionValue } = require('../motion')

const near = (a, b, eps = 0.6) => Math.abs(a - b) <= eps

function page (t) {
  const app = new App({ headless: true })
  t.after(() => app.quit())
  // the steps here are long: lag smoothing would take them for stalls
  gsap.ticker.lagSmoothing(0)
  const win = app.window({ width: 400, height: 300 })
  win.body.style = 'margin: 0'
  const box = (id, cls) => {
    const el = win.createElement('div')
    if (id) el.setAttribute('id', id)
    if (cls) el.className = cls
    el.style = { width: 10, height: 10 }
    win.body.appendChild(el)
    return el
  }
  return { app, win, box }
}

const width = async (el) => (await el.getBoundingClientRect()).width

test('gsap.to on a selector, stagger, controls and callbacks', async (t) => {
  const { app, box } = page(t)
  const a = box('a')
  const b1 = box(null, 'item')
  const b2 = box(null, 'item')
  await width(a)
  const seen = []
  const tw = gsap.to('#a', { width: 110, duration: 1, ease: 'none', onStart: () => seen.push('start'), onComplete: () => seen.push('complete') })
  gsap.to('.item', { width: 110, duration: 1, ease: 'none', stagger: 0.5 })
  await width(a) // the next layout finds the elements
  await app.advanceClock(500)
  assert.ok(near(await width(a), 60), `halfway: ${await width(a)}`)
  assert.ok(near(await width(b1), 60) && near(await width(b2), 10), 'staggered by 0.5 s')
  assert.ok(near(tw.progress(), 0.5, 0.01), `progress ${tw.progress()}`)
  assert.strictEqual(tw.isActive(), true)
  assert.ok(near(tw.duration(), 1, 1e-6))
  tw.pause()
  await app.advanceClock(300)
  assert.ok(near(await width(a), 60), 'paused')
  tw.resume()
  await app.advanceClock(600)
  assert.ok(near(await width(a), 110), 'done')
  assert.deepStrictEqual(seen, ['start', 'complete'])
  assert.strictEqual(tw.isActive(), false)
  await tw // thenable: resolves once complete
  tw.reverse()
  await app.advanceClock(250)
  assert.ok(near(await width(a), 85), `reversed a quarter: ${await width(a)}`)
})

test('gsap timelines: positions, labels, defaults, call', async (t) => {
  const { app, box } = page(t)
  const a = box('ta')
  const b = box('tb')
  await width(a)
  let called = 0
  const tl = gsap.timeline({ defaults: { duration: 1, ease: 'none' } })
  tl.to('#ta', { width: 110 })
    .addLabel('half', '<0.5')
    .to('#tb', { width: 110 }, 'half')
    .call(() => called++, null, '>')
  await width(a)
  assert.ok(near(tl.duration(), 1.5, 1e-6), `duration ${tl.duration()}`)
  await app.advanceClock(1000)
  assert.ok(near(await width(a), 110) && near(await width(b), 60), `a ${await width(a)} b ${await width(b)}`)
  await app.advanceClock(600)
  assert.ok(near(await width(b), 110))
  assert.strictEqual(called, 1, 'the call ran once')
  tl.seek(0.25)
  assert.ok(near(await width(a), 35) && near(await width(b), 10), 'seek: every child where it belongs')
  tl.seek('half')
  assert.ok(near(await width(a), 60), 'seek to a label')
})

test('gsap on a plain object and on a motion value; CustomEase', async (t) => {
  const { app, box } = page(t)
  await width(box('x'))
  const obj = { value: 0, other: 5 }
  const updates = []
  gsap.to(obj, { value: 100, other: 15, duration: 1, ease: 'none', onUpdate: () => updates.push(obj.value) })
  await app.advanceClock(500)
  assert.ok(near(obj.value, 50) && near(obj.other, 10), `object written each frame: ${obj.value} ${obj.other}`)
  assert.ok(updates.length >= 1)
  const mv = motionValue(30)
  gsap.to(mv, { value: 130, duration: 1, ease: 'none' })
  await app.advanceClock(500)
  assert.ok(near(mv.get(), 80), `motion value ${mv.get()}`)
  assert.strictEqual(CustomEase.create('hop', 'M0,0 C0.2,0.8 0.4,1 1,1'), 'hop')
  assert.throws(() => gsap.to('#x', { width: 10, stagger: { from: 'center' } }), /stagger/)
  assert.throws(() => gsap.to('#x', { width: 10, type: 'bouncy' }), /unknown type/)
})

test('Motion: animate(), motion props on elements, motion values in style', async (t) => {
  const { app, win, box } = page(t)
  const el = box('m')
  el.setMotion('initial', { width: 200 })
  el.setMotion('animate', { width: 100 })
  el.setMotion('transition', { duration: 1, ease: 'linear' })
  assert.ok(near(await width(el), 200), `starts at initial: ${await width(el)}`)
  await app.advanceClock(500)
  assert.ok(near(await width(el), 150), `initial -> animate halfway: ${await width(el)}`)
  await app.advanceClock(600)
  assert.ok(near(await width(el), 100))
  // a new target animates from where it is
  el.setMotion('animate', { width: 300 })
  await width(el)
  await app.advanceClock(500)
  assert.ok(near(await width(el), 200), `retarget halfway: ${await width(el)}`)

  // style={{ width: mv }}
  const mv = motionValue(40)
  const bound = win.createElement('div')
  bound.style = { height: 10, width: mv }
  win.body.appendChild(bound)
  assert.ok(near(await width(bound), 40), `bound: ${await width(bound)}`)
  mv.set(70)
  assert.ok(near(await width(bound), 70), 'follows set()')

  const seen = []
  const c = animate(0, 100, { duration: 1, ease: 'linear', onUpdate: (v) => seen.push(v) })
  await app.advanceClock(500)
  assert.ok(near(seen.at(-1), 50), `onUpdate(latest): ${seen.at(-1)}`)
  assert.ok(near(c.time, 0.5, 1e-3))
  c.complete()
  await c.finished
  const d = animate('#m', { width: 50 }, { duration: 1, ease: 'linear' })
  await width(el)
  await app.advanceClock(1000)
  assert.ok(near(await width(el), 50), `animate on a selector: ${await width(el)}`)
  assert.strictEqual(d.state, 'finished')
})

test('el.animate (Web Animations)', async (t) => {
  const { app, box } = page(t)
  const el = box('w')
  await width(el)
  const a = el.animate([{ width: '10px' }, { width: '210px' }], { duration: 1000, easing: 'linear' })
  let finished = 0
  a.onfinish = () => finished++
  await width(el)
  await app.advanceClock(250)
  assert.ok(near(await width(el), 60), `a quarter: ${await width(el)}`)
  assert.ok(near(a.currentTime, 250, 1))
  assert.strictEqual(a.playState, 'running')
  a.pause()
  assert.strictEqual(a.playState, 'paused')
  a.play()
  await app.advanceClock(800)
  await a.finished
  assert.strictEqual(finished, 1)
  assert.strictEqual(a.playState, 'finished')
  assert.ok(near(await width(el), 210), 'holds its end')
})
