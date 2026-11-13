# Writing BUI's docs

This is the guide for everyone writing pages for the BUI website. Read it once
before your first page; keep the [cheat-sheet](#component-cheat-sheet) open
while you write.

The four pages already written set the bar. Read them first, and copy their
shape:

- `src/content/docs/start/introduction.mdx`: a concept page
- `src/content/docs/start/installation.mdx`: a task page with `<LangTabs>`
- `src/content/docs/tutorials/first-app-python.mdx`: a tutorial with `<Steps>`
- `src/content/docs/project/roadmap.mdx`: a status page with `<StatusTable>`

---

## 1. Who we're writing for

From newcomers to experts, on the same page:

- **Newcomers** may never have used Blender, a UI toolkit or a terminal
  much. They need the first two paragraphs to make sense on their own, and
  every new term defined the first time it appears.
- **Experienced developers** skim. They need headings that say what's in the
  section, code they can copy, and exact names (`NumberSlider`, `BUI_GPU=1`).
- **Experts** want limits and edge cases. Put them in their own section
  ("Limitations", "How it works") near the end, not sprinkled through the
  tutorial.

When the two pull apart, write the main path for the newcomer and link the
expert to the detail.

### Per-language pages: one language, one reader

Pages that belong to one language (`tutorials/first-app-*`,
`tutorials/tkinter-app`, `python/*`, `rust/*`, `javascript/*`, `imgui/*`,
`c/*`) are read by someone who knows **only that language** and is coming
from it.

- Explain concepts in that language's own terms and idioms.
- No comparisons with other languages or their libraries ("like React's
  `useState`", "unlike Python…", "the same as the Rust crate"). No cards
  that send the reader to another language's page.
- Don't assume knowledge of other ecosystems. A Rust page may mention Rust
  crates (gpui, Leptos, taffy); a Python page may mention tkinter and Qt.
  Dear ImGui pages may map from ImGui's C++ names, because that's how
  ImGui's own docs are written.
- Shared pages (`start/`, `concepts/`, `reference/`, `performance/`,
  `project/`) may compare languages.

### Rust: both syntaxes, always

BUI's Rust crate has two first-class syntaxes: **`rsx!`** (HTML-like
markup, components and hooks) and **gpui builders** (`div().flex().gap_2()
.child(...)`, `Render`, `cx.listener`, state in the view's fields). Wherever
a Rust page shows a UI example, show **both**, side by side:

````mdx
<Tabs sync="rust-syntax" label="Rust syntax">
<Tab label="rsx!">

```rust
rsx! { <div flex gap-2>"Hi"</div> }
```

</Tab>
<Tab label="gpui builders">

```rust
div().flex().gap_2().child("Hi")
```

</Tab>
</Tabs>
````

Always use exactly these labels and `sync="rust-syntax"`, so the reader's
choice carries across pages. Never present the builder syntax as a
footnote or an "advanced" alternative. Say "HTML-like markup" for `rsx!`,
not "JSX".

### Rust is on crates.io

`blenderui` is published on crates.io: install with `cargo add blenderui`.
Never write "not published yet" or tell readers to use a git dependency for
it. The same goes for `blenderui-imgui`: `cargo add blenderui-imgui`.

### No recaps, no emoji

- **No `<Recap>` blocks.** End a page with next-step cards only.
- **No emoji anywhere in content** (no checkmarks, colored dots, test
  tubes, arrows-as-emoji). For status, use `<StatusTable>` status keys
  (`done`, `partial`, `experimental`, `planned`), `<Badge>`, or plain words
  ("Works", "Partial", "Experimental"). The components draw the icons.

### Demo tricks: once, and only where they're true

Drag-to-scrub, <Kbd keys="Ctrl+Z" /> undo and typing `2*pi` into a field are
BUI's party tricks. Mention each **at most once per page**, and only where
it's relevant. Typed math works only in **Blender number fields and
sliders** (Blender components, tkinter `Spinbox` / `Scale`, `QSlider`,
form controls inside `styled`, ImGui's Blender-widgets build). Never imply
it works in plain text inputs: an HTML `<input>` outside `styled`,
tkinter's `Entry`, `QLineEdit` or ImGui's `InputText`.

## 2. Voice

**Warm, confident, precise.** Like a friend who knows the code well and is
happy you're here.

- **Second person, active voice.** "Drag the slider", not "the slider can be
  dragged". "BUI redraws only the widget", not "only the widget is redrawn".
- **Short sentences.** One idea each. If a sentence has two commas and an
  "and", split it.
- **Plain words.** "Use", not "utilize". "Start", not "initialize" (unless
  it's the API's name).
- **Define terms on first use**, in the same sentence: "the **kit**, BUI's
  runtime written in C, …". Add new terms to the glossary page.
- **Lead with what the reader gets**, then how. "You get Blender's color
  picker: click the swatch." not "ColorSwatch opens a picker on click."
- **Be honest about status.** If something is partial, say so in the first
  place the reader would trip over it. Never imply something works when it
  doesn't. Unbuilt features get `<ComingSoon />` (see below).
- **Never invent numbers.** Every number (timings, counts, sizes) must come
  from the repository, with the same rounding. Benchmarks come with their
  context (machine, renderer, what was measured). If you can't find a source,
  leave the number out.
- **No fake quotes, testimonials or users.**
- **Headings** are sentence case, no trailing period: "Drag across
  checkboxes", not "Drag Across Checkboxes."
- **Write "BUI"** for the project, `blenderui` for the Python and Rust
  package names, and `@blender-ui/react` / `@blender-ui/native` for npm.
  In Python code, import `blenderui` (not `bui`): it's the published name.
- **Keyboard keys** use `<Kbd>`: <Kbd>Ctrl</Kbd>, `<Kbd keys="Ctrl+Shift+Z" />`.
  Write "Ctrl" (not "Control" or "^") and add "(Cmd on macOS)" only where
  it's true.

### Jokes

**One gentle joke per page, at most**, and it's optional. Put it in
`<Callout type="joke">` or a short aside in the text. Good jokes are about
Blender culture (the default cube, pressing <Kbd>Q</Kbd>, render noise) or
about software. Never about the reader, their skill, other projects or
people. If you have to explain it, cut it.

## 3. Page template

Copy this into a new `.mdx` file and fill it in:

```mdx
---
title: Number fields and sliders        # sentence case, no period
description: One sentence that says what the reader will get from this page.
section: Concepts                       # one of the sections in src/config/nav.ts
order: 30                               # position in the section; leave gaps
status: stable                          # stable | preview | coming-soon
languages: ["python", "rust"]           # optional: python rust javascript c cpp imgui
badges: ["Windows only"]                # optional, short
sources: ["docs/components.md", "kit/intern/kit_widgets.c"]   # repo files you checked
---

A hook: one or two sentences that tell the reader why this matters to them,
in their terms. What will they be able to do after reading?

## First section

Plain explanation, then a small, runnable example.

## …

## Next steps

<CardGrid>
  <Card title="…" href="docs/…/" icon="arrow-right">One sentence.</Card>
</CardGrid>
```

Rules of thumb:

- **The hook** is the first lines, before any heading. No "In this page we
  will…". Say what the reader gets.
- **Examples are small and runnable.** Copy-paste into a file and run. Show
  the imports. Prefer 5 to 25 lines. Give the file a name with `title=`.
- **Every page ends with next-step cards** (two to four). No `<Recap>`.
- **Length:** tutorials can be long; guides should be skimmable; reference
  pages are tables. Split a page when it has more than about eight `##`
  sections.
- **Don't repeat other pages.** Link to them. The installation page owns
  installation; the reference pages own full tables.

## 4. Adding a page or a section

**Add a page:** create one `.mdx` file under `src/content/docs/<folder>/`.
That's all. The sidebar, breadcrumbs, previous/next links, search index and
"Soon" badges are generated from the frontmatter. The URL follows the path:
`src/content/docs/rust/hooks.mdx` → `/docs/rust/hooks/`.

- `section` decides the sidebar group (the folder is just for tidiness; keep
  them matching).
- `order` decides the position inside the group (ties sort by title).
- `draft: true` keeps a page out of the build while you work on it.
- The build fails if the frontmatter doesn't match the schema in
  `src/content.config.ts`, so typos in `section` are caught for you.

**Add a section:** add an entry to `sections` in `src/config/nav.ts`, in the
order it should appear. Nothing else changes.

**Components need no imports.** Every component in `src/components/docs/` is
available in every `.mdx` page.

**Links between pages:** in Markdown, use relative links with a trailing
slash: from `/docs/start/installation/`, write
`[the tutorial](../../tutorials/first-app-python/)`. In `<Card href>`, use the
site path without a leading slash: `href="docs/tutorials/first-app-python/"`.
Both keep working if the site moves to a sub-path.

**Check your work:**

```sh
pnpm dev         # http://localhost:4321, live reload
pnpm build       # must finish with no errors or warnings
pnpm check       # type-checks components and frontmatter
```

## 5. Component cheat-sheet

All components live in `src/components/docs/` and are documented at the top
of each file.

### Callout

```mdx
<Callout type="tip">Hold Shift while dragging for fine control.</Callout>
<Callout type="warning" title="Windows only">The npm packages are prebuilt for Windows x64.</Callout>
<Callout type="joke">BUI is Blender's UI, minus the default cube.</Callout>
```

`type`: `note` (default), `tip`, `warning`, `joke`, `coming-soon`. Optional
`title`. One or two per page; if everything is a callout, nothing is.

### ComingSoon

For features that **aren't built yet**. It links to the roadmap and to
GitHub issues.

```mdx
<ComingSoon feature="WebAssembly" />

<ComingSoon feature="Per-glyph font fallback">
  Characters missing from Inter show as boxes for now. Blender's BLF fallback is next.
</ComingSoon>

Works on Linux and Windows; macOS GPU rendering is <ComingSoon inline />.
```

If a whole page describes something unbuilt, also set `status: coming-soon`
in the frontmatter: the sidebar shows "Soon" and the roadmap lists it.

### LangTabs, Tabs and Tab

`<LangTabs>` remembers the reader's language across pages. Use the labels
**Python, Rust, JavaScript, C, C++, Dear ImGui** so the choice carries over.
Leave blank lines around Markdown inside a `<Tab>`.

````mdx
<LangTabs>
<Tab label="Python">

```py
bui.set_gpu(True)
```

</Tab>
<Tab label="Rust">

```rust
Application::new().gpu(true)
```

</Tab>
</LangTabs>
````

`<Tabs>` is the same without the language memory; `<Tabs sync="pkg">`
syncs all tab groups with the same key (for example npm / pnpm / bun). Don't
nest tabs.

### Steps

Wrap a normal numbered list. Each step starts with a bold sentence that says
what to do.

```mdx
<Steps>

1. **Install BUI.**

   ```sh
   pip install blenderui
   ```

2. **Run the demo.**

</Steps>
```

The bold first sentence becomes the step's title: it stays pinned under the
header while the reader is in that step, and a rail fills as they go. Keep it
short (one line).

### Figure

```mdx
<Figure
  src="images/qt.png"
  alt="A Qt-syntax form on BUI: text field, dropdown, slider and buttons"
  caption="Qt syntax, drawn by BUI (<code>examples/qt_syntax_demo.py</code>)"
  width={520}
  height={222}
/>
```

`alt` says what's in the picture; `caption` says why it matters. `width` and
`height` are read from the file when you leave them out. `variant="bleed"`
drops the hatched stage for wide images.

**Light and dark screenshots.** The reader sees the one that matches the
site's theme:

```mdx
<Figure light="images/shots/qt-form-light.png" dark="images/shots/qt-form-dark.png" alt="…" />
<Figure shot="qt-form" src="images/qt.png" alt="…" />   {/* the pair from images/shots/, src until it exists */}
<ThemedImage shot="qt-form" fallback="images/qt.png" alt="…" />   {/* the bare image, no frame */}
```

### Charts: BenchChart, BarChart, GroupedBarChart, LineChart

Every published benchmark number lives in `src/data/bench.ts` (copied from
README.md "Performance"). Use it instead of retyping numbers:

```mdx
<BenchChart id="first-frame" />     {/* chart + "Open bench" with the exact table */}
<BenchTable id="startup" />         {/* just the exact table */}
```

Chart ids: `first-frame`, `idle-all`, `start-time`, `start-cpu`, `idle-cpu`,
`memory-start`, `start-all`, `fps`, `cpu-per-frame`, `ram`,
`cpu-per-frame-cells`, `uncapped-fps`, `frame-times`, `js-first-frame`,
`js-cpu-3000`. Table ids: `startup`, `cells3000`, `cellsFull`, `frameTimes`,
`jsStartup`, `jsAnim3000`, `jsAnim1000`, plus one-language views for per-language
pages: `startupGpui` (the same gpui code only), `jsStartupJs`, `jsAnim3000Js`,
`jsAnim1000Js` (no Rust rows). Optional `title`, `subtitle`, `plain` (no headline
number) and `table` (a different table id under "Open bench").

Your own numbers (still from the repository, with their source):

```mdx
<BarChart
  title="Wheel size" unit="MB" better="lower"
  bars={[
    { label: "blenderui", value: 4.2, ours: true },
    { label: "PySide6", value: 180 },
  ]}
/>
<GroupedBarChart title="…" unit="ms" series={[{ name: "CPU", ours: true }, { name: "GPU" }]}
  groups={[{ label: "Rust", values: [38, 42] }]} />
<LineChart title="…" unit="fps" x={[100, 1000, 3000]} xLabel="cells"
  lines={[{ name: "GPU", ours: true, values: [1491, 309, 94] }]} />
```

`ours: true` draws BUI's bars in Blender blue; the rest are hatched grey with
an automatic "N× more" note. Add `table={{ caption, columns, rows }}` for an
"Open bench" disclosure.

### StatusTable

```mdx
<StatusTable rows={[
  ["tkinter syntax", "done", "Every public name: 7,846 of 7,846"],
  ["Qt syntax", "partial", "31% of the core classes' methods"],
  ["macOS host", "experimental", "Written, not yet run on a Mac"],
  ["WebAssembly", "planned", "The kit in a <code>&lt;canvas&gt;</code>"],
]} />
```

Status: `done`, `partial`, `experimental`, `planned` (the component draws
the icon and label; never type emoji yourself). Optional
`columns={["Feature", "Status", "Notes"]}`, `legend={false}`.

### CardGrid and Card

`<Recap>` still exists as a component, but don't use it: pages have no
recaps.

```mdx
<CardGrid>
  <Card title="Hooks" href="docs/rust/hooks/" icon="layers" kicker="Rust">use_state, signals and effects.</Card>
  <Card title="egui" soon>BUI panels inside egui.</Card>
</CardGrid>
```

`<CardGrid cols={3}>` for three columns. Card icons: any
[Phosphor](https://phosphoricons.com) name (`rocket-launch`, `gauge`,
`file-py`, `terminal-window` …) or a short alias (`terminal`, `book`,
`sparkles`, `flag`, `layers`, `zap`, `cpu`, `monitor`, `undo`, `github`).
A misspelled name fails the build. The same icons work inline:
`<Icon name="flask" weight="duotone" />`.

### Compare

```mdx
<Compare left="Standard tkinter" right="On BUI">
  <div slot="left">

  Text, code or a `<Figure />`.

  </div>
  <div slot="right">

  The other side.

  </div>
</Compare>
```

### Kbd, Badge, Hatch, PageStatusList

```mdx
Press <Kbd>Q</Kbd>, or <Kbd keys="Ctrl+Shift+Z" />.
<Badge>New</Badge> <Badge tone="preview">Preview</Badge> <Badge tone="soon">Soon</Badge>
<Hatch />                               {/* a divider between big parts of a long page */}
<PageStatusList status="coming-soon" /> {/* pages by status, from the collection */}
```

### Code blocks

Code blocks use [Expressive Code](https://expressive-code.com): copy
buttons, light and dark themes, and these extras:

````mdx
```py title="app.py" {3}            ← file name tab, line 3 highlighted
```rust ins={2} del={1}             ← added / removed lines
```sh frame="terminal" title="PowerShell"   ← a terminal window
```js "useState"                    ← highlight a word
````

Use `py`, `rust`, `js`/`jsx`/`ts`, `c`, `cpp`, `cmake`, `toml`, `sh` (for
commands; no `$` prompts), `text` for output.

### Tables

Plain Markdown tables. They get borders and scroll sideways on phones. Align
numbers right with `| ---: |`.

## 6. Images

- **Screenshots of BUI** come from the repository: `docs/images/*.png`,
  regenerated by `python tools/gen_doc_images.py`. Copy the ones you use into
  `website/public/images/` with the same file name, and reference them as
  `images/<name>.png`.
- Don't edit, crop or upscale screenshots; they show real pixels. Never
  present a mock-up as a screenshot. If no screenshot exists, say so, or ask
  for one in your PR.
- **Diagrams**: prefer a short list or table. If you need a picture, use an
  inline SVG in the site's colors (`currentColor`, `var(--primary)`), with
  text in the SVG for accessibility.
- File names: lowercase, dashes, what it shows: `rust-forms.png`.
- Keep PNGs under about 200 KB.

Images available now in `public/images/`: `c-kit-hello.png`, `components.png`,
`components-layout.png`, `components-nodes.png`, `components-values.png`,
`components-views.png`, `first-app-python.png`, `imgui-blender.png`,
`imgui-python-hello.png`, `imgui-python-hello-blender.png`, `imgui-stock.png`,
`qt.png`, `qt-export.png`, `qt-gauge.png`, `react-counter.png`,
`rust-forms.png`, `rust-hello.png`, `rust-showcase.png`, `tkinter_canvas.png`,
`tkinter_items.png`, `tkinter_messagebox.png`, `tkinter_notes.png`,
`tkinter-render-settings.png`. New screenshots go in `public/images/shots/` as `-dark.png` / `-light.png` pairs,
listed in `public/images/shots/INDEX.md`. They're rendered at 2x and shown at
half their pixel size.

- **Show the result.** Wherever a page shows a UI result, add a `<Figure>`
  with a real screenshot. If none exists yet, leave a placeholder comment for
  the image pass, naming the file and what it must show:

  ```mdx
  {/* IMAGE: shots/<area>-<name>-dark.png — what it should show */}
  ```

## 7. Getting the facts right

The repository is the source of truth; the website explains it. Before you
write a claim, find it.

1. **Start with the existing guides**: `README.md`, `docs/*.md`,
   `INTEGRATION.md`, `ARCHITECTURE.md`, `CHANGELOG.md`,
   `bindings/imgui/README.md`, `bindings/imgui/blender/README.md`,
   `bindings/rust/bench/README.md`, `SYNTAX_COVERAGE.md`.
2. **Check names against the code**: Python in `bindings/python/bui/`, Rust
   in `bindings/rust/*/src/`, JavaScript in `bindings/node/` and
   `bindings/react/`, the C API in `kit/include/BUI_kit.h`, CMake options in
   `CMakeLists.txt`, Python packaging in `pyproject.toml`.
3. **Prefer runnable examples from `examples/` and the tests.** The tests
   (`bindings/python/tests/`, `bindings/rust/*/tests/`, `tests/`) show real,
   working usage.
4. **List what you checked** in the `sources:` frontmatter. They appear as
   links at the bottom of the page, so readers (and the next writer) can
   verify.
5. **When the docs disagree with the code, the code wins.** Note the
   mismatch in your PR. Known ones so far:
   - `docs/rust.md` says Rust 1.85; `bindings/rust/Cargo.toml` says
     `rust-version = "1.88"`. The site says 1.88.
   - `docs/publishing.md` says Linux wheels use the Tk window; `pyproject.toml`
     now installs the X11 headers for cibuildwheel, so Linux wheels get the
     native host (as `README.md` says).
6. **Don't run heavy builds to check a claim** (no parallel C or Rust
   builds; the maintainers' machine doesn't like it). Read the code instead.

## 8. Planned pages

About 50 pages, grouped by section. **Claim one** by opening an issue or a
draft PR titled `docs: <path>`, then write it. Keep the file path, so links
from other pages keep working. Orders leave gaps for later pages.

Already written: `start/introduction`, `start/installation`,
`tutorials/first-app-python`, `project/roadmap`.

### Tutorials (`src/content/docs/tutorials/`)

| File | Order | Scope |
| --- | --- | --- |
| `tkinter-app.mdx` | 20 | Run an existing tkinter app unchanged (`blenderui-tk`, `install()`, import by name), then use the Blender look (`LabelFrame` as panels, `Spinbox` as number fields). Sources: `docs/tkinter.md`, `examples/tk_unchanged_app.py` |
| `first-app-qt.mdx` | 30 | A small Qt-syntax form: `QApplication`, layouts, signals, a slider driving a progress bar. Be upfront that Qt is partial. Sources: `docs/qt.md`, `examples/qt_syntax_demo.py` |
| `first-app-rust.mdx` | 40 | The `hello.rs` counter in both syntaxes: `rsx!` with a `#[component]`, `use_state` and Tailwind attributes, and gpui builders with state in the view; a `<NumberSlider>`. Sources: `docs/rust.md`, `bindings/rust/blenderui/examples/hello.rs` |
| `first-app-react.mdx` | 50 | A React counter on Node or Bun, signals in JSX, then `bui build` to a 5 MB executable. Sources: `docs/javascript.md`, `bindings/react/examples/counter.jsx` |
| `first-app-imgui.mdx` | 60 | Dear ImGui's demo on BUI from Python and C++, then the Blender-widgets build. Sources: `docs/imgui.md`, `examples/imgui_demo.cpp` |
| `first-app-c.mdx` | 70 | `kit_hello.c` line by line: nodes, properties, the packer, the host callback, the window loop. Sources: `docs/c-api.md`, `examples/kit_hello.c` |

### Python (`src/content/docs/python/`)

| File | Order | Scope |
| --- | --- | --- |
| `tkinter.mdx` | 10 | tkinter compatibility: the three ways in, module map, what you get, the Blender look, behaviour notes, limitations, testing your app against stdlib tkinter. Source: `docs/tkinter.md` |
| `qt-syntax.mdx` | 20 | Qt syntax: how it maps, supported classes, signals, layouts, custom painting with `QPainter`, coverage (31%, from `tools/qt_coverage.py`) and what's missing. Source: `docs/qt.md` |
| `components.mdx` | 30 | The Blender components: `App`, the common API (`value`, `on_change`, `description`, `weight`), layout with `Column`/`Row`/`Panel`/`Split`, menus and pie menus. Source: `docs/components.md` |
| `components-reference.mdx` | 40 | Every component's signature in tables: layout, values, buttons, views, menus, color and editors, node editor and areas. Source: `docs/components.md`, `bindings/python/bui/components/` |
| `headless-testing.mdx` | 50 | Testing Python UIs without a window: `render_raster`, `press`/`motion`/`release`, pixels, frame budgets. Sources: `docs/components.md`, `bindings/python/tests/test_components.py` |

### Rust (`src/content/docs/rust/`)

| File | Order | Scope |
| --- | --- | --- |
| `rsx-and-components.mdx` | 10 | `rsx!` / `view!` markup, control flow in markup, `#[component]` with optional props and `key`, three component styles (zopra, React, Leptos). Source: `docs/rust.md` |
| `hooks.mdx` | 20 | `use_state`, `signal`, `Memo`, `Effect`, context; React, Leptos and zopra spellings; limits (no `use_resource` yet). Source: `docs/rust.md` |
| `html-elements.mdx` | 30 | All 113 HTML elements, what they draw like, `<details>`, lists, tables, inline text. Source: `docs/rust.md#elements` |
| `css-and-tailwind.mdx` | 40 | `<style>` sheets, selectors, the cascade, `var()`, colors, at-rules; Tailwind v4 classes and bare attributes; "did you mean" errors; `tw!`. Source: `docs/rust.md#styling`, `CHANGELOG.md` |
| `styled-and-blender-components.mdx` | 50 | The three looks: plain HTML, `styled`, and Blender components (`Button`, `Checkbox`, `NumberSlider` …); form controls. Sources: `docs/rust.md`, `examples/forms.rs` |
| `layout.mdx` | 55 | Flexbox, grid with named areas, tables, `calc()`, taffy under the hood. Source: `docs/rust.md#layout` |
| `effects-and-animations.mdx` | 60 | `@keyframes`, transitions, filters, `backdrop-filter`, blend modes, `clip-path`, masks, 2D transforms. Source: `docs/rust.md` |
| `events.mdx` | 70 | Listeners, `cx.listener`, closures without parameters, painting your own content. Source: `docs/rust.md#events` |
| `headless-testing.mdx` | 80 | `Application::headless()`, screenshots to PNG, tests. Source: `docs/rust.md#headless-rendering-and-tests` |
| `gpui-syntax.mdx` | 90 | The gpui-compatible app model (`Application`, `Render`, `WindowHandle`, `div().flex()`), compiling gpui code unchanged. Sources: `docs/rust.md#the-app-model`, `bindings/rust/bench/common/settings.rs` |

### JavaScript & React (`src/content/docs/javascript/`)

| File | Order | Scope |
| --- | --- | --- |
| `native-api.mdx` | 10 | `@blender-ui/native`: `App`, windows, `createElement`, attributes, events, `requestAnimationFrame`, headless mode. Source: `docs/javascript.md#plain-javascript` |
| `react.mdx` | 20 | `@blender-ui/react`: `render`, JSX setup (`jsxImportSource`), host components, Blender components, Suspense and transitions. Source: `docs/javascript.md#react` |
| `svelte.mdx` | 25 | `@blender-ui/svelte`: `render`, `register` (Node.js, Bun), elements and events, Blender components, scoped styles, signals as stores, `bui build`. Source: `docs/javascript.md#svelte` |
| `signals.mdx` | 30 | Hooks that hold signals: pass `{count}` to update one node, read `count.value` to re-render; the conformance suite. Source: `docs/javascript.md#hooks-and-signals` |
| `standalone-executables.mdx` | 40 | `bui build` (QuickJS, ~5 MB), `--bun`, `--console`, `--dev`, `bui run`. Source: `docs/javascript.md#standalone-apps` |

### Dear ImGui (`src/content/docs/imgui/`)

| File | Order | Scope |
| --- | --- | --- |
| `cpp.mdx` | 10 | `imgui.h` + `imgui_impl_bui.h`, `ImGui_ImplBUI_CreateApp`, linking `bui::imgui`. Source: `docs/imgui.md#c` |
| `c.mdx` | 20 | `cimgui.h`, all 399 functions, the C app helpers. Source: `docs/imgui.md` |
| `python.mdx` | 30 | `blenderui.imgui`, pyimgui-style returns, `imgui.run`. Source: `docs/imgui.md#python` |
| `rust.mdx` | 40 | The `blenderui-imgui` crate, the safe API and the FFI. Source: `docs/imgui.md#rust` |
| `blender-widgets.mdx` | 50 | `BUI_IMGUI_BLENDER_WIDGETS` / `blender-widgets`: what changes, the mapping table, behaviour differences, gaps. Sources: `bindings/imgui/blender/README.md` |
| `coverage.mdx` | 60 | What's covered per language, what's skipped and why, regenerating the bindings, the Blender style. Sources: `docs/imgui.md#coverage`, `bindings/imgui/README.md` |

### C & C++ (`src/content/docs/c/`)

| File | Order | Scope |
| --- | --- | --- |
| `c-api.mdx` | 10 | The kit API: nodes, properties, geometry managers, the context and host, windows, events. Source: `docs/c-api.md`, `kit/include/BUI_kit.h` |
| `embedding.mdx` | 20 | BUI drawing inside your own renderer: render sinks, the stable C ABI (`api/`), the Qt/QPainter backend, PySide, SVG output. Source: `INTEGRATION.md` |

### Concepts (`src/content/docs/concepts/`)

| File | Order | Scope |
| --- | --- | --- |
| `how-it-works.mdx` | 10 | The layers: your code → bindings → kit → renderer → window. What's ported from Blender. Sources: `README.md#how-it-works`, `ARCHITECTURE.md` |
| `blender-widgets.mdx` | 20 | Blender's widgets and interactions in depth: number-field dragging rules, Ctrl snapping, sliders, multi-field drags, drag-toggle, menus, search, undo. Sources: `docs/components.md#interactions`, `CHANGELOG.md` |
| `rendering.mdx` | 30 | CPU raster vs GPU (Direct3D 11, OpenGL 3.3): turning it on, pixel parity, memory trade-offs, Slug text. Sources: `docs/getting-started.md#gpu-rendering`, `ARCHITECTURE.md` |
| `damage-rendering.mdx` | 40 | Why idle costs nothing: damage rects, tiles, frame pacing. Sources: `README.md`, `ARCHITECTURE.md` |
| `window-hosts.mdx` | 50 | Native Win32 / X11 / Cocoa hosts, the Tk fallback (`BUI_HOST=tk`), headless mode, HiDPI, Linux without X11. Source: `docs/getting-started.md#window-hosts` |
| `text-and-fonts.mdx` | 60 | Inter embedded, FreeType rendering like Blender's BLF, Slug on the GPU, `BUI_FONT`, limits (no fallback, no IME, bidi without shaping). Sources: `CHANGELOG.md`, `README.md` |
| `theming.mdx` | 70 | Blender's theme in BUI: where colors come from (`userdef_default_theme.c`), icons, what you can change today. Verify in `kit/` before writing; mark anything unbuilt with `<ComingSoon>`. |

### Performance (`src/content/docs/performance/`)

| File | Order | Scope |
| --- | --- | --- |
| `benchmarks.mdx` | 10 | The published results with their context: start time and battery, frame times, Rust vs gpui, JavaScript startup and animation. Source: `README.md#performance` |
| `run-benchmarks.mdx` | 20 | How to rerun every benchmark, one at a time. Sources: `README.md#run-the-benchmarks-yourself`, `bindings/rust/bench/README.md` |
| `profiling.mdx` | 30 | `BUI_PROFILE=1`, choosing a renderer, keeping lists on signals, common slow paths. Sources: `README.md`, `docs/javascript.md#performance` |

### Reference (`src/content/docs/reference/`)

| File | Order | Scope |
| --- | --- | --- |
| `environment-variables.mdx` | 10 | Every `BUI_*` variable, what it does, defaults. Source: `docs/getting-started.md#environment-variables` |
| `cmake-options.mdx` | 20 | Every CMake option with default and effect. Sources: `docs/getting-started.md#build-options`, `CMakeLists.txt` |
| `platforms.mdx` | 30 | The full platform matrix and per-platform notes. Source: `README.md#platforms` |
| `coverage.mdx` | 40 | Coverage numbers in one place: tkinter, Qt, HTML, CSS, Tailwind, ImGui. Sources: `SYNTAX_COVERAGE.md`, `docs/rust.md#coverage`, `docs/qt.md#coverage` |
| `glossary.mdx` | 50 | Kit, binding, node, host, render sink, damage rendering, raster, Slug, signal, `styled` … one or two sentences each. |

### Project (`src/content/docs/project/`)

| File | Order | Scope |
| --- | --- | --- |
| `faq.mdx` | 20 | The README FAQ, expanded: Blender not needed, unchanged tkinter, not a browser, real ImGui, vs Electron, macro cost, why GPL. Source: `README.md#faq` |
| `contributing.mdx` | 30 | How to help: issues, PRs, tests, the code of conduct, security reports, writing docs (link this guide). Sources: `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `SECURITY.md` |
| `publishing.mdx` | 40 | Releasing to PyPI, npm and crates.io (for maintainers). Source: `docs/publishing.md` |
| `changelog.mdx` | 50 | A readable summary of `CHANGELOG.md` with a link to the full file; don't copy it wholesale. |
| `license-and-credits.mdx` | 60 | GPL-3.0-or-later (like Blender) and why, per-file headers, third-party licenses, acknowledgments. Source: `README.md#license` |

## 9. Before you open a PR

- [ ] Frontmatter complete: `title`, `description`, `section`, `order`, `sources`.
- [ ] A hook in the first lines and next-step cards. No `<Recap>`, no emoji.
- [ ] Per-language page: no comparisons with other languages.
- [ ] Rust page: every UI example in both `rsx!` and gpui builders.
- [ ] Demo tricks mentioned at most once; typed math only for number fields.
- [ ] A `<Figure>` (or an `IMAGE:` placeholder) wherever a UI result is shown.
- [ ] Every number and name checked against the repository.
- [ ] Unbuilt features marked with `<ComingSoon>` (and `status` if the whole page is).
- [ ] Code examples are complete and runnable as shown.
- [ ] At most one joke.
- [ ] `pnpm build` passes with no warnings; the page looks right in light
      and dark, on a phone and a desktop.
