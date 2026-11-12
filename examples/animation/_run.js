// Shared by the animation examples: opens a window, calls `build(win)`, and (for screenshots and CI)
// can run headless and save frames:
//   BUI_SHOT=out.png BUI_SHOT_MS=0,400,1200 node hero.js   -> out-0.png, out-400.png, out-1200.png
//   BUI_POINTER=300,200 BUI_SHOT=...                       -> the pointer is there from the first frame (hover states)
//   BUI_DRAG=100,100,300,150 BUI_SHOT=...                  -> press at the first point, drag to the second over 0.3 s
//   BUI_EXAMPLE_SECONDS=3 node hero.js                     -> a real window that closes itself
'use strict'
const fs = require('node:fs')
const zlib = require('node:zlib')
const { App } = require('../../bindings/node')
const { gsap } = require('../../bindings/node/gsap')

function png (width, height, rgba) {
  const raw = Buffer.alloc((width * 4 + 1) * height)
  for (let y = 0; y < height; y++) {
    raw[y * (width * 4 + 1)] = 0
    rgba.copy(raw, y * (width * 4 + 1) + 1, y * width * 4, (y + 1) * width * 4)
  }
  const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0 })
  const crc = (b) => { let c = 0xffffffff; for (const x of b) c = crcTable[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0 }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length)
    const td = Buffer.concat([Buffer.from(type), data])
    const c = Buffer.alloc(4); c.writeUInt32BE(crc(td))
    return Buffer.concat([len, td, c])
  }
  const head = Buffer.alloc(13); head.writeUInt32BE(width, 0); head.writeUInt32BE(height, 4); head[8] = 8; head[9] = 6
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', head), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))])
}

async function launch ({ title, width, height, build }) {
  const shot = process.env.BUI_SHOT
  const app = new App({ headless: Boolean(shot), gpu: process.env.BUI_GPU === '1' })
  if (shot) gsap.ticker.lagSmoothing(0) // the clock jumps in screenshots: those aren't stalls
  const win = app.window({ title, width, height })
  win.body.style = 'margin: 0'
  build(win)
  if (shot) {
    const at = (v) => (v || '').split(',').map(Number)
    const [px, py] = at(process.env.BUI_POINTER)
    if (process.env.BUI_POINTER) { await app.advanceClock(Number(process.env.BUI_POINTER_AT || 16)); win.mouseMove(px, py) }
    if (process.env.BUI_DRAG) {
      const [x0, y0, x1, y1] = at(process.env.BUI_DRAG)
      await app.advanceClock(16); win.mouseMove(x0, y0); win.mouseDown(x0, y0)
      for (let i = 1; i <= 10; i++) { await app.advanceClock(30); win.mouseMove(x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10) }
      win.mouseUp(x1, y1)
    }
    let t = 0
    for (const ms of (process.env.BUI_SHOT_MS || '0').split(',').map(Number)) {
      await app.advanceClock(ms - t)
      t = ms
      if (process.env.BUI_REBUILD) { win.body.setAttribute('data-r', String(ms)); win.flush(); await app.advanceClock(0) }
      const { width: w, height: h, data } = await win.screenshot()
      fs.writeFileSync(shot.replace(/\.png$/, `-${ms}.png`), png(w, h, data))
    }
    app.quit()
    return
  }
  if (process.env.BUI_EXAMPLE_SECONDS) setTimeout(() => app.quit(), Number(process.env.BUI_EXAMPLE_SECONDS) * 1000)
}

/** A `<div>` with a style object (and text) under `parent`. */
function el (win, parent, style, text) {
  const d = win.createElement('div')
  d.style = style
  if (text !== undefined) d.textContent = text
  parent.appendChild(d)
  return d
}

module.exports = { launch, el, png }
