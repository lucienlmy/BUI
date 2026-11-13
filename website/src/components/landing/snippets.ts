// The landing page's "Write it the way you already write UI" picker. Every snippet is
// taken from the repository's README or docs (see `source`), so it matches what BUI runs.
//
// Screenshots: `shot` names a light/dark pair in public/images/shots/ (<shot>-light.png and
// <shot>-dark.png, listed in public/images/shots/INDEX.md). The screenshot is a fuller app in
// the same syntax as the snippet; `caption` names its source. `fallback` is only used if a
// pair goes missing; see src/lib/shots.ts.

export interface Syntax {
  id: string;
  /** The syntax switcher's label. */
  label: string;
  title: string;
  blurb: string;
  lang: string;
  file: string;
  code: string;
  install: string;
  shot: string;
  fallback: string;
  alt: string;
  caption: string;
  docs: string;
  source: string;
}

export interface Language {
  id: string;
  tab: string;
  icon: string;
  syntaxes: Syntax[];
}

const tkinter: Syntax = {
  id: "tkinter",
  label: "tkinter",
  title: "Your tkinter app, unchanged",
  blurb:
    "Every public tkinter and ttk name is there: 7,846 of 7,846. Run an existing program with blenderui-tk and it wakes up looking like Blender.",
  lang: "py",
  file: "greet.py",
  code: `import blenderui.tksyntax as tk
from blenderui.tksyntax import ttk, messagebox

root = tk.Tk()
name = tk.StringVar(value="Cube")
ttk.Entry(root, textvariable=name).pack(fill="x", padx=8, pady=8)
ttk.Button(root, text="Greet",
           command=lambda: messagebox.showinfo("Hi", "Hello " + name.get())).pack()
root.mainloop()`,
  install: "pip install blenderui && blenderui-tk my_app.py",
  shot: "python-tkinter",
  fallback: "images/shots/python-tkinter-dark.png",
  alt: "An unmodified tkinter program on BUI: a glTF export dialog with entries, check and radio buttons, a spinbox, an option menu, a scale and a listbox, drawn with Blender's widgets",
  caption: "examples/site/tk_export.py, unmodified tkinter",
  docs: "docs/python/tkinter/",
  source: "README.md",
};

const qt: Syntax = {
  id: "qt",
  label: "Qt syntax",
  title: "Qt-style Python, no Qt required",
  blurb: "QWidget, layouts, signals and clicked.connect, drawn by BUI. The everyday subset works today; more is on the way.",
  lang: "py",
  file: "form.py",
  code: `from blenderui.qtsyntax import (QApplication, QMainWindow, QWidget, QVBoxLayout,
                                QFormLayout, QLineEdit, QSlider, QProgressBar, Qt)

app = QApplication([])
win = QMainWindow()
central = QWidget()
layout = QVBoxLayout(central)

form = QFormLayout()
level = QSlider(Qt.Horizontal)
level.setRange(0, 100)
bar = QProgressBar()
level.valueChanged.connect(bar.setValue)   # signals connect to any callable
form.addRow("Name:", QLineEdit())
form.addRow("Level:", level)
form.addRow("Progress:", bar)
layout.addLayout(form)

win.setCentralWidget(central)
win.show()
app.exec()`,
  install: "pip install blenderui",
  shot: "python-qt",
  fallback: "images/shots/python-qt-dark.png",
  alt: "A Qt-syntax material editor on BUI: a list, a line edit, a group box with a form layout, a combo box, a slider with a spin box, a check box and a progress bar",
  caption: "examples/site/qt_material.py",
  docs: "docs/python/qt-syntax/",
  source: "docs/qt.md",
};

