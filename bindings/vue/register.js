// `import '@blender-ui/vue/register'` (or `node --import` / `bun --preload` it): `.vue` files
// import as components. Node.js through module hooks, Bun through its plugin.
//
// SPDX-License-Identifier: GPL-2.0-or-later
import fs from 'node:fs'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { VUE_FILE, SELF, VUE_PACKAGE, selfFile, compileVue } from './compile.js'
import plugin from './plugin.js'

if (typeof Bun !== 'undefined') {
  Bun.plugin(plugin)
} else {
  const { registerHooks } = await import('node:module')
  if (typeof registerHooks !== 'function') throw new Error('@blender-ui/vue: needs Node.js 22.15 or newer (module.registerHooks), or Bun')
  registerHooks({
    resolve (specifier, context, next) {
      if (SELF.test(specifier)) {
        const cjs = context.conditions?.includes('require')
        const url = pathToFileURL(selfFile(cjs)).href
        return cjs ? { url, shortCircuit: true } : { url, format: 'module', shortCircuit: true }
      }
      if (VUE_PACKAGE.test(specifier)) return next(specifier, { ...context, parentURL: pathToFileURL(selfFile()).href })
      return next(specifier, context)
    },
    load (url, context, next) {
      if (!url.startsWith('file:') || !VUE_FILE.test(url)) return next(url, context)
      const file = fileURLToPath(url)
      return { format: 'module', source: compileVue(fs.readFileSync(file, 'utf8'), file).code, shortCircuit: true }
    },
  })
}
