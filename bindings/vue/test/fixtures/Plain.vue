<script>
export default {
  props: { label: { type: String, default: 'plain' } },
  data: () => ({ n: 0 }),
  methods: { inc () { this.n++ } },
}
</script>

<template>
  <div id="plain" @click="inc">{{ label }} {{ n }}</div>
</template>
