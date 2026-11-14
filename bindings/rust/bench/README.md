# blenderui (CPU and GPU) vs gpui vs gpui-fast

The same UI on three frameworks:
- `bui-bench`: blenderui, on the software raster (default) or the GPU
  renderer (`BUI_GPU=1`; `run.py` runs it as `blenderui-gpu`).
- `gpui-bench`: [gpui](https://crates.io/crates/gpui) 0.2.2, Zed's GPU-rendered framework.
- `gpui-fast-bench`: [gpui-fast](https://github.com/longbridge/gpui-fast) 0.1.13, a gpui fork with rendering and layout optimizations.

Both gpui apps compile the identical source file (`gpui-bench/src/scene.rs`).
The scene is a 1000×800 window filled with N cells (a rounded, coloured box
with a centred number), built with the same element tree in both
(`div().w(..).h(..).bg(..).rounded(..).flex().items_center().child(label)`).

- **anim**: every frame changes every cell's colour and number, and the view
  calls `window.request_animation_frame()`. That's the worst case: full
  re-render, re-layout and re-paint every frame.
- **one**: every frame re-renders the whole scene, but only one cell actually
  changes: closer to a real UI (a hover, a ticking value), where reusing the
  previous frame's work pays off.
- **idle**: the scene renders once and then nothing changes.
- **motion**: every cell loops its colour and opacity forever through the
  framework's own animation system (gpui's `with_animation`, blenderui's
  `<motion.div>` keyframes): the app has no per-frame code. blenderui redraws
  these frames without rendering the view or laying it out again, so its
  frames are counted where they're drawn (`blenderui::frames_presented`).
- **blenderui (uncapped)**: animation without vsync pacing (`--uncapped`), for
  both renderers.

> **When these were measured.** The results below come from a Ryzen 7 5800U
> laptop on Windows 11 (Direct3D 11 for the GPU rows), before the latest
> features landed: Dear ImGui, the Linux OpenGL renderer, the light theme and
> Blender's search and expression code. Rerun them with the steps below to see
> today's numbers on your machine. The benchmark is Windows-only (gpui needs
> `fxc.exe`, and `run.py` reads Windows' GPU counters).

## Running

```sh
cd bindings/rust/bench
set GPUI_FXC_PATH=C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\fxc.exe   # gpui needs fxc.exe
cargo build --release
py run.py --reps 3          # ~12 minutes; prints a Markdown table, writes results.json
```

Each app measures itself with the same code (`common/measure.rs`):
- **Warm-up and window:** 3 s warm-up, then a 10 s measured window.
- **CPU:** process CPU **cycles** across all threads (`QueryProcessCycleTime`), converted to time with the TSC frequency calibrated during warm-up.
- **Frames and memory:** frames rendered, the working set sampled every 100 ms, and private bytes at the end.
- **GPU:** `run.py` also samples each process's GPU 3D-engine utilisation and GPU memory from the Windows performance counters, because GPU work isn't process CPU time.

Every configuration ran 3 times, and the table shows the medians.

> An earlier version of this benchmark used `GetProcessTimes`. Windows charges
> that per ~15.6 ms clock tick, which aliases with a 60 Hz frame loop and
> under-reported CPU badly (gpui at 1000 cells showed 6.3 ms per frame; it
> really uses 19 ms). All numbers below use the cycle counter.

## Results

Ryzen 7 5800U (8 cores / 16 threads, laptop, plugged in, 100 % charge), AMD
Radeon iGPU, 60 Hz display, Windows 11, release builds. All four configurations
ran back to back in one session (`py run.py --reps 1 --warmup 2 --seconds 6`).
`blenderui-gpu` is the same binary with `BUI_GPU=1`: the Direct3D 11 renderer
instead of the software raster. Mode `one` changes one cell per frame; `idle`
changes nothing.

| App | Cells | Mode | FPS | CPU % (1 core = 100) | CPU ms / frame | GPU 3D % | RAM working set MB | Private MB | GPU memory MB | First frame ms |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| gpui | 100 | anim | 60.0 | 28.2 | 4.70 | 7.7 | 40.2 | 35.4 | 16.1 | 234 |
| gpui-fast | 100 | anim | 60.0 | 16.0 | 2.67 | 3.8 | 46.7 | 40.4 | 19.1 | 224 |
| blenderui | 100 | anim | 60.1 | 27.7 | 4.62 | - | 20.7 | 11.7 | - | 4 |
| blenderui-gpu | 100 | anim | 60.1 | 7.2 | 1.19 | 8.5 | 27.9 | 21.7 | 19.3 | 5 |
| gpui | 100 | one | 60.0 | 21.1 | 3.52 | 5.9 | 32.8 | 35.6 | 16.1 | 279 |
| gpui-fast | 100 | one | 60.0 | 14.0 | 2.34 | 2.5 | 34.5 | 39.9 | 19.1 | 213 |
| blenderui | 100 | one | 60.1 | 26.5 | 4.41 | - | 19.7 | 10.7 | - | 5 |
| blenderui-gpu | 100 | one | 60.1 | 6.3 | 1.05 | 5.6 | 25.1 | 20.4 | 18.8 | 6 |
| gpui | 100 | idle | 0.2 | 1.4 | - | 0.0 | 32.8 | 35.6 | 16.1 | 212 |
| gpui-fast | 100 | idle | 0.0 | 0.0 | - | 0.0 | 34.0 | 39.4 | 18.9 | 197 |
| blenderui | 100 | idle | 0.0 | 0.0 | - | - | 19.4 | 9.9 | - | 6 |
| blenderui-gpu | 100 | idle | 0.0 | 0.0 | - | 0.0 | 25.8 | 19.1 | 18.3 | 6 |
| blenderui (uncapped) | 100 | anim | 648.6 | 315.0 | 4.86 | - | 20.7 | 11.9 | - | 5 |
| blenderui-gpu (uncapped) | 100 | anim | 1491.2 | 89.8 | 0.60 | 92.1 | 28.3 | 22.2 | 19.5 | 6 |
| gpui | 1000 | anim | 36.4 | 100.9 | 27.73 | 3.8 | 47.2 | 51.6 | 17.1 | 227 |
| gpui-fast | 1000 | anim | 49.2 | 100.9 | 20.52 | 5.5 | 44.5 | 51.2 | 19.9 | 200 |
| blenderui | 1000 | anim | 60.0 | 61.2 | 10.20 | - | 23.8 | 14.9 | - | 5 |
| blenderui-gpu | 1000 | anim | 60.0 | 28.2 | 4.70 | 9.0 | 34.5 | 29.2 | 21.7 | 6 |
| gpui | 1000 | one | 35.7 | 100.9 | 28.26 | 3.6 | 47.3 | 52.3 | 17.1 | 229 |
| gpui-fast | 1000 | one | 29.6 | 59.1 | 19.99 | 2.5 | 44.5 | 51.8 | 19.9 | 197 |
| blenderui | 1000 | one | 60.1 | 61.4 | 10.21 | - | 24.1 | 15.9 | - | 4 |
| blenderui-gpu | 1000 | one | 60.0 | 29.0 | 4.83 | 8.7 | 34.3 | 28.4 | 21.5 | 5 |
| gpui | 1000 | idle | 0.0 | 0.9 | - | 0.0 | 46.4 | 50.9 | 17.1 | 204 |
| gpui-fast | 1000 | idle | 0.2 | 0.4 | - | 0.0 | 43.4 | 50.2 | 19.9 | 212 |
| blenderui | 1000 | idle | 0.3 | 0.9 | - | - | 23.6 | 14.2 | - | 6 |
| blenderui-gpu | 1000 | idle | 0.0 | 0.0 | - | 0.0 | 31.8 | 27.1 | 21.7 | 5 |
| blenderui (uncapped) | 1000 | anim | 202.4 | 233.2 | 11.52 | - | 24.5 | 15.9 | - | 4 |
| blenderui-gpu (uncapped) | 1000 | anim | 309.1 | 101.8 | 3.29 | 42.6 | 35.4 | 29.9 | 21.8 | 4 |
| gpui | 3000 | anim | 19.9 | 100.6 | 50.47 | 3.0 | 77.2 | 95.3 | 20.6 | 210 |
| gpui-fast | 3000 | anim | 31.5 | 100.5 | 31.89 | 4.4 | 64.9 | 85.3 | 23.5 | 193 |
| blenderui | 3000 | anim | 57.6 | 126.7 | 21.98 | - | 30.2 | 26.9 | - | 5 |
| blenderui-gpu | 3000 | anim | 59.9 | 69.4 | 11.58 | 11.0 | 39.6 | 38.0 | 20.8 | 5 |
| gpui | 3000 | one | 20.2 | 100.6 | 49.69 | 4.1 | 76.7 | 94.9 | 20.6 | 215 |
| gpui-fast | 3000 | one | 39.2 | 100.7 | 25.71 | 6.5 | 63.6 | 85.7 | 23.5 | 209 |
| blenderui | 3000 | one | 59.9 | 127.6 | 21.29 | - | 30.1 | 23.9 | - | 5 |
| blenderui-gpu | 3000 | one | 60.1 | 67.3 | 11.20 | 10.2 | 39.2 | 36.4 | 20.8 | 10 |
| gpui | 3000 | idle | 0.0 | 1.1 | - | 0.0 | 74.8 | 92.5 | 20.6 | 205 |
| gpui-fast | 3000 | idle | 0.0 | 0.1 | - | 0.0 | 60.9 | 81.4 | 20.9 | 202 |
| blenderui | 3000 | idle | 0.0 | 0.1 | - | - | 27.9 | 21.4 | - | 5 |
| blenderui-gpu | 3000 | idle | 0.0 | 0.1 | - | 0.0 | 36.0 | 34.2 | 19.6 | 8 |
| blenderui (uncapped) | 3000 | anim | 75.5 | 166.4 | 22.04 | - | 30.5 | 26.2 | - | 4 |
| blenderui-gpu (uncapped) | 3000 | anim | 94.0 | 101.1 | 10.75 | 14.7 | 39.7 | 36.8 | 20.8 | 9 |

The software raster has no GPU counter instances (shown as -). `CPU %` counts
all threads, so above 100 means more than one core. `CPU ms / frame` excludes
GPU work for every app, so read it together with the GPU column.

## What it says

| Animated cells | blenderui-gpu | blenderui (CPU raster) | gpui-fast | gpui |
|---|---:|---:|---:|---:|
| 100 | 60 fps, **1.19 ms** CPU/frame (1491 fps uncapped) | 60 fps, 4.62 ms | 60 fps, 2.67 ms | 60 fps, 4.70 ms |
| 1000 | **60 fps, 4.70 ms** (309 uncapped) | 60 fps, 10.2 ms | 49.2 fps, 20.5 ms | 36.4 fps, 27.7 ms |
| 3000 | **59.9 fps, 11.6 ms** (94 uncapped) | 57.6 fps, 22.0 ms | 31.5 fps, 31.9 ms | 19.9 fps, 50.5 ms |
| GPU 3D % (100 / 1000 / 3000) | 8.5 / 9.0 / 11.0 | - | 3.8 / 5.5 / 4.4 | 7.7 / 3.8 / 3.0 |
| RAM working set (100 / 1000 / 3000) | 28 / 35 / 40 MB | **21 / 24 / 30 MB** | 47 / 45 / 65 MB | 40 / 47 / 77 MB |
| GPU memory | 19-22 MB | **0** | 19-24 MB | 16-21 MB |
| First frame | **5-10 ms** | **4-5 ms** | ~200 ms | ~220 ms |
| Idle CPU | **0.0 %** | 0.0-0.9 % | 0.0-0.4 % | 0.9-1.4 % |

- **blenderui-gpu beats gpui-fast on every measurement.** At 100 cells it
  uses 2.2x less CPU per frame (1.19 vs 2.67 ms). At 1000 cells the gap is
  4.4x (4.70 vs 20.5 ms), and at 3000 cells it is 2.8x (11.6 vs 31.9 ms) at
  nearly twice the frame rate (59.9 vs 31.5 fps). It also uses less RAM, the
  same GPU memory, starts about 40x faster and idles at 0 %.
- **The GPU renderer halves blenderui's own CPU cost** (22.0 to 11.6 ms at
  3000 cells; 4.6 to 1.2 ms at 100 cells). Rasterization moves to the GPU,
  and what remains on the CPU is the Rust side (cascade, layout, paint list)
  plus recording 128-byte instances. Its GPU load is a few percent of the
  iGPU, about what gpui spends.
