// A hero intro, the kind award sites open with: letters rise in one by one, a rule draws itself, the
// subtitle and button follow, and three dots float forever. One GSAP timeline, no per-frame JavaScript.
//   node examples/animation/hero.js
'use strict'
const { gsap } = require('../../bindings/node/gsap')
const { launch, el } = require('./_run')

launch({
  title: 'Hero intro', width: 760, height: 420,
  build (win) {
    const page = el(win, win.body, { width: 760, height: 420, background: '#0e0e12', color: '#f4f1ea',
      display: 'flex', flexDirection: 'column', justifyContent: 'center', paddingLeft: 64, boxSizing: 'border-box' })

    // the headline, one box per letter so each can move on its own
    const row = el(win, page, { display: 'flex', paddingBottom: 6 })
    const letters = [...'MAKE IT MOVE.'].map((ch) =>
      el(win, row, { fontSize: 76, fontWeight: 800, letterSpacing: -2, whiteSpace: 'pre' }, ch))

    const rule = el(win, page, { width: 220, height: 4, background: '#ff5a36', marginTop: 10, marginBottom: 18 })
    const sub = el(win, page, { fontSize: 18, color: '#a9a69e', opacity: 0 }, 'Springs, timelines and staggers, on one native engine.')
    const button = el(win, page, { marginTop: 28, padding: '12px 26px', borderRadius: 30, background: '#f4f1ea',
      color: '#0e0e12', fontWeight: 700, width: 'fit-content', opacity: 0 }, 'Get started')
    const dots = [0, 1, 2].map((i) => el(win, page, { position: 'absolute', right: 60 + i * 46, top: 40 + (i % 2) * 36,
      width: 22, height: 22, borderRadius: 11, background: ['#ff5a36', '#7c5cff', '#2ad4a0'][i] }))

    rule.setMotion('style', { scaleX: 0 })
    const tl = gsap.timeline({ defaults: { ease: 'power4.out' } })
    tl.from(letters, { y: 90, rotate: 10, opacity: 0, duration: 0.9, stagger: 0.04 })
      .fromTo(rule, { scaleX: 0 }, { scaleX: 1, duration: 0.8, ease: 'expo.inOut' }, '-=0.4')
      .to(sub, { opacity: 1, y: 0, duration: 0.7 }, '-=0.5')
      .fromTo(button, { y: 30, scale: 0.8 }, { y: 0, scale: 1, opacity: 1, duration: 0.8, ease: 'back.out(2)' }, '-=0.4')
    gsap.from(dots, { scale: 0, duration: 0.8, ease: 'elastic.out(1, 0.4)', stagger: 0.12, delay: 0.6 })
    gsap.to(dots, { y: -16, duration: 1.4, ease: 'sine.inOut', repeat: -1, yoyo: true, stagger: { each: 0.25 }, delay: 1.2 })
  },
})
