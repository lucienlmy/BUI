'use strict'
// gsap, Motion and Web Animations on the engine, on headless windows' virtual
// clock: `app.advanceClock(ms)` moves it and renders that frame.
const test = require('node:test')
const assert = require('node:assert')
const { App, signal } = require('..')
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

// -- review fixes ---------------------------------------------------------------------------

test('a motion value is animated by the App that made it', async (t) => {
  const { app, box } = page(t)
  await width(box('x'))
  const mv = motionValue(0)
  gsap.to(mv, { value: 100, duration: 1, ease: 'none' })
  await app.advanceClock(500)
  assert.ok(near(mv.get(), 50), `animated in its own App: ${mv.get()}`)
  // values of two Apps can't be animated together (one process runs one App, so fake the other)
  const other = motionValue(0)
  other._m = { native: {} }
  assert.throws(() => gsap.to([mv, other], { value: 1 }), /different Apps/)
})

test('every animated motion value hears its changes', async (t) => {
  const { app, box } = page(t)
  await width(box('x'))
  const one = motionValue(0)
  const two = motionValue(0)
  const heard = []
  two.on('change', (v) => heard.push(v))
  gsap.to([one, two], { value: 100, duration: 1, ease: 'none' })
  await app.advanceClock(500)
  assert.ok(heard.length >= 1 && near(heard.at(-1), 50), `the second value's listener: ${heard}`)
})

test('reverse playback reaching its start settles the handle', async (t) => {
  const { app, box } = page(t)
  const a = box('a')
  await width(a)
  const tw = gsap.to('#a', { width: 110, duration: 1, ease: 'none' })
  await width(a)
  await app.advanceClock(1100)
  await tw
  tw.reverse()
  const done = tw.then(() => 'settled')
  await app.advanceClock(1100)
  const hung = new Promise((resolve) => setTimeout(() => resolve('hung'), 500))
  assert.strictEqual(await Promise.race([done, hung]), 'settled')
})

test('an animation that never ends and has no listener is not held', async (t) => {
  const { app, box } = page(t)
  const a = box('a')
  await width(a)
  const tw = gsap.to('#a', { width: 110, duration: 1, repeat: -1 })
  assert.strictEqual(app.motions._live.has(tw.id), false, 'nothing needs the handle kept')
  const listened = gsap.to('#a', { width: 50, duration: 1, repeat: -1, onRepeat () {} })
  assert.strictEqual(app.motions._live.has(listened.id), true, 'a callback does')
})

test('a style that replaces a Motion style lets go of its values', async (t) => {
  const { app, box } = page(t)
  const el = box('a')
  const mv = motionValue(20)
  el.style = { width: mv, height: 10 }
  await app.advanceClock(16)
  assert.ok(near(await width(el), 20), 'bound')
  el.style = 'width: 33px; height: 10px' // replaced by a string
  mv.set(70)
  await app.advanceClock(16)
  assert.ok(near(await width(el), 33), `the old value no longer moves it: ${await width(el)}`)
  el.style = { width: mv, height: 10 }
  el.style = null // removed
  mv.set(90)
  await app.advanceClock(16)
  assert.ok(!near(await width(el), 90), 'removed: the value no longer moves it')
})

test('a Motion prop that cannot be read does not lose the batch', async (t) => {
  const { win } = page(t)
  const el = win.createElement('div')
  el.setAttribute('id', 'late')
  el.style = { width: 40, height: 10 }
  win.body.appendChild(el)
  el.setMotion('drag', 'diagonal') // not true, "x" or "y"
  assert.throws(() => win.flush(), /drag/)
  const r = await el.getBoundingClientRect()
  assert.ok(r && near(r.width, 40), 'the element is in the native tree anyway')
})

test('cancel() rejects the pending finished promise with an AbortError', async (t) => {
  const { app, box } = page(t)
  const el = box('w')
  await width(el)
  const a = el.animate([{ width: '10px' }, { width: '210px' }], { duration: 1000 })
  const first = a.finished
  a.cancel()
  await assert.rejects(first, { name: 'AbortError' })
  assert.notStrictEqual(a.finished, first, 'later playback has a new promise')
  assert.strictEqual(a.playState, 'idle')
  a.play()
  await app.advanceClock(1100)
  await a.finished
})

test('exit: a removed element animates out first, then goes (AnimatePresence)', async (t) => {
  const { app, win, box } = page(t)
  const el = box('gone')
  el.style = { width: 10, height: 40 }
  el.setMotion('exit', { height: 0 })
  el.setMotion('transition', { duration: 1, ease: 'linear' })
  const quick = box('quick') // no exit: goes at once
  const tail = box('tail')
  const top = async () => (await tail.getBoundingClientRect()).y
  assert.strictEqual(await top(), 50)
  win.body.removeChild(el)
  win.body.removeChild(quick)
  await app.advanceClock(500)
  const mid = await top()
  assert.ok(mid > 0 && mid < 40, `it is still there, shrinking: ${mid}`)
  await app.advanceClock(700)
  assert.strictEqual(await top(), 0, 'and gone after its exit')
})

