'use strict'
const test = require('node:test')
const assert = require('node:assert')
const { App } = require('..')

test('tree, styles, Tailwind, events, screenshot (headless)', async (t) => {
  const app = new App({ headless: true })
  t.after(() => app.quit())
  const win = app.window({ width: 320, height: 200 })
  win.body.style = 'margin: 0; background: #000'
  const box = win.createElement('div')
  box.className = 'w-[100px] h-[40px] bg-[#ff0000]'
  win.body.appendChild(box)
  const btn = win.createElement('div')
  btn.style = { width: 80, height: 30, backgroundColor: '#0000ff' }
  const label = win.createTextNode('0')
  btn.appendChild(label)
  win.body.appendChild(btn)
  let clicks = 0
  const clicked = new Promise(resolve => btn.addEventListener('click', e => {
    clicks++
    label.data = String(clicks)
    btn.style = { width: 80, height: 30, backgroundColor: '#00ff00' }
    resolve(e)
  }))

  const r = await btn.getBoundingClientRect()
  assert.deepStrictEqual([r.x, r.y, r.width, r.height], [0, 40, 80, 30])
  let shot = await win.screenshot()
  const at = (s, x, y) => { const i = (y * s.width + x) * 4; return [...s.data.subarray(i, i + 3)] }
  assert.deepStrictEqual(at(shot, 10, 10), [255, 0, 0], 'Tailwind arbitrary class')
  assert.deepStrictEqual(at(shot, 70, 60), [0, 0, 255], 'style object')

  win.click(40, 55)
  const e = await clicked
  assert.strictEqual(e.type, 'click')
  assert.strictEqual(e.target, btn)
  shot = await win.screenshot()
  assert.deepStrictEqual(at(shot, 70, 60), [0, 255, 0], 'handler re-styled it')
  assert.strictEqual(btn.textContent, '1')
})

test('requestAnimationFrame runs callbacks per frame (headless)', async (t) => {
  const app = new App({ headless: true })
  t.after(() => app.quit())
  const win = app.window({ width: 100, height: 100 })
  const box = win.createElement('div')
  win.body.appendChild(box)
  let frames = 0
  await new Promise(resolve => {
    const step = () => {
      box.style = { width: ++frames, height: 10 }
      if (frames < 5) win.requestAnimationFrame(step)
      else resolve()
    }
    win.requestAnimationFrame(step)
  })
  const r = await box.getBoundingClientRect()
  assert.strictEqual(frames, 5)
  assert.strictEqual(r.width, 5, 'the last frame\'s change rendered')
})

test('signals bound to attributes and text update only their node (headless)', async (t) => {
  const { App, signal, memo } = require('..')
  const app = new App({ headless: true })
  t.after(() => app.quit())
  const win = app.window({ width: 100, height: 60 })
  win.body.style = 'margin: 0; background: #000'
  const color = signal('#0000ff')
  const n = signal(1)
  const box = win.createElement('div')
  box.style = memo(() => ({ width: 40, height: 20, backgroundColor: color.value }))
  const label = win.createElement('span')
  label.textContent = memo(() => `n = ${n.value}`)
  win.body.appendChild(box)
  win.body.appendChild(label)
  const tick = () => new Promise(r => setTimeout(r, 30))
  const at = (s, x, y) => { const i = (y * s.width + x) * 4; return [...s.data.subarray(i, i + 3)] }
  await tick()
  assert.deepStrictEqual(at(await win.screenshot(), 10, 10), [0, 0, 255])
  assert.strictEqual(label.textContent, 'n = 1')
  color.value = '#00ff00'
  n.value = 2
  assert.strictEqual(label.textContent, 'n = 2')
  await tick()
  assert.deepStrictEqual(at(await win.screenshot(), 10, 10), [0, 255, 0])
  box.style = 'width: 40px; height: 20px; background: #ff0000' // a plain value unbinds
  color.value = '#ffffff'
  await tick()
  assert.deepStrictEqual(at(await win.screenshot(), 10, 10), [255, 0, 0])
})

test('DOM semantics: moving nodes, insertBefore checks, bubbling with stopPropagation (headless)', async (t) => {
  const { App } = require('..')
  const app = new App({ headless: true })
  t.after(() => app.quit())
  const win = app.window({ width: 100, height: 100 })
  win.body.style = 'margin: 0'
  const a = win.createElement('div'), b = win.createElement('div'), x = win.createElement('div')
  x.style = 'width: 20px; height: 20px; background: #f00'
  win.body.appendChild(a)
  win.body.appendChild(b)
  a.appendChild(x)
  b.appendChild(x) // a move: must leave `a` natively too
  assert.strictEqual(a.childNodes.length, 0)
  assert.throws(() => a.insertBefore(win.createElement('i'), x), /not a child/)
  assert.throws(() => x.appendChild(b), /itself or its descendants/)
  const tick = () => new Promise(r => setTimeout(r, 30))
  await tick()
  const box = await x.getBoundingClientRect()
  assert.ok(box && box.width === 20, 'rendered once, in b')
  const log = []
  b.addEventListener('click', (e) => log.push(`b target=${e.target === x} current=${e.currentTarget === b}`))
  x.addEventListener('click', () => log.push('x'))
  win.body.addEventListener('click', () => log.push('body'))
  win.click(10, 10)
  await tick()
  assert.deepStrictEqual(log, ['x', 'b target=true current=true', 'body'], 'bubbles once through the JS tree')
  log.length = 0
  b.addEventListener('click', (e) => e.stopPropagation())
  win.click(10, 10)
  await tick()
  assert.deepStrictEqual(log, ['x', 'b target=true current=true'], 'stopPropagation stops it')
})