const rsx: Syntax = {
  id: "rsx",
  label: "rsx!",
  title: "HTML, CSS and Tailwind, natively",
  blurb:
    "rsx!, components and hooks, all 113 HTML elements and modern CSS. Add <NumberSlider> and you get Blender's real slider. Typos in Tailwind classes fail to compile, politely.",
  lang: "rust",
  file: "src/main.rs",
  code: `use blenderui::prelude::*;

struct Hello;

impl Render for Hello {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        rsx! {
            <body m-0 p-6 flex flex-col gap-4 bg-zinc-900 text-white>
                <h1 text-2xl font-bold>"Hello, BUI"</h1>
                <NumberSlider min={0.0} max={10.0} value="5" />   // a real Blender slider
            </body>
        }
    }
}

fn main() {
    Application::new().run(|cx| {
        cx.open_window(WindowOptions::default(), |_, cx| cx.new(|_| Hello)).unwrap();
    });
}`,
  install: "cargo add blenderui",
  shot: "rust-rsx-dashboard",
  fallback: "images/shots/rust-rsx-dashboard-dark.png",
  alt: "A render-farm dashboard written with rsx!: a CSS grid of stat cards, job rows with Blender progress bars, a node list, a Blender dropdown, button and checkbox",
  caption: "examples/site_dashboard.rs, rsx!",
  docs: "docs/rust/rsx-and-components/",
  source: "README.md",
};

const gpui: Syntax = {
  id: "gpui",
  label: "gpui builders",
  title: "gpui code compiles unchanged",
  blurb:
    "The same file builds against Zed's gpui, gpui-fast and blenderui. This is the benchmark's settings window: on blenderui it shows its first frame in 40 ms instead of 231 ms.",
  lang: "rust",
  file: "settings.rs",
  code: `impl Render for Settings {
    fn render(&mut self, window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        let field = |label: &'static str| {
            div().flex().items_center().gap(px(8.0))
                .child(div().w(px(70.0)).text_color(rgb(0xb8b8b8)).child(label))
                .child(div().flex_1().h(px(20.0)).px(px(6.0)).rounded(px(4.0)).bg(rgb(0x1d1d1d))
                    .border_1().border_color(rgb(0x3d3d3d)).text_color(rgb(0xe6e6e6)).child("Cube"))
        };
        div()
            .size_full().flex().flex_col().gap(px(8.0)).p(px(12.0))
            .bg(rgb(0x303030)).text_color(rgb(0xe6e6e6)).text_size(px(13.0))
            .child(div().text_size(px(16.0)).child("Settings"))
            .child(field("Name"))
            .child(div().w(px(80.0)).h(px(22.0)).rounded(px(4.0)).bg(rgb(0x545454))
                .flex().items_center().justify_center().child("Apply"))
    }
}`,
  install: "cargo add blenderui",
  shot: "rust-builder-dashboard",
  fallback: "images/shots/rust-builder-dashboard-dark.png",
  alt: "The same render-farm dashboard written with gpui-style builders, drawn with identical pixels",
  caption: "examples/site_dashboard_builder.rs, gpui builders",
  docs: "docs/rust/gpui-syntax/",
  source: "bindings/rust/bench/common/settings.rs",
};

const htmlBuilder: Syntax = {
  id: "html",
  label: "HTML builder",
  title: "Plain HTML elements, as functions",
  blurb:
    "One constructor per HTML element (h2(), ul(), input(), button() …) and gpui's app model. Elements with an id keep their state across renders; cx.notify() re-renders.",
  lang: "rust",
  file: "src/todo.rs",
  code: `use blenderui::prelude::*;

struct Todo { items: Vec<String>, draft: String }

impl Render for Todo {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div().p_4().flex().flex_col().gap_2()
            .child(h2().child(format!("{} items", self.items.len())))
            .child(ul().children(self.items.iter().map(|s| li().child(s.clone()))))
            .child(div().flex().gap_2()
                .child(input().id("draft").placeholder("New item").value(self.draft.clone())
                    .on_change(cx.listener(|this, e: &ChangeEvent, _, cx| {
                        this.draft = e.value.clone();
                        cx.notify();
                    })))
                .child(button().child("Add").on_click(cx.listener(|this, _, _, cx| {
                    this.items.push(std::mem::take(&mut this.draft));
                    cx.notify();
                }))))
    }
}`,
  install: "cargo add blenderui",
  shot: "rust-html-plain",
  fallback: "images/shots/rust-html-plain-dark.png",
  alt: "An export form and a table built from plain HTML element functions, with a browser's default look",
  caption: "examples/site_plain_html.rs, plain HTML",
  docs: "docs/rust/html-elements/",
  source: "docs/rust.md",
};

