// The scenarios of @blender-ui/react's and @blender-ui/svelte's tests, in Vue 3, and what Vue adds:
// v-model, v-for with keys, slots, provide/inject, scoped styles, lifecycle, plugins, TypeScript.
import test from 'node:test'
import assert from 'node:assert'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { spawnSync } from 'node:child_process'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { createRequire } from 'node:module'
import { render, createApp, createRoot, h, ref, onMounted, onUnmounted, nextTick, withModifiers, withKeys, Teleport, signal, Button, TextField } from '../index.js'
import native from '@blender-ui/native'
import * as ours from '../index.js'
import { compileVue } from '../compile.js'
import Counter from './fixtures/Counter.vue'
import Example from '../examples/Counter.vue'
import Form from './fixtures/Form.vue'
import Raw from './fixtures/Raw.vue'
import Loose from './fixtures/Loose.vue'
import List from './fixtures/List.vue'
import Cards from './fixtures/Cards.vue'
import Show from './fixtures/Show.vue'
import Scoped from './fixtures/Scoped.vue'
import Plain from './fixtures/Plain.vue'
import Typed from './fixtures/Typed.vue'

const tick = (ms = 30) => new Promise((r) => setTimeout(r, ms))
const at = (s, x, y) => { const i = (y * s.width + x) * 4; return [...s.data.subarray(i, i + 3)] }
const finder = (window) => (id) => [...window._nodes.values()].find((n) => n.getAttribute?.('id') === id)
const elements = (el) => el.childNodes.filter((n) => n.nodeType === 1)
const open = (t, component, options = {}) => {
  const r = render(component, { headless: true, width: 240, height: 160, ...options })
  if (!options.app) t.after(() => r.app.quit()) // one App at a time: a test that opens more shares the first's
  r.window.body.style = 'margin: 0; background: #000'
  return r
}
/** One rendered frame: the native side hit-tests against the last layout. */
const settle = async (window) => { await tick(); await window.screenshot() }

test('Vue: render, state, events, styles, conditional children (headless)', async (t) => {
  const { window } = open(t, Counter)
  await settle(window)
  const find = finder(window)
  const box = find('box')
  assert.strictEqual(box.textContent.trim(), 'count 0')
  let shot = await window.screenshot()
  assert.deepStrictEqual(at(shot, 70, 20), [0, 0, 255])
  window.click(30, 20)
  await tick()
  assert.strictEqual(box.textContent.trim(), 'count 1', 'a ref changed the text')
  shot = await window.screenshot()
  assert.deepStrictEqual(at(shot, 70, 20), [0, 255, 0], 'the :style object updated')
  assert.ok(find('extra'), 'v-if mounted its element')
  const extra = find('extra')
  window.click(30, 20)
  await tick()
  assert.strictEqual(box.textContent.trim(), 'count 2')
  assert.strictEqual(find('extra'), extra, 'the element stayed (same node)')
})

test('Vue: Blender widgets are components', async (t) => {
  const { window } = open(t, Counter)
  await settle(window)
  const find = finder(window)
  const b = find('b')
  assert.ok(b, 'the Button rendered')
  assert.strictEqual(b.textContent, 'Add ten', 'its slot is the label')
  assert.strictEqual(b.getAttribute('bui-blender'), '', 'a Blender widget, not a browser button')
  b._dispatch({ kind: 'click' })
  await tick()
  assert.strictEqual(find('box').textContent.trim(), 'count 10', '@click reached the widget once (not twice)')
})

