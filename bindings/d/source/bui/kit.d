/* BUI kit: Blender's interface as a retained-mode C runtime.
 *
 * Raw D declarations for kit/include/BUI_kit.h. The `bui` package wraps the
 * common parts in classes (Node, Context, Window, ...); this module is the
 * complete one-to-one API for everything else.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.kit;

import bui.gpu : BuiRaster;

/* ---- version ------------------------------------------------------------- */
enum BUI_KIT_VERSION_MAJOR = 0;
enum BUI_KIT_VERSION_MINOR = 5;
enum BUI_KIT_VERSION_PATCH = 1;
enum BUI_KIT_VERSION_STRING = "0.5.1";
enum BUI_KIT_VERSION =
    BUI_KIT_VERSION_MAJOR * 10000 + BUI_KIT_VERSION_MINOR * 100 + BUI_KIT_VERSION_PATCH;

/* ---- opaque handles ------------------------------------------------------ */
struct BKNode;
struct BKContext;
struct BKTreeItem;
struct BKImBuf;
struct BKAnim;
struct BKVideo;
struct BKWindow;

/* ---- modifiers / buttons ------------------------------------------------- */
enum BK_SHIFT = 1;
enum BK_CTRL = 2;
enum BK_ALT = 4;
enum BK_BUTTON1 = 8;
enum BK_BUTTON2 = 16;
enum BK_BUTTON3 = 32;

/* ---- cursors ------------------------------------------------------------- */
enum BK_CURSOR_DEFAULT = 0;
enum BK_CURSOR_EW = 1;
enum BK_CURSOR_NS = 2;
enum BK_CURSOR_TEXT = 3;
enum BK_CURSOR_CROSSHAIR = 4;
enum BK_CURSOR_HAND = 5;
enum BK_CURSOR_MOVE = 6;
enum BK_CURSOR_NONE = 7;
enum BK_CURSOR_NESW = 8;
enum BK_CURSOR_NWSE = 9;
enum BK_CURSOR_NOT_ALLOWED = 10;
enum BK_CURSOR_WAIT = 11;
enum BK_CURSOR_PROGRESS = 12;

/* ---- events -------------------------------------------------------------- */
enum BK_EV_CHANGE = 1;
enum BK_EV_CLICK = 2;
enum BK_EV_CALL = 3;
enum BK_EV_RENAME = 4;
enum BK_EV_ACTIVATE = 5;
enum BK_EV_TOGGLE = 6;
enum BK_EV_BROWSE = 7;
enum BK_EV_PRESS = 8;
enum BK_EV_RELEASE = 9;
enum BK_EV_DOUBLE = 10;
enum BK_EV_MOTION = 11;
enum BK_EV_ENTER = 12;
enum BK_EV_LEAVE = 13;
enum BK_EV_KEY = 14;
enum BK_EV_WHEEL = 15;
enum BK_EV_SUBMIT = 16;
enum BK_EV_VIEW = 17;
enum BK_EV_SCROLL = 18;
enum BK_EV_CONFIGURE = 19;
enum BK_EV_KEYRELEASE = 20;

/* ---- geometry ------------------------------------------------------------ */
enum BK_GEOM_NONE = 0;
enum BK_GEOM_PACK = 1;
enum BK_GEOM_GRID = 2;
enum BK_GEOM_PLACE = 3;

enum BK_ANCHOR_N = 1;
enum BK_ANCHOR_S = 2;
enum BK_ANCHOR_W = 4;
enum BK_ANCHOR_E = 8;

/* ---- images -------------------------------------------------------------- */
enum BK_IMAGE_NONE = 0;
enum BK_IMAGE_PNG = 1;
enum BK_IMAGE_JPEG = 2;
enum BK_IMAGE_WEBP = 3;
enum BK_IMAGE_GIF = 4;
enum BK_IMAGE_BMP = 5;
enum BK_IMAGE_TGA = 6;
enum BK_IMAGE_QOI = 7;
enum BK_IMAGE_HDR = 8;
enum BK_IMAGE_PNM = 9;
enum BK_IMAGE_SVG = 10;
enum BK_IMAGE_VIDEO = 11;

