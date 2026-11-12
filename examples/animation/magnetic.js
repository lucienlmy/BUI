// Magnetic buttons: each pill leans toward the pointer while it is near, and snaps back with a springy
// overshoot when it leaves. Motion's `animate` with a spring does the physics; the label moves a little
// more than the pill for depth.
//   node examples/animation/magnetic.js
'use strict'
const { animate } = require('../../bindings/node/motion')
const { gsap } = require('../../bindings/node/gsap')
const { launch, el } = require('./_run')

const PILLS = [['Work', '#ff5a36'], ['About', '#7c5cff'], ['Contact', '#2ad4a0']]

launch({
  title: 'Magnetic buttons', width: 640, height: 300,
  build (win) {
    const page = el(win, win.body, { width: 640, height: 300, background: '#0e0e12', display: 'flex',
      alignItems: 'center', justifyContent: 'center', gap: 40 })

    const pills = PILLS.map(([text, color]) => {
      // a generous hit area around the pill, so it starts to lean before the pointer touches it
      const zone = el(win, page, { padding: 36 })
      const pill = el(win, zone, { padding: '16px 34px', borderRadius: 40, background: color, color: '#0e0e12', fontWeight: 800, fontSize: 18 })
      const label = el(win, pill, {}, text)
      let box = null

      zone.addEventListener('mouseenter', async () => { box = await pill.getBoundingClientRect() })
      zone.addEventListener('mousemove', (e) => {
        if (!box) return
        // how far the pointer is from the pill's center
        const dx = e.clientX - (box.x + box.width / 2)
        const dy = e.clientY - (box.y + box.height / 2)
        animate(pill, { x: dx * 0.4, y: dy * 0.4 }, { type: 'spring', stiffness: 220, damping: 14 })
        animate(label, { x: dx * 0.15, y: dy * 0.15 }, { type: 'spring', stiffness: 220, damping: 14 })
      })
      zone.addEventListener('mouseleave', () => {
        // low damping: it overshoots before it settles
        animate(pill, { x: 0, y: 0 }, { type: 'spring', stiffness: 300, damping: 8 })
        animate(label, { x: 0, y: 0 }, { type: 'spring', stiffness: 300, damping: 8 })
      })
      return pill
    })

    gsap.from(pills, { scale: 0.4, opacity: 0, duration: 0.8, ease: 'back.out(2.2)', stagger: 0.12 })
  },
})
