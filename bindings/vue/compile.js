// Single-file components (`.vue`) for BUI: compiled with Vue's own compiler (`vue/compiler-sfc`),
// one place for the Node.js hooks (register.js) and the Bun plugin (plugin.js).
//
// What differs from a web build: the template compiler is Vue's DOM one with two changes. `v-model`
// is compiled with the core transform (props `modelValue` / `onUpdate:modelValue`, which model.js
// handles) because the DOM's `vModelText` and friends need a browser, and static hoisting into
// HTML strings (`createStaticVNode`) is off because there is no HTML parser to insert it with.
// `<style>` blocks are compiled by Vue (scoped ones get `[data-v-xxxx]` selectors) and handed to
// `addStyle`, which adds them to each window once. Only HTML templates, JavaScript or TypeScript
// scripts and CSS styles compile; other languages and `src=` blocks are errors, not silent.
//
// SPDX-License-Identifier: GPL-2.0-or-later
import { createRequire } from 'node:module'
import path from 'node:path'
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'
import host from '@blender-ui/native/host'

const { hash } = host
const require = createRequire(import.meta.url)
const HERE = path.dirname(fileURLToPath(import.meta.url))
const VERSION = require('vue/package.json').version

/** The cache of compiled components, in the project's node_modules: the nearest one from the
 *  working directory up; none, no cache. An app starts without loading Vue's compiler (most of
 *  Vue's size) unless a component changed. */
export const CACHE = (() => {
  for (let dir = process.cwd(); ; dir = path.dirname(dir)) {
    if (fs.existsSync(path.join(dir, 'node_modules'))) return path.join(dir, 'node_modules', '.cache', '@blender-ui', 'vue')
    if (path.dirname(dir) === dir) return null
  }
})()
// the compiled output's format: part of the cache key (bump it when this file's output changes)
const FORMAT = 1

/** Single-file components. */
export const VUE_FILE = /\.vue$/
/** This package, as compiled components import it: always this copy (one registry of styles),
 *  installed next to them or not. `require()` gets index.cjs, `import` index.js (see index.cjs). */
export const SELF = /^@blender-ui\/vue$/
export const selfFile = (cjs = false) => path.join(HERE, cjs ? 'index.cjs' : 'index.js')
/** `vue` and its subpaths: resolved from this package, wherever the importing file is, so that a
 *  component outside the project (or a library with a Vue of its own) shares this package's one Vue
 *  (two copies would have separate reactivity: a ref of one is not tracked by the other). */
export const VUE_PACKAGE = /^vue(\/.*)?$/

let sfc = null // vue/compiler-sfc, loaded on the first component (it is most of Vue's size)
let compiler = null

function load () {
  if (sfc !== null) return
  sfc = require('vue/compiler-sfc')
  const dom = createRequire(require.resolve('vue/package.json'))('@vue/compiler-dom')
  compiler = {
    ...dom,
    compile: (source, options) => dom.compile(source, {
      ...options,
      hoistStatic: false,
      directiveTransforms: { ...options.directiveTransforms, model: dom.transformModel },
    }),
  }
}

/** `module.stripTypeScriptTypes`, which Node.js still marks experimental: it is the one TypeScript
 *  tool Node.js ships, so its notice (printed once, on the first `lang="ts"` component) is
 *  dropped here, and only that one. It only removes types, as `node file.ts` does: enums,
 *  namespaces and parameter properties are errors (Bun compiles all of TypeScript). */
function stripTypes (code, filename) {
  const { stripTypeScriptTypes } = require('node:module')
  const emit = process.emitWarning
  process.emitWarning = (warning, ...rest) => {
    if (!String(warning).includes('stripTypeScriptTypes')) emit.call(process, warning, ...rest)
  }
  try {
    return stripTypeScriptTypes(code)
  } catch (err) {
    throw new Error(`@blender-ui/vue: ${filename}: ${err.message} (Node.js strips TypeScript types only; use a const object instead of an enum, or run on Bun)`, { cause: err })
  } finally {
    process.emitWarning = emit
  }
}

