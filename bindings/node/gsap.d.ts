// @blender-ui/native/gsap: GSAP's API on BUI's animation engine.
import type { Element } from './index'
import type { Keyframes, MotionValue } from './motion'

/** Selectors ('#id', '.class', 'tag', '[attr=v]'), elements, motion values,
 *  or one plain object (its numbers, written back each frame). */
export type TweenTarget = string | Element | MotionValue<any> | object | ArrayLike<string | Element | MotionValue<any>>

/** GSAP's position parameter: seconds, '<', '>', '+=1', '-=0.5', '<0.2',
 *  'label', 'label+=1', '50%'. */
export type Position = number | string

export interface TweenVars {
  duration?: number
  delay?: number
  /** every GSAP, Motion, CSS and Blender ease: 'power2.out', 'elastic.out(1, 0.3)', 'steps(4)', a CustomEase name ... */
  ease?: string | [number, number, number, number]
  repeat?: number
  repeatDelay?: number
  yoyo?: boolean
  /** seconds between targets (or `{ each }`) */
  stagger?: number | { each: number }
  paused?: boolean
  reversed?: boolean
  immediateRender?: boolean
  overwrite?: boolean | 'auto'
  timeScale?: number
  onStart?: (this: Tween) => void
  onUpdate?: (this: Tween) => void
  onComplete?: (this: Tween) => void
  onRepeat?: (this: Tween) => void
  onReverseComplete?: (this: Tween) => void
  onInterrupt?: (this: Tween) => void
  onStartParams?: unknown[]
  onUpdateParams?: unknown[]
  onCompleteParams?: unknown[]
  onRepeatParams?: unknown[]
  onReverseCompleteParams?: unknown[]
  /** properties: x, y, scale, rotate, opacity, any CSS property, an object's numbers */
  [property: string]: unknown
}

export interface TimelineVars extends TweenVars {
  /** what its children's vars leave out */
  defaults?: TweenVars
}

/** A tween: GSAP's controls (getters without an argument, setters with one);
 *  `await` it for its end. */
export class Tween implements PromiseLike<Tween> {
  play (from?: number | string): this
  pause (atTime?: number | string): this
  resume (): this
  reverse (from?: number | string): this
  restart (): this
  kill (): this
  seek (time: number | string): this
  progress (): number
  progress (value: number): this
  totalProgress (): number
  totalProgress (value: number): this
  time (): number
  time (value: number): this
  totalTime (): number
  timeScale (): number
  timeScale (value: number): this
  duration (): number
  totalDuration (): number
  isActive (): boolean
  paused (): boolean
  paused (value: boolean): this
  reversed (): boolean
  reversed (value: boolean): this
  then<A = Tween, B = never> (onFulfilled?: ((t: Tween) => A | PromiseLike<A>) | null,
    onRejected?: ((e: unknown) => B | PromiseLike<B>) | null): Promise<A | B>
}

export class Timeline extends Tween {
  to (targets: TweenTarget, vars: TweenVars, position?: Position): this
  from (targets: TweenTarget, vars: TweenVars, position?: Position): this
  fromTo (targets: TweenTarget, fromVars: TweenVars, toVars: TweenVars, position?: Position): this
  set (targets: TweenTarget, vars: TweenVars, position?: Position): this
  /** a tween or timeline, a label, a function (as `call`), or an array of them */
  add (child: Tween | string | (() => void) | Array<Tween | string | (() => void)>, position?: Position): this
  addLabel (name: string, position?: Position): this
  call (fn: (...args: any[]) => void, params?: unknown[] | null, position?: Position): this
}

export interface GSAP {
  to (targets: TweenTarget, vars: TweenVars): Tween
  from (targets: TweenTarget, vars: TweenVars): Tween
  fromTo (targets: TweenTarget, fromVars: TweenVars, toVars: TweenVars): Tween
  set (targets: TweenTarget, vars: TweenVars): Tween
  timeline (vars?: TimelineVars): Timeline
  delayedCall (delay: number, fn: (...args: any[]) => void, params?: unknown[]): Timeline
  ticker: {
    /** a frame later than `threshold` ms counts as `adjusted` ms; 0 turns it off */
    lagSmoothing (threshold?: number, adjusted?: number): void
  }
  /** CustomEase needs no registering; other plugins aren't here (it throws). */
  registerPlugin (...plugins: unknown[]): void
}

export const gsap: GSAP
export default gsap

/** GSAP's CustomEase: an SVG path as a named ease; returns the name. */
export const CustomEase: { create (name: string, path: string): string }

export type { Keyframes }
