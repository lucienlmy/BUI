<script setup>
import { ref, provide } from 'vue'
import Card from './Card.vue'
const picked = ref('')
provide('theme', 'dark')
</script>

<template>
  <div>
    <Card title="a" @pick="picked = $event">first</Card>
    <Card title="b" @pick="picked = $event"><template #head><b>head b</b></template>second</Card>
    <Card />
    <span id="picked">{{ picked }}</span>
  </div>
</template>