test('Vue: v-model on inputs, checkboxes, radios, selects and widgets', async (t) => {
  const { window } = open(t, Form)
  await settle(window)
  const find = finder(window)
  const out = () => find('out').textContent
  assert.strictEqual(out(), '|false|red|b|m|2:number||1:number||3:number', 'initial values')
  assert.strictEqual(find('rb').checked, true, 'the radio whose value matches is checked')
  assert.strictEqual(find('red').checked, true, 'a checkbox in the array is checked')
  assert.strictEqual(find('blue').checked, false)
  assert.strictEqual(find('size').value, 'm')
  assert.strictEqual(find('n').value, '2', 'a slider shows its number')

  find('name')._dispatch({ kind: 'input', value: 'Ada' })
  find('agree')._dispatch({ kind: 'change', checked: true })
  find('blue')._dispatch({ kind: 'change', checked: true })
  find('ra')._dispatch({ kind: 'change', checked: true })
  find('size')._dispatch({ kind: 'change', value: 's' })
  find('n')._dispatch({ kind: 'input', value: '7' })
  find('trimmed')._dispatch({ kind: 'input', value: '  hi  ' })
  find('count')._dispatch({ kind: 'input', value: '42' })
  find('ta')._dispatch({ kind: 'input', value: 'line one' })
  find('num')._dispatch({ kind: 'input', value: '12.5' })
  await tick()
  assert.strictEqual(out(), 'Ada|true|red+blue|a|s|7:number|hi|42:number|line one|12.5:number')
  assert.strictEqual(find('ta').value, 'line one', 'a textarea')
  assert.strictEqual(find('wagree').checked, true, 'two controls on one ref agree')
  assert.strictEqual(find('rb').checked, false, 'the other radio cleared')

  find('red')._dispatch({ kind: 'change', checked: false })
  find('agree')._dispatch({ kind: 'change', checked: false })
  await tick()
  assert.strictEqual(out(), 'Ada|false|blue|a|s|7:number|hi|42:number|line one|12.5:number', 'removed from the array; unchecked')
})

test('Vue: v-model keeps the types of bound values, and true-value / false-value', async (t) => {
  const { window } = open(t, Raw)
  await settle(window)
  const find = finder(window)
  const out = () => find('out').textContent
  assert.strictEqual(out(), '[1,3]|2:number|no|10', 'initial values')
  assert.deepStrictEqual([find('c1').checked, find('c2').checked, find('c3').checked], [true, false, true], 'numbers in the array')
  assert.deepStrictEqual([find('r1').checked, find('r2').checked], [false, true], 'a number radio')
  assert.strictEqual(find('yn').checked, false)
  // one event per loop turn, as the native layer delivers them: Vue refreshes a control's model on render
  for (const [id, checked] of [['c2', true], ['c1', false], ['r1', true], ['yn', true], ['t20', true]]) {
    find(id)._dispatch({ kind: 'change', checked })
    await nextTick()
  }
  await tick()
  assert.strictEqual(out(), '[3,2]|1:number|yes|10,20', 'numbers stay numbers; true-value; a Set')
  assert.strictEqual(find('yn').checked, true)
  find('yn')._dispatch({ kind: 'change', checked: false })
  await tick()
  assert.match(out(), /\|no\|/, 'false-value')
})

test('Vue: v-model compares objects by structure, and follows an input whose type changes', async (t) => {
  const { window, component } = open(t, Loose)
  await settle(window)
  const find = finder(window)
  // a fresh, equal object is the same value (Vue's looseEqual)
  assert.deepStrictEqual([find('o1').checked, find('o2').checked], [true, false])
  find('o2')._dispatch({ kind: 'change', checked: true })
  await tick()
  find('o1')._dispatch({ kind: 'change', checked: false })
  await tick()
  assert.strictEqual(find('out').textContent.split('|')[0], '[{"id":2}]', 'removed the equal object, not a second copy')
  // text -> checkbox: the control listens for `change` now, and shows the model as a checkbox
  const dyn = find('dyn')
  dyn._dispatch({ kind: 'input', value: 'typed' })
  await tick()
  assert.strictEqual(component.value, 'typed')
  component.kind = 'checkbox'
  await tick()
  dyn._dispatch({ kind: 'input', value: 'ignored' })
  await tick()
  assert.strictEqual(component.value, 'typed', 'an input event no longer reports')
  dyn._dispatch({ kind: 'change', checked: true })
  await tick()
  assert.strictEqual(component.value, true, 'a checkbox reports through change')
})

