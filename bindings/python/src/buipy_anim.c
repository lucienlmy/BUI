/* buipy animation: the kit's engine (kit/intern/anim, BUI_anim.h) under bui.gsap, bui.motion and
 * Qt syntax's animations.
 *
 * Python's arguments are read once, here, into the engine's tracks and options (property ids,
 * values, eases, springs), and checked here: an unknown option, ease, value or property raises at
 * the call. Then every frame runs in C; Python runs per frame only for what asks for it (on_update
 * and the other callbacks, and plain Python objects as targets).
 *
 *   Motion   a tween's or a timeline's handle, GSAP's controls (bui.gsap's Tween and Timeline)
 *   Ease     an ease read once (a GSAP / Motion / CSS / Blender name, a bezier, points, a path)
 *   motion_tween, motion_timeline, motion_tick ...
 *
 * Targets: kit nodes (BK_anim_node: "value", colors, any number BK_set_num takes) and any Python
 * object (its attributes, read and written by name). Kit widgets have no transforms or opacity.
 *
 * A motion owns its callbacks: they go to the engine as BKMotionVars.data / free_data and are
 * released with the motion (when its handle and the timelines holding it let go).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "buipy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "BUI_anim.h"

enum { FLAVOR_GSAP = 0, FLAVOR_MOTION };
enum { MODE_TO = 0, MODE_FROM, MODE_FROM_TO, MODE_SET };

/* -------------------------------------------------------------------- */
/** \name Ease
 * \{ */

typedef struct EaseObject {
  PyObject_HEAD
  BKEase e;
} EaseObject;

static PyTypeObject Ease_Type;

static int floats_read(PyObject *v, float *out, Py_ssize_t n, const char *what)
{
  PyObject *seq = PySequence_Fast(v, what);
  if (!seq) {
    return 0;
  }
  if (PySequence_Fast_GET_SIZE(seq) != n) {
    Py_DECREF(seq);
    PyErr_Format(PyExc_ValueError, "%s: expected %zd numbers", what, n);
    return 0;
  }
  for (Py_ssize_t i = 0; i < n; i++) {
    out[i] = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, i));
  }
  Py_DECREF(seq);
  return !PyErr_Occurred();
}

/* An ease: a name, an Ease, or a cubic bezier [x1, y1, x2, y2] (Motion's array form). */
static int ease_read(PyObject *v, BKEase *r)
{
  if (PyObject_TypeCheck(v, &Ease_Type)) {
    *r = ((EaseObject *)v)->e;
    return 1;
  }
  if (PyUnicode_Check(v)) {
    const char *s = PyUnicode_AsUTF8(v);
    if (!s) {
      return 0;
    }
    if (!BK_ease_parse(s, r)) {
      PyErr_Format(PyExc_ValueError, "unknown ease '%s'", s);
      return 0;
    }
    return 1;
  }
  if ((PyList_Check(v) || PyTuple_Check(v)) && PySequence_Size(v) == 4) {
    float b[4];
    if (!floats_read(v, b, 4, "a cubic bezier ease")) {
      return 0;
    }
    *r = BK_ease_bezier(b[0], b[1], b[2], b[3]);
    return 1;
  }
  PyErr_Format(PyExc_TypeError, "an ease is a name, an Ease or [x1, y1, x2, y2], not %.80s",
               Py_TYPE(v)->tp_name);
  return 0;
}

static PyObject *ease_wrap(const BKEase *e)
{
  EaseObject *o = PyObject_New(EaseObject, &Ease_Type);
  if (o) {
    o->e = *e;
  }
  return (PyObject *)o;
}

static PyObject *ease_new(PyTypeObject *type, PyObject *args, PyObject *kw)
{
  PyObject *spec;
  if (!PyArg_ParseTuple(args, "O:Ease", &spec)) {
    return NULL;
  }
  BKEase e;
  return ease_read(spec, &e) ? ease_wrap(&e) : NULL;
}

static PyObject *ease_call(EaseObject *self, PyObject *args, PyObject *kw)
{
  double t;
  if (!PyArg_ParseTuple(args, "d:Ease", &t)) {
    return NULL;
  }
  return PyFloat_FromDouble(BK_ease_eval(&self->e, (float)t));
}

