// The scenarios of @blender-ui/react's test/react.test.js, in Svelte 5, and
// what Svelte adds: bindings, keyed lists, snippets, context, lifecycle.
import test from 'node:test'
import assert from 'node:assert'
import { render, act, signal, Button } from '../index.js'
import { HANDLERS } from '../events.js'
import * as internal from '../internal.js'
import * as svelteInternal from 'svelte/internal/client'
import { compileSvelte } from '../compile.js'
import native from '@blender-ui/native'

const { REDIRECTS, redirected } = internal
const { document } = native
import Counter from './fixtures/Counter.svelte'
import Panel from './fixtures/Panel.svelte'
import Modes from './fixtures/Modes.svelte'
import Show from './fixtures/Show.svelte'
import Grow from './fixtures/Grow.svelte'
import Form from './fixtures/Form.svelte'
import Blocks from './fixtures/Blocks.svelte'
import Bound from './fixtures/Bound.svelte'

const tick = (ms = 30) => new Promise(r => setTimeout(r, ms))
const at = (s, x, y) => { const i = (y * s.width + x) * 4; return [...s.data.subarray(i, i + 3)] }
const finder = (window) => (id) => [...window._nodes.values()].find(n => n.getAttribute?.('id') === id)
const elements = (el) => el.childNodes.filter(n => n.nodeType === 1)

test('Svelte: render, state, events, conditional children (headless)', async (t) => {
  let renders = 0
  const { app, window } = render(Counter, { props: { onrender: () => renders++ }, headless: true, width: 200, height: 120 })
  t.after(() => app.quit())
  window.body.style = 'margin: 0; background: #000'
  await tick()
  let shot = await window.screenshot()
  assert.deepStrictEqual(at(shot, 70, 20), [0, 0, 255])
  const btn = finder(window)('btn')
  assert.strictEqual(btn.textContent, 'count 0')
  window.click(10, 10)
  await tick()
  assert.strictEqual(btn.textContent, 'count 1', 'state update changed the text')
  shot = await window.screenshot()
  assert.deepStrictEqual(at(shot, 70, 20), [0, 255, 0], 'style updated')
  assert.deepStrictEqual(at(shot, 10, 42), [255, 0, 0], 'conditional child mounted (8px gap below the button)')
  assert.strictEqual(renders, 1, 'the component ran once: runes updated the nodes')
})

test('Svelte: BUI signals are stores; writes update their nodes without re-running the component', async (t) => {
  const external = signal(0)
  let sig
  let renders = 0
  const { app, window } = render(Panel, {
    props: { external, expose: (n) => { sig = n }, onrender: () => renders++ },
    headless: true, width: 200, height: 120,
  })
  t.after(() => app.quit())
  window.body.style = 'margin: 0; background: #000'
  await tick()
  const find = finder(window)
  assert.strictEqual(find('box').textContent, 'count 0')
  assert.deepStrictEqual(at(await window.screenshot(), 70, 20), [0, 0, 255])
  window.click(10, 10)
  await tick()
  assert.strictEqual(find('box').textContent, 'count 1')
  assert.deepStrictEqual(at(await window.screenshot(), 70, 20), [0, 255, 0])
  sig.value = 2
  await tick()
  assert.strictEqual(find('box').textContent, 'count 2')
  external.value = 5
  await tick()
  assert.strictEqual(find('ext').textContent, 'ext 5')
  assert.strictEqual(renders, 1, 'Panel ran once: signal writes went straight to the nodes')
})

test('Svelte: a BUI signal passed as a value binds its node; writes skip Svelte', async (t) => {
  const style = signal('width: 80px; height: 30px; background-color: #0000ff')
  const label = signal('one')
  const cls = signal('a')
  let renders = 0
  const { app, window } = render(Bound, { props: { style, label, cls, onrender: () => renders++ }, headless: true, width: 200, height: 120 })
  t.after(() => app.quit())
  window.body.style = 'margin: 0; background: #000'
  await tick()
  const el = finder(window)('bound')
  assert.strictEqual(el.textContent, 'one')
  assert.strictEqual(el.getAttribute('title'), 'one')
  assert.match(el.className, /^a svelte-/, 'the scoping hash follows the signal')
  assert.deepStrictEqual(at(await window.screenshot(), 70, 20), [0, 0, 255])
  label.value = 'two'
  style.value = 'width: 80px; height: 30px; background-color: #00ff00'
  cls.value = 'b'
  // synchronously: the bindings wrote the nodes, no Svelte flush in between
  assert.strictEqual(el.textContent, 'two')
  assert.strictEqual(el.getAttribute('title'), 'two')
  assert.match(el.className, /^b svelte-/)
  assert.deepStrictEqual(at(await window.screenshot(), 70, 20), [0, 255, 0])
  assert.strictEqual(renders, 1)
})

