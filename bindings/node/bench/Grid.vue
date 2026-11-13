<!-- bench/cells.js --vue: the grid of the React and Svelte benchmarks in Vue 3. Cell 0 follows
     `t` (a ref); the component re-renders when it changes, as a React one does, and Vue patches
     the cells whose props did not change by comparing them. With `--isolate`, cell 0 is a
     component of its own (Cell0.vue) and only it re-renders. -->
<script setup>
import { ref } from 'vue'
import Cell0 from './Cell0.vue'
const props = defineProps(['cells', 'cellStyle', 'label', 'gridStyle', 'expose', 'isolate'])
const t = ref(0)
if (!props.isolate) props.expose((v) => { t.value = v })
const rest = Array.from({ length: props.cells - 1 }, (_, i) => i + 1)
</script>

<template>
  <div :style="gridStyle">
    <Cell0 v-if="isolate" :cell-style="cellStyle" :label="label" :expose="expose" />
    <div v-else :style="cellStyle(0, t)">{{ label(0, t) }}</div>
    <div v-for="i in rest" :key="i" :style="cellStyle(i, 0)">{{ label(i, 0) }}</div>
  </div>
</template>