const react: Syntax = {
  id: "react",
  label: "React (JSX)",
  title: "React, without a browser",
  blurb:
    "React 19.3 renders straight to BUI on Node.js or Bun. Hooks keep React's names but hold signals, so a change can update one node without a re-render.",
  lang: "jsx",
  file: "app.jsx",
  code: `/** @jsxImportSource @blender-ui/react */
import { render, useState, NumberSlider } from "@blender-ui/react"

function Counter() {
  const [count, setCount] = useState(0)
  return (
    <main styled className="p-6 flex flex-col gap-3">
      <span>Clicks: {count}</span>
      <button onClick={() => setCount((c) => c + 1)}>Add one</button>
      <NumberSlider min={0} max={10} />
    </main>
  )
}

render(<Counter />, { title: "Counter", width: 360, height: 200, gpu: true })`,
  install: "npm install @blender-ui/react react",
  shot: "js-react-tasks",
  fallback: "images/shots/js-react-tasks-dark.png",
  alt: "A task list in React on BUI: a Blender text field and button, checkboxes and a progress bar",
  caption: "examples/site/js/todo_react.js, React on BUI",
  docs: "docs/javascript/react/",
  source: "README.md",
};

const svelte: Syntax = {
  id: "svelte",
  label: "Svelte",
  title: "Svelte 5, in a native window",
  blurb:
    "Runes, snippets, bindings and scoped styles, unchanged. Svelte's compiled components run on BUI's DOM, so a $state change updates the one node that shows it.",
  lang: "svelte",
  file: "Counter.svelte",
  code: `<script>
  import { Button, NumberSlider } from "@blender-ui/svelte"
  let count = $state(0)
</script>

<main styled class="p-6 flex flex-col gap-3">
  <span>Clicks: {count}</span>
  <Button onclick={() => count++}>Add one</Button>
  <NumberSlider min={0} max={10} />
</main>

<!-- app.js: render(Counter, { title: "Counter", gpu: true }) -->`,
  install: "npm install @blender-ui/svelte svelte",
  shot: "js-svelte-tasks",
  fallback: "images/shots/js-svelte-tasks-dark.png",
  alt: "A task list in Svelte on BUI: a Blender text field and button, checkboxes and a progress bar",
  caption: "examples/site/js/TodoSvelte.svelte, Svelte on BUI",
  docs: "docs/javascript/svelte/",
  source: "bindings/svelte/README.md",
};

const vue: Syntax = {
  id: "vue",
  label: "Vue",
  title: "Vue 3, in a native window",
  blurb:
    "Refs, v-model, slots and scoped styles, unchanged. Vue's own runtime and compiler run through a custom renderer over BUI's DOM, so a ref change updates the nodes that show it.",
  lang: "vue",
  file: "Counter.vue",
  code: `<script setup>
import { ref } from "vue"
import { Button, NumberSlider } from "@blender-ui/vue"
const count = ref(0)
const size = ref(2)
</script>

<template>
  <main styled class="p-6 flex flex-col gap-3">
    <span>Clicks: {{ count }}</span>
    <Button @click="count++">Add one</Button>
    <NumberSlider v-model="size" :min="0" :max="10" />
  </main>
</template>

<!-- app.js: render(Counter, { title: "Counter", gpu: true }) -->`,
  install: "npm install @blender-ui/vue vue",
  shot: "js-vue-tasks",
  fallback: "images/shots/js-vue-tasks-dark.png",
  alt: "A task list in Vue on BUI: a Blender text field and button, checkboxes and a progress bar",
  caption: "examples/site/js/TodoVue.vue, Vue on BUI",
  docs: "docs/javascript/vue/",
  source: "bindings/vue/README.md",
};