test('Svelte: text and elements take turns; keyed lists reorder; dropped nodes are freed', async (t) => {
  let set
  const { app, window } = render(Modes, { props: { expose: (s) => { set = s } }, headless: true, width: 100, height: 100 })
  t.after(() => app.quit())
  const find = finder(window)
  await tick()
  assert.strictEqual(find('box').textContent, 'hello')
  assert.ok(find('in')._listeners.get('change')?.size, 'onchange listens natively')
  act(() => set.mode('el'))
  await tick()
  assert.strictEqual(elements(find('box'))[0].localName, 'span')
  assert.strictEqual(find('box').textContent, 'inner')
  assert.strictEqual(find('in').getAttribute('type'), 'checkbox')
  act(() => set.mode('none'))
  await tick()
  assert.deepStrictEqual(elements(find('box')).map(e => e.localName), ['input'], 'only the input left')
  const li = () => elements(find('list')).map(e => e.textContent)
  assert.deepStrictEqual(li(), ['1', '2', '3'])
  const two = elements(find('list'))[1]
  act(() => set.items([3, 2, 1]))
  await tick()
  assert.deepStrictEqual(li(), ['3', '2', '1'])
  assert.strictEqual(elements(find('list'))[1], two, 'keyed: the node moved, not re-created')
  const before = window._nodes.size
  act(() => { set.mode('el'); set.items([3, 2, 1, 4, 5]) })
  await tick()
  assert.ok(window._nodes.size > before)
  act(() => { set.mode('none'); set.items([3, 2, 1]) })
  await tick()
  assert.strictEqual(window._nodes.size, before, 'removed elements and their text are freed')
})

test('Svelte: hidden keeps a subtree; transitions animate (el.animate on the engine)', async (t) => {
  let setShow
  const { app, window } = render(Show, { props: { expose: (s) => { setShow = s } }, headless: true, width: 100, height: 100 })
  t.after(() => app.quit())
  const find = finder(window)
  await tick()
  assert.strictEqual(find('kid').hasAttribute('hidden'), false)
  assert.ok(find('vt'))
  setShow(false)
  await tick(100)
  assert.strictEqual(find('kid').hasAttribute('hidden'), true, 'hidden, and kept')
  assert.ok(find('vt'), 'the outro is running (the headless clock stands still)')
  // Svelte's delay animation, then its 400 ms fade
  for (let ms = 0; ms < 1000 && find('vt'); ms += 100) {
    await app.advanceClock(100)
    await tick()
  }
  assert.strictEqual(find('vt'), undefined, 'the outro finished and freed its node')
  setShow(true)
  await tick()
  for (let i = 0; i < 6; i++) {
    await app.advanceClock(100)
    await tick()
  }
  assert.strictEqual(find('kid').hasAttribute('hidden'), false)
  assert.strictEqual(find('vt').textContent, 'shown')
})

test('Svelte: a transition in and out runs on the engine, and toggling back mid-outro keeps one node', async (t) => {
  let setShow
  const { app, window } = render(Grow, { props: { expose: (s) => { setShow = s } }, headless: true, width: 300, height: 100 })
  t.after(() => app.quit())
  const find = finder(window)
  const count = () => [...window._nodes.values()].filter((n) => n.getAttribute?.('id') === 'g').length
  const wide = async () => (await find('g').getBoundingClientRect()).width
  const step = async (ms) => { await app.advanceClock(ms); await tick() }
  await tick()
  assert.strictEqual(count(), 0)
  setShow(true)
  await tick()
  await wide() // the next layout finds the element
  await step(100)
  await step(200)
  const mid = await wide()
  assert.ok(mid > 5 && mid < 95, `the intro is under way, its width moved by the engine: ${mid}`)
  await step(600)
  setShow(false)
  await tick()
  await step(100)
  assert.strictEqual(count(), 1, 'the outro is running: the node is still there')
  setShow(true) // back before the outro ended
  await tick()
  await step(600)
  assert.strictEqual(count(), 1, 'one node, not a leftover outro plus a new one')
  setShow(false)
  await tick()
  for (let ms = 0; ms < 2000 && count() > 0; ms += 100) await step(100)
  assert.strictEqual(count(), 0, 'the outro finished and freed the node')
})

