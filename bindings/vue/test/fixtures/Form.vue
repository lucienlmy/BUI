<script setup>
import { ref } from 'vue'
import { NumberSlider, TextField, Checkbox } from '@blender-ui/vue'
const name = ref('')
const agree = ref(false)
const colors = ref(['red'])
const pick = ref('b')
const size = ref('m')
const n = ref(2)
const trimmed = ref('')
const count = ref(1)
const ta = ref('')
const num = ref(3)
</script>

<template>
  <div>
    <input id="name" type="text" v-model="name">
    <input id="agree" type="checkbox" v-model="agree">
    <input id="red" type="checkbox" value="red" v-model="colors">
    <input id="blue" type="checkbox" value="blue" v-model="colors">
    <input id="ra" type="radio" value="a" v-model="pick">
    <input id="rb" type="radio" value="b" v-model="pick">
    <select id="size" v-model="size"><option value="s">S</option><option value="m">M</option></select>
    <NumberSlider id="n" v-model="n" :min="0" :max="10" />
    <TextField id="trimmed" v-model.trim="trimmed" />
    <TextField id="count" v-model.number="count" />
    <Checkbox id="wagree" v-model="agree" />
    <textarea id="ta" v-model="ta"></textarea>
    <input id="num" type="number" v-model="num">
    <span id="out">{{ name }}|{{ agree }}|{{ colors.join('+') }}|{{ pick }}|{{ size }}|{{ n }}:{{ typeof n }}|{{ trimmed }}|{{ count }}:{{ typeof count }}|{{ ta }}|{{ num }}:{{ typeof num }}</span>
  </div>
</template>
