// bench/first-frame.js in Vue 3 (bench/FirstFrame.vue). Used by
// `bench/startup.js --vue`.
'use strict'
const { render } = require('@blender-ui/vue')
const App = require('./FirstFrame.vue').default

require('./first-frame-report')(render(App, { title: 'first frame', width: 360, height: 200, gpu: true }))