test('el.animate: pause after the end, then play() replays; reverse() gets a new finished', async (t) => {
  const { app, box } = page(t)
  const el = box('w')
  await width(el)
  const a = el.animate([{ width: '10px' }, { width: '110px' }], { duration: 1000, fill: 'forwards' })
  await app.advanceClock(1100)
  await a.finished
  a.pause()
  a.play() // finished, then paused: it starts again from the start
  await app.advanceClock(500)
  assert.ok(near(await width(el), 60, 2), `replayed from the start: ${await width(el)}`)
  await app.advanceClock(700)
  const first = a.finished
  a.reverse()
  assert.notStrictEqual(a.finished, first, 'reverse() is a new run: a new finished promise')
})

test('a style signal that supplies a motion value binds it (and a plain value lets go)', async (t) => {
  const { app, box } = page(t)
  const el = box('a')
  const mv = motionValue(20)
  const style = signal({ width: mv, height: 10 })
  el.style = style
  await app.advanceClock(16)
  assert.ok(near(await width(el), 20), `bound through the signal: ${await width(el)}`)
  mv.set(70)
  await app.advanceClock(16)
  assert.ok(near(await width(el), 70), 'and it moves the element')
  style.value = 'width: 33px; height: 10px' // a string: the motion value lets go
  mv.set(90)
  await app.advanceClock(16)
  assert.ok(near(await width(el), 33), `${await width(el)}`)
  // a property that is a signal of a motion value
  const el2 = box('b')
  const mv2 = motionValue(15)
  el2.style = { width: signal(mv2), height: 10 }
  await app.advanceClock(16)
  assert.ok(near(await width(el2), 15), `a signal property: ${await width(el2)}`)
  mv2.set(45)
  await app.advanceClock(16)
  assert.ok(near(await width(el2), 45))
})

test('a position outside GSAP\'s grammar throws, the grammar passes', async (t) => {
  const { app, box } = page(t)
  box('a')
  const tl = gsap.timeline()
  for (const ok of [0, 1.5, -1, '+=1', '-=0.5', '<', '>', '<0.5', '>-0.2', '<+=1', 'intro', 'intro+=0.5', 'a-b-=1', '50%', '+=25%', '<50%', null, undefined]) {
    assert.doesNotThrow(() => tl.to('#a', { width: 20, duration: 0.1 }, ok), `accepts ${String(ok)}`)
  }
  for (const bad of ['<=1', 'abc def', '++1', '1x', '', '=1', '<<', '50%%']) {
    assert.throws(() => tl.to('#a', { width: 20 }, bad), RangeError, `rejects '${bad}'`)
  }
  for (const bad of [{}, [], true, () => 0]) assert.throws(() => tl.to('#a', { width: 20 }, bad), TypeError)
  for (const bad of [NaN, Infinity]) assert.throws(() => tl.addLabel('x', bad), RangeError)
  assert.throws(() => tl.add(tl, '<=1'), RangeError)
  await app.advanceClock(16)
})

test('times and repeats at the boundary: NaN and Infinity throw, negative is 0, huge repeats clamp', async (t) => {
  const { app, box } = page(t)
  const a = box('a')
  await width(a)
  for (const bad of [NaN, Infinity, -Infinity]) {
    assert.throws(() => gsap.to('#a', { width: 20, duration: bad }), RangeError, `duration ${bad}`)
    assert.throws(() => gsap.to('#a', { width: 20, delay: bad }), RangeError, `delay ${bad}`)
    assert.throws(() => gsap.to('#a', { width: 20, repeatDelay: bad }), RangeError, `repeatDelay ${bad}`)
    assert.throws(() => animate('#a', { width: 20 }, { duration: bad }), RangeError, `motion duration ${bad}`)
    assert.throws(() => gsap.timeline({ defaults: { duration: bad } }), RangeError, `defaults ${bad}`)
  }
  assert.throws(() => gsap.to('#a', { width: 20, repeat: NaN }), RangeError)
  assert.throws(() => gsap.to('#a', { width: 20, duration: '1' }), TypeError)
  assert.throws(() => a.animate([{ width: '10px' }, { width: '20px' }], { duration: NaN }), RangeError)
  assert.throws(() => a.animate([{ width: '10px' }, { width: '20px' }], { duration: 100, iterations: NaN }), RangeError)
  const tw = gsap.to('#a', { width: 20, duration: 1 })
  for (const bad of [NaN, Infinity]) {
    assert.throws(() => tw.seek(bad), RangeError, `seek ${bad}`)
    assert.throws(() => tw.timeScale(bad), RangeError)
    assert.throws(() => tw.progress(bad), RangeError)
  }
  tw.kill()
  // negative durations and delays are 0: the tween ends at once, where it is going
  gsap.to('#a', { width: 60, duration: -2, delay: -1 })
  await app.advanceClock(50)
  assert.ok(near(await width(a), 60), `negative duration is 0: ${await width(a)}`)
  // a repeat beyond the maximum runs, clamped (it does not throw or wrap)
  const big = gsap.to('#a', { width: 80, duration: 0.1, ease: 'none', repeat: 1e12 })
  const forever = gsap.to('#a', { height: 80, duration: 0.1, repeat: Infinity })
  await width(a) // the next layout finds the elements
  await app.advanceClock(250)
  assert.ok(near(big.totalDuration(), 100000.1, 1), `clamped to 1,000,000 repeats: ${big.totalDuration()}`)
  assert.strictEqual(big.isActive(), true)
  assert.ok((await a.getBoundingClientRect()).height > 10, 'Infinity repeats: forever, and it runs')
  big.kill()
  forever.kill()
})
