// Compile one component in a fresh process and say whether Vue's compiler was loaded to do it
// (test/vue.test.js: a cached component must not need it).
import fs from 'node:fs'
import { createRequire } from 'node:module'
import { compileVue } from '../../compile.js'

const file = process.argv[2]
compileVue(fs.readFileSync(file, 'utf8'), file)
console.log(Object.keys(createRequire(import.meta.url).cache).some((k) => k.includes('compiler-sfc')))