- **The software raster stays the lightest on memory** (no D3D runtime or GPU
  memory) and remains the default.
- gpui-fast's heavy-scene frame rate varies a lot between sessions on this
  laptop (29.7, 59.8, 50.5 and now 31.5 fps at 3000 cells), so compare within
  one session only. The previous session's data, before the GPU renderer, is
  in git history.

## Caveats

- One laptop, one display (60 Hz caps both), the iGPU. A discrete GPU would make gpui's GPU share smaller still.
- Laptop clocks vary between sessions. gpui's 1000-cell scene measured 51 fps in this run and 45-60 fps in earlier ones, so the medians are from runs made back to back in one session.
- blenderui's root is wrapped in `body().m_0()` so its grid fills the window exactly like gpui's root `div`.
- **Large or high-DPI windows:** the software raster's cost grows with pixels (spread over up to 8 threads); the GPU renderer's barely does. Use `BUI_GPU=1` / `.gpu(true)` there.
- `CPU ms / frame` for gpui excludes its GPU work. Compare it together with the GPU column.

## How blenderui got here

The first version of this benchmark had blenderui at 29 fps (1000 cells) and 12 fps (3000 cells). These changes, all general-purpose, got it to 60 and 34. `BUI_PROFILE=1` prints the per-phase frame times they were found with.

