// Launch to first frame, for every way to run an app: the same React app
// (bench/first-frame.js; --svelte, --vue: its twins, first-frame-svelte.js and first-frame-vue.js) on Node, on Bun, as a Bun executable and as a
// QuickJS executable (`bui build`). Prints a JSON line per runtime: median
// and best of N launches, wall time from launch to exit, the app's own
// first-frame time and memory then, and the file size.
//   node bench/startup.js [runs=12] [--svelte | --vue]
'use strict'
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const { spawnSync } = require('node:child_process')

const framework = process.argv.includes('--vue') ? 'vue' : process.argv.includes('--svelte') ? 'svelte' : 'react'
const runs = Number(process.argv.slice(2).find((a) => /^\d+$/.test(a)) ?? 12)
const app = path.join(__dirname, { react: 'first-frame.js', svelte: 'first-frame-svelte.js', vue: 'first-frame-vue.js' }[framework])
// Svelte and Vue compile their components on load (Node.js and Bun); `bui build` bundles them compiled
const register = framework === 'react' ? null : require.resolve(`@blender-ui/${framework}/register`)
const cli = path.join(__dirname, '..', 'cli.js')
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'bui-startup-'))
const exe = process.platform === 'win32' ? '.exe' : ''
const env = { ...process.env, NODE_ENV: 'production' }

function build (args, out) {
  const r = spawnSync(process.execPath, [cli, 'build', app, '-o', out, '--console', ...args], { encoding: 'utf8' })
  if (r.status !== 0) throw new Error(r.stderr)
  return out
}
const bunExe = spawnSync('bun', ['-e', 'process.stdout.write(process.execPath)'],
  { encoding: 'utf8', shell: process.platform === 'win32' }).stdout.trim()

const variants = [
  ['Node.js', process.execPath, register ? ['--import', require('node:url').pathToFileURL(register).href, app] : [app], null],
  ['Bun', bunExe, register ? ['--preload', register, app] : [app], null],
  ['Bun executable (bui build --bun)', build(['--bun'], path.join(dir, `bun-app${exe}`)), [], 'self'],
  ['QuickJS executable (bui build)', build([], path.join(dir, `qjs-app${exe}`)), [], 'self'],
]

const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)] }
for (const [name, file, args, sized] of variants) {
  const wall = [], first = [], rss = []
  let failedExits = 0
  for (let i = 0; i < runs + 1; i++) {
    const t = process.hrtime.bigint()
    const r = spawnSync(file, args, { encoding: 'utf8', env })
    const ms = Number(process.hrtime.bigint() - t) / 1e6
    const m = /first-frame ([\d.]+) rss ([\d.]+)/.exec(r.stdout)
    if (!m) throw new Error(`${name}: ${r.stderr || r.stdout}`)
    if (i === 0) continue // the first launch warms the disk cache (and virus scanners)
    // a failed exit (after the first frame) is counted, and its exit time left out
    if (r.status !== 0) failedExits++
    else wall.push(ms)
    first.push(Number(m[1]))
    rss.push(Number(m[2]))
  }
  console.log(JSON.stringify({
    framework,
    runtime: name,
    // null: every launch failed at exit
    launch_to_exit_ms: wall.length ? { median: +median(wall).toFixed(0), best: +Math.min(...wall).toFixed(0) } : null,
    first_frame_ms: { median: +median(first).toFixed(0), best: +Math.min(...first).toFixed(0) },
    rss_at_first_frame_mb: +median(rss).toFixed(1),
    size_mb: sized ? +(fs.statSync(file).size / 1048576).toFixed(1) : null,
    failed_exits: failedExits,
  }))
}
fs.rmSync(dir, { recursive: true, force: true })
