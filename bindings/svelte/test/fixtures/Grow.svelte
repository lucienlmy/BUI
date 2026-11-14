<!-- A custom transition whose css is a width: in and out run on the engine (el.animate), and a
     transition interrupted by toggling back. -->
<script>
  let { expose } = $props()
  let show = $state(false)
  expose((v) => { show = v })
  const grow = (node, { duration }) => ({ duration, css: (t) => `width: ${t * 100}px; height: 10px` })
</script>

<div>{#if show}<b id="g" style="display: block" transition:grow={{ duration: 400 }}>x</b>{/if}</div>


