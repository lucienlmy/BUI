<script setup>
import { ref } from 'vue'
import { Button } from '@blender-ui/vue'
const count = ref(0)
</script>

<template>
  <main class="counter" style="margin: 0">
    <div id="box" :style="{ width: '80px', height: '30px', backgroundColor: count % 2 ? '#00ff00' : '#0000ff' }" @click="count++">
      count {{ count }}
    </div>
    <Button id="b" @click="count += 10">Add ten</Button>
    <div v-if="count > 0" id="extra" class="w-[50px] h-[10px] bg-[#ff0000]"></div>
  </main>
</template>

<style scoped>
.counter { background: #000000; }
</style>
