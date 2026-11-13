# Animation examples

Four small windows with the moves award-winning sites are made of, using BUI's `gsap` and `motion`.
Each file is about 50 lines and runs on Node (or Bun) with the `@blender-ui/native` addon built
(`cd bindings/node && npm run build`):

```sh
node examples/animation/hero.js        # a headline whose letters rise in one by one, a rule that draws itself
node examples/animation/cards.js       # a feature grid: staggered entrance, spring tilt on hover, counting numbers
node examples/animation/magnetic.js    # buttons that lean toward the pointer and snap back with an overshoot
node examples/animation/playground.js  # an endless marquee, a puck to drag and fling, an equalizer
```

| Example | What it shows |
|---|---|
| `hero.js` | one GSAP timeline: `from` with `stagger`, position strings (`'-=0.4'`), `fromTo`, `back.out` and `elastic.out` eases, `repeat: -1` with `yoyo` |
| `cards.js` | `fromTo` with `stagger`, Motion's `whileHover` / `whileTap` springs (`el.setMotion`), `animate(0, n, { onUpdate })` for counters |
| `magnetic.js` | Motion `animate` with `type: 'spring'`, interrupted on every pointer move (position and velocity carry over), low damping for the overshoot |
| `playground.js` | a seamless marquee (`ease: 'none'`, `repeat: -1`), `drag` with `dragConstraints`, `dragElastic` and inertia, per-element delays for a ripple |

None of them runs JavaScript per frame unless it gives an `onUpdate`: each call hands its values to the engine once, and the engine
moves the elements. `x`, `y`, `scale`, `rotate` and `opacity` are draw-time transforms, so nothing is laid
out again while they move.

## Screenshots and CI

Every example runs headless and saves frames from the engine's virtual clock, so the same moment comes out
the same every time:

```sh
BUI_SHOT=out.png BUI_SHOT_MS=100,500,3000 node examples/animation/hero.js   # out-100.png, out-500.png, out-3000.png
BUI_EXAMPLE_SECONDS=3 node examples/animation/hero.js                       # a real window that closes itself
```

`BUI_GPU=1` uses the GPU renderer. `magnetic.js` needs a real pointer, so it has no screenshot.
