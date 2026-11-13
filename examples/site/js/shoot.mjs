// Screenshots of the JavaScript examples for the website (run by
// tools/gen_doc_images.py --site):  node examples/site/js/shoot.mjs OUT_DIR THEME
// Headless windows, BUI_UI_SCALE for the resolution scale.
import { createRequire } from 'node:module'
import fs from 'node:fs'
import path from 'node:path'
import zlib from 'node:zlib'

const require = createRequire(import.meta.url)
const { App } = require('../../../bindings/node')
const [out = '.', theme = process.env.BUI_THEME || 'dark'] = process.argv.slice(2)

function crc32 (buf) {
  let c, crc = 0xffffffff
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1
    crc = (crc >>> 8) ^ c
  }
  return (crc ^ 0xffffffff) >>> 0
}

function png ({ width, height, data }) {
  const raw = Buffer.alloc((width * 3 + 1) * height)
  for (let y = 0; y < height; y++) {
    raw[y * (width * 3 + 1)] = 0
    for (let x = 0; x < width; x++) {
      const s = (y * width + x) * 4, d = y * (width * 3 + 1) + 1 + x * 3
      raw[d] = data[s]; raw[d + 1] = data[s + 1]; raw[d + 2] = data[s + 2]
    }
  }
  const chunk = (type, body) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(body.length)
    const tb = Buffer.concat([Buffer.from(type), body])
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(tb))
    return Buffer.concat([len, tb, crc])
  }
  const ihdr = Buffer.alloc(13)
  ihdr.writeUInt32BE(width, 0); ihdr.writeUInt32BE(height, 4)
  ihdr[8] = 8; ihdr[9] = 2
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))])
}

const settle = () => new Promise(resolve => setTimeout(resolve, 120))

async function save (win, name) {
  await settle()
  const shot = await win.screenshot()
  const file = path.join(out, `${name}-${theme}.png`)
  fs.writeFileSync(file, png(shot))
  console.log(`saved ${path.basename(file)} ${shot.width}x${shot.height}`)
}

const app = new App({ headless: true, theme })
const dom = require('./todo_dom.js')
await save(dom.build(app), 'js-dom-tasks')
const react = require('./todo_react.js')
const r = react.build({ app })
await save(r.window, 'js-react-tasks')
await import('../../../bindings/svelte/register.js') // .svelte files import as components
const svelte = await import('./todo_svelte.mjs')
await save(svelte.build({ app }).window, 'js-svelte-tasks')
await import('../../../bindings/vue/register.js') // .vue files too
const vue = await import('./todo_vue.mjs')
await save(vue.build({ app }).window, 'js-vue-tasks')
app.quit()