enum BK_FIT_FILL = 0;
enum BK_FIT_CONTAIN = 1;
enum BK_FIT_COVER = 2;
enum BK_FIT_NONE = 3;
enum BK_FIT_SCALE_DOWN = 4;

/* ---- video --------------------------------------------------------------- */
enum BK_VIDEO_LOADING = 0;
enum BK_VIDEO_PLAYING = 1;
enum BK_VIDEO_PAUSED = 2;
enum BK_VIDEO_ENDED = 3;
enum BK_VIDEO_ERROR = 4;

/* ---- canvas -------------------------------------------------------------- */
enum BK_ITEM_RECT = 0;
enum BK_ITEM_OVAL = 1;
enum BK_ITEM_LINE = 2;
enum BK_ITEM_POLYGON = 3;
enum BK_ITEM_TEXT = 4;
enum BK_ITEM_ARC = 5;
enum BK_ITEM_IMAGE = 6;
enum BK_ITEM_ROUNDRECT = 7;

/* ---- custom nodes -------------------------------------------------------- */
enum BK_CUSTOM_PRESS = 1;
enum BK_CUSTOM_RELEASE = 2;
enum BK_CUSTOM_MOTION = 3;
enum BK_CUSTOM_LEAVE = 4;
enum BK_CUSTOM_WHEEL = 5;
enum BK_CUSTOM_KEY = 6;
enum BK_CUSTOM_KEYUP = 7;
enum BK_CUSTOM_FOCUS = 8;
enum BK_CUSTOM_BLUR = 9;

/* ---- IME ----------------------------------------------------------------- */
enum BK_IME_COMPOSITION_START = 0;
enum BK_IME_COMPOSITION = 1;
enum BK_IME_COMPOSITION_END = 2;

/* ---- widget drawing ------------------------------------------------------ */
enum BK_WIDGET_BUTTON = 1;
enum BK_WIDGET_TOGGLE = 2;
enum BK_WIDGET_CHECKBOX = 3;
enum BK_WIDGET_RADIO = 4;
enum BK_WIDGET_NUMBER = 5;
enum BK_WIDGET_SLIDER = 6;
enum BK_WIDGET_TEXT = 7;
enum BK_WIDGET_MENU = 8;
enum BK_WIDGET_PULLDOWN = 9;
enum BK_WIDGET_MENU_ITEM = 10;
enum BK_WIDGET_LIST_ITEM = 11;
enum BK_WIDGET_SWATCH = 12;
enum BK_WIDGET_PROGRESS = 13;
enum BK_WIDGET_TAB = 14;
enum BK_WIDGET_PANEL_HEADER = 15;
enum BK_WIDGET_DISCLOSURE = 16;
enum BK_WIDGET_MENU_BACK = 17;
enum BK_WIDGET_TOOLTIP_BACK = 18;
enum BK_WIDGET_SEPARATOR = 19;
enum BK_WIDGET_LABEL = 20;
enum BK_WIDGET_ARROW = 21;
enum BK_WIDGET_BOX = 22;

enum BK_WS_HOVER = 1 << 0;
enum BK_WS_PRESSED = 1 << 1;
enum BK_WS_SELECTED = 1 << 2;
enum BK_WS_DISABLED = 1 << 3;
enum BK_WS_EDITING = 1 << 4;
enum BK_WS_HOVER_LEFT = 1 << 5;
enum BK_WS_HOVER_RIGHT = 1 << 6;
enum BK_WS_MIXED = 1 << 7;
enum BK_WS_FLAT = 1 << 8;
enum BK_WS_OPEN = 1 << 9;

enum BK_CORNERS_ALL = 15;

/* ---- structs ------------------------------------------------------------- */
alias BKRequestFn = extern(C) void function(void* data);
alias BKSetCursorFn = extern(C) void function(void* data, int cursor);
alias BKWarpFn = extern(C) void function(void* data, int x, int y);
alias BKClipboardGetFn = extern(C) char* function(void* data);
alias BKClipboardSetFn = extern(C) void function(void* data, const(char)* text);
alias BKScheduleFn = extern(C) void function(void* data, int ms, int timer);
alias BKEmitFn =
    extern(C) void function(void* data, BKNode* node, int kind, void* ref_, double a, double b);