/** The languages this package compiles; anything else would be taken for plain JavaScript or HTML. */
function checkLanguages (descriptor, filename) {
  const fail = (what) => { throw new Error(`@blender-ui/vue: ${filename}: ${what} is not supported`) }
  const { template, script, scriptSetup } = descriptor
  if (template && template.lang && template.lang !== 'html') fail(`<template lang="${template.lang}">, only HTML`)
  for (const s of [script, scriptSetup]) {
    // JSX would need a JSX transform (Node strips types only), so only plain JavaScript and TypeScript
    if (s && s.lang && !['js', 'ts'].includes(s.lang)) fail(`<script lang="${s.lang}">, only js and ts`)
  }
  for (const block of [template, script, scriptSetup, ...descriptor.styles]) {
    if (block && block.src) fail('a block with src="…" (put the code in the file)')
  }
  // `v-bind()` in a style is kept up to date with a MutationObserver and the document's style rules;
  // CSS modules need the build tool to hand `$style` over
  if (descriptor.cssVars.length > 0) fail('v-bind() in <style>')
  if (descriptor.styles.some((s) => s.module)) fail('<style module>')
}

/**
 * The JavaScript of a `.vue` file. `lang="ts"` scripts come back as TypeScript when `keepTypes`
 * (Bun transpiles it), else with the types stripped (Node.js, `module.stripTypeScriptTypes`).
 * @returns {{ code: string, typescript: boolean }}
 */
export function compileVue (source, filename, { keepTypes = false } = {}) {
  if (CACHE === null) return compileNow(source, filename, keepTypes)
  const key = `${hash(`${FORMAT}\0${VERSION}\0${keepTypes}\0${filename}\0${source}`)}-${source.length}`
  const cached = path.join(CACHE, `${key}.json`)
  try {
    return JSON.parse(fs.readFileSync(cached, 'utf8'))
  } catch {
    const out = compileNow(source, filename, keepTypes)
    try {
      fs.mkdirSync(CACHE, { recursive: true })
      fs.writeFileSync(cached, JSON.stringify(out))
    } catch {} // a read-only place: no cache
    return out
  }
}

function compileNow (source, filename, keepTypes) {
  load()
  const { descriptor, errors } = sfc.parse(source, { filename })
  if (errors.length > 0) throw errors[0]
  checkLanguages(descriptor, filename)
  const id = hash(filename).slice(0, 8)
  const scopeId = `data-v-${id}`
  const scoped = descriptor.styles.some((s) => s.scoped)
  const typescript = [descriptor.script, descriptor.scriptSetup].some((s) => s?.lang === 'ts')
  const templateOptions = { compiler, scoped, id, compilerOptions: { scopeId: scoped ? scopeId : undefined } }

  const out = []
  if (descriptor.script || descriptor.scriptSetup) {
    const script = sfc.compileScript(descriptor, {
      id, inlineTemplate: descriptor.scriptSetup != null, templateOptions,
    })
    out.push(sfc.rewriteDefault(script.content, '_sfc_main', typescript ? ['typescript'] : undefined))
  } else {
    out.push('const _sfc_main = {}')
  }
  if (descriptor.template && !descriptor.scriptSetup) {
    const t = sfc.compileTemplate({ ...templateOptions, source: descriptor.template.content, filename })
    if (t.errors.length > 0) throw t.errors[0]
    out.push(t.code.replace(/\bexport function render\b/, 'function _sfc_render'), '_sfc_main.render = _sfc_render')
  }
  if (scoped) out.push(`_sfc_main.__scopeId = ${JSON.stringify(scopeId)}`)

  const css = []
  for (const style of descriptor.styles) {
    if (style.lang && style.lang !== 'css') throw new Error(`@blender-ui/vue: ${filename}: <style lang="${style.lang}"> is not supported, only CSS`)
    const r = sfc.compileStyle({ source: style.content, filename, id: scopeId, scoped: style.scoped })
    if (r.errors.length > 0) throw r.errors[0]
    css.push(r.code)
  }
  if (css.length > 0) {
    out.push("import { addStyle as _addStyle } from '@blender-ui/vue'", `_addStyle(${JSON.stringify(id)}, ${JSON.stringify(css.join('\n'))})`)
  }
  out.push('export default _sfc_main')

  let code = out.join('\n')
  if (typescript && !keepTypes) code = stripTypes(code, filename)
  return { code, typescript }
}