const dom: Syntax = {
  id: "dom",
  label: "DOM-like API",
  title: "A DOM you already know, minus the browser",
  blurb:
    "createElement, appendChild, addEventListener and className with Tailwind. Any attribute can be a signal: bind it once and the node follows.",
  lang: "js",
  file: "app.js",
  code: `const { App, signal } = require('@blender-ui/native')

const app = new App({ gpu: true })
const win = app.window({ title: 'Hello', width: 360, height: 220 })
win.body.setAttribute('styled', '') // Blender's look for the page
win.body.className = 'm-0 p-6 flex flex-col gap-3 bg-[#2b2b2b] text-[#e6e6e6]'

const clicks = signal(0)
const label = win.createElement('span')
label.textContent = clicks // bound: follows the signal
const button = win.createElement('button')
button.textContent = 'Add one'
button.addEventListener('click', () => clicks.value++)
win.body.appendChild(label)
win.body.appendChild(button)`,
  install: "npm install @blender-ui/native",
  shot: "js-dom-tasks",
  fallback: "images/shots/js-dom-tasks-dark.png",
  alt: "The same task list built with createElement, appendChild and addEventListener",
  caption: "examples/site/js/todo_dom.js, @blender-ui/native",
  docs: "docs/javascript/native-api/",
  source: "docs/javascript.md",
};

const imguiCpp: Syntax = {
  id: "cpp",
  label: "C++",
  title: "The real Dear ImGui, in Blender's clothes",
  blurb:
    "Existing ImGui code compiles unchanged. Flip one build option (BUI_IMGUI_BLENDER_WIDGETS) and DragFloat becomes Blender's drag-to-scrub field.",
  lang: "cpp",
  file: "imgui_demo.cpp",
  code: `#include "imgui.h"
#include "imgui_impl_bui.h"

static void frame(void*) { ImGui::ShowDemoWindow(); }

int main() {
  ImGui::CreateContext();
  ImGui_ImplBUI_App* app = ImGui_ImplBUI_CreateApp("Dear ImGui on BUI", 1280, 800, frame, nullptr);
  ImGui_ImplBUI_RunApp(app);       // BUI's message loop until the window closes
  ImGui_ImplBUI_DestroyApp(app);
  ImGui::DestroyContext();
}`,
  install: "cmake -S . -B build -DBUI_IMGUI_BLENDER_WIDGETS=ON && cmake --build build",
  shot: "imgui-cpp-blender",
  fallback: "images/shots/imgui-cpp-blender-dark.png",
  alt: "A Dear ImGui scene inspector drawn with Blender's widgets and style: a table, drag fields, a color edit, sliders, a combo, checkboxes, a progress bar and a plot",
  caption: "examples/site/imgui_inspector.cpp, BUI_IMGUI_BLENDER_WIDGETS=ON",
  docs: "docs/imgui/cpp/",
  source: "docs/imgui.md",
};

const imguiC: Syntax = {
  id: "c",
  label: "C",
  title: "All 399 functions, from C",
  blurb: "cimgui conventions: ImGui::Button is ImGui_Button, references become pointers, every struct keeps its C++ layout.",
  lang: "c",
  file: "imgui_demo_c.c",
  code: `#include "cimgui.h"
#include "cimgui_impl_bui.h"

static void frame(void* user)
{
    float* value = (float*)user;
    ImGui_ShowDemoWindow();
    ImGui_Begin("Hello");
    ImGui_SliderFloat("Value", value, 0.0f, 1.0f);
    ImGui_Text("%.2f", *value);
    ImGui_End();
}

int main(void)
{
    float value = 0.5f;
    ImGui_CreateContext();
    cImGui_ImplBUI_App* app = cImGui_ImplBUI_CreateApp("Hello", 1280, 800, frame, &value, NULL);
    cImGui_ImplBUI_RunApp(app);
    cImGui_ImplBUI_DestroyApp(app);
    ImGui_DestroyContext();
}`,
  install: "cmake -S . -B build && cmake --build build --target bui_imgui_demo_c",
  shot: "imgui-cpp-stock",
  fallback: "images/shots/imgui-cpp-stock-dark.png",
  alt: "The same Dear ImGui scene inspector with ImGui's own widgets and its StyleColorsDark / StyleColorsLight colors",
  caption: "ImGui's own widgets and colors (examples/site/imgui_inspector.cpp --stock)",
  docs: "docs/imgui/c/",
  source: "docs/imgui.md",
};

