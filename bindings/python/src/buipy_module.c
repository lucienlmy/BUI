/* buipy — CPython bindings for the BUI stable C API.
 *
 * Lets Python hosts receive every primitive the BUI toolkit draws through a
 * "sink" object (see bui.Backend) and drive the toolkit's GPU seam. Qt hosts
 * should use bui.qt.BuiCanvas, which installs a QPainter backend on top of
 * this module.
 *
 * Build: linked against bui_api + bui_gpu; no third-party dependencies.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "bui_c_api.h"
#include "GPU_raster.h"

#include <stdint.h>
#include <stdio.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Current text font (set_font); applies to Raster targets. */
static char g_font_family[64] = "";
static int g_font_flags = 0;

/* -------------------------------------------------------------------- */
/** \name Sink holder: forwards BUI primitives to a Python object
 * \{ */

typedef struct BuiPySinkHolder {
  PyObject *backend; /* owned reference; methods looked up per call */
} BuiPySinkHolder;

static BuiPySinkHolder *g_holder = NULL;

/* Call `backend.method(args...)`; returns without touching exceptions on
 * failure other than printing them, so the C draw path stays alive. */
static void buipy_call_backend(const char *method, const char *format, ...)
{
  if (!g_holder || !g_holder->backend) {
    return;
  }
  PyGILState_STATE gstate = PyGILState_Ensure();
  PyObject *func = PyObject_GetAttrString(g_holder->backend, method);
  if (func && PyCallable_Check(func)) {
    va_list args;
    va_start(args, format);
    PyObject *argtuple = Py_VaBuildValue(format, args);
    va_end(args);
    if (argtuple) {
      PyObject *result = PyObject_CallObject(func, argtuple);
      if (result == NULL) {
        PyErr_Print();
      }
      else {
        Py_DECREF(result);
      }
      Py_DECREF(argtuple);
    }
    else {
      PyErr_Print();
    }
  }
  Py_XDECREF(func);
  PyGILState_Release(gstate);
}

static void sink_begin_frame(void *user, int w, int h)
{
  buipy_call_backend("begin_frame", "(ii)", w, h);
}

static void sink_scissor(void *user, int x, int y, int w, int h)
{
  buipy_call_backend("scissor", "(iiii)", x, y, w, h);
}

/* coords: float[count][2]; colors: float[count][4]. */
static PyObject *bui_coords_list(const float (*coords)[2], int count)
{
  PyObject *list = PyList_New(count);
  for (int i = 0; i < count; i++) {
    PyList_SET_ITEM(list, i, Py_BuildValue("(ff)", coords[i][0], coords[i][1]));
  }
  return list;
}
static PyObject *bui_colors_list(const float (*colors)[4], int count)
{
  PyObject *list = PyList_New(count);
  for (int i = 0; i < count; i++) {
    PyList_SET_ITEM(
        list, i, Py_BuildValue("(ffff)", colors[i][0], colors[i][1], colors[i][2], colors[i][3]));
  }
  return list;
}

static void sink_triangles(void *user,
                            const float (*coords)[2],
                            const float (*colors)[4],
                            int count)
{
  PyObject *c = bui_coords_list(coords, count);
  PyObject *k = bui_colors_list(colors, count);
  buipy_call_backend("triangles", "(NN)", c, k);
}

static void sink_lines(void *user,
                        const float (*coords)[2],
                        const float (*colors)[4],
                        int count)
{
  PyObject *c = bui_coords_list(coords, count);
  PyObject *k = bui_colors_list(colors, count);
  buipy_call_backend("lines", "(NN)", c, k);
}

static void sink_points(void *user,
                         const float (*coords)[2],
                         const float (*colors)[4],
                         int count,
                         float size)
{
  PyObject *c = bui_coords_list(coords, count);
  PyObject *k = bui_colors_list(colors, count);
  buipy_call_backend("points", "(NNf)", c, k, size);
}

static void sink_text(void *user,
                       float x,
                       float y,
                       const char *text,
                       float size,
                       const float rgba[4])
{
  buipy_call_backend(
      "text", "(ffs#f(ffff))", x, y, text, (Py_ssize_t)strlen(text), size, rgba[0], rgba[1],
      rgba[2], rgba[3]);
}

static void sink_end_frame(void *user)
{
  buipy_call_backend("end_frame", "()");
}

