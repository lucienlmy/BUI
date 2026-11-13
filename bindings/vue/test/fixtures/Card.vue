<script setup>
import { inject } from 'vue'
const props = defineProps({ title: { type: String, default: 'untitled' } })
const emit = defineEmits(['pick'])
const theme = inject('theme', 'plain')
</script>

<template>
  <section :id="'card-' + title" :class="['card', theme]" @click="emit('pick', title)">
    <h3><slot name="head">{{ title }}</slot></h3>
    <div class="body"><slot /></div>
  </section>
</template>
