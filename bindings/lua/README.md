# bui-lua — Lua 5.4 Host for the BUI Toolkit

Standalone executable that embeds Lua 5.4 and exposes BUI's full retained-mode UI
toolkit as a native Lua module.

## Build

```sh
cmake -S . -B build -DBUI_BUILD_LUA_BINDINGS=ON
cmake --build build --target bui-lua
```

The binary is `build/bindings/lua/bui-lua`.

## Install with LuaRocks

```sh
luarocks install blenderui     # Lua 5.4; needs CMake and a C and C++20 compiler
lua -e 'local bui = require("bui"); print(bui.KIT_VERSION)'
```

On Linux it also needs the X11 development headers (`libx11-dev` on Debian and Ubuntu, `libX11-devel` on Fedora):
without them the kit has no native windows and `bui.Window` returns `nil`.
The rock builds the C kit and the loadable module (`_bui`) from the release tag and installs it with the
`bui.lua` wrapper. From a checkout: `luarocks make bindings/lua/blenderui-<version>-1.rockspec`.
(The CMake option `BUI_LUA_ROCK=ON` is what the rockspec sets: the module only, against the Lua that runs it.)

## Usage

### Script mode
```sh
./bui-lua app.lua          # run a Lua script
./bui-lua --headless test.lua  # run without a display
./bui-lua -e "print('hi')" # inline evaluation
```

### REPL mode
```sh
./bui-lua                  # interactive prompt
```
Type Lua expressions at the `bui>` prompt. Multi-line input is supported.
Call `bui.run()` to enter the BUI event loop (blocks until all windows close).

## API Overview

```lua
local bui = require("bui")

-- Node tree
local root = bui.Node("frame")       -- or: column, row, grid, box, panel, scroll
root:set("padding", 8)

local btn = bui.Node("button")       -- also: label, slider, toggle, checkbox, text,
btn:set("text", "Click me")          --   number, dropdown, progress, separator, image
btn:on(bui.EV_CLICK, function(self)  -- events: EV_CHANGE, EV_CLICK, EV_PRESS, EV_KEY...
    print("clicked!")
end)
root:add(btn)
btn:geom(bui.GEOM_PACK, {fill = 1})  -- geometry: pack, grid, place

-- Context + Window
local ctx = bui.Context(root)
local win = bui.Window(ctx, "Title", 320, 240)
win:show()
bui.run()  -- enter event loop (blocks)

-- Input forwarding (headless mode)
ctx:press(100, 50, 0)
ctx:release(100, 50, 0)
ctx:key("a", "a", 0)
```

## Constants

| Group | Prefix | Examples |
|-------|--------|---------|
| Events | `EV_` | `EV_CLICK`, `EV_CHANGE`, `EV_CALL`, `EV_KEY`, `EV_WHEEL` |
| Cursors | `CURSOR_` | `CURSOR_DEFAULT`, `CURSOR_HAND`, `CURSOR_TEXT` |
| Geometry | `GEOM_` | `GEOM_PACK`, `GEOM_GRID`, `GEOM_PLACE` |
| Modifiers | `MOD_` | `MOD_SHIFT`, `MOD_CTRL`, `MOD_ALT` |
| Image formats | `IMAGE_` | `IMAGE_PNG`, `IMAGE_JPEG`, `IMAGE_SVG` |
| Object-fit | `FIT_` | `FIT_FILL`, `FIT_CONTAIN`, `FIT_COVER` |
| Widget types | `WIDGET_` | `WIDGET_BUTTON`, `WIDGET_SLIDER`, `WIDGET_LABEL` |
| Widget states | `WS_` | `WS_HOVER`, `WS_PRESSED`, `WS_SELECTED` |
| Video states | `VIDEO_` | `VIDEO_PLAYING`, `VIDEO_PAUSED`, `VIDEO_ENDED` |
| Canvas items | `ITEM_` | `ITEM_RECT`, `ITEM_OVAL`, `ITEM_LINE`, `ITEM_TEXT` |

