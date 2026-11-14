/* buipy kit bindings: KitNode, KitTreeItem, KitContext.
 *
 * Thin wrappers over BUI_kit.h. The C kit owns all UI state and runs all
 * layout, input and drawing; Python only builds trees, sets properties and
 * receives callbacks (KitNode._on_event, menu/hotkey callables).
 *
 * Ownership: a node's Python wrapper owns the C node. Parents retain their
 * children's wrappers (Py_INCREF via the kit's ref hooks), so a subtree
 * lives as long as it is attached or referenced. C-owned nodes (a panel's
 * body, popups, picker parts) are exposed as borrowed wrappers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "buipy.h"

#include <stdlib.h>
#include <string.h>

BuiRaster *buipy_current_raster(void);
BuiRaster *buipy_raster_of(PyObject *obj);

/* -------------------------------------------------------------------- */
/** \name Error stash (exceptions raised inside callbacks)
 * \{ */

static PyObject *g_err_type, *g_err_value, *g_err_tb;

void buipy_stash_error(void)
{
  if (!PyErr_Occurred()) {
    return;
  }
  if (g_err_type) {
    PyErr_Clear(); /* keep the first error */
    return;
  }
  PyErr_Fetch(&g_err_type, &g_err_value, &g_err_tb);
}

int buipy_restore_error(void)
{
  if (!g_err_type) {
    return 0;
  }
  PyErr_Restore(g_err_type, g_err_value, g_err_tb);
  g_err_type = g_err_value = g_err_tb = NULL;
  return 1;
}

static void ref_retain(void *ref)
{
  Py_INCREF((PyObject *)ref);
}

static void ref_release(void *ref)
{
  Py_DECREF((PyObject *)ref);
}

/* The kit calls back into Python from places where the GIL may not be held: a native window's
 * message can be dispatched by another toolkit sharing the thread (Tk's update() releases the GIL
 * around Tcl, and Tcl's notifier dispatches every window message of the thread, BUI's included).
 * Every callback the kit gets is therefore one of these wrappers, which take the GIL first
 * (GIL_ENTER, buipy.h); PyGILState_Ensure is cheap and nests, so it also does no harm when the
 * GIL is already held. */

static void ref_retain_gil(void *ref)
{
  GIL_ENTER;
  ref_retain(ref);
  GIL_LEAVE;
}

