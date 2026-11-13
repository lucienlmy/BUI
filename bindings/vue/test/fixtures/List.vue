<script setup>
import { ref } from 'vue'
const items = ref([{ id: 1, t: 'one' }, { id: 2, t: 'two' }, { id: 3, t: 'three' }])
const empty = ref(false)
const reverse = () => { items.value = [...items.value].reverse() }
const drop = () => { items.value = items.value.slice(1) }
defineExpose({ reverse, drop, empty })
</script>

<template>
  <ul id="list">
    <li v-for="item in items" :key="item.id" :id="'li' + item.id">{{ item.t }}</li>
  </ul>
  <p v-if="items.length === 0" id="none">none</p>
  <p v-else-if="empty" id="empty">empty</p>
  <p v-else id="some">some</p>
</template>