/* Ease.points([(x, y), ...]): a curve through points, x ascending 0 .. 1 (a Blender curve). */
static PyObject *ease_points(PyObject *cls, PyObject *pts)
{
  PyObject *seq = PySequence_Fast(pts, "points: a sequence of (x, y)");
  if (!seq) {
    return NULL;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  float(*xy)[2] = n >= 2 ? PyMem_Malloc(sizeof(float[2]) * (size_t)n) : NULL;
  int ok = xy != NULL;
  if (n < 2) {
    PyErr_SetString(PyExc_ValueError, "points: at least two (x, y)");
  }
  for (Py_ssize_t i = 0; ok && i < n; i++) {
    ok = floats_read(PySequence_Fast_GET_ITEM(seq, i), xy[i], 2, "points: each one (x, y)");
  }
  BKEase e;
  if (ok && !BK_ease_points((const float(*)[2])xy, (int)n, &e)) {
    PyErr_SetString(PyExc_ValueError, "points: x must go from 0 to 1, ascending");
    ok = 0;
  }
  PyMem_Free(xy);
  Py_DECREF(seq);
  return ok ? ease_wrap(&e) : NULL;
}

/* Ease.path("M0,0 C0.2,0.8 0.4,1 1,1"): GSAP's CustomEase. */
static PyObject *ease_path(PyObject *cls, PyObject *arg)
{
  const char *s = PyUnicode_AsUTF8(arg);
  BKEase e;
  if (!s) {
    return NULL;
  }
  if (!BK_ease_path(s, &e)) {
    PyErr_Format(PyExc_ValueError, "not an SVG path ease: '%s'", s);
    return NULL;
  }
  return ease_wrap(&e);
}

/* ease.register(name): the ease by that name from now on (gsap.register_ease). */
static PyObject *ease_register(EaseObject *self, PyObject *arg)
{
  const char *s = PyUnicode_AsUTF8(arg);
  if (!s) {
    return NULL;
  }
  if (!BK_ease_register(s, &self->e)) {
    PyErr_Format(PyExc_ValueError, "can't register an ease as '%s'", s);
    return NULL;
  }
  return Py_NewRef(self);
}

static PyMethodDef ease_methods[] = {
    {"points", (PyCFunction)ease_points, METH_O | METH_CLASS,
     "Ease.points([(x, y), ...]): a curve through points (x 0 .. 1, ascending)."},
    {"path", (PyCFunction)ease_path, METH_O | METH_CLASS,
     "Ease.path(svg): GSAP's CustomEase, an SVG path from (0, 0) to (1, 1)."},
    {"register", (PyCFunction)ease_register, METH_O, "register(name): name this ease."},
    {NULL, NULL, 0, NULL},
};

static PyTypeObject Ease_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "buipy.Ease",
    .tp_basicsize = sizeof(EaseObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Ease(spec): an ease read once: a GSAP, Motion, CSS or Blender name (\"power2.out\", "
              "\"easeInOut\", \"cubic-bezier(...)\", \"bezier\"), or [x1, y1, x2, y2]. "
              "ease(t) evaluates it.",
    .tp_new = ease_new,
    .tp_call = (ternaryfunc)ease_call,
    .tp_methods = ease_methods,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Python objects as targets
 *
 * The object itself is the engine's target (so a second motion on the same attribute takes it
 * over, as on a node); an attribute's name is looked up by property id, the spelling Python used.
 * \{ */

/* The attribute names Python used for one target's properties. `scale_x` and `scaleX` are one
 * property to the engine, so the name is kept per (target, property): two objects animating
 * different spellings each write their own. An entry lives as long as the tweens on its target
 * (retain / release count them); the table is open addressing by the target's address. */
typedef struct ObjNames {
  PyObject *obj; /* NULL: empty, DELETED: removed */
  int refs, n, cap;
  int *props;
  PyObject **names;
} ObjNames;

#define DELETED ((PyObject *)1)

static ObjNames *g_objs;
static int g_nobjs, g_capobjs;

static size_t objs_hash(const PyObject *obj)
{
  const uintptr_t h = (uintptr_t)obj * 0x9E3779B97F4A7C15ull;
  return (size_t)(h >> 20);
}

static ObjNames *objs_find(const PyObject *obj)
{
  if (!g_capobjs) {
    return NULL;
  }
  for (size_t i = objs_hash(obj) & (size_t)(g_capobjs - 1), k = 0; k < (size_t)g_capobjs;
       i = (i + 1) & (size_t)(g_capobjs - 1), k++)
  {
    ObjNames *e = &g_objs[i];
    if (!e->obj) {
      return NULL;
    }
    if (e->obj == obj) {
      return e;
    }
  }
  return NULL;
}

static int objs_grow(void)
{
  ObjNames *old = g_objs;
  const int old_cap = g_capobjs;
  const int cap = old_cap ? old_cap * 2 : 16;
  ObjNames *grown = PyMem_Calloc((size_t)cap, sizeof(ObjNames));
  if (!grown) {
    PyErr_NoMemory();
    return 0;
  }
  g_objs = grown;
  g_capobjs = cap;
  g_nobjs = 0;
  for (int i = 0; i < old_cap; i++) {
    if (old[i].obj && old[i].obj != DELETED) {
      for (size_t k = objs_hash(old[i].obj) & (size_t)(cap - 1);; k = (k + 1) & (size_t)(cap - 1)) {
        if (!g_objs[k].obj) {
          g_objs[k] = old[i];
          g_nobjs++;
          break;
        }
      }
    }
  }
  PyMem_Free(old);
  return 1;
}

/* the entry for `obj`, made when there is none */
static ObjNames *objs_entry(PyObject *obj)
{
  ObjNames *e = objs_find(obj);
  if (e) {
    return e;
  }
  if ((g_nobjs + 1) * 2 > g_capobjs && !objs_grow()) {
    return NULL;
  }
  for (size_t i = objs_hash(obj) & (size_t)(g_capobjs - 1);; i = (i + 1) & (size_t)(g_capobjs - 1)) {
    if (!g_objs[i].obj || g_objs[i].obj == DELETED) {
      g_objs[i] = (ObjNames){.obj = obj};
      g_nobjs++;
      return &g_objs[i];
    }
  }
}

static int name_set(PyObject *obj, int prop, PyObject *name)
{
  ObjNames *e = objs_entry(obj);
  if (!e) {
    return 0;
  }
  for (int i = 0; i < e->n; i++) {
    if (e->props[i] == prop) {
      Py_XSETREF(e->names[i], Py_NewRef(name));
      return 1;
    }
  }
  if (e->n == e->cap) {
    const int cap = e->cap ? e->cap * 2 : 4;
    int *props = PyMem_Realloc(e->props, sizeof(int) * (size_t)cap);
    if (props) {
      e->props = props;
    }
    PyObject **names = props ? PyMem_Realloc(e->names, sizeof(PyObject *) * (size_t)cap) : NULL;
    if (!names) {
      PyErr_NoMemory();
      return 0;
    }
    e->names = names;
    e->cap = cap;
  }
  e->props[e->n] = prop;
  e->names[e->n++] = Py_NewRef(name);
  return 1;
}

/* the attribute `obj`'s property `prop` is, as Python spelled it (borrowed) */
static PyObject *name_of(const PyObject *obj, int prop)
{
  const ObjNames *e = objs_find(obj);
  for (int i = 0; e && i < e->n; i++) {
    if (e->props[i] == prop) {
      return e->names[i];
    }
  }
  return NULL;
}

/* a number, or a sequence of up to 4 */
static int numbers_of(PyObject *v, float r[4])
{
  if (PyFloat_Check(v) || PyLong_Check(v)) {
    r[0] = (float)PyFloat_AsDouble(v);
    return 1;
  }
  PyObject *seq = PySequence_Fast(v, "not a number");
  if (!seq) {
    return 0;
  }
  const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
  for (Py_ssize_t i = 0; i < n && i < 4; i++) {
    r[i] = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, i));
  }
  Py_DECREF(seq);
  return n >= 1 && n <= 4 && !PyErr_Occurred() ? (int)n : 0;
}

static int pyobj_get(void *obj, int prop, float r_value[4], int *r_unit)
{
  GIL_ENTER;
  PyObject *name = name_of((PyObject *)obj, prop);
  if (!name) {
    GIL_LEAVE;
    return 0;
  }
  PyObject *v = PyObject_GetAttr((PyObject *)obj, name);
  const int n = v ? numbers_of(v, r_value) : 0;
  Py_XDECREF(v);
  if (!n) {
    buipy_stash_error();
  }
  *r_unit = n == 4 ? BK_UNIT_COLOR : BK_UNIT_NONE;
  GIL_LEAVE;
  return n;
}

static void pyobj_set(void *obj, int prop, const float *value, int n, int unit)
{
  (void)unit;
  GIL_ENTER;
  PyObject *name = name_of((PyObject *)obj, prop);
  if (name) {
    PyObject *v = n == 1 ? PyFloat_FromDouble(value[0]) : PyTuple_New(n);
    for (int i = 0; v && n > 1 && i < n; i++) {
      PyTuple_SET_ITEM(v, i, PyFloat_FromDouble(value[i]));
    }
    if (!v || PyObject_SetAttr((PyObject *)obj, name, v) < 0) {
      buipy_stash_error();
    }
    Py_XDECREF(v);
  }
  GIL_LEAVE;
}

static void pyobj_retain(void *obj)
{
  GIL_ENTER;
  ObjNames *e = objs_find((PyObject *)obj);
  if (e) {
    e->refs++;
  }
  Py_INCREF((PyObject *)obj);
  GIL_LEAVE;
}

static void pyobj_release(void *obj)
{
  GIL_ENTER;
  ObjNames *e = objs_find((PyObject *)obj);
  if (e && --e->refs <= 0) { /* the last tween on it: its names go */
    for (int i = 0; i < e->n; i++) {
      Py_DECREF(e->names[i]);
    }
    PyMem_Free(e->props);
    PyMem_Free(e->names);
    *e = (ObjNames){.obj = DELETED};
  }
  Py_DECREF((PyObject *)obj);
  GIL_LEAVE;
}

static const BKAnimClass PYOBJ = {pyobj_get, pyobj_set, pyobj_retain, pyobj_release};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Options (GSAP's vars, Motion's transition)
 * \{ */

enum {
  O_DURATION, O_DELAY, O_EASE, O_TIMES, O_REPEAT, O_REPEAT_DELAY, O_YOYO, O_TIME_SCALE, O_PAUSED,
  O_REVERSED, O_IMMEDIATE, O_OVERWRITE, O_STAGGER, O_TYPE,
  O_STIFFNESS, O_DAMPING, O_MASS, O_BOUNCE, O_VISUAL_DURATION, O_VELOCITY, O_REST_DELTA, O_REST_SPEED,
  O_POWER, O_TIME_CONSTANT, O_MIN, O_MAX, O_BOUNCE_STIFFNESS, O_BOUNCE_DAMPING,
  O_ON_START, O_ON_UPDATE, O_ON_COMPLETE, O_ON_REPEAT, O_ON_REVERSE_COMPLETE, O_ON_INTERRUPT,
  O__N,
};
#define NEVENTS (O__N - O_ON_START) /* BK_EV_MOTION_START << i, in this order */

static const char *const opt_names[O__N] = {
    "duration", "delay", "ease", "times", "repeat", "repeat_delay", "yoyo", "time_scale", "paused",
    "reversed", "immediate_render", "overwrite", "stagger", "type",
    "stiffness", "damping", "mass", "bounce", "visual_duration", "velocity", "rest_delta", "rest_speed",
    "power", "time_constant", "min", "max", "bounce_stiffness", "bounce_damping",
    "on_start", "on_update", "on_complete", "on_repeat", "on_reverse_complete", "on_interrupt",
};

/* The options given (borrowed from the caller's dict, for the call). */
typedef struct Opts {
  PyObject *o[O__N];
} Opts;

static int opt_find(PyObject *key)
{
  const char *s = PyUnicode_Check(key) ? PyUnicode_AsUTF8(key) : NULL;
  for (int i = 0; s && i < O__N; i++) {
    if (strcmp(s, opt_names[i]) == 0) {
      return i;
    }
  }
  PyErr_Clear();
  return -1;
}

/* `strict`: every key is an option (Motion's transition); otherwise the keys that aren't are
 * properties (GSAP's vars). A None option is left to its default. */
static int opts_read(PyObject *dict, Opts *r, int strict)
{
  memset(r, 0, sizeof(*r));
  if (!dict || dict == Py_None) {
    return 1;
  }
  if (!PyDict_Check(dict)) {
    PyErr_SetString(PyExc_TypeError, "options must be a dict");
    return 0;
  }
  Py_ssize_t pos = 0;
  PyObject *k, *v;
  while (PyDict_Next(dict, &pos, &k, &v)) {
    const int id = opt_find(k);
    if (id < 0) {
      if (strict) {
        PyErr_Format(PyExc_TypeError, "unknown animation option '%S'", k);
        return 0;
      }
      continue;
    }
    if (v == Py_None) {
      continue;
    }
    if (id >= O_ON_START && !PyCallable_Check(v)) {
      PyErr_Format(PyExc_TypeError, "'%s' must be callable", opt_names[id]);
      return 0;
    }
    r->o[id] = v;
  }
  return 1;
}

/* 1: given (in *r), 0: not given, -1: not a number (raised) */
static int opt_num(const Opts *o, int id, double *r)
{
  PyObject *v = o->o[id];
  if (!v) {
    return 0;
  }
  const double d = PyFloat_AsDouble(v);
  if (d == -1.0 && PyErr_Occurred()) {
    PyErr_Format(PyExc_TypeError, "'%s' must be a number", opt_names[id]);
    return -1;
  }
  *r = d;
  return 1;
}

/* the same, into a float field */
static int opt_float(const Opts *o, int id, float *field)
{
  double d;
  const int got = opt_num(o, id, &d);
  if (got > 0) {
    *field = (float)d;
  }
  return got >= 0;
}

static int opt_flag(const Opts *o, int id, int *field)
{
  if (o->o[id]) {
    const int t = PyObject_IsTrue(o->o[id]);
    if (t < 0) {
      return 0;
    }
    *field = t;
  }
  return 1;
}

/* Motion's "ease": a list of eases is one per keyframe segment */
static int ease_is_list(PyObject *v)
{
  if (!v || !PyList_Check(v) || PyList_GET_SIZE(v) == 0) {
    return 0;
  }
  PyObject *first = PyList_GET_ITEM(v, 0);
  return PyUnicode_Check(first) || PyObject_TypeCheck(first, &Ease_Type);
}

static int is_physical(int p)
{
  return p == BK_PROP_X || p == BK_PROP_Y || p == BK_PROP_Z || p == BK_PROP_ROTATE ||
         p == BK_PROP_ROTATE_X || p == BK_PROP_ROTATE_Y || p == BK_PROP_SCALE ||
         p == BK_PROP_SCALE_X || p == BK_PROP_SCALE_Y;
}

static int is_scale(int p)
{
  return p == BK_PROP_SCALE || p == BK_PROP_SCALE_X || p == BK_PROP_SCALE_Y;
}

/* Motion's three spring forms: bounce + visual_duration, duration (+ bounce), or stiffness,
 * damping and mass; Motion's own defaults when none is given (bindings/rust motion/vars.rs). */
static int spring_read(const Opts *o, int flavor, int prop, float to, BKSpring *r)
{
  float bounce = 0.25f, vd = 0.0f, velocity = 0.0f, mass = 1.0f;
  double dur;
  const int has_dur = opt_num(o, O_DURATION, &dur);
  if (has_dur < 0 || !opt_float(o, O_BOUNCE, &bounce) || !opt_float(o, O_VISUAL_DURATION, &vd) ||
      !opt_float(o, O_VELOCITY, &velocity) || !opt_float(o, O_MASS, &mass))
  {
    return 0;
  }
  if (o->o[O_VISUAL_DURATION]) {
    *r = BK_spring_bounce(bounce, vd);
  }
  else if (has_dur && !o->o[O_STIFFNESS]) {
    *r = BK_spring_duration((float)dur, bounce, velocity, mass);
  }
  else {
    *r = BK_spring_default();
    if (flavor == FLAVOR_MOTION && !o->o[O_STIFFNESS] && !o->o[O_DAMPING]) {
      const int scale = is_scale(prop);
      r->stiffness = scale ? 550.0f : 500.0f;
      r->damping = !scale ? 25.0f : to == 0.0f ? 2.0f * sqrtf(550.0f) : 30.0f;
      r->rest_speed = 10.0f;
    }
  }
  return opt_float(o, O_STIFFNESS, &r->stiffness) && opt_float(o, O_DAMPING, &r->damping) &&
         opt_float(o, O_MASS, &r->mass) && opt_float(o, O_VELOCITY, &r->velocity) &&
         opt_float(o, O_REST_DELTA, &r->rest_delta) && opt_float(o, O_REST_SPEED, &r->rest_speed);
}

static int inertia_read(const Opts *o, BKInertia *r)
{
  float velocity = 0.0f;
  if (!opt_float(o, O_VELOCITY, &velocity)) {
    return 0;
  }
  *r = BK_inertia_default(velocity);
  return opt_float(o, O_POWER, &r->power) && opt_float(o, O_TIME_CONSTANT, &r->time_constant) &&
         opt_float(o, O_MIN, &r->min) && opt_float(o, O_MAX, &r->max) &&
         opt_float(o, O_BOUNCE_STIFFNESS, &r->bounce_stiffness) &&
         opt_float(o, O_BOUNCE_DAMPING, &r->bounce_damping) && opt_float(o, O_REST_DELTA, &r->rest_delta);
}

/* The engine's options with `flavor`'s defaults, for a motion whose first property is `prop`
 * with `nkeys` keyframes ending at `to` (GSAP: 0.5 s power1.out; Motion: springs for transforms,
 * 0.3 s eased tweens otherwise, 0.8 s keyframes). Callbacks are attached later (events_attach). */
static int vars_read(const Opts *o, int flavor, int prop, int nkeys, float to, BKMotionVars *v)
{
  *v = BK_motion_vars_default();
  int kind = -1;
  if (o->o[O_TYPE]) {
    const char *t = PyUnicode_Check(o->o[O_TYPE]) ? PyUnicode_AsUTF8(o->o[O_TYPE]) : NULL;
    kind = !t                                                        ? -2 :
           strcmp(t, "tween") == 0 || strcmp(t, "keyframes") == 0   ? BK_MOTION_TWEEN_KIND :
           strcmp(t, "spring") == 0                                  ? BK_MOTION_SPRING_KIND :
           strcmp(t, "inertia") == 0 || strcmp(t, "decay") == 0     ? BK_MOTION_INERTIA_KIND :
                                                                       -2;
    if (kind == -2) {
      PyErr_Format(PyExc_ValueError, "'type' is \"tween\", \"keyframes\", \"spring\" or \"inertia\", not %R",
                   o->o[O_TYPE]);
      return 0;
    }
  }
  const int segment_eases = ease_is_list(o->o[O_EASE]);
  const int has_ease = o->o[O_EASE] && !segment_eases;
  if (kind < 0) {
    kind = flavor == FLAVOR_MOTION && nkeys <= 1 && is_physical(prop) && !o->o[O_DURATION] && !has_ease ?
               BK_MOTION_SPRING_KIND :
               BK_MOTION_TWEEN_KIND;
  }
  if (flavor == FLAVOR_MOTION) { /* Motion's getDefaultTransition, and its keyframes */
    v->duration = nkeys > 1 ? 0.8 : 0.3;
    if (!o->o[O_TYPE] && !o->o[O_DURATION] && nkeys <= 1) {
      v->ease = BK_ease_bezier(0.25f, 0.1f, 0.35f, 1.0f);
    }
    else {
      BK_ease_parse("easeInOut", &v->ease);
    }
  }
  double d;
  int got;
  if ((got = opt_num(o, O_DURATION, &d)) < 0) {
    return 0;
  }
  if (got) {
    if (d < 0.0) {
      PyErr_SetString(PyExc_ValueError, "'duration' can't be negative");
      return 0;
    }
    v->duration = d;
  }
  if (has_ease && !ease_read(o->o[O_EASE], &v->ease)) {
    return 0;
  }
  /* a dict for the delay is Motion's stagger(): read with the stagger */
  if (o->o[O_DELAY] && !PyDict_Check(o->o[O_DELAY])) {
    if (opt_num(o, O_DELAY, &v->delay) < 0) {
      return 0;
    }
  }
  if (o->o[O_REPEAT]) {
    const long rep = PyLong_AsLong(o->o[O_REPEAT]);
    if (rep == -1 && PyErr_Occurred()) {
      PyErr_SetString(PyExc_TypeError, "'repeat' must be an int (-1: forever)");
      return 0;
    }
    v->repeat = (int)rep;
  }
  if (opt_num(o, O_REPEAT_DELAY, &v->repeat_delay) < 0 || !opt_flag(o, O_YOYO, &v->yoyo) ||
      !opt_flag(o, O_PAUSED, &v->paused) || !opt_flag(o, O_REVERSED, &v->reversed) ||
      !opt_flag(o, O_IMMEDIATE, &v->immediate))
  {
    return 0;
  }
  if ((got = opt_num(o, O_TIME_SCALE, &d)) < 0) {
    return 0;
  }
  if (got) {
    if (d <= 0.0) {
      PyErr_SetString(PyExc_ValueError, "'time_scale' must be > 0");
      return 0;
    }
    v->time_scale = d;
  }
  if (o->o[O_OVERWRITE]) {
    PyObject *ow = o->o[O_OVERWRITE];
    const char *s = PyUnicode_Check(ow) ? PyUnicode_AsUTF8(ow) : NULL;
    if (s) {
      v->overwrite = strcmp(s, "auto") == 0 ? BK_OVERWRITE_AUTO : strcmp(s, "all") == 0 ? BK_OVERWRITE_ALL : -1;
      if (v->overwrite < 0) {
        PyErr_Format(PyExc_ValueError, "'overwrite' is True, False, \"auto\" or \"all\", not '%s'", s);
        return 0;
      }
    }
    else {
      const int t = PyObject_IsTrue(ow);
      if (t < 0) {
        return 0;
      }
      v->overwrite = t ? BK_OVERWRITE_AUTO : BK_OVERWRITE_NONE;
    }
  }
  v->kind = kind;
  if (kind == BK_MOTION_SPRING_KIND) {
    return spring_read(o, flavor, prop, to, &v->spring);
  }
  if (kind == BK_MOTION_INERTIA_KIND) {
    return inertia_read(o, &v->inertia);
  }
  return 1;
}

/* GSAP's stagger (a number: each; a dict: each, amount, from, grid, axis, ease), or Motion's
 * delay=stagger(...) (a dict). 0 when there is none; -1 on an error. `from` stays borrowed. */
static int stagger_read(const Opts *o, BKStagger *s)
{
  memset(s, 0, sizeof(*s));
  PyObject *v = o->o[O_STAGGER] ? o->o[O_STAGGER] : o->o[O_DELAY] && PyDict_Check(o->o[O_DELAY]) ? o->o[O_DELAY] : NULL;
  if (!v) {
    return 0;
  }
  if (!PyDict_Check(v)) {
    s->each = PyFloat_AsDouble(v);
    if (PyErr_Occurred()) {
      PyErr_SetString(PyExc_TypeError, "'stagger' is a number (seconds each) or a dict");
      return -1;
    }
    return 1;
  }
  Py_ssize_t pos = 0;
  PyObject *k, *x;
  while (PyDict_Next(v, &pos, &k, &x)) {
    const char *name = PyUnicode_Check(k) ? PyUnicode_AsUTF8(k) : "";
    if (strcmp(name, "each") == 0 || strcmp(name, "amount") == 0) {
      const double d = PyFloat_AsDouble(x);
      if (PyErr_Occurred()) {
        return -1;
      }
      *(name[0] == 'e' ? &s->each : &s->amount) = d;
    }
    else if (strcmp(name, "from") == 0 || strcmp(name, "from_") == 0) {
      if (PyLong_Check(x)) {
        s->from = "index";
        s->from_index = (int)PyLong_AsLong(x);
      }
      else if (PyUnicode_Check(x)) {
        s->from = PyUnicode_AsUTF8(x);
        /* Motion's spellings */
        s->from = strcmp(s->from, "first") == 0 ? "start" : strcmp(s->from, "last") == 0 ? "end" : s->from;
        if (strcmp(s->from, "start") && strcmp(s->from, "center") && strcmp(s->from, "end") &&
            strcmp(s->from, "edges") && strcmp(s->from, "random"))
        {
          PyErr_Format(PyExc_ValueError, "stagger 'from' is an index or \"start\", \"center\", \"end\", "
                                         "\"edges\", \"random\", \"first\", \"last\", not '%s'", s->from);
          return -1;
        }
      }
      else {
        PyErr_SetString(PyExc_TypeError, "stagger 'from' is an index or a name");
        return -1;
      }
    }
    else if (strcmp(name, "grid") == 0) {
      float g[2];
      if (!floats_read(x, g, 2, "stagger 'grid': [rows, columns]")) {
        return -1;
      }
      s->grid_rows = (int)g[0];
      s->grid_cols = (int)g[1];
    }
    else if (strcmp(name, "axis") == 0) {
      s->axis = PyUnicode_Check(x) ? PyUnicode_AsUTF8(x) : NULL;
      if (!s->axis || (strcmp(s->axis, "x") && strcmp(s->axis, "y"))) {
        PyErr_SetString(PyExc_ValueError, "stagger 'axis' is \"x\" or \"y\"");
        return -1;
      }
    }
    else if (strcmp(name, "ease") == 0) {
      if (!ease_read(x, &s->ease)) {
        return -1;
      }
    }
    else {
      PyErr_Format(PyExc_TypeError, "unknown stagger option '%S'", k);
      return -1;
    }
  }
  return 1;
}

/* What a motion calls back (owned by the motion: freed with it). */
typedef struct Events {
  PyObject *fn[NEVENTS];
} Events;

static void events_dispatch(BKMotion *m, int ev, void *data)
{
  (void)m;
  Events *e = (Events *)data;
  for (int i = 0; i < NEVENTS; i++) {
    if (ev == (BK_EV_MOTION_START << i) && e->fn[i]) {
      GIL_ENTER;
      PyObject *r = PyObject_CallNoArgs(e->fn[i]);
      if (!r) {
        buipy_stash_error();
      }
      Py_XDECREF(r);
      GIL_LEAVE;
    }
  }
}

static void events_free(void *data)
{
  Events *e = (Events *)data;
  GIL_ENTER;
  for (int i = 0; i < NEVENTS; i++) {
    Py_XDECREF(e->fn[i]);
  }
  GIL_LEAVE;
  PyMem_RawFree(e);
}

/* the callbacks given go to the motion `v` makes (the last thing done before making it) */
static int events_attach(const Opts *o, BKMotionVars *v)
{
  int mask = 0;
  for (int i = 0; i < NEVENTS; i++) {
    mask |= o->o[O_ON_START + i] ? BK_EV_MOTION_START << i : 0;
  }
  if (!mask) {
    return 1;
  }
  Events *e = (Events *)PyMem_RawCalloc(1, sizeof(Events));
  if (!e) {
    PyErr_NoMemory();
    return 0;
  }
  for (int i = 0; i < NEVENTS; i++) {
    e->fn[i] = Py_XNewRef(o->o[O_ON_START + i]);
  }
  v->on = events_dispatch;
  v->events = mask;
  v->data = e;
  v->free_data = events_free;
  return 1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Values and tracks
 * \{ */

typedef struct Key {
  float v[4];
  int n, unit;
  int8_t rel;
  uint8_t current;
} Key;

/* One keyframe: None (where it is when the motion starts), a number, a string ("50%", "+=10",
 * "#f80", "rgb(...)"), or a tuple (a color: 3 or 4 numbers, or 1 .. 2 numbers). */
static int key_read(PyObject *v, PyObject *name, Key *k)
{
  memset(k, 0, sizeof(*k));
  if (v == Py_None) {
    k->current = 1;
    return 1;
  }
  if (PyFloat_Check(v) || PyLong_Check(v)) {
    k->v[0] = (float)PyFloat_AsDouble(v);
    k->n = 1;
    return !PyErr_Occurred();
  }
  if (PyUnicode_Check(v)) {
    const char *s = PyUnicode_AsUTF8(v);
    int rel = 0;
    if (!s) {
      return 0;
    }
    k->n = BK_anim_parse_value(s, k->v, &k->unit, &rel);
    if (!k->n) {
      PyErr_Format(PyExc_ValueError, "'%s' is not a value for '%U' (a number, \"50%%\", \"+=10\", \"#f80\", "
                                     "\"rgb(...)\")", s, name);
      return 0;
    }
    k->rel = (int8_t)rel;
    return 1;
  }
  if (PyTuple_Check(v) && PyTuple_GET_SIZE(v) >= 1 && PyTuple_GET_SIZE(v) <= 4) {
    k->n = (int)PyTuple_GET_SIZE(v);
    for (int i = 0; i < k->n; i++) {
      k->v[i] = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(v, i));
    }
    if (k->n >= 3) { /* a color: rgb(a) */
      k->v[3] = k->n == 4 ? k->v[3] : 1.0f;
      k->n = 4;
      k->unit = BK_UNIT_COLOR;
    }
    return !PyErr_Occurred();
  }
  PyErr_Format(PyExc_TypeError, "'%U' takes a number, a string, a tuple (a color) or a list "
                                "(keyframes), not %.80s", name, Py_TYPE(v)->tp_name);
  return 0;
}

typedef struct TrackPlan {
  int prop, first, nkeys, n, unit, from;
  int given; /* the keyframes given (what the defaults go by) */
  PyObject *name; /* borrowed */
} TrackPlan;

/* The tracks of a call, read once: every target's motion is made from them. */
typedef struct Plan {
  TrackPlan *t;
  int nt, cap_t;
  Key *k;
  int nk, cap_k;
  BKTrack *bt;
  float *values;
  uint8_t *cur;
  int8_t *rel;
  BKEase *eases; /* Motion's per-segment eases */
  int neases;
  float *times;  /* Motion's keyframe times */
  int ntimes;
} Plan;

static void plan_free(Plan *p)
{
  PyMem_Free(p->t);
  PyMem_Free(p->k);
  PyMem_Free(p->bt);
  PyMem_Free(p->values);
  PyMem_Free(p->cur);
  PyMem_Free(p->rel);
  PyMem_Free(p->eases);
  PyMem_Free(p->times);
  memset(p, 0, sizeof(*p));
}

static int grow(void **arr, int *cap, int need, size_t size)
{
  if (need <= *cap) {
    return 1;
  }
  const int ncap = need > *cap * 2 ? need : *cap * 2;
  void *g = PyMem_Realloc(*arr, size * (size_t)ncap);
  if (!g) {
    PyErr_NoMemory();
    return 0;
  }
  *arr = g;
  *cap = ncap;
  return 1;
}

/* A track for property `name`: `value` (a list: keyframes), after `start` when `has_start`
 * (from_to's from value; None: where it is). */
static int plan_track(Plan *p, PyObject *name, int has_start, PyObject *start, PyObject *value, int from)
{
  const char *s = PyUnicode_Check(name) ? PyUnicode_AsUTF8(name) : NULL;
  if (!s) {
    PyErr_SetString(PyExc_TypeError, "property names are strings");
    return 0;
  }
  const int list = PyList_Check(value);
  const Py_ssize_t nv = list ? PyList_GET_SIZE(value) : 1;
  if (nv == 0) {
    PyErr_Format(PyExc_ValueError, "'%s': no keyframes", s);
    return 0;
  }
  int nk = (int)nv + (has_start ? 1 : 0);
  if (!grow((void **)&p->k, &p->cap_k, p->nk + nk + 1, sizeof(Key)) || /* + 1: plan_bind's */
      !grow((void **)&p->t, &p->cap_t, p->nt + 1, sizeof(TrackPlan)))
  {
    return 0;
  }
  Key *k = &p->k[p->nk];
  if (has_start && !key_read(start, name, &k[0])) {
    return 0;
  }
  for (Py_ssize_t i = 0; i < nv; i++) {
    if (!key_read(list ? PyList_GET_ITEM(value, i) : value, name, &k[i + (has_start ? 1 : 0)])) {
      return 0;
    }
  }
  TrackPlan *t = &p->t[p->nt];
  memset(t, 0, sizeof(*t));
  for (int i = 0; i < nk; i++) {
    if (k[i].current) {
      continue;
    }
    if (t->n && k[i].n != t->n) {
      PyErr_Format(PyExc_ValueError, "'%s' mixes values of %d and %d numbers", s, t->n, k[i].n);
      return 0;
    }
    t->n = k[i].n;
    t->unit = k[i].unit;
  }
  if (!t->n) {
    PyErr_Format(PyExc_ValueError, "'%s' needs a value", s);
    return 0;
  }
  t->given = (int)nv;
  if (t->n == 4 && nk == 1) {
    /* a color to (or from) where it is: that key made explicit, for plan_bind */
    if (!from) {
      k[1] = k[0];
    }
    memset(&k[from ? 1 : 0], 0, sizeof(Key));
    k[from ? 1 : 0].current = 1;
    nk = 2;
    from = 0;
  }
  t->prop = BK_anim_prop(s);
  t->name = name;
  t->first = p->nk;
  t->nkeys = nk;
  t->from = from;
  p->nk += nk;
  p->nt++;
  return 1;
}

/* Motion's per-segment eases and keyframe times */
static int plan_curves(Plan *p, const Opts *o)
{
  if (ease_is_list(o->o[O_EASE])) {
    const Py_ssize_t n = PyList_GET_SIZE(o->o[O_EASE]);
    p->eases = PyMem_Malloc(sizeof(BKEase) * (size_t)n);
    if (!p->eases) {
      PyErr_NoMemory();
      return 0;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
      if (!ease_read(PyList_GET_ITEM(o->o[O_EASE], i), &p->eases[i])) {
        return 0;
      }
    }
    p->neases = (int)n;
  }
  if (o->o[O_TIMES]) {
    PyObject *seq = PySequence_Fast(o->o[O_TIMES], "'times' is a list of numbers (0 .. 1)");
    if (!seq) {
      return 0;
    }
    const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    p->times = PyMem_Malloc(sizeof(float) * (size_t)(n ? n : 1));
    for (Py_ssize_t i = 0; p->times && i < n; i++) {
      p->times[i] = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, i));
    }
    Py_DECREF(seq);
    if (!p->times) {
      PyErr_NoMemory();
      return 0;
    }
    if (PyErr_Occurred()) {
      return 0;
    }
    p->ntimes = (int)n;
  }
  return 1;
}

/* the engine's tracks (values packed per track; the plan owns the buffers) */
static int plan_finish(Plan *p)
{
  int nfloat = 0;
  for (int i = 0; i < p->nt; i++) {
    nfloat += p->t[i].nkeys * p->t[i].n;
  }
  p->bt = PyMem_Calloc((size_t)(p->nt ? p->nt : 1), sizeof(BKTrack));
  p->values = PyMem_Calloc((size_t)(nfloat ? nfloat : 1), sizeof(float));
  p->cur = PyMem_Calloc((size_t)(p->nk ? p->nk : 1), 1);
  p->rel = PyMem_Calloc((size_t)(p->nk ? p->nk : 1), 1);
  if (!p->bt || !p->values || !p->cur || !p->rel) {
    PyErr_NoMemory();
    return 0;
  }
  float *fv = p->values;
  for (int i = 0; i < p->nt; i++) {
    const TrackPlan *t = &p->t[i];
    BKTrack *b = &p->bt[i];
    int any_cur = 0, any_rel = 0;
    for (int j = 0; j < t->nkeys; j++) {
      const Key *k = &p->k[t->first + j];
      memcpy(&fv[j * t->n], k->v, sizeof(float) * (size_t)t->n);
      p->cur[t->first + j] = k->current;
      p->rel[t->first + j] = k->rel;
      any_cur |= k->current;
      any_rel |= k->rel != 0;
    }
    /* the times and eases of keyframes go to every track with that many keys (a track of one
     * value has none: it goes from where it is) */
    if (t->given > 1 && ((p->ntimes && p->ntimes != t->nkeys) || (p->neases && p->neases != t->nkeys - 1))) {
      PyErr_Format(PyExc_ValueError, "'%U' has %d keyframes: 'times' needs as many, a list 'ease' one less",
                   t->name, t->nkeys);
      return 0;
    }
    *b = (BKTrack){
        .prop = t->prop,
        .n = t->n,
        .nkeys = t->nkeys,
        .values = fv,
        .current = any_cur ? &p->cur[t->first] : NULL,
        .rel = any_rel ? &p->rel[t->first] : NULL,
        .times = t->given > 1 && p->ntimes ? p->times : NULL,
        .eases = t->given > 1 && p->neases ? p->eases : NULL,
        .unit = t->unit,
        .from = t->from,
    };
    fv += t->nkeys * t->n;
  }
  return 1;
}

/* The tracks of `values` (GSAP's vars when `opts` is them: its option keys skipped), for `mode`;
 * from_to's from values come from `from_values`. */
static int plan_read(Plan *p, PyObject *values, int mixed, PyObject *from_values, int mode, const Opts *o)
{
  if (!PyDict_Check(values)) {
    PyErr_SetString(PyExc_TypeError, "the values to animate must be a dict");
    return 0;
  }
  if (from_values != Py_None && !PyDict_Check(from_values)) {
    PyErr_SetString(PyExc_TypeError, "the from values must be a dict");
    return 0;
  }
  Py_ssize_t pos = 0;
  PyObject *k, *v;
  while (PyDict_Next(values, &pos, &k, &v)) {
    if (mixed && opt_find(k) >= 0) {
      continue;
    }
    if (mode == MODE_FROM_TO) {
      PyObject *start = PyDict_Check(from_values) ? PyDict_GetItemWithError(from_values, k) : NULL;
      if (!start && PyErr_Occurred()) {
        return 0;
      }
      if (!plan_track(p, k, 1, start ? start : Py_None, v, 0)) {
        return 0;
      }
    }
    else if (!plan_track(p, k, 0, NULL, v, mode == MODE_FROM)) {
      return 0;
    }
  }
  /* from_to: what only the from values have goes from there to where it is (gsap.from) */
  pos = 0;
  while (mode == MODE_FROM_TO && PyDict_Check(from_values) && PyDict_Next(from_values, &pos, &k, &v)) {
    const int in_to = PyDict_Contains(values, k);
    if (in_to < 0 || (!in_to && !plan_track(p, k, 0, NULL, v, 1))) {
      return 0;
    }
  }
  if (!p->nt) {
    PyErr_SetString(PyExc_ValueError, "nothing to animate: no properties given");
    return 0;
  }
  return plan_curves(p, o) && plan_finish(p);
}

/* A kit node's color "where it is" is read now: the kit keeps a node's number and color
 * "value" apart and reads a color only when asked for one (BK_anim_node reads numbers), so
 * a color track's current keys are this node's color (for a Python object: the engine reads
 * the attribute when the motion starts). */
static void plan_bind(Plan *p, BKNode *node)
{
  for (int i = 0; i < p->nt; i++) {
    const TrackPlan *t = &p->t[i];
    if (t->n != 4) {
      continue;
    }
    float *vals = (float *)p->bt[i].values;
    for (int j = 0; j < t->nkeys; j++) {
      const Key *k = &p->k[t->first + j];
      float c[4];
      const int now = node && k->current && BK_get_color(node, BK_anim_prop_name(t->prop), c);
      if (now) {
        memcpy(&vals[j * 4], c, sizeof(c));
      }
      p->cur[t->first + j] = k->current && !now;
    }
  }
}

/* What animates `target`: a kit node, or a Python object's attributes (checked here). */
static int target_read(PyObject *target, const Plan *p, const BKAnimClass **r_cls, void **r_obj)
{
  BKNode *node = buipy_kit_node(target);
  if (node) {
    for (int i = 0; i < p->nt; i++) {
      if (!BK_anim_node_has(node, p->t[i].prop, p->t[i].n == 4 ? 4 : 1)) {
        PyErr_Format(PyExc_ValueError, "a '%s' widget has no %s '%U' to animate (kit widgets animate their "
                                       "values, colors and sizes; they have no transforms or opacity)",
                     BK_node_type(node), p->t[i].n == 4 ? "color" : "number", p->t[i].name);
        return 0;
      }
    }
    *r_cls = &BK_anim_node;
    *r_obj = node;
    return 1;
  }
  PyErr_Clear();
  for (int i = 0; i < p->nt; i++) {
    PyObject *cur = PyObject_GetAttr(target, p->t[i].name);
    if (!cur) {
      return 0; /* AttributeError: nothing by that name */
    }
    Py_DECREF(cur);
    if (!name_set(target, p->t[i].prop, p->t[i].name)) {
      return 0;
    }
  }
  *r_cls = &PYOBJ;
  *r_obj = target;
  return 1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Motion: a tween's or a timeline's handle
 * \{ */

typedef struct MotionObject {
  PyObject_HEAD
  BKMotion *m; /* one hold (NULL: a build without animation) */
  int timeline;
} MotionObject;

static PyTypeObject Motion_Type;

static PyObject *motion_wrap(PyTypeObject *cls, BKMotion *m, int timeline)
{
  MotionObject *o = (MotionObject *)cls->tp_alloc(cls, 0);
  if (!o) {
    BK_motion_release(m);
    return NULL;
  }
  o->m = m;
  o->timeline = timeline;
  return (PyObject *)o;
}

static void motion_dealloc(MotionObject *self)
{
  BKMotion *m = self->m;
  self->m = NULL;
  BK_motion_release(m); /* may free callbacks: they take the GIL, which is held */
  Py_TYPE(self)->tp_free((PyObject *)self);
}

/* self, or the exception a callback raised during the call */
static PyObject *chain(MotionObject *self)
{
  return buipy_restore_error() ? NULL : Py_NewRef(self);
}

#define CONTROL(name, fn) \
  static PyObject *motion_##name(MotionObject *self, PyObject *unused) \
  { \
    (void)unused; \
    fn(self->m); \
    return chain(self); \
  }
CONTROL(play, BK_motion_play)
CONTROL(pause, BK_motion_pause)
CONTROL(resume, BK_motion_resume)
CONTROL(reverse, BK_motion_reverse)
CONTROL(restart, BK_motion_restart)
CONTROL(complete, BK_motion_complete)
CONTROL(kill, BK_motion_kill)
#undef CONTROL

#define QUERY(name, fn, wrap) \
  static PyObject *motion_##name(MotionObject *self, PyObject *unused) \
  { \
    (void)unused; \
    return wrap(fn(self->m)); \
  }
QUERY(duration, BK_motion_duration, PyFloat_FromDouble)
QUERY(total_duration, BK_motion_total_duration, PyFloat_FromDouble)
QUERY(is_active, BK_motion_is_active, PyBool_FromLong)
#undef QUERY

/* GSAP's getter / setter pairs: no argument reads, one sets (and returns the motion) */
#define ACCESSOR(name, get, set, check) \
  static PyObject *motion_##name(MotionObject *self, PyObject *const *args, Py_ssize_t n) \
  { \
    if (n == 0) { \
      return PyFloat_FromDouble(get(self->m)); \
    } \
    const double v = n == 1 ? PyFloat_AsDouble(args[0]) : -1.0; \
    if (n != 1 || (v == -1.0 && PyErr_Occurred())) { \
      PyErr_SetString(PyExc_TypeError, #name "(): one number, or none to read it"); \
      return NULL; \
    } \
    check; \
    set(self->m, v); \
    return chain(self); \
  }
ACCESSOR(progress, BK_motion_progress, BK_motion_set_progress, (void)0)
ACCESSOR(time, BK_motion_time, BK_motion_seek, (void)0)
ACCESSOR(total_time, BK_motion_total_time, BK_motion_seek, (void)0)
ACCESSOR(time_scale, BK_motion_time_scale, BK_motion_set_time_scale,
         if (v <= 0.0) {
           PyErr_SetString(PyExc_ValueError, "time_scale must be > 0");
           return NULL;
         })
#undef ACCESSOR

static double total_progress(const BKMotion *m)
{
  const double total = BK_motion_total_duration(m);
  return total > 0.0 ? BK_motion_total_time(m) / total : 1.0;
}

static PyObject *motion_total_progress(MotionObject *self, PyObject *const *args, Py_ssize_t n)
{
  if (n == 0) {
    return PyFloat_FromDouble(total_progress(self->m));
  }
  const double v = n == 1 ? PyFloat_AsDouble(args[0]) : -1.0;
  if (n != 1 || (v == -1.0 && PyErr_Occurred())) {
    PyErr_SetString(PyExc_TypeError, "total_progress(): one number, or none to read it");
    return NULL;
  }
  BK_motion_set_total_progress(self->m, v);
  return chain(self);
}

static PyObject *motion_seek(MotionObject *self, PyObject *arg)
{
  const double t = PyFloat_AsDouble(arg);
  if (t == -1.0 && PyErr_Occurred()) {
    return NULL;
  }
  BK_motion_seek(self->m, t);
  return chain(self);
}

/* paused() / reversed(): read; paused(True) pauses, reversed(True) plays backwards */
static PyObject *motion_flag(MotionObject *self, PyObject *const *args, Py_ssize_t n, int (*get)(const BKMotion *),
                             void (*on)(BKMotion *), void (*off)(BKMotion *))
{
  if (n == 0) {
    return PyBool_FromLong(get(self->m));
  }
  const int want = n == 1 ? PyObject_IsTrue(args[0]) : -1;
  if (want < 0) {
    if (!PyErr_Occurred()) {
      PyErr_SetString(PyExc_TypeError, "one bool, or none to read it");
    }
    return NULL;
  }
  if (want != get(self->m)) {
    (want ? on : off)(self->m);
  }
  return chain(self);
}

static PyObject *motion_paused(MotionObject *self, PyObject *const *args, Py_ssize_t n)
{
  return motion_flag(self, args, n, BK_motion_paused, BK_motion_pause, BK_motion_resume);
}

static PyObject *motion_reversed(MotionObject *self, PyObject *const *args, Py_ssize_t n)
{
  return motion_flag(self, args, n, BK_motion_reversed, BK_motion_reverse, BK_motion_reverse);
}

/* GSAP's position parameter: a string ("<", "+=0.5", "label") or seconds */
static const char *position_read(PyObject *pos, char buf[64])
{
  if (!pos || pos == Py_None) {
    return "";
  }
  if (PyUnicode_Check(pos)) {
    return PyUnicode_AsUTF8(pos);
  }
  const double t = PyFloat_AsDouble(pos);
  if (t == -1.0 && PyErr_Occurred()) {
    PyErr_SetString(PyExc_TypeError, "a position is a string (\"<\", \"+=1\", \"label\") or seconds");
    return NULL;
  }
  snprintf(buf, 64, "%.17g", t);
  return buf;
}

static int is_timeline(MotionObject *self)
{
  if (!self->timeline) {
    PyErr_SetString(PyExc_TypeError, "only a timeline has children, labels and calls");
  }
  return self->timeline;
}

static PyObject *motion_add(MotionObject *self, PyObject *args, PyObject *kw)
{
  static char *kwlist[] = {"child", "position", NULL};
  PyObject *child, *pos = NULL;
  char buf[64];
  if (!PyArg_ParseTupleAndKeywords(args, kw, "O!|O:add", kwlist, &Motion_Type, &child, &pos) ||
      !is_timeline(self))
  {
    return NULL;
  }
  const char *p = position_read(pos, buf);
  if (!p) {
    return NULL;
  }
  if (child == (PyObject *)self) {
    PyErr_SetString(PyExc_ValueError, "a timeline can't hold itself");
    return NULL;
  }
  BK_timeline_add(self->m, ((MotionObject *)child)->m, p);
  return chain(self);
}

static PyObject *motion_add_label(MotionObject *self, PyObject *args, PyObject *kw)
{
  static char *kwlist[] = {"name", "position", NULL};
  const char *name;
  PyObject *pos = NULL;
  char buf[64];
  if (!PyArg_ParseTupleAndKeywords(args, kw, "s|O:add_label", kwlist, &name, &pos) || !is_timeline(self)) {
    return NULL;
  }
  const char *p = position_read(pos, buf);
  if (!p) {
    return NULL;
  }
  BK_timeline_label(self->m, name, p);
  return Py_NewRef(self);
}

static PyObject *motion_label_time(MotionObject *self, PyObject *arg)
{
  const char *name = PyUnicode_AsUTF8(arg);
  if (!name || !is_timeline(self)) {
    return NULL;
  }
  const double t = BK_timeline_label_time(self->m, name);
  if (t < 0.0) {
    Py_RETURN_NONE;
  }
  return PyFloat_FromDouble(t);
}

/* a timeline's call: data is (fn, args), owned by the call */
static void call_dispatch(BKMotion *m, int ev, void *data)
{
  (void)m;
  (void)ev;
  GIL_ENTER;
  PyObject *r = PyObject_Call(PyTuple_GET_ITEM((PyObject *)data, 0), PyTuple_GET_ITEM((PyObject *)data, 1), NULL);
  if (!r) {
    buipy_stash_error();
  }
  Py_XDECREF(r);
  GIL_LEAVE;
}

static void call_free(void *data)
{
  GIL_ENTER;
  Py_DECREF((PyObject *)data);
  GIL_LEAVE;
}

/* call(fn, *args, position=""): fn(*args) when the playhead passes `position` */
static PyObject *motion_call(MotionObject *self, PyObject *args, PyObject *kw)
{
  PyObject *pos = kw ? PyDict_GetItemString(kw, "position") : NULL;
  if (kw && PyDict_GET_SIZE(kw) > (pos ? 1 : 0)) {
    PyErr_SetString(PyExc_TypeError, "call(fn, *args, position=\"\")");
    return NULL;
  }
  if (PyTuple_GET_SIZE(args) < 1 || !PyCallable_Check(PyTuple_GET_ITEM(args, 0))) {
    PyErr_SetString(PyExc_TypeError, "call(fn, *args, position=\"\"): fn must be callable");
    return NULL;
  }
  if (!is_timeline(self)) {
    return NULL;
  }
  char buf[64];
  const char *p = position_read(pos, buf);
  PyObject *rest = p ? PyTuple_GetSlice(args, 1, PyTuple_GET_SIZE(args)) : NULL;
  PyObject *data = rest ? PyTuple_Pack(2, PyTuple_GET_ITEM(args, 0), rest) : NULL;
  Py_XDECREF(rest);
  if (!data) {
    return NULL;
  }
  BK_timeline_call(self->m, call_dispatch, data, call_free, p); /* the call owns `data` */
  return chain(self);
}

static PyMethodDef motion_methods[] = {
    {"play", (PyCFunction)motion_play, METH_NOARGS, "Play forward from where it is."},
    {"pause", (PyCFunction)motion_pause, METH_NOARGS, "Pause where it is."},
    {"resume", (PyCFunction)motion_resume, METH_NOARGS, "Go on the way it was going."},
    {"reverse", (PyCFunction)motion_reverse, METH_NOARGS, "Turn around (and play)."},
    {"restart", (PyCFunction)motion_restart, METH_NOARGS, "From the start."},
    {"complete", (PyCFunction)motion_complete, METH_NOARGS, "To its end now."},
    {"kill", (PyCFunction)motion_kill, METH_NOARGS, "Stop it where it is, for good."},
    {"seek", (PyCFunction)motion_seek, METH_O, "seek(seconds): jump there."},
    {"progress", (PyCFunction)(void (*)(void))motion_progress, METH_FASTCALL,
     "progress() -> 0 .. 1 of an iteration; progress(p) jumps there."},
    {"total_progress", (PyCFunction)(void (*)(void))motion_total_progress, METH_FASTCALL,
     "total_progress() -> 0 .. 1 with repeats; total_progress(p) jumps there."},
    {"time", (PyCFunction)(void (*)(void))motion_time, METH_FASTCALL,
     "time() -> seconds into the iteration; time(t) jumps there."},
    {"total_time", (PyCFunction)(void (*)(void))motion_total_time, METH_FASTCALL,
     "total_time() -> seconds with repeats; total_time(t) jumps there."},
    {"time_scale", (PyCFunction)(void (*)(void))motion_time_scale, METH_FASTCALL,
     "time_scale() -> its speed; time_scale(s) sets it (> 0)."},
    {"paused", (PyCFunction)(void (*)(void))motion_paused, METH_FASTCALL,
     "paused() -> bool; paused(True / False) pauses / resumes."},
    {"reversed", (PyCFunction)(void (*)(void))motion_reversed, METH_FASTCALL,
     "reversed() -> bool; reversed(True / False) sets the direction."},
    {"duration", (PyCFunction)motion_duration, METH_NOARGS, "Seconds of one iteration."},
    {"total_duration", (PyCFunction)motion_total_duration, METH_NOARGS, "Seconds with repeats."},
    {"is_active", (PyCFunction)motion_is_active, METH_NOARGS, "Playing now (not paused, not done)."},
    {"add", (PyCFunction)(void (*)(void))motion_add, METH_VARARGS | METH_KEYWORDS,
     "Timeline: add(child, position=\"\"): place a tween or timeline (GSAP's position)."},
    {"add_label", (PyCFunction)(void (*)(void))motion_add_label, METH_VARARGS | METH_KEYWORDS,
     "Timeline: add_label(name, position=\"\")."},
    {"call", (PyCFunction)(void (*)(void))motion_call, METH_VARARGS | METH_KEYWORDS,
     "Timeline: call(fn, *args, position=\"\"): fn(*args) when the playhead passes it."},
    {"label_time", (PyCFunction)motion_label_time, METH_O, "Timeline: a label's time, or None."},
    {NULL, NULL, 0, NULL},
};

static PyTypeObject Motion_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "buipy.Motion",
    .tp_basicsize = sizeof(MotionObject),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc = "A tween or timeline of the animation engine, with GSAP's controls. Made by "
              "motion_tween / motion_timeline (bui.gsap, bui.motion); it runs on whether or not "
              "this handle is kept.",
    .tp_dealloc = (destructor)motion_dealloc,
    .tp_methods = motion_methods,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Making motions
 * \{ */

static int motion_class(PyObject *cls)
{
  if (!PyType_Check(cls) || !PyType_IsSubtype((PyTypeObject *)cls, &Motion_Type)) {
    PyErr_SetString(PyExc_TypeError, "cls must be buipy.Motion or a subclass");
    return 0;
  }
  return 1;
}

static int available(void)
{
  if (!BK_anim_available()) {
    PyErr_SetString(PyExc_RuntimeError, "this build has no animation (feature 'anim', BUI_ANIM)");
    return 0;
  }
  return 1;
}

/* motion_tween(targets, values, options=None, from_values=None, mode=0, flavor=0, cls=Motion) */
static PyObject *motion_tween(PyObject *mod, PyObject *args, PyObject *kw)
{
  static char *kwlist[] = {"targets", "values", "options", "from_values", "mode", "flavor", "cls", NULL};
  PyObject *targets, *values, *options = Py_None, *from_values = Py_None, *cls = (PyObject *)&Motion_Type;
  int mode = MODE_TO, flavor = FLAVOR_GSAP;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|OOiiO:motion_tween", kwlist, &targets, &values, &options,
                                   &from_values, &mode, &flavor, &cls) ||
      !motion_class(cls) || !available())
  {
    return NULL;
  }
  if (mode < MODE_TO || mode > MODE_SET || flavor < FLAVOR_GSAP || flavor > FLAVOR_MOTION) {
    PyErr_SetString(PyExc_ValueError, "motion_tween: unknown mode or flavor");
    return NULL;
  }
  const int mixed = options == Py_None;
  Opts o;
  Plan p = {0};
  BKStagger st;
  PyObject *seq = NULL;
  PyObject *result = NULL;
  const BKAnimClass **cls_of = NULL;
  void **obj_of = NULL;
  double *offs = NULL;
  if (!opts_read(mixed ? values : options, &o, !mixed) || !plan_read(&p, values, mixed, from_values, mode, &o)) {
    goto done;
  }
  const int stagger = stagger_read(&o, &st);
  if (stagger < 0) {
    goto done;
  }
  /* targets: one, or a list / tuple of them */
  seq = PyList_Check(targets) || PyTuple_Check(targets) ? PySequence_Fast(targets, "") : PyTuple_Pack(1, targets);
  if (!seq) {
    goto done;
  }
  const Py_ssize_t count = PySequence_Fast_GET_SIZE(seq);
  if (count == 0) {
    PyErr_SetString(PyExc_ValueError, "no targets to animate");
    goto done;
  }
  cls_of = PyMem_Calloc((size_t)count, sizeof(*cls_of));
  obj_of = PyMem_Calloc((size_t)count, sizeof(*obj_of));
  offs = PyMem_Calloc((size_t)count, sizeof(*offs));
  if (!cls_of || !obj_of || !offs) {
    PyErr_NoMemory();
    goto done;
  }
  for (Py_ssize_t i = 0; i < count; i++) {
    if (!target_read(PySequence_Fast_GET_ITEM(seq, i), &p, &cls_of[i], &obj_of[i])) {
      goto done;
    }
  }
  /* the defaults follow the keyframes given (from_to's start is not one: Motion's initial ->
   * animate is a two-value transition, not keyframes) */
  const TrackPlan *t0 = &p.t[0];
  BKMotionVars v;
  if (!vars_read(&o, flavor, t0->prop, t0->given, p.k[t0->first + t0->nkeys - 1].v[0], &v)) {
    goto done;
  }
  if (mode == MODE_SET) {
    v.duration = 0.0;
    v.kind = BK_MOTION_TWEEN_KIND;
  }
  if ((mode == MODE_FROM || mode == MODE_FROM_TO) && !o.o[O_IMMEDIATE]) {
    v.immediate = 1; /* GSAP's immediateRender */
  }
  BKMotion *m;
  int timeline = 0;
  if (count == 1) {
    if (!events_attach(&o, &v)) {
      goto done;
    }
    plan_bind(&p, cls_of[0] == &BK_anim_node ? (BKNode *)obj_of[0] : NULL);
    m = BK_tween(cls_of[0], obj_of[0], p.bt, p.nt, &v);
  }
  else {
    /* several: a timeline with each target's tween at its stagger offset (the delay, pausing and
     * the callbacks are the timeline's) */
    if (stagger) {
      BK_stagger(&st, (int)count, offs);
    }
    BKMotionVars tv = BK_motion_vars_default();
    tv.delay = v.delay;
    tv.paused = v.paused;
    v.delay = 0.0;
    v.paused = 0;
    if (!events_attach(&o, &tv)) {
      goto done;
    }
    m = BK_timeline(&tv);
    timeline = 1;
    for (Py_ssize_t i = 0; i < count; i++) {
      char at[64];
      snprintf(at, sizeof(at), "%.17g", offs[i]);
      plan_bind(&p, cls_of[i] == &BK_anim_node ? (BKNode *)obj_of[i] : NULL);
      BKMotion *c = BK_tween(cls_of[i], obj_of[i], p.bt, p.nt, &v);
      BK_timeline_add(m, c, at);
      BK_motion_release(c);
    }
  }
  result = motion_wrap((PyTypeObject *)cls, m, timeline);
  if (result && buipy_restore_error()) { /* a setter or callback raised while rendering at once */
    Py_CLEAR(result);
  }
done:
  PyMem_Free((void *)cls_of);
  PyMem_Free(obj_of);
  PyMem_Free(offs);
  Py_XDECREF(seq);
  plan_free(&p);
  return result;
}

/* motion_timeline(options=None, cls=Motion): delay, repeat, yoyo, paused, time_scale, callbacks */
static PyObject *motion_timeline(PyObject *mod, PyObject *args, PyObject *kw)
{
  static char *kwlist[] = {"options", "cls", NULL};
  PyObject *options = Py_None, *cls = (PyObject *)&Motion_Type;
  Opts o;
  BKMotionVars v;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "|OO:motion_timeline", kwlist, &options, &cls) ||
      !motion_class(cls) || !available() || !opts_read(options, &o, 1) ||
      !vars_read(&o, FLAVOR_GSAP, 0, 1, 0.0f, &v) || !events_attach(&o, &v))
  {
    return NULL;
  }
  return motion_wrap((PyTypeObject *)cls, BK_timeline(&v), 1);
}

/* motion_tick(now): every motion to `now` (seconds, increasing), for hosts with their own clock
 * (motion_set_host_clock(True)) and headless tests. True while something animates. */
static PyObject *motion_tick(PyObject *mod, PyObject *arg)
{
  const double now = PyFloat_AsDouble(arg);
  if (now == -1.0 && PyErr_Occurred()) {
    return NULL;
  }
  const int active = BK_anim_tick(now);
  if (buipy_restore_error()) {
    return NULL;
  }
  return PyBool_FromLong(active);
}

static PyObject *motion_set_host_clock(PyObject *mod, PyObject *arg)
{
  const int on = PyObject_IsTrue(arg);
  if (on < 0) {
    return NULL;
  }
  BK_anim_set_host_clock(on);
  Py_RETURN_NONE;
}

static PyObject *motion_set_lag_smoothing(PyObject *mod, PyObject *args)
{
  double threshold, adjusted = 0.033;
  if (!PyArg_ParseTuple(args, "d|d:motion_set_lag_smoothing", &threshold, &adjusted)) {
    return NULL;
  }
  BK_anim_set_lag_smoothing(threshold, adjusted);
  Py_RETURN_NONE;
}

static PyObject *motion_set_time_scale(PyObject *mod, PyObject *arg)
{
  const double s = PyFloat_AsDouble(arg);
  if (s == -1.0 && PyErr_Occurred()) {
    return NULL;
  }
  if (s <= 0.0) {
    PyErr_SetString(PyExc_ValueError, "the time scale must be > 0");
    return NULL;
  }
  BK_anim_set_time_scale(s);
  Py_RETURN_NONE;
}

/* motion_kill_target(target, props=None): stop the motions of a widget or object (of these
 * properties: a name or a list of names) */
static PyObject *motion_kill_target(PyObject *mod, PyObject *args)
{
  PyObject *target, *props = Py_None;
  if (!PyArg_ParseTuple(args, "O|O:motion_kill_target", &target, &props)) {
    return NULL;
  }
  BKNode *node = buipy_kit_node(target);
  void *obj = node ? (void *)node : (void *)target;
  PyErr_Clear();
  if (props == Py_None) {
    BK_anim_kill_target(obj, -1);
    Py_RETURN_NONE;
  }
  PyObject *seq = PyUnicode_Check(props) ? PyTuple_Pack(1, props) : PySequence_Fast(props, "props: names");
  if (!seq) {
    return NULL;
  }
  for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(seq); i++) {
    const char *s = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(seq, i));
    if (!s) {
      Py_DECREF(seq);
      return NULL;
    }
    BK_anim_kill_target(obj, BK_anim_prop(s));
  }
  Py_DECREF(seq);
  Py_RETURN_NONE;
}