test('Vue: v-for with keys keeps and moves nodes; v-if / v-else-if / v-else; fragments', async (t) => {
  const { window, component } = open(t, List)
  await settle(window)
  const find = finder(window)
  const [l1, l2, l3] = [find('li1'), find('li2'), find('li3')]
  assert.deepStrictEqual(elements(find('list')).map((e) => e.textContent), ['one', 'two', 'three'])
  assert.ok(find('some') && !find('none') && !find('empty'))
  component.reverse()
  await tick()
  assert.deepStrictEqual(elements(find('list')).map((e) => e.textContent), ['three', 'two', 'one'])
  assert.deepStrictEqual([find('li1'), find('li2'), find('li3')], [l1, l2, l3], 'moved, not recreated')
  component.drop()
  await tick()
  assert.deepStrictEqual(elements(find('list')).map((e) => e.textContent), ['two', 'one'])
  assert.strictEqual(find('li3'), undefined, 'the removed item is gone natively too')
  component.empty = true
  await nextTick()
  await tick()
  assert.ok(!find('some'), 'v-else gave way to v-else-if')
})

test('Vue: slots, named slots, props with defaults, emits, provide / inject', async (t) => {
  const { window } = open(t, Cards)
  await settle(window)
  const find = finder(window)
  assert.strictEqual(find('card-a').querySelector?.('h3') ?? elements(find('card-a'))[0].textContent, 'a', 'the head slot falls back to the title')
  const b = find('card-b')
  assert.strictEqual(elements(b)[0].textContent, 'head b', 'a named slot')
  assert.strictEqual(elements(b)[1].textContent, 'second', 'the default slot')
  assert.ok(find('card-untitled'), 'a prop default')
  assert.match(b.getAttribute('class'), /\bcard\b.*\bdark\b/, 'inject got the provided value')
  find('card-b')._dispatch({ kind: 'click' })
  await tick()
  assert.strictEqual(find('picked').textContent, 'b', 'an emitted event reached the parent')
})

test('Vue: Teleport to an element', async (t) => {
  const dest = native.document.createElement('div')
  dest.setAttribute('id', 'dest')
  const Comp = {
    render: () => h('section', { id: 'src' }, [h(Teleport, { to: dest }, [h('i', { id: 'tele' }, 'moved')])]),
  }
  const { window } = open(t, Comp)
  window.body.appendChild(dest)
  await settle(window)
  const find = finder(window)
  assert.strictEqual(find('tele').parentNode, dest, 'the content is under the teleport target')
  assert.strictEqual(elements(find('src')).length, 0, 'and not where it was written')
})

test('Vue: v-show and :class', async (t) => {
  const { window } = open(t, Show)
  await settle(window)
  const find = finder(window)
  let shot = await window.screenshot()
  assert.deepStrictEqual(at(shot, 10, 10), [255, 0, 0], 'shown')
  assert.deepStrictEqual(at(shot, 10, 30), [0, 0, 255], 'the class object: cold')
  find('toggle')._dispatch({ kind: 'click' })
  await tick()
  assert.match(find('a').getAttribute('style'), /display:\s*none/, 'v-show wrote display: none through el.style')
  shot = await window.screenshot()
  assert.notDeepStrictEqual(at(shot, 10, 10), [255, 0, 0], 'hidden: not painted')
  assert.strictEqual(find('b').getAttribute('class'), 'hot', 'the class object changed')
  find('toggle')._dispatch({ kind: 'click' })
  await tick()
  assert.doesNotMatch(find('a').getAttribute('style'), /none/, 'shown again')
})

