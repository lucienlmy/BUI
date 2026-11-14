// A feature grid with the usual award-site touches: cards stagger in from below, lift and tilt toward
// you on hover (a spring), and the numbers count up as the cards land.
//   node examples/animation/cards.js
'use strict'
const { gsap } = require('../../bindings/node/gsap')
const { animate } = require('../../bindings/node/motion')
const { launch, el } = require('./_run')

const CARDS = [
  { title: 'Springs', stat: 120, unit: 'fps feel', color: '#ff5a36' },
  { title: 'Timelines', stat: 48, unit: 'tweens, one clock', color: '#7c5cff' },
  { title: 'Staggers', stat: 96, unit: 'items per call', color: '#2ad4a0' },
  { title: 'Eases', stat: 41, unit: 'named curves', color: '#ffb02e' },
  { title: 'Drag', stat: 100, unit: '% inertia', color: '#3aa0ff' },
  { title: 'Layout', stat: 0, unit: 'jank', color: '#ff4fa3' },
]

launch({
  title: 'Cards', width: 760, height: 460,
  build (win) {
    const page = el(win, win.body, { width: 760, height: 460, background: '#0e0e12', boxSizing: 'border-box', padding: 36 })
    const head = el(win, page, { color: '#f4f1ea', fontSize: 30, fontWeight: 800, marginBottom: 22 }, 'What you get')
    const grid = el(win, page, { display: 'flex', flexWrap: 'wrap', gap: 16 })

    const cards = CARDS.map((c) => {
      const card = el(win, grid, { width: 216, height: 150, borderRadius: 16, background: '#17171d', padding: 18,
        boxSizing: 'border-box', borderTop: `3px solid ${c.color}`, opacity: 0 })
      el(win, card, { color: '#a9a69e', fontSize: 14 }, c.title)
      const number = el(win, card, { color: '#f4f1ea', fontSize: 48, fontWeight: 800, marginTop: 10 }, '0')
      el(win, card, { color: c.color, fontSize: 13 }, c.unit)

      // Motion's props: a spring while the pointer is over it or pressed
      card.setMotion('whileHover', { y: -8, scale: 1.04, rotate: -1.5 })
      card.setMotion('whileTap', { scale: 0.97 })
      card.setMotion('transition', { type: 'spring', stiffness: 380, damping: 22 })
      card._number = number
      card._stat = c.stat
      return card
    })

    // enter: cards rise one after another, and each number counts up as its card lands
    gsap.fromTo(cards, { y: 70, opacity: 0 }, { y: 0, opacity: 1, duration: 0.9, ease: 'power3.out', stagger: 0.09 })
    cards.forEach((card, i) => {
      animate(0, card._stat, {
        duration: 1.2, delay: 0.25 + i * 0.09, ease: 'easeOut',
        onUpdate: (v) => { card._number.textContent = String(Math.round(v)) },
      })
    })
    gsap.from(head, { x: -30, opacity: 0, duration: 0.8, ease: 'expo.out' })
  },
})
