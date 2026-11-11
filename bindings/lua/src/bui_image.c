/* BUI Lua binding: ImBuf and Video userdata wrappers.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "bui_internal.h"

/* ------------------------------------------------------------------ */
/* ImBuf methods                                                      */
/* ------------------------------------------------------------------ */

static int l_imbuf_size(lua_State *L) {
  luabui_ImBuf *iud = CHECK_IMBUF(L, 1);
  if (!iud->ib) return 0;
  int w, h;
  int frames = BK_imbuf_size(iud->ib, &w, &h);
  lua_pushinteger(L, w);
  lua_pushinteger(L, h);
  lua_pushinteger(L, frames);
  return 3;
}

static int l_imbuf_format(lua_State *L) {
  luabui_ImBuf *iud = CHECK_IMBUF(L, 1);
  lua_pushinteger(L, iud->ib ? BK_imbuf_format(iud->ib) : BK_IMAGE_NONE);
  return 1;
}

static int l_imbuf_pixels(lua_State *L) {
  luabui_ImBuf *iud = CHECK_IMBUF(L, 1);
  if (!iud->ib) return 0;
  int frame = (int)luaL_optinteger(L, 2, 0);
  int w, h;
  BK_imbuf_size(iud->ib, &w, &h);
  const uint32_t *pixels = BK_imbuf_pixels(iud->ib, frame);
  if (!pixels) return 0;
  lua_pushlstring(L, (const char *)pixels, (size_t)w * h * 4);
  return 1;
}

static int l_imbuf_delay(lua_State *L) {
  luabui_ImBuf *iud = CHECK_IMBUF(L, 1);
  int frame = (int)luaL_optinteger(L, 2, 0);
  lua_pushinteger(L, iud->ib ? BK_imbuf_delay(iud->ib, frame) : 0);
  return 1;
}

static int l_imbuf_loops(lua_State *L) {
  luabui_ImBuf *iud = CHECK_IMBUF(L, 1);
  lua_pushinteger(L, iud->ib ? BK_imbuf_loops(iud->ib) : 0);
  return 1;
}

static int l_imbuf_free(lua_State *L) {
  luabui_ImBuf *iud = CHECK_IMBUF(L, 1);
  if (iud->ib) {
    BK_imbuf_free(iud->ib);
    iud->ib = NULL;
  }
  return 0;
}

static int l_imbuf_gc(lua_State *L) {
  luabui_ImBuf *iud = (luabui_ImBuf *)luaL_checkudata(L, 1, LUABUI_IMBUF_MT);
  if (iud->ib) {
    BK_imbuf_free(iud->ib);
    iud->ib = NULL;
  }
  return 0;
}

static int l_imbuf_tostring(lua_State *L) {
  luabui_ImBuf *iud = (luabui_ImBuf *)luaL_checkudata(L, 1, LUABUI_IMBUF_MT);
  if (iud->ib) {
    int w, h;
    BK_imbuf_size(iud->ib, &w, &h);
    lua_pushfstring(L, "bui.ImBuf: %dx%d (%d frames)", w, h, BK_imbuf_size(iud->ib, NULL, NULL));
  } else {
    lua_pushfstring(L, "bui.ImBuf: (freed)");
  }
  return 1;
}

/* Factory: decode from data */
int l_image_decode(lua_State *L) {
  size_t len;
  const char *data = luaL_checklstring(L, 1, &len);
  int max_w = (int)luaL_optinteger(L, 2, 0);
  int max_h = (int)luaL_optinteger(L, 3, 0);

  BKImBuf *ib = BK_imbuf_decode(data, len, max_w, max_h);
  if (!ib) { lua_pushnil(L); return 1; }

  luabui_ImBuf *iud = (luabui_ImBuf *)lua_newuserdata(L, sizeof(luabui_ImBuf));
  iud->ib = ib;
  luaL_getmetatable(L, LUABUI_IMBUF_MT);
  lua_setmetatable(L, -2);
  return 1;
}