## Node types

`frame`, `column`, `row`, `grid`, `box`, `panel`, `tabs`, `sidetabs`, `scroll`,
`label`, `separator`, `progress`, `button`, `toggle`, `checkbox`, `decorator`,
`text`, `search`, `hotkey`, `number`, `slider`, `dropdown`, `enum`, `swatch`,
`popover`, `menubutton`, `menubar`, `image`, `video`, `textarea`, `custom`,
`canvas`, `list`, `tree`, `gridview`, `ramp`, `curve`, `normal`, `histogram`,
`radio`, `meter`, `filebrowser`

## Menus

Menu items are plain tables, matching the Python binding's `MenuItem`:

```lua
mb = bui.Node("menubutton")
mb:set("text", "File")
mb:set_menus({
  {text = "New", icon = "FILE_NEW", fn = new_file},           -- icon: name or id
  {text = "Bold", checked = true, fn = toggle_bold},           -- checked/enabled: bool or number
  {text = "Save", shortcut = "Ctrl S", enabled = false},
  {text = "-"},                                                 -- separator
  {text = "Recent", sub = {                                    -- submenu
    {text = "doc1.blend", fn = open_recent},
  }},
})

-- A menubar builds one menubutton child per top-level item (like Python's MenuBar)
bar = bui.Node("menubar")
bar:set_menus({
  {text = "File", sub = {{text = "New", fn = new_file}, {text = "Quit", fn = quit}}},
  {text = "Edit", sub = {{text = "Undo", fn = undo}}},
})

-- Popup menus from code
ctx:open_menu(owner, {{text = "Cut", fn = cut}}, "Edit", 120, 90)
ctx:open_pie({{text = "W", fn = w}, {text = "E", fn = e}}, "Direction")
```

`fn` callbacks live in the Lua registry for exactly as long as the kit holds a
copy of the item (menu button, open popup, search filter) — they are released
automatically when the items are replaced or the node/context is freed.

## Animation (`bui.gsap`, `bui.motion`)

GSAP's and Motion's APIs on the kit's animation engine (`kit/include/BUI_anim.h`), the
same engine every BUI language uses. A call's tables are read once, into the engine; a
frame runs no Lua unless a callback such as `onUpdate` was given.

```lua
local gsap, motion = bui.gsap, bui.motion

-- GSAP: to / from / fromTo / set; targets are a node or a list of nodes
local t = gsap.to(slider, {value = 80, duration = 0.6, ease = "power2.out",
                           repeat_ = 1, yoyo = true, onComplete = function() print("done") end})
gsap.from(progress, {factor = 0, duration = 1})
gsap.fromTo(frame, {backgroundColor = "#202020"}, {backgroundColor = "#4772b3"})
gsap.set(swatch, {color = {1, 0.5, 0, 1}})
gsap.to({a, b, c}, {value = 100, stagger = {each = 0.1, from = "center"}})
t:pause(); t:resume(); t:reverse(); t:restart(); t:seek(0.3); t:progress(0.5)
t:timeScale(2); t:kill(); t:isActive(); t:paused(true); t:reversed()

-- timelines: GSAP's position parameter, labels, calls, defaults
local tl = gsap.timeline({defaults = {duration = 0.5, ease = "none"}, repeat_ = 1})
tl:to(a, {value = 100}):to(b, {value = 100}, "<0.2"):addLabel("mid")
  :to(c, {value = 50}, "mid+=0.25"):call(print, {"halfway"}, "mid")

-- Motion: keyframes, times, per-segment eases, springs, stagger
motion.animate(slider, {value = {0, 100, 40}}, {duration = 1, times = {0, 0.3, 1}})
motion.animate(slider, {value = 75}, {type = "spring", stiffness = 300, damping = 20})
motion.animate(items, {value = 1}, {delay = motion.stagger(0.05, {from = "last"})})

gsap.delayedCall(1, fn); gsap.killTweensOf(slider, "value")
gsap.registerEase("myEase", {0.2, 0.8, 0.4, 1}); gsap.parseEase("back.out(2)")(0.5)
```

