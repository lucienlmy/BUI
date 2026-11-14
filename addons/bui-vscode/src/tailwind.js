'use strict';

// Compact generator for common Tailwind v4 utility names and variants.
// Not exhaustive: completion is a convenience, the BUI compiler is the validator.

const spacingScale = [
  '0', 'px', '0.5', '1', '1.5', '2', '2.5', '3', '3.5', '4', '5', '6', '7', '8', '9', '10', '11', '12',
  '14', '16', '20', '24', '28', '32', '36', '40', '44', '48', '52', '56', '60', '64', '72', '80', '96',
];
const fractions = ['1/2', '1/3', '2/3', '1/4', '3/4', '1/5', '2/5', '3/5', '4/5', '1/6', '5/6', 'full', 'auto'];
const sizeKeywords = ['full', 'screen', 'min', 'max', 'fit', 'auto'];
const palette = [
  'slate', 'gray', 'zinc', 'neutral', 'stone', 'red', 'orange', 'amber', 'yellow', 'lime', 'green',
  'emerald', 'teal', 'cyan', 'sky', 'blue', 'indigo', 'violet', 'purple', 'fuchsia', 'pink', 'rose',
];
const shades = ['50', '100', '200', '300', '400', '500', '600', '700', '800', '900', '950'];
const colorPrefixes = ['bg', 'text', 'border', 'ring', 'outline', 'fill', 'stroke', 'accent', 'caret', 'divide', 'from', 'via', 'to', 'shadow', 'decoration'];
const sizes = ['xs', 'sm', 'md', 'lg', 'xl', '2xl', '3xl'];

const spacingPrefixes = [
  'p', 'px', 'py', 'pt', 'pr', 'pb', 'pl', 'ps', 'pe', 'm', 'mx', 'my', 'mt', 'mr', 'mb', 'ml', 'ms', 'me',
  'gap', 'gap-x', 'gap-y', 'space-x', 'space-y', 'inset', 'inset-x', 'inset-y', 'top', 'right', 'bottom', 'left',
];
const sizePrefixes = ['w', 'h', 'size', 'min-w', 'min-h', 'max-w', 'max-h'];

const statics = [
  'flex', 'inline-flex', 'grid', 'inline-grid', 'block', 'inline-block', 'inline', 'hidden', 'contents',
  'flex-row', 'flex-col', 'flex-row-reverse', 'flex-col-reverse', 'flex-wrap', 'flex-nowrap', 'flex-1', 'flex-auto', 'flex-none',
  'grow', 'grow-0', 'shrink', 'shrink-0',
  'items-start', 'items-center', 'items-end', 'items-stretch', 'items-baseline',
  'justify-start', 'justify-center', 'justify-end', 'justify-between', 'justify-around', 'justify-evenly',
  'self-auto', 'self-start', 'self-center', 'self-end', 'self-stretch',
  'relative', 'absolute', 'fixed', 'sticky', 'static',
  'overflow-hidden', 'overflow-auto', 'overflow-scroll', 'overflow-visible', 'overflow-x-auto', 'overflow-y-auto', 'overflow-y-scroll',
  'rounded', 'rounded-none', 'rounded-sm', 'rounded-md', 'rounded-lg', 'rounded-xl', 'rounded-2xl', 'rounded-3xl', 'rounded-full',
  'border', 'border-0', 'border-2', 'border-4', 'border-t', 'border-b', 'border-l', 'border-r', 'border-x', 'border-y',
  'shadow', 'shadow-sm', 'shadow-md', 'shadow-lg', 'shadow-xl', 'shadow-2xl', 'shadow-none',
  'font-thin', 'font-light', 'font-normal', 'font-medium', 'font-semibold', 'font-bold', 'font-extrabold', 'font-mono', 'font-sans', 'font-serif',
  'italic', 'not-italic', 'underline', 'line-through', 'no-underline', 'uppercase', 'lowercase', 'capitalize',
  'truncate', 'text-ellipsis', 'text-clip', 'whitespace-nowrap', 'whitespace-normal', 'whitespace-pre',
  'text-left', 'text-center', 'text-right', 'text-justify',
  'leading-none', 'leading-tight', 'leading-snug', 'leading-normal', 'leading-relaxed', 'leading-loose',
  'tracking-tight', 'tracking-normal', 'tracking-wide',
  'cursor-pointer', 'cursor-default', 'cursor-text', 'cursor-not-allowed', 'select-none', 'pointer-events-none', 'pointer-events-auto',
  'opacity-0', 'opacity-25', 'opacity-50', 'opacity-75', 'opacity-100',
  'bg-white', 'bg-black', 'bg-transparent', 'text-white', 'text-black', 'text-transparent', 'border-white', 'border-black', 'border-transparent',
  'transition', 'transition-all', 'transition-colors', 'transition-opacity', 'transition-transform',
  'duration-75', 'duration-100', 'duration-150', 'duration-200', 'duration-300', 'duration-500', 'ease-in', 'ease-out', 'ease-in-out',
  'z-0', 'z-10', 'z-20', 'z-30', 'z-40', 'z-50',
  'grid-cols-1', 'grid-cols-2', 'grid-cols-3', 'grid-cols-4', 'grid-cols-6', 'grid-cols-12',
  'col-span-1', 'col-span-2', 'col-span-3', 'col-span-full', 'row-span-1', 'row-span-2',
  'object-cover', 'object-contain', 'object-center', 'sr-only', 'mx-auto', 'my-auto', 'm-auto',
  'text-xs', 'text-sm', 'text-base', 'text-lg', 'text-xl', 'text-2xl', 'text-3xl', 'text-4xl', 'text-5xl', 'text-6xl',
  'blur', 'blur-sm', 'blur-md', 'backdrop-blur', 'ring', 'ring-1', 'ring-2', 'ring-4', 'outline', 'outline-none',
  'scale-95', 'scale-100', 'scale-105', 'rotate-45', 'rotate-90', 'rotate-180',
];

function buildUtilities() {
  const out = new Set(statics);
  for (const p of spacingPrefixes) {
    for (const v of spacingScale) {
      out.add(`${p}-${v}`);
      if (/^(m|mx|my|mt|mr|mb|ml|ms|me|inset|inset-x|inset-y|top|right|bottom|left)$/.test(p)) out.add(`-${p}-${v}`);
    }
    out.add(`${p}-auto`);
  }
  for (const p of sizePrefixes) {
    for (const v of [...spacingScale, ...fractions, ...sizeKeywords]) out.add(`${p}-${v}`);
  }
  for (const s of sizes) out.add(`max-w-${s}`);
  for (const p of colorPrefixes) {
    for (const c of palette) for (const s of shades) out.add(`${p}-${c}-${s}`);
  }
  return [...out].sort();
}

const variants = [
  'hover', 'focus', 'focus-visible', 'focus-within', 'active', 'visited', 'disabled', 'enabled', 'checked',
  'first', 'last', 'odd', 'even', 'only', 'empty', 'required', 'invalid', 'placeholder', 'selection', 'file',
  'before', 'after', 'marker', 'first-line', 'first-letter',
  'sm', 'md', 'lg', 'xl', '2xl', 'max-sm', 'max-md', 'max-lg', 'max-xl', 'max-2xl',
  'dark', 'light', 'motion-safe', 'motion-reduce', 'print', 'portrait', 'landscape',
  'group-hover', 'group-focus', 'peer-hover', 'peer-focus', 'peer-checked', 'has-checked', 'open', 'aria-expanded', 'aria-selected', '*',
];

module.exports = { utilities: buildUtilities(), variants };
