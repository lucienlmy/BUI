// Vue's props on a BUI element: what `patchProp` of the renderer does.
//
// Vue gives the renderer one prop at a time (`class`, `style`, `onClick`, `disabled` ...). The
// attributes, `style` and the removal rules are @blender-ui/native/host's `applyProps`, shared with
// React; this file adds what is Vue's: its event names (`onClick`, `onMouseenter`, `onClickOnce`,
// a handler or an array of them), `value` and `checked` as properties, and `v-model` (model.js).
//
// SPDX-License-Identifier: GPL-2.0-or-later
import { callWithAsyncErrorHandling, ErrorCodes } from 'vue'
import host from '@blender-ui/native/host'
import { setModelValue, setModelUpdate, setModelModifiers, refreshModel, modelTypeChanged } from './model.js'

const { applyProps, NO_PROPS } = host

/** Vue's own test for an event prop: `on` and then not a lowercase letter. */
const isOn = (key) => key.charCodeAt(0) === 111 && key.charCodeAt(1) === 110 &&
  (key.charCodeAt(2) > 122 || key.charCodeAt(2) < 97)

const OPTION = /(Once|Passive|Capture)$/ // suffixes of `@click.once` and friends

/** `onClickOnce` -> ['click', true]. Passive and capture mean nothing here: BUI has one phase. */
function eventName (key) {
  let name = key.slice(2)
  let once = false
  for (let m = OPTION.exec(name); m !== null; m = OPTION.exec(name)) {
    once ||= m[1] === 'Once'
    name = name.slice(0, -m[1].length)
  }
  return [name.toLowerCase(), once]
}

/** Listen for `key` with `next` (a function or an array of them), or stop. One listener per event
 *  stays on the element: a new handler only replaces what it calls, as Vue's own renderer does.
 *  Errors in a handler go the way Vue sends them: `onErrorCaptured`, then `app.config.errorHandler`
 *  (through `instance`, the component whose template has the element), not out of the event loop. */
function patchEvent (el, key, next, instance) {
  const invokers = (el._vei ??= {})
  const existing = invokers[key]
  if (existing !== undefined && next != null) {
    existing.value = next
    existing.instance = instance
    return
  }
  const [name, once] = eventName(key)
  if (next != null) {
    const invoker = (event) => {
      if (once) {
        el.removeEventListener(name, invoker)
        invokers[key] = undefined
      }
      callWithAsyncErrorHandling(invoker.value, invoker.instance, ErrorCodes.NATIVE_EVENT_HANDLER, [event])
    }
    invoker.value = next
    invoker.instance = instance
    invokers[key] = invoker
    el.addEventListener(name, invoker)
  } else if (existing !== undefined) {
    el.removeEventListener(name, existing)
    invokers[key] = undefined
  }
}

/** What an element needs known before its props are patched: `v-model` on a checkbox or a radio
 *  compares with the element's value, and the compiled props put `modelValue` before `type`. */
export function preset (el, props) {
  if (props == null || el.localName !== 'input') return el
  if (props.type != null) el.setAttribute('type', props.type)
  const type = el.getAttribute('type')
  if ((type === 'checkbox' || type === 'radio') && props.value != null) el.setAttribute('value', props.value)
  return el
}

let warnedHtml = false
// reused for every prop: applyProps takes objects, and a template has hundreds of props
const prevProp = {}
const nextProp = {}

/** The renderer's `patchProp(el, key, prevValue, nextValue, namespace, parentComponent)`. */
export function patchProp (el, key, prev, next, namespace, instance) {
  if (isOn(key)) {
    if (key === 'onUpdate:modelValue') setModelUpdate(el, next, instance)
    else patchEvent(el, key, next, instance)
    return
  }
  switch (key) {
    case 'modelValue': return setModelValue(el, next)
    case 'modelModifiers': return setModelModifiers(el, next)
    // the element's state (what was typed), not only its attribute; the bound value itself is kept
    // for a checkbox or a radio in a v-model (a number stays a number), as Vue's DOM renderer does
    case 'value':
      el._value = next
      el.value = next
      refreshModel(el)
      return
    case 'checked': el.checked = next; return
    case 'true-value':
      el._trueValue = next
      refreshModel(el)
      return
    case 'false-value':
      el._falseValue = next
      refreshModel(el)
      return
    case 'textContent': el.textContent = next ?? ''; return
    case 'innerHTML':
      if (!warnedHtml) {
        warnedHtml = true
        console.warn('@blender-ui/vue: v-html / innerHTML is not supported; build the content with elements')
      }
      return
  }
  prevProp[key] = prev
  nextProp[key] = next
  applyProps(el, prev == null ? NO_PROPS : prevProp, nextProp)
  delete prevProp[key]
  delete nextProp[key]
  if (key === 'type' && prev != null) modelTypeChanged(el)
}
