<script setup lang="ts">
import { ref, type Ref } from 'vue'
interface Props { start?: number }
const props = withDefaults(defineProps<Props>(), { start: 5 })
const n: Ref<number> = ref(props.start)
</script>

<template>
  <div id="typed" @click="n++">{{ n }}</div>
</template>
