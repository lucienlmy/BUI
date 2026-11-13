// Svelte components for BUI: compiled for the client, with DOM-built
// templates (`fragments: 'tree'`: createElement and setAttribute, no HTML
// parser), and Svelte's browser build (the `browser` export condition). One
// place for both, used by the Node.js hooks and the Bun plugin.
//
// Compiled files are cached (node_modules/.cache/@blender-ui/svelte, by
// content, Svelte version and mode): an app starts without loading Svelte's
// compiler unless a component changed.
//
// SPDX-License-Identifier: GPL-2.0-or-later
import fs from 'node:fs'
import { createRequire } from 'node:module'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import host from '@blender-ui/native/host'

const require = createRequire(import.meta.url)
const HERE = path.dirname(fileURLToPath(import.meta.url))
export const VERSION = require('svelte/package.json').version

/** The cache (compiled components, runtime.js's bundle) in the project's
 *  node_modules: the nearest one from the working directory up; none, no cache. */
export const CACHE = (() => {
  for (let dir = process.cwd(); ; dir = path.dirname(dir)) {
    if (fs.existsSync(path.join(dir, 'node_modules'))) return path.join(dir, 'node_modules', '.cache', '@blender-ui', 'svelte')
    if (path.dirname(dir) === dir) return null
  }
})()
let compiler = null // svelte/compiler (its CommonJS build: one file), on the first cache miss

/** Components (`.svelte`) and rune modules (`.svelte.js`). */
export const SVELTE_FILE = /\.svelte(\.js)?$/
/** Packages whose browser build a BUI app needs (Svelte's runtime, and the
 *  `esm-env` flags it reads). */
export const BROWSER_PACKAGE = /^(svelte|esm-env)(\/.*)?$/
/** This package, as compiled components import it (`@blender-ui/svelte`, and
 *  `/internal` for Svelte's runtime): always this copy (one event patch, one
 *  stylesheet registry), installed next to them or not. */
export const SELF = /^@blender-ui\/svelte(\/internal)?$/
export const selfFile = (specifier) => path.join(HERE, specifier.endsWith('/internal') ? 'internal.js' : 'index.js')
// compiled output's format: part of the cache key (bump when the rewrite changes)
const FORMAT = 2

const CONDITIONS = ['browser', 'import', 'default']

/** The client JavaScript of a `.svelte` / `.svelte.js` file. */
export function compileSvelte (source, filename, dev = process.env.NODE_ENV === 'development') {
  if (CACHE === null) return compileNow(source, filename, dev)
  const key = `${hash(`${FORMAT}\0${VERSION}\0${dev}\0${filename}\0${source}`)}-${source.length}`
  const cached = path.join(CACHE, `${key}.js`)
  try {
    return fs.readFileSync(cached, 'utf8')
  } catch {
    const code = compileNow(source, filename, dev)
    try {
      fs.mkdirSync(CACHE, { recursive: true })
      fs.writeFileSync(cached, code)
    } catch {} // a read-only place: no cache
    return code
  }
}

function compileNow (source, filename, dev) {
  compiler ??= require('svelte/compiler')
  if (!filename.endsWith('.svelte')) return toBui(compiler.compileModule(source, { filename, generate: 'client', dev }).js.code)
  const r = compiler.compile(source, { filename, generate: 'client', fragments: 'tree', css: 'external', dev })
  if (r.css === null) return toBui(r.js.code)
  // the scoped stylesheet: added to each window once (index.js), not per instance
  const key = hash(filename)
  return `${toBui(r.js.code)}\nimport { addStyle as __buiAddStyle } from '@blender-ui/svelte'\n` +
    `__buiAddStyle(${JSON.stringify(key)}, ${JSON.stringify(r.css.code)})\n`
}

export const { hash } = host

/** Svelte's runtime as this package serves it to compiled code (internal.js). */
function toBui (code) {
  return code.replace("from 'svelte/internal/client'", "from '@blender-ui/svelte/internal'")
}

/** The file `specifier` (`svelte`, `svelte/store` ...) names in its package's
 *  browser build, resolved from `fromDir`. */
export function resolveBrowser (specifier, fromDir) {
  const key = `${fromDir}\0${specifier}`
  let file = resolved.get(key)
  if (file === undefined) resolved.set(key, (file = findBrowser(specifier, fromDir)))
  return file
}

const resolved = new Map() // fromDir + specifier -> file
const exportsByRoot = new Map() // package root -> its package.json's exports

export function packageExports (root) {
  let e = exportsByRoot.get(root)
  if (e === undefined) exportsByRoot.set(root, (e = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8')).exports ?? {}))
  return e
}

function findBrowser (specifier, fromDir) {
  const slash = specifier.indexOf('/')
  const name = slash < 0 ? specifier : specifier.slice(0, slash)
  const sub = slash < 0 ? '.' : '.' + specifier.slice(slash)
  for (let dir = fromDir; ; dir = path.dirname(dir)) {
    const root = path.join(dir, 'node_modules', name)
    if (fs.existsSync(path.join(root, 'package.json'))) {
      const target = pick(packageExports(root)[sub])
      if (target === null) throw new Error(`@blender-ui/svelte: ${specifier}: no browser export`)
      return path.join(root, target)
    }
    if (path.dirname(dir) === dir) {
      if (fromDir !== HERE) return findBrowser(specifier, HERE) // not installed there: as this package sees it
      throw new Error(`@blender-ui/svelte: cannot find ${name}`)
    }
  }
}

/** Files of those packages: Bun's runtime resolves them without plugins. */
export const BROWSER_PACKAGE_FILE = /[\\/]node_modules[\\/](svelte|esm-env)[\\/]/
const redirects = new Map() // package root -> Map(default file -> browser file)

/** The browser counterpart of `file` when it is a package's non-browser entry
 *  (`svelte/src/index-server.js` -> `index-client.js`), else `file`. */
export function browserTarget (file) {
  const m = /^(.*[\\/]node_modules[\\/](?:svelte|esm-env))[\\/]/.exec(file)
  if (m === null) return null
  let map = redirects.get(m[1])
  if (map === undefined) {
    map = new Map()
    for (const entry of Object.values(packageExports(m[1]))) {
      const browser = pick(entry)
      const fallback = typeof entry === 'object' && entry !== null ? pickFrom(entry, ['default']) : null
      if (browser && fallback && browser !== fallback) map.set(path.join(m[1], fallback), path.join(m[1], browser))
    }
    redirects.set(m[1], map)
  }
  return map.get(path.normalize(file)) ?? file
}

/** Module source re-exporting `file` (and its default export, if it has one). */
export function reexport (file) {
  const to = JSON.stringify(file)
  const hasDefault = /\bexport default\b|\bas default\b/.test(fs.readFileSync(file, 'utf8'))
  return `export * from ${to}\n${hasDefault ? `export { default } from ${to}\n` : ''}`
}

/** The browser build of an export entry (`./store`'s ...), or null. */
export function pick (entry) { return pickFrom(entry, CONDITIONS) }

function pickFrom (entry, conditions) {
  if (typeof entry === 'string' || entry == null) return entry ?? null
  for (const c of conditions) if (c in entry) return pickFrom(entry[c], conditions)
  return null
}