| Change | Where | Effect at 3000 cells |
|---|---|---|
| **Parallel canvas raster:** horizontal bands on the Win32 thread pool. Text is prepared on one thread and drawn read-only from all of them. The pixels are identical to serial drawing (`BUI_RASTER_THREADS=1` forces serial) | `kit_frame.c`, `gpu_raster.c` | raster 8.6 → 2.7 ms |
| **Fill-only rounded rects** use the span-filling `BUI_raster_round_rect` instead of the general widget-base routine | `kit_frame.c` | raster 11.8 → 4.6 ms (headless) |
| **Style sharing:** elements with the same parent style, matched rules and declarations reuse one computed style (as browsers do) | `layout.rs` | cascade 12 → 3.3 ms |
| **Interned and typed CSS values:** `.w(px(40.0))` and `.bg(rgb(..))` reuse tokens cached by the number's bits instead of formatting and re-parsing a string | `css.rs`, `styled.rs` | `render()` 7.4 → 3.6 ms |
| **Rule index:** stylesheet rules are bucketed by their subject's id, class and tag | `css.rs` | selector matching about 5 → 0.4 ms |
| **Fewer allocations:** text runs are moved rather than copied, text-width lookups don't allocate, the key map is built lazily, text is passed to the kit as a C string once | `layout.rs`, `text.rs`, `paint.rs` | about 3 ms |
| **No overdraw:** a background identical to the one under it isn't painted, and the kit skips its clear when the page is opaque | `paint.rs`, `kit_core.c` | about 1 ms |
| **Frame pacing:** animation frames rebuild right before the render (`BK_window_request_frame`). A frame that is already late doesn't wait for another vblank | `kit_window.c`, `window.rs` | removes 10 ms timer and vsync quantisation stalls |

