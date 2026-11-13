<script setup>
import { ref } from 'vue'
const ids = ref([1, 3])
const pick = ref(2)
const answer = ref('no')
const tags = ref(new Set([10]))
</script>

<template>
  <div>
    <input id="c1" type="checkbox" :value="1" v-model="ids">
    <input id="c2" type="checkbox" :value="2" v-model="ids">
    <input id="c3" type="checkbox" :value="3" v-model="ids">
    <input id="r1" type="radio" :value="1" v-model="pick">
    <input id="r2" type="radio" :value="2" v-model="pick">
    <input id="yn" type="checkbox" true-value="yes" false-value="no" v-model="answer">
    <input id="t10" type="checkbox" :value="10" v-model="tags">
    <input id="t20" type="checkbox" :value="20" v-model="tags">
    <span id="out">{{ JSON.stringify(ids) }}|{{ pick }}:{{ typeof pick }}|{{ answer }}|{{ [...tags].join() }}</span>
  </div>
</template>