- **Properties**: `value`, `color`, `backgroundColor` (a frame's `bg`) and the numbers
  `node:get` reads (`factor`, `min`, `max`, `height`, `padding`, `scroll`, ...). Kit widgets
  have no draw transforms or opacity, so `x`, `y`, `scale`, `rotate`, `opacity` are errors.
- **Values**: numbers, strings (`"+=30"`, `"50%"`, `"#ff8800"`, `"rgb(255 128 0)"`),
  colors as `{r, g, b[, a]}`, and arrays for keyframes.
- **Options** (GSAP's camelCase or snake_case; `repeat_` or `["repeat"]` for Lua's keyword):
  `duration`, `delay`, `repeat`, `repeatDelay`, `repeatType`, `yoyo`, `timeScale`, `paused`,
  `reversed`, `ease` (every GSAP, Motion, CSS and Blender name, or `{x1, y1, x2, y2}`),
  `overwrite`, `immediateRender`, `stagger`, `times`, `type` (`tween`, `spring`, `inertia`),
  the spring's `stiffness`, `damping`, `mass`, `velocity`, `bounce`, `visualDuration`,
  `restDelta`, `restSpeed`, inertia's `power`, `timeConstant`, `min`, `max`, and the
  callbacks `onStart`, `onUpdate`, `onComplete`, `onRepeat`, `onReverseComplete`,
  `onInterrupt`.
- **Errors at the call**: an unknown ease, property or option, a value of the wrong kind,
  a freed node: each is a Lua error naming it.
- **Callbacks** live in the registry for exactly as long as their motion: a tween whose
  handle is collected plays on, and lets go of its callbacks when it is done.
- **Clock**: the kit ticks animations in its frames. A host with its own clock (or a
  headless test) calls `gsap.ticker.useHostClock(true)` and `gsap.ticker.tick(seconds)`.

## Examples

```sh
./bui-lua bindings/lua/examples/hello.lua         # minimal button (entry point)
./bui-lua bindings/lua/examples/counter.lua       # counter with increment/decrement/reset
./bui-lua bindings/lua/examples/todos.lua         # todo list manager
./bui-lua bindings/lua/examples/gallery.lua       # widget gallery (button, toggle, checkbox,
                                                   #   text, search, number, slider, dropdown,
                                                   #   progress, label)
./bui-lua bindings/lua/examples/forms.lua          # full form controls (ALL widget types:
                                                   #   button, toggle, checkbox, text, search,
                                                   #   hotkey, number, slider, dropdown, enum,
                                                   #   radio, swatch, progress, meter, color,
                                                   #   decorator)
./bui-lua bindings/lua/examples/dashboard.lua     # render-farm dashboard with tabs, panels,
                                                   #   progress bars, node status, settings
./bui-lua bindings/lua/examples/anim.lua           # pump-driven animation loop (progress,
                                                   #   color cycling, wave slider, frame
                                                   #   counter)
./bui-lua bindings/lua/examples/lua_table_editor.lua  # full-featured interactive Lua table
                                                       # editor (tree view, type-aware editing,
                                                       # collapsible panel, undo/redo,
                                                       # search/filter, export, metatable
                                                       # display, Ctrl+Z / Ctrl+Shift+Z)
 ./bui-lua bindings/lua/examples/ph_nutrients.lua      # soil pH / nutrient availability chart
                                                        # (canvas-drawn pH scale, nutrient
                                                        # availability bands, two overlapping
                                                        # curves, redraws on window resize via
                                                        # EV_CONFIGURE, editable via number
                                                        # fields and sliders; a Table tab edits
                                                        # the same data as a Lua table — tree
                                                        # view, Apply, add/delete, undo/redo)
```
