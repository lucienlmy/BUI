// The Bun plugin: `.vue` files. `bui build` finds the default export through this package's
// `"bui": { "bunPlugin" }` field and bundles with it; Bun's runtime loads it through register.js.
//
// SPDX-License-Identifier: GPL-2.0-or-later
import fs from 'node:fs'
import path from 'node:path'
import { createRequire } from 'node:module'
import { VUE_FILE, SELF, VUE_PACKAGE, selfFile, compileVue } from './compile.js'

// `vue` as this package finds it: one copy of Vue (and of its reactivity) for the renderer and every
// component, as register.js does for Node
const fromHere = createRequire(selfFile(true))
const HERE = path.dirname(selfFile())

export default {
  name: '@blender-ui/vue',
  setup (build) {
    build.onResolve({ filter: SELF }, (args) => ({ path: selfFile(args.kind === 'require-call') }))
    build.onResolve({ filter: VUE_PACKAGE }, (args) => ({
      path: args.kind === 'require-call' ? fromHere.resolve(args.path) : Bun.resolveSync(args.path, HERE),
    }))
    build.onLoad({ filter: VUE_FILE }, (args) => {
      // Bun transpiles TypeScript itself
      const { code, typescript } = compileVue(fs.readFileSync(args.path, 'utf8'), args.path, { keepTypes: true })
      return { contents: code, loader: typescript ? 'ts' : 'js' }
    })
  },
}
