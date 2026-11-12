// A diagnostic for hover and drag with a REAL pointer: move the mouse over the red square, then drag the blue
// one. It prints what the window reports: pointer events, frame times, and where the squares are.
//   node examples/animation/_check.js
'use strict'
const { launch, el } = require('./_run')

launch({
  title: 'Pointer check', width: 520, height: 260,
  build (win) {
    const page = el(win, win.body, { width: 520, height: 260, background: '#101014', color: '#ddd', fontSize: 13 })
    const red = el(win, page, { position: 'absolute', left: 40, top: 60, width: 120, height: 120, background: '#ff5a36' })
    red.setMotion('whileHover', { scale: 1.4 })
    red.setMotion('transition', { type: 'spring', stiffness: 380, damping: 22 })
    const blue = el(win, page, { position: 'absolute', left: 300, top: 60, width: 120, height: 120, background: '#3aa0ff' })
    blue.setMotion('drag', true)
    blue.setMotion('whileDrag', { scale: 1.1 })
    const log = el(win, page, { position: 'absolute', left: 16, top: 10 }, 'move over red, drag blue')

    let moves = 0
    let last = performance.now()
    let worst = 0
    win.body.addEventListener('mousemove', () => { moves++ })
    const frame = () => { const now = performance.now(); worst = Math.max(worst, now - last); last = now; win.requestAnimationFrame(frame) }
    win.requestAnimationFrame(frame)
    win.on('close', () => process.exit(0))
    setInterval(async () => {
      const r = await red.getBoundingClientRect()
      const b = await blue.getBoundingClientRect()
      log.textContent = `mousemoves ${moves} | slowest frame ${worst.toFixed(0)} ms | red ${r ? r.width.toFixed(0) : '?'} px wide | blue x ${b ? b.x.toFixed(0) : '?'}`
      worst = 0
    }, 250)
  },
})