test('Vue: scoped styles apply to the component\'s own elements only', async (t) => {
  const outer = { render: () => h(Scoped, null, { default: () => h('div', { id: 'slotted', class: 'swatch', style: 'width: 40px; height: 20px' }) }) }
  const { window } = open(t, outer)
  await settle(window)
  const find = finder(window)
  assert.ok(find('mine').getAttributeNames().some((n) => n.startsWith('data-v-')), 'the scope attribute is set')
  const shot = await window.screenshot()
  assert.deepStrictEqual(at(shot, 10, 10), [255, 0, 255], 'the scoped rule paints the component\'s element')
  assert.deepStrictEqual(at(shot, 10, 30), [0, 0, 0], 'an element of the parent with the same class is outside the scope')
  const styles = window.body.childNodes.filter((n) => n.localName === 'style').map((n) => n.textContent)
  assert.strictEqual(styles.filter((css) => /\.swatch\[data-v-/.test(css)).length, 1, 'one <style> for the component, scoped')
})

test('Vue: a <script> component, a TypeScript <script setup>', async (t) => {
  const plain = open(t, Plain, { props: { label: 'hello' } })
  await settle(plain.window)
  const p = finder(plain.window)('plain')
  assert.strictEqual(p.textContent, 'hello 0', 'props and data in the options API')
  p._dispatch({ kind: 'click' })
  await tick()
  assert.strictEqual(p.textContent, 'hello 1', 'a method ran')

  const typed = open(t, Typed, { props: { start: 9 }, app: plain.app })
  await settle(typed.window)
  const el = finder(typed.window)('typed')
  assert.strictEqual(el.textContent, '9', 'types stripped, defineProps<Props>() runtime props')
  el._dispatch({ kind: 'click' })
  await tick()
  assert.strictEqual(el.textContent, '10')
})

test('Vue: events: once, stop, an array of handlers, a replaced handler, keys', async (t) => {
  const log = []
  const handlers = ref([() => log.push('a')])
  const Comp = {
    setup () {
      return () => h('div', { id: 'outer', onClick: () => log.push('outer') }, [
        h('div', { id: 'once', onClickOnce: () => log.push('once') }),
        h('div', { id: 'many', onClick: handlers.value }),
        h('div', { id: 'key', onKeydown: (e) => log.push(`key:${e.key}`) }),
      ])
    },
  }
  const { window } = open(t, Comp)
  await settle(window)
  const find = finder(window)
  find('once')._dispatch({ kind: 'click' })
  find('once')._dispatch({ kind: 'click' })
  assert.deepStrictEqual(log.splice(0), ['once', 'outer', 'outer'], 'once ran once; the click still bubbled both times')
  find('many')._dispatch({ kind: 'click' })
  assert.deepStrictEqual(log.splice(0), ['a', 'outer'], 'an array of handlers')
  handlers.value = [() => log.push('b'), () => log.push('c')]
  await nextTick()
  find('many')._dispatch({ kind: 'click' })
  assert.deepStrictEqual(log.splice(0), ['b', 'c', 'outer'], 'the handlers were replaced, not added')
  find('key')._dispatch({ kind: 'keydown', key: 'Enter' })
  assert.deepStrictEqual(log.splice(0), ['key:Enter'])
})

test('Vue: event modifiers (.stop, .self, .enter) as templates compile them', async (t) => {
  const log = []
  const Comp = {
    setup () {
      return () => h('div', { id: 'outer', onClick: () => log.push('outer') }, [
        h('div', { id: 'stop', onClick: withModifiers(() => log.push('stop'), ['stop']) }),
        h('div', { id: 'self', onClick: withModifiers(() => log.push('self'), ['self']) }, [h('i', { id: 'inner' })]),
        h('div', { id: 'enter', onKeydown: withKeys(() => log.push('enter'), ['enter']) }),
      ])
    },
  }
  const { window } = open(t, Comp)
  await settle(window)
  const find = finder(window)
  find('stop')._dispatch({ kind: 'click' })
  assert.deepStrictEqual(log.splice(0), ['stop'], '.stop kept the click from the parent')
  find('self')._dispatch({ kind: 'click' })
  assert.deepStrictEqual(log.splice(0), ['self', 'outer'])
  find('enter')._dispatch({ kind: 'keydown', key: 'a' })
  find('enter')._dispatch({ kind: 'keydown', key: 'Enter' })
  assert.deepStrictEqual(log.splice(0), ['enter'], '.enter ignored other keys')
})

test('Vue: errors in event and v-model handlers go to app.config.errorHandler, not out of the event loop', async (t) => {
  const handled = []
  const Comp = {
    render: () => h('div', null, [
      h('div', { id: 'boom', style: 'width: 40px; height: 20px', onClick: () => { throw new Error('click boom') } }),
      h(TextField, { id: 'tf', modelValue: '', 'onUpdate:modelValue': () => { throw new Error('model boom') } }),
    ]),
  }
  const app = createApp(Comp)
  app.config.errorHandler = (err) => handled.push(err.message)
  const { window } = open(t, app)
  await settle(window)
  const find = finder(window)
  find('boom')._dispatch({ kind: 'click' })
  find('tf')._dispatch({ kind: 'input', value: 'x' })
  await tick()
  assert.deepStrictEqual(handled, ['click boom', 'model boom'], 'a throwing handler would have ended the process')
})

test('Vue: mouse button modifiers, with real and raw events', async (t) => {
  const log = []
  const Comp = {
    render: () => h('div', { id: 'l', style: 'width: 40px; height: 20px', onClick: withModifiers(() => log.push('left'), ['left']), onMousedown: withModifiers(() => log.push('right'), ['right']) }),
  }
  const { window } = open(t, Comp)
  await settle(window)
  window.click(5, 5)
  await tick()
  assert.deepStrictEqual(log, ['left'], 'a real click is the left button')
  finder(window)('l')._dispatch({ kind: 'click', button: 2 })
  finder(window)('l')._dispatch({ kind: 'mousedown', button: 2 })
  assert.deepStrictEqual(log, ['left', 'right'], '.left ignored button 2, .right took it')
})

test('Vue: a keyed reorder reaches the native tree, and unmounting frees the nodes', async (t) => {
  const items = ref([{ id: 1, c: '#ff0000' }, { id: 2, c: '#00ff00' }, { id: 3, c: '#0000ff' }])
  const shown = ref(true)
  const Comp = {
    render: () => (shown.value
      ? h('div', null, items.value.map((it) => h('div', { key: it.id, style: `height: 20px; width: 40px; background: ${it.c}` })))
      : h('p', 'gone')),
  }
  const { window } = open(t, Comp)
  await settle(window)
  const column = async () => { const s = await window.screenshot(); return [10, 30, 50].map((y) => at(s, 5, y).join()) }
  assert.deepStrictEqual(await column(), ['255,0,0', '0,255,0', '0,0,255'])
  const nodes = window._nodes.size
  items.value = [items.value[2], items.value[0], items.value[1]]
  await nextTick(); await tick()
  assert.deepStrictEqual(await column(), ['0,0,255', '255,0,0', '0,255,0'], 'the pixels follow the keys')
  for (let i = 0; i < 10; i++) { shown.value = !shown.value; await nextTick(); await tick(5) }
  shown.value = true
  await nextTick(); await tick()
  assert.ok(window._nodes.size <= nodes + 1, `native nodes did not pile up (${nodes} -> ${window._nodes.size})`)
})

test('Vue: createApp, plugins, render(app), createRoot; unmounting when the window closes', async (t) => {
  const events = []
  const Child = { setup () { onMounted(() => events.push('mounted')); onUnmounted(() => events.push('unmounted')); return () => h('div', { id: 'child' }, 'hi') } }
  const plugin = { install (app, options) { app.provide('who', options.who) } }
  const Root = { inject: ['who'], render () { return h('p', { id: 'who' }, [this.who, h(Child)]) } }
  const app = createApp(Root).use(plugin, { who: 'plugin' })
  const app_ = open(t, app)
  const { window, component } = app_
  await settle(window)
  const find = finder(window)
  assert.strictEqual(find('who').childNodes[0].textContent, 'plugin', 'a plugin provided a value')
  assert.deepStrictEqual(events, ['mounted'])
  assert.ok(component, 'render returns the root instance')
  window.close()
  await tick()
  assert.deepStrictEqual(events, ['mounted', 'unmounted'], 'closing the window unmounted the app')

  const second = app_.app.window({ width: 240, height: 160 }) // a window of our own, for createRoot
  const root = createRoot(second)
  root.render({ render: () => h('b', { id: 'one' }, 'one') })
  root.render({ render: () => h('b', { id: 'two' }, 'two') })
  await tick()
  const find2 = finder(second)
  assert.ok(find2('two') && !find2('one'), 'a root replaces what it showed')
})

test('Vue: BUI signals bind attributes; the component does not re-render', async (t) => {
  const color = signal('#0000ff')
  let renders = 0
  const Comp = { render () { renders++; return h('div', { id: 'sig', style: 'width: 40px; height: 20px', class: color.value ? undefined : '' , 'data-c': color }) } }
  const { window } = open(t, Comp)
  await settle(window)
  const el = finder(window)('sig')
  assert.strictEqual(el.getAttribute('data-c'), '#0000ff')
  color.value = '#ff0000'
  await tick()
  assert.strictEqual(el.getAttribute('data-c'), '#ff0000', 'the signal updated the attribute')
  assert.strictEqual(renders, 1, 'without a render')
})

test('Vue: the example fills its window: no white below the content or in the margin', async (t) => {
  // the example's own body setup (examples/counter.js), applied to its component
  const setup = /body\.className = '([^']+)'/.exec(fs.readFileSync(new URL('../examples/counter.js', import.meta.url), 'utf8'))
  assert.ok(setup, 'examples/counter.js sets the body class')
  let shared = null // one App at a time
  const shotOf = async (bodyClass) => {
    const r = open(t, Example, { width: 420, height: 300, ...(shared ? { app: shared } : {}) })
    shared ??= r.app
    const { window } = r
    window.body.style = ''
    window.body.setAttribute('styled', '')
    window.body.className = bodyClass
    await tick()
    return window.screenshot()
  }
  const corners = (s) => [[2, 2], [417, 2], [2, 297], [417, 297], [210, 280]].map(([x, y]) => at(s, x, y).join())
  assert.deepStrictEqual(new Set(corners(await shotOf(setup[1]))), new Set(['43,43,43']), 'the page color reaches every edge')
  // what the class string without min-h-screen did: the color stopped at the content, white below
  assert.strictEqual(at(await shotOf('m-0 bg-[#2b2b2b] text-[#e6e6e6]'), 210, 280).join(), '255,255,255', 'the cause: a body is only as tall as its content')
})

