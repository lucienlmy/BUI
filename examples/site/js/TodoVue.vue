<!-- The task list of todo_dom.js, todo_react.js and TodoSvelte.svelte in Vue 3 (@blender-ui/vue). -->
<script setup>
import { ref, computed } from 'vue'
import { Button, Checkbox, ProgressBar, TextField } from '../../../bindings/vue/index.js'
import style from './todo_style.js'
const { TASKS, TAG_COLORS, CSS } = style

let nextId = 0
const tasks = ref(TASKS.map((t) => ({ ...t, id: nextId++ })))
const filter = ref('All')
const draft = ref('')
const done = computed(() => tasks.value.filter((t) => t.done).length)
const pct = computed(() => Math.round(100 * done.value / tasks.value.length))
const shown = computed(() => tasks.value.filter((t) => filter.value === 'All' || (filter.value === 'Done') === t.done))
const add = () => { if (draft.value) tasks.value.push({ id: nextId++, text: draft.value, tag: 'Modeling', done: false }) }
</script>

<template>
  <div>
    <component :is="'style'">{{ CSS }}</component>
    <div class="head">
      <h1>Robot Arm · Sprint 4</h1>
      <span class="muted">{{ tasks.length - done }} open · {{ done }} done</span>
    </div>
    <div class="add">
      <TextField placeholder="Add a task…" v-model="draft" />
      <Button @click="add">Add</Button>
    </div>
    <div class="filters">
      <span v-for="f in ['All', 'Active', 'Done']" :key="f" :class="filter === f ? 'filter on' : 'filter'" @click="filter = f">{{ f }}</span>
    </div>
    <div class="list">
      <div v-for="task in shown" :key="task.id" :class="task.done ? 'item done' : 'item'">
        <Checkbox v-model="task.done" />
        <span class="text">{{ task.text }}</span>
        <span class="tag" :style="{ backgroundColor: TAG_COLORS[task.tag] }">{{ task.tag }}</span>
      </div>
    </div>
    <div class="foot">
      <ProgressBar class="bar" max="100" :value="String(pct)" />
      <span class="muted">{{ pct }}% complete</span>
    </div>
  </div>
</template>
