# BUI RSX for Zed

Syntax highlighting, bracket/indent support and snippets for BUI's `rsx!` markup in the Zed editor.

## What you get

| Feature | Where it works |
| --- | --- |
| "BUI RSX" language: tags, attributes, bare Tailwind attributes (`p-4`, `bg-[#4772b3]`, `hover:bg-sky-600`, `w-1/2`, `-mt-2`, `2xl:flex`), variant attributes (`hover:class="..."`), `{rust}` interpolation highlighted as Rust, fragments, `//` comments, tag auto-close, brackets, indents | `.rsx` files only |
| Snippets for `rsx!`, `#[component]`, `use_state`, `for`/`if`/`match` blocks | Rust files (`snippets/rust.json`) and `.rsx` files (`snippets/bui rsx.json`) |

Snippet prefixes in Rust: `rsx`, `rsxt`, `bcomp`, `bcompc`, `bstate`, `rfor`, `rif`, `rmatch`, `rhover`, `ronclick`.

## Limitations (read this first)

- **No highlighting inside `rsx! { ... }` in `.rs` files.** Zed extensions cannot add to or override the built-in Rust `injections.scm`. That file hardcodes which macros get which language: for `view!` and `html!` it injects a language named `rstml` (a comment in Zed's source says "we need a better way for the leptos extension to declare that it wants to inject inside of rust"), every other macro, including `rsx!`, gets plain Rust. There is no extension API to register another macro name. So `rsx!` bodies in Rust files stay Rust-highlighted (mostly as token soup) no matter what this extension does.
- **`view!` is a partial workaround, not provided by this extension.** BUI documents `view!` as the same macro as `rsx!`. Installing Zed's *Leptos RSTML* extension makes `view! { ... }` highlight via the `rstml` grammar. That grammar does not understand bare Tailwind attributes (`bg-sky-500`, `p-4`, `w-1/2` produce parse errors; only attributes like `class="..."`, `hover:class="..."`, `disabled` parse cleanly), so highlighting degrades around them. This extension deliberately does not register a language named `rstml`, to avoid clashing with the Leptos one.
- **No Tailwind injection.** Zed has no built-in "Tailwind classes" language to inject into `class="..."`, and no extension API to colour utilities by meaning. `class="..."` strings and bare Tailwind attributes are tagged `@string.special` so a theme can distinguish them. Tailwind IntelliSense would need an LSP, and nothing here configures one.
- **Rust inside `{...}` is highlighted per chunk.** Text between nested braces/markup is injected as separate Rust fragments, so `for it in items ` and `if self.done ` highlight their keywords but are not parsed as whole statements. `a<b` (no space) inside braces is read as the start of a tag; write `a < b`.
- Bare words as children (`<p>all done</p>`) are highlighted as strings; `'}'` char literals inside braces are not understood.
- The `rsx!` wrapper itself is not part of the `.rsx` language: a `.rsx` file contains just the markup.

## Installing

In Zed run `zed: install dev extension` and pick this folder (`addons/bui-zed`). Zed builds the grammar from the repository named in `extension.toml`. After editing `grammar/grammar.js`, run `tree-sitter generate` in `grammar/`, commit, and point `rev` at that commit.

## Layout

```
extension.toml                  id bui-rsx, grammar + snippets
grammar/grammar.js, src/        tree-sitter-bui_rsx (Tailwind-friendly attribute names)
languages/bui_rsx/              config.toml, highlights, injections, brackets, indents
snippets/rust.json              snippets for Rust files
snippets/bui rsx.json           snippets for .rsx files (file name = lowercase language name)
```

## License

GPL-3.0-or-later, like BUI.