test('document: detached nodes join a window when inserted; fragments, clones, comments (headless)', async (t) => {
  const { App, document } = require('..')
  const app = new App({ headless: true })
  t.after(() => app.quit())
  const win = app.window({ width: 100, height: 100 })
  win.body.style = 'margin: 0'
  const tpl = document.createElement('div')
  tpl.className = 'flex gap-[20px]'
  tpl.append(document.createElement('Button'), document.createComment('anchor'), 'text')
  assert.strictEqual(tpl.ownerWindow, null, 'detached: nothing native yet')
  const copy = tpl.cloneNode(true)
  assert.deepStrictEqual(copy.childNodes.map(n => n.nodeName), ['BUTTON', '#comment', '#text'])
  assert.strictEqual(copy.firstChild.getAttribute('bui-blender'), '', 'a Blender widget, cloned')
  const frag = document.createDocumentFragment()
  frag.append(copy)
  win.body.appendChild(frag)
  assert.strictEqual(frag.firstChild, null, 'a fragment inserts its children')
  assert.strictEqual(copy.ownerWindow, win)
  assert.ok(copy.childNodes.every(n => n.ownerWindow === win && win._nodes.get(n.id) === n), 'the subtree joined the window')
  assert.strictEqual(copy.textContent, 'text', 'comments are not text')
  assert.ok(copy.isConnected)
  // linked siblings
  const [btn, comment, text] = copy.childNodes
  assert.strictEqual(btn.nextSibling, comment)
  assert.strictEqual(text.previousSibling, comment)
  comment.after('more')
  assert.strictEqual(comment.nextSibling.data, 'more')
  // the style object, classList, reflected properties
  btn.style.setProperty('width', '30px')
  btn.style.setProperty('height', '10px')
  btn.style.removeProperty('width')
  assert.strictEqual(btn.style.cssText.trim(), 'height: 10px;')
  // CSS properties as camelCase properties, as in a browser (Vue's v-show writes style.display)
  btn.style.display = 'none'
  btn.style.backgroundColor = 'red'
  assert.strictEqual(btn.style.display, 'none')
  assert.strictEqual(btn.style.getPropertyValue('background-color'), 'red')
  assert.match(btn.style.cssText, /display: none/)
  btn.style.display = ''
  assert.doesNotMatch(btn.style.cssText, /display/, 'an empty value removes the declaration')
  btn.style.removeProperty('background-color')
  assert.strictEqual(btn.style.cssText.trim(), 'height: 10px;', "Style's own members still work")
  btn.classList.add('a', 'b')
  btn.classList.toggle('a')
  assert.strictEqual(btn.className, 'b')
  btn.disabled = true
  assert.strictEqual(btn.getAttribute('disabled'), '')
  btn.disabled = false
  assert.strictEqual(btn.hasAttribute('disabled'), false)
  // dispatchEvent and composedPath; event types BUI doesn't report are accepted
  const seen = []
  win.body.addEventListener('custom', (e) => seen.push(e.target === btn && e.currentTarget === win.body))
  btn.addEventListener('pointerdown', () => {})
  btn.dispatchEvent(new CustomEvent('custom', { bubbles: true }))
  assert.deepStrictEqual(seen, [true])
  // a document's node removed and dropped is freed at the end of the tick; reinserted, kept
  await new Promise(r => setTimeout(r, 30))
  const size = win._nodes.size
  text.remove()
  btn.remove()
  copy.appendChild(btn)
  await new Promise(r => setTimeout(r, 30))
  assert.strictEqual(win._nodes.size, size - 1, 'the dropped text node is freed; the moved button is not')
  assert.ok(win._nodes.has(btn.id))
})

test('window kinds: layer-shell names are checked', (t) => {
  const app = new App({ headless: true })
  t.after(() => app.quit())
  assert.throws(() => app.window({ layerShell: { layer: 'middle' } }), /layerShell.layer: 'middle'/)
  assert.throws(() => app.window({ layerShell: { anchor: ['up'] } }), /layerShell.anchor/)
  assert.throws(() => app.window({ layerShell: { keyboard: 'all' } }), /layerShell.keyboard/)
  app.window({ alwaysOnTop: true, frame: false, layerShell: { layer: 'top', anchor: ['top', 'left'], exclusiveZone: 24 } })
})
