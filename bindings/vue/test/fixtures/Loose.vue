<script setup>
import { ref } from 'vue'
const picked = ref([{ id: 1 }])
const kind = ref('text')
const value = ref('hi')
defineExpose({ picked, kind, value })
</script>

<template>
  <div>
    <input id="o1" type="checkbox" :value="{ id: 1 }" v-model="picked">
    <input id="o2" type="checkbox" :value="{ id: 2 }" v-model="picked">
    <input id="dyn" :type="kind" v-model="value">
    <span id="out">{{ JSON.stringify(picked) }}|{{ JSON.stringify(value) }}</span>
  </div>
</template>
