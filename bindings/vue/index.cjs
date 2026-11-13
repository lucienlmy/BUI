// `require('@blender-ui/vue')`: what `vue` exports, and BUI's own names over it. (An ES module that
// does `export *` from a CommonJS one, as index.js does with Vue, lets the CommonJS name win over its
// own export when it is `require()`d: Vue's browser `render` would come back instead of BUI's.)
//
// SPDX-License-Identifier: GPL-2.0-or-later
'use strict'
module.exports = Object.assign({}, require('vue'), require('./own.js'))
