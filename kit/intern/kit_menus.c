/* Menus (dropdown, context, menu bar, submenus, type-to-search), pie menus
 * and popover panels; popup placement.
 *
 * As in Blender: menus open on press and pick on release, hovering a
 * submenu item opens it, typing searches every item including submenus,
 * Up/Down/Return/Right/Left navigate; pies pick by direction. Backgrounds
 * use widget_menu_back (soft shadow + menu_back colors), rows use
 * widget_menu_itembut with widget_state_menu_item.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "BUI_blender_c.h"
#include "blender/bk_blender.h"
#include "kit_intern.h"

static void menu_text_path(float x, float baseline, float cy, const char *s, const uchar c[4]);

static int u(void)
{
  return bk_unit();
}

/* -------------------------------------------------------------------- */
/** \name Menu specs (deep copies with retained refs)
 * \{ */

static bool is_sep(const BKMenuSpec *s)
{
  return !s->text || strcmp(s->text, "-") == 0;
}

BKMenuSpec *bk_menuspec_copy(const BKMenuSpec *items, int n)
{
  if (n <= 0) {
    return NULL;
  }
  BKMenuSpec *out = (BKMenuSpec *)calloc((size_t)n, sizeof(BKMenuSpec));
  for (int i = 0; i < n; i++) {
    out[i] = items[i];
    out[i].text = bk_strdup(items[i].text ? items[i].text : "-");
    out[i].shortcut = items[i].shortcut ? bk_strdup(items[i].shortcut) : NULL;
    out[i].sub = items[i].nsub ? bk_menuspec_copy(items[i].sub, items[i].nsub) : NULL;
    bk_retain(items[i].ref);
  }
  return out;
}

