/* buipy: what the extension's C files share.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "BUI_kit.h"

/* The kit calls back into Python from places where the GIL may not be held (buipy_kit.c says
 * why); every callback the kit or the animation engine gets takes the GIL first. */
#define GIL_ENTER PyGILState_STATE gil_ = PyGILState_Ensure()
#define GIL_LEAVE PyGILState_Release(gil_)

/* The node of a live KitNode; NULL with a TypeError. */
BKNode *buipy_kit_node(PyObject *obj);

/* An exception raised in a callback the kit made: kept (the first one) until the Python call
 * that drove the kit returns, which raises it (buipy_restore_error() returns 1 then). */
void buipy_stash_error(void);
int buipy_restore_error(void);

int buipy_kit_init(PyObject *module);   /* buipy_kit.c */
int buipy_video_init(PyObject *module); /* buipy_video.c */
int buipy_anim_init(PyObject *module);  /* buipy_anim.c */
