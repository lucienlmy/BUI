// The example app through `bui run` (bundled for the QuickJS runtime) and as `bui build --bun`'s
// executable: the Bun plugin (plugin.js) compiling `.vue` files, and Vue's runtime in both.
import test from 'node:test'
import assert from 'node:assert'
import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const cli = path.join(here, '..', '..', 'node', 'cli.js')
const app = path.join(here, '..', 'examples', 'counter.js')
const hasBun = spawnSync('bun', ['--version'], { shell: process.platform === 'win32' }).status === 0
const env = { ...process.env, BUI_HEADLESS: '1', BUI_EXAMPLE_SECONDS: '1' }

test('bui run: the Vue example on the QuickJS runtime', { skip: !hasBun }, () => {
  const r = spawnSync(process.execPath, [cli, 'run', app], { encoding: 'utf8', env })
  assert.strictEqual(r.status, 0, r.stderr)
  assert.doesNotMatch(r.stderr, /error/i)
})

test('bui build --bun: the Vue example as one executable', { skip: !hasBun }, (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'bui-vue-'))
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }))
  const out = path.join(dir, `vue-app${process.platform === 'win32' ? '.exe' : ''}`)
  const b = spawnSync(process.execPath, [cli, 'build', app, '-o', out, '--bun', '--console'], { encoding: 'utf8' })
  assert.strictEqual(b.status, 0, b.stderr)
  const r = spawnSync(out, [], { encoding: 'utf8', env })
  assert.strictEqual(r.status, 0, r.stderr)
})
