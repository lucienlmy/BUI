# BUI Architecture

BUI is a decoupled UI toolkit extracted from the Blender project
(`source/blender/editors/interface` and its support layers). It contains **only**
reusable interface machinery — no Blender application logic (no scene graph,
depsgraph, nodes, operators-on-datablocks, file loading, Python).

**What actually runs.** Every app and binding runs on **`kit/`**, a C runtime
that re-ports Blender's interface code (widget drawing from
`interface_widgets.cc`, interaction from `interface_handlers.cc`) and draws
through `gpu/`'s software raster or GPU renderer:

```
 tkinter / Qt syntax / components (Python) · Rust (blenderui) · JS / React · Dear ImGui · C
                                       │
 kit/      node tree · geometry managers · input & interaction · widget drawing · menus
           · editors · undo · animation · themes · native windows (Win32, X11, Cocoa)
           · kit/intern/blender/ (Blender files copied in: string search, UI undo, float precision)
                                       │
 gpu/      software raster (default) or GPU renderer (Direct3D 11 / OpenGL 3.3) · text engine
                                       │
 core/     blenlib subset (math, rct, string_search.cc, expr_pylike_eval.cc ...)
```

**Blender's original sources, kept for reference.** `ui/` holds Blender's
`editors/interface` files, compiled only with `-DBUI_BUILD_UI=ON` (off by
default). They sit on `types/`, `compat/` and the immediate-mode seam of
`gpu/`, as below; `kit/` doesn't use them.

```
┌──────────────────────────────────────────────────────────────────┐
│  ui/      widget toolkit: blocks/buttons, layout engine,         │
│           widget drawing, handlers, views, view2d                │
└──────┬───────────────┬──────────────┬─────────────┬──────────────┘
       │               │              │             │
┌──────▼──────┐ ┌──────▼──────┐ ┌─────▼──────────┐ ┌▼──────────────────┐
│ types/      │ │ gpu/        │ │ compat/        │ │ api/  stable C    │
│ DNA data    │ │ draw seam   │ │ RNA / CTX / WM │ │ ABI for hosts     │
│ shapes      │ │ (null back- │ │ / ED / BLF /   │ │ & bindings        │
│ (verbatim)  │ │  end + sink │ │ IMB seams      │ └───┬─────────┬─────┘
│             │ │  dispatch)  │ │ (stubs & hooks)│     │         │
└──────┬──────┘ └──────┬──────┘ └─────┬──────────┘ ┌───▼───┐ ┌───▼──────┐
       │               │              │            │buipy  │ │bui_qt    │
       │               │              │            │Python │ │QPainter  │
       │               │              │            │binding│ │backend   │
┌──────▼───────────────▼──────────────▼──────────┐└───────┘ └──────────┘
│  core/   pure C11 foundation extracted from blenlib            │
│          listbase · ghash · mempool · memarena · dynstr        │
│          math (vec/mat/rot/color/geom) · strings · rct         │
│          guarded-allocator shim · threads stubs                │
└────────────────────────────────────────────────────────────────┘
```

Dependency rule: arrows point downward only. `ui/` never includes Blender
application code; everything it needs arrives through `types/`, `gpu/`, or
`compat/`. (This diagram is the reference build; the runtime above doesn't
include `ui/`.)

## Modules

