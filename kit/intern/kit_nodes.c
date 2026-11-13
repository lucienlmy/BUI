/* The node editor: a View2D canvas of nodes, sockets and links.
 *
 * Ported from Blender's space_node: node_draw.cc (node_update_basis,
 * node_draw_basis, the dot grid), drawnode.cc (node_link_bezier_handles,
 * link drawing), node_select.cc / node_relationships.cc (select, move, link
 * drag, picking up a linked input, cut links) and view2d_ops.cc (pan, zoom).
 *
 * The editor ("nodeeditor") holds its nodes as children ("graphnode"). A
 * node's sockets are its `items`: "i|Name|type|value" for inputs (value may be
 * empty when the input has no field), "o|Name|type" for outputs. The editor's
 * `items` are its links: "from_node from_output to_node to_input" (indices
 * among the editor's nodes and among each node's outputs / inputs).
 *
 * A node can also hold controls, Blender's node buttons (`draw_buttons`, laid out by
 * node_update_basis_buttons between the outputs and the inputs): "b|Label" a push
 * button, "c|Label|0 or 1" a checkbox and "d|Label|index|Option;Option" a dropdown.
 *
 * Events (BK_EV_CHANGE): on a node, a = 1 moved, 2 selection, 3 input value
 * (b = input index), 4 collapsed, 5 a control was pressed or changed (b = control
 * index; its value is in the item); on the editor, a = 10 links changed,
 * 11 delete the selected nodes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "kit_intern.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* node_intern.hh, in units of U.widget_unit = 20 at scale 1 */
#define NODE_DY 20.0f
#define NODE_DYS 10.0f
#define NODE_ITEM_SPACING_Y 2.0f
#define NODE_SOCKSIZE 5.0f
#define BASIS_RAD 4.0f
#define NODE_LINK_RESOL 12
#define NOODLE_CURVING 4.0f /* theme space_node.noodle_curving */

enum { EV_MOVED = 1, EV_SELECT = 2, EV_VALUE = 3, EV_COLLAPSE = 4, EV_CONTROL = 5, EV_LINKS = 10, EV_DELETE = 11 };

/* -------------------------------------------------------------------- */
/** \name Data
 * \{ */

typedef struct GNode {
  float x, y, width; /* Blender units (node.location, node.width), y down */
  bool collapsed, active;
} GNode;

typedef enum { M_NONE, M_PAN, M_ZOOM, M_MOVE, M_BOX, M_LINK, M_VALUE, M_CUT, M_CONTROL } Mode;

#define MAX_CUT 256

typedef struct NodeEdit {
  float zoom, panx, pany; /* screen = n->x + pan + graph * scale * zoom */
  Mode mode;
  /* drag scratch */
  float start_panx, start_pany, start_zoom;
  int press_x, press_y;
  BKNode *hit;                 /* node pressed */
  int link_node, link_sock;    /* the fixed end of a dragged link */
  bool link_from_output;
  int value_node, value_sock;
  double value_start;
  BKNode *ctl_node; /* the control pressed (M_CONTROL), or the one whose menu is open */
  int ctl_k;
  bool shift;
  float cut[MAX_CUT][2];
  int ncut;
  int cur_x, cur_y;
} NodeEdit;

static int u(void)
{
  return bk_unit();
}

static NodeEdit *ed(const BKNode *n)
{
  return (NodeEdit *)n->ext;
}

static GNode *gn(const BKNode *n)
{
  return (GNode *)n->ext;
}

static float scale_of(const BKNode *e)
{
  return bk_scale_fac * ed(e)->zoom;
}

/* graph -> screen */
static float sx(const BKNode *e, float gx)
{
  return (float)e->x + ed(e)->panx + gx * scale_of(e);
}

static float sy(const BKNode *e, float gy)
{
  return (float)e->y + ed(e)->pany + gy * scale_of(e);
}

static float gx_of(const BKNode *e, float x)
{
  return (x - (float)e->x - ed(e)->panx) / scale_of(e);
}

static float gy_of(const BKNode *e, float y)
{
  return (y - (float)e->y - ed(e)->pany) / scale_of(e);
}

/* A socket from the node's items. */
typedef struct Sock {
  bool input;
  char name[64], type[24], value[32];
  int item; /* index in items */
} Sock;

static bool sock_parse(const char *s, Sock *r)
{
  memset(r, 0, sizeof(*r));
  if (!s || (s[0] != 'i' && s[0] != 'o') || s[1] != '|') {
    return false;
  }
  r->input = s[0] == 'i';
  const char *p = s + 2;
  char *fields[3] = {r->name, r->type, r->value};
  const int sizes[3] = {sizeof(r->name), sizeof(r->type), sizeof(r->value)};
  for (int f = 0; f < 3 && *p; f++) {
    const char *end = strchr(p, '|');
    const int len = end ? (int)(end - p) : (int)strlen(p);
    snprintf(fields[f], (size_t)sizes[f], "%.*s", len, p);
    if (!end) {
      break;
    }
    p = end + 1;
  }
  return true;
}

/* The k-th output / input of a node. */
static bool sock_get(const BKNode *node, bool input, int k, Sock *r)
{
  int count = 0;
  for (int i = 0; i < node->items.n; i++) {
    Sock s;
    if (sock_parse(node->items.v[i], &s) && s.input == input) {
      if (count++ == k) {
        *r = s;
        r->item = i;
        return true;
      }
    }
  }
  return false;
}

static int sock_count(const BKNode *node, bool input)
{
  int count = 0;
  for (int i = 0; i < node->items.n; i++) {
    Sock s;
    count += sock_parse(node->items.v[i], &s) && s.input == input;
  }
  return count;
}

/* A control from the node's items (see the header). */
enum { CTL_BUTTON, CTL_CHECK, CTL_MENU };

typedef struct Ctl {
  int kind;
  char label[64];
  int value;      /* a checkbox: 0 / 1; a dropdown: the chosen option */
  char opts[256]; /* a dropdown: "Option;Option" */
  int item;       /* index in items */
} Ctl;

static bool ctl_parse(const char *s, Ctl *r)
{
  memset(r, 0, sizeof(*r));
  if (!s || s[1] != '|' || (s[0] != 'b' && s[0] != 'c' && s[0] != 'd')) {
    return false;
  }
  r->kind = s[0] == 'b' ? CTL_BUTTON : (s[0] == 'c' ? CTL_CHECK : CTL_MENU);
  const char *p = s + 2;
  const char *end = strchr(p, '|');
  snprintf(r->label, sizeof(r->label), "%.*s", end ? (int)(end - p) : (int)strlen(p), p);
  if (r->kind != CTL_BUTTON && end) {
    r->value = atoi(end + 1);
    const char *o = strchr(end + 1, '|');
    if (r->kind == CTL_MENU && o) {
      snprintf(r->opts, sizeof(r->opts), "%s", o + 1);
    }
  }
  return true;
}

static bool ctl_get(const BKNode *node, int k, Ctl *r)
{
  int count = 0;
  for (int i = 0; i < node->items.n; i++) {
    Ctl c;
    if (ctl_parse(node->items.v[i], &c) && count++ == k) {
      *r = c;
      r->item = i;
      return true;
    }
  }
  return false;
}

