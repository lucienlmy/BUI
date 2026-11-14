// Three small things in one window: an endless marquee (one linear tween on a repeating track), a puck you
// can drag and fling (drag with inertia), and an equalizer whose bars ripple out from the center (a delay
// by distance from the middle, yoyo, forever).
//   node examples/animation/playground.js
'use strict'
const { gsap } = require('../../bindings/node/gsap')
const { launch, el } = require('./_run')

launch({
  title: 'Playground', width: 760, height: 460,
  build (win) {
    const page = el(win, win.body, { width: 760, height: 460, background: '#0e0e12', color: '#f4f1ea', overflow: 'hidden' })

    // marquee: the same words twice, moved by exactly one copy's width, forever
    const lane = el(win, page, { height: 70, overflow: 'hidden', borderBottom: '1px solid #26262e' })
    const track = el(win, lane, { display: 'flex', width: 'max-content', paddingTop: 12 })
    const words = ['SPRINGS', 'TIMELINES', 'STAGGERS', 'EASES', 'DRAG', 'LAYOUT']
    for (let copy = 0; copy < 2; copy++) {
      for (const w of words) el(win, track, { fontSize: 34, fontWeight: 800, marginRight: 36, whiteSpace: 'pre', color: '#f4f1ea' }, `${w}  /`)
    }
    // half the track is one copy: move by exactly that, and the jump back is invisible
    const loop = () => track.getBoundingClientRect().then((r) => {
      if (!r) return setTimeout(loop, 50) // not laid out yet
      gsap.to(track, { x: -r.width / 2, duration: 14, ease: 'none', repeat: -1 })
    })
    loop()

    // a puck to throw around: drag it, let go, it keeps its momentum and stops at the walls
    const arena = el(win, page, { position: 'absolute', left: 40, top: 110, width: 400, height: 300, borderRadius: 18,
      background: '#14141a', border: '1px solid #26262e' })
    el(win, arena, { position: 'absolute', left: 16, top: 12, color: '#6e6c66', fontSize: 13 }, 'drag and fling')
    const puck = el(win, arena, { position: 'absolute', left: 170, top: 120, width: 60, height: 60, borderRadius: 30, background: '#ff5a36' })
    puck.setMotion('drag', true)
    puck.setMotion('dragConstraints', { left: -170, right: 170, top: -120, bottom: 120 })
    puck.setMotion('dragElastic', 0.2)
    puck.setMotion('whileDrag', { scale: 1.15 })
    puck.setMotion('transition', { type: 'spring', stiffness: 400, damping: 22 })

    // equalizer: bars scale from their base, rippling out from the middle
    const eq = el(win, page, { position: 'absolute', left: 480, top: 110, width: 240, height: 300, display: 'flex',
      alignItems: 'flex-end', gap: 8 })
    const bars = Array.from({ length: 9 }, (_, i) => el(win, eq, { flex: 1, height: 240, borderRadius: 6,
      background: ['#ff5a36', '#7c5cff', '#2ad4a0'][i % 3], transformOrigin: '50% 100%' }))
    bars.forEach((bar, i) => {
      gsap.fromTo(bar, { scaleY: 0.15 }, { scaleY: 1, duration: 0.7, ease: 'sine.inOut', repeat: -1, yoyo: true,
        delay: Math.abs(i - 4) * 0.09 }) // the further from the middle, the later it starts
    })
  },
})