test('Vue: require() and import give the same package: BUI\'s render and createApp, Vue\'s everything else', () => {
  // an ES module that does `export *` from a CommonJS one loses its own names to it under require():
  // Vue's browser `render` came back (and returned nothing). index.cjs is what require() gets.
  const vue = createRequire(import.meta.url)('vue')
  const required = createRequire(import.meta.url)('..')
  // by name, as an app does: the register hook maps the name to this copy, and must pick the right file
  const byName = createRequire(import.meta.url)('@blender-ui/vue')
  for (const [name, mod] of [['import', ours], ['require', required], ['require by name', byName]]) {
    assert.notStrictEqual(mod.render, vue.render, `${name}: render is BUI's`)
    assert.notStrictEqual(mod.createApp, vue.createApp, `${name}: createApp is BUI's`)
    assert.strictEqual(mod.ref, vue.ref, `${name}: ref is Vue's own`)
    assert.strictEqual(mod.h, vue.h, `${name}: h is Vue's own`)
    assert.strictEqual(typeof mod.Button, 'function', `${name}: the widgets`)
    assert.strictEqual(typeof mod.createRoot, 'function', `${name}: createRoot`)
  }
  assert.strictEqual(required.render, ours.render, 'one render, however the package is loaded')
  assert.strictEqual(byName.render, ours.render)
  assert.strictEqual(required.addStyle, ours.addStyle, 'one style registry')
})

