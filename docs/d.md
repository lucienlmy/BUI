# D: a DUB package with wrappers and the raw kit

The `bindings/d` package gives D programs BUI's widgets and interactions. It is
a normal DUB package with three layers: raw `extern(C)` declarations that match
the C headers one to one, D classes and delegates that make the common paths
feel like D, and an HTML/CSS/RSX authoring layer.

- [Install](#install)
- [A first window](#a-first-window)
- [Headless rendering](#headless-rendering)
- [HTML, CSS and RSX](#html-css-and-rsx)
- [The package](#the-package)
- [Nodes and properties](#nodes-and-properties)
- [Events](#events)
- [Ownership and lifetimes](#ownership-and-lifetimes)
- [Menus](#menus)
- [Images, video and canvas](#images-video-and-canvas)
- [Trees, text areas and custom nodes](#trees-text-areas-and-custom-nodes)
- [Raw declarations](#raw-declarations)
- [Building and testing](#building-and-testing)
- [Platform notes](#platform-notes)

## Install

You need a D compiler with DUB, plus CMake and a C compiler. The binding is
tested with **LDC 1.43** (DMD 2.113 front end). On Windows, install the Visual
Studio C++ tools; the build script finds CMake through Visual Studio when it is
not on `PATH`.

From the repository:

```sh
cd bindings/d
dub run --config=hello
```

In another DUB project, use a path dependency:

```json
{
    "name": "my-app",
    "targetType": "executable",
    "dependencies": {
        "bui": { "path": "../BUI/bindings/d" }
    }
}
```

The first build runs CMake and builds the C kit into `bindings/d/.bui/`.
Later builds reuse it. Set `BUI_SOURCE_DIR` to point at another BUI checkout,
and `BUI_CMAKE_GENERATOR` to choose a generator such as `"Ninja"`.

## A first window

```d
import bui;
import std.stdio;

void main() {
    auto root = new Node("frame");
    root.set("padding", 8);

    auto size = new Node("slider");
    size.set("label", "Size");
    size.set("min", 0);
    size.set("max", 10);
    size.set("value", 2.5);
    size.onChange((NodeEvent e) {
        writeln("size = ", e.node.num("value"));
    });
    root.add(size);
    size.pack("fill", 1);

    auto apply = new Node("button");
    apply.set("text", "Apply");
    apply.onClick((NodeEvent) {
        writeln("clicked");
        quit();
    });
    root.add(apply);
    apply.pack("fill", 1).pack("pady0", 6);

    auto context = new Context(root);
    auto window = new Window(context, "BUI from D", 320, 120);
    window.show();
    run();

    window.free();
    context.free();
    root.free();
}
```

`new Node("frame")` creates a node in the kit's retained tree. `pack`, `grid`
and `place` select Tk's geometry managers; `set`, `num`, `str`, `color`,
`setStrs` and `strs` read and write properties by name.

`Context` owns input state, focus, popups and undo. `Window` is one native
top-level window that drives a context. `run` enters the platform message loop;
`quit` leaves it.

## Headless rendering

No window is required. A `Raster` is a frame buffer, and `Context.render`
draws the tree into it:

```d
auto root = new Node("column");
auto button = new Node("button");
button.set("text", "Click me");
button.onClick((NodeEvent) { ++clicks; });
root.add(button);

auto context = new Context(root);
auto raster = new Raster(320, 200);
const regions = context.render(raster, true);

const r = button.rect();
context.press(r[0] + r[2] / 2, r[1] + r[3] / 2, 0);
context.release(r[0] + r[2] / 2, r[1] + r[3] / 2, 0);
```

`render` returns the number of damaged regions it redrew. Pass `full = true`
to force a complete frame. `needsRender` and `animating` tell you whether
anything still needs a frame. The same input methods (`press`, `release`,
`motion`, `wheel`, `scroll`, `key`, `keyRelease`, `contextMenu`, `button`)
drive widgets without a platform window, which is how `tests/run.d` works.

## HTML, CSS and RSX

`bui.html` builds an `Element` tree, `bui.css` cascades a CSS subset over it,
and `bui.rsx` lowers markup to the same `Element` calls at compile time:

```d
import bui;
import std.conv : to;

int count;
Element counter;

auto page = mixin(rsx!`
    <body class="app">
        <style>
            .app { background: #1e1e1e; padding: 12px; color: #e6e6e6; }
            .row { display: flex; flex-direction: row; gap: 6px; }
            button:hover { color: #8fd4ff; }
        </style>
        <h1>BUI RSX</h1>
        <p id="count">Count: {count}</p>
        <div class="row">
            <button onclick={() { ++count; counter.setText("Count: " ~ to!string(count)); }}>Add</button>
        </div>
    </body>
`);

auto document = new Document(page);
counter = document.find("#count");

auto context = new Context(document.node());
```

The hand-written form uses the tag constructors and chainable methods:
`body_(div(h1("Hello"), p("text")).class_("card"))`. `Element` has `id`,
`class_`, `attr`, `style`, `text`, `setText`, `child`, `append` and the event
methods (`onClick`, `onChange`, ...).

`Document` combines the user-agent sheet, the document's `<style>` elements and
an optional CSS string, then realises the tree:

- block boxes and text become frames and labels;
- `display: flex` maps to a pack row or column; `display: grid` maps to a frame
  grid;
- `padding`, `margin`, `gap`, `width`, `height`, `flex`, colours, borders,
  fonts, `text-align`, `cursor`, `visibility` and `object-fit` map to kit
  properties;
- tables become a frame grid with captions above it;
- `input`, `select`, `textarea`, `button`, `progress` and `meter` become their
  kit controls; `img`, `canvas` and `video` become replaced nodes.

Type, `#id`, `.class`, attribute and combinator selectors work, including
dynamic states such as `:hover` and `:active`, with standard specificity and
inline-style precedence. Custom properties, `var()`, `em`, `rem`, `px`, `vw`
and `vh` resolve.

Tag constructors keep HTML names except `body_`, `template_`, `object_` and
`select_`; the first three collide with D and `bui.tree.select` already exists.
Markup always uses the HTML spelling: `<select>` inside `rsx!` needs no
underscore.

`rsx!` handles attributes, `{expression}` text and values, event delegates,
boolean attributes, entities, comments, void tags, raw `<style>` text and
custom elements. `class={names}` and `hidden={flag}` toggle through
`flagAttr`. `Document.restyle` re-runs the cascade for visual properties after
a state change; `Element.setText` updates a realised text leaf in place. Layout
structure is fixed when the document is built, so a different structure needs a
new `Document`.

The CSS subset is smaller than the Rust engine's taffy layout: no flex
wrapping, media queries, pseudo-elements, animations or Tailwind, and text does
not wrap around floats.

## The package

| Module          | What it covers                                                                 |
| --------------- | ------------------------------------------------------------------------------ |
| `bui.node`      | `Node`, properties, geometry, events, images, video, ramps, curves, histograms |
| `bui.context`   | `Context`, `Host`, input, rendering, popups, menus, undo, clipboard, IME       |
| `bui.window`    | `Window`, `run`, `pump`, `quit`, timers, focus, window-manager requests         |
| `bui.menu`      | `MenuItem` descriptions for context, pie and menu-button menus                  |
| `bui.raster`    | `Raster`: drawing, alpha, damage, text, textures, GPU attach                    |
| `bui.image`     | `Image` decoding, generated images, probes, `Animation`                        |
| `bui.video`     | `Video` playback, seeking, listeners and frame textures                        |
| `bui.canvas`    | `CanvasItem` display lists on `canvas` nodes                                   |
| `bui.tree`      | `TreeItem` rows and the `tree` node operations                                 |
| `bui.textarea`  | Text and selection editing on `textarea` nodes                                 |
| `bui.custom`    | `CustomHandlers` for host-drawn `custom` nodes                                 |
| `bui.theme`     | Scale, fonts, text shadows, theme files, theme colors and Blender's icons      |
| `bui.css`       | `Stylesheet`, `Style`, selectors, colours and CSS tokens                       |
| `bui.html`      | `Element`, tag constructors, the user-agent sheet and `Document`               |
| `bui.rsx`       | The compile-time `rsx!` markup mixin                                           |
| `bui.kit`       | The complete raw `BK_*` API                                                    |
| `bui.gpu`       | The complete raw `BUI_*` raster, text, texture and sink API                    |
| `bui.capi`      | The stable render-sink ABI in `api/bui_c_api.h`                                |

`import bui;` publicly imports all of them.

## Nodes and properties

Node types include layouts (`frame`, `column`, `row`, `grid`, `box`, `panel`,
`tabs`, `sidetabs`, `scroll`), widgets (`label`, `button`, `checkbox`,
`slider`, `number`, `dropdown`, `enum`, `swatch`, `menubar`), views (`list`,
`tree`, `gridview`, `canvas`, `textarea`, `image`, `video`), editors
(`nodeeditor`, `filebrowser`) and `custom`.

```d
auto row = new Node("row");
auto name = new Node("text");
name.set("value", "Cube");

auto enabled = new Node("checkbox");
enabled.set("text", "Enabled");
enabled.set("value", true);

row.add(name).add(enabled);
name.pack("fill", 1);
enabled.pack("fill", 1);
```

Every setter returns the node, so calls chain. Property names and their
meaning are the kit's, documented in `kit/include/BUI_kit.h`; the wrapper does
not rename them.

A node's string list is a live view, not a copy:

```d
dropdown.setStrs("items", ["One", "Two", "Three"]);
foreach (item; dropdown.strs("items")) {
    // item is a const(char)[] into the kit's storage
}
```

## Events

All handlers have the same shape, `void delegate(NodeEvent)`:

```d
button.onClick((NodeEvent e) {
    writeln("clicked ", e.node.typeName());
});
slider.onChange((NodeEvent e) {
    writeln("value = ", e.node.num("value"));
});
```

`on(handler)` sees every event, `on(kind, handler)` sees one `BK_EV_*` kind,
and `onClick`, `onChange`, `onSubmit`, `onActivate`, `onToggle` and
`onBrowse` are shorthands. `Node.dispatch` is the path the kit's host callback
uses to deliver events to the wrapper.

Callbacks run inside C frames, so they must not throw across them. The
wrappers catch exceptions and store the first one. `Context.pendingError()`
returns it, and `Context.check()` rethrows it; every context input and render
method calls `check()` on both sides of the call. A throwing `Host.request`,
for example, surfaces on the next `render`, `press` or other input call.
`Custom` and `Video` use the same `pendingError` / `check` pattern.

## Ownership and lifetimes

The binding follows the Python binding's reference model. A `Node` created in
D **owns** its C node:

- `free()` releases it and is idempotent; the destructor calls it;
- attaching it to a parent makes the kit retain the D wrapper through
  `BK_set_ref_hooks`, so dropping your last reference is safe;
- nodes returned by the kit (`parent`, `child`, `hover`, `focus`, `popup`)
  are **borrowed**. They never free their C node. If the node has a D wrapper,
  the wrapper is returned, so identity survives the round trip.

Freeing an owned child detaches it from its parent first, then frees it.

The same borrowed/owned split applies to other handles:

- `Window.raster()` returns a borrowed `Raster`; `new Raster(w, h)` owns one.
- `Node.setImage(Image)` and `Node.setVideo(Video)` make the node borrow the
  buffer or player, and the node keeps the D wrapper alive. A `CanvasItem`
  only borrows its `Image`/`Video`: keep the wrapper alive yourself while the
  item can be drawn. `CanvasItem.family` is kept alive by the canvas.
- A `TreeItem` is retained when it is added to a parent or made a root.
  `TreeItem.free()` does nothing while a parent or a tree still holds it.
  Replace the roots first (`setRoots(tree, [])`) when you want to release them.
- Menu descriptions are copied, and the kit retains each menu callback through
  the reference hooks. The `MenuItem` objects can be temporary.

`Context` keeps its root `Node` alive, and `Window` keeps its `Context` alive.
`free()` is safe to call more than once on all of them.

## Menus

```d
context.openMenu(root, [
    new MenuItem("Open", { /* ... */ }).shortcut_("Ctrl+O"),
    MenuItem.separator(),
    MenuItem.check("Grid", true, { /* ... */ }),
    MenuItem.submenu("Recent", [
        new MenuItem("a.txt", { /* ... */ }),
    ]),
]);
```

`MenuItem.separator`, `MenuItem.check`, `MenuItem.submenu`, `shortcut_`,
`icon_` and `disabled` build rows without a builder type. `Context.openPie`
opens a pie menu (at most eight rows are reachable) at the pointer or at
explicit window pixels, `Context.hitAt` finds the interactive node under a
point, and `Node.setMenu` fills a menu button. For tests, `popupCount`, `popup`,
`popupOwner`, `menuRowCount`, `menuRowText` and `menuRowRect` inspect the open
menu.

## Images, video and canvas

```d
auto image = Image.load("photo.png");
auto imageNode = new Node("image");
imageNode.setImage(image);

auto player = new Video("clip.mp4");
player.setLoop(true);
player.play();
auto videoNode = new Node("video");
videoNode.setVideo(player);
```

`Image` uses the kit's shared decoders: PNG, JPEG, WebP, GIF, BMP, TGA, QOI,
HDR, PNM and SVG. `Image.fromRgba`, `fromArgb`, `decode`, `probe`, `toRgba`
and `pixels` cover generated and inspected data. `Animation` steps an animated
image by hand; each frame from `next` is a new `Image`, so free it (or let the
GC finalizer do it) when done.

A canvas is a display list. `setItem` replaces an item with the same id,
`deleteItem` removes by id, `putItem` inserts by index, `truncate` drops the
tail, and `raiseItem` changes z-order:

```d
auto canvas = new Node("canvas");
canvas.pack("fill", 1);
setItem(canvas, CanvasItem.rect(1, 4, 4, 60, 40, [0.2f, 0.4f, 0.8f, 1]));
addItem(canvas, CanvasItem.label(2, 8, 8, "hello"));
```

## Trees, text areas and custom nodes

`bui.tree` wraps the `"tree"` node's rows. Add `TreeItem` children, set
`expanded`, `marked`, `icon` and `toggle0`..`toggle31`, and drive the node with
`setRoots`, `selected`, `select`, `rowCount`, `rowItem` and `rowRect`:

```d
auto tree = new Node("tree");
auto rootItem = new TreeItem("Scene");
rootItem.add(new TreeItem("Cube"));
rootItem.add(new TreeItem("Camera"));
setRoots(tree, [rootItem]);
```

`bui.textarea` edits a `"textarea"` node by UTF-8 byte offsets:
`insertText`, `deleteText`, `caret`, `setCaret`, `selection`, `setSelection`,
`setSpans`, `indexAt` and `see`.

`bui.custom` gives a `"custom"` node its own event, frame and draw callbacks:

```d
auto custom = setCustom(node, CustomHandlers(
    (Node node, CustomEvent event) { return 1; },
    (Node node) { /* frame */ },
    (Node node, Raster raster, const(int)* clip) { /* draw */ },
));
requestFrame(node, 16);
```

The node keeps the `Custom` object alive. `custom.pendingError()` and
`custom.check()` report exceptions from its callbacks.

## Raw declarations

When a wrapper does not cover something, use the raw module directly:

```d
import bui.kit;
import bui.gpu;

BKNode* n = BK_node_new("label");
BK_set_str(n, "text", "raw");
BK_node_free(n);

auto r = BUI_raster_create(320, 200);
BUI_raster_free(r);
```

The declarations match `kit/include/BUI_kit.h`, `gpu/include/GPU_raster.h`,
`gpu/include/GPU_backend_sink.h` and `api/bui_c_api.h`. Opaque handles are
`struct` declarations, enums keep the C values, and function pointer
parameters use `extern(C)` aliases. The two field names that collide with D
keywords are renamed: `ref` to `ref_`, `align` to `align_`.

`bui.capi` declares the stable render-sink ABI: `bui_get_version`,
`bui_version_string`, `bui_set_render_sink`, `bui_get_render_sink` and
`bui_set_viewport`.

## Building and testing

`dub.json` runs `scripts/build_bui.ps1` on Windows and
`scripts/build_bui.sh` elsewhere before compiling. The scripts configure
CMake with the other language bindings off, build `bui_kit` and `bui_api` in
Release, and copy `bui_kit`, `bui_gpu`, `bui_core` and `bui_api` into
`.bui/lib`.

```sh
dub build --config=library
dub run --config=hello
dub run --config=headless
dub run --config=rsx
dub run --config=test
```

`tests/run.d` covers versioning, nodes, properties, geometry, render and input,
clipboard, images, animation, text areas, canvases, trees, menus, custom nodes,
host exceptions, themes, icons, the CSS cascade, the HTML realiser, RSX and the
stable C API version.

On Windows the C kit is built with the dynamic UCRT (`/MD`). LDC defaults to
the static CRT, so the package passes `-mscrtlib=msvcrt`; this keeps one CRT in
the process and avoids duplicate-symbol errors at link time.

## Platform notes

- **Windows x64** is tested, with LDC 1.43 and the Visual Studio C++ tools.
- **Linux** builds the X11 host; Wayland is used when the headers and
  `wayland-scanner` are present. The link flags include `-lstdc++` and `-ldl`.
- **macOS** uses the experimental Cocoa host and the frameworks in
  `lflags-osx`.
- A desktop session is needed for `--config=hello`. Headless CI runs
  `--config=headless` or `--config=test`, both of which render into a raster.

The complete API notes and ownership rules also live in
[`bindings/d/README.md`](../bindings/d/README.md).
