// With `"jsxImportSource": "@blender-ui/react"` in tsconfig, host elements'
// props accept signals: `<div style={styleMemo} className={cls}>`, and
// Motion's props (`<motion.div animate={{ x: 100 }}>`, any element).
import type * as React from 'react'
import type { ReadSignal, MotionProps } from '@blender-ui/native'
import type { MotionValue } from '@blender-ui/native/motion'
export { Fragment, jsx, jsxs } from 'react/jsx-runtime'

type Fixed = 'children' | 'key' | 'ref' | 'dangerouslySetInnerHTML'
/** A style object whose values may be motion values: `style={{ x: mv, width: mv2 }}`. */
export type MotionStyle<S> = { [K in keyof S]: S[K] | MotionValue<any> } & { [K in 'x' | 'y' | 'scale' | 'scaleX' | 'scaleY' | 'rotate' | 'opacity']?: number | string | MotionValue<any> }
/** Each prop also takes a signal of its value (event handlers stay functions). */
export type Bindable<P> = {
  [K in keyof P]: K extends Fixed | `on${string}` ? P[K]
    : K extends 'style' ? MotionStyle<Exclude<P[K], undefined>> | string | ReadSignal<Exclude<P[K], undefined> | string>
    : P[K] | ReadSignal<Exclude<P[K], undefined>>
}
type Intrinsic = { [K in keyof React.JSX.IntrinsicElements]: Bindable<Omit<React.JSX.IntrinsicElements[K], keyof MotionProps>> & MotionProps }

export namespace JSX {
  type ElementType = React.JSX.ElementType
  interface Element extends React.JSX.Element {}
  interface ElementClass extends React.JSX.ElementClass {}
  interface ElementAttributesProperty extends React.JSX.ElementAttributesProperty {}
  interface ElementChildrenAttribute extends React.JSX.ElementChildrenAttribute {}
  type LibraryManagedAttributes<C, P> = React.JSX.LibraryManagedAttributes<C, P>
  interface IntrinsicAttributes extends React.JSX.IntrinsicAttributes {}
  interface IntrinsicClassAttributes<T> extends React.JSX.IntrinsicClassAttributes<T> {}
  interface IntrinsicElements extends Intrinsic {}
}
