// GSAP and Motion on BUI's engine: a spring on hover, a timeline, and a staggered entrance.
//   node examples/animation.js            (BUI_GPU=1 for the GPU renderer)
'use strict'
const { App } = require('..')
const { gsap } = require('../gsap')

const app = new App({ gpu: process.env.BUI_GPU === '1' })
const win = app.window({ title: 'Animation', width: 360, height: 240 })
win.body.setAttribute('styled', '')
win.body.className = 'm-0 p-6 flex flex-col gap-4 bg-[#2b2b2b] text-[#e6e6e6]'

const row = win.createElement('div')
row.className = 'flex gap-3'
const boxes = ['#4772b3', '#9b6fe0', '#e8903c'].map(color => {
  const b = win.createElement('div')
  b.className = 'rounded-xl'
  b.style = { width: 56, height: 56, background: color }
  // Motion's props: a spring while hovered or pressed
  b.setMotion('whileHover', { scale: 1.12 })
  b.setMotion('whileTap', { scale: 0.94 })
  b.setMotion('transition', { type: 'spring', stiffness: 400, damping: 22 })
  row.appendChild(b)
  return b
})
win.body.appendChild(row)

const bar = win.createElement('div')
bar.style = { height: 8, width: 0, background: '#3fa66b', borderRadius: 4 }
win.body.appendChild(bar)

// GSAP: the boxes rise one after another, then a timeline fills the bar and pulses it
gsap.from(boxes, { opacity: 0, y: 24, duration: 0.5, ease: 'back.out(1.7)', stagger: 0.1 })
gsap.timeline({ delay: 0.4 })
  .to(bar, { width: 312, duration: 1, ease: 'power2.inOut' })
  .to(bar, { opacity: 0.4, duration: 0.3, repeat: 3, yoyo: true, ease: 'sine.inOut' })

win.on('close', () => console.log('closed'))
if (process.env.BUI_EXAMPLE_SECONDS) {
  setTimeout(() => app.quit(), Number(process.env.BUI_EXAMPLE_SECONDS) * 1000)
}
