/* The animation API in a build without the feature (BUI_ANIM off): every function is there,
 * nothing animates, and BK_anim_available() says so. Values set by an animation's end are
 * not written either: an app that needs them checks BK_anim_available().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <string.h>

#include "BUI_anim.h"

int BK_anim_available(void) { return 0; }

int BK_ease_parse(const char *name, BKEase *r_ease) { (void)name; memset(r_ease, 0, sizeof(*r_ease)); return 0; }
float BK_ease_eval(const BKEase *e, float t) { (void)e; return t; }
BKEase BK_ease_bezier(float x1, float y1, float x2, float y2) { (void)x1; (void)y1; (void)x2; (void)y2; BKEase e; memset(&e, 0, sizeof(e)); return e; }
int BK_ease_points(const float (*xy)[2], int n, BKEase *r_ease) { (void)xy; (void)n; memset(r_ease, 0, sizeof(*r_ease)); return 0; }
int BK_ease_path(const char *svg_path, BKEase *r_ease) { (void)svg_path; memset(r_ease, 0, sizeof(*r_ease)); return 0; }
int BK_ease_register(const char *name, const BKEase *e) { (void)name; (void)e; return 0; }

BKSpring BK_spring_default(void) { BKSpring s; memset(&s, 0, sizeof(s)); return s; }
BKSpring BK_spring_bounce(float bounce, float visual_duration) { (void)bounce; (void)visual_duration; return BK_spring_default(); }
BKSpring BK_spring_duration(float duration, float bounce, float velocity, float mass) { (void)duration; (void)bounce; (void)velocity; (void)mass; return BK_spring_default(); }
void BK_spring_eval(const BKSpring *s, float from, float to, double t, float *r_pos, float *r_vel) { (void)s; (void)from; (void)t; if (r_pos) *r_pos = to; if (r_vel) *r_vel = 0.0f; }
double BK_spring_settle(const BKSpring *s, float from, float to) { (void)s; (void)from; (void)to; return 0.0; }
BKInertia BK_inertia_default(float velocity) { BKInertia i; memset(&i, 0, sizeof(i)); i.velocity = velocity; return i; }

int BK_anim_prop(const char *name) { (void)name; return 0; }
const char *BK_anim_prop_name(int prop) { (void)prop; return NULL; }
int BK_anim_parse_value(const char *text, float r_value[4], int *r_unit, int *r_rel) { (void)text; (void)r_value; *r_unit = 0; *r_rel = 0; return 0; }

BKMotionVars BK_motion_vars_default(void) { BKMotionVars v; memset(&v, 0, sizeof(v)); return v; }
BKMotion *BK_tween(const BKAnimClass *cls, void *obj, const BKTrack *tracks, int ntracks, const BKMotionVars *vars) { (void)cls; (void)obj; (void)tracks; (void)ntracks; if (vars && vars->free_data && vars->data) vars->free_data(vars->data); return NULL; }
BKMotion *BK_timeline(const BKMotionVars *vars) { if (vars && vars->free_data && vars->data) vars->free_data(vars->data); return NULL; }
double BK_timeline_add(BKMotion *tl, BKMotion *child, const char *position) { (void)tl; (void)child; (void)position; return 0.0; }
double BK_timeline_label(BKMotion *tl, const char *name, const char *position) { (void)tl; (void)name; (void)position; return 0.0; }
double BK_timeline_call(BKMotion *tl, BKMotionFn fn, void *data, void (*free_data)(void *data), const char *position) { (void)tl; (void)fn; (void)position; if (free_data && data) free_data(data); return 0.0; }
double BK_timeline_label_time(const BKMotion *tl, const char *name) { (void)tl; (void)name; return -1.0; }
void BK_stagger(const BKStagger *s, int count, double *r_offsets) { (void)s; for (int i = 0; i < count; i++) r_offsets[i] = 0.0; }

void BK_motion_play(BKMotion *m) { (void)m; }
void BK_motion_pause(BKMotion *m) { (void)m; }
void BK_motion_resume(BKMotion *m) { (void)m; }
void BK_motion_reverse(BKMotion *m) { (void)m; }
void BK_motion_restart(BKMotion *m) { (void)m; }
void BK_motion_seek(BKMotion *m, double time) { (void)m; (void)time; }
void BK_motion_set_progress(BKMotion *m, double progress) { (void)m; (void)progress; }
void BK_motion_set_total_progress(BKMotion *m, double progress) { (void)m; (void)progress; }
void BK_motion_set_time_scale(BKMotion *m, double scale) { (void)m; (void)scale; }
void BK_motion_complete(BKMotion *m) { (void)m; }
void BK_motion_kill(BKMotion *m) { (void)m; }
int BK_motion_is_active(const BKMotion *m) { (void)m; return 0; }
int BK_motion_paused(const BKMotion *m) { (void)m; return 0; }
int BK_motion_reversed(const BKMotion *m) { (void)m; return 0; }
double BK_motion_time(const BKMotion *m) { (void)m; return 0.0; }
double BK_motion_total_time(const BKMotion *m) { (void)m; return 0.0; }
double BK_motion_duration(const BKMotion *m) { (void)m; return 0.0; }
double BK_motion_total_duration(const BKMotion *m) { (void)m; return 0.0; }
double BK_motion_progress(const BKMotion *m) { (void)m; return 1.0; }
double BK_motion_time_scale(const BKMotion *m) { (void)m; return 1.0; }
float BK_motion_velocity(const BKMotion *m, int prop) { (void)m; (void)prop; return 0.0f; }
void BK_motion_retain(BKMotion *m) { (void)m; }
void BK_motion_release(BKMotion *m) { (void)m; }

int BK_anim_tick(double now) { (void)now; return 0; }
int BK_anim_active(void) { return 0; }
void BK_anim_set_reduced_motion(int on) { (void)on; }
int BK_anim_reduced_motion(void) { return 0; }
void BK_anim_set_time_scale(double scale) { (void)scale; }
void BK_anim_set_lag_smoothing(double threshold, double adjusted) { (void)threshold; (void)adjusted; }
void BK_anim_kill_target(void *obj, int prop) { (void)obj; (void)prop; }
void BK_anim_exit(void) {}
void BK_anim_set_host_clock(int on) { (void)on; }
void BK_anim_set_clock(double (*now)(void)) { (void)now; }
int BK_anim_host_clock(void) { return 0; }
void BK_anim_set_wake(void (*wake)(void *data), void *data) { (void)wake; (void)data; }
const BKAnimClass BK_anim_node = {NULL, NULL, NULL, NULL};
int BK_anim_node_has(struct BKNode *node, int prop, int n) { (void)node; (void)prop; (void)n; return 0; }