struct BKHost {
    void* data;
    BKRequestFn request;
    BKSetCursorFn set_cursor;
    BKWarpFn warp;
    BKClipboardGetFn clipboard_get;
    BKClipboardSetFn clipboard_set;
    BKScheduleFn schedule;
    BKEmitFn emit;
}

struct BKMenuSpec {
    const(char)* text;
    int icon;
    const(char)* shortcut;
    int checked;
    int enabled;
    void* ref_;
    const(BKMenuSpec)* sub;
    int nsub;
    extern(C) void function(void*) fn;
    void* data;
}

struct BKImageInfo {
    int format;
    int width;
    int height;
    int frames;
}

struct BKImageStats {
    size_t bytes;
    size_t budget;
    int entries;
    int pending;
    int decoded;
    int failed;
    int cancelled;
    int evicted;
    double decode_ms;
}

struct BKCanvasItem {
    int id;
    int type;
    const(float)* coords;
    int ncoords;
    float[4] fill;
    float[4] outline;
    float width;
    float radius;
    float size;
    float start;
    float extent;
    const(char)* text;
    int anchor;
    const(ubyte)* rgba;
    int iw;
    int ih;
    int hidden;
    int text_flags;
    int clip_on;
    float[4] clip;
    const(char)* family;
    const(BKImBuf)* imbuf;
    const(char)* src;
    int fit;
    int frame;
    BKVideo* video;
}

struct BKCustomEvent {
    int type;
    float x;
    float y;
    int button;
    int mods;
    int clicks;
    float wheel_x;
    float wheel_y;
    const(char)* text;
    const(char)* keysym;
}

alias BKCustomEventFn =
    extern(C) int function(void* data, BKNode* node, const(BKCustomEvent)* event);
alias BKCustomFrameFn = extern(C) void function(void* data, BKNode* node);
alias BKCustomDrawFn =
    extern(C) void function(void* data, BKNode* node, BuiRaster* raster, const(int)* clip);
alias BKCustomFreeFn = extern(C) void function(void* data, BKNode* node);

struct BKCustomClass {
    BKCustomEventFn event;
    BKCustomFrameFn frame;
    BKCustomDrawFn draw;
    BKCustomFreeFn free;
    int natural_w;
    int natural_h;
}

struct BKTextSpan {
    int start;
    int end;
    float[4] fg;
    float[4] bg;
    int has_fg;
    int has_bg;
}

struct BKImeEvent {
    const(char)* result;
    const(char)* composite;
    int cursor_pos;
    int sel_start;
    int sel_end;
}

struct BKImeState {
    int enabled;
    int restart;
    int x;
    int y;
}

alias BKWindowTimerFn = extern(C) void function(void* data, BKWindow* win, int id);
alias BKWindowCloseFn = extern(C) int function(void* data, BKWindow* win);
alias BKWindowFrameFn = extern(C) void function(void* data, BKWindow* win);
alias BKWindowFocusFn = extern(C) void function(void* data, BKWindow* win, int active);

struct BKWindowCallbacks {
    void* data;
    BKWindowTimerFn on_timer;
    BKWindowCloseFn on_close;
    BKWindowFrameFn on_frame;
    BKWindowFocusFn on_focus;
}

struct BKWidgetDraw {
    int kind;
    int state;
    int corners;
    int align_;
    float[4] rect;
    float[4] clip;
    float[4] text_rect;
    float value;
    float[4] color;
    float[4] aux;
    const(char)* text;
    const(char)* text2;
}