static int ctl_count(const BKNode *node)
{
  int count = 0;
  for (int i = 0; i < node->items.n; i++) {
    Ctl c;
    count += ctl_parse(node->items.v[i], &c);
  }
  return count;
}

/* The k-th option of a dropdown control; false past the end. */
static bool ctl_option(const Ctl *c, int k, char *out, size_t size)
{
  const char *p = c->opts;
  for (int i = 0; *p || i == 0; i++) {
    const char *end = strchr(p, ';');
    const int len = end ? (int)(end - p) : (int)strlen(p);
    if (i == k) {
      snprintf(out, size, "%.*s", len, p);
      return true;
    }
    if (!end) {
      break;
    }
    p = end + 1;
  }
  return false;
}

/* std_node_socket_colors (node_socket.cc), Blender 4.x / 5.x */
static void socket_color(const char *type, float c[4])
{
  static const struct {
    const char *name;
    uint32_t rgb;
  } colors[] = {
      {"float", 0xa1a1a1},    {"value", 0xa1a1a1},     {"int", 0x598c5c},
      {"bool", 0xcca6d6},     {"boolean", 0xcca6d6},   {"vector", 0x6363c7},
      {"color", 0xc7c729},    {"rgba", 0xc7c729},      {"shader", 0x63c763},
      {"geometry", 0x00d6a3}, {"string", 0x70b2ff},    {"object", 0xed9e5c},
      {"image", 0x633863},    {"collection", 0xf5f5f5}, {"texture", 0x9e4fa3},
      {"material", 0xeb7582}, {"rotation", 0xa663c7}, {"menu", 0x666666},
      {"matrix", 0xb83385},   {"bundle", 0x4d8080},    {"closure", 0x7d7d3a},
      {"int_vector", 0x5c789c}, {"font", 0x635742},    {"sound", 0x635742},
  };
  uint32_t rgb = 0xa1a1a1;
  for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
    if (strcmp(type, colors[i].name) == 0) {
      rgb = colors[i].rgb;
    }
  }
  c[0] = ((rgb >> 16) & 255) / 255.0f;
  c[1] = ((rgb >> 8) & 255) / 255.0f;
  c[2] = (rgb & 255) / 255.0f;
  c[3] = 1.0f;
}

/* A link "a b c d". */
typedef struct Link {
  int from, out, to, in;
} Link;

static int links_get(const BKNode *e, Link *out, int max)
{
  int count = 0;
  for (int i = 0; i < e->items.n && count < max; i++) {
    Link l;
    if (sscanf(e->items.v[i], "%d %d %d %d", &l.from, &l.out, &l.to, &l.in) == 4 && l.from >= 0 &&
        l.to >= 0 && l.from < e->nkids && l.to < e->nkids)
    {
      out[count++] = l;
    }
  }
  return count;
}

static void links_set_ex(BKNode *e, const Link *l, int count, bool notify)
{
  char **v = (char **)calloc((size_t)BK_MAX(1, count), sizeof(char *));
  for (int i = 0; i < count; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d %d %d %d", l[i].from, l[i].out, l[i].to, l[i].in);
    v[i] = buf[0] ? strdup(buf) : NULL;
  }
  bk_strs_set(&e->items, (const char *const *)v, count);
  for (int i = 0; i < count; i++) {
    free(v[i]);
  }
  free(v);
  bk_invalidate(e);
  if (notify) {
    bk_emit(e, BK_EV_CHANGE, NULL, EV_LINKS, count);
  }
}

static void links_set(BKNode *e, const Link *l, int count)
{
  links_set_ex(e, l, count, true);
}

#define MAX_LINKS 1024