static BuiGpuSink g_py_sink = {
    NULL,
    sink_begin_frame,
    sink_scissor,
    sink_triangles,
    sink_lines,
    sink_points,
    sink_text,
    sink_end_frame,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Demo scene: exercises the GPU seam from C
 * \{ */

/* Draws a small showcase (rects, lines, points) through the immediate-mode
 * seam; everything flows into the installed backend. */
static PyObject *buipy_demo_scene(PyObject *self, PyObject *args)
{
  int w = 640, h = 480;
  if (!PyArg_ParseTuple(args, "|ii:demo_scene", &w, &h)) {
    return NULL;
  }
  const BuiGpuSink *sink = bui_get_render_sink();
  if (sink && sink->begin_frame) {
    sink->begin_frame(sink->user, w, h);
  }
  bui_set_viewport(w, h);

  const float panel[4] = {0.16f, 0.16f, 0.18f, 1.0f};
  const float accent[4] = {0.25f, 0.47f, 0.85f, 1.0f};
  const float white[4] = {0.92f, 0.92f, 0.94f, 1.0f};
  const float line_col[4] = {0.4f, 0.7f, 0.95f, 1.0f};

  /* Filled panel (two triangles). */
  float coords[6][2] = {{40, 40}, {w - 40, 40}, {w - 40, h - 40}, {40, 40}, {w - 40, h - 40}, {40, h - 40}};
  float colors[12][4]; /* reused below for up to NPTS (12) points */
  for (int i = 0; i < 6; i++) {
    memcpy(colors[i], panel, sizeof(panel));
  }
  if (sink && sink->triangles) {
    sink->triangles(sink->user, coords, colors, 6);
  }

  /* Accent bar. */
  float bar[6][2] = {{40, 40}, {w - 40, 40}, {w - 40, 70}, {40, 40}, {w - 40, 70}, {40, 70}};
  for (int i = 0; i < 6; i++) {
    memcpy(colors[i], accent, sizeof(accent));
  }
  if (sink && sink->triangles) {
    sink->triangles(sink->user, bar, colors, 6);
  }

  /* Cross lines. */
  float lc[4][2] = {{40, 80}, {w - 40, h - 40}, {40, h - 40}, {w - 40, 80}};
  for (int i = 0; i < 4; i++) {
    memcpy(colors[i], line_col, sizeof(line_col));
  }
  if (sink && sink->lines) {
    sink->lines(sink->user, lc, colors, 4);
  }

  /* Diagonal point row. */
  enum { NPTS = 12 };
  float pc[NPTS][2];
  for (int i = 0; i < NPTS; i++) {
    pc[i][0] = 80.0f + i * ((w - 160.0f) / (NPTS - 1));
    pc[i][1] = h - 80.0f;
  }
  for (int i = 0; i < NPTS; i++) {
    memcpy(colors[i], white, sizeof(white));
  }
  if (sink && sink->points) {
    sink->points(sink->user, pc, colors, NPTS, 3.0f);
  }

  if (sink && sink->text) {
    sink->text(sink->user, 60.0f, 110.0f, "BUI demo scene", 14.0f, white);
  }

  if (sink && sink->end_frame) {
    sink->end_frame(sink->user);
  }
  Py_RETURN_NONE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Raster target: native frame renderer (GPU_raster.h)
 *
 * When a Raster is the active target, every draw_* call below renders
 * straight into its frame buffer in C — no Python callbacks, no per-primitive
 * host objects. Hosts then present the frame with one blit per damaged rect.
 * \{ */

typedef struct BuiPyRaster {
  PyObject_HEAD
  BuiRaster *raster;
  PyObject *text_runs; /* list of fallback text runs when no native text */
} BuiPyRaster;

static PyTypeObject BuiPyRaster_Type;

/* Active raster target (borrowed by the C seam, owned reference here). */
static BuiPyRaster *g_target = NULL;

static int raster_init(BuiPyRaster *self, PyObject *args, PyObject *kw)
{
  int w = 1, h = 1;
  static char *kwlist[] = {"width", "height", NULL};
  if (!PyArg_ParseTupleAndKeywords(args, kw, "|ii:Raster", kwlist, &w, &h)) {
    return -1;
  }
  if (self->raster) {
    BUI_raster_free(self->raster);
  }
  self->raster = BUI_raster_create(w, h);
  if (!self->raster) {
    PyErr_NoMemory();
    return -1;
  }
  Py_XSETREF(self->text_runs, PyList_New(0));
  return self->text_runs ? 0 : -1;
}

static void raster_dealloc(BuiPyRaster *self)
{
  if (g_target == self) {
    g_target = NULL;
    bui_set_render_sink(NULL);
  }
  BUI_raster_free(self->raster);
  Py_XDECREF(self->text_runs);
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *raster_size(BuiPyRaster *self, void *closure)
{
  (void)closure;
  return Py_BuildValue("(ii)", BUI_raster_width(self->raster), BUI_raster_height(self->raster));
}

static PyObject *raster_damage(BuiPyRaster *self, PyObject *noargs)
{
  (void)noargs;
  const int(*rects)[4];
  const int n = BUI_raster_damage(self->raster, &rects);
  PyObject *list = PyList_New(n);
  for (int i = 0; i < n; i++) {
    PyList_SET_ITEM(list, i, Py_BuildValue("(iiii)", rects[i][0], rects[i][1], rects[i][2], rects[i][3]));
  }
  return list;
}

static PyObject *raster_invalidate(BuiPyRaster *self, PyObject *noargs)
{
  (void)noargs;
  BUI_raster_invalidate(self->raster);
  Py_RETURN_NONE;
}

/* present_native(window_handle, full=False) -> bool */
static PyObject *raster_present_native(BuiPyRaster *self, PyObject *args)
{
  unsigned long long handle = 0;
  int full = 0;
  if (!PyArg_ParseTuple(args, "K|p:present_native", &handle, &full)) {
    return NULL;
  }
  return PyBool_FromLong(BUI_raster_present_native(self->raster, (void *)(uintptr_t)handle, full));
}

/* ppm(x, y, w, h) -> bytes: binary P6 of a sub-rect (Tk PhotoImage.put). */
static PyObject *raster_ppm(BuiPyRaster *self, PyObject *args)
{
  int x, y, w, h;
  if (!PyArg_ParseTuple(args, "iiii:ppm", &x, &y, &w, &h)) {
    return NULL;
  }
  const int rw = BUI_raster_width(self->raster), rh = BUI_raster_height(self->raster);
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > rw) { w = rw - x; }
  if (y + h > rh) { h = rh - y; }
  if (w <= 0 || h <= 0) {
    return PyBytes_FromStringAndSize(NULL, 0);
  }
  char header[64];
  const int hl = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", w, h);
  PyObject *out = PyBytes_FromStringAndSize(NULL, hl + (Py_ssize_t)w * h * 3);
  if (!out) {
    return NULL;
  }
  unsigned char *dst = (unsigned char *)PyBytes_AS_STRING(out);
  memcpy(dst, header, (size_t)hl);
  dst += hl;
  const uint32_t *pix = BUI_raster_pixels(self->raster);
  for (int row = 0; row < h; row++) {
    const uint32_t *src = pix + (size_t)(y + row) * rw + x;
    for (int col = 0; col < w; col++) {
      const uint32_t p = src[col];
      *dst++ = (unsigned char)(p >> 16);
      *dst++ = (unsigned char)(p >> 8);
      *dst++ = (unsigned char)p;
    }
  }
  return out;
}

/* rgb32() -> bytes: the whole frame, 0x00RRGGBB little-endian (QImage
 * Format_RGB32 / Win32 DIB layout). */
static PyObject *raster_rgb32(BuiPyRaster *self, PyObject *noargs)
{
  (void)noargs;
  const int rw = BUI_raster_width(self->raster), rh = BUI_raster_height(self->raster);
  return PyBytes_FromStringAndSize((const char *)BUI_raster_pixels(self->raster),
                                   (Py_ssize_t)rw * rh * 4);
}

/* pixel(x, y) -> (r, g, b) — for tests and color pickers. */
static PyObject *raster_pixel(BuiPyRaster *self, PyObject *args)
{
  int x, y;
  if (!PyArg_ParseTuple(args, "ii:pixel", &x, &y)) {
    return NULL;
  }
  const int rw = BUI_raster_width(self->raster), rh = BUI_raster_height(self->raster);
  if (x < 0 || y < 0 || x >= rw || y >= rh) {
    PyErr_SetString(PyExc_IndexError, "pixel out of range");
    return NULL;
  }
  const uint32_t p = BUI_raster_pixels(self->raster)[(size_t)y * rw + x];
  return Py_BuildValue("(iii)", (int)((p >> 16) & 255), (int)((p >> 8) & 255), (int)(p & 255));
}

/* text_runs() -> list of (x, y, text, size, rgba, family, flags) drawn this
 * frame that the host must render itself (only when no native text). */
static PyObject *raster_text_runs(BuiPyRaster *self, PyObject *noargs)
{
  (void)noargs;
  return Py_NewRef(self->text_runs);
}

static PyMethodDef raster_methods[] = {
    {"damage", (PyCFunction)raster_damage, METH_NOARGS,
     "damage() -> [(x, y, w, h)]: regions that changed in the last frame."},
    {"invalidate", (PyCFunction)raster_invalidate, METH_NOARGS,
     "invalidate(): report the whole frame as damaged next time."},
    {"present_native", (PyCFunction)raster_present_native, METH_VARARGS,
     "present_native(hwnd, full=False) -> bool: blit damage to a native window."},
    {"ppm", (PyCFunction)raster_ppm, METH_VARARGS, "ppm(x, y, w, h) -> bytes (binary P6)."},
    {"rgb32", (PyCFunction)raster_rgb32, METH_NOARGS, "rgb32() -> bytes (QImage Format_RGB32)."},
    {"pixel", (PyCFunction)raster_pixel, METH_VARARGS, "pixel(x, y) -> (r, g, b)."},
    {"text_runs", (PyCFunction)raster_text_runs, METH_NOARGS,
     "text_runs() -> host-drawn text runs for this frame (no native text engine)."},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef raster_getset[] = {
    {"size", (getter)raster_size, NULL, "(width, height) of the frame buffer", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyTypeObject BuiPyRaster_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "buipy.Raster",
    .tp_basicsize = sizeof(BuiPyRaster),
    .tp_dealloc = (destructor)raster_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Raster(width, height): native anti-aliased frame renderer.\n\n"
              "Pass to buipy.set_backend() to render everything in C, then present\n"
              "with present_native() / ppm() / rgb32().",
    .tp_methods = raster_methods,
    .tp_getset = raster_getset,
    .tp_init = (initproc)raster_init,
    .tp_new = PyType_GenericNew,
};

/* Fast RGBA parse: tuples of 4 numbers (the model's color format). */
static int parse_color(PyObject *color, float rgba[4])
{
  if (PyTuple_CheckExact(color) && PyTuple_GET_SIZE(color) == 4) {
    for (int i = 0; i < 4; i++) {
      PyObject *v = PyTuple_GET_ITEM(color, i);
      rgba[i] = (float)(PyFloat_CheckExact(v) ? PyFloat_AS_DOUBLE(v) : PyFloat_AsDouble(v));
    }
    return PyErr_Occurred() ? 0 : 1;
  }
  return PyArg_ParseTuple(color, "ffff", &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing API: drive the BUI seam from Python
 *
 * These emit primitives directly into the installed sink, so pure-Python UI
 * layers (Qt-syntax, tkinter-syntax) render through the same BUI pipeline.
 * \{ */

static PyObject *buipy_begin_frame(PyObject *self, PyObject *args)
{
  int w, h;
  (void)self;
  if (!PyArg_ParseTuple(args, "ii:begin_frame", &w, &h)) {
    return NULL;
  }
  if (g_target) {
    BUI_raster_begin_frame(g_target->raster, w, h);
    PyList_SetSlice(g_target->text_runs, 0, PY_SSIZE_T_MAX, NULL);
    bui_set_viewport(w, h);
    Py_RETURN_NONE;
  }
  const BuiGpuSink *sink = bui_get_render_sink();
  if (sink && sink->begin_frame) {
    sink->begin_frame(sink->user, w, h);
  }
  bui_set_viewport(w, h);
  Py_RETURN_NONE;
}

static PyObject *buipy_end_frame(PyObject *self, PyObject *args)
{
  const BuiGpuSink *sink = bui_get_render_sink();
  (void)self; (void)args;
  if (g_target) {
    BUI_raster_end_frame(g_target->raster);
    Py_RETURN_NONE;
  }
  if (sink && sink->end_frame) {
    sink->end_frame(sink->user);
  }
  Py_RETURN_NONE;
}

static PyObject *buipy_draw_rect(PyObject *self, PyObject *args)
{
  float x, y, w, h;
  PyObject *color;
  (void)self;
  if (!PyArg_ParseTuple(args, "ffffO:draw_rect", &x, &y, &w, &h, &color)) {
    return NULL;
  }
  float rgba[4];
  if (!parse_color(color, rgba)) {
    return NULL;
  }
  if (g_target) {
    BUI_raster_fill_rect(g_target->raster, x, y, w, h, rgba);
    Py_RETURN_NONE;
  }
  const BuiGpuSink *sink = bui_get_render_sink();
  if (sink && sink->triangles) {
    float coords[6][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y}, {x + w, y + h}, {x, y + h}};
    float colors[6][4];
    for (int i = 0; i < 6; i++) {
      memcpy(colors[i], rgba, sizeof(rgba));
    }
    sink->triangles(sink->user, coords, colors, 6);
  }
  Py_RETURN_NONE;
}

static PyObject *buipy_draw_frame(PyObject *self, PyObject *args)
{
  float x, y, w, h, thickness;
  PyObject *color;
  (void)self;
  if (!PyArg_ParseTuple(args, "fffffO:draw_frame", &x, &y, &w, &h, &thickness, &color)) {
    return NULL;
  }
  float rgba[4];
  if (!parse_color(color, rgba)) {
    return NULL;
  }
  if (g_target) {
    BUI_raster_frame_rect(g_target->raster, x, y, w, h, thickness, rgba);
    Py_RETURN_NONE;
  }
  const BuiGpuSink *sink = bui_get_render_sink();
  if (sink && sink->triangles) {
    float t = thickness;
    float top[6][2] = {{x, y}, {x + w, y}, {x + w, y + t}, {x, y}, {x + w, y + t}, {x, y + t}};
    float bottom[6][2] = {{x, y + h - t}, {x + w, y + h - t}, {x + w, y + h},
                          {x, y + h - t}, {x + w, y + h}, {x, y + h}};
    float left[6][2] = {{x, y}, {x + t, y}, {x + t, y + h}, {x, y}, {x + t, y + h}, {x, y + h}};
    float right[6][2] = {{x + w - t, y}, {x + w, y}, {x + w, y + h},
                         {x + w - t, y}, {x + w, y + h}, {x + w - t, y + h}};
    float *sides[4] = {&top[0][0], &bottom[0][0], &left[0][0], &right[0][0]};
    for (int r = 0; r < 4; r++) {
      float colors[6][4];
      for (int i = 0; i < 6; i++) {
        memcpy(colors[i], rgba, sizeof(rgba));
      }
      sink->triangles(sink->user, (const float (*)[2])sides[r], colors, 6);
    }
  }
  Py_RETURN_NONE;
}

static PyObject *buipy_draw_line(PyObject *self, PyObject *args)
{
  float x1, y1, x2, y2, width = 1.0f;
  PyObject *color;
  (void)self;
  if (!PyArg_ParseTuple(args, "ffffO|f:draw_line", &x1, &y1, &x2, &y2, &color, &width)) {
    return NULL;
  }
  float rgba[4];
  if (!parse_color(color, rgba)) {
    return NULL;
  }
  if (g_target) {
    BUI_raster_line(g_target->raster, x1, y1, x2, y2, width, rgba);
    Py_RETURN_NONE;
  }
  const BuiGpuSink *sink = bui_get_render_sink();
  if (sink && sink->lines) {
    float coords[2][2] = {{x1, y1}, {x2, y2}};
    float colors[2][4];
    for (int i = 0; i < 2; i++) {
      memcpy(colors[i], rgba, sizeof(rgba));
    }
    sink->lines(sink->user, coords, colors, 2);
  }
  Py_RETURN_NONE;
}

static PyObject *buipy_draw_circle(PyObject *self, PyObject *args)
{
  float cx, cy, radius;
  int filled = 1;
  int segments = 48;
  PyObject *color;
  (void)self;
  if (!PyArg_ParseTuple(args, "fffO|ii:draw_circle", &cx, &cy, &radius, &color, &filled, &segments)) {
    return NULL;
  }
  float rgba[4];
  if (!parse_color(color, rgba)) {
    return NULL;
  }
  if (g_target) {
    BUI_raster_circle(g_target->raster, cx, cy, radius, filled ? 0.0f : 1.0f, rgba);
    Py_RETURN_NONE;
  }
  if (segments < 3) {
    segments = 3;
  }
  const BuiGpuSink *sink = bui_get_render_sink();
  if (!sink) {
    Py_RETURN_NONE;
  }
  if (filled && sink->triangles) {
    for (int i = 0; i < segments; i++) {
      float a0 = (float)(2.0 * 3.141592653589793 * i / segments);
      float a1 = (float)(2.0 * 3.141592653589793 * (i + 1) / segments);
      float coords[3][2] = {{cx, cy},
                            {cx + radius * cosf(a0), cy + radius * sinf(a0)},
                            {cx + radius * cosf(a1), cy + radius * sinf(a1)}};
      float colors[3][4];
      for (int v = 0; v < 3; v++) {
        memcpy(colors[v], rgba, sizeof(rgba));
      }
      sink->triangles(sink->user, coords, colors, 3);
    }
  }
  else if (!filled && sink->lines) {
    for (int i = 0; i < segments; i++) {
      float a0 = (float)(2.0 * 3.141592653589793 * i / segments);
      float a1 = (float)(2.0 * 3.141592653589793 * (i + 1) / segments);
      float coords[2][2] = {{cx + radius * cosf(a0), cy + radius * sinf(a0)},
                            {cx + radius * cosf(a1), cy + radius * sinf(a1)}};
      float colors[2][4];
      for (int v = 0; v < 2; v++) {
        memcpy(colors[v], rgba, sizeof(rgba));
      }
      sink->lines(sink->user, coords, colors, 2);
    }
  }
  Py_RETURN_NONE;
}

/* draw_ellipse(cx, cy, rx, ry, color, filled=True, line_width=1) */
static PyObject *buipy_draw_ellipse(PyObject *self, PyObject *args)
{
  float cx, cy, rx, ry, lw = 1.0f;
  int filled = 1;
  PyObject *color;
  (void)self;
  if (!PyArg_ParseTuple(args, "ffffO|pf:draw_ellipse", &cx, &cy, &rx, &ry, &color, &filled, &lw)) {
    return NULL;
  }
  float rgba[4];
  if (!parse_color(color, rgba)) {
    return NULL;
  }
  if (g_target) {
    BUI_raster_ellipse(g_target->raster, cx, cy, rx, ry, filled ? 0.0f : lw, rgba);
    Py_RETURN_NONE;
  }
  const BuiGpuSink *sink = bui_get_render_sink();
  if (!sink) {
    Py_RETURN_NONE;
  }
  enum { SEG = 48 };
  for (int i = 0; i < SEG; i++) {
    const float a0 = (float)(2.0 * 3.141592653589793 * i / SEG);
    const float a1 = (float)(2.0 * 3.141592653589793 * (i + 1) / SEG);
    const float p0[2] = {cx + rx * cosf(a0), cy + ry * sinf(a0)};
    const float p1[2] = {cx + rx * cosf(a1), cy + ry * sinf(a1)};
    float colors[3][4];
    for (int v = 0; v < 3; v++) {
      memcpy(colors[v], rgba, sizeof(rgba));
    }
    if (filled && sink->triangles) {
      float coords[3][2] = {{cx, cy}, {p0[0], p0[1]}, {p1[0], p1[1]}};
      sink->triangles(sink->user, coords, colors, 3);
    }
    else if (!filled && sink->lines) {
      float coords[2][2] = {{p0[0], p0[1]}, {p1[0], p1[1]}};
      sink->lines(sink->user, coords, colors, 2);
    }
  }
  Py_RETURN_NONE;
}

/* draw_polygon([(x, y), ...], color): filled polygon (even-odd). */
static PyObject *buipy_draw_polygon(PyObject *self, PyObject *args)
{
  PyObject *pts_obj, *color;
  (void)self;
  if (!PyArg_ParseTuple(args, "OO:draw_polygon", &pts_obj, &color)) {
    return NULL;
  }
  float rgba[4];
  if (!parse_color(color, rgba)) {
    return NULL;
  }
  PyObject *seq = PySequence_Fast(pts_obj, "points must be a sequence of (x, y)");
  if (!seq) {
    return NULL;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  float(*pts)[2] = (float(*)[2])malloc(sizeof(float[2]) * (size_t)(n > 0 ? n : 1));
  if (!pts) {
    Py_DECREF(seq);
    return PyErr_NoMemory();
  }
  for (Py_ssize_t i = 0; i < n; i++) {
    if (!PyArg_ParseTuple(PySequence_Fast_GET_ITEM(seq, i), "ff", &pts[i][0], &pts[i][1])) {
      free(pts);
      Py_DECREF(seq);
      return NULL;
    }
  }
  Py_DECREF(seq);
  if (g_target) {
    BUI_raster_polygon(g_target->raster, (const float(*)[2])pts, (int)n, rgba);
  }
  else {
    const BuiGpuSink *sink = bui_get_render_sink();
    if (sink && sink->triangles) {
      float colors[3][4];
      for (int v = 0; v < 3; v++) {
        memcpy(colors[v], rgba, sizeof(rgba));
      }
      for (Py_ssize_t i = 1; i + 1 < n; i++) { /* fan: exact for convex */
        float coords[3][2] = {{pts[0][0], pts[0][1]}, {pts[i][0], pts[i][1]},
                              {pts[i + 1][0], pts[i + 1][1]}};
        sink->triangles(sink->user, coords, colors, 3);
      }
    }
  }
  free(pts);
  Py_RETURN_NONE;
}

/* Emit triangles to the active target (raster) or the installed sink. */
static void emit_triangles(const float (*co)[2], const float (*col)[4], int n)
{
  if (g_target) {
    BUI_raster_triangles(g_target->raster, co, col, n);
    return;
  }
  const BuiGpuSink *sink = bui_get_render_sink();
  if (sink && sink->triangles && n >= 3) {
    sink->triangles(sink->user, co, col, n);
  }
}

static void hsv_rgb(float h, float s, float v, float out[4])
{
  h = h - floorf(h);
  const float h6 = h * 6.0f;
  float c[3] = {fabsf(h6 - 3.0f) - 1.0f, 2.0f - fabsf(h6 - 2.0f), 2.0f - fabsf(h6 - 4.0f)};
  for (int k = 0; k < 3; k++) {
    c[k] = c[k] < 0 ? 0 : (c[k] > 1 ? 1 : c[k]);
    out[k] = ((c[k] - 1.0f) * s + 1.0f) * v;
  }
  out[3] = 1.0f;
}

static PyObject *buipy_draw_triangles(PyObject *self, PyObject *args)
{
  PyObject *co_obj, *col_obj;
  (void)self;
  if (!PyArg_ParseTuple(args, "OO:draw_triangles", &co_obj, &col_obj)) {
    return NULL;
  }
  PyObject *cs = PySequence_Fast(co_obj, "coords must be a sequence of (x, y)");
  if (!cs) {
    return NULL;
  }
  PyObject *ks = PySequence_Fast(col_obj, "colors must be a sequence of (r, g, b, a)");
  if (!ks) {
    Py_DECREF(cs);
    return NULL;
  }
  Py_ssize_t n = PySequence_Fast_GET_SIZE(cs);
  if (PySequence_Fast_GET_SIZE(ks) != n) {
    Py_DECREF(cs);
    Py_DECREF(ks);
    PyErr_SetString(PyExc_ValueError, "coords and colors must have the same length");
    return NULL;
  }
  n -= n % 3;
  float(*co)[2] = (float(*)[2])malloc(sizeof(float[2]) * (size_t)(n > 0 ? n : 1));
  float(*col)[4] = (float(*)[4])malloc(sizeof(float[4]) * (size_t)(n > 0 ? n : 1));
  int ok = co && col;
  for (Py_ssize_t i = 0; ok && i < n; i++) {
    ok = PyArg_ParseTuple(PySequence_Fast_GET_ITEM(cs, i), "ff", &co[i][0], &co[i][1]) &&
         parse_color(PySequence_Fast_GET_ITEM(ks, i), col[i]);
  }
  Py_DECREF(cs);
  Py_DECREF(ks);
  if (ok) {
    emit_triangles((const float(*)[2])co, (const float(*)[4])col, (int)n);
  }
  free(co);
  free(col);
  if (!ok) {
    return PyErr_Occurred() ? NULL : PyErr_NoMemory();
  }
  Py_RETURN_NONE;
}

static PyObject *buipy_draw_gradient(PyObject *self, PyObject *args)
{
  float x, y, w, h;
  PyObject *o[4];
  float c[4][4];
  (void)self;
  if (!PyArg_ParseTuple(args, "ffffOOOO:draw_gradient", &x, &y, &w, &h, &o[0], &o[1], &o[2],
                        &o[3]))
  {
    return NULL;
  }
  for (int i = 0; i < 4; i++) {
    if (!parse_color(o[i], c[i])) {
      return NULL;
    }
  }
  if (g_target) {
    BUI_raster_gradient(g_target->raster, x, y, w, h, c[0], c[1], c[2], c[3]);
  }
  else {
    const float co[6][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y}, {x + w, y + h}, {x, y + h}};
    float col[6][4];
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
      memcpy(col[i], c[idx[i]], sizeof(float[4]));
    }
    emit_triangles(co, (const float(*)[4])col, 6);
  }
  Py_RETURN_NONE;
}

static PyObject *buipy_draw_hsv_circle(PyObject *self, PyObject *args)
{
  float cx, cy, radius, value;
  (void)self;
  if (!PyArg_ParseTuple(args, "ffff:draw_hsv_circle", &cx, &cy, &radius, &value)) {
    return NULL;
  }
  if (g_target) {
    BUI_raster_hsv_circle(g_target->raster, cx, cy, radius, value);
    Py_RETURN_NONE;
  }
  enum { SEG = 64 };
  float co[3][2], col[3][4];
  for (int i = 0; i < SEG; i++) {
    const float a0 = (float)(2.0 * 3.141592653589793 * i / SEG);
    const float a1 = (float)(2.0 * 3.141592653589793 * (i + 1) / SEG);
    co[0][0] = cx;
    co[0][1] = cy;
    co[1][0] = cx + radius * sinf(a0);
    co[1][1] = cy - radius * cosf(a0);
    co[2][0] = cx + radius * sinf(a1);
    co[2][1] = cy - radius * cosf(a1);
    hsv_rgb(0.0f, 0.0f, value, col[0]);
    hsv_rgb(a0 / 6.2831853f + 0.5f, 1.0f, value, col[1]);
    hsv_rgb(a1 / 6.2831853f + 0.5f, 1.0f, value, col[2]);
    emit_triangles((const float(*)[2])co, (const float(*)[4])col, 3);
  }
  Py_RETURN_NONE;
}

static PyObject *buipy_draw_sphere(PyObject *self, PyObject *args)
{
  float cx, cy, radius, light[3];
  PyObject *color;
  (void)self;
  if (!PyArg_ParseTuple(args, "fff(fff)O:draw_sphere", &cx, &cy, &radius, &light[0], &light[1],
                        &light[2], &color))
  {
    return NULL;
  }
  float rgba[4];
  if (!parse_color(color, rgba)) {
    return NULL;
  }
  if (g_target) {
    BUI_raster_sphere(g_target->raster, cx, cy, radius, light, rgba);
    Py_RETURN_NONE;
  }
  enum { SEG = 48 };
  float co[3][2], col[3][4];
  for (int v = 0; v < 3; v++) {
    memcpy(col[v], rgba, sizeof(rgba));
  }
  for (int i = 0; i < SEG; i++) {
    const float a0 = (float)(2.0 * 3.141592653589793 * i / SEG);
    const float a1 = (float)(2.0 * 3.141592653589793 * (i + 1) / SEG);
    co[0][0] = cx;
    co[0][1] = cy;
    co[1][0] = cx + radius * cosf(a0);
    co[1][1] = cy + radius * sinf(a0);
    co[2][0] = cx + radius * cosf(a1);
    co[2][1] = cy + radius * sinf(a1);
    emit_triangles((const float(*)[2])co, (const float(*)[4])col, 3);
  }
  Py_RETURN_NONE;
}

/* Optional color argument: None -> NULL. */
static int parse_opt_color(PyObject *obj, float rgba[4], const float **r_ptr)
{
  if (obj == NULL || obj == Py_None) {
    *r_ptr = NULL;
    return 1;
  }
  if (!parse_color(obj, rgba)) {
    return 0;
  }
  *r_ptr = rgba;
  return 1;
}

/* draw_widget(x, y, w, h, radius, inner, outline=None, emboss=None,
 *             inner_bottom=None, corners=15, outline_width=1.0)
 * Blender widget base: shaded inner, inside outline, emboss underline. */
static PyObject *buipy_draw_widget(PyObject *self, PyObject *args, PyObject *kw)
{
  static char *kwlist[] = {"x", "y", "w", "h", "radius", "inner", "outline", "emboss",
                           "inner_bottom", "corners", "outline_width", NULL};
  float x, y, w, h, radius, outline_width = 1.0f;
  PyObject *inner_o = Py_None, *outline_o = Py_None, *emboss_o = Py_None, *bottom_o = Py_None;
  int corners = 15;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "fffffO|OOOif:draw_widget", kwlist, &x, &y, &w,
                                   &h, &radius, &inner_o, &outline_o, &emboss_o, &bottom_o,
                                   &corners, &outline_width))
  {
    return NULL;
  }
  float c_in[4], c_out[4], c_emb[4], c_bot[4];
  const float *in, *out, *emb, *bot;
  if (!parse_opt_color(inner_o, c_in, &in) || !parse_opt_color(outline_o, c_out, &out) ||
      !parse_opt_color(emboss_o, c_emb, &emb) || !parse_opt_color(bottom_o, c_bot, &bot))
  {
    return NULL;
  }
  if (g_target) {
    BUI_raster_widget(g_target->raster, x, y, w, h, radius, corners, in, bot ? bot : in, out,
                      outline_width, emb);
    Py_RETURN_NONE;
  }
  /* Python sinks: approximate with rounded rects (outline, then inner). */
  PyObject *res;
  if (out) {
    res = PyObject_CallMethod(PyImport_AddModule("buipy"), "draw_roundrect", "fffffO", x, y, w,
                              h, radius, outline_o);
    if (!res) {
      return NULL;
    }
    Py_DECREF(res);
  }
  if (in) {
    const float o = out ? outline_width : 0.0f;
    res = PyObject_CallMethod(PyImport_AddModule("buipy"), "draw_roundrect", "fffffO", x + o,
                              y + o, w - 2 * o, h - 2 * o, radius > o ? radius - o : 0.0f,
                              inner_o);
    if (!res) {
      return NULL;
    }
    Py_DECREF(res);
  }
  Py_RETURN_NONE;
}

/* draw_shadow(x, y, w, h, radius, size, alpha): soft popup shadow. */
static PyObject *buipy_draw_shadow(PyObject *self, PyObject *args)
{
  float x, y, w, h, radius, size, alpha;
  (void)self;
  if (!PyArg_ParseTuple(args, "fffffff:draw_shadow", &x, &y, &w, &h, &radius, &size, &alpha)) {
    return NULL;
  }
  if (g_target) {
    BUI_raster_shadow(g_target->raster, x, y, w, h, radius, size, alpha);
  }
  Py_RETURN_NONE;
}

static PyObject *buipy_draw_roundrect(PyObject *self, PyObject *args)
{
  float x, y, w, h, radius;
  PyObject *color;
  (void)self;
  if (!PyArg_ParseTuple(args, "fffffO:draw_roundrect", &x, &y, &w, &h, &radius, &color)) {
    return NULL;
  }
  float rgba[4];
  if (!parse_color(color, rgba)) {
    return NULL;
  }
  if (g_target) {
    BUI_raster_round_rect(g_target->raster, x, y, w, h, radius, rgba);
    Py_RETURN_NONE;
  }
  const BuiGpuSink *sink = bui_get_render_sink();
  if (!sink || !sink->triangles) {
    Py_RETURN_NONE;
  }
  const float max_r = (w < h ? w : h) / 2.0f;
  if (radius < 0.0f) {
    radius = 0.0f;
  }
  if (radius > max_r) {
    radius = max_r;
  }
  const float cx = x + w / 2.0f;
  const float cy = y + h / 2.0f;
  /* Build the rounded-rect outline as a polyline, then emit a triangle fan. */
  const int seg = 6;               /* segments per corner */
  const int points = seg * 4 + 1;  /* closed loop; last == first */
  float(*outline)[2] = malloc(sizeof(float) * 2 * points);
  const float centers[4][2] = {{x + w - radius, y + radius},
                               {x + radius, y + radius},
                               {x + radius, y + h - radius},
                               {x + w - radius, y + h - radius}};
  const float starts[4] = {0.0f, 1.5707963f, 3.1415926f, 4.7123890f};
  int n = 0;
  for (int c = 0; c < 4; c++) {
    for (int i = 0; i <= seg; i++) {
      if (c > 0 && i == 0) {
        continue; /* corner start equals previous corner end */
      }
      const float a = starts[c] + (float)(1.5707963267948966 * i / seg);
      outline[n][0] = centers[c][0] + radius * cosf(a);
      outline[n][1] = centers[c][1] + radius * sinf(a);
      n++;
    }
  }
  for (int i = 0; i < n; i++) {
    float coords[3][2] = {{cx, cy}, {outline[i][0], outline[i][1]},
                          {outline[(i + 1) % n][0], outline[(i + 1) % n][1]}};
    float colors[3][4];
    for (int v = 0; v < 3; v++) {
      memcpy(colors[v], rgba, sizeof(rgba));
    }
    sink->triangles(sink->user, coords, colors, 3);
  }
  free(outline);
  Py_RETURN_NONE;
}

static PyObject *buipy_set_clip(PyObject *self, PyObject *args)
{
  int x, y, w, h;
  (void)self;
  if (!PyArg_ParseTuple(args, "iiii:set_clip", &x, &y, &w, &h)) {
    return NULL;
  }
  if (g_target) {
    BUI_raster_set_clip(g_target->raster, x, y, w, h);
    Py_RETURN_NONE;
  }
  const BuiGpuSink *sink = bui_get_render_sink();
  if (sink && sink->scissor) {
    sink->scissor(sink->user, x, y, w, h);
  }
  Py_RETURN_NONE;
}

static PyObject *buipy_draw_text(PyObject *self, PyObject *args)
{
  float x, y, size;
  const char *text;
  Py_ssize_t text_len;
  PyObject *color;
  (void)self;
  if (!PyArg_ParseTuple(args, "fffs#O:draw_text", &x, &y, &size, &text, &text_len, &color)) {
    return NULL;
  }
  float rgba[4];
  if (!parse_color(color, rgba)) {
    return NULL;
  }
  if (g_target) {
    if (!BUI_raster_text(g_target->raster, x, y, text, (int)text_len, size, rgba)) {
      /* No native text engine: hand the run to the host (drawn on top). */
      PyObject *run = Py_BuildValue("(ffs#f(ffff)si)", x, y, text, text_len, size, rgba[0],
                                    rgba[1], rgba[2], rgba[3], g_font_family, g_font_flags);
      if (!run || PyList_Append(g_target->text_runs, run) < 0) {
        Py_XDECREF(run);
        return NULL;
      }
      Py_DECREF(run);
    }
    Py_RETURN_NONE;
  }
  const BuiGpuSink *sink = bui_get_render_sink();
  if (sink && sink->text) {
    sink->text(sink->user, x, y, text, size, rgba);
  }
  Py_RETURN_NONE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Module glue
 * \{ */

/* set_font(family, flags): font for subsequent draw_text (flags: 1 bold,
 * 2 italic, 4 underline). Cheap; widgets call it before painting text. */
static PyObject *buipy_set_font(PyObject *self, PyObject *args)
{
  const char *family = "";
  int flags = 0;
  (void)self;
  if (!PyArg_ParseTuple(args, "|zi:set_font", &family, &flags)) {
    return NULL;
  }
  if (!family) {
    family = "";
  }
  if (strcmp(family, g_font_family) != 0 || flags != g_font_flags) {
    strncpy(g_font_family, family, sizeof(g_font_family) - 1);
    g_font_family[sizeof(g_font_family) - 1] = '\0';
    g_font_flags = flags;
    if (g_target) {
      BUI_raster_set_font(g_target->raster, g_font_family, g_font_flags);
    }
  }
  Py_RETURN_NONE;
}

/* measure_text(text, size, family="", flags=0) -> float advance in pixels,
 * or None when no native text engine exists (callers fall back). */
static PyObject *buipy_measure_text(PyObject *self, PyObject *args)
{
  const char *text;
  Py_ssize_t len;
  float size;
  const char *family = "";
  int flags = 0;
  (void)self;
  if (!PyArg_ParseTuple(args, "s#f|zi:measure_text", &text, &len, &size, &family, &flags)) {
    return NULL;
  }
  float width;
  if (!BUI_text_measure(family, flags, size, text, (int)len, &width)) {
    Py_RETURN_NONE;
  }
  return PyFloat_FromDouble(width);
}

static PyObject *buipy_has_native_text(PyObject *self, PyObject *noargs)
{
  (void)self;
  (void)noargs;
  return PyBool_FromLong(BUI_text_native());
}

static PyObject *buipy_set_gpu(PyObject *self, PyObject *on)
{
  (void)self;
  const int v = PyObject_IsTrue(on);
  if (v < 0) {
    return NULL;
  }
  BUI_gpu_set_default(v);
  Py_RETURN_NONE;
}

static PyObject *buipy_set_slug(PyObject *self, PyObject *on)
{
  (void)self;
  const int v = PyObject_IsTrue(on);
  if (v < 0) {
    return NULL;
  }
  BUI_gpu_set_slug(v);
  Py_RETURN_NONE;
}

static PyObject *buipy_gpu_default(PyObject *self, PyObject *noargs)
{
  (void)self;
  (void)noargs;
  return PyBool_FromLong(BUI_gpu_default());
}

static PyObject *buipy_version(PyObject *self, PyObject *args)
{
  int major, minor, patch;
  bui_get_version(&major, &minor, &patch);
  return PyUnicode_FromFormat("%d.%d.%d", major, minor, patch);
}

static void target_clear(void)
{
  if (g_target) {
    BuiPyRaster *t = g_target;
    g_target = NULL;
    Py_DECREF(t);
  }
}

static PyObject *buipy_set_backend(PyObject *self, PyObject *arg)
{
  if (PyObject_TypeCheck(arg, &BuiPyRaster_Type)) {
    /* Native path: drop any Python sink, render into the raster. */
    if (g_holder) {
      Py_XDECREF(g_holder->backend);
      free(g_holder);
      g_holder = NULL;
    }
    target_clear();
    g_target = (BuiPyRaster *)Py_NewRef(arg);
    BUI_raster_set_font(g_target->raster, g_font_family, g_font_flags);
    /* C toolkit code drawing through the GPU seam lands in the raster too. */
    bui_set_render_sink(BUI_raster_sink(g_target->raster));
    Py_RETURN_NONE;
  }
  target_clear();
  if (arg == Py_None) {
    bui_set_render_sink(NULL);
    if (g_holder) {
      Py_XDECREF(g_holder->backend);
      free(g_holder);
      g_holder = NULL;
    }
    Py_RETURN_NONE;
  }
  if (!PyObject_HasAttrString(arg, "triangles")) {
    PyErr_SetString(PyExc_TypeError, "backend must implement the bui.Backend protocol");
    return NULL;
  }
  /* Drop an existing holder first. */
  if (g_holder) {
    bui_set_render_sink(NULL);
    Py_XDECREF(g_holder->backend);
    free(g_holder);
    g_holder = NULL;
  }
  g_holder = (BuiPySinkHolder *)malloc(sizeof(BuiPySinkHolder));
  g_holder->backend = Py_NewRef(arg);
  bui_set_render_sink(&g_py_sink);
  Py_RETURN_NONE;
}

static PyObject *buipy_set_viewport(PyObject *self, PyObject *args)
{
  int w, h;
  if (!PyArg_ParseTuple(args, "ii:set_viewport", &w, &h)) {
    return NULL;
  }
  bui_set_viewport(w, h);
  Py_RETURN_NONE;
}

static PyMethodDef buipy_methods[] = {
    {"version", buipy_version, METH_NOARGS, "Return the BUI version string."},
    {"set_backend", buipy_set_backend, METH_O, "Install a bui.Backend (or None to clear)."},
    {"set_viewport", buipy_set_viewport, METH_VARARGS, "Record the host viewport size in pixels."},
    {"demo_scene", buipy_demo_scene, METH_VARARGS, "Draw a small showcase scene through the backend."},
    {"begin_frame", buipy_begin_frame, METH_VARARGS, "begin_frame(w, h): start a frame batch."},
    {"end_frame", buipy_end_frame, METH_NOARGS, "end_frame(): finish the frame batch."},
    {"draw_rect", buipy_draw_rect, METH_VARARGS, "draw_rect(x, y, w, h, (r, g, b, a)): filled rectangle."},
    {"draw_frame", buipy_draw_frame, METH_VARARGS, "draw_frame(x, y, w, h, thickness, (r, g, b, a)): rectangle outline."},
    {"draw_line", buipy_draw_line, METH_VARARGS, "draw_line(x1, y1, x2, y2, (r, g, b, a), width=1): line."},
    {"draw_widget", (PyCFunction)(void (*)(void))buipy_draw_widget, METH_VARARGS | METH_KEYWORDS, "draw_widget(x, y, w, h, radius, inner, outline=None, emboss=None, inner_bottom=None, corners=15, outline_width=1): Blender widget base."},
    {"draw_shadow", buipy_draw_shadow, METH_VARARGS, "draw_shadow(x, y, w, h, radius, size, alpha): soft popup shadow."},
    {"draw_ellipse", buipy_draw_ellipse, METH_VARARGS, "draw_ellipse(cx, cy, rx, ry, (r, g, b, a), filled=True, line_width=1)."},
    {"draw_polygon", buipy_draw_polygon, METH_VARARGS, "draw_polygon([(x, y), ...], (r, g, b, a)): filled polygon."},
    {"draw_circle", buipy_draw_circle, METH_VARARGS, "draw_circle(cx, cy, radius, (r, g, b, a), filled=True, segments=48)."},
    {"draw_text", buipy_draw_text, METH_VARARGS, "draw_text(x, y, size, text, (r, g, b, a)): text run at baseline."},
    {"draw_roundrect", buipy_draw_roundrect, METH_VARARGS, "draw_roundrect(x, y, w, h, radius, (r, g, b, a)): filled rounded rectangle."},
    {"set_font", buipy_set_font, METH_VARARGS, "set_font(family='', flags=0): font for draw_text (1 bold, 2 italic, 4 underline)."},
    {"measure_text", buipy_measure_text, METH_VARARGS, "measure_text(text, size, family='', flags=0) -> float | None."},
    {"has_native_text", buipy_has_native_text, METH_NOARGS, "True when draw_text renders natively into a Raster."},
    {"set_gpu", buipy_set_gpu, METH_O,
     "set_gpu(on): new native windows draw on the GPU (Direct3D 11). BUI_GPU=1/0 overrides."},
    {"gpu_default", buipy_gpu_default, METH_NOARGS, "Whether new native windows use the GPU."},
    {"set_slug", buipy_set_slug, METH_O,
     "set_slug(on): GPU text from glyph outlines (Slug) or a glyph atlas. BUI_SLUG=1/0 overrides."},
    {"draw_triangles", buipy_draw_triangles, METH_VARARGS, "draw_triangles([(x, y), ...], [(r, g, b, a), ...]): per-vertex colored triangles."},
    {"draw_gradient", buipy_draw_gradient, METH_VARARGS, "draw_gradient(x, y, w, h, tl, tr, br, bl): bilinear corner-color rectangle."},
    {"draw_hsv_circle", buipy_draw_hsv_circle, METH_VARARGS, "draw_hsv_circle(cx, cy, radius, value): Blender HSV color wheel."},
    {"draw_sphere", buipy_draw_sphere, METH_VARARGS, "draw_sphere(cx, cy, radius, (lx, ly, lz), color): lit sphere (normal ball)."},
    {"set_clip", buipy_set_clip, METH_VARARGS, "set_clip(x, y, w, h): clip subsequent drawing (w/h <= 0 clears)."},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef buipy_module = {
    PyModuleDef_HEAD_INIT,
    "buipy",
    "Python bindings for the BUI UI toolkit (Blender-derived).",
    -1,
    buipy_methods,
    NULL,
    NULL,
    NULL,
    NULL,
};

/* Shared with buipy_kit.c: the active raster target / a Raster object. */
BuiRaster *buipy_current_raster(void)
{
  return g_target ? g_target->raster : NULL;
}

BuiRaster *buipy_raster_of(PyObject *obj)
{
  if (obj && PyObject_TypeCheck(obj, &BuiPyRaster_Type)) {
    return ((BuiPyRaster *)obj)->raster;
  }
  return NULL;
}

#include "buipy.h"

#ifdef BUI_PY_IMGUI
int buipy_imgui_init(PyObject *module); /* buiimgui_module.cpp: Dear ImGui, in the same binary */
#endif

PyMODINIT_FUNC PyInit_buipy(void)
{
  if (PyType_Ready(&BuiPyRaster_Type) < 0) {
    return NULL;
  }
  PyObject *m = PyModule_Create(&buipy_module);
  if (!m) {
    return NULL;
  }
  if (PyModule_AddObjectRef(m, "Raster", (PyObject *)&BuiPyRaster_Type) < 0 ||
      buipy_kit_init(m) < 0 || buipy_video_init(m) < 0 || buipy_anim_init(m) < 0
#ifdef BUI_PY_IMGUI
      || buipy_imgui_init(m) < 0
#endif
  )
  {
    Py_DECREF(m);
    return NULL;
  }
  return m;
}

/** \} */
