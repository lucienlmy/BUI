# BUI rsx!

VS Code support for the `rsx!` markup macro of [BUI](https://github.com/ppmpreetham/BUI) (blenderui), inside ordinary Rust files.

## Features

- **Syntax highlighting** for the body of `rsx! { ... }`, `rsx!( ... )`, `rsx![ ... ]` and its alias `view!`:
  - tags (`<div>`), components (`<Card>` in a distinct scope), fragments (`<>...</>`), closing tags;
  - attribute names, including `on*` event handlers, `data-*`, and variant prefixes (`hover:`, `md:dark:hover:`);
  - `class="..."` / `className="..."` strings, with each Tailwind token and each variant prefix scoped separately;
  - bare Tailwind attributes (`<div p-4 bg-[#4772b3] w-1/2>`);
  - `{expr}` embeds, highlighted as Rust (control flow such as `{for x in xs { <li/> }}` can contain markup directly);
  - string-literal text children and `//`, `/* */` comments.
- `tw!("...")` class strings get the same Tailwind highlighting.
- **Snippets**: `rsx`, `component`, `componentchildren`, `<div`, `<span`, `<button`, `<input`, `rsxfor`, `rsxif`, `rsxife`, `rsxmatch`, `<>`, `usestate`, `tw`.
- **Tailwind class completion** inside `class="..."` (also `tw!("...")`) within rsx blocks: common utilities and variants, not the full Tailwind catalogue. The BUI compiler remains the validator.

Ordinary Rust outside these macros is untouched. The extension is a TextMate injection grammar on `source.rust`, so it works with rust-analyzer and any Rust theme.

## Scopes

| Piece | Scope |
| --- | --- |
| macro name | `entity.name.function.macro.rsx` |
| HTML tag / component | `entity.name.tag.rsx` / `entity.name.type.component.rsx` |
| attribute name / event | `entity.other.attribute-name.rsx` / `...event.rsx` / `...class.rsx` |
| variant prefix | `keyword.other.variant.rsx` |
| Tailwind token | `support.constant.tailwind.rsx` |
| class string | `string.quoted.double.class.rsx` |
| text child / attribute string | `string.quoted.double.text.rsx` / `string.quoted.double.rsx` |
| `{expr}` | `meta.embedded.expression.rsx` (contains `source.rust`) |

## Known limits

- Bare words as text children (`<p>all done</p>`) are not scoped; quote text for highlighting.
- A bare attribute is highlighted as a Tailwind utility unless it is a known HTML boolean attribute or `data-*` / `aria-*`.
- Markup inside `{...}` is detected by a `<tag` not preceded by an identifier character, so `a <b` written without a space after `<` in a comparison can be mistaken for a tag.

## Build

```
npx @vscode/vsce package
```

then install the resulting `.vsix` with `code --install-extension bui-rsx-0.1.0.vsix`.