static bool input_linked(const BKNode *e, int node, int in, Link *r)
{
  Link l[MAX_LINKS];
  const int count = links_get(e, l, MAX_LINKS);
  for (int i = 0; i < count; i++) {
    if (l[i].to == node && l[i].in == in) {
      if (r) {
        *r = l[i];
      }
      return true;
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node layout (node_update_basis), in graph units
 * \{ */

/* The controls' block (node_update_basis_buttons): padded NODE_DYS / 4 above and below, one
 * row per control, between the outputs and the inputs. */
static float ctl_block(const BKNode *node)
{
  const int n = ctl_count(node);
  return n ? NODE_DYS / 2 + n * (NODE_DY + NODE_ITEM_SPACING_Y) : 0.0f;
}

static float node_height(const BKNode *node)
{
  if (gn(node)->collapsed) {
    return NODE_DY;
  }
  const int rows = sock_count(node, false) + sock_count(node, true);
  return NODE_DY + NODE_DYS / 2 + rows * (NODE_DY + NODE_ITEM_SPACING_Y) + ctl_block(node) +
         NODE_DYS / 2;
}

/* The row center of a control. */
static float ctl_y(const BKNode *node, int k)
{
  const GNode *g = gn(node);
  return g->y + NODE_DY + NODE_DYS / 2 + sock_count(node, false) * (NODE_DY + NODE_ITEM_SPACING_Y) +
         NODE_DYS / 4 + k * (NODE_DY + NODE_ITEM_SPACING_Y) + NODE_DY * 0.5f;
}

/* The row center of a socket (outputs first, then inputs). */
static float socket_y(const BKNode *node, bool input, int k)
{
  const GNode *g = gn(node);
  if (g->collapsed) {
    return g->y + NODE_DY * 0.5f;
  }
  const int row = input ? sock_count(node, false) + k : k;
  return g->y + NODE_DY + NODE_DYS / 2 + row * (NODE_DY + NODE_ITEM_SPACING_Y) +
         (input ? ctl_block(node) : 0.0f) + NODE_DY * 0.5f;
}

static float socket_x(const BKNode *node, bool input)
{
  return input ? gn(node)->x : gn(node)->x + gn(node)->width;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Graph nodes
 * \{ */

static void gnode_init(BKNode *n)
{
  GNode *g = (GNode *)calloc(1, sizeof(GNode));
  g->width = 140.0f; /* the default node width */
  n->ext = g;
  n->color[0] = 0.18f; /* header: a converter-ish default */
  n->color[1] = 0.18f;
  n->color[2] = 0.18f;
  n->color[3] = 1.0f;
}

static void gnode_free(BKNode *n)
{
  free(n->ext);
  n->ext = NULL;
}

static int gnode_set_num(BKNode *n, const char *key, double v)
{
  GNode *g = gn(n);
  if (strcmp(key, "nx") == 0) {
    g->x = (float)v;
  }
  else if (strcmp(key, "ny") == 0) {
    g->y = (float)v;
  }
  else if (strcmp(key, "nwidth") == 0) {
    g->width = (float)BK_MAX(40.0, v);
  }
  else if (strcmp(key, "collapsed") == 0) {
    g->collapsed = v != 0;
  }
  else if (strcmp(key, "selected") == 0) {
    n->flags = v != 0 ? (n->flags | NF_SELECTED) : (n->flags & ~NF_SELECTED);
  }
  else if (strcmp(key, "active") == 0) {
    g->active = v != 0;
  }
  else {
    return 0;
  }
  if (n->parent) {
    bk_invalidate(n->parent);
  }
  return 1;
}

static int gnode_get_num(BKNode *n, const char *key, double *r)
{
  const GNode *g = gn(n);
  if (strcmp(key, "nx") == 0) {
    *r = g->x;
  }
  else if (strcmp(key, "ny") == 0) {
    *r = g->y;
  }
  else if (strcmp(key, "nwidth") == 0) {
    *r = g->width;
  }
  else if (strcmp(key, "collapsed") == 0) {
    *r = g->collapsed;
  }
  else if (strcmp(key, "selected") == 0) {
    *r = (n->flags & NF_SELECTED) != 0;
  }
  else if (strcmp(key, "active") == 0) {
    *r = g->active;
  }
  else {
    return 0;
  }
  return 1;
}

static void gnode_measure(BKNode *n, int *rw, int *rh)
{
  *rw = *rh = 0;
}

const BKClass bk_cls_graphnode = {.name = "graphnode",
                                  .init = gnode_init,
                                  .free_ext = gnode_free,
                                  .measure = gnode_measure,
                                  .set_num = gnode_set_num,
                                  .get_num = gnode_get_num};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing (node_draw.cc)
 * \{ */

/* view2d_dot_grid_draw (view2d.cc), called by node_draw_space with min_step =
 * NODE_GRID_STEP_SIZE (20 * UI_SCALE_FAC view units) and the theme's grid_levels (3). View units
 * are graph units * UI_SCALE_FAC, so zoom_x (px per view unit) is the editor zoom. Each level is
 * 5x the previous; dot size is 2 * pixelsize * level_scale * zoom clamped to [2, 3] * pixelsize,
 * alpha (precise / drawn)^2, and the finest level fades by 1 - fract(view_level). */
static void draw_grid(BKNode *e)
{
  const int grid_subdivisions = 3; /* space_node.grid_levels */
  float c[4];
  bk_rgba(bk_theme.node_grid, c); /* space_node.grid */
  const float zoom = BK_MAX(1e-4f, ed(e)->zoom), px = bk_px();
  const float min_point_size = 2.0f * px, max_point_size = 1.5f * min_point_size;
  const float view_level = logf(NODE_DY * bk_scale_fac / zoom) / logf(5.0f);
  const int largest_visible_level = (int)view_level;
  for (int level_offset = 0; level_offset <= grid_subdivisions; level_offset++) {
    const int level = largest_visible_level - level_offset;
    if (level < 0) {
      break;
    }
    const float level_scale = powf(5.0f, (float)level);
    const float precise = min_point_size * level_scale * zoom;
    const float drawn = ceilf(BK_CLAMP(precise, min_point_size, max_point_size));
    const float offset = (drawn / 2.0f - px) * zoom; /* view-space offset, in pixels */
    const float alpha = (precise / drawn) * (precise / drawn);
    const float alpha_cutoff = 0.01f;
    if (alpha < alpha_cutoff) {
      break;
    }
    const float alpha_clamped = BK_CLAMP((1.0f + alpha_cutoff) * alpha - alpha_cutoff, 0.0f, 1.0f);
    const float fade = level_offset == grid_subdivisions ?
                           1.0f - (view_level - floorf(view_level)) :
                           1.0f;
    const float dc[4] = {c[0], c[1], c[2], alpha_clamped * fade};
    const float step = NODE_DY * level_scale * scale_of(e); /* screen pixels between dots */
    if (step < 1.0f) {
      continue;
    }
    const float ox = fmodf(ed(e)->panx, step), oy = fmodf(ed(e)->pany, step);
    for (float y = (float)e->y + oy - step; y < e->y + e->h + step; y += step) {
      for (float x = (float)e->x + ox - step; x < e->x + e->w + step; x += step) {
        if (x >= e->x && y >= e->y) {
          bk_circle(x + offset, y - offset, drawn * 0.5f, dc); /* POINT_..._COLOR_AA */
        }
      }
    }
  }
}

/* calculate_inner_link_bezier_points + evaluation: NODE_LINK_RESOL * 2 segments.
 * The handles shrink as the link nears horizontal; curving 0 is a straight line. */
static void bezier_points(float x0, float y0, float x3, float y3, float pts[][2], int n)
{
  const float dist_x = fabsf(x0 - x3), dist_y = fabsf(y0 - y3);
  const float slope = dist_x != 0.0f ? dist_y / dist_x : 0.0f;
  const float clamp_factor = BK_MIN(1.0f, slope * (4.5f - 0.25f * NOODLE_CURVING));
  const float dist = NOODLE_CURVING * 0.10f * dist_x * clamp_factor;
  const float x1 = x0 + dist, x2 = x3 - dist;
  for (int i = 0; i <= n; i++) {
    const float t = (float)i / n, it = 1.0f - t;
    const float a = it * it * it, b = 3 * it * it * t, cc = 3 * it * t * t, d = t * t * t;
    pts[i][0] = a * x0 + b * x1 + cc * x2 + d * x3;
    pts[i][1] = a * y0 + b * y0 + cc * y3 + d * y3;
  }
}

static void draw_noodle(float x0, float y0, float x3, float y3, const float color[4], bool hi,
                        float zoom)
{
  enum { N = NODE_LINK_RESOL * 2 };
  float pts[N + 1][2];
  bezier_points(x0, y0, x3, y3, pts, N);
  /* node_draw_link: LINK_WIDTH 2.5 * max(UI_SCALE_FAC * zoom, 1). The link
   * shader draws the outline 2 * thickness wide and the colored line on top
   * at 0.65 of that. */
  const float link_w = 2.5f * BK_MAX(bk_scale_fac * zoom, 1.0f);
  const float th = 2.0f * link_w * 0.65f, outline_w = 2.0f * link_w;
  float wire[4];
  bk_rgba(bk_theme.node_wire, wire); /* space_node.wire */
  float inner[4];
  memcpy(inner, color, sizeof(inner));
  if (hi) { /* edge_select: links of selected nodes brighten */
    for (int k = 0; k < 3; k++) {
      inner[k] = inner[k] + (1.0f - inner[k]) * 0.5f;
    }
  }
  for (int i = 0; i < N; i++) {
    bk_line(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], wire, outline_w);
  }
  for (int i = 0; i < N; i++) {
    bk_line(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], inner, th);
  }
}

static void draw_socket(float x, float y, const char *type, float s)
{
  /* node_socket_outline_color_get: the theme wire color, opaque */
  float c[4], o[4];
  bk_rgba(bk_theme.node_wire, o);
  o[3] = 1.0f;
  socket_color(type, c);
  const float r = NODE_SOCKSIZE * s;
  bk_circle(x, y, r + BK_MAX(1.0f, bk_scale_fac), o);
  bk_circle(x, y, r, c);
}

/* A node's input field: a number button with the name on the left. */
static void draw_input_field(BKRect r, const char *name, const char *value)
{
  BKWCol w;
  bk_widget_state(&w, &bk_theme.num, 0);
  bk_widget_numbut(&w, &r, 0, CNR_ALL);
  BKRect lr = r, vr = r;
  const float vw = bk_text_width(value, -1, false);
  lr.xmax = (int)(r.xmax - vw - bk_pad());
  bk_text_in(&lr, name, w.text, BK_ALIGN_LEFT, false);
  bk_text_in(&vr, value, w.text, BK_ALIGN_RIGHT, false);
}

/* A node's control: a push button, a checkbox or a dropdown (the widgets of Blender's node buttons). */
static void draw_ctl(BKRect r, const Ctl *c, bool pressed)
{
  const int st = pressed ? ST_SELECT : 0;
  BKWCol w;
  if (c->kind == CTL_CHECK) {
    bk_widget_state(&w, &bk_theme.option, st | (c->value ? ST_SELECT : 0));
    bk_widget_optionbut(&w, &r, st | (c->value ? ST_SELECT : 0));
    bk_text_in_pad(&r, c->label, w.text, BK_ALIGN_LEFT, false, 0);
  }
  else if (c->kind == CTL_MENU) {
    bk_widget_state(&w, &bk_theme.menu, st);
    bk_widget_menubut(&w, &r, st, CNR_ALL);
    char text[64];
    if (!ctl_option(c, c->value, text, sizeof(text))) {
      text[0] = '\0';
    }
    char buf[160];
    snprintf(buf, sizeof(buf), c->label[0] ? "%s: %s" : "%s%s", c->label, text);
    bk_text_in(&r, buf, w.text, BK_ALIGN_LEFT, false);
  }
  else {
    bk_widget_state(&w, &bk_theme.regular, st);
    bk_widget_but(&w, &r, st, CNR_ALL);
    bk_text_in(&r, c->label, w.text, BK_ALIGN_CENTER, false);
  }
}

static void draw_node(BKNode *e, BKNode *node, int index)
{
  const GNode *g = gn(node);
  const float s = scale_of(e);
  const float x = sx(e, g->x), y = sy(e, g->y);
  const float w = g->width * s, h = node_height(node) * s, hh = NODE_DY * s;
  if (x > e->x + e->w || y > e->y + e->h || x + w < e->x || y + h < e->y) {
    return;
  }
  const float rad = BASIS_RAD * s;
  const bool selected = (node->flags & NF_SELECTED) != 0;
  /* node_draw_shadow */
  const float shadow[4] = {0.0f, 0.0f, 0.0f, 0.25f};
  bk_roundbox(x - 1.0f * s, y + 1.0f * s, w + 2.0f * s, h + 2.0f * s, rad + s, CNR_ALL, shadow);
  /* body (TH_NODE) and header (the node class color) */
  float body[4], head[4];
  bk_rgba(bk_theme.node_body, body); /* space_node.syntaxl (node_backdrop) */
  memcpy(head, node->color, sizeof(head));
  head[3] = 1.0f;
  bk_roundbox(x, y, w, h, rad, CNR_ALL, body);
  bk_roundbox(x, y, w, hh, rad, g->collapsed ? CNR_ALL : (CNR_TL | CNR_TR), head);
  /* the outline: node_outline, or select / active */
  float line[4];
  bk_rgba(g->active && selected ? bk_theme.node_active
                                 : (selected ? bk_theme.node_selected : bk_theme.node_outline),
          line);
  BuiWidgetBase wb = {{x, y, w, h}, rad, rad - 1.0f, CNR_ALL};
  memcpy(wb.outline, line, sizeof(line));
  wb.pixelsize = selected ? 1.5f * bk_scale_fac : 1.0f;
  BUI_raster_widget_base(bk_draw.r, &wb);

  const float prev = bk_text_scale;
  bk_text_scale = ed(e)->zoom;
  /* header: the collapse arrow, then the title (node_draw_basis) */
  uchar tc[4] = {230, 230, 230, 255};
  const float arrow_x = x + 0.5f * u() * ed(e)->zoom;
  bk_icon(bk_icons_lookup(g->collapsed ? "RIGHTARROW" : "DOWNARROW_HLT"), arrow_x,
          y + hh * 0.5f, tc, 0.8f, 0.8f * u() * ed(e)->zoom);
  if (s > 0.3f) {
    BKRect tr = bk_rect((int)(x + 1.0f * u() * ed(e)->zoom - bk_pad()), (int)y,
                        (int)(w - 1.2f * u() * ed(e)->zoom), (int)hh);
    bk_text_in(&tr, node->label ? node->label : "", tc, BK_ALIGN_LEFT, false);
  }
  /* the controls, between the outputs and the inputs */
  if (!g->collapsed && s > 0.3f) {
    const float m = NODE_DYS * s;
    const int count = ctl_count(node);
    for (int k = 0; k < count; k++) {
      Ctl c;
      ctl_get(node, k, &c);
      const float cy = sy(e, ctl_y(node, k));
      const BKRect row = bk_rect((int)(x + m), (int)(cy - hh * 0.5f), (int)(w - 2 * m), (int)hh);
      draw_ctl(row, &c, ed(e)->mode == M_CONTROL && ed(e)->ctl_node == node && ed(e)->ctl_k == k);
    }
  }
  /* sockets and their rows */
  for (int pass = 0; pass < 2; pass++) {
    const bool input = pass == 1;
    const int count = sock_count(node, input);
    for (int k = 0; k < count; k++) {
      Sock so;
      sock_get(node, input, k, &so);
      const float cy = sy(e, socket_y(node, input, k));
      if (!g->collapsed && s > 0.3f) {
        const float m = NODE_DYS * s; /* content margin */
        BKRect row = bk_rect((int)(x + m), (int)(cy - hh * 0.5f), (int)(w - 2 * m), (int)hh);
        if (input && so.value[0] && !input_linked(e, index, k, NULL)) {
          draw_input_field(row, so.name, so.value);
        }
        else {
          uchar lc[4];
          memcpy(lc, bk_theme.text, 4);
          bk_text_in_pad(&row, so.name, lc, input ? BK_ALIGN_LEFT : BK_ALIGN_RIGHT, false, 0);
        }
      }
      draw_socket(input ? x : x + w, cy, so.type, s);
    }
  }
  bk_text_scale = prev;
}

static void link_ends(const BKNode *e, const Link *l, float *x0, float *y0, float *x1, float *y1)
{
  const BKNode *a = e->kids[l->from], *b = e->kids[l->to];
  *x0 = sx(e, socket_x(a, false));
  *y0 = sy(e, socket_y(a, false, l->out));
  *x1 = sx(e, socket_x(b, true));
  *y1 = sy(e, socket_y(b, true, l->in));
}

static void editor_draw(BKNode *e)
{
  float back[4];
  bk_rgba(bk_theme.node_back, back); /* space_node.back */
  bk_fill_f((float)e->x, (float)e->y, (float)e->w, (float)e->h, back);
  bk_clip_push(e->x, e->y, e->w, e->h);
  draw_grid(e);
  NodeEdit *d = ed(e);
  /* links under the nodes */
  Link l[MAX_LINKS];
  const int count = links_get(e, l, MAX_LINKS);
  for (int i = 0; i < count; i++) {
    Sock so;
    if (!sock_get(e->kids[l[i].from], false, l[i].out, &so)) {
      continue;
    }
    float c[4], x0, y0, x1, y1;
    socket_color(so.type, c);
    link_ends(e, &l[i], &x0, &y0, &x1, &y1);
    const bool hi = (e->kids[l[i].from]->flags | e->kids[l[i].to]->flags) & NF_SELECTED;
    draw_noodle(x0, y0, x1, y1, c, hi, d->zoom);
  }
  for (int i = 0; i < e->nkids; i++) {
    if (e->kids[i]->cls == &bk_cls_graphnode && NODE_VISIBLE(e->kids[i])) {
      draw_node(e, e->kids[i], i);
    }
  }
  /* the link being dragged (node_draw_link_dragged) */
  if (d->mode == M_LINK) {
    Sock so;
    BKNode *fixed = e->kids[d->link_node];
    sock_get(fixed, !d->link_from_output, d->link_sock, &so);
    float c[4];
    socket_color(so.type, c);
    const float fx = sx(e, socket_x(fixed, !d->link_from_output));
    const float fy = sy(e, socket_y(fixed, !d->link_from_output, d->link_sock));
    if (d->link_from_output) {
      draw_noodle(fx, fy, (float)d->cur_x, (float)d->cur_y, c, true, d->zoom);
    }
    else {
      draw_noodle((float)d->cur_x, (float)d->cur_y, fx, fy, c, true, d->zoom);
    }
  }
  /* box select (WM_gesture_box) */
  if (d->mode == M_BOX) {
    const float x0 = (float)BK_MIN(d->press_x, d->cur_x), y0 = (float)BK_MIN(d->press_y, d->cur_y);
    const float bw = (float)abs(d->cur_x - d->press_x), bh = (float)abs(d->cur_y - d->press_y);
    const float fill[4] = {1.0f, 1.0f, 1.0f, 0.05f}, edge[4] = {1.0f, 1.0f, 1.0f, 0.6f};
    bk_fill_f(x0, y0, bw, bh, fill);
    BUI_raster_frame_rect(bk_draw.r, x0, y0, bw, bh, 1.0f, edge);
  }
  /* the cut line (WM_gesture_lines) */
  if (d->mode == M_CUT) {
    const float c[4] = {1.0f, 1.0f, 1.0f, 0.7f};
    for (int i = 0; i + 1 < d->ncut; i++) {
      bk_line(d->cut[i][0], d->cut[i][1], d->cut[i + 1][0], d->cut[i + 1][1], c, 1.0f);
    }
  }
  bk_clip_pop();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Picking
 * \{ */

/* The topmost node under a point (the last drawn wins). */
static int node_at(const BKNode *e, int x, int y)
{
  const float gx = gx_of(e, (float)x), gy = gy_of(e, (float)y);
  for (int i = e->nkids - 1; i >= 0; i--) {
    const BKNode *node = e->kids[i];
    const GNode *g = gn(node);
    if (gx >= g->x && gx <= g->x + g->width && gy >= g->y && gy <= g->y + node_height(node)) {
      return i;
    }
  }
  return -1;
}

/* node_find_indicated_socket: the socket near a point. */
static bool socket_at(const BKNode *e, int x, int y, int *r_node, bool *r_input, int *r_k)
{
  const float s = scale_of(e);
  const float reach = BK_MAX(6.0f, (NODE_SOCKSIZE + 4.0f) * s);
  float best = reach * reach;
  bool found = false;
  for (int i = e->nkids - 1; i >= 0; i--) {
    const BKNode *node = e->kids[i];
    for (int pass = 0; pass < 2; pass++) {
      const bool input = pass == 1;
      const int count = sock_count(node, input);
      for (int k = 0; k < count; k++) {
        const float dx = sx(e, socket_x(node, input)) - x, dy = sy(e, socket_y(node, input, k)) - y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < best) {
          best = d2;
          *r_node = i;
          *r_input = input;
          *r_k = k;
          found = true;
        }
      }
    }
  }
  return found;
}

/* An unlinked input's field under a point. */
static bool field_at(const BKNode *e, int x, int y, int *r_node, int *r_k)
{
  const int i = node_at(e, x, y);
  if (i < 0 || gn(e->kids[i])->collapsed) {
    return false;
  }
  const BKNode *node = e->kids[i];
  const float s = scale_of(e);
  const float gx0 = sx(e, gn(node)->x) + NODE_DYS * s, gx1 = gx0 + (gn(node)->width - 2 * NODE_DYS) * s;
  if (x < gx0 || x > gx1) {
    return false;
  }
  const int count = sock_count(node, true);
  for (int k = 0; k < count; k++) {
    Sock so;
    sock_get(node, true, k, &so);
    const float cy = sy(e, socket_y(node, true, k));
    if (so.value[0] && fabsf(y - cy) <= NODE_DY * s * 0.5f && !input_linked(e, i, k, NULL)) {
      *r_node = i;
      *r_k = k;
      return true;
    }
  }
  return false;
}

/* A node's control under a point. */
static bool ctl_at(const BKNode *e, int x, int y, int *r_node, int *r_k)
{
  const int i = node_at(e, x, y);
  if (i < 0 || gn(e->kids[i])->collapsed) {
    return false;
  }
  const BKNode *node = e->kids[i];
  const float s = scale_of(e);
  const float x0 = sx(e, gn(node)->x) + NODE_DYS * s, x1 = x0 + (gn(node)->width - 2 * NODE_DYS) * s;
  if (x < x0 || x > x1) {
    return false;
  }
  const int count = ctl_count(node);
  for (int k = 0; k < count; k++) {
    if (fabsf(y - sy(e, ctl_y(node, k))) <= NODE_DY * s * 0.5f) {
      *r_node = i;
      *r_k = k;
      return true;
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

static void select_node(BKNode *e, BKNode *node, bool on)
{
  const bool was = (node->flags & NF_SELECTED) != 0;
  node->flags = on ? (node->flags | NF_SELECTED) : (node->flags & ~NF_SELECTED);
  if (was != on) {
    bk_emit(node, BK_EV_CHANGE, NULL, EV_SELECT, on);
  }
}

static void set_active(BKNode *e, BKNode *node)
{
  for (int i = 0; i < e->nkids; i++) {
    gn(e->kids[i])->active = e->kids[i] == node;
  }
}

/* node_sort: the active node draws on top. Moves the node to the end of the
 * children; link indices follow. */
static void raise_node(BKNode *e, int index)
{
  if (index < 0 || index == e->nkids - 1) {
    return;
  }
  BKNode *node = e->kids[index];
  for (int i = index; i < e->nkids - 1; i++) {
    e->kids[i] = e->kids[i + 1];
  }
  e->kids[e->nkids - 1] = node;
  /* remap links: index -> last, the ones after it shift down */
  Link l[MAX_LINKS];
  const int count = links_get(e, l, MAX_LINKS);
  const int last = e->nkids - 1;
  for (int i = 0; i < count; i++) {
    int *ends[2] = {&l[i].from, &l[i].to};
    for (int k = 0; k < 2; k++) {
      *ends[k] = *ends[k] == index ? last : (*ends[k] > index ? *ends[k] - 1 : *ends[k]);
    }
  }
  if (count) {
    links_set_ex(e, l, count, false); /* the same links, renumbered */
  }
  bk_emit(e, BK_EV_CHANGE, NULL, EV_LINKS + 2, index); /* a = 12: node order changed */
}

static void view_all(BKNode *e)
{
  NodeEdit *d = ed(e);
  if (e->nkids == 0) {
    d->zoom = 1.0f;
    d->panx = e->w * 0.5f;
    d->pany = e->h * 0.5f;
    return;
  }
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (int i = 0; i < e->nkids; i++) {
    const GNode *g = gn(e->kids[i]);
    x0 = BK_MIN(x0, g->x);
    y0 = BK_MIN(y0, g->y);
    x1 = BK_MAX(x1, g->x + g->width);
    y1 = BK_MAX(y1, g->y + node_height(e->kids[i]));
  }
  const float margin = 2.0f * u();
  const float bw = (x1 - x0) * bk_scale_fac, bh = (y1 - y0) * bk_scale_fac;
  float z = BK_MIN((e->w - 2 * margin) / BK_MAX(1.0f, bw), (e->h - 2 * margin) / BK_MAX(1.0f, bh));
  d->zoom = BK_CLAMP(z, 0.09f, 1.0f); /* space_node_view_flag: don't zoom past 1 */
  d->panx = e->w * 0.5f - (x0 + x1) * 0.5f * scale_of(e);
  d->pany = e->h * 0.5f - (y0 + y1) * 0.5f * scale_of(e);
}

static void zoom_at(BKNode *e, float factor, int x, int y)
{
  NodeEdit *d = ed(e);
  const float gx = gx_of(e, (float)x), gy = gy_of(e, (float)y);
  d->zoom = BK_CLAMP(d->zoom * factor, 0.09f, 2.31f); /* SpaceNode v2d min/max zoom */
  d->panx = x - e->x - gx * scale_of(e);
  d->pany = y - e->y - gy * scale_of(e);
  bk_invalidate(e);
}

static bool segments_cross(const float a[2], const float b[2], const float c[2], const float d[2])
{
  const float d1 = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
  const float d2 = (b[0] - a[0]) * (d[1] - a[1]) - (b[1] - a[1]) * (d[0] - a[0]);
  const float d3 = (d[0] - c[0]) * (a[1] - c[1]) - (d[1] - c[1]) * (a[0] - c[0]);
  const float d4 = (d[0] - c[0]) * (b[1] - c[1]) - (d[1] - c[1]) * (b[0] - c[0]);
  return (d1 > 0) != (d2 > 0) && (d3 > 0) != (d4 > 0);
}

/* NODE_OT_links_cut: remove the links the stroke crosses. */
static void cut_links(BKNode *e)
{
  NodeEdit *d = ed(e);
  Link l[MAX_LINKS], keep[MAX_LINKS];
  const int count = links_get(e, l, MAX_LINKS);
  int kept = 0;
  enum { N = NODE_LINK_RESOL * 2 };
  for (int i = 0; i < count; i++) {
    float x0, y0, x1, y1, pts[N + 1][2];
    link_ends(e, &l[i], &x0, &y0, &x1, &y1);
    bezier_points(x0, y0, x1, y1, pts, N);
    bool cut = false;
    for (int a = 0; a < N && !cut; a++) {
      for (int b = 0; b + 1 < d->ncut && !cut; b++) {
        cut = segments_cross(pts[a], pts[a + 1], d->cut[b], d->cut[b + 1]);
      }
    }
    if (!cut) {
      keep[kept++] = l[i];
    }
  }
  if (kept != count) {
    links_set(e, keep, kept);
  }
}

/* node_link_insert / add: an input takes one link, replacing the old one. */
static void add_link(BKNode *e, int from, int out, int to, int in)
{
  if (from == to) {
    return;
  }
  Link l[MAX_LINKS];
  int count = links_get(e, l, MAX_LINKS), w = 0;
  for (int i = 0; i < count; i++) {
    if (!(l[i].to == to && l[i].in == in)) {
      l[w++] = l[i];
    }
  }
  if (w < MAX_LINKS) {
    l[w++] = (Link){from, out, to, in};
  }
  links_set(e, l, w);
}

static void remove_input_link(BKNode *e, int to, int in)
{
  Link l[MAX_LINKS];
  int count = links_get(e, l, MAX_LINKS), w = 0;
  for (int i = 0; i < count; i++) {
    if (!(l[i].to == to && l[i].in == in)) {
      l[w++] = l[i];
    }
  }
  links_set(e, l, w);
}

static void set_input_value(BKNode *node, int k, double v)
{
  Sock so;
  if (!sock_get(node, true, k, &so)) {
    return;
  }
  const bool is_int = strcmp(so.type, "int") == 0;
  char buf[160];
  if (is_int) {
    snprintf(buf, sizeof(buf), "i|%s|%s|%d", so.name, so.type, (int)v);
  }
  else {
    snprintf(buf, sizeof(buf), "i|%s|%s|%.3f", so.name, so.type, v);
  }
  free(node->items.v[so.item]);
  node->items.v[so.item] = strdup(buf);
  bk_invalidate(node->parent);
  bk_emit(node, BK_EV_CHANGE, NULL, EV_VALUE, k);
}

/* A checkbox or dropdown takes a new value; every control reports it. */
static void ctl_set_value(BKNode *node, int k, int v)
{
  Ctl c;
  if (!ctl_get(node, k, &c)) {
    return;
  }
  if (c.kind != CTL_BUTTON) {
    char buf[512];
    if (c.kind == CTL_CHECK) {
      snprintf(buf, sizeof(buf), "c|%s|%d", c.label, v ? 1 : 0);
    }
    else {
      snprintf(buf, sizeof(buf), "d|%s|%d|%s", c.label, v, c.opts);
    }
    free(node->items.v[c.item]);
    node->items.v[c.item] = strdup(buf);
  }
  bk_invalidate(node->parent);
  bk_emit(node, BK_EV_CHANGE, NULL, EV_CONTROL, k);
}

/* A dropdown control opens a menu of its options, owned by the editor. */
static void ctl_open_menu(BKNode *e, BKNode *node, int k)
{
  BKContext *ctx = bk_ctx(e);
  Ctl c;
  if (!ctx || !ctl_get(node, k, &c)) {
    return;
  }
  BKMenuSpec items[64];
  char names[64][64];
  int n = 0;
  while (n < 64 && ctl_option(&c, n, names[n], sizeof(names[n]))) {
    memset(&items[n], 0, sizeof(items[n]));
    items[n].text = names[n];
    items[n].checked = -1;
    items[n].enabled = 1;
    items[n].data = (void *)(intptr_t)n; /* the picked option (editor_popup_pick) */
    n++;
  }
  if (!n) {
    return;
  }
  ed(e)->ctl_node = node;
  ed(e)->ctl_k = k;
  char cur[64];
  ctl_option(&c, c.value, cur, sizeof(cur));
  bk_open_popup(ctx, bk_menu_new(items, n, NULL, cur), e, NULL); /* the menu copies its items */
}

static void editor_popup_pick(BKNode *e, void *data)
{
  NodeEdit *d = ed(e);
  for (int i = 0; i < e->nkids; i++) { /* the node may have gone while the menu was open */
    if (e->kids[i] == d->ctl_node) {
      ctl_set_value(d->ctl_node, d->ctl_k, (int)(intptr_t)data);
      break;
    }
  }
}

static int editor_press(BKNode *e, BKEvent *ev)
{
  NodeEdit *d = ed(e);
  d->press_x = d->cur_x = ev->x;
  d->press_y = d->cur_y = ev->y;
  d->start_panx = d->panx;
  d->start_pany = d->pany;
  d->start_zoom = d->zoom;
  d->shift = EV_SHIFT(ev);
  d->hit = NULL;
  if (ev->mods & BK_BUTTON2) { /* VIEW2D_OT_pan, Ctrl: VIEW2D_OT_zoom */
    d->mode = EV_CTRL(ev) ? M_ZOOM : M_PAN;
    return 1;
  }
  if (ev->mods & BK_BUTTON3) { /* Ctrl+RMB: NODE_OT_links_cut */
    d->mode = EV_CTRL(ev) ? M_CUT : M_NONE;
    d->ncut = 0;
    return 1;
  }
  int ni, k;
  bool input;
  if (socket_at(e, ev->x, ev->y, &ni, &input, &k)) { /* NODE_OT_link */
    Link old;
    if (input && input_linked(e, ni, k, &old)) {
      /* pick up the link from the input: drag it from its output */
      remove_input_link(e, ni, k);
      d->link_node = old.from;
      d->link_sock = old.out;
      d->link_from_output = true;
    }
    else {
      d->link_node = ni;
      d->link_sock = k;
      d->link_from_output = !input;
    }
    d->mode = M_LINK;
    bk_invalidate(e);
    return 1;
  }
  if (ctl_at(e, ev->x, ev->y, &ni, &k)) { /* a node's button, checkbox or dropdown */
    d->mode = M_CONTROL;
    d->ctl_node = e->kids[ni];
    d->ctl_k = k;
    bk_invalidate(e);
    return 1;
  }
  if (field_at(e, ev->x, ev->y, &ni, &k)) { /* a node's number field */
    Sock so;
    sock_get(e->kids[ni], true, k, &so);
    d->mode = M_VALUE;
    d->value_node = ni;
    d->value_sock = k;
    d->value_start = atof(so.value);
    return 1;
  }
  const int i = node_at(e, ev->x, ev->y);
  if (i >= 0) { /* NODE_OT_select, then NODE_OT_translate on drag */
    BKNode *node = e->kids[i];
    const GNode *g = gn(node);
    const float s = scale_of(e);
    /* the collapse arrow in the header */
    if (ev->x < sx(e, g->x) + 1.0f * u() * d->zoom && ev->y < sy(e, g->y) + NODE_DY * s) {
      gn(node)->collapsed = !g->collapsed;
      bk_emit(node, BK_EV_CHANGE, NULL, EV_COLLAPSE, gn(node)->collapsed);
      d->mode = M_NONE;
      bk_invalidate(e);
      return 1;
    }
    if (EV_SHIFT(ev)) {
      select_node(e, node, !(node->flags & NF_SELECTED) || !g->active);
    }
    else if (!(node->flags & NF_SELECTED)) {
      for (int j = 0; j < e->nkids; j++) {
        select_node(e, e->kids[j], e->kids[j] == node);
      }
    }
    if (node->flags & NF_SELECTED) {
      set_active(e, node);
    }
    raise_node(e, i);
    d->hit = node;
    d->mode = M_MOVE;
    for (int j = 0; j < e->nkids; j++) { /* remember where they began */
      e->kids[j]->drag_x0 = gn(e->kids[j])->x;
      e->kids[j]->drag_start = gn(e->kids[j])->y;
    }
    bk_invalidate(e);
    return 1;
  }
  d->mode = M_BOX; /* tweak on empty space: box select; a click deselects */
  return 1;
}

static int editor_drag(BKNode *e, BKEvent *ev)
{
  NodeEdit *d = ed(e);
  d->cur_x = ev->x;
  d->cur_y = ev->y;
  switch (d->mode) {
    case M_PAN:
      d->panx = d->start_panx + ev->dx;
      d->pany = d->start_pany + ev->dy;
      break;
    case M_ZOOM: {
      const float f = powf(1.005f, (float)(ev->dx - ev->dy));
      d->zoom = d->start_zoom;
      d->panx = d->start_panx;
      d->pany = d->start_pany;
      zoom_at(e, f, d->press_x, d->press_y);
      break;
    }
    case M_MOVE: {
      const float s = scale_of(e);
      for (int j = 0; j < e->nkids; j++) {
        BKNode *node = e->kids[j];
        if (node->flags & NF_SELECTED) {
          float nx = (float)node->drag_x0 + ev->dx / s, ny = (float)node->drag_start + ev->dy / s;
          if (EV_CTRL(ev)) { /* snapping to the grid */
            nx = roundf(nx / NODE_DY) * NODE_DY;
            ny = roundf(ny / NODE_DY) * NODE_DY;
          }
          gn(node)->x = nx;
          gn(node)->y = ny;
        }
      }
      break;
    }
    case M_VALUE: {
      BKNode *node = e->kids[d->value_node];
      Sock so;
      sock_get(node, true, d->value_sock, &so);
      const bool is_int = strcmp(so.type, "int") == 0;
      /* numedit_but_NUM: floats 0.01 per pixel (Shift: a tenth), ints 1/16 */
      const double fac = is_int ? 1.0 / 16.0 : (EV_SHIFT(ev) ? 0.001 : 0.01);
      double v = d->value_start + ev->dx * fac;
      if (EV_CTRL(ev)) {
        v = is_int ? 10.0 * (long)(v / 10.0) : round(v * 10.0) * 0.1;
      }
      set_input_value(node, d->value_sock, v);
      break;
    }
    case M_CUT:
      if (d->ncut < MAX_CUT) {
        d->cut[d->ncut][0] = (float)ev->x;
        d->cut[d->ncut][1] = (float)ev->y;
        d->ncut++;
      }
      break;
    default:
      break;
  }
  bk_invalidate(e);
  return 1;
}

static int editor_release(BKNode *e, BKEvent *ev)
{
  NodeEdit *d = ed(e);
  const Mode mode = d->mode;
  d->mode = M_NONE;
  switch (mode) {
    case M_LINK: {
      int ni, k;
      bool input;
      if (socket_at(e, ev->x, ev->y, &ni, &input, &k) && input == d->link_from_output) {
        if (d->link_from_output) {
          add_link(e, d->link_node, d->link_sock, ni, k);
        }
        else {
          add_link(e, ni, k, d->link_node, d->link_sock);
        }
      }
      break;
    }
    case M_MOVE:
      if (ev->dragged) {
        for (int j = 0; j < e->nkids; j++) {
          if (e->kids[j]->flags & NF_SELECTED) {
            bk_emit(e->kids[j], BK_EV_CHANGE, NULL, EV_MOVED, 0);
          }
        }
      }
      else if (d->hit && !d->shift) { /* a click selects only that node */
        for (int j = 0; j < e->nkids; j++) {
          select_node(e, e->kids[j], e->kids[j] == d->hit);
        }
      }
      break;
    case M_BOX: {
      if (!ev->dragged) {
        for (int j = 0; j < e->nkids; j++) {
          select_node(e, e->kids[j], false);
        }
        break;
      }
      const float x0 = gx_of(e, (float)BK_MIN(d->press_x, ev->x));
      const float x1 = gx_of(e, (float)BK_MAX(d->press_x, ev->x));
      const float y0 = gy_of(e, (float)BK_MIN(d->press_y, ev->y));
      const float y1 = gy_of(e, (float)BK_MAX(d->press_y, ev->y));
      for (int j = 0; j < e->nkids; j++) {
        const GNode *g = gn(e->kids[j]);
        const bool in = g->x < x1 && g->x + g->width > x0 && g->y < y1 &&
                        g->y + node_height(e->kids[j]) > y0;
        if (in) {
          select_node(e, e->kids[j], true);
        }
        else if (!d->shift) {
          select_node(e, e->kids[j], false);
        }
      }
      break;
    }
    case M_CUT:
      cut_links(e);
      break;
    case M_CONTROL: {
      int ni, k;
      if (ctl_at(e, ev->x, ev->y, &ni, &k) && e->kids[ni] == d->ctl_node && k == d->ctl_k) {
        Ctl c;
        if (ctl_get(d->ctl_node, k, &c)) {
          if (c.kind == CTL_MENU) {
            ctl_open_menu(e, d->ctl_node, k);
          }
          else {
            ctl_set_value(d->ctl_node, k, c.kind == CTL_CHECK ? !c.value : 0);
          }
        }
      }
      break;
    }
    default:
      break;
  }
  bk_invalidate(e);
  return 1;
}

static void editor_cancel(BKNode *e)
{
  NodeEdit *d = ed(e);
  if (d->mode == M_MOVE) {
    for (int j = 0; j < e->nkids; j++) {
      gn(e->kids[j])->x = (float)e->kids[j]->drag_x0;
      gn(e->kids[j])->y = (float)e->kids[j]->drag_start;
    }
  }
  else if (d->mode == M_PAN || d->mode == M_ZOOM) {
    d->panx = d->start_panx;
    d->pany = d->start_pany;
    d->zoom = d->start_zoom;
  }
  else if (d->mode == M_VALUE) {
    set_input_value(e->kids[d->value_node], d->value_sock, d->value_start);
  }
  d->mode = M_NONE;
  bk_invalidate(e);
}

static int editor_wheel(BKNode *e, BKEvent *ev)
{
  NodeEdit *d = ed(e);
  const float step = 2.0f * u(); /* view2d scroll step */
  if (EV_CTRL(ev)) { /* VIEW2D_OT_scroll_right / left */
    d->panx += ev->delta * step;
  }
  else if (EV_SHIFT(ev)) { /* VIEW2D_OT_scroll_up / down */
    d->pany += ev->delta * step;
  }
  else { /* VIEW2D_OT_zoom_in / out about the cursor */
    zoom_at(e, ev->delta > 0 ? 1.25f : 0.8f, ev->x, ev->y);
  }
  bk_invalidate(e);
  return 1;
}

static int editor_key(BKNode *e, BKEvent *ev)
{
  const char *k = ev->keysym;
  if (bk_keysym_is(k, "x") || bk_keysym_is(k, "Delete")) { /* NODE_OT_delete */
    bk_emit(e, BK_EV_CHANGE, NULL, EV_DELETE, 0);
    return 1;
  }
  if (bk_keysym_is(k, "a")) { /* NODE_OT_select_all, Alt: deselect */
    for (int j = 0; j < e->nkids; j++) {
      select_node(e, e->kids[j], !EV_ALT(ev));
    }
    bk_invalidate(e);
    return 1;
  }
  if (bk_keysym_is(k, "Home")) { /* NODE_OT_view_all */
    view_all(e);
    bk_invalidate(e);
    return 1;
  }
  if (bk_keysym_is(k, "h") && !EV_CTRL(ev)) { /* NODE_OT_hide_toggle */
    for (int j = 0; j < e->nkids; j++) {
      if (e->kids[j]->flags & NF_SELECTED) {
        gn(e->kids[j])->collapsed = !gn(e->kids[j])->collapsed;
        bk_emit(e->kids[j], BK_EV_CHANGE, NULL, EV_COLLAPSE, gn(e->kids[j])->collapsed);
      }
    }
    bk_invalidate(e);
    return 1;
  }
  return 0;
}

static void editor_init(BKNode *e)
{
  NodeEdit *d = (NodeEdit *)calloc(1, sizeof(NodeEdit));
  d->zoom = 1.0f;
  d->panx = d->pany = -1e30f; /* centered on the first layout */
  e->ext = d;
}

static void editor_free(BKNode *e)
{
  free(e->ext);
  e->ext = NULL;
}

static void editor_measure(BKNode *e, int *rw, int *rh)
{
  *rw = 20 * u();
  /* "height" in units; unset, not a number (the test is false for NaN) or negative: 14. Capped so the
   * float-to-int conversion stays defined. */
  const float h = e->height_units;
  *rh = h > 0.0f ? (int)((h > 1000.0f ? 1000.0f : h) * u()) : 14 * u();
}

static void editor_arrange(BKNode *e)
{
  NodeEdit *d = ed(e);
  if (d->panx < -1e29f && e->w > 0) {
    view_all(e);
  }
  for (int i = 0; i < e->nkids; i++) { /* nodes are drawn by the editor */
    e->kids[i]->x = e->kids[i]->y = e->kids[i]->w = e->kids[i]->h = 0;
  }
}

static BKNode *editor_hit(BKNode *e, int x, int y)
{
  return e;
}

static int editor_cursor_at(BKNode *e, int x, int y)
{
  return ed(e)->mode == M_PAN ? BK_CURSOR_MOVE : BK_CURSOR_DEFAULT;
}

static int editor_set_num(BKNode *e, const char *key, double v)
{
  NodeEdit *d = ed(e);
  if (strcmp(key, "zoom") == 0) {
    d->zoom = (float)BK_CLAMP(v, 0.09, 2.31);
  }
  else if (strcmp(key, "view_all") == 0) {
    view_all(e);
  }
  else if (strcmp(key, "panx") == 0) {
    d->panx = (float)v;
  }
  else if (strcmp(key, "pany") == 0) {
    d->pany = (float)v;
  }
  else {
    return 0;
  }
  bk_invalidate(e);
  return 1;
}

static int editor_get_num(BKNode *e, const char *key, double *r)
{
  if (strcmp(key, "zoom") == 0) {
    *r = ed(e)->zoom;
    return 1;
  }
  if (strcmp(key, "panx") == 0 || strcmp(key, "pany") == 0) {
    *r = key[3] == 'x' ? ed(e)->panx : ed(e)->pany;
    return 1;
  }
  return 0;
}

const BKClass bk_cls_nodeeditor = {.name = "nodeeditor",
                                   .flags = CLS_INTERACTIVE | CLS_BUTTONS,
                                   .init = editor_init,
                                   .free_ext = editor_free,
                                   .measure = editor_measure,
                                   .arrange = editor_arrange,
                                   .draw = editor_draw,
                                   .draw_tree = editor_draw,
                                   .hit = editor_hit,
                                   .press = editor_press,
                                   .drag = editor_drag,
                                   .release = editor_release,
                                   .cancel = editor_cancel,
                                   .popup_pick = editor_popup_pick,
                                   .wheel = editor_wheel,
                                   .key = editor_key,
                                   .cursor_at = editor_cursor_at,
                                   .set_num = editor_set_num,
                                   .get_num = editor_get_num};

/** \} */