static PyObject *motion_available(PyObject *mod, PyObject *unused)
{
  return PyBool_FromLong(BK_anim_available());
}

static PyMethodDef anim_functions[] = {
    {"motion_tween", (PyCFunction)(void (*)(void))motion_tween, METH_VARARGS | METH_KEYWORDS,
     "motion_tween(targets, values, options=None, from_values=None, mode=0, flavor=0, cls=Motion): "
     "a tween (one target) or a staggered timeline (several). options None: `values` is GSAP's "
     "vars (options and properties in one dict). mode: MOTION_TO, MOTION_FROM, MOTION_FROM_TO, "
     "MOTION_SET; flavor: FLAVOR_GSAP or FLAVOR_MOTION (whose defaults)."},
    {"motion_timeline", (PyCFunction)(void (*)(void))motion_timeline, METH_VARARGS | METH_KEYWORDS,
     "motion_timeline(options=None, cls=Motion): an empty timeline."},
    {"motion_tick", motion_tick, METH_O, "motion_tick(now): advance every motion to `now` seconds."},
    {"motion_set_host_clock", motion_set_host_clock, METH_O,
     "True: motion_tick drives the motions, not the kit's frames."},
    {"motion_set_lag_smoothing", motion_set_lag_smoothing, METH_VARARGS,
     "motion_set_lag_smoothing(threshold, adjusted=0.033): GSAP's lagSmoothing (0: off)."},
    {"motion_set_time_scale", motion_set_time_scale, METH_O, "Every motion's speed."},
    {"motion_kill_target", motion_kill_target, METH_VARARGS,
     "motion_kill_target(target, props=None): stop a target's motions."},
    {"motion_available", motion_available, METH_NOARGS, "True when this build animates."},
    {NULL, NULL, 0, NULL},
};

/** \} */

int buipy_anim_init(PyObject *module)
{
  if (PyType_Ready(&Ease_Type) < 0 || PyType_Ready(&Motion_Type) < 0 ||
      PyModule_AddObjectRef(module, "Ease", (PyObject *)&Ease_Type) < 0 ||
      PyModule_AddObjectRef(module, "Motion", (PyObject *)&Motion_Type) < 0 ||
      PyModule_AddIntConstant(module, "MOTION_TO", MODE_TO) < 0 ||
      PyModule_AddIntConstant(module, "MOTION_FROM", MODE_FROM) < 0 ||
      PyModule_AddIntConstant(module, "MOTION_FROM_TO", MODE_FROM_TO) < 0 ||
      PyModule_AddIntConstant(module, "MOTION_SET", MODE_SET) < 0 ||
      PyModule_AddIntConstant(module, "FLAVOR_GSAP", FLAVOR_GSAP) < 0 ||
      PyModule_AddIntConstant(module, "FLAVOR_MOTION", FLAVOR_MOTION) < 0)
  {
    return -1;
  }
  return PyModule_AddFunctions(module, anim_functions);
}