extern(C) {

alias BKRefHookFn = extern(C) void function(void* ref_);
alias BKVideoListenerFn = extern(C) void function(BKVideo* v, int state, void* data);
alias BKImageNotifyFn = extern(C) void function(void* data);
alias BKStopCheckFn = extern(C) int function();
alias BKWakeInitFn = extern(C) int function();

/* -- version and reference hooks ------------------------------------------ */
int BK_version();
const(char)* BK_version_string();
void BK_set_ref_hooks(BKRefHookFn retain, BKRefHookFn release);

/* -- global look ----------------------------------------------------------- */
void BK_set_scale(float scale);
float BK_scale();
void BK_set_text_shadow(int on);
int BK_text_shadow();
int BK_unit();
void BK_set_font(const(char)* family);
void BK_set_font_size(float points);
float BK_font_size_points();
int BK_set_theme(const(char)* name);
const(char)* BK_theme_name();
int BK_theme_color(const(char)* key, float* r_rgba);
int BK_theme_load_json(const(char)* json);
int BK_theme_load_file(const(char)* path);
const(char)* BK_theme_error();
int BK_theme_ignored();
int BK_theme_count();
const(char)* BK_theme_at(int index);
int BK_theme_set_color(const(char)* key, const(float)* rgba);
int BK_theme_set_num(const(char)* key, double value);
int BK_theme_num(const(char)* key, double* r_value);
int BK_theme_generation();
const(char)* BK_theme_mode();
int BK_icon_id(const(char)* name);
int BK_icon_count();
const(char)* BK_icon_name(int id);
int BK_icon_list_count();
const(char)* BK_icon_list_name(int i);

/* -- nodes ----------------------------------------------------------------- */
BKNode* BK_node_new(const(char)* type);
void BK_node_free(BKNode* node);
const(char)* BK_node_type(const(BKNode)* node);
void BK_node_set_ref(BKNode* node, void* ref_);
void* BK_node_ref(const(BKNode)* node);
int BK_node_add(BKNode* parent, BKNode* child, int index);
void BK_node_remove(BKNode* parent, BKNode* child);
int BK_node_child_count(const(BKNode)* node);
BKNode* BK_node_child(const(BKNode)* node, int i);
BKNode* BK_node_parent(const(BKNode)* node);
void BK_node_rect(const(BKNode)* node, int* r_rect);
void BK_node_natural(BKNode* node, int* r_size);
void BK_node_invalidate(BKNode* node);
void BK_node_relayout(BKNode* node);

/* -- properties ------------------------------------------------------------ */
int BK_set_num(BKNode* node, const(char)* key, double value);
int BK_set_str(BKNode* node, const(char)* key, const(char)* value);
int BK_set_color(BKNode* node, const(char)* key, const(float)* rgba);
int BK_set_strs(BKNode* node, const(char)* key, const(char*)* values, int n);
double BK_get_num(const(BKNode)* node, const(char)* key);
const(char)* BK_get_str(const(BKNode)* node, const(char)* key);
int BK_get_color(const(BKNode)* node, const(char)* key, float* r_rgba);
int BK_strs_count(const(BKNode)* node, const(char)* key);
const(char)* BK_strs_get(const(BKNode)* node, const(char)* key, int i);
void BK_link_fields(BKNode** fields, int n);

/* -- ramps, curves, histograms --------------------------------------------- */
int BK_ramp_count(BKNode* ramp);
void BK_ramp_get(BKNode* ramp, int i, float* r_pos, float* r_rgba);
void BK_ramp_set(BKNode* ramp, const(float)* pos, const(float)* rgba, int n);
int BK_ramp_active(BKNode* ramp);
void BK_ramp_evaluate(BKNode* ramp, float t, float* r_rgba);
int BK_curve_count(BKNode* curve, int channel);
void BK_curve_get(BKNode* curve, int channel, int i, float* r_xy);
void BK_curve_set(BKNode* curve, int channel, const(float)* xy, int n);
float BK_curve_evaluate(BKNode* curve, int channel, float x);
void BK_ramp_set_active_color(BKNode* ramp, const(float)* rgba);
void BK_histogram_set(BKNode* hist, const(float)* bins, int n, int channels);
void BK_gridview_set_colors(BKNode* grid, const(float)* colors, int n);

/* -- trees ----------------------------------------------------------------- */
BKTreeItem* BK_tree_item_new(const(char)* label, int icon);
void BK_tree_item_free(BKTreeItem* item);
void BK_tree_item_set_ref(BKTreeItem* item, void* ref_);
void* BK_tree_item_ref(const(BKTreeItem)* item);
void BK_tree_item_add(BKTreeItem* parent, BKTreeItem* child);
void BK_tree_item_clear(BKTreeItem* parent);
void BK_tree_item_set(BKTreeItem* item, const(char)* key, double value);
double BK_tree_item_get(const(BKTreeItem)* item, const(char)* key);
void BK_tree_item_set_label(BKTreeItem* item, const(char)* label);
const(char)* BK_tree_item_label(const(BKTreeItem)* item);
int BK_tree_item_child_count(const(BKTreeItem)* item);
BKTreeItem* BK_tree_item_child(const(BKTreeItem)* item, int i);
void BK_tree_set_roots(BKNode* tree, BKTreeItem** roots, int n);
BKTreeItem* BK_tree_selected(BKNode* tree);
void BK_tree_select(BKNode* tree, BKTreeItem* item);
int BK_tree_row_count(BKNode* tree);
BKTreeItem* BK_tree_row(BKNode* tree, int row);
void BK_tree_row_rect(BKNode* tree, int row, int* r_rect);

/* -- menus ----------------------------------------------------------------- */
void BK_menubutton_set_items(BKNode* button, const(BKMenuSpec)* items, int n);
void BK_open_menu(BKContext* ctx,
                  BKNode* owner,
                  const(BKMenuSpec)* items,
                  int n,
                  const(char)* title,
                  int at_x,
                  int at_y);
BKNode* BK_hit_at(BKContext* ctx, int x, int y);
void BK_open_pie(
    BKContext* ctx, const(BKMenuSpec)* items, int n, const(char)* title, int at_x, int at_y);

/* -- geometry managers ----------------------------------------------------- */
void BK_geom_set_manager(BKNode* child, int manager);
int BK_geom_manager(const(BKNode)* child);
void BK_geom_set(BKNode* child, const(char)* key, double value);
double BK_geom_get(const(BKNode)* child, const(char)* key);
void BK_frame_grid_weight(BKNode* frame, int column_axis, int index, double weight, int minsize);

/* -- images ---------------------------------------------------------------- */
int BK_image_probe(const(void)* data, size_t len, BKImageInfo* r_info);
int BK_image_probe_file(const(char)* path, BKImageInfo* r_info);
const(char)* BK_image_format_name(int format);
BKImBuf* BK_imbuf_decode(const(void)* data, size_t len, int max_w, int max_h);
BKImBuf* BK_imbuf_load(const(char)* path, int max_w, int max_h);
BKImBuf* BK_imbuf_from_rgba(const(ubyte)* rgba, int w, int h, int stride);
BKImBuf* BK_imbuf_from_argb(const(uint)* argb, int w, int h);
void BK_imbuf_free(BKImBuf* ib);
int BK_imbuf_size(const(BKImBuf)* ib, int* r_w, int* r_h);
int BK_imbuf_format(const(BKImBuf)* ib);
const(uint)* BK_imbuf_pixels(const(BKImBuf)* ib, int frame);
int BK_imbuf_delay(const(BKImBuf)* ib, int frame);
int BK_imbuf_loops(const(BKImBuf)* ib);
int BK_imbuf_rgba(const(BKImBuf)* ib, int frame, ubyte* dst);
BKImBuf* BK_imbuf_decode_frame(const(void)* data, size_t len, int frame, int max_w, int max_h);
int BK_imbuf_texture(const(BKImBuf)* ib, int frame);
int BK_imbuf_texture_update(const(BKImBuf)* ib, int frame, int tex);

/* -- animations ------------------------------------------------------------ */
BKAnim* BK_anim_open(const(void)* data, size_t len, int max_w, int max_h);
BKImBuf* BK_anim_next(BKAnim* anim, int* r_delay_ms);
int BK_anim_info(const(BKAnim)* anim, int* r_w, int* r_h, int* r_loops);
void BK_anim_free(BKAnim* anim);

/* -- video ----------------------------------------------------------------- */
BKVideo* BK_video_new(const(char)* src);
void BK_video_free(BKVideo* v);
void BK_video_play(BKVideo* v);
void BK_video_pause(BKVideo* v);
void BK_video_seek(BKVideo* v, double seconds);
void BK_video_set_rate(BKVideo* v, double rate);
void BK_video_set_loop(BKVideo* v, int loop);
void BK_video_set_poster(BKVideo* v, const(char)* src);
void BK_video_set_controls(BKVideo* v, int on);
int BK_video_state(const(BKVideo)* v);
double BK_video_time(const(BKVideo)* v);
double BK_video_duration(const(BKVideo)* v);
int BK_video_size(const(BKVideo)* v, int* r_w, int* r_h);
void BK_video_set_listener(BKVideo* v, BKVideoListenerFn fn, void* data);
int BK_video_texture(BKVideo* v, int tex, int max_w, int max_h);
void BK_node_set_video(BKNode* node, BKVideo* v, int fit);
const(char)* BK_video_backends();

/* -- shared image cache ---------------------------------------------------- */
void BK_image_set_budget(size_t bytes);
void BK_image_set_threads(int n);
void BK_image_set_notify(BKImageNotifyFn notify, void* data);
int BK_image_poll();
void BK_image_stats(BKImageStats* r_stats);
int BK_image_info(const(char)* src, BKImageInfo* r_info);
void BK_image_prefetch(const(char)* src, int w, int h, int fit);
void BK_image_clear_cache();
void BK_image_exit();

/* -- canvas ---------------------------------------------------------------- */
void BK_canvas_set_item(BKNode* canvas, const(BKCanvasItem)* item);
void BK_canvas_add_item(BKNode* canvas, const(BKCanvasItem)* item);
void BK_canvas_put_item(BKNode* canvas, int index, const(BKCanvasItem)* item);
void BK_canvas_truncate(BKNode* canvas, int count);
void BK_canvas_delete(BKNode* canvas, int id);
void BK_canvas_raise(BKNode* canvas, int id, int to_top);
int BK_canvas_count(BKNode* canvas);

/* -- images on nodes ------------------------------------------------------- */
void BK_node_set_image(BKNode* node, const(ubyte)* rgba, int w, int h);
void BK_node_set_imbuf(BKNode* node, const(BKImBuf)* ib);
void BK_node_set_image_source(BKNode* node, const(char)* src, int fit);
void BK_node_set_image_frame(BKNode* node, int frame);

/* -- custom nodes ---------------------------------------------------------- */
int BK_custom_set(BKNode* node, const(BKCustomClass)* cls, void* data);
void* BK_custom_data(BKNode* node);
void BK_custom_request_frame(BKNode* node, int ms);
void BK_custom_set_cursor(BKNode* node, int cursor);
int BK_custom_has_focus(BKNode* node);
BKContext* BK_node_context(const(BKNode)* node);

/* -- text area ------------------------------------------------------------- */
void BK_textarea_insert(BKNode* n, int offset, const(char)* text);
void BK_textarea_delete(BKNode* n, int start, int end);
int BK_textarea_caret(BKNode* n);
void BK_textarea_set_caret(BKNode* n, int offset);
int BK_textarea_selection(BKNode* n, int* r_start, int* r_end);
void BK_textarea_set_selection(BKNode* n, int start, int end);
void BK_textarea_set_spans(BKNode* n, const(BKTextSpan)* spans, int count);
int BK_textarea_index_at(BKNode* n, int x, int y);
void BK_textarea_see(BKNode* n, int offset);

/* -- tree columns ---------------------------------------------------------- */
void BK_tree_item_set_values(BKTreeItem* item, const(char*)* values, int n);

/* -- undo ------------------------------------------------------------------ */
int BK_undo(BKContext* ctx);
int BK_redo(BKContext* ctx);
int BK_undo_depth(BKContext* ctx, int* r_redo);

/* -- animation ------------------------------------------------------------- */
int BK_animating(BKContext* ctx);
void BK_set_animations(int enabled);

/* -- context --------------------------------------------------------------- */
BKContext* BK_context_new(BKNode* root);
void BK_context_free(BKContext* ctx);
void BK_context_set_host(BKContext* ctx, const(BKHost)* host);
void BK_context_set_header(BKContext* ctx, BKNode* header);
BKNode* BK_context_root(BKContext* ctx);

int BK_render(BKContext* ctx, BuiRaster* r, int w, int h, int full);
int BK_needs_render(BKContext* ctx);

/* -- input ----------------------------------------------------------------- */
int BK_press(BKContext* ctx, int x, int y, int mods);
int BK_release(BKContext* ctx, int x, int y, int mods);
int BK_motion(BKContext* ctx, int x, int y, int mods);
int BK_wheel(BKContext* ctx, int x, int y, int delta, int mods);
int BK_scroll(BKContext* ctx, int x, int y, float dx, float dy, int mods);
int BK_key(BKContext* ctx, const(char)* text, const(char)* keysym, int mods);
int BK_context_menu(BKContext* ctx, int x, int y, int mods);

/* -- input methods --------------------------------------------------------- */
int BK_ime_event(BKContext* ctx, int type, const(BKImeEvent)* e);
int BK_ime_state(BKContext* ctx, BKImeState* state);
int BK_ime_composing(BKContext* ctx);
int BK_key_release(BKContext* ctx, const(char)* keysym, int mods);
int BK_button(BKContext* ctx, int x, int y, int button, int release, int mods);
void BK_timer(BKContext* ctx, int timer);
void BK_bind(BKContext* ctx, const(char)* combo, void* ref_);
void BK_close_popups(BKContext* ctx);
void BK_set_pointer(BKContext* ctx, int x, int y);

/* -- introspection --------------------------------------------------------- */
int BK_cursor(BKContext* ctx);
BKNode* BK_hover(BKContext* ctx);
BKNode* BK_focus(BKContext* ctx);
void BK_focus_node(BKContext* ctx, BKNode* n);
char* BK_clipboard_get(BKContext* ctx);
void BK_clipboard_set(BKContext* ctx, const(char)* text);
int BK_modal_active(BKContext* ctx);
int BK_tooltip_visible(BKContext* ctx);
int BK_popup_count(BKContext* ctx);
BKNode* BK_popup(BKContext* ctx, int i);
BKNode* BK_popup_owner(BKContext* ctx, int i);
int BK_menu_row_count(BKNode* menu);
const(char)* BK_menu_row_text(BKNode* menu, int i);
void BK_menu_row_rect(BKNode* menu, int i, int* r_rect);
BKNode* BK_picker_part(BKNode* picker, const(char)* name);

/* -- native windows -------------------------------------------------------- */
int BK_window_supported();
BKWindow* BK_window_new(BKContext* ctx, const(char)* title, int width, int height);
void BK_window_free(BKWindow* win);
void BK_window_set_callbacks(BKWindow* win, const(BKWindowCallbacks)* cb);
void BK_window_show(BKWindow* win);
void BK_window_close(BKWindow* win);
int BK_window_is_open(const(BKWindow)* win);
void BK_window_set_title(BKWindow* win, const(char)* title);
void BK_window_set_size(BKWindow* win, int width, int height);
void BK_window_get_size(const(BKWindow)* win, int* r_width, int* r_height);
BuiRaster* BK_window_raster(const(BKWindow)* win);
void BK_window_set_gpu(BKWindow* win, int on);
int BK_window_gpu(const(BKWindow)* win);
int BK_window_frames(const(BKWindow)* win);
void BK_window_render_now(BKWindow* win);
int BK_window_timer(BKWindow* win, int ms);
void BK_window_request_frame(BKWindow* win, int vsync);
void BK_window_cancel_timer(BKWindow* win, int id);
void* BK_window_handle(const(BKWindow)* win);
void BK_window_wm(BKWindow* win, const(char)* key, double a, double b);
int BK_window_wm_get(const(BKWindow)* win, const(char)* key, double* r_a, double* r_b);
void BK_window_run();
int BK_window_pump();
void BK_window_quit();
void BK_window_set_stop_check(BKStopCheckFn stop);
int BK_window_wake_init(BKWakeInitFn cb);
void BK_window_wake();
void BK_window_set_keep_alive(int on);

/* -- Blender widgets, straight into a raster ------------------------------- */
void BK_draw_widget(BuiRaster* r, const(BKWidgetDraw)* w);
float BK_text_width(const(char)* s, int len);

/* -- expressions ----------------------------------------------------------- */
int BK_eval_number(const(char)* expr, double* r_value);

/* -- file browser ---------------------------------------------------------- */
int BK_file_dialog(const(char)* title,
                   int mode,
                   const(char)* dir,
                   const(char)* file,
                   const(char*)* filters,
                   int nfilters,
                   int multiple,
                   char*** r_paths);
void BK_file_dialog_free(char** paths);

/* -- recent menu searches -------------------------------------------------- */
void BK_recent_searches_enable(int enabled);
void BK_recent_searches_clear();
void BK_recent_searches_load(const(char)* path);
void BK_recent_searches_save(const(char)* path);

} // extern(C)
