# Site screenshots

Every image here exists twice: `-dark.png` (Blender's default theme) and
`-light.png` (Blender's Light preset, `BUI_THEME=light`). All are rendered
off-screen at 2x (`BUI_UI_SCALE=2`) and recompressed losslessly
(`tools/pngopt.py`). Regenerate them all with one command:

```sh
BUIPY_BUILD_DIR=<build>/bindings/python BUI_BUILD_DIR=<build> python tools/gen_doc_images.py --site
#   --only python|rust|js|imgui|c   one producer;   --theme dark|light   one theme
```

The CMake build needs `BUI_BUILD_EXAMPLES=ON`, `BUI_BUILD_IMGUI=ON` and the
Python bindings; the JavaScript shots need the addon (`npm run build` in
bindings/node).

## Python

| Image | Shows | Source |
| --- | --- | --- |
| `python-components-{dark,light}.png` | Blender components: a Render Properties editor (top bar menus, side tabs, panels and sub-panels with header checkboxes, number fields, sliders, dropdowns, enum row, color swatch, progress bar, status line) | `examples/site/render_props.py` |
| `python-tkinter-{dark,light}.png` | An unchanged tkinter program (classic Tk widgets): a glTF export dialog with Entry, Button, LabelFrame, Checkbutton, Radiobutton, Spinbox, OptionMenu, Scale and Listbox | `examples/site/tk_export.py` |
| `python-ttk-{dark,light}.png` | An unchanged tkinter program (ttk widgets): a render-farm queue with Combobox, Spinbox, Notebook, Treeview, Progressbar, Checkbutton | `examples/site/ttk_queue.py` |
| `python-qt-{dark,light}.png` | Qt syntax: a material editor with QListWidget, QLineEdit, QGroupBox + QFormLayout, QComboBox, QSlider + QDoubleSpinBox, QCheckBox, QProgressBar | `examples/site/qt_material.py` |
| `imgui-python-stock-{dark,light}.png` | Dear ImGui from Python with ImGui's own widgets and its StyleColorsDark / StyleColorsLight: a scene inspector (table, drag fields, color edit, sliders, combo, checkboxes, progress bar, plot) | `examples/site/imgui_inspector.py` |
| `imgui-python-blender-{dark,light}.png` | The same inspector with Blender widgets and Blender's style (following the theme) | `examples/site/imgui_inspector.py` |

## Rust

| Image | Shows | Source |
| --- | --- | --- |
| `rust-rsx-dashboard-{dark,light}.png` | A render-farm dashboard in `rsx!`: CSS grid of stat cards, job rows with Blender progress bars, node list, a Blender dropdown, button and checkbox, inside `styled` | `bindings/rust/blenderui/examples/site_dashboard.rs` (+ `site/dashboard_data.rs`) |
| `rust-builder-dashboard-{dark,light}.png` | The same dashboard in gpui's builder syntax (identical pixels) | `bindings/rust/blenderui/examples/site_dashboard_builder.rs` |
| `rust-html-plain-{dark,light}.png` | Plain HTML: a form and a table with a browser's default look (the same in both themes) | `bindings/rust/blenderui/examples/site_plain_html.rs` (+ `site/form_page.rs`) |
| `rust-html-styled-{dark,light}.png` | The same markup inside `styled`: Blender's theme and widgets | `bindings/rust/blenderui/examples/site_styled.rs` |
| `rust-components-{dark,light}.png` | Blender components from Rust (`NumberField`, `NumberSlider`, `Dropdown`, `Checkbox`, `TextField`, `ColorSwatch`, `ProgressBar`, `Button`): camera properties | `bindings/rust/blenderui/examples/site_components.rs` |

## JavaScript

| Image | Shows | Source |
| --- | --- | --- |
| `js-dom-tasks-{dark,light}.png` | A task list with the native DOM-like API (createElement, appendChild, addEventListener), Blender TextField / Button / Checkbox / ProgressBar | `examples/site/js/todo_dom.js` (+ `todo_style.js`; shot by `shoot.mjs`) |
| `js-react-tasks-{dark,light}.png` | The same task list in React (@blender-ui/react, hooks) | `examples/site/js/todo_react.js` |
| `js-svelte-tasks-{dark,light}.png` | The same task list in Svelte 5 (@blender-ui/svelte, runes); pixel-identical to the React shot | `examples/site/js/TodoSvelte.svelte` (+ `todo_svelte.mjs`) |
| `js-vue-tasks-{dark,light}.png` | The same task list in Vue 3 (@blender-ui/vue, refs and v-model) | `examples/site/js/TodoVue.vue` (+ `todo_vue.mjs`) |

## Lua

| Image | Shows | Source |
| --- | --- | --- |
| `lua-editor-{dark,light}.png` | The interactive Lua table editor: tree view with three columns, toolbar with demo selector, search, add/delete/export/undo/redo buttons, collapsible editor panel | `bindings/lua/examples/editor_screenshot.lua` |

## C / C++

| Image | Shows | Source |
| --- | --- | --- |
| `c-kit-app-{dark,light}.png` | The C kit API: a modifier stack (panels, enum rows, number fields, dropdowns, slider, checkboxes, buttons) | `examples/site/kit_modifiers.c` (target `bui_site_kit_app`) |
| `imgui-cpp-stock-{dark,light}.png` | Dear ImGui from C++, ImGui's own widgets and colors: the scene inspector | `examples/site/imgui_inspector.cpp --stock` (target `bui_site_imgui`) |
| `imgui-cpp-blender-{dark,light}.png` | The same with Blender widgets and Blender's style | `examples/site/imgui_inspector.cpp` |

## Features

| Image | Shows | Source |
| --- | --- | --- |
| `feature-menu-search-{dark,light}.png` | The File menu with `exp` typed: Blender's fuzzy menu search through submenus | `examples/site/render_props.py` (`tools/site_shots_py.py`) |
| `feature-pie-menu-{dark,light}.png` | A pie menu (Shading) open over a node editor, the pointer toward "Rendered" | `examples/site/geometry_nodes.py` |
| `feature-tooltip-{dark,light}.png` | A tooltip on the Denoise dropdown | `examples/site/render_props.py` |
| `feature-color-picker-{dark,light}.png` | The color picker of a swatch (HSV wheel, value strip, HSV sliders, hex field, eyedropper) | `examples/site/render_props.py` |
| `feature-node-editor-{dark,light}.png` | A Geometry Nodes editor: scatter instances on a surface (7 nodes, typed sockets, links, header menus) | `examples/site/geometry_nodes.py` |
| `feature-html-effects-{dark,light}.png` | CSS effects on a gradient landscape: filters (grayscale, sepia, hue-rotate, blur, drop-shadow), clip-path (circle, polygon, star), transforms (rotate, skew, scale), mix-blend-mode (multiply, screen, difference), backdrop-filter | `bindings/rust/blenderui/examples/site_effects.rs` |
| `feature-css-grid-{dark,light}.png` | CSS grid: a material library with `repeat(auto-fill, minmax())`, row/column spans and `grid-auto-flow: dense` | `bindings/rust/blenderui/examples/site_grid.rs` |
| `feature-animation-{dark,light}.png` | CSS `@keyframes`: three moments (0.25 s, 0.6 s, 1.0 s) of a spinner, a progress sweep, a bouncing ball and a fading toast, side by side | `bindings/rust/blenderui/examples/site_animation.rs` |