static void ref_release_gil(void *ref)
{
  GIL_ENTER;
  ref_release(ref);
  GIL_LEAVE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name KitNode
 * \{ */

typedef struct KitNodeObject {
  PyObject_HEAD
  BKNode *node;
  int borrowed; /* C-owned node: never freed by this wrapper */
  PyObject *dict;
} KitNodeObject;

static PyTypeObject KitNode_Type;
static PyTypeObject KitTreeItem_Type;

static PyObject *node_wrap(BKNode *node)
{
  if (!node) {
    Py_RETURN_NONE;
  }
  if (BK_node_ref(node)) {
    PyObject *o = (PyObject *)BK_node_ref(node);
    Py_INCREF(o);
    return o;
  }
  KitNodeObject *o = (KitNodeObject *)KitNode_Type.tp_alloc(&KitNode_Type, 0);
  if (o) {
    o->node = node;
    o->borrowed = 1;
  }
  return (PyObject *)o;
}

static BKNode *node_of(PyObject *obj)
{
  if (!PyObject_TypeCheck(obj, &KitNode_Type) || !((KitNodeObject *)obj)->node) {
    PyErr_SetString(PyExc_TypeError, "expected a KitNode");
    return NULL;
  }
  return ((KitNodeObject *)obj)->node;
}

static int kitnode_init(KitNodeObject *self, PyObject *args, PyObject *kw)
{
  const char *type;
  if (!PyArg_ParseTuple(args, "s:KitNode", &type)) {
    return -1;
  }
  if (self->node) {
    PyErr_SetString(PyExc_RuntimeError, "KitNode already initialized");
    return -1;
  }
  self->node = BK_node_new(type);
  if (!self->node) {
    PyErr_Format(PyExc_ValueError, "unknown kit node type '%s'", type);
    return -1;
  }
  BK_node_set_ref(self->node, self);
  return 0;
}

BKNode *buipy_kit_node(PyObject *obj)
{
  if (!obj || !PyObject_TypeCheck(obj, &KitNode_Type) || !((KitNodeObject *)obj)->node) {
    PyErr_SetString(PyExc_TypeError, "expected a live KitNode");
    return NULL;
  }
  return ((KitNodeObject *)obj)->node;
}

static void kitnode_dealloc(KitNodeObject *self)
{
  PyObject_GC_UnTrack(self);
  Py_CLEAR(self->dict);
  if (self->node && !self->borrowed) {
    BK_node_set_ref(self->node, NULL);
    BK_node_free(self->node);
  }
  self->node = NULL;
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static int kitnode_traverse(KitNodeObject *self, visitproc visit, void *arg)
{
  Py_VISIT(self->dict);
  return 0;
}

static int kitnode_clear(KitNodeObject *self)
{
  Py_CLEAR(self->dict);
  return 0;
}

static int parse_rgba(PyObject *v, float out[4])
{
  PyObject *seq = PySequence_Fast(v, "color must be a sequence of 3 or 4 floats");
  if (!seq) {
    return 0;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  if (n != 3 && n != 4) {
    Py_DECREF(seq);
    PyErr_SetString(PyExc_ValueError, "color must have 3 or 4 components");
    return 0;
  }
  out[3] = 1.0f;
  for (Py_ssize_t i = 0; i < n; i++) {
    out[i] = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, i));
  }
  Py_DECREF(seq);
  return !PyErr_Occurred();
}

static PyObject *kitnode_set(KitNodeObject *self, PyObject *args)
{
  const char *key;
  PyObject *v;
  if (!PyArg_ParseTuple(args, "sO:set", &key, &v) || !self->node) {
    return NULL;
  }
  int ok;
  if (v == Py_None) {
    ok = BK_set_str(self->node, key, "");
  }
  else if (PyUnicode_Check(v)) {
    ok = BK_set_str(self->node, key, PyUnicode_AsUTF8(v));
  }
  else if (PyBool_Check(v) || PyLong_Check(v) || PyFloat_Check(v)) {
    ok = BK_set_num(self->node, key, PyFloat_AsDouble(v));
  }
  else if (PySequence_Check(v)) {
    const Py_ssize_t n = PySequence_Size(v);
    PyObject *first = n > 0 ? PySequence_GetItem(v, 0) : NULL;
    const int strings = first && (PyUnicode_Check(first) || first == Py_None);
    Py_XDECREF(first);
    if (strings || n == 0) {
      const char **vals = (const char **)calloc((size_t)(n > 0 ? n : 1), sizeof(char *));
      PyObject *seq = PySequence_Fast(v, "expected a sequence");
      for (Py_ssize_t i = 0; seq && i < n; i++) {
        PyObject *it = PySequence_Fast_GET_ITEM(seq, i);
        vals[i] = (it == Py_None) ? "" : PyUnicode_AsUTF8(it);
      }
      ok = (seq && !PyErr_Occurred()) ? BK_set_strs(self->node, key, vals, (int)n) : 0;
      Py_XDECREF(seq);
      free(vals);
      if (PyErr_Occurred()) {
        return NULL;
      }
    }
    else {
      float c[4];
      if (!parse_rgba(v, c)) {
        return NULL;
      }
      ok = BK_set_color(self->node, key, c);
    }
  }
  else {
    PyErr_Format(PyExc_TypeError, "unsupported value for '%s'", key);
    return NULL;
  }
  if (buipy_restore_error()) {
    return NULL;
  }
  if (!ok) {
    PyErr_Format(PyExc_KeyError, "'%s' has no property '%s'", BK_node_type(self->node), key);
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kitnode_get(KitNodeObject *self, PyObject *args)
{
  const char *key;
  if (!PyArg_ParseTuple(args, "s:get", &key) || !self->node) {
    return NULL;
  }
  return PyFloat_FromDouble(BK_get_num(self->node, key));
}

static PyObject *kitnode_get_str(KitNodeObject *self, PyObject *args)
{
  const char *key;
  if (!PyArg_ParseTuple(args, "s:get_str", &key) || !self->node) {
    return NULL;
  }
  return PyUnicode_FromString(BK_get_str(self->node, key));
}

static PyObject *kitnode_get_color(KitNodeObject *self, PyObject *args)
{
  const char *key = "color";
  if (!PyArg_ParseTuple(args, "|s:get_color", &key) || !self->node) {
    return NULL;
  }
  float c[4] = {0, 0, 0, 0};
  BK_get_color(self->node, key, c);
  return Py_BuildValue("(dddd)", c[0], c[1], c[2], c[3]);
}

static PyObject *kitnode_strs(KitNodeObject *self, PyObject *args)
{
  const char *key = "items";
  if (!PyArg_ParseTuple(args, "|s:strs", &key) || !self->node) {
    return NULL;
  }
  const int n = BK_strs_count(self->node, key);
  PyObject *list = PyList_New(n);
  for (int i = 0; list && i < n; i++) {
    PyList_SET_ITEM(list, i, PyUnicode_FromString(BK_strs_get(self->node, key, i)));
  }
  return list;
}

static PyObject *kitnode_add(KitNodeObject *self, PyObject *args)
{
  PyObject *child;
  int index = -1;
  if (!PyArg_ParseTuple(args, "O|i:add", &child, &index) || !self->node) {
    return NULL;
  }
  BKNode *c = node_of(child);
  if (!c) {
    return NULL;
  }
  BK_node_add(self->node, c, index);
  Py_INCREF(child);
  return child;
}

static PyObject *kitnode_remove(KitNodeObject *self, PyObject *child)
{
  BKNode *c = node_of(child);
  if (!c || !self->node) {
    return NULL;
  }
  BK_node_remove(self->node, c);
  Py_RETURN_NONE;
}

static PyObject *kitnode_children(KitNodeObject *self, void *closure)
{
  const int n = self->node ? BK_node_child_count(self->node) : 0;
  PyObject *list = PyList_New(n);
  for (int i = 0; list && i < n; i++) {
    PyList_SET_ITEM(list, i, node_wrap(BK_node_child(self->node, i)));
  }
  return list;
}

static PyObject *kitnode_parent(KitNodeObject *self, void *closure)
{
  return node_wrap(self->node ? BK_node_parent(self->node) : NULL);
}

static PyObject *kitnode_rect(KitNodeObject *self, void *closure)
{
  int r[4] = {0, 0, 0, 0};
  if (self->node) {
    BK_node_rect(self->node, r);
  }
  return Py_BuildValue("(iiii)", r[0], r[1], r[2], r[3]);
}

static PyObject *kitnode_coord(KitNodeObject *self, void *closure)
{
  int r[4] = {0, 0, 0, 0};
  if (self->node) {
    BK_node_rect(self->node, r);
  }
  return PyLong_FromLong(r[(intptr_t)closure]);
}

static PyObject *kitnode_type(KitNodeObject *self, void *closure)
{
  return PyUnicode_FromString(self->node ? BK_node_type(self->node) : "");
}

static PyObject *kitnode_natural(KitNodeObject *self, PyObject *unused)
{
  int s[2] = {0, 0};
  if (self->node) {
    BK_node_natural(self->node, s);
  }
  return Py_BuildValue("(ii)", s[0], s[1]);
}

static PyObject *kitnode_invalidate(KitNodeObject *self, PyObject *unused)
{
  if (self->node) {
    BK_node_invalidate(self->node);
  }
  Py_RETURN_NONE;
}

static PyObject *kitnode_relayout(KitNodeObject *self, PyObject *unused)
{
  if (self->node) {
    BK_node_relayout(self->node);
  }
  Py_RETURN_NONE;
}

static PyObject *kitnode_on_event(PyObject *self, PyObject *args)
{
  Py_RETURN_NONE; /* overridden by Python components */
}

/* -- data: ramp / curve / histogram / grid ------------------------------------ */

static PyObject *kitnode_ramp(KitNodeObject *self, PyObject *unused)
{
  const int n = BK_ramp_count(self->node);
  PyObject *list = PyList_New(n);
  for (int i = 0; list && i < n; i++) {
    float pos, c[4];
    BK_ramp_get(self->node, i, &pos, c);
    PyList_SET_ITEM(list, i, Py_BuildValue("(d(dddd))", pos, c[0], c[1], c[2], c[3]));
  }
  return list;
}

static PyObject *kitnode_set_ramp(KitNodeObject *self, PyObject *stops)
{
  PyObject *seq = PySequence_Fast(stops, "stops must be a sequence of (pos, color)");
  if (!seq) {
    return NULL;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  float *pos = (float *)calloc((size_t)(n + 1), sizeof(float));
  float(*col)[4] = (float(*)[4])calloc((size_t)(n + 1), sizeof(float[4]));
  for (Py_ssize_t i = 0; i < n; i++) {
    PyObject *p, *c;
    if (!PyArg_ParseTuple(PySequence_Fast_GET_ITEM(seq, i), "OO", &p, &c) || !parse_rgba(c, col[i])) {
      break;
    }
    pos[i] = (float)PyFloat_AsDouble(p);
  }
  if (!PyErr_Occurred()) {
    BK_ramp_set(self->node, pos, (const float(*)[4])col, (int)n);
  }
  free(pos);
  free(col);
  Py_DECREF(seq);
  if (PyErr_Occurred()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kitnode_ramp_eval(KitNodeObject *self, PyObject *t)
{
  float c[4];
  BK_ramp_evaluate(self->node, (float)PyFloat_AsDouble(t), c);
  return Py_BuildValue("(dddd)", c[0], c[1], c[2], c[3]);
}

static PyObject *kitnode_set_ramp_color(KitNodeObject *self, PyObject *c)
{
  float rgba[4];
  if (!parse_rgba(c, rgba)) {
    return NULL;
  }
  BK_ramp_set_active_color(self->node, rgba);
  Py_RETURN_NONE;
}

static PyObject *kitnode_curve(KitNodeObject *self, PyObject *args)
{
  int ch = 0;
  if (!PyArg_ParseTuple(args, "|i:curve", &ch)) {
    return NULL;
  }
  const int n = BK_curve_count(self->node, ch);
  PyObject *list = PyList_New(n);
  for (int i = 0; list && i < n; i++) {
    float xy[2];
    BK_curve_get(self->node, ch, i, xy);
    PyList_SET_ITEM(list, i, Py_BuildValue("(dd)", xy[0], xy[1]));
  }
  return list;
}

static PyObject *kitnode_set_curve(KitNodeObject *self, PyObject *args)
{
  int ch;
  PyObject *pts;
  if (!PyArg_ParseTuple(args, "iO:set_curve", &ch, &pts)) {
    return NULL;
  }
  PyObject *seq = PySequence_Fast(pts, "points must be a sequence of (x, y)");
  if (!seq) {
    return NULL;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  float(*xy)[2] = (float(*)[2])calloc((size_t)(n + 1), sizeof(float[2]));
  for (Py_ssize_t i = 0; i < n; i++) {
    if (!PyArg_ParseTuple(PySequence_Fast_GET_ITEM(seq, i), "ff", &xy[i][0], &xy[i][1])) {
      break;
    }
  }
  if (!PyErr_Occurred()) {
    BK_curve_set(self->node, ch, (const float(*)[2])xy, (int)n);
  }
  free(xy);
  Py_DECREF(seq);
  if (PyErr_Occurred()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kitnode_curve_eval(KitNodeObject *self, PyObject *args)
{
  int ch;
  float x;
  if (!PyArg_ParseTuple(args, "if:curve_eval", &ch, &x)) {
    return NULL;
  }
  return PyFloat_FromDouble(BK_curve_evaluate(self->node, ch, x));
}

static PyObject *kitnode_set_histogram(KitNodeObject *self, PyObject *args)
{
  PyObject *bins;
  int channels = 1;
  if (!PyArg_ParseTuple(args, "O|i:set_histogram", &bins, &channels)) {
    return NULL;
  }
  PyObject *seq = PySequence_Fast(bins, "bins must be a sequence");
  if (!seq) {
    return NULL;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  float *data = (float *)calloc((size_t)(n * channels + 1), sizeof(float));
  for (Py_ssize_t i = 0; i < n; i++) {
    PyObject *it = PySequence_Fast_GET_ITEM(seq, i);
    if (channels == 1) {
      data[i] = (float)PyFloat_AsDouble(it);
    }
    else {
      for (int c = 0; c < channels; c++) {
        PyObject *v = PySequence_GetItem(it, c);
        data[i * channels + c] = v ? (float)PyFloat_AsDouble(v) : 0.0f;
        Py_XDECREF(v);
      }
    }
  }
  if (!PyErr_Occurred()) {
    BK_histogram_set(self->node, data, (int)n, channels);
  }
  free(data);
  Py_DECREF(seq);
  if (PyErr_Occurred()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kitnode_set_grid_colors(KitNodeObject *self, PyObject *colors)
{
  PyObject *seq = PySequence_Fast(colors, "colors must be a sequence");
  if (!seq) {
    return NULL;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  float(*c)[4] = (float(*)[4])calloc((size_t)(n + 1), sizeof(float[4]));
  for (Py_ssize_t i = 0; i < n; i++) {
    PyObject *it = PySequence_Fast_GET_ITEM(seq, i);
    if (it != Py_None && !parse_rgba(it, c[i])) {
      break;
    }
  }
  if (!PyErr_Occurred()) {
    BK_gridview_set_colors(self->node, (const float(*)[4])c, (int)n);
  }
  free(c);
  Py_DECREF(seq);
  if (PyErr_Occurred()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

/* -- menus: convert MenuItem-like Python objects to BKMenuSpec ---------------- */

static int build_specs(PyObject *items, BKMenuSpec **r_specs, int *r_n);

static void free_specs(BKMenuSpec *specs, int n)
{
  for (int i = 0; i < n; i++) {
    free_specs((BKMenuSpec *)specs[i].sub, specs[i].nsub);
  }
  free(specs);
}

static int build_spec(PyObject *it, BKMenuSpec *s)
{
  memset(s, 0, sizeof(*s));
  s->checked = -1;
  s->enabled = 1;
  if (it == Py_None || PyUnicode_Check(it)) {
    s->text = it == Py_None ? "-" : PyUnicode_AsUTF8(it);
    return s->text != NULL;
  }
  PyObject *v = PyObject_GetAttrString(it, "text");
  if (!v) {
    return 0;
  }
  s->text = PyUnicode_AsUTF8(v); /* owned by `it`, alive for the call */
  Py_DECREF(v);
  if (!s->text) {
    return 0;
  }
#define OPT_ATTR(name, code) \
  if ((v = PyObject_GetAttrString(it, name)) != NULL) { \
    if (v != Py_None) { \
      code; \
    } \
    Py_DECREF(v); \
  } \
  else { \
    PyErr_Clear(); \
  }
  OPT_ATTR("icon", s->icon = BK_icon_id(PyUnicode_Check(v) ? PyUnicode_AsUTF8(v) : ""));
  OPT_ATTR("shortcut", s->shortcut = PyUnicode_AsUTF8(v));
  OPT_ATTR("checked", s->checked = PyObject_IsTrue(v));
  OPT_ATTR("enabled", s->enabled = PyObject_IsTrue(v));
  OPT_ATTR("on_click", s->ref = v);
  OPT_ATTR("submenu", {
    BKMenuSpec *sub;
    int nsub;
    if (!build_specs(v, &sub, &nsub)) {
      Py_DECREF(v);
      return 0;
    }
    s->sub = sub;
    s->nsub = nsub;
  });
#undef OPT_ATTR
  return !PyErr_Occurred();
}

static int build_specs(PyObject *items, BKMenuSpec **r_specs, int *r_n)
{
  PyObject *seq = PySequence_Fast(items, "menu items must be a sequence");
  if (!seq) {
    return 0;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  BKMenuSpec *specs = (BKMenuSpec *)calloc((size_t)(n + 1), sizeof(BKMenuSpec));
  for (Py_ssize_t i = 0; i < n; i++) {
    if (!build_spec(PySequence_Fast_GET_ITEM(seq, i), &specs[i])) {
      free_specs(specs, (int)i);
      Py_DECREF(seq);
      return 0;
    }
  }
  Py_DECREF(seq);
  *r_specs = specs;
  *r_n = (int)n;
  return 1;
}

static PyObject *kitnode_set_menu(KitNodeObject *self, PyObject *items)
{
  BKMenuSpec *specs;
  int n;
  if (!build_specs(items, &specs, &n)) {
    return NULL;
  }
  BK_menubutton_set_items(self->node, specs, n);
  free_specs(specs, n);
  Py_RETURN_NONE;
}

/* -- tree ------------------------------------------------------------------------ */

typedef struct KitTreeItemObject {
  PyObject_HEAD
  BKTreeItem *item;
  PyObject *dict;
} KitTreeItemObject;

static PyObject *item_wrap(BKTreeItem *it)
{
  if (!it || !BK_tree_item_ref(it)) {
    Py_RETURN_NONE;
  }
  PyObject *o = (PyObject *)BK_tree_item_ref(it);
  Py_INCREF(o);
  return o;
}

static PyObject *kitnode_set_tree_roots(KitNodeObject *self, PyObject *roots)
{
  PyObject *seq = PySequence_Fast(roots, "roots must be a sequence of KitTreeItem");
  if (!seq) {
    return NULL;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  BKTreeItem **items = (BKTreeItem **)calloc((size_t)(n + 1), sizeof(BKTreeItem *));
  for (Py_ssize_t i = 0; i < n; i++) {
    PyObject *it = PySequence_Fast_GET_ITEM(seq, i);
    if (!PyObject_TypeCheck(it, &KitTreeItem_Type)) {
      PyErr_SetString(PyExc_TypeError, "expected KitTreeItem");
      break;
    }
    items[i] = ((KitTreeItemObject *)it)->item;
  }
  if (!PyErr_Occurred()) {
    BK_tree_set_roots(self->node, items, (int)n);
  }
  free(items);
  Py_DECREF(seq);
  if (PyErr_Occurred()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kitnode_tree_selected(KitNodeObject *self, PyObject *unused)
{
  return item_wrap(BK_tree_selected(self->node));
}

static PyObject *kitnode_tree_select(KitNodeObject *self, PyObject *item)
{
  BK_tree_select(self->node, item == Py_None ? NULL : ((KitTreeItemObject *)item)->item);
  Py_RETURN_NONE;
}

static PyObject *kitnode_tree_rows(KitNodeObject *self, PyObject *unused)
{
  const int n = BK_tree_row_count(self->node);
  PyObject *list = PyList_New(n);
  for (int i = 0; list && i < n; i++) {
    int r[4];
    BK_tree_row_rect(self->node, i, r);
    PyObject *it = item_wrap(BK_tree_row(self->node, i));
    PyList_SET_ITEM(list, i, Py_BuildValue("(N(iiii))", it, r[0], r[1], r[2], r[3]));
  }
  return list;
}

static PyObject *kit_link_fields(PyObject *mod, PyObject *fields)
{
  PyObject *seq = PySequence_Fast(fields, "expected a sequence of number nodes");
  if (!seq) {
    return NULL;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  BKNode **nodes = (BKNode **)calloc((size_t)(n + 1), sizeof(BKNode *));
  for (Py_ssize_t i = 0; i < n; i++) {
    nodes[i] = node_of(PySequence_Fast_GET_ITEM(seq, i));
    if (!nodes[i]) {
      break;
    }
  }
  if (!PyErr_Occurred()) {
    BK_link_fields(nodes, (int)n);
  }
  free(nodes);
  Py_DECREF(seq);
  if (PyErr_Occurred()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

/* -- geometry managers ----------------------------------------------------------- */

static PyObject *kitnode_geom(KitNodeObject *self, PyObject *args, PyObject *kw)
{
  int manager;
  if (!PyArg_ParseTuple(args, "i:geom", &manager)) {
    return NULL;
  }
  BK_geom_set_manager(self->node, manager);
  if (kw) {
    PyObject *key, *value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(kw, &pos, &key, &value)) {
      const double v = PyFloat_AsDouble(value);
      if (PyErr_Occurred()) {
        return NULL;
      }
      BK_geom_set(self->node, PyUnicode_AsUTF8(key), v);
    }
  }
  Py_RETURN_NONE;
}

static PyObject *kitnode_geom_get(KitNodeObject *self, PyObject *args)
{
  const char *key;
  if (!PyArg_ParseTuple(args, "s:geom_get", &key)) {
    return NULL;
  }
  return PyFloat_FromDouble(BK_geom_get(self->node, key));
}

static PyObject *kitnode_grid_weight(KitNodeObject *self, PyObject *args)
{
  int column_axis, index, minsize = 0;
  double weight;
  if (!PyArg_ParseTuple(args, "iid|i:grid_weight", &column_axis, &index, &weight, &minsize)) {
    return NULL;
  }
  BK_frame_grid_weight(self->node, column_axis, index, weight, minsize);
  Py_RETURN_NONE;
}

/* -- canvas ---------------------------------------------------------------------- */

static int opt_rgba(PyObject *v, float out[4])
{
  out[0] = out[1] = out[2] = out[3] = 0.0f;
  if (!v || v == Py_None) {
    return 1;
  }
  return parse_rgba(v, out);
}

static PyObject *kitnode_canvas_set(KitNodeObject *self, PyObject *args, PyObject *kw)
{
  static char *kwlist[] = {"id", "type", "coords", "fill", "outline", "width", "text", "anchor",
                           "radius", "size", "start", "extent", "image", "hidden", "src", "fit",
                           "frame", NULL};
  int id, type, anchor = 0, hidden = 0, fit = BK_FIT_CONTAIN, frame = 0;
  PyObject *coords, *fill = NULL, *outline = NULL, *image = NULL;
  float width = 1.0f, radius = 0.0f, size = 0.0f, start = 0.0f, extent = 90.0f;
  const char *text = NULL, *src = NULL;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "iiO|OOfziffffOpzii:canvas_set", kwlist, &id, &type,
                                   &coords, &fill, &outline, &width, &text, &anchor, &radius,
                                   &size, &start, &extent, &image, &hidden, &src, &fit, &frame))
  {
    return NULL;
  }
  BKCanvasItem it;
  memset(&it, 0, sizeof(it));
  it.id = id;
  it.type = type;
  it.width = width;
  it.text = text;
  it.anchor = anchor;
  it.radius = radius;
  it.size = size;
  it.start = start;
  it.extent = extent;
  it.hidden = hidden;
  it.fit = fit;
  it.frame = frame;
  if (src && image && image != Py_None) {
    PyErr_SetString(PyExc_ValueError, "an image item takes image= or src=, not both");
    return NULL;
  }
  it.src = src; /* a file decoded on the kit's image workers (copied) */
  if (!opt_rgba(fill, it.fill) || !opt_rgba(outline, it.outline)) {
    return NULL;
  }
  PyObject *seq = PySequence_Fast(coords, "coords must be a sequence of numbers");
  if (!seq) {
    return NULL;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  float *c = (float *)calloc((size_t)(n + 1), sizeof(float));
  for (Py_ssize_t i = 0; i < n; i++) {
    c[i] = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, i));
  }
  Py_DECREF(seq);
  it.coords = c;
  it.ncoords = (int)n;
  Py_buffer buf = {0};
  if (image && image != Py_None) {
    PyObject *data;
    if (!PyArg_ParseTuple(image, "iiO", &it.iw, &it.ih, &data) ||
        PyObject_GetBuffer(data, &buf, PyBUF_SIMPLE) < 0)
    {
      free(c);
      return NULL;
    }
    if (buf.len < (Py_ssize_t)it.iw * it.ih * 4) {
      PyBuffer_Release(&buf);
      free(c);
      PyErr_SetString(PyExc_ValueError, "image data too short (need w*h*4 RGBA bytes)");
      return NULL;
    }
    it.rgba = (const unsigned char *)buf.buf;
  }
  if (!PyErr_Occurred()) {
    BK_canvas_set_item(self->node, &it);
  }
  if (buf.buf) {
    PyBuffer_Release(&buf);
  }
  free(c);
  if (PyErr_Occurred()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kitnode_canvas_delete(KitNodeObject *self, PyObject *arg)
{
  BK_canvas_delete(self->node, (int)PyLong_AsLong(arg));
  Py_RETURN_NONE;
}

static PyObject *kitnode_canvas_raise(KitNodeObject *self, PyObject *args)
{
  int id, top = 1;
  if (!PyArg_ParseTuple(args, "i|p:canvas_raise", &id, &top)) {
    return NULL;
  }
  BK_canvas_raise(self->node, id, top);
  Py_RETURN_NONE;
}

static PyObject *kitnode_set_image(KitNodeObject *self, PyObject *args)
{
  int w = 0, h = 0;
  PyObject *data = Py_None;
  if (!PyArg_ParseTuple(args, "|iiO:set_image", &w, &h, &data)) {
    return NULL;
  }
  if (data == Py_None) {
    BK_node_set_image(self->node, NULL, 0, 0);
    Py_RETURN_NONE;
  }
  Py_buffer buf;
  if (PyObject_GetBuffer(data, &buf, PyBUF_SIMPLE) < 0) {
    return NULL;
  }
  if (buf.len < (Py_ssize_t)w * h * 4) {
    PyBuffer_Release(&buf);
    PyErr_SetString(PyExc_ValueError, "image data too short (need w*h*4 RGBA bytes)");
    return NULL;
  }
  BK_node_set_image(self->node, (const unsigned char *)buf.buf, w, h);
  PyBuffer_Release(&buf);
  Py_RETURN_NONE;
}

static PyObject *kitnode_set_image_frame(KitNodeObject *self, PyObject *arg)
{
  const long frame = PyLong_AsLong(arg);
  if (frame == -1 && PyErr_Occurred()) {
    return NULL;
  }
  BK_node_set_image_frame(self->node, (int)frame);
  Py_RETURN_NONE;
}

static PyObject *kitnode_set_image_source(KitNodeObject *self, PyObject *args)
{
  const char *src = NULL;
  int fit = BK_FIT_CONTAIN;
  if (!PyArg_ParseTuple(args, "z|i:set_image_source", &src, &fit)) {
    return NULL;
  }
  BK_node_set_image_source(self->node, src, fit);
  Py_RETURN_NONE;
}

/* -- text area -------------------------------------------------------------------- */

static PyObject *kitnode_ta_insert(KitNodeObject *self, PyObject *args)
{
  int off;
  const char *text;
  if (!PyArg_ParseTuple(args, "is:ta_insert", &off, &text)) {
    return NULL;
  }
  BK_textarea_insert(self->node, off, text);
  Py_RETURN_NONE;
}

static PyObject *kitnode_ta_delete(KitNodeObject *self, PyObject *args)
{
  int a, b;
  if (!PyArg_ParseTuple(args, "ii:ta_delete", &a, &b)) {
    return NULL;
  }
  BK_textarea_delete(self->node, a, b);
  Py_RETURN_NONE;
}

static PyObject *kitnode_ta_caret(KitNodeObject *self, PyObject *args)
{
  int off = -1;
  if (!PyArg_ParseTuple(args, "|i:ta_caret", &off)) {
    return NULL;
  }
  if (off >= 0) {
    BK_textarea_set_caret(self->node, off);
  }
  return PyLong_FromLong(BK_textarea_caret(self->node));
}

static PyObject *kitnode_ta_selection(KitNodeObject *self, PyObject *args)
{
  int a = -1, b = -1;
  if (!PyArg_ParseTuple(args, "|ii:ta_selection", &a, &b)) {
    return NULL;
  }
  if (a >= 0) {
    BK_textarea_set_selection(self->node, a, b);
  }
  if (!BK_textarea_selection(self->node, &a, &b)) {
    Py_RETURN_NONE;
  }
  return Py_BuildValue("(ii)", a, b);
}

static PyObject *kitnode_ta_spans(KitNodeObject *self, PyObject *spans)
{
  PyObject *seq = PySequence_Fast(spans, "spans must be [(start, end, fg, bg=None)]");
  if (!seq) {
    return NULL;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  BKTextSpan *out = (BKTextSpan *)calloc((size_t)(n + 1), sizeof(BKTextSpan));
  for (Py_ssize_t i = 0; i < n; i++) {
    PyObject *fg = Py_None, *bg = Py_None;
    BKTextSpan *s = &out[i];
    if (!PyArg_ParseTuple(PySequence_Fast_GET_ITEM(seq, i), "iiO|O", &s->start, &s->end, &fg,
                          &bg))
    {
      break;
    }
    s->has_fg = fg != Py_None;
    s->has_bg = bg != Py_None;
    if ((s->has_fg && !parse_rgba(fg, s->fg)) || (s->has_bg && !parse_rgba(bg, s->bg))) {
      break;
    }
  }
  if (!PyErr_Occurred()) {
    BK_textarea_set_spans(self->node, out, (int)n);
  }
  free(out);
  Py_DECREF(seq);
  if (PyErr_Occurred()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kitnode_ta_index_at(KitNodeObject *self, PyObject *args)
{
  int x, y;
  if (!PyArg_ParseTuple(args, "ii:ta_index_at", &x, &y)) {
    return NULL;
  }
  return PyLong_FromLong(BK_textarea_index_at(self->node, x, y));
}

static PyObject *kitnode_ta_see(KitNodeObject *self, PyObject *arg)
{
  BK_textarea_see(self->node, (int)PyLong_AsLong(arg));
  Py_RETURN_NONE;
}

static PyMethodDef kitnode_methods[] = {
    {"geom", (PyCFunction)(void (*)(void))kitnode_geom, METH_VARARGS | METH_KEYWORDS,
     "geom(manager, **params): pack=1 grid=2 place=3 (0 forgets)."},
    {"geom_get", (PyCFunction)kitnode_geom_get, METH_VARARGS, "geom_get(key) -> float"},
    {"grid_weight", (PyCFunction)kitnode_grid_weight, METH_VARARGS,
     "grid_weight(column_axis, index, weight, minsize=0)"},
    {"canvas_set", (PyCFunction)(void (*)(void))kitnode_canvas_set, METH_VARARGS | METH_KEYWORDS,
     "canvas_set(id, type, coords, fill=None, outline=None, ...): insert or replace an item."},
    {"canvas_delete", (PyCFunction)kitnode_canvas_delete, METH_O, "Delete item (id < 0: all)."},
    {"canvas_raise", (PyCFunction)kitnode_canvas_raise, METH_VARARGS, "Raise/lower an item."},
    {"set_image", (PyCFunction)kitnode_set_image, METH_VARARGS, "set_image(w, h, rgba_bytes)"},
    {"set_image_source", (PyCFunction)kitnode_set_image_source, METH_VARARGS,
     "set_image_source(path, fit=1): an image file decoded off the UI thread (fit: BK_FIT_*)"},
    {"set_image_frame", (PyCFunction)kitnode_set_image_frame, METH_O,
     "set_image_frame(n): animations play (0) or hold frame n - 1"},
    {"ta_insert", (PyCFunction)kitnode_ta_insert, METH_VARARGS, "Insert text at a byte offset."},
    {"ta_delete", (PyCFunction)kitnode_ta_delete, METH_VARARGS, "Delete [a, b)."},
    {"ta_caret", (PyCFunction)kitnode_ta_caret, METH_VARARGS, "Get/set the caret offset."},
    {"ta_selection", (PyCFunction)kitnode_ta_selection, METH_VARARGS, "Get/set the selection."},
    {"ta_spans", (PyCFunction)kitnode_ta_spans, METH_O,
     "Styled spans [(a, b, fg_rgba_or_None, bg_rgba_or_None)]."},
    {"ta_index_at", (PyCFunction)kitnode_ta_index_at, METH_VARARGS,
     "ta_index_at(x, y): byte offset under a window point."},
    {"ta_see", (PyCFunction)kitnode_ta_see, METH_O, "Scroll an offset into view."},
    {"set", (PyCFunction)kitnode_set, METH_VARARGS, "set(key, value): set a property."},
    {"get", (PyCFunction)kitnode_get, METH_VARARGS, "get(key) -> float"},
    {"get_str", (PyCFunction)kitnode_get_str, METH_VARARGS, "get_str(key) -> str"},
    {"get_color", (PyCFunction)kitnode_get_color, METH_VARARGS, "get_color(key='color')"},
    {"strs", (PyCFunction)kitnode_strs, METH_VARARGS, "strs(key='items') -> list[str]"},
    {"add", (PyCFunction)kitnode_add, METH_VARARGS, "add(child, index=-1) -> child"},
    {"remove", (PyCFunction)kitnode_remove, METH_O, "remove(child)"},
    {"natural", (PyCFunction)kitnode_natural, METH_NOARGS, "natural() -> (w, h)"},
    {"invalidate", (PyCFunction)kitnode_invalidate, METH_NOARGS, "Redraw this node."},
    {"relayout", (PyCFunction)kitnode_relayout, METH_NOARGS, "Re-layout the window."},
    {"_on_event", (PyCFunction)kitnode_on_event, METH_VARARGS, "Callback hook."},
    {"ramp", (PyCFunction)kitnode_ramp, METH_NOARGS, "ramp() -> [(pos, rgba)]"},
    {"set_ramp", (PyCFunction)kitnode_set_ramp, METH_O, "set_ramp([(pos, rgba)])"},
    {"ramp_eval", (PyCFunction)kitnode_ramp_eval, METH_O, "ramp_eval(t) -> rgba"},
    {"set_ramp_color", (PyCFunction)kitnode_set_ramp_color, METH_O, "Active stop color."},
    {"curve", (PyCFunction)kitnode_curve, METH_VARARGS, "curve(channel=0) -> [(x, y)]"},
    {"set_curve", (PyCFunction)kitnode_set_curve, METH_VARARGS, "set_curve(channel, pts)"},
    {"curve_eval", (PyCFunction)kitnode_curve_eval, METH_VARARGS, "curve_eval(channel, x)"},
    {"set_histogram", (PyCFunction)kitnode_set_histogram, METH_VARARGS, "set_histogram(bins, ch)"},
    {"set_grid_colors", (PyCFunction)kitnode_set_grid_colors, METH_O, "Preview colors."},
    {"set_menu", (PyCFunction)kitnode_set_menu, METH_O, "Menu button items."},
    {"set_tree_roots", (PyCFunction)kitnode_set_tree_roots, METH_O, "Tree roots."},
    {"tree_selected", (PyCFunction)kitnode_tree_selected, METH_NOARGS, "Selected item."},
    {"tree_select", (PyCFunction)kitnode_tree_select, METH_O, "Select an item."},
    {"tree_rows", (PyCFunction)kitnode_tree_rows, METH_NOARGS, "[(item, rect)] visible rows."},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef kitnode_getset[] = {
    {"children", (getter)kitnode_children, NULL, "child nodes", NULL},
    {"parent", (getter)kitnode_parent, NULL, "parent node", NULL},
    {"rect", (getter)kitnode_rect, NULL, "(x, y, w, h) from the last layout", NULL},
    {"type", (getter)kitnode_type, NULL, "node type name", NULL},
    {"x", (getter)kitnode_coord, NULL, "x from the last layout", (void *)0},
    {"y", (getter)kitnode_coord, NULL, "y from the last layout", (void *)1},
    {"w", (getter)kitnode_coord, NULL, "width from the last layout", (void *)2},
    {"h", (getter)kitnode_coord, NULL, "height from the last layout", (void *)3},
    {"__dict__", PyObject_GenericGetDict, PyObject_GenericSetDict, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyTypeObject KitNode_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "buipy.KitNode",
    .tp_basicsize = sizeof(KitNodeObject),
    .tp_dealloc = (destructor)kitnode_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "KitNode(type): a node of the C UI kit (see BUI_kit.h).",
    .tp_traverse = (traverseproc)kitnode_traverse,
    .tp_clear = (inquiry)kitnode_clear,
    .tp_methods = kitnode_methods,
    .tp_getset = kitnode_getset,
    .tp_dictoffset = offsetof(KitNodeObject, dict),
    .tp_init = (initproc)kitnode_init,
    .tp_new = PyType_GenericNew,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name KitTreeItem
 * \{ */

static int treeitem_init(KitTreeItemObject *self, PyObject *args, PyObject *kw)
{
  const char *label = "", *icon = "";
  static char *kwlist[] = {"label", "icon", NULL};
  if (!PyArg_ParseTupleAndKeywords(args, kw, "|sz:KitTreeItem", kwlist, &label, &icon)) {
    return -1;
  }
  if (!self->item) {
    self->item = BK_tree_item_new(label, BK_icon_id(icon ? icon : ""));
    BK_tree_item_set_ref(self->item, self);
  }
  return 0;
}

static void treeitem_dealloc(KitTreeItemObject *self)
{
  PyObject_GC_UnTrack(self);
  Py_CLEAR(self->dict);
  if (self->item) {
    BK_tree_item_set_ref(self->item, NULL);
    BK_tree_item_free(self->item);
  }
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static int treeitem_traverse(KitTreeItemObject *self, visitproc visit, void *arg)
{
  Py_VISIT(self->dict);
  return 0;
}

static int treeitem_clear(KitTreeItemObject *self)
{
  Py_CLEAR(self->dict);
  return 0;
}

static PyObject *treeitem_add(KitTreeItemObject *self, PyObject *child)
{
  if (!PyObject_TypeCheck(child, &KitTreeItem_Type)) {
    PyErr_SetString(PyExc_TypeError, "expected KitTreeItem");
    return NULL;
  }
  BK_tree_item_add(self->item, ((KitTreeItemObject *)child)->item);
  Py_INCREF(child);
  return child;
}

static PyObject *treeitem_clear_children(KitTreeItemObject *self, PyObject *unused)
{
  BK_tree_item_clear(self->item);
  Py_RETURN_NONE;
}

static PyObject *treeitem_set(KitTreeItemObject *self, PyObject *args)
{
  const char *key;
  double v;
  if (!PyArg_ParseTuple(args, "sd:set", &key, &v)) {
    return NULL;
  }
  BK_tree_item_set(self->item, key, v);
  Py_RETURN_NONE;
}

static PyObject *treeitem_get(KitTreeItemObject *self, PyObject *args)
{
  const char *key;
  if (!PyArg_ParseTuple(args, "s:get", &key)) {
    return NULL;
  }
  return PyFloat_FromDouble(BK_tree_item_get(self->item, key));
}

static PyObject *treeitem_get_label(KitTreeItemObject *self, void *c)
{
  return PyUnicode_FromString(BK_tree_item_label(self->item));
}

static int treeitem_set_label(KitTreeItemObject *self, PyObject *v, void *c)
{
  const char *s = v ? PyUnicode_AsUTF8(v) : NULL;
  if (!s) {
    return -1;
  }
  BK_tree_item_set_label(self->item, s);
  return 0;
}

static PyObject *treeitem_get_children(KitTreeItemObject *self, void *c)
{
  const int n = BK_tree_item_child_count(self->item);
  PyObject *list = PyList_New(n);
  for (int i = 0; list && i < n; i++) {
    PyList_SET_ITEM(list, i, item_wrap(BK_tree_item_child(self->item, i)));
  }
  return list;
}

static PyObject *treeitem_set_values(KitTreeItemObject *self, PyObject *values)
{
  PyObject *seq = PySequence_Fast(values, "values must be a sequence of str");
  if (!seq) {
    return NULL;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  const char **v = (const char **)calloc((size_t)(n + 1), sizeof(char *));
  PyObject **tmp = (PyObject **)calloc((size_t)(n + 1), sizeof(PyObject *));
  for (Py_ssize_t i = 0; i < n; i++) {
    tmp[i] = PyObject_Str(PySequence_Fast_GET_ITEM(seq, i));
    v[i] = tmp[i] ? PyUnicode_AsUTF8(tmp[i]) : "";
  }
  if (!PyErr_Occurred()) {
    BK_tree_item_set_values(self->item, v, (int)n);
  }
  for (Py_ssize_t i = 0; i < n; i++) {
    Py_XDECREF(tmp[i]);
  }
  free(tmp);
  free(v);
  Py_DECREF(seq);
  if (PyErr_Occurred()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyMethodDef treeitem_methods[] = {
    {"set_values", (PyCFunction)treeitem_set_values, METH_O, "Extra column values."},
    {"add", (PyCFunction)treeitem_add, METH_O, "add(child) -> child"},
    {"clear_children", (PyCFunction)treeitem_clear_children, METH_NOARGS,
     "Detach all child items."},
    {"set", (PyCFunction)treeitem_set, METH_VARARGS, "set('expanded'|'toggleN', value)"},
    {"get", (PyCFunction)treeitem_get, METH_VARARGS, "get(key) -> float"},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef treeitem_getset[] = {
    {"label", (getter)treeitem_get_label, (setter)treeitem_set_label, "label", NULL},
    {"children", (getter)treeitem_get_children, NULL, "child items", NULL},
    {"__dict__", PyObject_GenericGetDict, PyObject_GenericSetDict, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyTypeObject KitTreeItem_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "buipy.KitTreeItem",
    .tp_basicsize = sizeof(KitTreeItemObject),
    .tp_dealloc = (destructor)treeitem_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "KitTreeItem(label, icon=None): a tree view item.",
    .tp_traverse = (traverseproc)treeitem_traverse,
    .tp_clear = (inquiry)treeitem_clear,
    .tp_methods = treeitem_methods,
    .tp_getset = treeitem_getset,
    .tp_dictoffset = offsetof(KitTreeItemObject, dict),
    .tp_init = (initproc)treeitem_init,
    .tp_new = PyType_GenericNew,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name KitContext
 * \{ */

typedef struct KitContextObject {
  PyObject_HEAD
  BKContext *ctx;
  PyObject *root, *header;
  PyObject *on_request, *on_cursor, *on_warp, *on_clip_get, *on_clip_set, *on_schedule;
} KitContextObject;

static void call_quiet(PyObject *fn, PyObject *args)
{
  if (!fn || fn == Py_None) {
    return;
  }
  PyObject *r = PyObject_CallObject(fn, args);
  if (!r) {
    buipy_stash_error();
  }
  Py_XDECREF(r);
}

/* Python callbacks do not nest. The kit calls Python from the middle of its own work (a key press walks
 * the node tree and emits an event from inside that walk), and a handler may run a nested message
 * loop with the GIL released: os.startfile (ShellExecute), a file dialog, MessageBox ... The window
 * messages that arrive meanwhile re-enter the kit, and their callbacks would run Python while the
 * outer call still holds node pointers it is about to use again. So while a Python callback is
 * running, the ones that arrive are queued, and run in order as soon as the outer one returns. */
static int g_py_depth;
static PyObject *g_deferred; /* list of (callable, args tuple | None) */

static void defer_call(PyObject *fn, PyObject *args)
{
  if (!fn || fn == Py_None) {
    return;
  }
  if (!g_deferred && !(g_deferred = PyList_New(0))) {
    PyErr_Clear();
    return;
  }
  PyObject *item = PyTuple_Pack(2, fn, args ? args : Py_None);
  if (!item || PyList_Append(g_deferred, item) < 0) {
    PyErr_Clear();
  }
  Py_XDECREF(item);
}

static void run_deferred(void)
{
  g_py_depth++;
  while (g_deferred && PyList_GET_SIZE(g_deferred) > 0) {
    PyObject *item = Py_NewRef(PyList_GET_ITEM(g_deferred, 0));
    PyList_SetSlice(g_deferred, 0, 1, NULL);
    PyObject *args = PyTuple_GET_ITEM(item, 1);
    call_quiet(PyTuple_GET_ITEM(item, 0), args == Py_None ? NULL : args);
    Py_DECREF(item);
  }
  g_py_depth--;
}

static void host_request(void *data)
{
  call_quiet(((KitContextObject *)data)->on_request, NULL);
}

static void host_set_cursor(void *data, int cursor)
{
  PyObject *a = Py_BuildValue("(i)", cursor);
  call_quiet(((KitContextObject *)data)->on_cursor, a);
  Py_XDECREF(a);
}

static void host_warp(void *data, int x, int y)
{
  PyObject *a = Py_BuildValue("(ii)", x, y);
  call_quiet(((KitContextObject *)data)->on_warp, a);
  Py_XDECREF(a);
}

static char *host_clip_get(void *data)
{
  KitContextObject *self = (KitContextObject *)data;
  if (!self->on_clip_get || self->on_clip_get == Py_None) {
    return NULL;
  }
  PyObject *r = PyObject_CallObject(self->on_clip_get, NULL);
  if (!r) {
    PyErr_Clear(); /* empty / non-text clipboard */
    return NULL;
  }
  char *out = NULL;
  const char *s = PyUnicode_Check(r) ? PyUnicode_AsUTF8(r) : NULL;
  if (s) {
    out = (char *)malloc(strlen(s) + 1);
    strcpy(out, s);
  }
  PyErr_Clear();
  Py_DECREF(r);
  return out;
}

static void host_clip_set(void *data, const char *text)
{
  PyObject *a = Py_BuildValue("(s)", text);
  call_quiet(((KitContextObject *)data)->on_clip_set, a);
  Py_XDECREF(a);
}

static void host_schedule(void *data, int ms, int timer)
{
  PyObject *a = Py_BuildValue("(ii)", ms, timer);
  call_quiet(((KitContextObject *)data)->on_schedule, a);
  Py_XDECREF(a);
}

static void host_emit(void *data, BKNode *node, int kind, void *ref, double a, double b)
{
  if (kind == BK_EV_CALL && ref) {
    call_quiet((PyObject *)ref, NULL);
    return;
  }
  PyObject *target = node ? (PyObject *)BK_node_ref(node) : NULL;
  if (!target) {
    return;
  }
  PyObject *r;
  if ((kind >= BK_EV_PRESS && kind <= BK_EV_WHEEL) || kind == BK_EV_KEYRELEASE) {
    /* raw events: ref is a C string */
    r = PyObject_CallMethod(target, "_on_event", "isdd", kind, ref ? (const char *)ref : "", a,
                            b);
  }
  else {
    r = PyObject_CallMethod(target, "_on_event", "iOdd", kind, ref ? (PyObject *)ref : Py_None,
                            a, b);
  }
  if (!r) {
    buipy_stash_error();
  }
  Py_XDECREF(r);
}

static void host_request_gil(void *data)
{
  GIL_ENTER;
  host_request(data);
  GIL_LEAVE;
}

static void host_set_cursor_gil(void *data, int cursor)
{
  GIL_ENTER;
  host_set_cursor(data, cursor);
  GIL_LEAVE;
}

static void host_warp_gil(void *data, int x, int y)
{
  GIL_ENTER;
  host_warp(data, x, y);
  GIL_LEAVE;
}

static char *host_clip_get_gil(void *data)
{
  GIL_ENTER;
  char *out = host_clip_get(data);
  GIL_LEAVE;
  return out;
}

static void host_clip_set_gil(void *data, const char *text)
{
  GIL_ENTER;
  host_clip_set(data, text);
  GIL_LEAVE;
}

static void host_schedule_gil(void *data, int ms, int timer)
{
  GIL_ENTER;
  host_schedule(data, ms, timer);
  GIL_LEAVE;
}

/* host_emit's call, kept for later: what it would call, with what. */
static void defer_emit(BKNode *node, int kind, void *ref, double a, double b)
{
  if (kind == BK_EV_CALL && ref) {
    defer_call((PyObject *)ref, NULL);
    return;
  }
  PyObject *target = node ? (PyObject *)BK_node_ref(node) : NULL;
  PyObject *fn = target ? PyObject_GetAttrString(target, "_on_event") : NULL;
  if (!fn) {
    PyErr_Clear();
    return;
  }
  PyObject *payload;
  if ((kind >= BK_EV_PRESS && kind <= BK_EV_WHEEL) || kind == BK_EV_KEYRELEASE) {
    payload = PyUnicode_FromString(ref ? (const char *)ref : ""); /* raw events: a C string */
  }
  else {
    payload = Py_NewRef(ref ? (PyObject *)ref : Py_None);
  }
  PyObject *args = payload ? Py_BuildValue("(iOdd)", kind, payload, a, b) : NULL;
  defer_call(fn, args);
  Py_XDECREF(args);
  Py_XDECREF(payload);
  Py_DECREF(fn);
  PyErr_Clear();
}

static void host_emit_gil(void *data, BKNode *node, int kind, void *ref, double a, double b)
{
  GIL_ENTER;
  if (g_py_depth > 0) {
    defer_emit(node, kind, ref, a, b);
  }
  else {
    g_py_depth++;
    host_emit(data, node, kind, ref, a, b);
    g_py_depth--;
    run_deferred();
  }
  GIL_LEAVE;
}

static int kitctx_init(KitContextObject *self, PyObject *args, PyObject *kw)
{
  PyObject *root;
  if (!PyArg_ParseTuple(args, "O:KitContext", &root)) {
    return -1;
  }
  BKNode *r = node_of(root);
  if (!r) {
    return -1;
  }
  if (self->ctx) {
    BK_context_free(self->ctx);
  }
  self->ctx = BK_context_new(r);
  Py_XSETREF(self->root, Py_NewRef(root));
  BKHost host = {self,               host_request_gil,  host_set_cursor_gil, host_warp_gil,
                 host_clip_get_gil,  host_clip_set_gil, host_schedule_gil,   host_emit_gil};
  BK_context_set_host(self->ctx, &host);
  return 0;
}

static void kitctx_dealloc(KitContextObject *self)
{
  PyObject_GC_UnTrack(self);
  if (self->ctx) {
    BK_context_free(self->ctx);
    self->ctx = NULL;
  }
  Py_CLEAR(self->root);
  Py_CLEAR(self->header);
  Py_CLEAR(self->on_request);
  Py_CLEAR(self->on_cursor);
  Py_CLEAR(self->on_warp);
  Py_CLEAR(self->on_clip_get);
  Py_CLEAR(self->on_clip_set);
  Py_CLEAR(self->on_schedule);
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static int kitctx_traverse(KitContextObject *self, visitproc visit, void *arg)
{
  Py_VISIT(self->root);
  Py_VISIT(self->header);
  Py_VISIT(self->on_request);
  Py_VISIT(self->on_cursor);
  Py_VISIT(self->on_warp);
  Py_VISIT(self->on_clip_get);
  Py_VISIT(self->on_clip_set);
  Py_VISIT(self->on_schedule);
  return 0;
}

static int kitctx_clear(KitContextObject *self)
{
  Py_CLEAR(self->on_request);
  Py_CLEAR(self->on_cursor);
  Py_CLEAR(self->on_warp);
  Py_CLEAR(self->on_clip_get);
  Py_CLEAR(self->on_clip_set);
  Py_CLEAR(self->on_schedule);
  return 0;
}

static PyObject *kitctx_set_host(KitContextObject *self, PyObject *args, PyObject *kw)
{
  static char *kwlist[] = {"request", "cursor", "warp", "clipboard_get", "clipboard_set",
                           "schedule", NULL};
  PyObject *req = NULL, *cur = NULL, *warp = NULL, *cg = NULL, *cs = NULL, *sch = NULL;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "|OOOOOO:set_host", kwlist, &req, &cur, &warp, &cg,
                                   &cs, &sch))
  {
    return NULL;
  }
#define SETCB(field, v) \
  if (v) { \
    Py_XSETREF(self->field, Py_NewRef(v)); \
  }
  SETCB(on_request, req);
  SETCB(on_cursor, cur);
  SETCB(on_warp, warp);
  SETCB(on_clip_get, cg);
  SETCB(on_clip_set, cs);
  SETCB(on_schedule, sch);
#undef SETCB
  Py_RETURN_NONE;
}

static PyObject *kitctx_set_header(KitContextObject *self, PyObject *header)
{
  if (header == Py_None) {
    BK_context_set_header(self->ctx, NULL);
    Py_CLEAR(self->header);
    Py_RETURN_NONE;
  }
  BKNode *h = node_of(header);
  if (!h) {
    return NULL;
  }
  BK_context_set_header(self->ctx, h);
  Py_XSETREF(self->header, Py_NewRef(header));
  Py_RETURN_NONE;
}

static PyObject *kitctx_render(KitContextObject *self, PyObject *args, PyObject *kw)
{
  static char *kwlist[] = {"width", "height", "raster", "full", NULL};
  int w, h, full = 0;
  PyObject *raster_obj = Py_None;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "ii|Op:render", kwlist, &w, &h, &raster_obj, &full)) {
    return NULL;
  }
  BuiRaster *r = raster_obj == Py_None ? buipy_current_raster() : buipy_raster_of(raster_obj);
  if (!r) {
    PyErr_SetString(PyExc_RuntimeError, "render: no raster (pass one or set_backend(Raster))");
    return NULL;
  }
  const int regions = BK_render(self->ctx, r, w, h, full);
  if (buipy_restore_error()) {
    return NULL;
  }
  return PyLong_FromLong(regions);
}

#define INPUT3(name, call) \
  static PyObject *kitctx_##name(KitContextObject *self, PyObject *args) \
  { \
    int x, y, mods = 0; \
    if (!PyArg_ParseTuple(args, "ii|i:" #name, &x, &y, &mods)) { \
      return NULL; \
    } \
    const int r = call(self->ctx, x, y, mods); \
    if (buipy_restore_error()) { \
      return NULL; \
    } \
    return PyBool_FromLong(r); \
  }

INPUT3(press, BK_press)
INPUT3(release, BK_release)
INPUT3(motion, BK_motion)
INPUT3(context_menu, BK_context_menu)

static PyObject *kitctx_wheel(KitContextObject *self, PyObject *args)
{
  int x, y, delta, mods = 0;
  if (!PyArg_ParseTuple(args, "iii|i:wheel", &x, &y, &delta, &mods)) {
    return NULL;
  }
  const int r = BK_wheel(self->ctx, x, y, delta, mods);
  if (buipy_restore_error()) {
    return NULL;
  }
  return PyBool_FromLong(r);
}

static PyObject *kitctx_key(KitContextObject *self, PyObject *args)
{
  const char *text, *keysym;
  int mods = 0;
  if (!PyArg_ParseTuple(args, "zz|i:key", &text, &keysym, &mods)) {
    return NULL;
  }
  const int r = BK_key(self->ctx, text ? text : "", keysym ? keysym : "", mods);
  if (buipy_restore_error()) {
    return NULL;
  }
  return PyBool_FromLong(r);
}

static PyObject *kitctx_key_release(KitContextObject *self, PyObject *args)
{
  const char *keysym;
  int mods = 0;
  if (!PyArg_ParseTuple(args, "s|i:key_release", &keysym, &mods)) {
    return NULL;
  }
  const int r = BK_key_release(self->ctx, keysym, mods);
  if (buipy_restore_error()) {
    return NULL;
  }
  return PyBool_FromLong(r);
}

static PyObject *kitctx_button(KitContextObject *self, PyObject *args)
{
  int x, y, button, release, mods = 0;
  if (!PyArg_ParseTuple(args, "iiip|i:button", &x, &y, &button, &release, &mods)) {
    return NULL;
  }
  const int r = BK_button(self->ctx, x, y, button, release, mods);
  if (buipy_restore_error()) {
    return NULL;
  }
  return PyBool_FromLong(r);
}

static PyObject *kitctx_focus_node(KitContextObject *self, PyObject *arg)
{
  BKNode *n = NULL;
  if (arg != Py_None) {
    if (!PyObject_TypeCheck(arg, &KitNode_Type)) {
      PyErr_SetString(PyExc_TypeError, "focus_node(node or None)");
      return NULL;
    }
    n = ((KitNodeObject *)arg)->node;
  }
  BK_focus_node(self->ctx, n);
  if (buipy_restore_error()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kitctx_clipboard_get(KitContextObject *self, PyObject *unused)
{
  char *s = BK_clipboard_get(self->ctx);
  PyObject *r = PyUnicode_FromString(s ? s : "");
  free(s);
  return r;
}

static PyObject *kitctx_clipboard_set(KitContextObject *self, PyObject *arg)
{
  const char *s = PyUnicode_AsUTF8(arg);
  if (!s) {
    return NULL;
  }
  BK_clipboard_set(self->ctx, s);
  Py_RETURN_NONE;
}

static PyObject *kitctx_timer(KitContextObject *self, PyObject *arg)
{
  BK_timer(self->ctx, (int)PyLong_AsLong(arg));
  if (buipy_restore_error()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kitctx_bind(KitContextObject *self, PyObject *args)
{
  const char *combo;
  PyObject *fn;
  if (!PyArg_ParseTuple(args, "sO:bind", &combo, &fn)) {
    return NULL;
  }
  BK_bind(self->ctx, combo, fn);
  Py_RETURN_NONE;
}

static PyObject *kitctx_close_popups(KitContextObject *self, PyObject *unused)
{
  BK_close_popups(self->ctx);
  Py_RETURN_NONE;
}

static PyObject *kitctx_set_pointer(KitContextObject *self, PyObject *args)
{
  int x, y;
  if (!PyArg_ParseTuple(args, "ii:set_pointer", &x, &y)) {
    return NULL;
  }
  BK_set_pointer(self->ctx, x, y);
  Py_RETURN_NONE;
}

static PyObject *kitctx_needs_render(KitContextObject *self, PyObject *unused)
{
  return PyBool_FromLong(BK_needs_render(self->ctx));
}

static PyObject *kitctx_popup_count(KitContextObject *self, PyObject *unused)
{
  return PyLong_FromLong(BK_popup_count(self->ctx));
}

static PyObject *kitctx_popup(KitContextObject *self, PyObject *arg)
{
  const long i = PyLong_AsLong(arg);
  BKNode *p = BK_popup(self->ctx, (int)(i < 0 ? BK_popup_count(self->ctx) + i : i));
  return node_wrap(p);
}

static PyObject *kitctx_popup_owner(KitContextObject *self, PyObject *arg)
{
  const long i = PyLong_AsLong(arg);
  return node_wrap(BK_popup_owner(self->ctx, (int)(i < 0 ? BK_popup_count(self->ctx) + i : i)));
}

static PyObject *kitctx_menu_rows(KitContextObject *self, PyObject *arg)
{
  const long i = PyLong_AsLong(arg);
  BKNode *p = BK_popup(self->ctx, (int)(i < 0 ? BK_popup_count(self->ctx) + i : i));
  if (!p) {
    return PyList_New(0);
  }
  const int n = BK_menu_row_count(p);
  PyObject *list = PyList_New(n);
  for (int k = 0; list && k < n; k++) {
    int r[4];
    BK_menu_row_rect(p, k, r);
    PyList_SET_ITEM(list, k,
                    Py_BuildValue("(s(iiii))", BK_menu_row_text(p, k), r[0], r[1], r[2], r[3]));
  }
  return list;
}

static PyObject *kitctx_picker_part(KitContextObject *self, PyObject *args)
{
  const char *name;
  int i = -1;
  if (!PyArg_ParseTuple(args, "s|i:picker_part", &name, &i)) {
    return NULL;
  }
  BKNode *p = BK_popup(self->ctx, i < 0 ? BK_popup_count(self->ctx) + i : i);
  return node_wrap(p ? BK_picker_part(p, name) : NULL);
}

static PyObject *kitctx_hit(KitContextObject *self, PyObject *args)
{
  int x, y;
  if (!PyArg_ParseTuple(args, "ii:hit", &x, &y)) {
    return NULL;
  }
  return node_wrap(BK_hit_at(self->ctx, x, y));
}

static PyObject *kitctx_open_menu(KitContextObject *self, PyObject *args, PyObject *kw)
{
  static char *kwlist[] = {"owner", "items", "title", "at", NULL};
  PyObject *owner, *items;
  const char *title = NULL;
  PyObject *at = Py_None;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|zO:open_menu", kwlist, &owner, &items, &title,
                                   &at))
  {
    return NULL;
  }
  BKNode *o = owner == Py_None ? NULL : node_of(owner);
  if (owner != Py_None && !o) {
    return NULL;
  }
  int ax = -100000, ay = -100000;
  if (at != Py_None && !PyArg_ParseTuple(at, "ii", &ax, &ay)) {
    return NULL;
  }
  BKMenuSpec *specs;
  int n;
  if (!build_specs(items, &specs, &n)) {
    return NULL;
  }
  BK_open_menu(self->ctx, o, specs, n, title, ax, ay);
  free_specs(specs, n);
  Py_RETURN_NONE;
}

static PyObject *kitctx_open_pie(KitContextObject *self, PyObject *args, PyObject *kw)
{
  static char *kwlist[] = {"items", "title", "at", NULL};
  PyObject *items;
  const char *title = NULL;
  PyObject *at = Py_None;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "O|zO:open_pie", kwlist, &items, &title, &at)) {
    return NULL;
  }
  int ax = -100000, ay = -100000;
  if (at != Py_None && !PyArg_ParseTuple(at, "ii", &ax, &ay)) {
    return NULL;
  }
  BKMenuSpec *specs;
  int n;
  if (!build_specs(items, &specs, &n)) {
    return NULL;
  }
  BK_open_pie(self->ctx, specs, n, title, ax, ay);
  free_specs(specs, n);
  Py_RETURN_NONE;
}

static PyObject *kitctx_cursor(KitContextObject *self, void *c)
{
  return PyLong_FromLong(BK_cursor(self->ctx));
}
static PyObject *kitctx_hover(KitContextObject *self, void *c)
{
  return node_wrap(BK_hover(self->ctx));
}
static PyObject *kitctx_focus(KitContextObject *self, void *c)
{
  return node_wrap(BK_focus(self->ctx));
}
static PyObject *kitctx_modal(KitContextObject *self, void *c)
{
  return PyBool_FromLong(BK_modal_active(self->ctx));
}
static PyObject *kitctx_tooltip(KitContextObject *self, void *c)
{
  return PyBool_FromLong(BK_tooltip_visible(self->ctx));
}

static PyObject *kitctx_undo(KitContextObject *self, PyObject *unused)
{
  const int r = BK_undo(self->ctx);
  if (buipy_restore_error()) {
    return NULL;
  }
  return PyBool_FromLong(r);
}

static PyObject *kitctx_redo(KitContextObject *self, PyObject *unused)
{
  const int r = BK_redo(self->ctx);
  if (buipy_restore_error()) {
    return NULL;
  }
  return PyBool_FromLong(r);
}

static PyObject *kitctx_undo_depth(KitContextObject *self, PyObject *unused)
{
  int redo = 0;
  const int undo = BK_undo_depth(self->ctx, &redo);
  return Py_BuildValue("(ii)", undo, redo);
}

static PyObject *kitctx_animating(KitContextObject *self, PyObject *unused)
{
  return PyBool_FromLong(BK_animating(self->ctx));
}

static PyMethodDef kitctx_methods[] = {
    {"undo", (PyCFunction)kitctx_undo, METH_NOARGS, "Undo the last UI edit (Ctrl+Z)."},
    {"redo", (PyCFunction)kitctx_redo, METH_NOARGS, "Redo (Ctrl+Shift+Z)."},
    {"undo_depth", (PyCFunction)kitctx_undo_depth, METH_NOARGS, "(undo steps, redo steps)"},
    {"animating", (PyCFunction)kitctx_animating, METH_NOARGS, "Animations running."},
    {"set_host", (PyCFunction)(void (*)(void))kitctx_set_host, METH_VARARGS | METH_KEYWORDS,
     "set_host(request, cursor, warp, clipboard_get, clipboard_set, schedule)"},
    {"set_header", (PyCFunction)kitctx_set_header, METH_O, "Fixed top bar node."},
    {"render", (PyCFunction)(void (*)(void))kitctx_render, METH_VARARGS | METH_KEYWORDS,
     "render(width, height, raster=None, full=False) -> regions redrawn"},
    {"needs_render", (PyCFunction)kitctx_needs_render, METH_NOARGS, ""},
    {"press", (PyCFunction)kitctx_press, METH_VARARGS, "press(x, y, mods)"},
    {"release", (PyCFunction)kitctx_release, METH_VARARGS, "release(x, y, mods)"},
    {"motion", (PyCFunction)kitctx_motion, METH_VARARGS, "motion(x, y, mods)"},
    {"context_menu", (PyCFunction)kitctx_context_menu, METH_VARARGS, "Right click."},
    {"wheel", (PyCFunction)kitctx_wheel, METH_VARARGS, "wheel(x, y, delta, mods)"},
    {"key", (PyCFunction)kitctx_key, METH_VARARGS, "key(text, keysym, mods)"},
    {"focus_node", (PyCFunction)kitctx_focus_node, METH_O,
     "focus_node(node): start editing a text node (None ends editing)."},
    {"clipboard_get", (PyCFunction)kitctx_clipboard_get, METH_NOARGS, "Clipboard text."},
    {"clipboard_set", (PyCFunction)kitctx_clipboard_set, METH_O, "Set the clipboard text."},
    {"key_release", (PyCFunction)kitctx_key_release, METH_VARARGS,
     "key_release(keysym, mods): raw <KeyRelease> for bindings."},
    {"button", (PyCFunction)kitctx_button, METH_VARARGS,
     "button(x, y, button, release, mods): raw press/release of buttons 2/3."},
    {"timer", (PyCFunction)kitctx_timer, METH_O, "Fire a scheduled timer."},
    {"bind", (PyCFunction)kitctx_bind, METH_VARARGS, "bind(combo, fn)"},
    {"close_popups", (PyCFunction)kitctx_close_popups, METH_NOARGS, ""},
    {"set_pointer", (PyCFunction)kitctx_set_pointer, METH_VARARGS, ""},
    {"popup_count", (PyCFunction)kitctx_popup_count, METH_NOARGS, ""},
    {"popup", (PyCFunction)kitctx_popup, METH_O, "Popup node i (borrowed)."},
    {"popup_owner", (PyCFunction)kitctx_popup_owner, METH_O, ""},
    {"menu_rows", (PyCFunction)kitctx_menu_rows, METH_O, "[(text, rect)] of menu popup i"},
    {"picker_part", (PyCFunction)kitctx_picker_part, METH_VARARGS, "Color picker part."},
    {"hit", (PyCFunction)kitctx_hit, METH_VARARGS, "hit(x, y): the node under the point, or None"},
    {"open_menu", (PyCFunction)(void (*)(void))kitctx_open_menu, METH_VARARGS | METH_KEYWORDS,
     "open_menu(owner, items, title=None, at=None)"},
    {"open_pie", (PyCFunction)(void (*)(void))kitctx_open_pie, METH_VARARGS | METH_KEYWORDS,
     "open_pie(items, title=None, at=None)"},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef kitctx_getset[] = {
    {"cursor", (getter)kitctx_cursor, NULL, "current cursor id", NULL},
    {"hover", (getter)kitctx_hover, NULL, "hovered node", NULL},
    {"focus", (getter)kitctx_focus, NULL, "node with text focus", NULL},
    {"modal", (getter)kitctx_modal, NULL, "modal tool active (eyedropper)", NULL},
    {"tooltip", (getter)kitctx_tooltip, NULL, "tooltip shown", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyTypeObject KitContext_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "buipy.KitContext",
    .tp_basicsize = sizeof(KitContextObject),
    .tp_dealloc = (destructor)kitctx_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "KitContext(root): one window's input + rendering context.",
    .tp_traverse = (traverseproc)kitctx_traverse,
    .tp_clear = (inquiry)kitctx_clear,
    .tp_methods = kitctx_methods,
    .tp_getset = kitctx_getset,
    .tp_init = (initproc)kitctx_init,
    .tp_new = PyType_GenericNew,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Module functions
 * \{ */

static PyObject *kit_set_scale(PyObject *m, PyObject *arg)
{
  BK_set_scale((float)PyFloat_AsDouble(arg));
  Py_RETURN_NONE;
}

static PyObject *kit_set_text_shadow(PyObject *m, PyObject *arg)
{
  const int on = PyObject_IsTrue(arg);
  if (on < 0) {
    return NULL;
  }
  BK_set_text_shadow(on);
  Py_RETURN_NONE;
}

static PyObject *kit_scale(PyObject *m, PyObject *unused)
{
  return PyFloat_FromDouble(BK_scale());
}

static PyObject *kit_unit(PyObject *m, PyObject *unused)
{
  return PyLong_FromLong(BK_unit());
}

static PyObject *kit_set_theme(PyObject *m, PyObject *arg)
{
  const char *name = PyUnicode_AsUTF8(arg);
  if (!name) {
    return NULL;
  }
  if (!BK_set_theme(name)) {
    PyErr_Format(PyExc_ValueError, "unknown theme %R (expected 'dark', 'light' or a theme loaded with load_theme)", arg);
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kit_version(PyObject *m, PyObject *unused)
{
  return PyUnicode_FromString(BK_version_string());
}

static PyObject *kit_theme_name(PyObject *m, PyObject *unused)
{
  return PyUnicode_FromString(BK_theme_name());
}

static PyObject *kit_theme_color(PyObject *m, PyObject *arg)
{
  const char *key = PyUnicode_AsUTF8(arg);
  float c[4];
  if (!key) {
    return NULL;
  }
  if (!BK_theme_color(key, c)) {
    PyErr_Format(PyExc_KeyError, "%R", arg);
    return NULL;
  }
  return Py_BuildValue("(dddd)", (double)c[0], (double)c[1], (double)c[2], (double)c[3]);
}

/* Theme files (kit_theme.c): register the themes of a JSON text or file; ValueError says why not. */
static PyObject *theme_loaded(int n)
{
  if (n < 0) {
    PyErr_SetString(PyExc_ValueError, BK_theme_error());
    return NULL;
  }
  return Py_BuildValue("(ii)", n, BK_theme_ignored());
}

static PyObject *kit_theme_load_json(PyObject *m, PyObject *arg)
{
  const char *text = PyUnicode_AsUTF8(arg);
  return text ? theme_loaded(BK_theme_load_json(text)) : NULL;
}

static PyObject *kit_theme_load_file(PyObject *m, PyObject *arg)
{
  PyObject *path = PyOS_FSPath(arg);
  if (!path) {
    return NULL;
  }
  PyObject *enc = PyUnicode_EncodeFSDefault(path);
  Py_DECREF(path);
  if (!enc) {
    return NULL;
  }
  PyObject *r = theme_loaded(BK_theme_load_file(PyBytes_AS_STRING(enc)));
  Py_DECREF(enc);
  return r;
}

static PyObject *kit_theme_names(PyObject *m, PyObject *unused)
{
  PyObject *list = PyList_New(0);
  for (int i = 0; list && BK_theme_at(i); i++) {
    PyObject *s = PyUnicode_FromString(BK_theme_at(i));
    if (!s || PyList_Append(list, s) < 0) {
      Py_XDECREF(s);
      Py_CLEAR(list);
      break;
    }
    Py_DECREF(s);
  }
  return list;
}

static PyObject *kit_theme_set_color(PyObject *m, PyObject *args)
{
  const char *key;
  PyObject *rgba;
  if (!PyArg_ParseTuple(args, "sO:kit_theme_set_color", &key, &rgba)) {
    return NULL;
  }
  PyObject *seq = PySequence_Fast(rgba, "a color is (r, g, b) or (r, g, b, a)");
  if (!seq) {
    return NULL;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  float f[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  if (n != 3 && n != 4) {
    Py_DECREF(seq);
    PyErr_SetString(PyExc_ValueError, "a color is (r, g, b) or (r, g, b, a)");
    return NULL;
  }
  for (Py_ssize_t i = 0; i < n; i++) {
    f[i] = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, i));
  }
  Py_DECREF(seq);
  if (PyErr_Occurred()) {
    return NULL;
  }
  if (!BK_theme_set_color(key, f)) {
    PyErr_Format(PyExc_KeyError, "no theme color %s", key);
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kit_theme_set_num(PyObject *m, PyObject *args)
{
  const char *key;
  double v;
  if (!PyArg_ParseTuple(args, "sd:kit_theme_set_num", &key, &v)) {
    return NULL;
  }
  if (!BK_theme_set_num(key, v)) {
    PyErr_Format(PyExc_KeyError, "no theme number %s", key);
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kit_theme_num(PyObject *m, PyObject *arg)
{
  const char *key = PyUnicode_AsUTF8(arg);
  double v;
  if (!key) {
    return NULL;
  }
  if (!BK_theme_num(key, &v)) {
    PyErr_Format(PyExc_KeyError, "%R", arg);
    return NULL;
  }
  return PyFloat_FromDouble(v);
}

static PyObject *kit_set_font(PyObject *m, PyObject *arg)
{
  BK_set_font(PyUnicode_AsUTF8(arg));
  Py_RETURN_NONE;
}

static PyObject *kit_icon_names(PyObject *m, PyObject *unused)
{
  const int n = BK_icon_list_count();
  PyObject *list = PyList_New(n);
  for (int i = 0; list && i < n; i++) {
    PyList_SET_ITEM(list, i, PyUnicode_FromString(BK_icon_list_name(i)));
  }
  return list;
}

static PyObject *kit_eval_number(PyObject *m, PyObject *arg)
{
  const char *s = PyUnicode_AsUTF8(arg);
  double v;
  if (!s) {
    return NULL;
  }
  if (!BK_eval_number(s, &v)) {
    Py_RETURN_NONE;
  }
  return PyFloat_FromDouble(v);
}

/* -- images (kit/intern/image: the one decoder set every binding uses) ------------ */

static PyObject *kit_image_probe(PyObject *mod, PyObject *arg)
{
  (void)mod;
  Py_buffer buf;
  if (PyObject_GetBuffer(arg, &buf, PyBUF_SIMPLE) < 0) {
    return NULL;
  }
  BKImageInfo info;
  const int ok = BK_image_probe(buf.buf, (size_t)buf.len, &info);
  PyBuffer_Release(&buf);
  if (!ok) {
    Py_RETURN_NONE;
  }
  return Py_BuildValue("(siii)", BK_image_format_name(info.format), info.width, info.height,
                       info.frames);
}

static PyObject *kit_image_decode(PyObject *mod, PyObject *args, PyObject *kw)
{
  (void)mod;
  static char *kwlist[] = {"data", "max_w", "max_h", NULL};
  Py_buffer buf;
  int max_w = 0, max_h = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "y*|ii:image_decode", kwlist, &buf, &max_w, &max_h)) {
    return NULL;
  }
  BKImBuf *ib;
  Py_BEGIN_ALLOW_THREADS; /* decoding is thread-safe and may take a while */
  ib = BK_imbuf_decode(buf.buf, (size_t)buf.len, max_w, max_h);
  Py_END_ALLOW_THREADS;
  PyBuffer_Release(&buf);
  if (!ib) {
    PyErr_SetString(PyExc_ValueError, "not an image BUI decodes (or corrupt)");
    return NULL;
  }
  int w, h;
  const int frames = BK_imbuf_size(ib, &w, &h);
  PyObject *list = PyList_New(frames);
  for (int f = 0; list && f < frames; f++) {
    PyObject *px = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)w * h * 4);
    if (!px) {
      Py_CLEAR(list);
      break;
    }
    BK_imbuf_rgba(ib, f, (unsigned char *)PyBytes_AS_STRING(px));
    PyList_SET_ITEM(list, f, Py_BuildValue("(Ni)", px, BK_imbuf_delay(ib, f)));
  }
  const char *fmt = BK_image_format_name(BK_imbuf_format(ib));
  BK_imbuf_free(ib);
  return list ? Py_BuildValue("(siiN)", fmt, w, h, list) : NULL;
}

static PyObject *kit_image_decode_frame(PyObject *mod, PyObject *args, PyObject *kw)
{
  (void)mod;
  static char *kwlist[] = {"data", "index", "max_w", "max_h", NULL};
  Py_buffer buf;
  int index, max_w = 0, max_h = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "y*i|ii:image_decode_frame", kwlist, &buf, &index,
                                   &max_w, &max_h))
  {
    return NULL;
  }
  BKImBuf *ib;
  Py_BEGIN_ALLOW_THREADS;
  ib = BK_imbuf_decode_frame(buf.buf, (size_t)buf.len, index, max_w, max_h);
  Py_END_ALLOW_THREADS;
  PyBuffer_Release(&buf);
  if (!ib) {
    PyErr_SetString(PyExc_ValueError, "no such frame, or not an image BUI decodes");
    return NULL;
  }
  int w, h;
  BK_imbuf_size(ib, &w, &h);
  PyObject *px = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)w * h * 4);
  if (px) {
    BK_imbuf_rgba(ib, 0, (unsigned char *)PyBytes_AS_STRING(px));
  }
  const char *fmt = BK_image_format_name(BK_imbuf_format(ib));
  BK_imbuf_free(ib);
  return px ? Py_BuildValue("(siiN)", fmt, w, h, px) : NULL;
}

/* -- animations stepped from Python (Dear ImGui textures) ---------------------------- */

#define ANIM_CAPSULE "buipy.BKAnim"

static void anim_capsule_free(PyObject *capsule)
{
  BK_anim_free((BKAnim *)PyCapsule_GetPointer(capsule, ANIM_CAPSULE));
}

static PyObject *kit_anim_open(PyObject *mod, PyObject *args, PyObject *kw)
{
  (void)mod;
  static char *kwlist[] = {"data", "max_w", "max_h", NULL};
  Py_buffer buf;
  int max_w = 0, max_h = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "y*|ii:anim_open", kwlist, &buf, &max_w, &max_h)) {
    return NULL;
  }
  BKAnim *a = BK_anim_open(buf.buf, (size_t)buf.len, max_w, max_h);
  PyBuffer_Release(&buf);
  if (!a) {
    PyErr_SetString(PyExc_ValueError, "not an animation BUI decodes (GIF, animated WebP)");
    return NULL;
  }
  int w, h, loops;
  const int frames = BK_anim_info(a, &w, &h, &loops);
  PyObject *cap = PyCapsule_New(a, ANIM_CAPSULE, anim_capsule_free);
  if (!cap) {
    BK_anim_free(a);
    return NULL;
  }
  return Py_BuildValue("(Niiii)", cap, w, h, frames, loops);
}

static PyObject *kit_anim_next(PyObject *mod, PyObject *args)
{
  (void)mod;
  PyObject *cap;
  int tex = 0;
  if (!PyArg_ParseTuple(args, "O|i:anim_next", &cap, &tex)) {
    return NULL;
  }
  BKAnim *a = (BKAnim *)PyCapsule_GetPointer(cap, ANIM_CAPSULE);
  if (!a) {
    return NULL;
  }
  int delay = 0;
  BKImBuf *ib = BK_anim_next(a, &delay);
  if (!ib) {
    PyErr_SetString(PyExc_ValueError, "the animation could not be decoded further");
    return NULL;
  }
  int w, h;
  BK_imbuf_size(ib, &w, &h);
  PyObject *out;
  if (tex >= 0) { /* into a texture: new (0) or updated in place */
    const int t = tex ? (BK_imbuf_texture_update(ib, 0, tex) ? tex : 0) : BK_imbuf_texture(ib, 0);
    out = Py_BuildValue("(ii)", t, delay);
  }
  else { /* the straight RGBA bytes */
    PyObject *px = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)w * h * 4);
    if (px) {
      BK_imbuf_rgba(ib, 0, (unsigned char *)PyBytes_AS_STRING(px));
    }
    out = px ? Py_BuildValue("(Nii)", px, w, h) : NULL;
    if (out) {
      PyObject *t = Py_BuildValue("(Oi)", out, delay);
      Py_DECREF(out);
      out = t;
    }
  }
  BK_imbuf_free(ib);
  return out;
}

static PyObject *kit_image_stats(PyObject *mod, PyObject *unused)
{
  (void)mod;
  (void)unused;
  BKImageStats st;
  BK_image_stats(&st);
  return Py_BuildValue("{s:n,s:n,s:i,s:i,s:i,s:i,s:i,s:i,s:d}", "bytes", (Py_ssize_t)st.bytes,
                       "budget", (Py_ssize_t)st.budget, "entries", st.entries, "pending",
                       st.pending, "decoded", st.decoded, "failed", st.failed, "cancelled",
                       st.cancelled, "evicted", st.evicted, "decode_ms", st.decode_ms);
}

static PyObject *kit_image_poll(PyObject *mod, PyObject *unused)
{
  (void)mod;
  (void)unused;
  return PyLong_FromLong(BK_image_poll());
}

static PyObject *kit_image_set_budget(PyObject *mod, PyObject *arg)
{
  (void)mod;
  const Py_ssize_t n = PyNumber_AsSsize_t(arg, PyExc_OverflowError);
  if (n < 0) {
    return PyErr_Occurred() ? NULL : (PyErr_SetString(PyExc_ValueError, "budget >= 0"), NULL);
  }
  BK_image_set_budget((size_t)n);
  Py_RETURN_NONE;
}

static PyObject *kit_image_texture(PyObject *mod, PyObject *args, PyObject *kw)
{
  (void)mod;
  static char *kwlist[] = {"data", "max_w", "max_h", NULL};
  Py_buffer buf;
  int max_w = 0, max_h = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "y*|ii:image_texture", kwlist, &buf, &max_w, &max_h)) {
    return NULL;
  }
  BKImBuf *ib = BK_imbuf_decode(buf.buf, (size_t)buf.len, max_w, max_h);
  PyBuffer_Release(&buf);
  const int tex = ib ? BK_imbuf_texture(ib, 0) : 0;
  int w = 0, h = 0;
  BK_imbuf_size(ib, &w, &h);
  BK_imbuf_free(ib);
  if (!tex) {
    PyErr_SetString(PyExc_ValueError, "not an image BUI decodes (or corrupt)");
    return NULL;
  }
  return Py_BuildValue("(iii)", tex, w, h);
}

static PyObject *kit_texture_free(PyObject *mod, PyObject *arg)
{
  (void)mod;
  const long tex = PyLong_AsLong(arg);
  if (tex == -1 && PyErr_Occurred()) {
    return NULL;
  }
  BUI_texture_free((int)tex);
  Py_RETURN_NONE;
}

static PyMethodDef kit_functions[] = {
    {"image_texture", (PyCFunction)(void (*)(void))kit_image_texture, METH_VARARGS | METH_KEYWORDS,
     "image_texture(data, max_w=0, max_h=0) -> (texture_id, w, h): a decoded image as a BUI\n"
     "texture (a Dear ImGui ImTextureID). Free it with texture_free."},
    {"texture_free", kit_texture_free, METH_O, "Free a BUI texture (image_texture)."},
    {"image_probe", kit_image_probe, METH_O,
     "image_probe(data) -> (format, width, height, frames) or None: the header only."},
    {"image_decode", (PyCFunction)(void (*)(void))kit_image_decode, METH_VARARGS | METH_KEYWORDS,
     "image_decode(data, max_w=0, max_h=0) -> (format, w, h, [(rgba, delay_ms), ...]):\n"
     "PNG, JPEG, WebP, GIF, BMP, TGA, QOI, HDR, PNM, SVG; straight RGBA frames."},
    {"image_decode_frame", (PyCFunction)(void (*)(void))kit_image_decode_frame,
     METH_VARARGS | METH_KEYWORDS,
     "image_decode_frame(data, index, max_w=0, max_h=0) -> (format, w, h, rgba): one frame."},
    {"anim_open", (PyCFunction)(void (*)(void))kit_anim_open, METH_VARARGS | METH_KEYWORDS,
     "anim_open(data, max_w=0, max_h=0) -> (anim, w, h, frames, loops): frame by frame."},
    {"anim_next", kit_anim_next, METH_VARARGS,
     "anim_next(anim, tex=-1) -> ((rgba, w, h), delay_ms), or with tex >= 0 (texture, delay_ms):\n"
     "the next frame into a new (0) or that BUI texture."},
    {"image_stats", kit_image_stats, METH_NOARGS, "The shared image cache: bytes, pending, ..."},
    {"image_poll", kit_image_poll, METH_NOARGS,
     "Hand finished decodes to the cache (hosts without the kit's window loop)."},
    {"image_set_budget", kit_image_set_budget, METH_O, "Image cache budget in bytes."},
    {"kit_set_scale", kit_set_scale, METH_O, "UI scale (1.0 = 20 px widgets)."},
    {"kit_scale", kit_scale, METH_NOARGS, ""},
    {"kit_set_text_shadow", kit_set_text_shadow, METH_O,
     "Blender's UI text shadows (off in Python by default)."},
    {"kit_unit", kit_unit, METH_NOARGS, "U.widget_unit in pixels."},
    {"kit_set_font", kit_set_font, METH_O, "UI font family."},
    {"kit_set_theme", kit_set_theme, METH_O, "Theme: 'dark' (default) or 'light' (Blender's presets)."},
    {"kit_version", kit_version, METH_NOARGS, "The kit's version, 'MAJOR.MINOR.PATCH' (the same as BUI's)."},
    {"kit_theme_name", kit_theme_name, METH_NOARGS, "The active theme: 'dark' or 'light'."},
    {"kit_theme_color", kit_theme_color, METH_O, "A theme color as (r, g, b, a) 0..1: 'button.background', 'user_interface.wcol_regular.inner', 'region_bg'."},
    {"kit_theme_load_json", kit_theme_load_json, METH_O, "Register the themes of a theme file's text: (added, ignored keys)."},
    {"kit_theme_load_file", kit_theme_load_file, METH_O, "Register the themes of a theme file: (added, ignored keys)."},
    {"kit_theme_names", kit_theme_names, METH_NOARGS, "The registered (custom) themes."},
    {"kit_theme_set_color", kit_theme_set_color, METH_VARARGS, "kit_theme_set_color(key, (r, g, b[, a])): one color of the active theme."},
    {"kit_theme_set_num", kit_theme_set_num, METH_VARARGS, "kit_theme_set_num(key, value): a number of the active theme (roundness, border.width, ...)."},
    {"kit_theme_num", kit_theme_num, METH_O, "A number of the active theme."},
    {"kit_icon_names", kit_icon_names, METH_NOARGS, "All icon names."},
    {"kit_eval_number", kit_eval_number, METH_O, "Evaluate number input ('2*pi')."},
    {"kit_link_fields", kit_link_fields, METH_O, "Link number fields for Alt editing."},
    {NULL, NULL, 0, NULL},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name KitWindow (native host)
 * \{ */

typedef struct KitWindowObject {
  PyObject_HEAD
  BKWindow *win;
  PyObject *ctx;      /* KitContext (kept alive) */
  PyObject *timers;   /* id -> callable */
  PyObject *on_close; /* callable -> bool, or None */
  PyObject *on_focus; /* callable(active: bool), or None */
} KitWindowObject;

static int stop_check(void)
{
  return g_err_type != NULL; /* a Python callback raised: leave the loop */
}

static void win_on_timer(void *data, BKWindow *w, int id)
{
  KitWindowObject *self = (KitWindowObject *)data;
  PyObject *key = PyLong_FromLong(id);
  PyObject *fn = key ? PyDict_GetItemWithError(self->timers, key) : NULL;
  if (fn) {
    Py_INCREF(fn);
    PyDict_DelItem(self->timers, key);
    call_quiet(fn, NULL);
    Py_DECREF(fn);
  }
  Py_XDECREF(key);
  if (PyErr_Occurred()) {
    buipy_stash_error();
  }
}

static int win_on_close(void *data, BKWindow *w)
{
  KitWindowObject *self = (KitWindowObject *)data;
  if (!self->on_close || self->on_close == Py_None) {
    return 1;
  }
  PyObject *r = PyObject_CallObject(self->on_close, NULL);
  if (!r) {
    buipy_stash_error();
    return 1;
  }
  const int allow = (r == Py_None) ? 0 : PyObject_IsTrue(r); /* Tk protocol: handler decides */
  Py_DECREF(r);
  return allow;
}

static void win_on_focus(void *data, BKWindow *w, int active)
{
  KitWindowObject *self = (KitWindowObject *)data;
  if (self->on_focus && self->on_focus != Py_None) {
    PyObject *r = PyObject_CallOneArg(self->on_focus, active ? Py_True : Py_False);
    if (!r) {
      buipy_stash_error();
    }
    Py_XDECREF(r);
  }
}

static void win_on_focus_gil(void *data, BKWindow *w, int active)
{
  KitWindowObject *self = (KitWindowObject *)data;
  GIL_ENTER;
  if (g_py_depth > 0) { /* queued behind the running callback, like the timers */
    PyObject *args = PyTuple_Pack(1, active ? Py_True : Py_False);
    if (args) {
      defer_call(self->on_focus, args);
    }
    Py_XDECREF(args);
    PyErr_Clear();
  }
  else {
    g_py_depth++;
    win_on_focus(data, w, active);
    g_py_depth--;
    run_deferred();
  }
  GIL_LEAVE;
}

/* The timer's callable, taken off the table (one-shot) and queued behind the running callback. */
static void win_defer_timer(void *data, int id)
{
  KitWindowObject *self = (KitWindowObject *)data;
  PyObject *key = PyLong_FromLong(id);
  PyObject *fn = key ? PyDict_GetItemWithError(self->timers, key) : NULL;
  if (fn) {
    Py_INCREF(fn);
    PyDict_DelItem(self->timers, key);
    defer_call(fn, NULL);
    Py_DECREF(fn);
  }
  Py_XDECREF(key);
  PyErr_Clear();
}

static void win_on_timer_gil(void *data, BKWindow *w, int id)
{
  GIL_ENTER;
  if (g_py_depth > 0) {
    win_defer_timer(data, id);
  }
  else {
    g_py_depth++;
    win_on_timer(data, w, id);
    g_py_depth--;
    run_deferred();
  }
  GIL_LEAVE;
}

/* The close that was queued behind a running callback: ask the handler now, close if it agrees. */
static PyObject *win_deferred_close(PyObject *self, PyObject *unused)
{
  KitWindowObject *win = (KitWindowObject *)self;
  if (win->win && BK_window_is_open(win->win) && win_on_close(win, win->win)) {
    BK_window_close(win->win);
  }
  Py_RETURN_NONE;
}

static PyMethodDef win_deferred_close_def = {"close", win_deferred_close, METH_NOARGS, NULL};

static void win_defer_close(void *data)
{
  PyObject *fn = PyCFunction_New(&win_deferred_close_def, (PyObject *)data);
  defer_call(fn, NULL);
  Py_XDECREF(fn);
  PyErr_Clear();
}

static int win_on_close_gil(void *data, BKWindow *w)
{
  GIL_ENTER;
  int allow = 0; /* a close that arrives while a callback runs is answered after it */
  if (g_py_depth == 0) {
    g_py_depth++;
    allow = win_on_close(data, w);
    g_py_depth--;
    run_deferred();
  }
  else {
    win_defer_close(data);
  }
  GIL_LEAVE;
  return allow;
}

static int kitwin_init(KitWindowObject *self, PyObject *args, PyObject *kw)
{
  static char *kwlist[] = {"context", "title", "width", "height", NULL};
  PyObject *ctx;
  const char *title = "BUI";
  int w = 640, h = 480;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "O|sii:KitWindow", kwlist, &ctx, &title, &w, &h)) {
    return -1;
  }
  if (!PyObject_TypeCheck(ctx, &KitContext_Type)) {
    PyErr_SetString(PyExc_TypeError, "KitWindow needs a KitContext");
    return -1;
  }
  if (!BK_window_supported()) {
    PyErr_SetString(PyExc_NotImplementedError, "native windows are not supported here");
    return -1;
  }
  self->win = BK_window_new(((KitContextObject *)ctx)->ctx, title, w, h);
  if (!self->win) {
    PyErr_SetString(PyExc_OSError, "could not create the native window");
    return -1;
  }
  Py_XSETREF(self->ctx, Py_NewRef(ctx));
  Py_XSETREF(self->timers, PyDict_New());
  BKWindowCallbacks cb = {
      .data = self, .on_timer = win_on_timer_gil, .on_close = win_on_close_gil,
      .on_focus = win_on_focus_gil};
  BK_window_set_callbacks(self->win, &cb);
  return 0;
}

static void kitwin_dealloc(KitWindowObject *self)
{
  PyObject_GC_UnTrack(self);
  if (self->win) {
    BK_window_free(self->win);
    self->win = NULL;
  }
  Py_CLEAR(self->ctx);
  Py_CLEAR(self->timers);
  Py_CLEAR(self->on_close);
  Py_CLEAR(self->on_focus);
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static int kitwin_traverse(KitWindowObject *self, visitproc visit, void *arg)
{
  Py_VISIT(self->ctx);
  Py_VISIT(self->timers);
  Py_VISIT(self->on_close);
  Py_VISIT(self->on_focus);
  return 0;
}

static int kitwin_clear(KitWindowObject *self)
{
  Py_CLEAR(self->timers);
  Py_CLEAR(self->on_close);
  Py_CLEAR(self->on_focus);
  return 0;
}

static PyObject *kitwin_show(KitWindowObject *self, PyObject *unused)
{
  BK_window_show(self->win);
  Py_RETURN_NONE;
}

static PyObject *kitwin_close(KitWindowObject *self, PyObject *unused)
{
  BK_window_close(self->win);
  Py_RETURN_NONE;
}

static PyObject *kitwin_is_open(KitWindowObject *self, PyObject *unused)
{
  return PyBool_FromLong(BK_window_is_open(self->win));
}

static PyObject *kitwin_set_title(KitWindowObject *self, PyObject *arg)
{
  const char *t = PyUnicode_AsUTF8(arg);
  if (!t) {
    return NULL;
  }
  BK_window_set_title(self->win, t);
  Py_RETURN_NONE;
}

static PyObject *kitwin_set_size(KitWindowObject *self, PyObject *args)
{
  int w, h;
  if (!PyArg_ParseTuple(args, "ii:set_size", &w, &h)) {
    return NULL;
  }
  BK_window_set_size(self->win, w, h);
  Py_RETURN_NONE;
}

static PyObject *kitwin_wm(KitWindowObject *self, PyObject *args)
{
  const char *key;
  double a = 0.0, b = 0.0;
  if (!PyArg_ParseTuple(args, "s|dd:wm", &key, &a, &b)) {
    return NULL;
  }
  BK_window_wm(self->win, key, a, b);
  Py_RETURN_NONE;
}

static PyObject *kitwin_wm_get(KitWindowObject *self, PyObject *args)
{
  const char *key;
  if (!PyArg_ParseTuple(args, "s:wm_get", &key)) {
    return NULL;
  }
  double a, b;
  if (!BK_window_wm_get(self->win, key, &a, &b)) {
    Py_RETURN_NONE;
  }
  return Py_BuildValue("(dd)", a, b);
}

static PyObject *kitwin_size(KitWindowObject *self, void *c)
{
  int w, h;
  BK_window_get_size(self->win, &w, &h);
  return Py_BuildValue("(ii)", w, h);
}

static PyObject *kitwin_frames(KitWindowObject *self, void *c)
{
  return PyLong_FromLong(BK_window_frames(self->win));
}

static PyObject *kitwin_get_gpu(KitWindowObject *self, void *c)
{
  return PyBool_FromLong(self->win && BK_window_gpu(self->win));
}

static int kitwin_set_gpu(KitWindowObject *self, PyObject *v, void *c)
{
  const int on = v ? PyObject_IsTrue(v) : 0;
  if (on < 0) {
    return -1;
  }
  if (self->win) {
    BK_window_set_gpu(self->win, on);
  }
  return 0;
}

static PyObject *kitwin_hwnd(KitWindowObject *self, void *c)
{
  return PyLong_FromVoidPtr(BK_window_handle(self->win));
}

static PyObject *kitwin_render_now(KitWindowObject *self, PyObject *unused)
{
  BK_window_render_now(self->win);
  if (buipy_restore_error()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kitwin_pixel(KitWindowObject *self, PyObject *args)
{
  int x, y;
  if (!PyArg_ParseTuple(args, "ii:pixel", &x, &y)) {
    return NULL;
  }
  BuiRaster *r = BK_window_raster(self->win);
  const int w = BUI_raster_width(r), h = BUI_raster_height(r);
  if (x < 0 || y < 0 || x >= w || y >= h) {
    PyErr_SetString(PyExc_IndexError, "pixel out of range");
    return NULL;
  }
  const uint32_t p = BUI_raster_pixels(r)[(size_t)y * w + x];
  return Py_BuildValue("(iii)", (int)((p >> 16) & 255), (int)((p >> 8) & 255), (int)(p & 255));
}

static PyObject *kitwin_after(KitWindowObject *self, PyObject *args)
{
  int ms;
  PyObject *fn;
  if (!PyArg_ParseTuple(args, "iO:after", &ms, &fn)) {
    return NULL;
  }
  const int id = BK_window_timer(self->win, ms);
  PyObject *key = PyLong_FromLong(id);
  if (!key || PyDict_SetItem(self->timers, key, fn) < 0) {
    Py_XDECREF(key);
    return NULL;
  }
  return key;
}

static PyObject *kitwin_after_cancel(KitWindowObject *self, PyObject *arg)
{
  const long id = PyLong_AsLong(arg);
  if (id == -1 && PyErr_Occurred()) {
    return NULL;
  }
  BK_window_cancel_timer(self->win, (int)id);
  if (PyDict_DelItem(self->timers, arg) < 0) {
    PyErr_Clear();
  }
  Py_RETURN_NONE;
}

/* A callback attribute; `offset` is the field's offsetof in KitWindowObject. */
static PyObject *kitwin_get_cb(KitWindowObject *self, void *offset)
{
  PyObject *v = *(PyObject **)((char *)self + (size_t)offset);
  return Py_NewRef(v ? v : Py_None);
}

static int kitwin_set_cb(KitWindowObject *self, PyObject *v, void *offset)
{
  Py_XSETREF(*(PyObject **)((char *)self + (size_t)offset), Py_NewRef(v ? v : Py_None));
  return 0;
}

static PyMethodDef kitwin_methods[] = {
    {"show", (PyCFunction)kitwin_show, METH_NOARGS, ""},
    {"close", (PyCFunction)kitwin_close, METH_NOARGS, ""},
    {"is_open", (PyCFunction)kitwin_is_open, METH_NOARGS, ""},
    {"set_title", (PyCFunction)kitwin_set_title, METH_O, ""},
    {"set_size", (PyCFunction)kitwin_set_size, METH_VARARGS, "Logical size (DPI scaled)."},
    {"wm", (PyCFunction)kitwin_wm, METH_VARARGS,
     "wm(key, a=0, b=0): pos, minsize, maxsize, resizable, topmost, alpha, override, state, "
     "lift, lower, focus, transparent (Linux, before show), layer, anchor (Wayland layer shell)."},
    {"wm_get", (PyCFunction)kitwin_wm_get, METH_VARARGS,
     "wm_get(key) -> (a, b) or None: pos, state, screen, pointer, scale, layer."},
    {"render_now", (PyCFunction)kitwin_render_now, METH_NOARGS, "Render + present now."},
    {"pixel", (PyCFunction)kitwin_pixel, METH_VARARGS, "Frame buffer pixel (r, g, b)."},
    {"after", (PyCFunction)kitwin_after, METH_VARARGS, "after(ms, fn) -> id (one-shot)."},
    {"after_cancel", (PyCFunction)kitwin_after_cancel, METH_O, ""},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef kitwin_getset[] = {
    {"size", (getter)kitwin_size, NULL, "client size in pixels", NULL},
    {"frames", (getter)kitwin_frames, NULL, "presented frames", NULL},
    {"gpu", (getter)kitwin_get_gpu, (setter)kitwin_set_gpu,
     "True once frames are drawn on the GPU; set True to switch this window to the GPU", NULL},
    {"hwnd", (getter)kitwin_hwnd, NULL, "native handle", NULL},
    {"on_close", (getter)kitwin_get_cb, (setter)kitwin_set_cb,
     "called on the close button; return True to close",
     (void *)offsetof(KitWindowObject, on_close)},
    {"on_focus", (getter)kitwin_get_cb, (setter)kitwin_set_cb,
     "called with True when the window gets the keyboard focus, False when it loses it",
     (void *)offsetof(KitWindowObject, on_focus)},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyTypeObject KitWindow_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "buipy.KitWindow",
    .tp_basicsize = sizeof(KitWindowObject),
    .tp_dealloc = (destructor)kitwin_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "KitWindow(context, title='BUI', width=640, height=480): native window.",
    .tp_traverse = (traverseproc)kitwin_traverse,
    .tp_clear = (inquiry)kitwin_clear,
    .tp_methods = kitwin_methods,
    .tp_getset = kitwin_getset,
    .tp_init = (initproc)kitwin_init,
    .tp_new = PyType_GenericNew,
};

static PyObject *kit_run(PyObject *m, PyObject *unused)
{
  BK_window_run();
  if (buipy_restore_error()) {
    return NULL;
  }
  Py_RETURN_NONE;
}

static PyObject *kit_pump(PyObject *m, PyObject *unused)
{
  const int open = BK_window_pump();
  if (buipy_restore_error()) {
    return NULL;
  }
  return PyLong_FromLong(open);
}

static PyObject *kit_quit(PyObject *m, PyObject *unused)
{
  BK_window_quit();
  Py_RETURN_NONE;
}

static PyObject *kit_window_supported(PyObject *m, PyObject *unused)
{
  return PyBool_FromLong(BK_window_supported());
}

static PyObject *kit_set_animations(PyObject *m, PyObject *arg)
{
  BK_set_animations(PyObject_IsTrue(arg));
  Py_RETURN_NONE;
}

static PyMethodDef window_functions[] = {
    {"kit_run", kit_run, METH_NOARGS, "Run the native message loop until all windows close."},
    {"kit_pump", kit_pump, METH_NOARGS, "Handle pending native messages; returns open windows."},
    {"kit_quit", kit_quit, METH_NOARGS, "Leave kit_run()."},
    {"kit_window_supported", kit_window_supported, METH_NOARGS, ""},
    {"kit_set_animations", kit_set_animations, METH_O, "Enable/disable UI animations."},
    {NULL, NULL, 0, NULL},
};

/** \} */

int buipy_kit_init(PyObject *module)
{
  BK_set_text_shadow(0); /* Python: crisp text without Blender's UI shadow */
  if (PyType_Ready(&KitNode_Type) < 0 || PyType_Ready(&KitTreeItem_Type) < 0 ||
      PyType_Ready(&KitContext_Type) < 0 || PyType_Ready(&KitWindow_Type) < 0)
  {
    return -1;
  }
  if (PyModule_AddObjectRef(module, "KitWindow", (PyObject *)&KitWindow_Type) < 0 ||
      PyModule_AddFunctions(module, window_functions) < 0)
  {
    return -1;
  }
  BK_window_set_stop_check(stop_check);
  if (PyModule_AddObjectRef(module, "KitNode", (PyObject *)&KitNode_Type) < 0 ||
      PyModule_AddObjectRef(module, "KitTreeItem", (PyObject *)&KitTreeItem_Type) < 0 ||
      PyModule_AddObjectRef(module, "KitContext", (PyObject *)&KitContext_Type) < 0)
  {
    return -1;
  }
  if (PyModule_AddFunctions(module, kit_functions) < 0) {
    return -1;
  }
  BK_set_ref_hooks(ref_retain_gil, ref_release_gil);
  return 0;
}


