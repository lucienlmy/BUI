<!-- The counter of @blender-ui/react's and @blender-ui/svelte's examples, in Vue 3. -->
<script setup>
import { ref } from 'vue'
import { Button, NumberSlider } from '@blender-ui/vue'

const count = ref(0)
const name = ref('')
const size = ref(4)
</script>

<template>
  <main class="p-6 flex flex-col gap-3">
    <h1 class="text-xl font-bold">BUI + Vue</h1>
    <div class="flex items-center gap-3">
      <span>Clicks: {{ count }}</span>
      <Button @click="count++">Add one</Button>
    </div>
    <input placeholder="Your name" v-model="name" />
    <p v-if="name" class="text-[#9ab8e8]">Hello, {{ name }}!</p>
    <NumberSlider v-model="size" :min="1" :max="10" />
    <div class="bg-[#4772b3]" :style="{ width: size * 20 + 'px', height: '12px' }"></div>
  </main>
</template>