| Module | Contents | Status |
|---|---|---|
| `core/` | C11 subset of blenlib, de-C++ified with mechanical transforms (namespaces stripped, casts normalized, MEM API mapped). Inline math lives in `include/intern/*.c` included by headers (`MINLINE` = `static inline` in C). | **Builds + tested** |
| `types/` | Verbatim `DNA_*_types.h` struct definitions. `DNA_listBase.h` aliases `blender::{Link,LinkData,ListBase}` onto the core's C structs so DNA structs and the C listbase API share one type. | **Builds** |
| `gpu/` | The draw surface used by the toolkit (`GPU_*`, `imm*`) behind seam headers + a null backend that *records and forwards*: with a sink installed it emits screen-space primitives (`GPU_backend_sink.h`); without one it discards them. Theme colors reach the GPU layer through function-pointer hooks. | **Builds + tested** |
| `compat/` | Seam headers carrying upstream API names (`RNA_access.hh`, `BLF_api.hh`, …) with BUI-owned opaque types and stub implementations (`intern/bui_stubs.cc`). | **Builds** |
| `ui/` | The toolkit sources from `editors/interface`: interface.cc family, widgets, handlers, regions, view2d, views, textbox. Kept for reference and porting work; **not compiled by default** (`-DBUI_BUILD_UI=ON` builds it as `bui_ui`). `kit/` re-ports what it needs in C. | **Compiles + links** (opt-in) |
| `kit/` | Blender's UI as a retained-mode C runtime: node tree, uiLayout-style layout, `interface_handlers.cc` interaction rules, widget drawing ported from `interface_widgets.cc` (widget-base shader parameters rendered by `gpu/` raster), menus, color picker, views, editors; Tk-style pack/grid/place geometry managers, canvas display list, multi-line text area; UI undo/redo, Tab field traversal, vertical multi-drag, panel animation; Blender's file browser; images (`kit/intern/image`: Blender's imbuf design, one format table over Wuffs, libjpeg-turbo, libwebp and BUI's HDR/PNM/SVG readers in `kit/extern`, decode at the drawn size, a worker pool and a memory-budgeted cache; see [the Images page](https://bui.preetham.top/docs/concepts/images/)); damage-driven rendering; Blender's Dark and Light themes (`kit_theme_presets.c`, generated by `tools/gen_theme.py` from Blender's `Blender_Dark.xml` / `Blender_Light.xml` presets over `userdef_default_theme.c`; `BK_set_theme`); native window hosts for Win32, X11 and Cocoa (`kit_window_win32.c`, `kit_window_x11.c`, `kit_window_cocoa.m`; `kit_window_null.c` elsewhere). The Win32 host is BUI's own; the X11 host follows Blender's GHOST input mapping, and the Cocoa host (experimental: checked by CI and tried by hand on two Apple Silicon Macs) is ported from GHOST's Cocoa backend. `kit/intern/blender/` holds Blender files copied in and edited for the kit (each edit marked `BUI:`): `interface_string_search.cc` (menu search with recent searches), `interface_undo.cc` (text-edit undo), `interface_utils.cc` (float display precision). C API: `kit/include/BUI_kit.h`. | **Builds + tested** |
| `api/` | `bui_c_api.h` — small stable C ABI: version info + sink/viewport plumbing for hosts and bindings. | **Builds + tested** |
| `bindings/python/` | `buipy` CPython extension (no third-party deps; Dear ImGui is compiled into it, `blenderui.imgui`, so a process holds one kit) + `bui` package: `bui.components` (Blender component library), `bui.qtsyntax` / `bui.tksyntax` (Qt / tkinter APIs as thin facades over the kit), `bui.host` (native or Tk windows), `SvgSink` headless renderer, `bui.qt.BuiCanvas` PySide6/PyQt6 widget. | **Builds + tested** |
| `bindings/imgui/` | Dear ImGui (vendored, 1.93.1 WIP) on BUI: `imgui_impl_bui` platform + renderer backend hosted in a kit `custom` node (input, cursors, clipboard, DPI, damage-driven frames) drawing through `BUI_raster_mesh` and BUI textures; Blender style and Inter font; the C API (`cimgui.h`) and the generator behind the Python and Rust bindings; the Blender-widgets build (`blender/`, `-DBUI_IMGUI_BLENDER_WIDGETS=ON`). Gate: `-DBUI_BUILD_IMGUI` (ON with C++). | **Builds + tested** |
| `bindings/rust/` | `blenderui` (HTML/CSS/Tailwind, `rsx!` and gpui-style builders, hooks, Blender components; layout by taffy) over `blenderui-sys`; `blenderui-macros`, `blenderui-tailwind`, `blenderui-imgui`, and the JavaScript runtimes `blenderui-jsview`, `blenderui-node` (napi-rs) and `blenderui-qjs` (QuickJS). | **Builds + tested** |
| `bindings/node/`, `bindings/react/` | `@blender-ui/native` (DOM-like API, signals) and `@blender-ui/react` (React 19.3 renderer) on `blenderui-node`. | **Builds + tested** |
| `backends/qt/` | C++ Qt backend: `BUI::QtSink` (BuiGpuSink → QPainter) + `BUI::BuiWidget` (QWidget host, no moc needed). Gate: `-DBUI_BUILD_QT_BACKEND=ON`. | **Shipped** (builds where Qt exists) |
| `examples/` | `kit_hello.c` (the kit in a native window), `svg_demo.c` (C, sink → SVG), `python_demo.py` (buipy → SVG), the Python, Qt, tkinter and ImGui demos, and `site/` (the website's screenshot programs). C examples need `-DBUI_BUILD_EXAMPLES=ON`. | **Builds + run** |

## Render-sink dispatch

The null backend is the integration seam for the immediate-mode GPU API
(`imm*`, batches) that `ui/`, `buipy.demo_scene` and the sink examples use.
The kit doesn't draw through it (see below). `gpu/include/GPU_backend_sink.h`
defines `BuiGpuSink` — a function-pointer table (`triangles`, `lines`,
`points`, `text`, `scissor`, `begin_frame`, `end_frame`). The null backend:

- records immediate-mode vertices (`immBegin/immAttr*/immVertex*/immEnd`),
- decodes vertex batches (pos/color attributes, index buffers),
- applies the model matrix and forwards screen-space primitives to the sink,
- or, with no sink installed, discards everything (pure null).

### Raster and GPU renderer

The kit itself draws through `GPU_raster.h`:
- **Software raster** (`gpu/intern/gpu_raster.c`): the default. It renders
  anti-aliased SDF primitives into a CPU frame buffer, in parallel bands.
  Text is a glyph-run cache with Inter (Blender's UI font) embedded as the
  default face (`gpu/extern/font_inter.h`). Glyphs are rendered by the
  vendored FreeType 2.13.3 the way Blender's BLF does (light hinting, gamma
  1.43, optical size; `BUI_TEXT_FREETYPE`, on by default), or by stb_truetype
  with it off; stb_truetype also reads metrics and character maps.
  `BUI_TEXT_GDI=ON` switches Windows to GDI and system fonts.
  The GPU renderer draws text one quad per glyph. Slug (`K_SLUG` in
  `bui_ui.hlsl`) is the default: glyph outlines are uploaded once as
  quadratic curves with horizontal and vertical bands, and the pixel shader
  integrates exact coverage. With `BUI_SLUG=0`, it uses glyph bitmaps from
  the shared glyph cache in the mask atlas instead.
- **GPU renderer**: Direct3D 11 on Windows (`gpu/intern/gpu_d3d11.c`,
  `gpu/shaders/bui_ui.hlsl`) and OpenGL 3.3 core on Linux
  (`gpu/intern/gpu_gl.c`: the same instances, atlases and Slug textures, the
  uber-shader translated to GLSL, GLX on the X11 window or EGL offscreen,
  libGL loaded at run time). Opt-in with `BUI_GPU=1`, `BUI_gpu_set_default`,
  Rust `.gpu(true)` or tkinter `Tk(gpu=True)`.

Images (`BUI_raster_image`) are premultiplied `0xAARRGGBB` pixmaps with a key:
the software raster filters them bilinearly (exact copies at 1:1); the GPU
renderers place each key once in the RGBA atlas and sample it the same way.
Images over 2047 px get a texture of their own (tiled past the device
limit), freed when the kit frees the image (`BUI_image_forget`) or when
unused. Animated images (`kit/intern/image/anim.c`, `play.c`) are decoded a
frame at a time on the workers and advance on the context's frame timer,
shared with custom nodes (`bk_ctx_arm_timer`), only while on screen.

Video uses the same path: one codec interface (`BKImAnimCodec`: open, output
size, next, seek, close) for GIF, WebP and the OS movie backends
(`movie_mf.c` Media Foundation, `movie_gst.c` GStreamer, `movie_avf.m`
AVFoundation; loaded at run time), one clock (`play.c`), and private cache
entries per player (`video.c`). Its sound (`kit/intern/audio`) is a second
reader on the audio track, pulled by the OS audio output's thread (WASAPI,
CoreAudio, PulseAudio), and its position is the clock the frames follow.
Frames are NV12 pixmaps
(`BUI_PIXMAP_NV12_*`) at the size drawn, two per player reused; a pixmap's
`stream` keeps one GPU texture pair per player, updated in place, converted
to RGB in the shader (the software raster converts what it draws).

The optional parts (image formats, video, the GPU renderers, FreeType, the
file browser, Dear ImGui) are one table, `features.json`, read by CMake
(`features.cmake`), cargo (`blenderui-sys/build.rs`) and `bui build`
(`bindings/node/features.js`). Off, their sources aren't compiled; their API
stays.

Textured triangle meshes (`BUI_raster_mesh`, what Dear ImGui emits) are the
one non-analytic primitive: indexed vertices with UV and color sampling a
`BUI_texture_*` texture. The software raster fills them with a fixed-point
top-left rule and bilinear sampling; the GPU paths draw them indexed with a
scissor (`vs_mesh`/`ps_mesh` in `bui_ui.hlsl`, the GLSL twin in `gpu_gl.c`),
uploading each texture once and then only its logged update rectangles.

In GPU mode the same `BUI_raster_*` calls record one 128-byte instance per
primitive, after the same clip test. This follows Blender's
`GPU_SHADER_2D_WIDGET_BASE_INST` batching in `interface_widgets.cc`:
- One uber-shader evaluates the software raster's coverage formulas, so the
  pixels match. `gpu/tests/test_gpu_parity.c` checks this.
- Text runs and images are cached in atlases. Polygons are covered on the
  CPU and uploaded as masks.
- Frames render into a persistent target, so damage rectangles still limit
  redraws. On Windows, presentation uses a flip-model swap chain with dirty
  rects.
- The device is created on a worker thread, and a lost device falls back
  to software.

Real renderers plug into the immediate-mode seam at three levels: C
(`bui_set_render_sink`), Qt/C++ (`BUI::QtSink`), Python
(`buipy.set_backend`). To show kit widgets in your own renderer, render the
kit into a `BuiRaster` and blit it instead. See INTEGRATION.md.

## Build

```sh
cmake -S . -B build -G "MinGW Makefiles"     # or any generator
cmake --build build
ctest --test-dir build                       # core tests + buipy tests
cmake -S . -B build-ui -DBUI_BUILD_UI=ON -DBUI_BUILD_EXAMPLES=ON
cmake --build build-ui                       # + widget toolkit (bui_ui)
```

Options: `BUI_BUILD_PYTHON_BINDINGS` (ON; auto-skips without Python dev
files), `BUI_BUILD_QT_BACKEND` (OFF; needs Qt5/Qt6 Widgets),
`BUI_BUILD_EXAMPLES`, `BUI_BUILD_TESTS`, `BUI_BUILD_UI` (OFF),
`BUI_BUILD_IMGUI`, `BUI_IMGUI_BLENDER_WIDGETS`, `BUI_TEXT_FREETYPE`,
`BUI_TEXT_GDI`, `BUI_SLUG`, `BUI_IMAGE_SVG`, `BUI_WARNINGS_AS_ERRORS`. See
[docs/getting-started.md](docs/getting-started.md#build-options).

Windows note: the toolchain must resolve MinGW runtime DLLs from
`/c/msys64/ucrt64/bin` **before** `/mingw64/bin` or other vendors' paths
(cc1.exe loads zlib1/gmp from PATH). If builds fail silently, prepend the
ucrt64 dir: `export PATH="/c/msys64/ucrt64/bin:$PATH"`.

## Decoupling inventory (what was cut, what remains)

Cut entirely (Blender application logic): scene/mesh/node/depsgraph/modifiers,
animrig (only the DNA structs + a minimal `animrig::Slot` wrapper remain),
asset system, sequencer, render, Python-in-Blender, file I/O (BLO),
eyedroppers, app-specific UI templates, window manager backends (GHOST; the
kit's X11 and Cocoa hosts re-port the parts they need), operator system.

The table below is about the **`ui/` reference sources** only. `kit/` has
real widgets on screen today; these seams are what Blender's original
`editors/interface` code would still need to be hosted as-is. Call-site
counts measured from the copied sources:

| Seam | Touchpoints | Notes |
|---|---|---|
| WM (events/operators/keymaps) | ~183 | Needs lite `wmEvent`/`wmOperator` structs + handler plumbing in `compat/` |
| ED (screen/region services) | ~120 | Region redraw tagging, area/region lookup — small BUI-owned implementations |
| CTX (context) | ~46 | Minimal `bContext` replacement |
| BLF (fonts) | ~45 | Text measurement/draw hook; null impl first |
| IMB (images/icons) | ~23 | Icon texture registration hook |
| RNA (properties) | ~16 entry points | Opaque `PropertyRNA` + accessor vtable |

These seams block *widget-tree hosting* (creating blocks/buttons driven by
events), not compilation: all five libraries — `bui_core`, `bui_gpu`,
`bui_compat`, `bui_api`, `bui_ui` — build and link cleanly today, and the
render pipeline is exercisable end-to-end through the sink (see
`examples/`).

## Porting notes

- Upstream files keep their names and SPDX headers (GPL-2.0-or-later). The
  mechanical de-C++ pass is intentionally boring so future syncs stay easy.
- `core/include/fmt/*` is a declaration-level stand-in for the external {fmt}
  library; swap for real {fmt} if you need formatted strings.
- `MEM_guardedalloc.h` is a plain-malloc shim with the upstream C++ template
  surface (`MEM_new<T>`, `MEM_new_zeroed<T>`, `MEM_new_array_uninitialized<T>`,
  `MEM_delete<T>`, `MEM_SAFE_DELETE`).
- `GPU_batch.hh` carries a layout contract: the first four members of
  `blender::gpu::VertBuf` must match `GPUVertBufInternal` in `gpu_null.c`.
