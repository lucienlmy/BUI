/* The animation engine's own declarations (kit/intern/anim): BUI_anim.h is the API.
 *
 *  ease.c     easing: the Penner families, curves, tables, names
 *  spring.c   springs and inertia in closed form
 *  value.c    property ids and values as text (lengths, angles, colors)
 *  motion.c   tweens, timelines, the clock, overwriting, stagger
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <stdbool.h>

#include "BLI_string.h"
#include "BUI_anim.h"
#include "MEM_guardedalloc.h"

#ifndef BK_MIN /* as kit_intern.h */
#  define BK_MIN(a, b) ((a) < (b) ? (a) : (b))
#  define BK_MAX(a, b) ((a) > (b) ? (a) : (b))
#  define BK_CLAMP(v, a, b) ((v) < (a) ? (a) : ((v) > (b) ? (b) : (v)))
#endif

#define MEM_reallocN(p, len) MEM_reallocN_id((p), (len), __func__)
#define MEM_SAFE_FREE(p) \
  do { \
    if (p) { \
      MEM_freeN(p); \
      (p) = NULL; \
    } \
  } while (0)

/* One value driven by physics: a spring, or inertia that turns into a spring at a bound. */
typedef struct BKPhysics {
  int kind; /* BK_MOTION_SPRING_KIND / INERTIA_KIND */
  BKSpring spring;
  BKInertia inertia;
  BKSpring bspring;  /* inertia: the spring at the bound */
  float from, to;    /* inertia: `to` is where the decay heads */
  float amplitude, bound;
  double switch_t;   /* inertia: when it reaches the bound (< 0: never) */
  double duration;   /* until it settles */
} BKPhysics;
void bk_physics_init(BKPhysics *ph, int kind, const BKSpring *s, const BKInertia *in, float from, float to);
void bk_physics_eval(const BKPhysics *ph, double t, float *r_pos, float *r_vel);

/* The pools the engine keeps for the program's life (BK_anim_exit frees them). */
void bk_ease_exit(void);
void bk_value_exit(void);
void bk_motion_exit(void);