test('Vue: compiled components are cached: a cached start does not load Vue\'s compiler', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'bui-vue-cache-'))
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }))
  const file = path.join(dir, 'Fresh.vue')
  fs.writeFileSync(file, `<template><div>${Date.now()}</div></template>`) // a component no run has compiled
  const script = fileURLToPath(new URL('./fixtures/compile-once.mjs', import.meta.url))
  const run = () => spawnSync(process.execPath, [script, file], { encoding: 'utf8', cwd: path.dirname(script) }).stdout.trim()
  assert.strictEqual(run(), 'true', 'the first start compiles it, with Vue\'s compiler')
  assert.strictEqual(run(), 'false', 'the next one reads the compiled file: no compiler loaded')
  fs.writeFileSync(file, `<template><div>${Date.now()}!</div></template>`)
  assert.strictEqual(run(), 'true', 'a changed component compiles again')
})

test('Vue: a component outside the project imports the same Vue as the package', async (t) => {
  // no node_modules above it: `import 'vue'` resolves from this package, so there is one Vue
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'bui-vue-outside-'))
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }))
  const file = path.join(dir, 'Outside.vue')
  fs.writeFileSync(file, `<script setup>\nimport { ref, version } from 'vue'\nconst n = ref(1)\ndefineExpose({ n, version })\n</script>\n<template><div id="outside">{{ n }}</div></template>`)
  const Outside = (await import(pathToFileURL(file).href)).default
  const { window, component } = open(t, Outside)
  await settle(window)
  assert.strictEqual(finder(window)('outside').textContent, '1', 'it rendered')
  assert.strictEqual(component.version, createRequire(import.meta.url)('vue').version, 'with the package\'s Vue')
  component.n = 2
  await nextTick()
  assert.strictEqual(finder(window)('outside').textContent, '2', 'and its refs are tracked by the renderer\'s Vue')
})

