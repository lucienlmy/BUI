<!-- bench/first-frame.js's React app, in Vue 3 (bench/startup.js --vue). -->
<script setup>
import { ref } from 'vue'
const count = ref(0)
</script>

<template>
  <main class="p-6 flex flex-col gap-3 bg-[#2b2b2b] text-[#e6e6e6]">
    <h1 class="text-xl font-bold">Hello</h1>
    <div class="flex items-center gap-3">
      <span>Clicks: {{ count }}</span>
      <button @click="count++">Add one</button>
    </div>
  </main>
</template>
