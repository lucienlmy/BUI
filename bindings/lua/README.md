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
