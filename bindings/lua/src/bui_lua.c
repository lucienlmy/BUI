/* BUI Lua binding: module entry point and constant registration.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <ctype.h>

#include "bui_internal.h"

/* Numeric property keys (for node:get() dispatch) */
static const char *g_numeric_keys[] = {
  "value", "min", "max", "step", "precision", "padding", "margin",
  "width", "height", "reqwidth", "reqheight", "fill", "expand",
  "side", "padx0", "padx1", "pady0", "pady1", "ipadx", "ipady",
  "row", "column", "rowspan", "columnspan", "sticky",
  "relx", "rely", "relwidth", "relheight",
  "anchor", "scale", "factor", "progress", "value_min", "value_max",
  "mode", "multiple", "hidden", "sort", "state", "count", "filter", "execute",
  "dialog_width", "dialog_height",
  "icon", "checked", "enabled", "selected", "open", "invert",
  "rows", "cols", "wrap", "readonly", "password",
  "x", "y", "w", "h",
  "html", "editable",
  "weight", "scroll", "tile", "current", "default",
  NULL
};

int luabui_is_numeric_key(const char *key) {
  for (int i = 0; g_numeric_keys[i]; i++) {
    if (strcmp(key, g_numeric_keys[i]) == 0) return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Global functions                                                   */
/* ------------------------------------------------------------------ */

static int l_run(lua_State *L) {
  (void)L;
  BK_window_run();
  return 0;
}

static int l_pump(lua_State *L) {
  int open = BK_window_pump();
  lua_pushinteger(L, open);
  return 1;
}

static int l_set_scale(lua_State *L) {
  BK_set_scale((float)luaL_checknumber(L, 1));
  return 0;
}

static int l_set_theme(lua_State *L) {
  int ok = BK_set_theme(luaL_checkstring(L, 1));
  lua_pushinteger(L, ok);
  return 1;
}

/* bui.load_theme(source): register the themes of a theme file (a path) or of theme JSON text.
 * Returns how many were added, or nil and the message (the line and column of a mistake). */
static int l_load_theme(lua_State *L) {
  const char *src = luaL_checkstring(L, 1);
  while (isspace((unsigned char)*src)) src++;
  const int n = (*src == '{') ? BK_theme_load_json(lua_tostring(L, 1)) : BK_theme_load_file(lua_tostring(L, 1));
  if (n < 0) {
    lua_pushnil(L);
    lua_pushstring(L, BK_theme_error());
    return 2;
  }
  lua_pushinteger(L, n);
  return 1;
}

/* bui.themes(): the names of the loaded themes. */
static int l_themes(lua_State *L) {
  lua_newtable(L);
  for (int i = 0; BK_theme_at(i); i++) {
    lua_pushstring(L, BK_theme_at(i));
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

/* bui.set_theme_color(key, r, g, b [, a]): one color of the active theme (0..1); false for an unknown key. */
static int l_set_theme_color(lua_State *L) {
  const float rgba[4] = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3),
                         (float)luaL_checknumber(L, 4), (float)luaL_optnumber(L, 5, 1.0)};
  lua_pushboolean(L, BK_theme_set_color(luaL_checkstring(L, 1), rgba));
  return 1;
}

/* bui.set_theme_value(key, number | boolean): roundness, border.width, corners.*, ...; false for an unknown key. */
static int l_set_theme_value(lua_State *L) {
  const double v = lua_isboolean(L, 2) ? (lua_toboolean(L, 2) ? 1.0 : 0.0) : luaL_checknumber(L, 2);
  lua_pushboolean(L, BK_theme_set_num(luaL_checkstring(L, 1), v));
  return 1;
}

/* bui.theme_value(key): a number of the active theme, or nil. */
static int l_theme_value(lua_State *L) {
  double v;
  if (BK_theme_num(luaL_checkstring(L, 1), &v)) {
    lua_pushnumber(L, v);
    return 1;
  }
  lua_pushnil(L);
  return 1;
}

static int l_theme_name(lua_State *L) {
  lua_pushstring(L, BK_theme_name());
  return 1;
}

static int l_theme_color(lua_State *L) {
  float rgba[4];
  if (BK_theme_color(luaL_checkstring(L, 1), rgba)) {
    lua_pushnumber(L, rgba[0]);
    lua_pushnumber(L, rgba[1]);
    lua_pushnumber(L, rgba[2]);
    lua_pushnumber(L, rgba[3]);
    return 4;
  }
  return 0;
}

static int l_unit(lua_State *L) {
  lua_pushinteger(L, BK_unit());
  return 1;
}

static int l_version(lua_State *L) {
  int major, minor, patch;
  bui_get_version(&major, &minor, &patch);
  lua_pushinteger(L, major);
  lua_pushinteger(L, minor);
  lua_pushinteger(L, patch);
  return 3;
}

static int l_eval(lua_State *L) {
  double result;
  if (BK_eval_number(luaL_checkstring(L, 1), &result)) {
    lua_pushnumber(L, result);
    return 1;
  }
  return 0;
}

static int l_quit(lua_State *L) {
  BK_window_quit();
  return 0;
}

static int l_window_supported(lua_State *L) {
  lua_pushboolean(L, BK_window_supported());
  return 1;
}

static int l_set_animations(lua_State *L) {
  BK_set_animations(lua_toboolean(L, 1));
  return 0;
}

static int l_icon_names(lua_State *L) {
  int n = BK_icon_list_count();
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    lua_pushstring(L, BK_icon_list_name(i));
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int l_icon_id(lua_State *L) {
  lua_pushinteger(L, BK_icon_id(luaL_checkstring(L, 1)));
  return 1;
}

static int l_icon_count(lua_State *L) {
  lua_pushinteger(L, BK_icon_count());
  return 1;
}

static int l_icon_name(lua_State *L) {
  int id = (int)luaL_checkinteger(L, 1);
  const char *name = BK_icon_name(id);
  if (name) lua_pushstring(L, name);
  else lua_pushnil(L);
  return 1;
}

static int l_image_probe(lua_State *L) {
  size_t len;
  const char *data = luaL_checklstring(L, 1, &len);
  BKImageInfo info;
  if (!BK_image_probe(data, len, &info)) { lua_pushnil(L); return 1; }
  lua_pushstring(L, BK_image_format_name(info.format));
  lua_pushinteger(L, info.width);
  lua_pushinteger(L, info.height);
  lua_pushinteger(L, info.frames);
  return 4;
}

static int l_image_probe_file(lua_State *L) {
  BKImageInfo info;
  if (!BK_image_probe_file(luaL_checkstring(L, 1), &info)) { lua_pushnil(L); return 1; }
  lua_pushstring(L, BK_image_format_name(info.format));
  lua_pushinteger(L, info.width);
  lua_pushinteger(L, info.height);
  lua_pushinteger(L, info.frames);
  return 4;
}

static int l_image_info(lua_State *L) {
  BKImageInfo info;
  if (!BK_image_info(luaL_checkstring(L, 1), &info)) { lua_pushnil(L); return 1; }
  lua_pushstring(L, BK_image_format_name(info.format));
  lua_pushinteger(L, info.width);
  lua_pushinteger(L, info.height);
  lua_pushinteger(L, info.frames);
  return 4;
}

static int l_image_set_threads(lua_State *L) {
  BK_image_set_threads((int)luaL_checkinteger(L, 1));
  return 0;
}

static int l_image_clear_cache(lua_State *L) {
  BK_image_clear_cache();
  return 0;
}

static int l_image_exit(lua_State *L) {
  BK_image_exit();
  return 0;
}

static int l_image_prefetch(lua_State *L) {
  const char *src = luaL_checkstring(L, 1);
  int w = (int)luaL_optinteger(L, 2, 0);
  int h = (int)luaL_optinteger(L, 3, 0);
  int fit = (int)luaL_optinteger(L, 4, 0);
  BK_image_prefetch(src, w, h, fit);
  return 0;
}

static int l_video_backends(lua_State *L) {
  const char *s = BK_video_backends();
  lua_pushstring(L, s ? s : "");
  return 1;
}

static int l_image_stats(lua_State *L) {
  BKImageStats st;
  BK_image_stats(&st);
  lua_createtable(L, 0, 9);
  lua_pushinteger(L, (lua_Integer)st.bytes);    lua_setfield(L, -2, "bytes");
  lua_pushinteger(L, (lua_Integer)st.budget);   lua_setfield(L, -2, "budget");
  lua_pushinteger(L, st.entries);               lua_setfield(L, -2, "entries");
  lua_pushinteger(L, st.pending);               lua_setfield(L, -2, "pending");
  lua_pushinteger(L, st.decoded);               lua_setfield(L, -2, "decoded");
  lua_pushinteger(L, st.failed);                lua_setfield(L, -2, "failed");
  lua_pushinteger(L, st.cancelled);             lua_setfield(L, -2, "cancelled");
  lua_pushinteger(L, st.evicted);               lua_setfield(L, -2, "evicted");
  lua_pushnumber(L, st.decode_ms);              lua_setfield(L, -2, "decode_ms");
  return 1;
}

static int l_image_decode_frame(lua_State *L) {
  size_t len;
  const char *data = luaL_checklstring(L, 1, &len);
  int index = (int)luaL_checkinteger(L, 2);
  int max_w = (int)luaL_optinteger(L, 3, 0);
  int max_h = (int)luaL_optinteger(L, 4, 0);
  BKImBuf *ib = BK_imbuf_decode_frame(data, len, index, max_w, max_h);
  if (!ib) { lua_pushnil(L); return 1; }
  int w, h;
  BK_imbuf_size(ib, &w, &h);
  const char *fmt = BK_image_format_name(BK_imbuf_format(ib));
  /* Return pixel data as Lua string */
  lua_pushstring(L, fmt);
  lua_pushinteger(L, w);
  lua_pushinteger(L, h);
  /* Allocate buffer and copy RGBA pixels */
  luaL_Buffer buf;
  luaL_buffinit(L, &buf);
  size_t pixsize = (size_t)w * h * 4;
  char *pix = (char *)malloc(pixsize);
  if (pix) {
    BK_imbuf_rgba(ib, 0, (unsigned char *)pix);
    luaL_addlstring(&buf, pix, pixsize);
    free(pix);
  }
  luaL_pushresult(&buf);
  BK_imbuf_free(ib);
  return 4; /* format, width, height, rgba_bytes */
}

static int l_image_poll(lua_State *L) {
  lua_pushinteger(L, BK_image_poll());
  return 1;
}

static int l_image_set_budget(lua_State *L) {
  BK_image_set_budget((size_t)luaL_checkinteger(L, 1));
  return 0;
}

static int l_set_font(lua_State *L) {
  BK_set_font(luaL_checkstring(L, 1));
  return 0;
}

static int l_link_fields(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  int n = (int)lua_rawlen(L, 1);
  BKNode **fields = (BKNode **)malloc((size_t)n * sizeof(BKNode *));
  for (int i = 0; i < n; i++) {
    lua_rawgeti(L, 1, i + 1);
    luabui_Node *ud = (luabui_Node *)luaL_checkudata(L, -1, LUABUI_NODE_MT);
    fields[i] = ud->node;
    lua_pop(L, 1);
  }
  BK_link_fields(fields, n);
  free(fields);
  return 0;
}

static int l_scale(lua_State *L) {
  lua_pushnumber(L, BK_scale());
  return 1;
}

/* ------------------------------------------------------------------ */
/* Factory functions                                                  */
/* ------------------------------------------------------------------ */

static int l_new_node(lua_State *L) {
  const char *type = luaL_checkstring(L, 1);
  BKNode *node = BK_node_new(type);
  if (!node) { lua_pushnil(L); return 1; }

  luabui_Node *ud = (luabui_Node *)lua_newuserdata(L, sizeof(luabui_Node));
  ud->node = node;
  luaL_getmetatable(L, LUABUI_NODE_MT);
  lua_setmetatable(L, -2);

  /* Create uservalue table for event callbacks */
  lua_newtable(L);
  lua_setuservalue(L, -2);

  luabui_map_node(L, node, -1);
  return 1;
}

static int l_new_context(lua_State *L) {
  luabui_Node *ud = CHECK_NODE(L, 1);
  if (!ud->node) { lua_pushnil(L); return 1; }

  BKContext *ctx = BK_context_new(ud->node);
  if (!ctx) { lua_pushnil(L); return 1; }

  luabui_Context *cud = (luabui_Context *)lua_newuserdata(L, sizeof(luabui_Context));
  cud->ctx = ctx;
  luaL_getmetatable(L, LUABUI_CONTEXT_MT);
  lua_setmetatable(L, -2);

  /* Set up host callbacks with Lua state as data */
  luabui_init_host_callbacks(L, ctx);
  return 1;
}

static int l_new_window(lua_State *L) {
  luabui_Context *cud = CHECK_CTX(L, 1);
  if (!cud->ctx) { lua_pushnil(L); return 1; }

  /* Headless mode: --headless flag or BUI_HEADLESS env var skips native windows */
  const char *hl = getenv("BUI_HEADLESS");
  if (hl && hl[0] == '1') { lua_pushnil(L); return 1; }

  const char *title = luaL_checkstring(L, 2);
  int w = (int)luaL_checkinteger(L, 3);
  int h = (int)luaL_checkinteger(L, 4);

  if (!BK_window_supported()) { lua_pushnil(L); return 1; }

  BKWindow *win = BK_window_new(cud->ctx, title, w, h);
  if (!win) { lua_pushnil(L); return 1; }

  luabui_Window *wud = (luabui_Window *)lua_newuserdata(L, sizeof(luabui_Window));
  wud->win = win;
  wud->L = L;
  wud->on_close_ref = LUA_NOREF;
  wud->on_timer_ref = LUA_NOREF;
  wud->on_frame_ref = LUA_NOREF;
  wud->self_ref = LUA_NOREF;
  wud->depth = 0;
  wud->zombie = NULL;
  lua_pushvalue(L, 1); /* the window draws this context: keep it alive as long as the window is */
  wud->ctx_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  luaL_getmetatable(L, LUABUI_WINDOW_MT);
  lua_setmetatable(L, -2);
  return 1;
}

static int l_new_treeitem(lua_State *L) {
  const char *label = luaL_checkstring(L, 1);
  int icon = (int)luaL_optinteger(L, 2, 0);

  BKTreeItem *item = BK_tree_item_new(label, icon);
  if (!item) { lua_pushnil(L); return 1; }

  luabui_TreeItem *tud = (luabui_TreeItem *)lua_newuserdata(L, sizeof(luabui_TreeItem));
  tud->item = item;
  luaL_getmetatable(L, LUABUI_TREEITEM_MT);
  lua_setmetatable(L, -2);
  luabui_map_treeitem(L, item, -1);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Module entry point                                                 */
/* ------------------------------------------------------------------ */

static int l_headless(lua_State *L) {
  const char *hl = getenv("BUI_HEADLESS");
  lua_pushboolean(L, hl && hl[0] == '1');
  return 1;
}

static int l_set_headless(lua_State *L) {
  int v = lua_toboolean(L, 1);
  setenv("BUI_HEADLESS", v ? "1" : "0", 1);
  return 0;
}

static const struct luaL_Reg bui_module[] = {
  {"Node",      l_new_node},
  {"Context",   l_new_context},
  {"Window",    l_new_window},
  {"TreeItem",  l_new_treeitem},
  {"run",       l_run},
  {"pump",      l_pump},
  {"quit",      l_quit},
  {"headless",  l_headless},
  {"set_headless", l_set_headless},
  {"window_supported", l_window_supported},
  {"set_animations", l_set_animations},
  {"set_scale", l_set_scale},
  {"scale",     l_scale},
  {"set_theme", l_set_theme},
  {"load_theme", l_load_theme},
  {"themes", l_themes},
  {"set_theme_color", l_set_theme_color},
  {"set_theme_value", l_set_theme_value},
  {"theme_value", l_theme_value},
  {"set_font",  l_set_font},
  {"theme_name",l_theme_name},
  {"theme_color",l_theme_color},
  {"unit",      l_unit},
  {"version",   l_version},
  {"eval",      l_eval},
  {"icon_names",   l_icon_names},
  {"icon_id",      l_icon_id},
  {"icon_count",   l_icon_count},
  {"icon_name",    l_icon_name},
  {"image_probe",      l_image_probe},
  {"image_probe_file", l_image_probe_file},
  {"image_info",       l_image_info},
  {"image_stats",      l_image_stats},
  {"image_poll",       l_image_poll},
  {"image_set_budget", l_image_set_budget},
  {"image_set_threads", l_image_set_threads},
  {"image_clear_cache", l_image_clear_cache},
  {"image_exit",       l_image_exit},
  {"image_prefetch",   l_image_prefetch},
  {"image_decode", l_image_decode},
  {"image_decode_frame", l_image_decode_frame},
  {"image_load",   l_image_load},
  {"link_fields", l_link_fields},
  {"video_backends", l_video_backends},
  {"Video",        l_video_new},
  {NULL, NULL}
};

BUI_LUA_ENTRY int luaopen_bui(lua_State *L) {
  /* Version check: refuse to load if the kit version doesn't match the header */
  int kit_ver = BK_version();
  int hdr_ver = BUI_KIT_VERSION;
  if (kit_ver != hdr_ver) {
    int kmaj = kit_ver / 10000, kmin = (kit_ver % 10000) / 100, kpat = kit_ver % 100;
    int hmaj = hdr_ver / 10000, hmin = (hdr_ver % 10000) / 100, hpat = hdr_ver % 100;
    lua_pushfstring(L, "bui: kit version mismatch (runtime %d.%d.%d, header %d.%d.%d). "
                       "Recompile the binding.\n"
                       "Expected \"%s\", linked \"%s\".",
                     kmaj, kmin, kpat, hmaj, hmin, hpat,
                     BUI_KIT_VERSION_STRING, BK_version_string());
    return lua_error(L);
  }

  /* Menu/hotkey ref hooks (retain/release), as in the Python binding */
  luabui_set_ref_state(L);

  /* Weak-keyed node map in registry */
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "k");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, -2);
  lua_setfield(L, LUA_REGISTRYINDEX, LUABUI_NODE_MAP);

  /* Weak-keyed tree item map in registry */
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "k");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, -2);
  lua_setfield(L, LUA_REGISTRYINDEX, LUABUI_TREEITEM_MAP);

  /* Register types */
  luabui_register_node(L);
  luabui_register_context(L);
  luabui_register_window(L);
  luabui_register_treeitem(L);
  luabui_register_imbuf(L);
  luabui_register_video(L);

  /* Module table */
  lua_newtable(L);
  luaL_setfuncs(L, bui_module, 0);
  luabui_register_anim(L);

  /* Version constants */
  lua_pushstring(L, BUI_KIT_VERSION_STRING); lua_setfield(L, -2, "KIT_VERSION");
  {
    int vmaj, vmin, vpat;
    bui_get_version(&vmaj, &vmin, &vpat);
    char vbuf[32];
    int n = snprintf(vbuf, sizeof(vbuf), "%d.%d.%d", vmaj, vmin, vpat);
    lua_pushlstring(L, vbuf, (size_t)n);
    lua_setfield(L, -2, "_VERSION");
  }

  /* Event constants */
  lua_pushinteger(L, BK_EV_CHANGE); lua_setfield(L, -2, "EV_CHANGE");   lua_pushinteger(L, BK_EV_CLICK); lua_setfield(L, -2, "EV_CLICK");
  lua_pushinteger(L, BK_EV_CALL); lua_setfield(L, -2, "EV_CALL");     lua_pushinteger(L, BK_EV_RENAME); lua_setfield(L, -2, "EV_RENAME");
  lua_pushinteger(L, BK_EV_ACTIVATE); lua_setfield(L, -2, "EV_ACTIVATE"); lua_pushinteger(L, BK_EV_TOGGLE); lua_setfield(L, -2, "EV_TOGGLE");
  lua_pushinteger(L, BK_EV_BROWSE); lua_setfield(L, -2, "EV_BROWSE");   lua_pushinteger(L, BK_EV_PRESS); lua_setfield(L, -2, "EV_PRESS");
  lua_pushinteger(L, BK_EV_RELEASE); lua_setfield(L, -2, "EV_RELEASE");  lua_pushinteger(L, BK_EV_DOUBLE); lua_setfield(L, -2, "EV_DOUBLE");
  lua_pushinteger(L, BK_EV_MOTION); lua_setfield(L, -2, "EV_MOTION");   lua_pushinteger(L, BK_EV_ENTER); lua_setfield(L, -2, "EV_ENTER");
  lua_pushinteger(L, BK_EV_LEAVE); lua_setfield(L, -2, "EV_LEAVE");    lua_pushinteger(L, BK_EV_KEY); lua_setfield(L, -2, "EV_KEY");
  lua_pushinteger(L, BK_EV_WHEEL); lua_setfield(L, -2, "EV_WHEEL");    lua_pushinteger(L, BK_EV_SUBMIT); lua_setfield(L, -2, "EV_SUBMIT");
  lua_pushinteger(L, BK_EV_VIEW); lua_setfield(L, -2, "EV_VIEW");     lua_pushinteger(L, BK_EV_SCROLL); lua_setfield(L, -2, "EV_SCROLL");
  lua_pushinteger(L, BK_EV_CONFIGURE); lua_setfield(L, -2, "EV_CONFIGURE");lua_pushinteger(L, BK_EV_KEYRELEASE); lua_setfield(L, -2, "EV_KEYRELEASE");

  /* Cursor constants */
  lua_pushinteger(L, BK_CURSOR_DEFAULT); lua_setfield(L, -2, "CURSOR_DEFAULT");  lua_pushinteger(L, BK_CURSOR_EW); lua_setfield(L, -2, "CURSOR_EW");
  lua_pushinteger(L, BK_CURSOR_NS); lua_setfield(L, -2, "CURSOR_NS");       lua_pushinteger(L, BK_CURSOR_TEXT); lua_setfield(L, -2, "CURSOR_TEXT");
  lua_pushinteger(L, BK_CURSOR_CROSSHAIR); lua_setfield(L, -2, "CURSOR_CROSSHAIR");lua_pushinteger(L, BK_CURSOR_HAND); lua_setfield(L, -2, "CURSOR_HAND");
  lua_pushinteger(L, BK_CURSOR_MOVE); lua_setfield(L, -2, "CURSOR_MOVE");     lua_pushinteger(L, BK_CURSOR_NONE); lua_setfield(L, -2, "CURSOR_NONE");

  /* Geometry constants */
  lua_pushinteger(L, BK_GEOM_NONE); lua_setfield(L, -2, "GEOM_NONE");  lua_pushinteger(L, BK_GEOM_PACK); lua_setfield(L, -2, "GEOM_PACK");
  lua_pushinteger(L, BK_GEOM_GRID); lua_setfield(L, -2, "GEOM_GRID");  lua_pushinteger(L, BK_GEOM_PLACE); lua_setfield(L, -2, "GEOM_PLACE");

  /* Modifier bits */
  lua_pushinteger(L, BK_SHIFT); lua_setfield(L, -2, "MOD_SHIFT");  lua_pushinteger(L, BK_CTRL); lua_setfield(L, -2, "MOD_CTRL");
  lua_pushinteger(L, BK_ALT); lua_setfield(L, -2, "MOD_ALT");     lua_pushinteger(L, BK_BUTTON1); lua_setfield(L, -2, "MOD_BUTTON1");
  lua_pushinteger(L, BK_BUTTON2); lua_setfield(L, -2, "MOD_BUTTON2"); lua_pushinteger(L, BK_BUTTON3); lua_setfield(L, -2, "MOD_BUTTON3");

  /* Image formats */
  lua_pushinteger(L, BK_IMAGE_NONE); lua_setfield(L, -2, "IMAGE_NONE");  lua_pushinteger(L, BK_IMAGE_PNG); lua_setfield(L, -2, "IMAGE_PNG");
  lua_pushinteger(L, BK_IMAGE_JPEG); lua_setfield(L, -2, "IMAGE_JPEG");  lua_pushinteger(L, BK_IMAGE_WEBP); lua_setfield(L, -2, "IMAGE_WEBP");
  lua_pushinteger(L, BK_IMAGE_GIF); lua_setfield(L, -2, "IMAGE_GIF");   lua_pushinteger(L, BK_IMAGE_SVG); lua_setfield(L, -2, "IMAGE_SVG");
  lua_pushinteger(L, BK_IMAGE_VIDEO); lua_setfield(L, -2, "IMAGE_VIDEO");

  /* Object-fit */
  lua_pushinteger(L, BK_FIT_FILL); lua_setfield(L, -2, "FIT_FILL");    lua_pushinteger(L, BK_FIT_CONTAIN); lua_setfield(L, -2, "FIT_CONTAIN");
  lua_pushinteger(L, BK_FIT_COVER); lua_setfield(L, -2, "FIT_COVER");   lua_pushinteger(L, BK_FIT_NONE); lua_setfield(L, -2, "FIT_NONE");
  lua_pushinteger(L, BK_FIT_SCALE_DOWN); lua_setfield(L, -2, "FIT_SCALE_DOWN");

  /* Video states */
  lua_pushinteger(L, BK_VIDEO_LOADING); lua_setfield(L, -2, "VIDEO_LOADING"); lua_pushinteger(L, BK_VIDEO_PLAYING); lua_setfield(L, -2, "VIDEO_PLAYING");
  lua_pushinteger(L, BK_VIDEO_PAUSED); lua_setfield(L, -2, "VIDEO_PAUSED");  lua_pushinteger(L, BK_VIDEO_ENDED); lua_setfield(L, -2, "VIDEO_ENDED");
  lua_pushinteger(L, BK_VIDEO_ERROR); lua_setfield(L, -2, "VIDEO_ERROR");

  /* Canvas item types */
  lua_pushinteger(L, BK_ITEM_RECT); lua_setfield(L, -2, "ITEM_RECT");     lua_pushinteger(L, BK_ITEM_OVAL); lua_setfield(L, -2, "ITEM_OVAL");
  lua_pushinteger(L, BK_ITEM_LINE); lua_setfield(L, -2, "ITEM_LINE");     lua_pushinteger(L, BK_ITEM_POLYGON); lua_setfield(L, -2, "ITEM_POLYGON");
  lua_pushinteger(L, BK_ITEM_TEXT); lua_setfield(L, -2, "ITEM_TEXT");     lua_pushinteger(L, BK_ITEM_ARC); lua_setfield(L, -2, "ITEM_ARC");
  lua_pushinteger(L, BK_ITEM_IMAGE); lua_setfield(L, -2, "ITEM_IMAGE");    lua_pushinteger(L, BK_ITEM_ROUNDRECT); lua_setfield(L, -2, "ITEM_ROUNDRECT");

  /* Widget draw ids */
  lua_pushinteger(L, BK_WIDGET_BUTTON); lua_setfield(L, -2, "WIDGET_BUTTON");  lua_pushinteger(L, BK_WIDGET_TOGGLE); lua_setfield(L, -2, "WIDGET_TOGGLE");
  lua_pushinteger(L, BK_WIDGET_CHECKBOX); lua_setfield(L, -2, "WIDGET_CHECKBOX");lua_pushinteger(L, BK_WIDGET_RADIO); lua_setfield(L, -2, "WIDGET_RADIO");
  lua_pushinteger(L, BK_WIDGET_SLIDER); lua_setfield(L, -2, "WIDGET_SLIDER");  lua_pushinteger(L, BK_WIDGET_TEXT); lua_setfield(L, -2, "WIDGET_TEXT");
  lua_pushinteger(L, BK_WIDGET_LABEL); lua_setfield(L, -2, "WIDGET_LABEL");   lua_pushinteger(L, BK_WIDGET_SEPARATOR); lua_setfield(L, -2, "WIDGET_SEPARATOR");

  /* Widget states */
  lua_pushinteger(L, BK_WS_HOVER); lua_setfield(L, -2, "WS_HOVER");     lua_pushinteger(L, BK_WS_PRESSED); lua_setfield(L, -2, "WS_PRESSED");
  lua_pushinteger(L, BK_WS_SELECTED); lua_setfield(L, -2, "WS_SELECTED");  lua_pushinteger(L, BK_WS_DISABLED); lua_setfield(L, -2, "WS_DISABLED");
  lua_pushinteger(L, BK_WS_EDITING); lua_setfield(L, -2, "WS_EDITING");   lua_pushinteger(L, BK_WS_OPEN); lua_setfield(L, -2, "WS_OPEN");

  return 1;
}

/* The loadable module is the CMake target `_bui` (the name avoids a clash with bui.lua), and Lua looks
 * for luaopen_<file name> in it: luaopen__bui. */
BUI_LUA_ENTRY int luaopen__bui(lua_State *L) {
  return luaopen_bui(L);
}
