/* buipy.Video: the kit's <video> player (BK_video_*, kit/intern/image/video.c),
 * the same one every binding uses. UI thread: the state listener runs inside
 * the window loop's calls (timers, image polls), with the GIL held.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "buipy.h"

typedef struct VideoObject {
  PyObject_HEAD
  BKVideo *v;
  PyObject *on_state; /* callable(state: int) or NULL */
} VideoObject;

static void heard(BKVideo *v, int state, void *data)
{
  (void)v;
  VideoObject *self = (VideoObject *)data;
  const PyGILState_STATE gil = PyGILState_Ensure(); /* window loops may run without it */
  if (self->on_state) {
    PyObject *r = PyObject_CallFunction(self->on_state, "i", state);
    if (!r) {
      PyErr_WriteUnraisable(self->on_state);
    }
    Py_XDECREF(r);
  }
  PyGILState_Release(gil);
}

static int video_init(VideoObject *self, PyObject *args, PyObject *kw)
{
  static char *kwlist[] = {"src", NULL};
  const char *src;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "s:Video", kwlist, &src)) {
    return -1;
  }
  if (self->v) {
    PyErr_SetString(PyExc_RuntimeError, "Video already initialized");
    return -1;
  }
  self->v = BK_video_new(src);
  if (!self->v) {
    PyErr_SetString(PyExc_ValueError, "an empty video path");
    return -1;
  }
  BK_video_set_listener(self->v, heard, self);
  return 0;
}

static void video_dealloc(VideoObject *self)
{
  PyObject_GC_UnTrack(self);
  if (self->v) {
    BK_video_set_listener(self->v, NULL, NULL);
    BK_video_free(self->v);
  }
  Py_CLEAR(self->on_state);
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static int video_traverse(VideoObject *self, visitproc visit, void *arg)
{
  Py_VISIT(self->on_state);
  return 0;
}

static int video_clear(VideoObject *self)
{
  Py_CLEAR(self->on_state);
  return 0;
}

#define LIVE(self) \
  if (!(self)->v) { \
    PyErr_SetString(PyExc_RuntimeError, "Video not initialized"); \
    return NULL; \
  }

static PyObject *video_play(VideoObject *self, PyObject *unused)
{
  (void)unused;
  LIVE(self);
  BK_video_play(self->v);
  Py_RETURN_NONE;
}

static PyObject *video_pause(VideoObject *self, PyObject *unused)
{
  (void)unused;
  LIVE(self);
  BK_video_pause(self->v);
  Py_RETURN_NONE;
}

static PyObject *video_seek(VideoObject *self, PyObject *arg)
{
  LIVE(self);
  const double t = PyFloat_AsDouble(arg);
  if (t == -1.0 && PyErr_Occurred()) {
    return NULL;
  }
  BK_video_seek(self->v, t);
  Py_RETURN_NONE;
}

static PyObject *video_texture(VideoObject *self, PyObject *args)
{
  LIVE(self);
  int tex = 0, max_w = 0, max_h = 0;
  if (!PyArg_ParseTuple(args, "|iii:texture", &tex, &max_w, &max_h)) {
    return NULL;
  }
  return PyLong_FromLong(BK_video_texture(self->v, tex, max_w, max_h));
}

static PyObject *video_attach(VideoObject *self, PyObject *args)
{
  LIVE(self);
  PyObject *node;
  int fit = BK_FIT_CONTAIN;
  if (!PyArg_ParseTuple(args, "O|i:attach", &node, &fit)) {
    return NULL;
  }
  BKNode *n = buipy_kit_node(node);
  if (!n) {
    return NULL;
  }
  BK_node_set_video(n, self->v, fit);
  Py_RETURN_NONE;
}

static PyMethodDef video_methods[] = {
    {"play", (PyCFunction)video_play, METH_NOARGS, "Play (an ended video starts over)."},
    {"pause", (PyCFunction)video_pause, METH_NOARGS, "Pause."},
    {"seek", (PyCFunction)video_seek, METH_O, "seek(seconds): that frame shows, paused or not."},
    {"texture", (PyCFunction)video_texture, METH_VARARGS,
     "texture(tex=0, max_w=0, max_h=0) -> texture id (Dear ImGui): the current frame, updated in "
     "place; calling it each frame keeps the video playing."},
    {"attach", (PyCFunction)video_attach, METH_VARARGS,
     "attach(node, fit=BK_FIT_CONTAIN): show it on a \"video\" or \"image\" node (the node "
     "borrows it: keep the Video alive while the node shows it)."},
    {NULL, NULL, 0, NULL},
};

/* -- properties ---------------------------------------------------------------------- */

static PyObject *get_state(VideoObject *self, void *c)
{
  (void)c;
  LIVE(self);
  return PyLong_FromLong(BK_video_state(self->v));
}

static PyObject *get_time(VideoObject *self, void *c)
{
  (void)c;
  LIVE(self);
  return PyFloat_FromDouble(BK_video_time(self->v));
}

static int set_time(VideoObject *self, PyObject *value, void *c)
{
  (void)c;
  const double t = value ? PyFloat_AsDouble(value) : -1.0;
  if (!self->v || (t == -1.0 && PyErr_Occurred())) {
    return -1;
  }
  BK_video_seek(self->v, t);
  return 0;
}

static PyObject *get_duration(VideoObject *self, void *c)
{
  (void)c;
  LIVE(self);
  return PyFloat_FromDouble(BK_video_duration(self->v));
}

static PyObject *get_size(VideoObject *self, void *c)
{
  (void)c;
  LIVE(self);
  int w, h;
  if (!BK_video_size(self->v, &w, &h)) {
    Py_RETURN_NONE;
  }
  return Py_BuildValue("(ii)", w, h);
}

/* write-only settings: the kit keeps them */
#define SETTER(name, call) \
  static int set_##name(VideoObject *self, PyObject *value, void *c) \
  { \
    (void)c; \
    if (!self->v || !value) { \
      PyErr_SetString(PyExc_AttributeError, "can't delete or set on an uninitialized Video"); \
      return -1; \
    } \
    call; \
    return PyErr_Occurred() ? -1 : 0; \
  }
SETTER(rate, BK_video_set_rate(self->v, PyFloat_AsDouble(value)))
SETTER(loop, BK_video_set_loop(self->v, PyObject_IsTrue(value)))
SETTER(controls, BK_video_set_controls(self->v, PyObject_IsTrue(value)))
SETTER(poster, BK_video_set_poster(self->v, value == Py_None ? NULL : PyUnicode_AsUTF8(value)))

static PyObject *get_on_state(VideoObject *self, void *c)
{
  (void)c;
  return Py_NewRef(self->on_state ? self->on_state : Py_None);
}

static int set_on_state(VideoObject *self, PyObject *value, void *c)
{
  (void)c;
  if (value && value != Py_None && !PyCallable_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "on_state must be callable or None");
    return -1;
  }
  Py_XSETREF(self->on_state, value && value != Py_None ? Py_NewRef(value) : NULL);
  return 0;
}