test('Svelte: bindings, Blender widgets, snippets, context and lifecycle', async (t) => {
  const log = []
  const { app, window } = render(Form, { props: { log }, headless: true, width: 300, height: 300 })
  t.after(() => app.quit())
  const find = finder(window)
  await tick()
  assert.deepStrictEqual(log, ['mount'])
  find('name')._dispatch({ kind: 'input', value: 'Ada' })
  find('agree')._dispatch({ kind: 'change', checked: true })
  await tick()
  assert.strictEqual(find('hello').textContent, 'Hello, Ada! (agreed)', 'bind:value and bind:checked')
  const apply = find('apply')
  assert.strictEqual(apply.localName, 'button')
  assert.ok(apply.hasAttribute('bui-blender'), 'a Blender widget')
  assert.ok(apply._listeners.get('click')?.size, 'its onclick listens natively')
  apply._dispatch({ kind: 'click' })
  await tick()
  assert.strictEqual(apply.textContent, 'Apply 1')
  const size = find('size')
  assert.deepStrictEqual([size.localName, size.getAttribute('type'), size.getAttribute('max')], ['input', 'range', '10'])
  assert.strictEqual(find('card').getAttribute('data-theme'), 'blender', 'context')
  assert.strictEqual(find('card').textContent.trim(), 'Titlebody text', 'snippets')
  assert.strictEqual(typeof Button, 'function')
  window.close()
  assert.deepStrictEqual(log, ['mount', 'destroy'], 'closing the window unmounts')
})

test('Svelte: {#await}, {#key}, class objects, bind:group and a scoped <style>', async (t) => {
  let resolve
  let set
  const promise = new Promise((r) => { resolve = r })
  const { app, window } = render(Blocks, { props: { promise, expose: (s) => { set = s } }, headless: true, width: 100, height: 100 })
  t.after(() => app.quit())
  window.body.style = 'margin: 0; background: #000'
  const find = finder(window)
  await tick()
  assert.strictEqual(find('await').textContent, 'loading')
  resolve(42)
  await tick()
  assert.strictEqual(find('await').textContent, 'got 42')
  const first = find('key')
  act(() => set.bump())
  await tick()
  assert.notStrictEqual(find('key'), first, '{#key} re-created its content')
  assert.strictEqual(find('key').textContent, 'v2')
  assert.strictEqual(find('cls').className, 'base')
  act(() => set.toggle())
  assert.deepStrictEqual(find('cls').className.split(' ').sort(), ['base', 'on'])
  find('fb')._dispatch({ kind: 'change', checked: true })
  await tick()
  assert.strictEqual(find('flavor').textContent, 'b', 'bind:group')
  // (no id attribute: BUI measures elements without one)
  const red = [...window._nodes.values()].find((n) => n.getAttribute?.('class')?.startsWith('red'))
  assert.match(red.className, /svelte-/, 'scoped class')
  const box = await red.getBoundingClientRect()
  assert.deepStrictEqual([box.width, box.height], [40, 20], 'the component stylesheet applies')
  const styles = window.body.childNodes.filter((n) => n.localName === 'style' && n.textContent.includes('.red'))
  assert.strictEqual(styles.length, 1, 'one stylesheet per component, per window')
})

test('Svelte internals: the event-delegation symbol is found (fails when a Svelte release changes it)', () => {
  assert.strictEqual(typeof HANDLERS, 'symbol')
})

test('Svelte internals: every redirected update function matches Svelte\'s (fails when a release changes one)', () => {
  // Svelte still has each one, with the shape internal.js was written for
  // (otherwise internal.js falls back to Svelte's own, and this fails loudly)
  assert.deepStrictEqual(redirected, Object.keys(REDIRECTS), 'a redirected function changed shape in this Svelte release')
  // compiled templates still call them
  const code = compileSvelte('<script>let { a, b } = $props()</script><p class={a} style={b} title={a}>{a}</p>', 'Probe.svelte')
  assert.match(code, /from '@blender-ui\/svelte\/internal'/)
  for (const name of Object.keys(REDIRECTS)) assert.match(code, new RegExp(`\\$\\.${name}\\(`), `compiled code no longer calls ${name}`)
  // and each does what Svelte's does, on the same inputs
  const make = () => {
    const el = document.createElement('div')
    el.append(document.createTextNode(' '))
    return el
  }
  const state = (el) => [el.getAttributeNames().sort().map((n) => [n, el.getAttribute(n)]), el.firstChild.data]
  const cases = [
    ['set_text', (el) => [el.firstChild, 42]], ['set_text', (el) => [el.firstChild, null]],
    ['set_attribute', (el) => [el, 'title', 'hi']], ['set_attribute', (el) => [el, 'title', null]],
    ['set_style', (el) => [el, 'color: red']], ['set_style', (el) => [el, '']], ['set_style', (el) => [el, null]],
    ['set_class', (el) => [el, 1, 'a b', 'svelte-x']], ['set_class', (el) => [el, 1, null, undefined]],
  ]
  for (const v of ['a', { b: true, c: false }, ['d', { e: 1 }], null]) assert.deepStrictEqual(internal.clsx(v), svelteInternal.clsx(v), 'clsx')
  for (const [name, args] of cases) {
    const ours = make(), theirs = make()
    internal[name](...args(ours))
    svelteInternal[name](...args(theirs))
    assert.deepStrictEqual(state(ours), state(theirs), `${name}${JSON.stringify(args(make()).slice(1))}`)
  }
})