void bk_menuspec_free(BKMenuSpec *items, int n)
{
  if (!items) {
    return;
  }
  for (int i = 0; i < n; i++) {
    free((char *)items[i].text);
    free((char *)items[i].shortcut);
    bk_menuspec_free((BKMenuSpec *)items[i].sub, items[i].nsub);
    bk_release(items[i].ref);
  }
  free(items);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Placement
 * \{ */

#define PLACE_NONE (-100000)

static BKPopupPlace *popup_place_of(BKNode *p)
{
  return (BKPopupPlace *)p->ext; /* every popup's ext starts with BKPopupPlace */
}

static void place_init(BKPopupPlace *pl)
{
  pl->at_x = pl->at_y = PLACE_NONE;
  pl->side_x = pl->side_y = pl->side_xl = PLACE_NONE;
}

void bk_popup_place(BKNode *p, int win_w, int win_h)
{
  if (p->cls == &bk_cls_pie) {
    bk_arrange(p, 0, 0, win_w, win_h);
    return;
  }
  BKPopupPlace *pl = popup_place_of(p);
  int w, h;
  bk_natural(p, &w, &h);
  int x, y;
  const BKNode *o = p->parent;
  if (pl->side_x != PLACE_NONE) { /* submenu beside its row */
    x = (pl->side_x + w <= win_w) ? pl->side_x : BK_MAX(0, pl->side_xl - w);
    y = BK_MAX(0, BK_MIN(pl->side_y, win_h - h));
  }
  else if (pl->at_x != PLACE_NONE) { /* at a point (context menus) */
    x = BK_MAX(0, BK_MIN(pl->at_x, win_w - w));
    y = pl->at_y;
    if (y + h > win_h) {
      y = BK_MAX(0, y - h);
    }
  }
  else { /* below the owner; flipped above when there is no room */
    w = BK_MAX(w, o ? o->w : 0);
    const int ox = o ? o->x : 0, oy = o ? o->y : 0, oh = o ? o->h : 0;
    y = oy + oh;
    if (y + h > win_h && oy - h >= 0) {
      y = oy - h;
    }
    x = BK_MAX(0, BK_MIN(ox, win_w - w));
  }
  bk_arrange(p, x, y, w, h);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Menu
 * \{ */

static BKRect pie_item_rect_pub(BKNode *n, int i);

typedef struct MenuData {
  BKPopupPlace place;
  BKMenuSpec *all;
  int nall;
  BKMenuSpec *filtered; /* search results (owned) */
  int nfiltered;
  char *title, *current;
  char query[128];
  int from_row; /* row of the parent menu that opened this submenu */
} MenuData;

#define MENU(n) ((MenuData *)(n)->ext)

static BKMenuSpec *menu_items(BKNode *n, int *r_count)
{
  MenuData *m = MENU(n);
  if (m->query[0]) {
    *r_count = m->nfiltered;
    return m->filtered;
  }
  *r_count = m->nall;
  return m->all;
}

static int menu_pad_blender(void)
{
  return bk_iround(0.2f * u()); /* UI_MENU_PADDING */
}
/* Browser look (a <select>'s popup, Chrome on Windows): a 1px border around
 * 17px option rows (13.33px text, padding 0 2px 1px, min-height 1.2em). */
static int menu_pad_n(const BKNode *n)
{
  return BK_HTML(n) ? BK_MAX(1, (int)floorf(bk_hpx(1.0f))) : menu_pad_blender();
}
#define menu_pad() menu_pad_n(n)
static int menu_row_h(const BKNode *n)
{
  return BK_HTML(n) ? bk_iround(bk_hpx(17.0f)) : u();
}
static int menu_sep_h_n(const BKNode *n)
{
  return BK_HTML(n) ? bk_iround(bk_hpx(9.0f)) : bk_iround(0.5f * u());
}
#define menu_sep_h() menu_sep_h_n(n)
static int menu_title_h(BKNode *n)
{
  return (MENU(n)->title || MENU(n)->query[0]) ? u() : 0;
}

static void menu_row_geom(BKNode *n, int row, int *r_y, int *r_h)
{
  int count;
  BKMenuSpec *items = menu_items(n, &count);
  int y = menu_pad() + menu_title_h(n);
  for (int i = 0; i < count; i++) {
    const int h = is_sep(&items[i]) ? menu_sep_h() : menu_row_h(n);
    if (i == row) {
      *r_y = n->y + y;
      *r_h = h;
      return;
    }
    y += h;
  }
  *r_y = n->y + y;
  *r_h = 0;
}

static int menu_row_at(BKNode *n, int y)
{
  int count;
  BKMenuSpec *items = menu_items(n, &count);
  for (int i = 0; i < count; i++) {
    int ry, rh;
    menu_row_geom(n, i, &ry, &rh);
    if (y >= ry && y < ry + rh) {
      return (!is_sep(&items[i]) && items[i].enabled) ? i : -1;
    }
  }
  return -1;
}

static bool menu_has_icons(BKMenuSpec *items, int count)
{
  for (int i = 0; i < count; i++) {
    if (!is_sep(&items[i]) && (items[i].icon || items[i].checked >= 0)) {
      return true;
    }
  }
  return false;
}

/* -- browser look: a <select> popup -- */

#define HTML_GROUP_LABEL (-2) /* BKMenuSpec.checked of an <optgroup> label row */

static float html_menu_font_begin(void)
{
  const float saved = bk_text_scale;
  bk_text_scale = BK_HTML_SCALE();
  bk_html_text_scale = bk_text_scale;
  return saved;
}

/* option text start: padding-inline 2px + 2px (Chrome's popup), +16px under an <optgroup> */
static float html_menu_text_x(const BKMenuSpec *items, int i)
{
  if (items[i].checked == HTML_GROUP_LABEL) {
    return bk_hpx(4.0f);
  }
  for (int k = i - 1; k >= 0; k--) {
    if (items[k].checked == HTML_GROUP_LABEL) {
      return bk_hpx(20.0f);
    }
  }
  return bk_hpx(4.0f);
}

static void html_menu_measure(BKNode *n, int *rw, int *rh)
{
  int count;
  BKMenuSpec *items = menu_items(n, &count);
  const float saved = html_menu_font_begin();
  float w = 0;
  int h = 0;
  for (int i = 0; i < count; i++) {
    if (is_sep(&items[i])) {
      h += menu_sep_h();
      continue;
    }
    h += menu_row_h(n);
    w = BK_MAX(w, html_menu_text_x(items, i) +
                      bk_text_width(items[i].text, -1, items[i].checked == HTML_GROUP_LABEL) +
                      bk_hpx(4.0f));
  }
  bk_text_scale = saved;
  *rw = (int)ceilf(w) + 2 * menu_pad();
  *rh = BK_MAX(h, menu_row_h(n)) + 2 * menu_pad();
}

static void html_menu_draw(BKNode *n)
{
  const float saved = html_menu_font_begin();
  float c[4];
  bk_argb(0xFFFFFFFFu, c);
  float b[4];
  bk_argb(0xFF767676u, b);
  bk_html_box(n->x, n->y, n->w, n->h, 0.0f, c, b, (float)menu_pad());
  int count;
  BKMenuSpec *items = menu_items(n, &count);
  const int p = menu_pad();
  bk_clip_push(n->x + p, n->y + p, n->w - 2 * p, n->h - 2 * p);
  for (int i = 0; i < count; i++) {
    int ry, rh;
    menu_row_geom(n, i, &ry, &rh);
    const BKMenuSpec *it = &items[i];
    const BKRect row = bk_rect(n->x + p, ry, n->w - 2 * p, rh);
    if (is_sep(it)) { /* <hr> in a select */
      bk_argb(0xFFC0C0C0u, c);
      bk_fill_f(row.xmin + bk_hpx(4.0f), ry + rh / 2, bk_rw(&row) - bk_hpx(8.0f),
                BK_MAX(1.0f, floorf(bk_hpx(1.0f))), c);
      continue;
    }
    const bool group = it->checked == HTML_GROUP_LABEL;
    const bool hot = i == n->hot && it->enabled && !group;
    static const uchar black[4] = {0, 0, 0, 255}, white[4] = {255, 255, 255, 255},
                       gray[4] = {16, 16, 16, 77};
    if (hot) { /* the highlighted option: Highlight / HighlightText */
      bk_argb(0xFF1E90FFu, c);
      bk_fill_f(row.xmin, row.ymin, bk_rw(&row), rh, c);
    }
    const uchar *tc = hot ? white : ((it->enabled || group) ? black : gray);
    bk_text(row.xmin + html_menu_text_x(items, i), bk_text_baseline(&row), it->text, -1, tc,
            group);
  }
  bk_clip_pop();
  bk_text_scale = saved;
}

/* Typeahead: a printable key moves to the next option starting with it. */
static int html_menu_typeahead(BKNode *n, BKEvent *e)
{
  if (!e->text || (unsigned char)e->text[0] < 0x20 || EV_CTRL(e)) {
    return 0;
  }
  int count;
  BKMenuSpec *items = menu_items(n, &count);
  const int ch = tolower((unsigned char)e->text[0]);
  for (int k = 1; k <= count; k++) {
    const int i = ((n->hot < 0 ? -1 : n->hot) + k + count) % count;
    const BKMenuSpec *it = &items[i];
    if (!is_sep(it) && it->enabled && tolower((unsigned char)it->text[0]) == ch) {
      n->hot = i;
      bk_invalidate(n);
      break;
    }
  }
  return 1;
}

static void menu_measure(BKNode *n, int *rw, int *rh)
{
  if (BK_HTML(n)) {
    html_menu_measure(n, rw, rh);
    return;
  }
  int count;
  BKMenuSpec *items = menu_items(n, &count);
  const float uu = (float)u();
  float text_w = 0, sc_w = 0;
  bool sub = false;
  int h = 0;
  for (int i = 0; i < count; i++) {
    if (is_sep(&items[i])) {
      h += menu_sep_h();
      continue;
    }
    h += u();
    text_w = BK_MAX(text_w, bk_text_width(items[i].text, -1, false));
    if (items[i].shortcut) {
      sc_w = BK_MAX(sc_w, bk_text_width(items[i].shortcut, -1, false));
    }
    sub |= items[i].nsub > 0;
  }
  float w = text_w + 2 * bk_pad() + (menu_has_icons(items, count) ? bk_icon_size() + 0.3f * uu : 0);
  w += sc_w > 0 ? sc_w + 1.5f * uu : 0;
  w += sub ? uu : 0;
  if (MENU(n)->title) {
    w = BK_MAX(w, bk_text_width(MENU(n)->title, -1, false) + 2 * bk_pad());
  }
  w = BK_MAX(w, 9 * uu); /* UI_MENU_WIDTH_MIN */
  if (count == 0 && MENU(n)->query[0]) {
    h = u(); /* "No results found" */
  }
  *rw = (int)w + 2 * menu_pad();
  *rh = BK_MAX(h, u()) + menu_title_h(n) + 2 * menu_pad();
}

static int menu_child_row(BKNode *n)
{
  BKContext *ctx = bk_ctx(n);
  const int k = ctx ? bk_popup_index(ctx, n) : -1;
  if (k >= 0 && k + 1 < ctx->npopups && ctx->popups[k + 1]->parent == n) {
    return MENU(ctx->popups[k + 1])->from_row;
  }
  return -1;
}

static void menu_open_sub(BKNode *n, int row)
{
  BKContext *ctx = bk_ctx(n);
  int count;
  BKMenuSpec *items = menu_items(n, &count);
  if (!ctx || row < 0 || row >= count || !items[row].nsub || menu_child_row(n) == row) {
    return;
  }
  BKNode *sub = bk_menu_new(items[row].sub, items[row].nsub, NULL, NULL);
  int ry, rh;
  menu_row_geom(n, row, &ry, &rh);
  MENU(sub)->place.side_x = n->x + n->w;
  MENU(sub)->place.side_y = ry - menu_pad();
  MENU(sub)->place.side_xl = n->x;
  MENU(sub)->from_row = row;
  bk_open_popup(ctx, sub, n, n);
}

static BKNode *menu_root(BKNode *n)
{
  while (n->parent && n->parent->cls == &bk_cls_menu) {
    n = n->parent;
  }
  return n;
}

static void menu_pick(BKNode *n, int row)
{
  int count;
  BKMenuSpec *items = menu_items(n, &count);
  if (row < 0 || row >= count || is_sep(&items[row]) || !items[row].enabled) {
    return;
  }
  if (items[row].nsub) {
    menu_open_sub(n, row);
    return;
  }
  BKContext *ctx = bk_ctx(n);
  if (!ctx) {
    return;
  }
  /* take what we need before the menus (and their copies) are freed */
  void (*fn)(void *) = items[row].fn;
  void *data = items[row].data;
  void *ref = items[row].ref;
  BKNode *owner = menu_root(n)->parent;
  if (MENU(menu_root(n))->query[0]) {
    /* a menu-search result: remembered as a recent search (interface_string_search.cc) */
    bk_search_recent_add(items[row].text);
  }
  bk_retain(ref);
  BK_close_popups(ctx);
  if (fn) {
    fn(data);
  }
  else if (ref) {
    if (ctx->host.emit) {
      ctx->host.emit(ctx->host.data, owner, BK_EV_CALL, ref, 0, 0);
    }
  }
  else if (owner && owner->cls->popup_pick) {
    owner->cls->popup_pick(owner, data);
  }
  else if (owner && (owner->cls == &bk_cls_dropdown || owner->cls == &bk_cls_enum)) {
    bk_set_value(owner, (double)(intptr_t)data); /* enum menu: the item index */
  }
  bk_release(ref);
}

static void menu_filter_walk(MenuData *m, const BKMenuSpec *items, int count, const char *path,
                             const char *q, BKMenuSpec **out, int *nout, int *cap)
{
  for (int i = 0; i < count; i++) {
    const BKMenuSpec *it = &items[i];
    if (is_sep(it)) {
      continue;
    }
    char full[512];
    if (path[0]) {
      snprintf(full, sizeof(full), "%s \xe2\x96\xb8 %s", path, it->text);
    }
    else {
      snprintf(full, sizeof(full), "%s", it->text);
    }
    if (it->nsub) {
      menu_filter_walk(m, it->sub, it->nsub, full, q, out, nout, cap);
      continue;
    }
    /* every leaf is a candidate; Blender's string search filters and ranks them */
    if (*nout == *cap) {
      *cap = *cap ? *cap * 2 : 16;
      *out = (BKMenuSpec *)realloc(*out, sizeof(BKMenuSpec) * (size_t)*cap);
    }
    BKMenuSpec s = *it;
    s.text = bk_strdup(full);
    s.shortcut = it->shortcut ? bk_strdup(it->shortcut) : NULL;
    s.sub = NULL;
    s.nsub = 0;
    bk_retain(s.ref);
    (*out)[(*nout)++] = s;
  }
}

static void menu_filter(BKNode *n)
{
  MenuData *m = MENU(n);
  bk_menuspec_free(m->filtered, m->nfiltered);
  m->filtered = NULL;
  m->nfiltered = 0;
  if (m->query[0]) {
    /* Blender's menu search (interface_template_search_menu.cc): every leaf as
     * "Menu ▸ Submenu ▸ Item", filtered and ranked by blenlib's
     * string_search (word-initial matches, typo tolerance, recent items first),
     * weighting the last group (the item itself). */
    BKMenuSpec *all = NULL;
    int nall = 0, cap = 0;
    menu_filter_walk(m, m->all, m->nall, "", m->query, &all, &nall, &cap);
    BUIStringSearch *search = BUI_string_search_new(bk_search_recent_cache(),
                                                    BUI_SEARCH_MAIN_LAST);
    for (int i = 0; i < nall; i++) {
      BUI_string_search_add(search, all[i].text, (void *)(intptr_t)(i + 1), 0);
    }
    void **found = (void **)malloc(sizeof(void *) * (size_t)(nall ? nall : 1));
    const int nfound = BUI_string_search_query(search, m->query, found, nall);
    BUI_string_search_free(search);
    bool *used = (bool *)calloc((size_t)(nall ? nall : 1), sizeof(bool));
    m->filtered = nfound ? (BKMenuSpec *)malloc(sizeof(BKMenuSpec) * (size_t)nfound) : NULL;
    for (int k = 0; k < nfound && k < nall; k++) {
      const int i = (int)(intptr_t)found[k] - 1;
      m->filtered[m->nfiltered++] = all[i];
      used[i] = true;
    }
    for (int i = 0; i < nall; i++) {
      if (!used[i]) {
        free((char *)all[i].text);
        free((char *)all[i].shortcut);
        bk_release(all[i].ref);
      }
    }
    free(used);
    free(found);
    free(all);
  }
  n->hot = (m->query[0] && m->nfiltered) ? 0 : -1;
  BKContext *ctx = bk_ctx(n);
  if (ctx) {
    bk_close_popups_after(ctx, n);
    bk_invalidate(n); /* old size */
    bk_popup_place(n, ctx->win_w, ctx->win_h);
  }
  bk_relayout(n);
  bk_invalidate(n);
}

static int menu_move(BKNode *n, BKEvent *e)
{
  const int hot = menu_row_at(n, e->y);
  if (BK_HTML(n) && (hot < 0 || !bk_contains(n, e->x, e->y))) {
    return 0; /* the highlight stays on the last option (Chrome) */
  }
  if (hot == n->hot) {
    return 0;
  }
  n->hot = hot;
  bk_invalidate(n);
  int count;
  BKMenuSpec *items = menu_items(n, &count);
  if (hot >= 0 && items[hot].nsub) {
    menu_open_sub(n, hot);
  }
  else if (hot >= 0 && menu_child_row(n) >= 0) {
    bk_close_popups_after(bk_ctx(n), n);
  }
  return 1;
}

static int menu_press(BKNode *n, BKEvent *e)
{
  return 1;
}

static int menu_release(BKNode *n, BKEvent *e)
{
  const int row = menu_row_at(n, e->y);
  if (bk_contains(n, e->x, e->y) && row >= 0) {
    menu_pick(n, row);
  }
  return 1;
}

static void menu_step(BKNode *n, int d)
{
  int count;
  BKMenuSpec *items = menu_items(n, &count);
  int i = n->hot;
  for (int k = 0; k < count; k++) {
    i = i >= 0 ? (i + d + count) % count : (d > 0 ? 0 : count - 1);
    if (!is_sep(&items[i]) && items[i].enabled) {
      n->hot = i;
      bk_invalidate(n);
      return;
    }
  }
}

static int menu_search_key(BKNode *n, BKEvent *e)
{
  MenuData *m = MENU(n);
  if (bk_keysym_is(e->keysym, "Escape") && m->query[0]) {
    m->query[0] = 0;
    menu_filter(n);
    return 1;
  }
  if (bk_keysym_is(e->keysym, "BackSpace") && m->query[0]) {
    size_t l = strlen(m->query);
    do {
      l--;
    } while (l > 0 && ((unsigned char)m->query[l] & 0xC0) == 0x80);
    m->query[l] = 0;
    menu_filter(n);
    return 1;
  }
  if (e->text && (unsigned char)e->text[0] >= 0x20 && !EV_CTRL(e) &&
      strlen(m->query) + strlen(e->text) < sizeof(m->query) - 1)
  {
    strcat(m->query, e->text);
    menu_filter(n);
    return 1;
  }
  return 0;
}

static int menu_key(BKNode *n, BKEvent *e)
{
  int count;
  BKMenuSpec *items = menu_items(n, &count);
  if (bk_keysym_is(e->keysym, "Down") || bk_keysym_is(e->keysym, "Up")) {
    menu_step(n, bk_keysym_is(e->keysym, "Down") ? 1 : -1);
    return 1;
  }
  if (bk_keysym_is(e->keysym, "Return") || bk_keysym_is(e->keysym, "KP_Enter")) {
    menu_pick(n, n->hot);
    return 1;
  }
  if (bk_keysym_is(e->keysym, "Right") && n->hot >= 0 && n->hot < count && items[n->hot].nsub) {
    menu_open_sub(n, n->hot);
    return 1;
  }
  if (bk_keysym_is(e->keysym, "Left") && n->parent && n->parent->cls == &bk_cls_menu) {
    bk_close_popups_after(bk_ctx(n), n->parent);
    return 1;
  }
  if (BK_HTML(n)) {
    if (bk_keysym_is(e->keysym, "Home") || bk_keysym_is(e->keysym, "End")) {
      n->hot = -1;
      menu_step(n, bk_keysym_is(e->keysym, "Home") ? 1 : -1);
      return 1;
    }
    if (bk_keysym_is(e->keysym, "space") || bk_keysym_is(e->keysym, "Tab")) {
      menu_pick(n, n->hot); /* Chrome: Space / Tab accept the highlighted option */
      return 1;
    }
    return html_menu_typeahead(n, e);
  }
  /* typing always searches from the top menu (Blender menu search) */
  return menu_search_key(menu_root(n), e);
}

static void menu_check_icon(float cx, float cy, bool on)
{
  const int s = bk_iround(0.6f * u());
  BKRect r = bk_rect((int)(cx - s * 0.5f), (int)(cy - s * 0.5f), s, s);
  BKWCol w;
  bk_widget_state(&w, &bk_theme.option, on ? ST_SELECT : 0);
  bk_widget_optionbut(&w, &r, on ? ST_SELECT : 0);
}

static void menu_draw(BKNode *n)
{
  if (BK_HTML(n)) {
    html_menu_draw(n);
    return;
  }
  MenuData *m = MENU(n);
  BKRect r = bk_node_rect(n);
  bk_widget_menu_back(&r, CNR_ALL);
  const int p = menu_pad(), th = menu_title_h(n);
  const float uu = (float)u();
  const BKWCol *mi = &bk_theme.menu_item;
  if (m->query[0]) {
    uchar ic[4];
    memcpy(ic, mi->text, 4);
    ic[3] = 180;
    bk_icon(bk_icons_lookup("SEARCH"), n->x + p + 0.2f * uu + bk_icon_size() * 0.5f,
            n->y + p + th * 0.5f, ic, 1.0f, 0);
    BKRect qr = bk_rect(n->x + p + bk_icon_size() + (int)(0.3f * uu) - bk_pad(), n->y + p,
                        n->w - 2 * p, th);
    bk_text_in(&qr, m->query, mi->text, BK_ALIGN_LEFT, false);
  }
  else if (m->title) {
    BKRect tr = bk_rect(n->x + p, n->y + p, n->w - 2 * p, th);
    uchar tc[4];
    memcpy(tc, bk_theme.menu_back.text, 4);
    bk_text_in(&tr, m->title, tc, BK_ALIGN_LEFT, false);
  }
  int count;
  BKMenuSpec *items = menu_items(n, &count);
  if (count == 0 && m->query[0]) {
    BKRect nr = bk_rect(n->x + p, n->y + p + th, n->w - 2 * p, u());
    uchar tc[4];
    memcpy(tc, mi->text, 4);
    tc[3] = 128;
    bk_text_in(&nr, "No results found", tc, BK_ALIGN_LEFT, false);
  }
  const bool icons = menu_has_icons(items, count);
  const int open_child = menu_child_row(n);
  for (int i = 0; i < count; i++) {
    int ry, rh;
    menu_row_geom(n, i, &ry, &rh);
    BKMenuSpec *it = &items[i];
    BKRect row = bk_rect(n->x + p, ry, n->w - 2 * p, rh);
    if (is_sep(it)) {
      float c[4];
      bk_rgba_alpha(mi->text, 0.15f, c);
      bk_fill_f(row.xmin + bk_pad(), ry + rh / 2, bk_rw(&row) - 2 * bk_pad(), 1, c);
      continue;
    }
    int st = it->enabled ? 0 : ST_DISABLED;
    if ((i == n->hot || i == open_child) && it->enabled) {
      st |= ST_HOVER;
    }
    if (m->current && strcmp(it->text, m->current) == 0) {
      st |= ST_ACTIVE_DEFAULT; /* the current enum value */
    }
    BKWCol w;
    bk_widget_state_menu_item(&w, mi, st);
    BKRect wr = row;
    if (st & (ST_HOVER | ST_ACTIVE_DEFAULT)) {
      bk_widget_menu_itembut(&w, &wr);
    }
    float tx = row.xmin + bk_pad();
    if (icons) {
      const float cx = tx + bk_icon_size() * 0.5f - 0.1f * uu;
      if (it->checked >= 0) {
        menu_check_icon(cx, ry + rh * 0.5f, it->checked > 0);
      }
      else if (it->icon) {
        bk_icon(it->icon, cx, ry + rh * 0.5f, NULL, it->enabled ? 1.0f : 0.5f, 0);
      }
      tx += bk_icon_size() + 0.2f * uu;
    }
    bk_clip_push(row.xmin, row.ymin, bk_rw(&row), rh);
    BKRect lr = bk_rect(row.xmin, ry, bk_rw(&row), rh);
    menu_text_path(tx, bk_text_baseline(&lr), ry + rh * 0.5f, it->text, w.text);
    if (it->shortcut) {
      uchar sc[4];
      memcpy(sc, w.text, 4);
      sc[3] = (uchar)(sc[3] * 0.55f);
      const float sw = bk_text_width(it->shortcut, -1, false);
      bk_text(row.xmax - bk_pad() - sw - (it->nsub ? uu : 0), bk_text_baseline(&lr), it->shortcut,
              -1, sc, false);
    }
    bk_clip_pop();
    if (it->nsub) {
      uchar ac[4];
      memcpy(ac, w.text, 4);
      ac[3] = (uchar)(ac[3] * 0.8f);
      bk_icon(bk_icons_lookup("RIGHTARROW"), row.xmax - 0.5f * uu, ry + rh * 0.5f, ac, 1.0f,
              0.6f * uu);
    }
  }
}

/* Search results read "Menu > Item" (UI_MENU_ARROW_SEP). The UI font has no
 * U+25B8, so the separators are drawn as small triangles (Blender's glyph). */
static void menu_text_path(float x, float baseline, float cy, const char *s, const uchar c[4])
{
  static const char sep[] = " \xe2\x96\xb8 ";
  const int sl = (int)sizeof(sep) - 1;
  float fc[4];
  bk_rgba(c, fc);
  for (;;) {
    const char *hit = strstr(s, sep);
    const int len = hit ? (int)(hit - s) : (int)strlen(s);
    bk_text(x, baseline, s, len, c, false);
    if (!hit) {
      return;
    }
    x += bk_text_width(s, len, false);
    const float gap = bk_text_width(" ", 1, false), tri = 0.3f * bk_font_size();
    bk_tria(x + gap + tri * 0.4f, cy, 'h', fc, tri * 0.5f);
    x += 2.0f * gap + tri;
    s = hit + sl;
  }
}

static void menu_free(BKNode *n)
{
  MenuData *m = MENU(n);
  bk_menuspec_free(m->all, m->nall);
  bk_menuspec_free(m->filtered, m->nfiltered);
  free(m->title);
  free(m->current);
  free(m);
}

void bk_menu_on_close(BKNode *menu)
{
  (void)menu;
}

const BKClass bk_cls_menu = {.name = "menu",
                             .flags = CLS_INTERACTIVE | CLS_POPUP,
                             .free_ext = menu_free,
                             .measure = menu_measure,
                             .draw = menu_draw,
                             .press = menu_press,
                             .release = menu_release,
                             .move = menu_move,
                             .key = menu_key};

BKNode *bk_menu_new(const BKMenuSpec *items, int n, const char *title, const char *current)
{
  BKNode *menu = bk_node_new_cls(&bk_cls_menu);
  MenuData *m = (MenuData *)calloc(1, sizeof(MenuData));
  place_init(&m->place);
  m->all = bk_menuspec_copy(items, n);
  m->nall = n;
  m->title = title ? bk_strdup(title) : NULL;
  m->current = current ? bk_strdup(current) : NULL;
  m->from_row = -1;
  menu->ext = m;
  menu->hot = -1;
  if (current) {
    for (int i = 0; i < n; i++) {
      if (!is_sep(&items[i]) && strcmp(items[i].text, current) == 0) {
        menu->hot = i;
      }
    }
  }
  return menu;
}

void BK_open_menu(BKContext *ctx, BKNode *owner, const BKMenuSpec *items, int n, const char *title,
                  int at_x, int at_y)
{
  BKNode *menu = bk_menu_new(items, n, title, NULL);
  if (at_x > PLACE_NONE && at_y > PLACE_NONE && (at_x || at_y)) {
    MENU(menu)->place.at_x = at_x;
    MENU(menu)->place.at_y = at_y;
  }
  bk_open_popup(ctx, menu, owner ? owner : ctx->root, NULL);
}

int BK_menu_row_count(BKNode *menu)
{
  int count = 0;
  if (menu->cls == &bk_cls_menu || menu->cls == &bk_cls_pie) {
    menu_items(menu, &count);
  }
  return count;
}

void BK_menu_row_rect(BKNode *menu, int i, int r[4])
{
  if (menu->cls == &bk_cls_pie) {
    BKRect rr = pie_item_rect_pub(menu, i);
    r[0] = rr.xmin;
    r[1] = rr.ymin;
    r[2] = bk_rw(&rr);
    r[3] = bk_rh(&rr);
    return;
  }
  int ry, rh;
  menu_row_geom(menu, i, &ry, &rh);
  r[0] = menu->x + menu_pad_n(menu);
  r[1] = ry;
  r[2] = menu->w - 2 * menu_pad_n(menu);
  r[3] = rh;
}

const char *BK_menu_row_text(BKNode *menu, int i)
{
  int count = 0;
  BKMenuSpec *items = menu_items(menu, &count);
  return (i >= 0 && i < count) ? items[i].text : "";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pie menu (Blender order: W, E, S, N, NW, NE, SW, SE)
 * \{ */

/* RadialDirection (interface_intern.hh: N=0, NE, E, SE, S, SW, W, NW, clockwise) of the
 * i-th item (radial_dir_order) and radial_dir_to_angle in degrees, y-up. */
static const int g_pie_rdir[8] = {6, 2, 4, 0, 7, 1, 5, 3};
static const short g_pie_angle[8] = {90, 45, 0, 315, 270, 225, 180, 135};

/* pie_menu_begin / pie_handler (interface_handlers.cc): U.pie_tap_timeout (20 cs) and
 * PIE_CLICK_THRESHOLD_SQ (interface_intern.hh). */
#define PIE_TAP_TIMEOUT_S 0.2
#define PIE_MAX_ITEMS 8
#define PIE_ANIMATION_TIMEOUT_S 0.06 /* U.pie_animation_timeout (6 cs) */
#define PIE_CLICK_THRESHOLD_SQ 50.0f

typedef struct PieData {
  MenuData menu; /* items in menu.all */
  float cx, cy;
  int at_x, at_y;
  int init_x, init_y; /* pie_center_init: the pointer at open */
  float dir[2];       /* pie_data->pie_dir: normalized pointer direction, y-up */
  bool dir_valid;     /* !(flags & PIE_INVALID_DIR) */
  bool click_style;   /* PIE_CLICK_STYLE: pick on click, not on the key release */
  bool drag_style;    /* PIE_DRAG_STYLE: moved, so a quick release still picks */
  char key[32];       /* pie_data->event_type: the key that opened the pie */
  char accel[PIE_MAX_ITEMS]; /* Button.menu_key: 'a' .. 'z', or 0 */
  double opened;      /* bk_time at open (the scroll timer's time_duration) */
} PieData;

#define PIE(n) ((PieData *)(n)->ext)

/* button_pie_dir: unit vector of a RadialDirection, y-up. */
static void pie_rdir_vec(int rdir, float r_vec[2])
{
  const float a = g_pie_angle[rdir] * (float)M_PI / 180.0f;
  r_vec[0] = cosf(a);
  r_vec[1] = sinf(a);
}

static float pie_radius(void)
{
  return 100.0f * bk_scale_fac; /* U.pie_menu_radius * UI_SCALE_FAC */
}

static float pie_threshold(void)
{
  return 12.0f * bk_scale_fac; /* U.pie_menu_threshold * UI_SCALE_FAC */
}

/* LayoutRadial::resolve_impl: buttons are 1.5x taller and 1.5 units wider, placed on a circle of
 * pie_menu_radius; x factor aligns E items' left / W items' right edge to the circle, y factor
 * puts the N item's bottom and the S item's top edge on it (others centered). */
static BKRect pie_item_rect(BKNode *n, int i)
{
  PieData *pd = PIE(n);
  const BKMenuSpec *it = &pd->menu.all[i];
  const float uu = (float)u();
  float w = bk_text_width(it->text, -1, false) + 2 * bk_pad() +
            (it->icon ? bk_icon_size() + 0.3f * uu : 0);
  w = BK_MAX(w, 3 * uu) + 1.5f * uu;
  const float h = 1.5f * uu;
  float vec[2];
  pie_rdir_vec(g_pie_rdir[i], vec);
  const float fx = vec[0] > 0.01f ? 0.0f : (vec[0] < -0.01f ? -1.0f : -0.5f);
  const float fy = vec[1] > 0.99f ? 0.0f : (vec[1] < -0.99f ? -1.0f : -0.5f);
  /* pie_handler's animation: the item's center moves out from the pie center by n->anim */
  const float fac = n->anim;
  const float x = pd->cx + (vec[0] * pie_radius() + (fx + 0.5f) * w) * fac - 0.5f * w;
  const float y_up = (vec[1] * pie_radius() + (fy + 0.5f) * h) * fac - 0.5f * h;
  return bk_rect((int)x, (int)(pd->cy - y_up - h), (int)w, (int)h);
}

static BKRect pie_item_rect_pub(BKNode *n, int i)
{
  return pie_item_rect(n, i);
}

/* but_isect_pie_seg (interface_query.cc): an item owns the pointer direction when it lies within
 * 45 degrees and is closer than the adjacent direction (8th if that item exists, else 4th). */
static bool pie_isect_seg(PieData *pd, int i, int mask)
{
  if (!pd->dir_valid) {
    return false;
  }
  const float angle_4th_cos = (float)M_SQRT1_2, angle_8th_cos = 0.9238795f, eps = 1e-4f;
  const int rd = g_pie_rdir[i];
  float bd[2], ad[2];
  pie_rdir_vec(rd, bd);
  const float c = bd[0] * pd->dir[0] + bd[1] * pd->dir[1];
  if (c < angle_4th_cos - eps) {
    return false;
  }
  if (c > angle_8th_cos + eps) {
    return true;
  }
  /* angle_signed_v2v2(but_dir, pie_dir) < 0 -> previous (counter-clockwise) neighbours */
  const float perp = bd[1] * pd->dir[0] - bd[0] * pd->dir[1];
  const bool prev = atan2f(perp, c) < 0.0f;
  const int adj8 = prev ? (rd + 7) % 8 : (rd + 1) % 8;
  const int adj4 = prev ? (adj8 + 7) % 8 : (adj8 + 1) % 8;
  const int adj = (mask & (1 << adj8)) ? adj8 : adj4;
  pie_rdir_vec(adj, ad);
  const float ca = ad[0] * pd->dir[0] + ad[1] * pd->dir[1];
  if (c == ca) {
    return rd > adj;
  }
  return c > ca;
}

/* ui_block_calc_pie_segment + ui_but_find_mouse_over: no item inside the
 * pie_menu_threshold dead zone, otherwise the item owning the pointer direction. */
static int pie_dir_hot(BKNode *n, int x, int y)
{
  PieData *pd = PIE(n);
  const int count = pd->menu.nall;
  const float dx = x - pd->cx, dy = pd->cy - y; /* y-up */
  const float len = hypotf(dx, dy);
  pd->dir_valid = len >= pie_threshold();
  if (len > 0.0f) {
    pd->dir[0] = dx / len;
    pd->dir[1] = dy / len;
  }
  int mask = 0;
  for (int i = 0; i < count; i++) {
    mask |= 1 << g_pie_rdir[i];
  }
  for (int i = 0; i < count; i++) {
    if (pie_isect_seg(pd, i, mask)) {
      return i;
    }
  }
  return -1;
}

static void pie_arrange(BKNode *n)
{
  PieData *pd = PIE(n);
  const float r = pie_radius(), uu = (float)u();
  const float mx = r + 6 * uu, my = r + 2.5f * uu;
  const float ax = pd->at_x > PLACE_NONE ? pd->at_x : n->w * 0.5f;
  const float ay = pd->at_y > PLACE_NONE ? pd->at_y : n->h * 0.5f;
  pd->cx = BK_MAX(mx, BK_MIN(n->w - mx, ax));
  pd->cy = BK_MAX(my, BK_MIN(n->h - my, ay));
}

static BKNode *pie_hit(BKNode *n, int x, int y)
{
  return n; /* modal: owns the whole window while open */
}

static void pie_pick(BKNode *n, int i)
{
  PieData *pd = PIE(n);
  BKContext *ctx = bk_ctx(n);
  if (!ctx) {
    return;
  }
  if (i < 0 || i >= pd->menu.nall || !pd->menu.all[i].enabled) {
    BK_close_popups(ctx);
    return;
  }
  if (pd->menu.all[i].nsub) {
    /* pie_menu_level_invoke / wm.call_menu_pie: the sub-pie opens at the pointer, titled by its
     * item. Copied first: closing the pie frees its items. */
    const int nsub = pd->menu.all[i].nsub;
    BKMenuSpec *sub = bk_menuspec_copy(pd->menu.all[i].sub, nsub);
    char *title = bk_strdup(pd->menu.all[i].text);
    BK_close_popups(ctx);
    BK_open_pie(ctx, sub, nsub, title, PLACE_NONE, PLACE_NONE);
    bk_menuspec_free(sub, nsub);
    free(title);
    return;
  }
  void (*fn)(void *) = pd->menu.all[i].fn;
  void *data = pd->menu.all[i].data, *ref = pd->menu.all[i].ref;
  bk_retain(ref);
  BK_close_popups(ctx);
  if (fn) {
    fn(data);
  }
  else if (ref && ctx->host.emit) {
    ctx->host.emit(ctx->host.data, NULL, BK_EV_CALL, ref, 0, 0);
  }
  bk_release(ref);
}

static int pie_move(BKNode *n, BKEvent *e)
{
  PieData *pd = PIE(n);
  const float ix = (float)(e->x - pd->init_x), iy = (float)(e->y - pd->init_y);
  if (!pd->click_style && ix * ix + iy * iy > PIE_CLICK_THRESHOLD_SQ) {
    pd->drag_style = true;
  }
  const bool was_valid = pd->dir_valid;
  const int hot = pie_dir_hot(n, e->x, e->y);
  /* the center sector tracks the pointer direction, so redraw whenever it is shown */
  if (hot != n->hot || pd->dir_valid || was_valid) {
    n->hot = hot;
    bk_invalidate(n);
    return 1;
  }
  return 0;
}

/* draw_disk_shaded (interface_widgets.cc, unshaded): an annulus sector from `start` spanning
 * `angle` radians (y-up, like BL), as one even-odd polygon. */
static void pie_draw_disk(
    float cx, float cy, float start, float angle, float r_in, float r_out, const float c[4])
{
  enum { SUBD = 40 };
  float pts[2 * (SUBD + 1)][2];
  for (int i = 0; i <= SUBD; i++) {
    const float a = start + angle * i / SUBD;
    pts[i][0] = cx + r_out * cosf(a);
    pts[i][1] = cy - r_out * sinf(a);
    pts[2 * SUBD + 1 - i][0] = cx + r_in * cosf(a);
    pts[2 * SUBD + 1 - i][1] = cy - r_in * sinf(a);
  }
  bk_polygon((const float(*)[2])pts, 2 * (SUBD + 1), c);
}

static int pie_press(BKNode *n, BKEvent *e)
{
  pie_pick(n, pie_dir_hot(n, e->x, e->y));
  return 1;
}

static int pie_key(BKNode *n, BKEvent *e)
{
  PieData *pd = PIE(n);
  if (!pd->click_style && bk_keysym_is(e->keysym, pd->key)) {
    return 1; /* auto-repeat of the held pie key */
  }
  /* CASE_NUM_TO_DIR: the number keys are the numpad's directions (5 is the centre) */
  static const int num_rdir[10] = {-1, 5, 4, 3, 6, -1, 2, 7, 0, 1};
  if (e->text && e->text[0] >= '1' && e->text[0] <= '9' && !e->text[1]) {
    const int rdir = num_rdir[e->text[0] - '0'];
    for (int i = 0; i < pd->menu.nall; i++) {
      if (rdir >= 0 && g_pie_rdir[i] == rdir) {
        pie_pick(n, i); /* block_pie_dir_activate */
        return 1;
      }
    }
    return 1;
  }
  /* EVT_AKEY .. EVT_ZKEY with no modifier but Alt: the item with that menu key */
  if (e->text && isalpha((uchar)e->text[0]) && !e->text[1] && !(e->mods & (BK_SHIFT | BK_CTRL))) {
    for (int i = 0; i < pd->menu.nall; i++) {
      if (pd->accel[i] == tolower((uchar)e->text[0])) {
        pie_pick(n, i);
        return 1;
      }
    }
    return 1;
  }
  if (bk_keysym_is(e->keysym, "Return") || bk_keysym_is(e->keysym, "KP_Enter")) {
    pie_pick(n, n->hot);
    return 1;
  }
  return 0;
}

/* pie_handler, event_type release: a quick tap without a drag keeps the pie open as a click
 * pie, a later release picks the item in the pointer direction (none: cancel). */
static int pie_key_up(BKNode *n, BKEvent *e)
{
  PieData *pd = PIE(n);
  if (pd->click_style || !bk_keysym_is(e->keysym, pd->key)) {
    return 0;
  }
  if (bk_time() - pd->opened < PIE_TAP_TIMEOUT_S && !pd->drag_style) {
    pd->click_style = true;
    return 1;
  }
  pie_pick(n, pie_dir_hot(n, e->x, e->y));
  return 1;
}

/* menu_block_set_keyaccels (interface.cc): the first free first letter of a word, then in a
 * second pass any free letter. */
static void pie_set_keyaccels(PieData *pd)
{
  unsigned mask = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < pd->menu.nall; i++) {
      const char *s = pd->menu.all[i].text;
      if (pd->accel[i] || !s || !s[0] || !strcmp(s, "-")) {
        continue;
      }
      do {
        const int c = tolower((uchar)*s);
        if (c >= 'a' && c <= 'z' && !(mask & 1u << (c - 'a'))) {
          mask |= 1u << (c - 'a');
          pd->accel[i] = (char)c;
          break;
        }
        if (pass == 0) { /* first pass: skip to the next word */
          while (isalpha((uchar)*s)) {
            s++;
          }
          if (*s) {
            s++;
          }
        }
        else {
          s++;
        }
      } while (*s);
    }
  }
}

/* widget_draw_text: the accelerator's position in the text, upper case first; -1 none. */
static int pie_accel_index(const char *text, char accel)
{
  const char *p = accel ? strchr(text, accel - 32) : NULL;
  p = p ? p : (accel ? strchr(text, accel) : NULL);
  return p ? (int)(p - text) : -1;
}

static void pie_draw(BKNode *n)
{
  PieData *pd = PIE(n);
  const float uu = (float)u();
  float c[4];
  /* draw_pie_center (interface_widgets.cc): ring between pie_menu_threshold and threshold + 7
   * filled with wcol_pie_menu.inner, the pointer direction's sector in inner_sel (PI/4 wide when
   * the menu mixes axis-aligned and diagonal items, else PI/2), outline circles. */
  const float r_in = pie_threshold(), r_out = (12.0f + 7.0f) * bk_scale_fac;
  bk_rgba(bk_theme.pie_menu.inner, c);
  pie_draw_disk(pd->cx, pd->cy, 0.0f, 2.0f * (float)M_PI, r_in, r_out, c);
  if (pd->dir_valid) {
    int mask = 0;
    for (int i = 0; i < pd->menu.nall; i++) {
      mask |= 1 << g_pie_rdir[i];
    }
    const bool has_aligned = (mask & 0x55) != 0, has_diagonal = (mask & 0xAA) != 0;
    const float range = (has_aligned && has_diagonal) ? (float)M_PI_4 : (float)M_PI_2;
    const float angle = atan2f(pd->dir[1], pd->dir[0]);
    bk_rgba(bk_theme.pie_menu.inner_sel, c);
    pie_draw_disk(pd->cx, pd->cy, angle - range / 2.0f, range, r_in, r_out, c);
  }
  bk_rgba(bk_theme.pie_menu.outline, c);
  bk_ring(pd->cx, pd->cy, r_in, c, 1.0f);
  bk_ring(pd->cx, pd->cy, r_out, c, 1.0f);
  if (pd->menu.title) {
    /* LayoutRootPieMenu::resolve_impl: title bottom at threshold + 9 above the center */
    const float tw = bk_text_width(pd->menu.title, -1, false) + 2 * bk_pad();
    BKRect tr = bk_rect((int)(pd->cx - tw * 0.5f),
                        (int)(pd->cy - (12.0f + 9.0f) * bk_scale_fac - uu), (int)tw, (int)uu);
    bk_widget_menu_back(&tr, CNR_ALL);
    bk_text_in(&tr, pd->menu.title, bk_theme.menu_back.text, BK_ALIGN_CENTER, false);
  }
  for (int i = 0; i < pd->menu.nall; i++) {
    BKRect r = pie_item_rect(n, i);
    const BKMenuSpec *it = &pd->menu.all[i];
    int st = (i == n->hot ? ST_HOVER : 0) | (it->enabled ? 0 : ST_DISABLED);
    BKWCol w = bk_theme.pie_menu;
    /* widget_state_pie_menu_item */
    if (st & ST_HOVER) {
      memcpy(w.text, w.text_sel, 3);
      memcpy(w.inner, w.item, 4);
    }
    if (n->anim < 1.0f) { /* widget_menu_pie_itembut: alphafac while the pie animates */
      uchar *alpha[] = {&w.inner[3], &w.inner_sel[3], &w.item[3],
                        &w.text[3],  &w.text_sel[3],  &w.outline[3]};
      for (int k = 0; k < (int)(sizeof(alpha) / sizeof(*alpha)); k++) {
        *alpha[k] = (uchar)(*alpha[k] * n->anim);
      }
    }
    else {
      bk_shadow(&r, bk_widget_radius(&w));
    }
    BKRect wr = r;
    bk_widget_but(&w, &wr, 0, CNR_ALL);
    bk_text_underline(it->text ? pie_accel_index(it->text, pd->accel[i]) : -1);
    if (it->icon && it->text && it->text[0]) {
      /* BUT_ICON_LEFT pie item (widget_draw_text_icon): rect.xmin += 0.3 * widget_unit, icon
       * at xmin + 4 * scale, text centered in the space after icon_size + 2 * scale. */
      const float is = (float)bk_icon_size(), x0 = r.xmin + 0.3f * uu;
      bk_icon(it->icon, x0 + 4.0f * bk_scale_fac + is * 0.5f, r.ymin + bk_rh(&r) * 0.5f, NULL,
              it->enabled ? 1.0f : 0.5f, 0);
      BKRect tr = r;
      tr.xmin = (int)(x0 + is + 2.0f * bk_scale_fac);
      bk_text_in(&tr, it->text, w.text, BK_ALIGN_CENTER, false);
    }
    else {
      bk_text_icon(&r, it->text, it->icon, w.text, BK_ALIGN_CENTER, it->enabled);
    }
    bk_text_underline(-1);
  }
}

static void pie_free(BKNode *n)
{
  PieData *pd = PIE(n);
  bk_menuspec_free(pd->menu.all, pd->menu.nall);
  free(pd->menu.title);
  free(pd);
}

const BKClass bk_cls_pie = {.name = "pie",
                            .flags = CLS_INTERACTIVE | CLS_POPUP,
                            .anim_time = PIE_ANIMATION_TIMEOUT_S,
                            .free_ext = pie_free,
                            .arrange = pie_arrange,
                            .draw = pie_draw,
                            .hit = pie_hit,
                            .press = pie_press,
                            .move = pie_move,
                            .key = pie_key,
                            .key_up = pie_key_up};

void BK_open_pie(
    BKContext *ctx, const BKMenuSpec *items, int n, const char *title, int at_x, int at_y)
{
  const bool at = at_x > PLACE_NONE && at_y > PLACE_NONE;
  /* pie_menu_level_create: past PIE_MAX_ITEMS, the last slot is "More" with the rest */
  BKMenuSpec level[PIE_MAX_ITEMS];
  if (n > PIE_MAX_ITEMS) {
    memcpy(level, items, sizeof(BKMenuSpec) * (PIE_MAX_ITEMS - 1));
    level[PIE_MAX_ITEMS - 1] = (BKMenuSpec){.text = "More",
                                            .icon = BK_icon_id("PLUS"),
                                            .checked = -1,
                                            .enabled = 1,
                                            .sub = items + PIE_MAX_ITEMS - 1,
                                            .nsub = n - (PIE_MAX_ITEMS - 1)};
    items = level;
    n = PIE_MAX_ITEMS;
  }
  BKNode *pie = bk_node_new_cls(&bk_cls_pie);
  PieData *pd = (PieData *)calloc(1, sizeof(PieData));
  place_init(&pd->menu.place);
  pd->menu.all = bk_menuspec_copy(items, n);
  pd->menu.nall = n;
  pie_set_keyaccels(pd);
  pd->menu.title = title ? bk_strdup(title) : NULL;
  pd->at_x = at ? at_x : ctx->pointer_x;
  pd->at_y = at ? at_y : ctx->pointer_y;
  pd->init_x = ctx->pointer_x;
  pd->init_y = ctx->pointer_y;
  /* pie_menu_begin: opened while a key is down, its release picks; else click style */
  snprintf(pd->key, sizeof(pd->key), "%s", ctx->key_held);
  pd->click_style = !pd->key[0];
  pd->opened = bk_time();
  pie->ext = pd;
  pie->hot = -1;
  pie->anim = 0.0f; /* items grow out of the center (none with reduced motion) */
  bk_open_popup(ctx, pie, ctx->root, NULL);
  bk_anim_start(pie);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Popover panel (holds the Popover's body while open)
 * \{ */

typedef struct PopoverData {
  BKPopupPlace place;
} PopoverData;

static int popover_pad(void)
{
  return 2 * bk_iround(0.2f * u());
}

static void popover_panel_measure(BKNode *n, int *rw, int *rh)
{
  int bw = 0, bh = 0;
  if (n->nkids) {
    bk_natural(n->kids[0], &bw, &bh);
  }
  const int p = popover_pad();
  *rw = BK_MAX(bw, 10 * u()) + 2 * p;
  *rh = bh + 2 * p;
}

static void popover_panel_arrange(BKNode *n)
{
  const int p = popover_pad();
  if (n->nkids) {
    bk_arrange(n->kids[0], n->x + p, n->y + p, n->w - 2 * p, n->h - 2 * p);
  }
}

static BKNode *popover_panel_hit(BKNode *n, int x, int y)
{
  BKNode *f = n->nkids ? bk_hit(n->kids[0], x, y) : NULL;
  return f ? f : n;
}

static void popover_panel_draw(BKNode *n)
{
  BKRect r = bk_node_rect(n);
  bk_widget_menu_back(&r, CNR_ALL);
}

static void popover_panel_free(BKNode *n)
{
  free(n->ext);
}

const BKClass bk_cls_popover_panel = {.name = "popover_panel",
                                      .flags = CLS_INTERACTIVE | CLS_POPUP,
                                      .free_ext = popover_panel_free,
                                      .measure = popover_panel_measure,
                                      .arrange = popover_panel_arrange,
                                      .draw = popover_panel_draw,
                                      .hit = popover_panel_hit};

BKNode *bk_popover_panel_new(BKNode *owner, BKNode *body)
{
  BKNode *p = bk_node_new_cls(&bk_cls_popover_panel);
  PopoverData *d = (PopoverData *)calloc(1, sizeof(PopoverData));
  place_init(&d->place);
  p->ext = d;
  /* borrow the body (it stays owned by the popover button) */
  p->kids = (BKNode **)malloc(sizeof(BKNode *));
  p->kids[0] = body;
  p->nkids = 1;
  p->capkids = 1;
  body->parent = p;
  return p;
}

/** \} */
