# BUI for D

[D](https://dlang.org) bindings for BUI: Blender's widget drawing, layout and
interaction rules in a small C runtime, with an idiomatic D face.

The package has three layers:

- **Raw declarations** in `bui.kit`, `bui.gpu` and `bui.capi`, a one-to-one
  translation of `kit/include/BUI_kit.h`, `gpu/include/GPU_raster.h`,
  `gpu/include/GPU_backend_sink.h` and `api/bui_c_api.h`. Nothing is hidden;
  every C function is available.
- **Wrappers** in `bui.node`, `bui.context`, `bui.window`, `bui.menu`,
  `bui.raster`, `bui.image`, `bui.video`, `bui.canvas`, `bui.tree`,
  `bui.textarea`, `bui.custom` and `bui.theme`. They turn handles into classes,
  delegates into callbacks, and C arrays into D slices.
- **HTML, CSS and RSX** in `bui.html`, `bui.css` and `bui.rsx`: an `Element`
  tree, tag constructors, a CSS cascade and a compile-time `rsx!` mixin that
  turns markup into that tree.

`import bui;` gives you all of them.

## Requirements

- A D compiler. The package is tested with **LDC 1.43** (DMD 2.113 front end)
  and its `dub`. DMD 2.113 or newer should also work.
- **CMake** and a C compiler. On Windows, install the Visual Studio C++ tools;
  the build script finds `cmake` through Visual Studio when it is not on
  `PATH`. On Linux, a normal GCC/Clang toolchain is enough. macOS links the
  Cocoa frameworks listed in `dub.json`.
- No separate BUI install: the package builds the C kit into `.bui/` on the
  first DUB build, and CMake keeps it up to date afterwards.

## Quick start

From this repository:

```sh
cd bindings/d
dub run --config=hello      # a native window
dub run --config=headless   # render off-screen, no window
dub run --config=rsx        # an HTML/CSS page in a native window
dub run --config=test       # the integration suite
```

Use the package from another DUB project with a path dependency:

```json
{
    "name": "my-app",
    "targetType": "executable",
    "dependencies": {
        "bui": { "path": "../BUI/bindings/d" }
    }
}
```

```sh
dub run --compiler=ldc2
```

To build another BUI checkout, set `BUI_SOURCE_DIR` before DUB runs. Set
`BUI_CMAKE_GENERATOR` when you want a generator other than Visual Studio, such
as `"Ninja"` with `cl` on `PATH`.

## A first window

`examples/hello.d`:

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

Nodes are made by type name: `frame`, `column`, `row`, `grid`, `box`,
`panel`, `tabs`, `sidetabs`, `scroll`, `label`, `separator`, `progress`,
`button`, `toggle`, `checkbox`, `radio`, `decorator`, `text`, `search`,
`hotkey`, `number`, `slider`, `dropdown`, `enum`, `swatch`, `popover`,
`menubutton`, `menubar`, `list`, `tree`, `gridview`, `canvas`, `textarea`,
`image`, `video`, `ramp`, `curve`, `normal`, `histogram`, `custom`,
`filebrowser` and the Blender editors (`nodeeditor`, `fileselector`, ...).

Properties are set and read by name with `set`, `num`, `str`, `color`,
`setStrs` and `strs`. Geometry uses Tk's managers through `pack`, `grid` and
`place`.

## Headless rendering

You do not need a window to use BUI. Create a context, render into a `Raster`,
send input, and inspect the pixels. `examples/headless.d` does exactly that:

```d
auto root = new Node("column");
auto button = new Node("button");
button.set("text", "Click me");
button.onClick((NodeEvent) { ++clicks; });
root.add(button);

auto context = new Context(root);
auto raster = new Raster(320, 200);
context.render(raster, true);

const r = button.rect();
context.press(r[0] + r[2] / 2, r[1] + r[3] / 2, 0);
context.release(r[0] + r[2] / 2, r[1] + r[3] / 2, 0);
```

`context.render` returns the number of damaged regions redrawn. `full = true`
forces a complete frame. `needsRender` and `animating` tell you when there is
work to do.

## HTML, CSS and RSX

`bui.html` builds a DOM-like `Element` tree and realises it as kit nodes.
`bui.css` parses a practical CSS subset and cascades it over the tree.
`bui.rsx` turns markup into the same calls at compile time:

```d
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
auto window = new Window(context, "BUI RSX", 360, 180);
window.show();
run();
```

The same tree can be written by hand: `body_(div(h1("Hello"), p("text")).class_("card"))`.
`Element` has `id`, `class_`, `attr`, `style`, `text`, `setText`, `child`,
`append` and the event methods (`onClick`, `onChange`, ...).

`Document` runs the user-agent sheet plus the document's `<style>` elements and
any CSS string passed to its constructor, then maps:

- block boxes and inline text to frames and labels;
- `display: flex` with `flex-direction: row` to a pack row, column to a pack
  column;
- `display: grid` to a frame grid with column weights;
- `padding`, `margin`, `gap`, `width`, `height`, `flex`, colours, borders,
  fonts, `text-align`, `cursor`, `visibility` and `object-fit` to the kit's
  properties;
- tables to a frame grid with captions above it;
- standard widgets (`input`, `select`, `textarea`, `button`, `progress`,
  `meter`) to their kit controls;
- `img`, `canvas` and `video` to the replaced nodes.

Selectors include type, `#id`, `.class`, attributes, descendant/child/sibling
combinators, dynamic states such as `:hover` and `:active`, and the usual
specificity plus inline-style precedence. Custom properties, `var()`, `em`,
`rem`, `px`, `vw` and `vh` resolve.

Tag constructors keep HTML names, with four exceptions for D: `body_`,
`template_`, `object_` and `select_` (because `bui.tree.select` already exists).
Markup inside `rsx!` always uses the HTML spelling, so `<select>` is fine there.

`rsx!` parses in CTFE and lowers to ordinary `Element` calls: attributes,
`{expression}` text and values, `onclick`/`onchange`/... delegates, boolean
attributes, entities, comments, void tags, raw `<style>` text and custom
elements. Dynamic attributes such as `class={names}` and `hidden={flag}`
stringify or toggle through `flagAttr`.

`Document.restyle` re-runs the cascade and applies visual properties after
`Element.setState("hover", true)` or in the kit `ENTER`/`LEAVE`/`PRESS`/
`RELEASE` handlers; layout structure is fixed when the document is built. A
changed text leaf updates in place with `Element.setText`; a different document
structure needs a new `Document`.

The subset is deliberately smaller than the Rust engine's full taffy layout: no
flex wrapping, no media queries, no pseudo-elements, no animations and no
Tailwind. Text stays one label per leaf.

## Events

Handlers are `void delegate(NodeEvent)`:

```d
button.onClick((NodeEvent e) {
    writeln("clicked ", e.node.typeName());
});
size.onChange((NodeEvent e) {
    writeln("value = ", e.node.num("value"));
});
```

`on` adds a handler for every event, `on(kind, handler)` for one `BK_EV_*`
kind, and `onClick`, `onChange`, `onSubmit`, `onActivate`, `onToggle`,
`onBrowse` are shorthands. The event carries the node, the kind, the payload
`ref_`, the two doubles and, for raw key events, the key text.

Callbacks must not throw through C frames. The wrappers catch exceptions where
the kit calls in:
`Context.pendingError()` returns the first one and `Context.check()` rethrows
it. `Context`'s input and render methods call `check()` for you, so a throwing
host callback surfaces at the next input or render call. `Custom` and `Video`
callbacks use the same pattern.

## Ownership

`new Node(type)` owns its C node. `free()` releases it and is idempotent; the
destructor calls it too. Nodes the kit returns (`parent`, `child`, `hover`,
`focus`, `popup`) are **borrowed**: they come back as wrappers that never free
the C node. If the node was created in D its existing wrapper is returned, so
identity is preserved.

When a node is attached to a parent, the kit retains its wrapper through the
reference hooks, exactly like the Python binding's `INCREF`/`DECREF`:

- dropping the last D reference to an attached node is safe;
- `Node.free()` on an owned child detaches it first, then frees it;
- a C-created child that has no D wrapper is wrapped as borrowed and left to
  its owner.

The same rule applies to:

- **`Image`** and **`Video`** attached with `Node.setImage` or `Node.setVideo`.
  The node borrows the buffer/player, so the wrapper keeps it alive for you.
  For `CanvasItem.image` and `CanvasItem.video`, keep the wrapper alive while
  the item can be drawn; the canvas only borrows the handle. `CanvasItem.family`
  is kept alive by the canvas, so the string can be temporary.
- **`Raster`** returned by `Window.raster()`: borrowed, so never free it.
  `new Raster(w, h)` owns its raster.
- **`TreeItem`**: adding a child or making an item a root retains it. `free()`
  does nothing while a parent or a tree still holds the item; replace the
  tree's roots (`setRoots(tree, [])`) before freeing them.
- **Menu callbacks**: the kit copies the menu description and retains each
  callback through the reference hooks. The D `MenuItem` tree can be
  temporary.

`Window` keeps its `Context` alive, and `Context` keeps its root `Node`
alive. Free them in any order; each `free()` is safe to call more than once.

## Menus

```d
context.openMenu(root, [
    new MenuItem("Open", { /* ... */ }).shortcut_("Ctrl+O"),
    MenuItem.separator(),
    MenuItem.check("Grid", true, { /* ... */ }),
    MenuItem.submenu("Recent", [new MenuItem("a.txt", { /* ... */ })]),
]);
```

The `MenuItem` helpers are `separator`, `check`, `submenu`, `shortcut_`,
`icon_` and `disabled`. `Context.openPie` opens a pie menu at the pointer, or
at explicit window pixels (`openPie(items, title, x, y)`; `pieAtPointer` is the
default for either coordinate). `Node.setMenu` fills a menu button.
`Context.popup`, `popupCount`, `menuRowCount`, `menuRowText` and `menuRowRect`
are there for tests and automation. `Context.hitAt(x, y)` returns the
interactive node under a point.

## Images, video and canvas

```d
auto image = Image.load("photo.png");
auto node = new Node("image");
node.setImage(image);          // the node borrows it; node keeps it alive

auto player = new Video("clip.mp4");
player.setLoop(true);
player.play();
auto videoNode = new Node("video");
videoNode.setVideo(player);
```

`Image` decodes PNG, JPEG, WebP, GIF, BMP, TGA, QOI, HDR, PNM and SVG through
the kit's shared decoders. `Image.fromRgba`, `fromArgb`, `decode`, `probe`,
`toRgba` and `pixels` cover generated and inspected images. `Animation` steps
an animated image by hand; each frame from `next` is a new `Image`, so free it
(or let the GC finalizer do it) when done.

`bui.canvas` is a display list on a `canvas` node:

```d
auto canvas = new Node("canvas");
canvas.pack("fill", 1);
setItem(canvas, CanvasItem.rect(1, 4, 4, 60, 40, [0.2f, 0.4f, 0.8f, 1]));
addItem(canvas, CanvasItem.label(2, 8, 8, "hello"));
```

Items have ids: `setItem` inserts or replaces by id, `deleteItem` deletes by
id (`-1` deletes all), `putItem` inserts by index, `truncate` drops the tail,
and `raiseItem` changes z-order.

## Text areas, trees and custom nodes

- `bui.textarea`: `insertText`, `deleteText`, `caret`, `setCaret`,
  `selection`, `setSelection`, `setSpans`, `indexAt` and `see`. Offsets are
  UTF-8 bytes.
- `bui.tree`: `TreeItem` rows with label, icon, `expanded`, `marked`,
  `toggle0`..`toggle31`, columns (`setValues`) and children. `setRoots`,
  `selected`, `select`, `rowCount`, `rowItem` and `rowRect` drive a `"tree"`
  node.
- `bui.custom`: `setCustom(node, CustomHandlers(...))` gives a `"custom"` node
  its own event, frame and draw callbacks. The node keeps the `Custom` object
  alive; `requestFrame`, `setCursor` and `hasFocus` complete the API.

## Host, window and theme

`Context` installs a `BKHost` whose C callbacks forward to `Host`:

```d
context.setHost(Host(
    request: { /* schedule a render */ },
    setCursor: (int cursor) { /* change the pointer */ },
    warp: (int x, int y) { /* move the pointer */ },
    clipboardGet: { return "text"; },
    clipboardSet: (string text) { /* set the clipboard */ },
    schedule: (int ms, int timer) { /* call context.timer(timer) later */ },
));
```

Do this **before** creating a `Window`: the native window installs its own host
that provides these services and forwards events to the handlers above.
`Window` adds `show`, `close`, `setTitle`, `setSize`, `size`, `raster`,
`setGpu`, `gpu`, `frames`, `renderNow`, `timer`, `requestFrame`,
`cancelTimer`, `nativeHandle`, `wm`/`wmGet`, `onTimer`, `onClose`, `onFrame`
and `onFocus`. `run`, `pump`, `quit`, `wake`, `wakeInit`, `setStopCheck`,
`setKeepAlive` and `windowSupported` are module-level.

`bui.theme` has `setScale`, `scale`, `setTextShadow`, `textShadow`, `unit`,
`setFont`, `setFontSize`, `fontSizePoints`, `setTheme`, `themeName`,
`themeColor`, `loadTheme`, `loadThemeFile`, `themeError`, `themeIgnored`,
`themeCount`, `themeAt`, `setThemeColor`, `setThemeNum`, `themeNum`,
`themeGeneration`, `themeMode`, `iconId`, `iconCount`, `iconName`,
`iconListCount` and `iconListName`. Theme files use the JSON format in
`docs/theming.md`; a custom theme becomes active with `setTheme(itsName)`.

`Raster` adds `setAlpha`, `alpha` and `clear` for premultiplied transparent
frames, alongside `setClip`, `damage`, `invalidate`, the drawing calls and the
GPU attach/poll methods.

## Raw C API

Use the raw modules when a wrapper does not cover what you need:

```d
import bui.kit;
import bui.gpu;

BKNode* n = BK_node_new("label");
BK_set_str(n, "text", "raw");
BK_node_free(n);

auto r = BUI_raster_create(320, 200);
BUI_raster_free(r);
```

Opaque handles are `struct` declarations, enums match the headers, and all
function pointer parameters use `extern(C)` aliases. D keywords that appear in
the C API are renamed: `ref` becomes `ref_`, `align` becomes `align_`.
`bui.capi` declares the stable render-sink ABI (`bui_get_version`,
`bui_version_string`, `bui_set_render_sink`, `bui_get_render_sink`,
`bui_set_viewport`).

## Building the C kit

`scripts/build_bui.ps1` and `scripts/build_bui.sh` run CMake with the bindings
off and build `bui_kit` and `bui_api` in Release, then copy the four static
libraries into `.bui/lib`. DUB calls the right script from `preBuildCommands`,
so a normal `dub build`, `dub run` or `dub run --config=test` is enough.
Override the checkout with `BUI_SOURCE_DIR`.

On Windows the C libraries use the dynamic UCRT (`/MD`, CMake's Release
default). LDC defaults to the static CRT, so `dub.json` passes
`-mscrtlib=msvcrt`; keeping one CRT avoids duplicate symbols and two heaps.

## Tests and examples

- `examples/hello.d` — slider and button in a native window.
- `examples/headless.d` — no window: render, click, read pixels.
- `examples/rsx_page.d` — an HTML/CSS page from `rsx!` in a native window.
- `tests/run.d` — version, nodes and properties, geometry, render/input,
  clipboard, images, text areas, canvases, trees, menus, custom nodes, host
  errors, themes, icons, the CSS cascade, the HTML realiser, RSX and the stable
  C API version.

Run the suite with `dub run --config=test`. It is a normal executable, so it
also works from CI steps that expect a process exit code.

## Platform notes

- **Windows x64** is the tested target. It needs the Visual Studio C++ tools,
  CMake, and the dynamic UCRT.
- **Linux** builds the X11 host by default, with Wayland when the headers and
  `wayland-scanner` are available; the link flags include `-lstdc++ -ldl`.
- **macOS** uses the experimental Cocoa host and the frameworks in
  `lflags-osx`.
- `dub run --config=hello` opens a real window. In a headless CI session use
  `--config=headless` or `--config=test`.

## License

GPL-3.0-or-later, like BUI.