test('Vue: Button, and what compile.js does to a template', () => {
  assert.strictEqual(typeof Button, 'function')
  const code = (src) => compileVue(src, 'x.vue').code
  const model = code('<template><input v-model="x"><input type="checkbox" v-model="y"></template>')
  assert.doesNotMatch(model, /vModelText|vModelCheckbox/, 'v-model is the core transform: no DOM directives')
  assert.match(model, /modelValue/)
  const big = '<template><div>' + '<p>static text</p>'.repeat(40) + '</div></template>'
  assert.doesNotMatch(code(big), /createStaticVNode/, 'no static HTML strings: there is no HTML parser to insert them with')
  assert.throws(() => code('<template><div/></template><style lang="scss">a { b { c: d } }</style>'), /scss/)
  assert.throws(() => code('<script setup lang="tsx">const a = 1</script><template><div/></template>'), /tsx/, 'JSX needs a transform')
  assert.throws(() => code('<script lang="jsx">export default {}</script><template><div/></template>'), /jsx/)
  assert.throws(() => code('<template lang="pug">div</template>'), /pug/, 'a template language is an error, not HTML')
  assert.throws(() => code('<template><div/></template><style>.a { color: v-bind(c) }</style><script setup>const c = 1</script>'), /v-bind/, 'v-bind() in <style> needs a browser')
  assert.throws(() => code('<template><div/></template><style module>.a{color:red}</style>'), /module/, 'CSS modules')
  assert.throws(() => code('<script setup lang="ts">enum E { A }</script><template><div/></template>'), /enum/, 'a TypeScript enum says so')
  assert.match(code('<template><div/></template><style scoped>.a{color:red}</style>'), /addStyle[\s\S]*data-v-/)
})
