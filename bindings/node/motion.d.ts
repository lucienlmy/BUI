// @blender-ui/native/motion: Motion's (Framer Motion's) functions on BUI's
// animation engine; the elements' Motion props are `MotionProps`.
import type { App, Element } from './index'

/** A value to animate to: a number (px for lengths, degrees for angles), a
 *  CSS value ('50%', '2rem', '45deg', '#f80', 'rgb(...)', '+=50'), null (where
 *  it is now), or keyframes. */
export type Value = number | string | null | MotionValue
export type Keyframes = Value | Array<number | string | null>

/** Motion's `transition` (and GSAP's timing options): every ease name of GSAP,
 *  Motion, CSS and Blender, `[x1, y1, x2, y2]`, or a name per keyframe. */
export interface Transition {
  type?: 'tween' | 'keyframes' | 'spring' | 'inertia' | 'decay'
  duration?: number
  /** seconds; `stagger(each)` in `animate()` */
  delay?: number | Stagger
  ease?: string | [number, number, number, number] | string[]
  times?: number[]
  repeat?: number
  repeatType?: 'loop' | 'reverse' | 'mirror'
  repeatDelay?: number
  stiffness?: number
  damping?: number
  mass?: number
  bounce?: number
  visualDuration?: number
  velocity?: number
  restDelta?: number
  restSpeed?: number
  power?: number
  timeConstant?: number
  min?: number
  max?: number
  bounceStiffness?: number
  bounceDamping?: number
  /** a transition per property: `{ x: { type: 'spring' } }`; `default` for the rest */
  [property: string]: unknown
}

/** Properties to animate (`x`, `y`, `scale`, `rotate`, `opacity`, any CSS
 *  property in camelCase or kebab-case), with an optional `transition`. */
export interface Target {
  transition?: Transition
  [property: string]: Keyframes | Transition | undefined
}

/** Motion's props, on every element (`el.setMotion(name, value)`; React's
 *  `<motion.div>` / any host element). A target is values, a variant's name,
 *  or false (`initial={false}`: start at `animate`). */
export interface MotionProps {
  initial?: Target | string | boolean
  animate?: Target | string
  /** what it animates to when it is removed: it stays until that ends (Motion's AnimatePresence) */
  exit?: Target | string
  whileHover?: Target | string
  whileTap?: Target | string
  whileFocus?: Target | string
  /** held while the element is in the viewport */
  whileInView?: Target | string
  /** held while it is dragged */
  whileDrag?: Target | string
  viewport?: { once?: boolean, amount?: number | 'some' | 'all' }
  transition?: Transition
  variants?: Record<string, Target>
  /** drag it: both axes, or one */
  drag?: boolean | 'x' | 'y'
  /** pixels from where the drag started */
  dragConstraints?: { left?: number, right?: number, top?: number, bottom?: number }
  /** how far past the constraints it goes, 0 to 1 */
  dragElastic?: number | boolean
  /** keep moving by inertia after the pointer lets go */
  dragMomentum?: boolean
  dragSnapToOrigin?: boolean
  /** a change of its box animates from the old box to the new one */
  layout?: boolean | 'position' | 'size'
}

/** A number (or CSS value) the engine animates; bind it with `style={{ x: mv }}`. */
export class MotionValue<T = number> {
  constructor (init?: T, app?: App)
  /** Its value now (asked of the UI thread). */
  get (): T
  /** Units per second, now. */
  getVelocity (): number
  /** Set it now, stopping what animates it. */
  set (value: T): void
  jump (value: T): void
  /** Every change JavaScript sees: its `set()`s, and each frame of motions
   *  started after this. Returns the unsubscribe function. */
  on (event: 'change', fn: (value: T) => void): () => void
  /** Free it now (else when it is garbage). */
  destroy (): void
}
export function motionValue<T = number> (init?: T, app?: App): MotionValue<T>
export function isMotionValue (v: unknown): v is MotionValue<unknown>

/** Motion's playback controls; await it (or `finished`) for its end. */
export class AnimationPlaybackControls implements PromiseLike<void> {
  play (): void
  pause (): void
  /** Stop where it is. */
  stop (): void
  /** To its end now. */
  complete (): void
  /** Back to its start, stopped. */
  cancel (): void
  time: number
  speed: number
  readonly duration: number
  readonly state: 'running' | 'paused' | 'finished'
  readonly finished: Promise<void>
  then<A = void, B = never> (onFulfilled?: ((v: void) => A | PromiseLike<A>) | null,
    onRejected?: ((e: unknown) => B | PromiseLike<B>) | null): Promise<A | B>
}

export interface AnimationOptions extends Transition {
  onUpdate?: (latest: number) => void
  onComplete?: () => void
  onStart?: () => void
  onRepeat?: () => void
}

/** Elements: a selector ('#id', '.class', 'tag', '[attr=v]'), an element, or a list. */
export type ElementTargets = string | Element | ArrayLike<string | Element>

/** Motion's `animate`: Motion's defaults (springs for x / y / scale / rotate). */
export function animate (target: ElementTargets | object, keyframes: Record<string, Keyframes>, options?: AnimationOptions): AnimationPlaybackControls
export function animate<T extends number | string> (value: MotionValue<T>, to: T | T[], options?: AnimationOptions): AnimationPlaybackControls
export function animate (from: number, to: number, options?: AnimationOptions): AnimationPlaybackControls

/** What `stagger()` makes. */
export interface Stagger { readonly __stagger: unique symbol }
/** `delay: stagger(0.05)`: each target starts `each` seconds after the last. */
export function stagger (each: number): Stagger