const imguiPy: Syntax = {
  id: "python",
  label: "Python",
  title: "pyimgui-style Python",
  blurb: "snake_case functions; an in/out value comes back after the result: changed, v = imgui.slider_float(…).",
  lang: "py",
  file: "hello_imgui.py",
  code: `import blenderui.imgui as imgui

state = {"value": 0.5}

def frame():
    imgui.show_demo_window()
    imgui.begin("Hello")
    _, state["value"] = imgui.slider_float("Value", state["value"], 0.0, 1.0)
    imgui.end()

imgui.run(frame, title="Hello", size=(1280, 800))`,
  install: "pip install blenderui",
  shot: "imgui-python-blender",
  fallback: "images/shots/imgui-python-blender-dark.png",
  alt: "The Dear ImGui scene inspector from Python, with Blender's widgets and style",
  caption: "examples/site/imgui_inspector.py, blenderui.imgui",
  docs: "docs/imgui/python/",
  source: "docs/imgui.md",
};

const imguiRust: Syntax = {
  id: "rust",
  label: "Rust",
  title: "A safe Rust API, and the raw FFI",
  blurb: "&mut values for in/out arguments, &str for strings, slices for (pointer, count) pairs. imgui::sys has every C function.",
  lang: "rust",
  file: "src/main.rs",
  code: `use blenderui_imgui as imgui;

fn main() {
    let mut value = 0.5f32;
    imgui::run("Hello", [1280, 800], move || {
        imgui::show_demo_window();
        imgui::begin("Hello");
        imgui::slider_float("Value", &mut value, 0.0, 1.0);
        if imgui::button("Reset") {
            value = 0.5;
        }
        imgui::text(&format!("value {value:.2}"));
        imgui::end();
    });
}`,
  install: "cargo run -p blenderui-imgui --example demo",
  shot: "imgui-python-stock",
  fallback: "images/shots/imgui-python-stock-dark.png",
  alt: "The Dear ImGui scene inspector with ImGui's own widgets and colors",
  caption: "The scene inspector, ImGui's own style (examples/site/imgui_inspector.py)",
  docs: "docs/imgui/rust/",
  source: "docs/imgui.md",
};

const c: Syntax = {
  id: "kit",
  label: "C kit",
  title: "The kit itself, in C",
  blurb: "Everything above sits on one C runtime. Build a node tree, set properties, and open a native window.",
  lang: "c",
  file: "hello.c",
  code: `#include "BUI_kit.h"

int main(void)
{
  BKNode *root = BK_node_new("frame");
  BKNode *size = BK_node_new("slider");
  BK_set_str(size, "label", "Size");
  BK_set_num(size, "max", 10);
  BK_set_num(size, "value", 2.5);
  BK_node_add(root, size, -1);              /* -1: append */
  BK_geom_set_manager(size, BK_GEOM_PACK);  /* Tk's packer */

  BKContext *ctx = BK_context_new(root);
  BKHost host = {0};                        /* the window fills it in */
  BK_context_set_host(ctx, &host);
  BKWindow *win = BK_window_new(ctx, "BUI from C", 320, 120);
  BK_window_show(win);
  BK_window_run();
  return 0;
}`,
  install: "cmake -S . -B build && cmake --build build",
  shot: "c-kit-app",
  fallback: "images/shots/c-kit-app-dark.png",
  alt: "A modifier stack from the C kit: panels, enum rows, number fields, dropdowns, a slider, checkboxes and buttons",
  caption: "examples/site/kit_modifiers.c, the C kit",
  docs: "docs/c/c-api/",
  source: "docs/c-api.md",
};

export const languages: Language[] = [
  { id: "python", tab: "Python", icon: "file-py", syntaxes: [tkinter, qt] },
  { id: "rust", tab: "Rust", icon: "file-rs", syntaxes: [rsx, gpui, htmlBuilder] },
  { id: "javascript", tab: "JavaScript", icon: "file-js", syntaxes: [react, svelte, vue, dom] },
  { id: "imgui", tab: "Dear ImGui", icon: "squares-four", syntaxes: [imguiCpp, imguiC, imguiPy, imguiRust] },
  { id: "c", tab: "C", icon: "file-c", syntaxes: [c] },
];
