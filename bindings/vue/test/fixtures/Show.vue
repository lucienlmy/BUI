<script setup>
import { ref } from 'vue'
const shown = ref(true)
const on = ref(false)
</script>

<template>
  <div>
    <div id="a" v-show="shown" style="width: 40px; height: 20px; background: #ff0000"></div>
    <div id="b" :class="{ hot: on, cold: !on }" style="width: 40px; height: 20px"></div>
    <button id="toggle" @click="shown = !shown; on = !on">toggle</button>
  </div>
</template>

<style>
.hot { background: #00ff00; }
.cold { background: #0000ff; }
</style>
