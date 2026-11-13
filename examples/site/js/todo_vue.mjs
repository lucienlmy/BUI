// The task list of todo_dom.js written in Vue 3 (@blender-ui/vue):
// TodoVue.vue, state in refs.
//   node --import ./bindings/vue/register.js examples/site/js/todo_vue.mjs
//   bun --preload ./bindings/vue/register.js examples/site/js/todo_vue.mjs
import { render } from '../../../bindings/vue/index.js'
import Tasks from './TodoVue.vue'

export function build (options = {}) {
  const r = render(Tasks, { title: 'Tasks (Vue)', width: 440, height: 370, ...options })
  r.window.body.setAttribute('styled', '')
  return r
}

if (import.meta.main ?? process.argv[1]?.endsWith('todo_vue.mjs')) build({ theme: process.env.BUI_THEME })
