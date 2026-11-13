<!-- bench/cells.js --vue --isolate: cell 0 as a component of its own, so a change of `t` re-renders
     this component alone and not the grid (how Vue code that updates one cell is written). -->
<script setup>
import { ref } from 'vue'
const props = defineProps(['cellStyle', 'label', 'expose'])
const t = ref(0)
props.expose((v) => { t.value = v })
</script>

<template>
  <div :style="cellStyle(0, t)">{{ label(0, t) }}</div>
</template>
