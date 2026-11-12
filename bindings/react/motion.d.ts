// @blender-ui/react/motion: Motion for React's API on BUI's animation engine.
// Every host element takes Motion's props (with `"jsxImportSource":
// "@blender-ui/react"`): `motion.div` is the `div` element, `motion.Button`
// Blender's Button.
import type { ComponentType, JSX } from 'react'
import type { MotionValue } from '@blender-ui/native/motion'
import type * as BUI from './index'

export type { MotionProps, MotionValue, Transition, Target, AnimationOptions, AnimationPlaybackControls }
  from '@blender-ui/native/motion'
export { animate, stagger, motionValue, isMotionValue } from '@blender-ui/native/motion'

type Widgets = 'Button' | 'Checkbox' | 'Radio' | 'NumberField' | 'NumberSlider' | 'TextField' | 'TextArea' |
  'ColorSwatch' | 'Dropdown' | 'ProgressBar'

/** `motion.div` ... (the host elements), `motion.Button` ... (Blender's
 *  widgets), `motion.create(Component)` / `motion(Component)`. */
export const motion: {
  <C extends ComponentType<any>>(component: C): C
  create<C extends ComponentType<any>>(component: C): C
} & { [K in keyof JSX.IntrinsicElements]: K } & { [K in Widgets]: typeof BUI[K] }

/** The same motion value every render. */
export function useMotionValue<T = number> (init: T): MotionValue<T>
/** Follows `source` (a number, or a motion value's changes) with a spring. */
export function useSpring (source: number | MotionValue<number>, options?: import('@blender-ui/native/motion').Transition): MotionValue<number>
/** `source` mapped from `input`'s ranges into `output`'s (clamped), or through `fn`. */
export function useTransform (source: MotionValue<number>, input: number[], output: number[]): MotionValue<number>
export function useTransform<T> (source: MotionValue<number>, fn: (v: number) => T): MotionValue<T>
export function useMotionValueEvent<T> (value: MotionValue<T>, event: 'change', fn: (v: T) => void): void
/** Motion's `transform(input, output)`. */
export function transform (input: number[], output: number[]): (v: number) => number

/** An element removed while it has an `exit` animates out before it goes (every element does, in or out of
 *  this component); it passes its children through, so Motion code that names it works. */
export function AnimatePresence (props: { children?: import('react').ReactNode, [prop: string]: unknown }): import('react').ReactElement | null