/* Factory: load from file */
int l_image_load(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  int max_w = (int)luaL_optinteger(L, 2, 0);
  int max_h = (int)luaL_optinteger(L, 3, 0);

  BKImBuf *ib = BK_imbuf_load(path, max_w, max_h);
  if (!ib) { lua_pushnil(L); return 1; }

  luabui_ImBuf *iud = (luabui_ImBuf *)lua_newuserdata(L, sizeof(luabui_ImBuf));
  iud->ib = ib;
  luaL_getmetatable(L, LUABUI_IMBUF_MT);
  lua_setmetatable(L, -2);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Video methods                                                      */
/* ------------------------------------------------------------------ */

static int l_video_play(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  if (vud->video) BK_video_play(vud->video);
  return 0;
}

static int l_video_pause(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  if (vud->video) BK_video_pause(vud->video);
  return 0;
}

static int l_video_seek(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  if (vud->video) BK_video_seek(vud->video, luaL_checknumber(L, 2));
  return 0;
}

static int l_video_set_rate(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  if (vud->video) BK_video_set_rate(vud->video, luaL_checknumber(L, 2));
  return 0;
}

static int l_video_set_loop(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  if (vud->video) BK_video_set_loop(vud->video, (int)lua_toboolean(L, 2));
  return 0;
}

static int l_video_set_muted(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  if (vud->video) BK_video_set_muted(vud->video, (int)lua_toboolean(L, 2));
  return 0;
}

static int l_video_muted(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  lua_pushboolean(L, vud->video && BK_video_muted(vud->video));
  return 1;
}

static int l_video_set_volume(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  if (vud->video) BK_video_set_volume(vud->video, luaL_checknumber(L, 2));
  return 0;
}

static int l_video_volume(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  lua_pushnumber(L, vud->video ? BK_video_volume(vud->video) : 0.0);
  return 1;
}

static int l_video_has_audio(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  lua_pushboolean(L, vud->video && BK_video_has_audio(vud->video));
  return 1;
}

static int l_video_set_poster(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  if (vud->video) BK_video_set_poster(vud->video, luaL_checkstring(L, 2));
  return 0;
}

static int l_video_set_controls(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  if (vud->video) BK_video_set_controls(vud->video, (int)lua_toboolean(L, 2));
  return 0;
}

static int l_video_state(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  lua_pushinteger(L, vud->video ? BK_video_state(vud->video) : BK_VIDEO_ERROR);
  return 1;
}

static int l_video_time(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  lua_pushnumber(L, vud->video ? BK_video_time(vud->video) : 0);
  return 1;
}

static int l_video_duration(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  lua_pushnumber(L, vud->video ? BK_video_duration(vud->video) : 0);
  return 1;
}

static int l_video_size(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  if (!vud->video) return 0;
  int w, h;
  BK_video_size(vud->video, &w, &h);
  lua_pushinteger(L, w);
  lua_pushinteger(L, h);
  return 2;
}

static int l_video_free(lua_State *L) {
  luabui_Video *vud = CHECK_VIDEO(L, 1);
  if (vud->video) {
    BK_video_free(vud->video);
    vud->video = NULL;
  }
  return 0;
}

static int l_video_gc(lua_State *L) {
  luabui_Video *vud = (luabui_Video *)luaL_checkudata(L, 1, LUABUI_VIDEO_MT);
  if (vud->video) {
    BK_video_free(vud->video);
    vud->video = NULL;
  }
  return 0;
}

static int l_video_tostring(lua_State *L) {
  luabui_Video *vud = (luabui_Video *)luaL_checkudata(L, 1, LUABUI_VIDEO_MT);
  lua_pushfstring(L, "bui.Video (%p)", (void *)(vud->video));
  return 1;
}

/* Factory: create video from source */
int l_video_new(lua_State *L) {
  const char *src = luaL_checkstring(L, 1);
  BKVideo *video = BK_video_new(src);
  if (!video) { lua_pushnil(L); return 1; }

  luabui_Video *vud = (luabui_Video *)lua_newuserdata(L, sizeof(luabui_Video));
  vud->video = video;
  luaL_getmetatable(L, LUABUI_VIDEO_MT);
  lua_setmetatable(L, -2);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

static const luaL_Reg imbuf_methods[] = {
  {"size",      l_imbuf_size},
  {"format",    l_imbuf_format},
  {"pixels",    l_imbuf_pixels},
  {"delay",     l_imbuf_delay},
  {"loops",     l_imbuf_loops},
  {"free",      l_imbuf_free},
  {"__gc",      l_imbuf_gc},
  {"__tostring",l_imbuf_tostring},
  {NULL, NULL}
};

static const luaL_Reg video_methods[] = {
  {"play",        l_video_play},
  {"pause",       l_video_pause},
  {"seek",        l_video_seek},
  {"set_rate",    l_video_set_rate},
  {"set_loop",    l_video_set_loop},
  {"set_poster",  l_video_set_poster},
  {"set_muted",   l_video_set_muted},
  {"muted",       l_video_muted},
  {"set_volume",  l_video_set_volume},
  {"volume",      l_video_volume},
  {"has_audio",   l_video_has_audio},
  {"set_controls",l_video_set_controls},
  {"state",       l_video_state},
  {"time",        l_video_time},
  {"duration",    l_video_duration},
  {"size",        l_video_size},
  {"free",        l_video_free},
  {"__gc",        l_video_gc},
  {"__tostring",  l_video_tostring},
  {NULL, NULL}
};

void luabui_register_imbuf(lua_State *L) {
  luaL_newmetatable(L, LUABUI_IMBUF_MT);
  luaL_setfuncs(L, imbuf_methods, 0);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
}

void luabui_register_video(lua_State *L) {
  luaL_newmetatable(L, LUABUI_VIDEO_MT);
  luaL_setfuncs(L, video_methods, 0);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
}

/* These are registered as module-level functions in bui_lua.c */
void luabui_register_image_factories(lua_State *L) {
  /* Called during luaopen_bui to add image_decode / image_load / Video to module table */
  (void)L;
}

/* Note: image_decode and image_load are registered as module functions in bui_lua.c
 * via the bui_module table's "image_decode" and "image_load" entries.
 * The Video factory is also registered there. */