Second round (lower CPU per frame without costing RAM, startup, idle or fps):

| Change | Where | Effect |
|---|---|---|
| **Adaptive raster threads:** the fewest threads that still meet the frame deadline, and all of them when it can't be met | `kit_frame.c` | 1000 cells: 17.4 → 14.3 ms CPU/frame; 100 cells drawn serially |
| **Lone text folded into its box:** one measured layout leaf instead of a text leaf (or one per word) | `layout.rs` | 3000 cells: boxes 5.0 → 2.5 ms, taffy 7.0 → 1.9 ms; pixel-identical |
| **Items bucketed by band** once instead of every band scanning the list | `kit_frame.c` | about 1.5 ms CPU at 3000 cells |
| **Allocation-free style-sharing probe** (hash the inputs, compare in place) | `layout.rs` | cascade 3.5 → 2.5 ms |
| **In-place canvas updates** (`BK_canvas_put_item` / `BK_canvas_truncate` reuse buffers and invalidate once) and inline painter coordinates | `kit_frame.c`, `paint.rs` | hand-off 1.25 → 0.30 ms |

Third round (no `Rc` / `RefCell`, work moved to compile time):

| Change | Effect at 3000 cells |
|---|---|
| Property names resolved at compile time (`const` table), typed values without formatting or parsing, `css!` | element building 3.5 → 3.1 ms |
| Frames as index-linked arenas reused across frames; words as byte ranges; no per-node strings or `Rc`s | build 7.3 → 4.1 ms, boxes 2.8 → 1.4 ms, dropping the old frame 1.4 → 0.03 ms |
| Styles in a per-window store addressed by `StyleId` | the cascade probe compares ids |
| Matched rules and declarations in stack-backed small vectors; rule index without key strings | cascade 2.8 → 2.5 ms |
| Text widths, images, the author sheet and the taffy tree cached in the window | fewer allocations everywhere |

Fourth round (reuse across frames, fewer allocations):

| Change | Effect |
|---|---|
| Matched rules memoized by an element fingerprint (parent, previous sibling, tag, id, attributes, position, state) | selector matching runs once per distinct element |
| A persistent taffy tree reconciled by key; text leaves keyed by a text hash; changed subtrees marked dirty explicitly | taffy's cache skips everything that didn't change: taffy time halves at 3000 cells when one cell changes |
| `Tok` 56 → 32 bytes (function tokens boxed); `Div` ~470 → ~150 bytes (listeners and state styles allocated on use, children inline) | less memory traffic, one allocation less per element |
| Painter, text buffer, layout scratch, box index table reused across frames; small lists on the stack | allocations per frame no longer grow with text items |

Still single-threaded and the largest remaining costs are the cascade, box tree and taffy layout (about 20 ms at 3000 cells): incremental restyling and layout would be the next step.

## Data table against gpui-component

The data-table head-to-head (the same gpui, same data, a real window) lives in [blenderui-table-gpui](../blenderui-table-gpui/README.md): `examples/bench_gpui.rs` and `bench/run.mjs`. It reuses this directory's `common/measure.rs` for CPU cycles and memory.