static PyGetSetDef video_getset[] = {
    {"state", (getter)get_state, NULL, "LOADING 0, PLAYING 1, PAUSED 2, ENDED 3, ERROR 4", NULL},
    {"time", (getter)get_time, (setter)set_time, "Seconds (setting it seeks).", NULL},
    {"duration", (getter)get_duration, NULL, "Seconds; 0 until loaded.", NULL},
    {"size", (getter)get_size, NULL, "(w, h) once loaded, else None.", NULL},
    {"rate", NULL, (setter)set_rate, "Playback speed (write-only), 1/16 .. 16.", NULL},
    {"loop", NULL, (setter)set_loop, "Loop (write-only).", NULL},
    {"controls", NULL, (setter)set_controls, "Blender-style controls on video nodes.", NULL},
    {"poster", NULL, (setter)set_poster, "An image file shown until the first frame.", NULL},
    {"on_state", (getter)get_on_state, (setter)set_on_state, "callable(state) on changes.", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyTypeObject Video_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "buipy.Video",
    .tp_basicsize = sizeof(VideoObject),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "Video(src): a <video> player (muted autoplay; loads when first drawn).",
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)video_init,
    .tp_dealloc = (destructor)video_dealloc,
    .tp_traverse = (traverseproc)video_traverse,
    .tp_clear = (inquiry)video_clear,
    .tp_methods = video_methods,
    .tp_getset = video_getset,
};

int buipy_video_init(PyObject *module)
{
  if (PyType_Ready(&Video_Type) < 0 ||
      PyModule_AddObjectRef(module, "Video", (PyObject *)&Video_Type) < 0 ||
      PyModule_AddStringConstant(module, "video_backends", BK_video_backends()) < 0)
  {
    return -1;
  }
  return 0;
}
