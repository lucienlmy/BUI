/* BUI Lua binding: GSAP-style (bui.gsap) and Motion-style (bui.motion) animation of kit
 * widgets, on the kit's one engine (BUI_anim.h).
 *
 * Every call reads its Lua tables once, here at the boundary, into the engine's tracks and
 * vars (unknown eases, properties and options are errors naming them); after that a frame is
 * the engine's alone: no Lua runs unless a callback (onUpdate, ...) was given. Callbacks are
 * registry refs owned by their motion and let go of in its free_data.
 *
 * Kit widgets have no draw transforms or opacity: they animate `value`, colors (`color`,
 * `backgroundColor`) and the numbers BK_set_num takes (sizes, padding, factor, ...).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>

#include "BUI_anim.h"
#include "bui_internal.h"

#define LUABUI_TWEEN_MT    "bui.Tween"
#define LUABUI_TIMELINE_MT "bui.Timeline"
#define LUABUI_ANIM_GUARD  "bui._anim_guard"

#define MAX_TRACKS 16  /* properties in one call */
#define MAX_KEYS 64    /* keyframes of every track of one call */
#define MAX_FLOATS (MAX_KEYS * 4)
#define TARGETS_ON_STACK 64

/* -------------------------------------------------------------------- */
/** \name Callbacks: registry refs owned by a motion
 * \{ */

enum { CB_START, CB_UPDATE, CB_COMPLETE, CB_REPEAT, CB_REVERSE_COMPLETE, CB_INTERRUPT, CB_EVENTS, CB_CALL = CB_EVENTS, CB_N };

typedef struct Calls {
  lua_State *L; /* the main thread; NULL once the state closed */
  int ref[CB_N];
  int args;     /* tl:call / delayedCall parameters (a table), LUA_NOREF: none */
  struct Calls *prev, *next;
} Calls;

static Calls *g_calls; /* every live one: a closing state lets go of its own */

static void calls_free(void *data)
{
  Calls *c = (Calls *)data;
  if (c->L) {
    for (int i = 0; i < CB_N; i++) {
      luaL_unref(c->L, LUA_REGISTRYINDEX, c->ref[i]);
    }
    luaL_unref(c->L, LUA_REGISTRYINDEX, c->args);
  }
  if (c->prev) {
    c->prev->next = c->next;
  }
  else {
    g_calls = c->next;
  }
  if (c->next) {
    c->next->prev = c->prev;
  }
  free(c);
}

static void calls_fire(Calls *c, int which)
{
  lua_State *L = c->L;
  if (!L || c->ref[which] == LUA_NOREF) {
    return;
  }
  const int top = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref[which]);
  int nargs = 0;
  if (c->args != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->args);
    nargs = (int)lua_rawlen(L, -1);
    luaL_checkstack(L, nargs, "bui: callback parameters");
    for (int i = 1; i <= nargs; i++) {
      lua_rawgeti(L, top + 2, i);
    }
    lua_remove(L, top + 2);
  }
  if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
    luabui_report_error(L);
  }
  lua_settop(L, top);
}

static void on_event(BKMotion *m, int event, void *data)
{
  (void)m;
  int i = 0;
  while (i < CB_EVENTS && event != (1 << i)) {
    i++;
  }
  if (i < CB_EVENTS) {
    calls_fire((Calls *)data, i);
  }
}

static void on_call(BKMotion *m, int event, void *data)
{
  (void)m;
  (void)event;
  calls_fire((Calls *)data, CB_CALL);
}

static Calls *calls_new(lua_State *L)
{
  Calls *c = (Calls *)malloc(sizeof(Calls));
  if (!c) {
    luaL_error(L, "bui: out of memory");
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
  c->L = lua_tothread(L, -1);
  lua_pop(L, 1);
  for (int i = 0; i < CB_N; i++) {
    c->ref[i] = LUA_NOREF;
  }
  c->args = LUA_NOREF;
  c->prev = NULL;
  c->next = g_calls;
  if (g_calls) {
    g_calls->prev = c;
  }
  g_calls = c;
  return c;
}

/* the state closes: motions that outlive it keep their (now silent) callbacks */
static int l_guard_gc(lua_State *L)
{
  lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
  lua_State *main = lua_tothread(L, -1);
  for (Calls *c = g_calls; c; c = c->next) {
    if (c->L == main) {
      c->L = NULL;
    }
  }
  return 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Options: GSAP's vars, Motion's transition
 * \{ */

enum {
  V_DURATION, V_DELAY, V_REPEAT, V_REPEAT_DELAY, V_REPEAT_TYPE, V_YOYO, V_TIME_SCALE, V_PAUSED,
  V_REVERSED, V_EASE, V_OVERWRITE, V_IMMEDIATE, V_STAGGER, V_TYPE, V_STIFFNESS, V_DAMPING, V_MASS,
  V_VELOCITY, V_BOUNCE, V_VISUAL_DURATION, V_REST_DELTA, V_REST_SPEED, V_POWER, V_TIME_CONSTANT,
  V_MIN, V_MAX, V_BOUNCE_STIFFNESS, V_BOUNCE_DAMPING, V_TIMES, V_DEFAULTS,
  V_ON_START, /* .. V_ON_START + CB_EVENTS - 1: the callbacks, in CB_* order */
};

/* by name, lowercase without '_': GSAP's camelCase and snake_case are one name, and Lua's
 * keyword `repeat` can be written `repeat_` */
static const struct {
  const char *name;
  int id;
} g_vars[] = {
    {"duration", V_DURATION}, {"delay", V_DELAY}, {"repeat", V_REPEAT},
    {"repeatdelay", V_REPEAT_DELAY}, {"repeattype", V_REPEAT_TYPE}, {"yoyo", V_YOYO},
    {"timescale", V_TIME_SCALE}, {"paused", V_PAUSED}, {"reversed", V_REVERSED}, {"ease", V_EASE},
    {"overwrite", V_OVERWRITE}, {"immediaterender", V_IMMEDIATE}, {"stagger", V_STAGGER},
    {"type", V_TYPE}, {"stiffness", V_STIFFNESS}, {"damping", V_DAMPING}, {"mass", V_MASS},
    {"velocity", V_VELOCITY}, {"bounce", V_BOUNCE}, {"visualduration", V_VISUAL_DURATION},
    {"restdelta", V_REST_DELTA}, {"restspeed", V_REST_SPEED}, {"power", V_POWER},
    {"timeconstant", V_TIME_CONSTANT}, {"min", V_MIN}, {"max", V_MAX},
    {"bouncestiffness", V_BOUNCE_STIFFNESS}, {"bouncedamping", V_BOUNCE_DAMPING},
    {"times", V_TIMES}, {"defaults", V_DEFAULTS}, {"onstart", V_ON_START + CB_START},
    {"onupdate", V_ON_START + CB_UPDATE}, {"oncomplete", V_ON_START + CB_COMPLETE},
    {"onrepeat", V_ON_START + CB_REPEAT}, {"onreversecomplete", V_ON_START + CB_REVERSE_COMPLETE},
    {"oninterrupt", V_ON_START + CB_INTERRUPT},
};

/* what a table may hold */
enum {
  ALLOW_PROPS = 1 << 0,     /* gsap.to's vars: other keys are properties (min, max too) */
  ALLOW_CALLBACKS = 1 << 1,
  ALLOW_TARGETED = 1 << 2,  /* a call with targets: stagger, keyframe times */
  TIMELINE_VARS = 1 << 3,   /* gsap.timeline's own: defaults, and only what a timeline has */
  ONLY_PROPS = 1 << 4,      /* gsap.fromTo's from-vars, motion.animate's keyframes */
};

/* what gsap.timeline's own vars may set (the rest goes in its defaults) */
#define TIMELINE_OPTIONS \
  ((1ull << V_DELAY) | (1ull << V_REPEAT) | (1ull << V_REPEAT_DELAY) | (1ull << V_YOYO) | \
   (1ull << V_TIME_SCALE) | (1ull << V_PAUSED) | (1ull << V_REVERSED) | (1ull << V_DEFAULTS))

enum { FLAVOR_GSAP, FLAVOR_MOTION };

/* The timing a table gave: what a timeline's defaults hold and a call's vars add to. */
typedef struct Timing {
  BKMotionVars v;
  uint64_t set; /* 1 << V_*: given */
  float stiffness, damping, mass, velocity, bounce, visual_duration, rest_delta, rest_speed;
  float power, time_constant, min, max, bounce_stiffness, bounce_damping;
} Timing;

/* One call's parse: its timing, stagger, keyframe options and tracks. */
typedef struct Parse {
  Timing t;
  int flavor;
  int has_stagger;
  BKStagger stagger;
  int nseg_eases, ntimes;
  BKEase seg_eases[MAX_KEYS];
  float times[MAX_KEYS];
  /* tracks: every key's numbers in `values`, flags in `rel` */
  int ntracks, nvalues, nkeys;
  BKTrack tracks[MAX_TRACKS];
  float values[MAX_FLOATS];
  int8_t rel[MAX_KEYS];
  int any_rel[MAX_TRACKS];
  int slots; /* stack index of the callbacks' slots (CB_EVENTS of them), 0: none */
  BKNode *probe; /* the first target: what the properties are checked against */
  const char *fn; /* "gsap.to", ...: errors name the call */
} Parse;

static int var_id(const char *key)
{
  char k[24];
  int n = 0;
  for (const char *s = key; *s; s++) {
    if (*s == '_') {
      continue;
    }
    if (n == (int)sizeof(k) - 1) {
      return -1;
    }
    k[n++] = (char)tolower((unsigned char)*s);
  }
  k[n] = '\0';
  for (size_t i = 0; i < sizeof(g_vars) / sizeof(g_vars[0]); i++) {
    if (strcmp(k, g_vars[i].name) == 0) {
      return g_vars[i].id;
    }
  }
  return -1;
}

static float num_opt(lua_State *L, const Parse *p, const char *key)
{
  if (!lua_isnumber(L, -1)) {
    luaL_error(L, "%s: '%s' takes a number, not a %s", p->fn, key, luaL_typename(L, -1));
  }
  return (float)lua_tonumber(L, -1);
}

static int bool_opt(lua_State *L, const Parse *p, const char *key)
{
  if (!lua_isboolean(L, -1) && !lua_isnumber(L, -1)) {
    luaL_error(L, "%s: '%s' takes a boolean, not a %s", p->fn, key, luaL_typename(L, -1));
  }
  return lua_isboolean(L, -1) ? lua_toboolean(L, -1) : lua_tonumber(L, -1) != 0.0;
}

/* an ease: a name (every GSAP, Motion, CSS and Blender spelling), or Motion's {x1, y1, x2, y2} */
static BKEase read_ease(lua_State *L, int idx, const char *fn)
{
  BKEase e;
  if (lua_type(L, idx) == LUA_TSTRING) {
    if (!BK_ease_parse(lua_tostring(L, idx), &e)) {
      luaL_error(L, "%s: unknown ease '%s'", fn, lua_tostring(L, idx));
    }
    return e;
  }
  if (lua_istable(L, idx) && lua_rawlen(L, idx) == 4) {
    float b[4];
    for (int i = 0; i < 4; i++) {
      lua_rawgeti(L, idx, i + 1);
      if (!lua_isnumber(L, -1)) {
        luaL_error(L, "%s: a cubic-bezier ease is four numbers", fn);
      }
      b[i] = (float)lua_tonumber(L, -1);
      lua_pop(L, 1);
    }
    return BK_ease_bezier(b[0], b[1], b[2], b[3]);
  }
  luaL_error(L, "%s: an ease is a name (\"power2.out\") or {x1, y1, x2, y2}, not a %s", fn,
             luaL_typename(L, idx));
  return e;
}

/* GSAP's stagger: a number (each), or {each, amount, from, grid, axis, ease} */
static void read_stagger(lua_State *L, int idx, Parse *p)
{
  BKStagger *s = &p->stagger;
  memset(s, 0, sizeof(*s));
  s->from = "start";
  BK_ease_parse("none", &s->ease);
  p->has_stagger = 1;
  if (lua_isnumber(L, idx)) {
    s->each = lua_tonumber(L, idx);
    return;
  }
  if (!lua_istable(L, idx)) {
    luaL_error(L, "%s: stagger is a number or a table, not a %s", p->fn, luaL_typename(L, idx));
  }
  idx = lua_absindex(L, idx);
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    const char *k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
    if (strcmp(k, "each") == 0) {
      s->each = num_opt(L, p, "stagger.each");
    }
    else if (strcmp(k, "amount") == 0) {
      s->amount = num_opt(L, p, "stagger.amount");
    }
    else if (strcmp(k, "from") == 0) {
      if (lua_isnumber(L, -1)) {
        s->from = "index";
        s->from_index = (int)lua_tointeger(L, -1);
      }
      else {
        const char *f = lua_tostring(L, -1);
        if (!f || (strcmp(f, "start") && strcmp(f, "center") && strcmp(f, "end") &&
                   strcmp(f, "edges") && strcmp(f, "random") && strcmp(f, "first") &&
                   strcmp(f, "last")))
        {
          luaL_error(L, "%s: stagger.from is start, center, end, edges, random or an index", p->fn);
        }
        /* Motion's names for the ends */
        s->from = strcmp(f, "first") == 0 ? "start" : strcmp(f, "last") == 0 ? "end" : f;
      }
    }
    else if (strcmp(k, "grid") == 0) {
      if (!lua_istable(L, -1) || lua_rawlen(L, -1) != 2) {
        luaL_error(L, "%s: stagger.grid is {rows, columns} (\"auto\" needs element positions)", p->fn);
      }
      lua_rawgeti(L, -1, 1);
      lua_rawgeti(L, -2, 2);
      s->grid_rows = (int)lua_tointeger(L, -2);
      s->grid_cols = (int)lua_tointeger(L, -1);
      lua_pop(L, 2);
    }
    else if (strcmp(k, "axis") == 0) {
      const char *a = lua_tostring(L, -1);
      if (!a || (strcmp(a, "x") && strcmp(a, "y"))) {
        luaL_error(L, "%s: stagger.axis is \"x\" or \"y\"", p->fn);
      }
      s->axis = a; /* the vars table holds the string while the call runs */
    }
    else if (strcmp(k, "ease") == 0) {
      s->ease = read_ease(L, -1, p->fn);
    }
    else {
      luaL_error(L, "%s: unknown stagger option '%s'", p->fn, k);
    }
    lua_pop(L, 1);
  }
}

/* one option (the value on top of the stack) */
static void read_var(lua_State *L, Parse *p, int id, const char *key, int allow)
{
  Timing *t = &p->t;
  BKMotionVars *v = &t->v;
  if (id >= V_ON_START) {
    if (!(allow & ALLOW_CALLBACKS) || !p->slots) {
      luaL_error(L, "%s: '%s' doesn't go here (callbacks go in a motion's own vars)", p->fn, key);
    }
    if (!lua_isfunction(L, -1)) {
      luaL_error(L, "%s: '%s' takes a function, not a %s", p->fn, key, luaL_typename(L, -1));
    }
    lua_pushvalue(L, -1);
    lua_replace(L, p->slots + (id - V_ON_START));
    v->events |= 1 << (id - V_ON_START);
    return;
  }
  if ((allow & TIMELINE_VARS) && !((TIMELINE_OPTIONS >> id) & 1)) {
    luaL_error(L, "%s: '%s' doesn't go in a timeline's vars (put it in its defaults)", p->fn, key);
  }
  const int targeted = id == V_STAGGER || id == V_TIMES || (id == V_DELAY && lua_istable(L, -1));
  if (targeted && !(allow & ALLOW_TARGETED)) {
    luaL_error(L, "%s: '%s' goes in the vars of a call with targets", p->fn, key);
  }
  t->set |= (uint64_t)1 << id;
  switch (id) {
    case V_DURATION:
      v->duration = num_opt(L, p, key);
      break;
    case V_DELAY:
      if (lua_istable(L, -1)) { /* Motion's delay: stagger(0.05) */
        read_stagger(L, -1, p);
      }
      else {
        v->delay = num_opt(L, p, key);
      }
      break;
    case V_REPEAT: {
      const double r = num_opt(L, p, key);
      v->repeat = (r < 0.0 || r >= 1e9) ? -1 : (int)r; /* -1, math.huge: forever */
      break;
    }
    case V_REPEAT_DELAY:
      v->repeat_delay = num_opt(L, p, key);
      break;
    case V_REPEAT_TYPE: { /* Motion's: loop, reverse, mirror */
      const char *s = lua_tostring(L, -1);
      if (!s || (strcmp(s, "loop") && strcmp(s, "reverse") && strcmp(s, "mirror"))) {
        luaL_error(L, "%s: repeatType is loop, reverse or mirror", p->fn);
      }
      v->yoyo = strcmp(s, "loop") != 0;
      break;
    }
    case V_YOYO:
      v->yoyo = bool_opt(L, p, key);
      break;
    case V_TIME_SCALE:
      v->time_scale = num_opt(L, p, key);
      break;
    case V_PAUSED:
      v->paused = bool_opt(L, p, key);
      break;
    case V_REVERSED:
      v->reversed = bool_opt(L, p, key);
      break;
    case V_EASE: {
      int first = LUA_TNIL;
      if (lua_istable(L, -1)) {
        first = lua_rawgeti(L, -1, 1);
        lua_pop(L, 1);
      }
      if (first == LUA_TSTRING || first == LUA_TTABLE) {
        /* Motion's per-keyframe eases: {"easeIn", "linear", ...} */
        const int n = (int)lua_rawlen(L, -1);
        if (n > MAX_KEYS) {
          luaL_error(L, "%s: more than %d eases", p->fn, MAX_KEYS);
        }
        for (int i = 0; i < n; i++) {
          lua_rawgeti(L, -1, i + 1);
          p->seg_eases[i] = read_ease(L, -1, p->fn);
          lua_pop(L, 1);
        }
        p->nseg_eases = n;
        t->set &= ~((uint64_t)1 << V_EASE);
      }
      else {
        v->ease = read_ease(L, -1, p->fn);
      }
      break;
    }
    case V_OVERWRITE:
      if (lua_isboolean(L, -1)) {
        v->overwrite = lua_toboolean(L, -1) ? BK_OVERWRITE_AUTO : BK_OVERWRITE_NONE;
      }
      else {
        const char *s = lua_tostring(L, -1);
        if (!s || (strcmp(s, "auto") && strcmp(s, "all") && strcmp(s, "none"))) {
          luaL_error(L, "%s: overwrite is true, false, \"auto\", \"all\" or \"none\"", p->fn);
        }
        v->overwrite = s[0] == 'n' ? BK_OVERWRITE_NONE : s[1] == 'l' ? BK_OVERWRITE_ALL : BK_OVERWRITE_AUTO;
      }
      break;
    case V_IMMEDIATE:
      v->immediate = bool_opt(L, p, key);
      break;
    case V_STAGGER:
      read_stagger(L, -1, p);
      break;
    case V_TYPE: {
      const char *s = lua_tostring(L, -1);
      if (s && (strcmp(s, "tween") == 0 || strcmp(s, "keyframes") == 0)) {
        v->kind = BK_MOTION_TWEEN_KIND;
      }
      else if (s && strcmp(s, "spring") == 0) {
        v->kind = BK_MOTION_SPRING_KIND;
      }
      else if (s && (strcmp(s, "inertia") == 0 || strcmp(s, "decay") == 0)) {
        v->kind = BK_MOTION_INERTIA_KIND;
      }
      else {
        luaL_error(L, "%s: type is tween, keyframes, spring, inertia or decay", p->fn);
      }
      break;
    }
    case V_STIFFNESS: t->stiffness = num_opt(L, p, key); break;
    case V_DAMPING: t->damping = num_opt(L, p, key); break;
    case V_MASS: t->mass = num_opt(L, p, key); break;
    case V_VELOCITY: t->velocity = num_opt(L, p, key); break;
    case V_BOUNCE: t->bounce = num_opt(L, p, key); break;
    case V_VISUAL_DURATION: t->visual_duration = num_opt(L, p, key); break;
    case V_REST_DELTA: t->rest_delta = num_opt(L, p, key); break;
    case V_REST_SPEED: t->rest_speed = num_opt(L, p, key); break;
    case V_POWER: t->power = num_opt(L, p, key); break;
    case V_TIME_CONSTANT: t->time_constant = num_opt(L, p, key); break;
    case V_MIN: t->min = num_opt(L, p, key); break;
    case V_MAX: t->max = num_opt(L, p, key); break;
    case V_BOUNCE_STIFFNESS: t->bounce_stiffness = num_opt(L, p, key); break;
    case V_BOUNCE_DAMPING: t->bounce_damping = num_opt(L, p, key); break;
    case V_TIMES: {
      luaL_checktype(L, -1, LUA_TTABLE);
      const int n = (int)lua_rawlen(L, -1);
      if (n > MAX_KEYS) {
        luaL_error(L, "%s: more than %d times", p->fn, MAX_KEYS);
      }
      for (int i = 0; i < n; i++) {
        lua_rawgeti(L, -1, i + 1);
        p->times[i] = num_opt(L, p, "times");
        lua_pop(L, 1);
      }
      p->ntimes = n;
      break;
    }
    default:
      luaL_error(L, "%s: '%s' doesn't go here", p->fn, key);
  }
}

static int has(const Timing *t, int id)
{
  return (t->set >> id) & 1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tracks: the properties of kit widgets
 * \{ */

/* one value of `n` numbers (the value on top of the stack): a number, a string the engine
 * reads ("+=10", "50%", "#ff8800", "rgb(...)"), or a color's {r, g, b[, a]} */
static void read_value(lua_State *L, Parse *p, const char *prop, int n, float *out, int8_t *rel,
                       int *unit)
{
  *rel = 0;
  if (lua_type(L, -1) == LUA_TNUMBER) {
    if (n != 1) {
      luaL_error(L, "%s: '%s' is a color: a string (\"#ff8800\") or {r, g, b, a}", p->fn, prop);
    }
    out[0] = (float)lua_tonumber(L, -1);
    return;
  }
  if (lua_type(L, -1) == LUA_TSTRING) {
    float v[4];
    int r = 0;
    const int got = BK_anim_parse_value(lua_tostring(L, -1), v, unit, &r);
    if (got == 0) {
      luaL_error(L, "%s: '%s' can't be '%s'", p->fn, prop, lua_tostring(L, -1));
    }
    if (got != n) {
      luaL_error(L, "%s: '%s' takes %s, not '%s'", p->fn, prop, n == 4 ? "a color" : "a number",
                 lua_tostring(L, -1));
    }
    memcpy(out, v, sizeof(float) * (size_t)n);
    *rel = (int8_t)r;
    return;
  }
  if (n == 4 && lua_istable(L, -1)) {
    const int len = (int)lua_rawlen(L, -1);
    if (len == 3 || len == 4) {
      out[3] = 1.0f;
      for (int i = 0; i < len; i++) {
        lua_rawgeti(L, -1, i + 1);
        if (!lua_isnumber(L, -1)) {
          luaL_error(L, "%s: a color for '%s' is {r, g, b[, a]} numbers", p->fn, prop);
        }
        out[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
      }
      return;
    }
  }
  luaL_error(L, "%s: '%s' can't be a %s", p->fn, prop, luaL_typename(L, -1));
}

/* is the value on top keyframes (an array of values)? a color's {r, g, b, a} is one value */
static int is_keyframes(lua_State *L, int n)
{
  if (!lua_istable(L, -1)) {
    return 0;
  }
  if (n == 1) {
    return 1;
  }
  const int first = lua_rawgeti(L, -1, 1);
  lua_pop(L, 1);
  return first != LUA_TNUMBER;
}

/* the property `name`'s id and numbers (1, or 4 for a color) on kit nodes, or an error */
static int kit_prop(lua_State *L, const Parse *p, const char *name, int *r_n)
{
  const int prop = BK_anim_prop(name);
  if (prop <= BK_PROP_OPACITY) {
    luaL_error(L, "%s: kit widgets have no transforms or opacity ('%s'); they animate value, "
                  "colors and sizes", p->fn, name);
  }
  float cur[4];
  int unit;
  const int n = BK_anim_node.get(p->probe, prop, cur, &unit);
  /* the engine's node class writes BK_set_num by the property's name: it must be one */
  if (n == 0 || (n == 1 && !luabui_is_numeric_key(BK_anim_prop_name(prop)))) {
    luaL_error(L, "%s: a %s has no property '%s' to animate", p->fn, BK_node_type(p->probe), name);
  }
  *r_n = n;
  return prop;
}

/* the property `name` with its value(s) on top of the stack: one more track */
static void read_track(lua_State *L, Parse *p, const char *name, int from)
{
  if (p->ntracks == MAX_TRACKS) {
    luaL_error(L, "%s: more than %d properties", p->fn, MAX_TRACKS);
  }
  int n;
  const int prop = kit_prop(L, p, name, &n);
  const int keys = is_keyframes(L, n);
  const int nkeys = keys ? (int)lua_rawlen(L, -1) : 1;
  if (nkeys < 1 || p->nkeys + nkeys > MAX_KEYS) {
    luaL_error(L, "%s: '%s' needs 1 to %d keyframes", p->fn, name, MAX_KEYS - p->nkeys);
  }
  BKTrack *t = &p->tracks[p->ntracks];
  memset(t, 0, sizeof(*t));
  t->prop = prop;
  t->n = n;
  t->nkeys = nkeys;
  t->from = from;
  t->values = &p->values[p->nvalues];
  t->unit = n == 4 ? BK_UNIT_COLOR : BK_UNIT_NONE;
  int8_t *rel = &p->rel[p->nkeys];
  int any = 0;
  for (int k = 0; k < nkeys; k++) {
    if (keys) {
      lua_rawgeti(L, -1, k + 1);
    }
    int unit = t->unit;
    read_value(L, p, name, n, &p->values[p->nvalues + k * n], &rel[k], &unit);
    any |= rel[k];
    if (keys) {
      lua_pop(L, 1);
    }
  }
  t->rel = any ? rel : NULL;
  p->nvalues += nkeys * n;
  p->nkeys += nkeys;
  p->ntracks++;
}

/* a vars table: its options into p->t, its properties into p's tracks */
static void read_table(lua_State *L, int idx, Parse *p, int allow, int from)
{
  if (lua_isnoneornil(L, idx)) {
    return;
  }
  luaL_checktype(L, idx, LUA_TTABLE);
  idx = lua_absindex(L, idx);
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    if (lua_type(L, -2) != LUA_TSTRING) {
      luaL_error(L, "%s: vars keys are names, not %ss", p->fn, luaL_typename(L, -2));
    }
    const char *key = lua_tostring(L, -2);
    const int id = (allow & ONLY_PROPS) ? -1 : var_id(key);
    const int is_prop = id < 0 || ((allow & ALLOW_PROPS) && (id == V_MIN || id == V_MAX));
    if (is_prop) {
      if (!(allow & (ALLOW_PROPS | ONLY_PROPS))) {
        luaL_error(L, "%s: unknown option '%s'", p->fn, key);
      }
      read_track(L, p, key, from);
    }
    else if (id == V_DEFAULTS) {
      if (!(allow & TIMELINE_VARS)) {
        luaL_error(L, "%s: 'defaults' goes in gsap.timeline's vars", p->fn);
      }
      /* read by the timeline itself */
    }
    else {
      read_var(L, p, id, key, allow);
    }
    lua_pop(L, 1);
  }
}

/* a timing that starts from GSAP's or Motion's defaults (or a timeline's) */
static void parse_init(Parse *p, const char *fn, int flavor, const Timing *base)
{
  p->fn = fn;
  p->flavor = flavor;
  if (base) {
    p->t = *base;
  }
  else {
    memset(&p->t, 0, sizeof(p->t));
    p->t.v = BK_motion_vars_default();
  }
  p->t.v.events = 0;
  p->has_stagger = 0;
  p->nseg_eases = p->ntimes = 0;
  p->ntracks = p->nvalues = p->nkeys = 0;
  p->slots = 0;
  p->probe = NULL;
}

/* reserve the callbacks' stack slots (nil until a vars table fills them) */
static void parse_slots(lua_State *L, Parse *p)
{
  p->slots = lua_gettop(L) + 1;
  lua_settop(L, p->slots + CB_EVENTS - 1);
}

/* after the tables: the options that depend on each other (springs, Motion's defaults,
 * keyframe times and eases), checked once */
static void parse_finish(lua_State *L, Parse *p)
{
  Timing *t = &p->t;
  BKMotionVars *v = &t->v;
  int maxkeys = 1;
  for (int i = 0; i < p->ntracks; i++) {
    maxkeys = p->tracks[i].nkeys > maxkeys ? p->tracks[i].nkeys : maxkeys;
  }
  if (p->flavor == FLAVOR_MOTION) { /* Motion's defaults (getDefaultTransition) */
    if (!has(t, V_DURATION)) {
      v->duration = maxkeys > 1 ? 0.8 : 0.3;
    }
    if (!has(t, V_EASE)) {
      const int plain = !has(t, V_TYPE) && !has(t, V_DURATION) && maxkeys <= 1;
      if (plain) {
        v->ease = BK_ease_bezier(0.25f, 0.1f, 0.35f, 1.0f);
      }
      else {
        BK_ease_parse("easeInOut", &v->ease);
      }
    }
  }
  for (int i = 0; i < p->ntracks; i++) {
    BKTrack *tr = &p->tracks[i];
    if (tr->nkeys < 2) {
      continue;
    }
    if (p->ntimes) {
      if (p->ntimes != tr->nkeys) {
        luaL_error(L, "%s: %d times for %d keyframes", p->fn, p->ntimes, tr->nkeys);
      }
      tr->times = p->times;
    }
    if (p->nseg_eases) {
      if (p->nseg_eases != tr->nkeys - 1) {
        luaL_error(L, "%s: %d eases for %d keyframes (one per segment)", p->fn, p->nseg_eases,
                   tr->nkeys);
      }
      tr->eases = p->seg_eases;
    }
  }
  if (v->kind == BK_MOTION_SPRING_KIND) { /* Motion's three forms */
    BKSpring s;
    const float bounce = has(t, V_BOUNCE) ? t->bounce : 0.25f;
    if (has(t, V_VISUAL_DURATION)) {
      s = BK_spring_bounce(bounce, t->visual_duration);
    }
    else if (has(t, V_DURATION) && !has(t, V_STIFFNESS)) {
      s = BK_spring_duration((float)v->duration, bounce, has(t, V_VELOCITY) ? t->velocity : 0.0f,
                             has(t, V_MASS) ? t->mass : 1.0f);
    }
    else {
      s = BK_spring_default();
      if (p->flavor == FLAVOR_MOTION && !has(t, V_STIFFNESS) && !has(t, V_DAMPING)) {
        s.stiffness = 500.0f;
        s.damping = 25.0f;
        s.rest_speed = 10.0f;
      }
    }
    if (has(t, V_STIFFNESS)) s.stiffness = t->stiffness;
    if (has(t, V_DAMPING)) s.damping = t->damping;
    if (has(t, V_MASS)) s.mass = t->mass;
    if (has(t, V_VELOCITY)) s.velocity = t->velocity;
    if (has(t, V_REST_DELTA)) s.rest_delta = t->rest_delta;
    if (has(t, V_REST_SPEED)) s.rest_speed = t->rest_speed;
    v->spring = s;
  }
  else if (v->kind == BK_MOTION_INERTIA_KIND) {
    BKInertia in = BK_inertia_default(has(t, V_VELOCITY) ? t->velocity : 0.0f);
    if (has(t, V_POWER)) in.power = t->power;
    if (has(t, V_TIME_CONSTANT)) in.time_constant = t->time_constant;
    if (has(t, V_MIN)) in.min = t->min;
    if (has(t, V_MAX)) in.max = t->max;
    if (has(t, V_BOUNCE_STIFFNESS)) in.bounce_stiffness = t->bounce_stiffness;
    if (has(t, V_BOUNCE_DAMPING)) in.bounce_damping = t->bounce_damping;
    if (has(t, V_REST_DELTA)) in.rest_delta = t->rest_delta;
    v->inertia = in;
  }
}

/* the callbacks in the slots, owned by the motion from here on (no error can follow) */
static void parse_callbacks(lua_State *L, Parse *p, BKMotionVars *v)
{
  v->on = NULL;
  v->data = NULL;
  v->free_data = NULL;
  if (!p->slots || !v->events) {
    v->events = 0;
    return;
  }
  Calls *c = calls_new(L);
  for (int i = 0; i < CB_EVENTS; i++) {
    if (!lua_isnil(L, p->slots + i)) {
      lua_pushvalue(L, p->slots + i);
      c->ref[i] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
  }
  v->on = on_event;
  v->data = c;
  v->free_data = calls_free;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Targets and motions
 * \{ */

static BKNode *check_live_node(lua_State *L, int idx, const char *fn)
{
  luabui_Node *ud = (luabui_Node *)luaL_testudata(L, idx, LUABUI_NODE_MT);
  if (!ud) {
    luaL_error(L, "%s: targets are bui nodes (or a list of them), not a %s", fn, luaL_typename(L, idx));
  }
  if (!ud->node) {
    luaL_error(L, "%s: attempt to animate a freed node", fn);
  }
  return ud->node;
}

/* the nodes at `idx`: one, or a list; `stack` holds up to TARGETS_ON_STACK, more go in a
 * scratch block Lua collects */
static BKNode **read_targets(lua_State *L, int idx, BKNode **stack, int *r_count, const char *fn)
{
  if (!lua_istable(L, idx)) {
    stack[0] = check_live_node(L, idx, fn);
    *r_count = 1;
    return stack;
  }
  const int n = (int)lua_rawlen(L, idx);
  if (n == 0) {
    luaL_error(L, "%s: no targets", fn);
  }
  BKNode **nodes = n <= TARGETS_ON_STACK ? stack : (BKNode **)lua_newuserdatauv(L, sizeof(BKNode *) * (size_t)n, 0);
  for (int i = 0; i < n; i++) {
    lua_rawgeti(L, idx, i + 1);
    nodes[i] = check_live_node(L, -1, fn);
    lua_pop(L, 1);
  }
  *r_count = n;
  return nodes;
}

/* every target has the color properties the first one has */
static void check_targets(lua_State *L, const Parse *p, BKNode **nodes, int count)
{
  for (int i = 1; i < count; i++) {
    for (int k = 0; k < p->ntracks; k++) {
      float cur[4];
      int unit;
      if (BK_anim_node.get(nodes[i], p->tracks[k].prop, cur, &unit) != p->tracks[k].n) {
        luaL_error(L, "%s: a %s has no property '%s' to animate", p->fn, BK_node_type(nodes[i]),
                   BK_anim_prop_name(p->tracks[k].prop));
      }
    }
  }
}

/* the motion of a parsed call: one tween, or (several targets, a stagger) a timeline of one
 * tween per target that holds the call's delay, controls and callbacks, as GSAP's one tween
 * of many targets does */
static BKMotion *make_motion(lua_State *L, Parse *p, BKNode **nodes, int count)
{
  check_targets(L, p, nodes, count);
  BKMotionVars v = p->t.v;
  if (count == 1 && !p->has_stagger) {
    parse_callbacks(L, p, &v);
    return BK_tween(&BK_anim_node, nodes[0], p->tracks, p->ntracks, &v);
  }
  double stack[TARGETS_ON_STACK];
  double *offs = count <= TARGETS_ON_STACK ? stack : (double *)lua_newuserdatauv(L, sizeof(double) * (size_t)count, 0);
  if (p->has_stagger) {
    BK_stagger(&p->stagger, count, offs);
  }
  else {
    memset(offs, 0, sizeof(double) * (size_t)count);
  }
  BKMotionVars group = BK_motion_vars_default();
  group.delay = v.delay;
  group.paused = v.paused;
  group.reversed = v.reversed;
  group.time_scale = v.time_scale;
  group.events = v.events;
  parse_callbacks(L, p, &group);
  BKMotion *tl = BK_timeline(&group);
  BKMotionVars each = v;
  each.delay = 0.0;
  each.paused = each.reversed = 0;
  each.time_scale = 1.0;
  each.events = 0;
  each.on = NULL;
  each.data = NULL;
  each.free_data = NULL;
  for (int i = 0; i < count; i++) {
    BKMotion *m = BK_tween(&BK_anim_node, nodes[i], p->tracks, p->ntracks, &each);
    char pos[32];
    snprintf(pos, sizeof(pos), "%.17g", offs[i]);
    BK_timeline_add(tl, m, pos);
    BK_motion_release(m); /* the timeline holds it */
  }
  return tl;
}

/* GSAP's fromTo: the from-vars' values become the first key of the to-vars' tracks (a
 * property only in the from-vars goes from there to where it is) */
static void merge_from(lua_State *L, Parse *to, const Parse *from)
{
  for (int i = 0; i < from->ntracks; i++) {
    const BKTrack *f = &from->tracks[i];
    BKTrack *t = NULL;
    for (int k = 0; k < to->ntracks; k++) {
      if (to->tracks[k].prop == f->prop) {
        t = &to->tracks[k];
      }
    }
    if (f->nkeys != 1 || (t && t->nkeys != 1)) {
      luaL_error(L, "%s: keyframes don't go in fromTo (use to with an array)", to->fn);
    }
    if (to->nkeys + 1 > MAX_KEYS) {
      luaL_error(L, "%s: more than %d keyframes", to->fn, MAX_KEYS);
    }
    if (!t) {
      if (to->ntracks == MAX_TRACKS) {
        luaL_error(L, "%s: more than %d properties", to->fn, MAX_TRACKS);
      }
      t = &to->tracks[to->ntracks++];
      *t = *f;
      t->values = &to->values[to->nvalues];
      memcpy(&to->values[to->nvalues], f->values, sizeof(float) * (size_t)f->n);
      to->rel[to->nkeys] = f->rel ? f->rel[0] : 0;
      t->rel = f->rel ? &to->rel[to->nkeys] : NULL;
      to->nvalues += f->n;
      to->nkeys += 1;
      t->from = 1;
      continue;
    }
    /* two keys: from, then to, side by side at the end of the buffers */
    float *vals = &to->values[to->nvalues];
    int8_t *rel = &to->rel[to->nkeys];
    memcpy(vals, f->values, sizeof(float) * (size_t)f->n);
    memcpy(vals + f->n, t->values, sizeof(float) * (size_t)t->n);
    rel[0] = f->rel ? f->rel[0] : 0;
    rel[1] = t->rel ? t->rel[0] : 0;
    t->values = vals;
    t->rel = (rel[0] || rel[1]) ? rel : NULL;
    t->nkeys = 2;
    to->nvalues += 2 * t->n;
    to->nkeys += 2;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tween and timeline handles
 * \{ */

typedef struct LTween {
  BKMotion *m;
} LTween;

typedef struct LTimeline {
  BKMotion *m;
  int has_defaults;
  Timing defaults; /* gsap.timeline({defaults = {...}}): what its tweens start from */
} LTimeline;

static BKMotion *check_motion(lua_State *L, int idx)
{
  LTween *t = (LTween *)luaL_testudata(L, idx, LUABUI_TWEEN_MT);
  if (!t) {
    t = (LTween *)luaL_checkudata(L, idx, LUABUI_TIMELINE_MT); /* the same first member */
  }
  if (!t->m) {
    luaL_error(L, "bui: this motion was released");
  }
  return t->m;
}

static void push_tween(lua_State *L, BKMotion *m)
{
  LTween *t = (LTween *)lua_newuserdatauv(L, sizeof(LTween), 0);
  t->m = m;
  luaL_setmetatable(L, LUABUI_TWEEN_MT);
}

static int l_motion_gc(lua_State *L)
{
  LTween *t = (LTween *)lua_touserdata(L, 1);
  if (t && t->m) {
    BK_motion_release(t->m); /* a motion still running plays on: the engine holds it */
    t->m = NULL;
  }
  return 0;
}

/* controls, GSAP's names (setters return the motion, getters a value) */
#define CONTROL(name, call) \
  static int name(lua_State *L) \
  { \
    call(check_motion(L, 1)); \
    lua_settop(L, 1); \
    return 1; \
  }
CONTROL(l_play, BK_motion_play)
CONTROL(l_pause, BK_motion_pause)
CONTROL(l_resume, BK_motion_resume)
CONTROL(l_reverse, BK_motion_reverse)
CONTROL(l_restart, BK_motion_restart)
CONTROL(l_complete, BK_motion_complete)
CONTROL(l_kill, BK_motion_kill)
#undef CONTROL

static int l_seek(lua_State *L)
{
  BK_motion_seek(check_motion(L, 1), luaL_checknumber(L, 2));
  lua_settop(L, 1);
  return 1;
}

/* get or set a number: m:progress() / m:progress(0.5) */
#define ACCESSOR(name, get, set) \
  static int name(lua_State *L) \
  { \
    BKMotion *m = check_motion(L, 1); \
    if (lua_isnoneornil(L, 2)) { \
      lua_pushnumber(L, get(m)); \
      return 1; \
    } \
    set(m, luaL_checknumber(L, 2)); \
    lua_settop(L, 1); \
    return 1; \
  }
static double total_progress(const BKMotion *m)
{
  const double d = BK_motion_total_duration(m);
  return d > 0.0 ? BK_motion_total_time(m) / d : 1.0;
}
ACCESSOR(l_progress, BK_motion_progress, BK_motion_set_progress)
ACCESSOR(l_total_progress, total_progress, BK_motion_set_total_progress)
ACCESSOR(l_time_scale, BK_motion_time_scale, BK_motion_set_time_scale)
ACCESSOR(l_time, BK_motion_time, BK_motion_seek)
#undef ACCESSOR

static int l_duration(lua_State *L)
{
  lua_pushnumber(L, BK_motion_duration(check_motion(L, 1)));
  return 1;
}

static int l_total_duration(lua_State *L)
{
  lua_pushnumber(L, BK_motion_total_duration(check_motion(L, 1)));
  return 1;
}

static int l_is_active(lua_State *L)
{
  lua_pushboolean(L, BK_motion_is_active(check_motion(L, 1)));
  return 1;
}

/* m:paused() / m:paused(true) */
static int l_paused(lua_State *L)
{
  BKMotion *m = check_motion(L, 1);
  if (lua_isnone(L, 2)) {
    lua_pushboolean(L, BK_motion_paused(m));
    return 1;
  }
  if (lua_toboolean(L, 2)) {
    BK_motion_pause(m);
  }
  else {
    BK_motion_resume(m);
  }
  lua_settop(L, 1);
  return 1;
}

/* m:reversed() / m:reversed(true) */
static int l_reversed(lua_State *L)
{
  BKMotion *m = check_motion(L, 1);
  if (lua_isnone(L, 2)) {
    lua_pushboolean(L, BK_motion_reversed(m));
    return 1;
  }
  if (!lua_toboolean(L, 2) != !BK_motion_reversed(m)) {
    BK_motion_reverse(m);
  }
  lua_settop(L, 1);
  return 1;
}

/* every motion's controls (GSAP's, and Motion's complete / stop / speed) */
static const luaL_Reg g_controls[] = {
    {"play", l_play},
    {"pause", l_pause},
    {"resume", l_resume},
    {"reverse", l_reverse},
    {"restart", l_restart},
    {"seek", l_seek},
    {"progress", l_progress},
    {"totalProgress", l_total_progress},
    {"timeScale", l_time_scale},
    {"speed", l_time_scale},
    {"time", l_time},
    {"duration", l_duration},
    {"totalDuration", l_total_duration},
    {"kill", l_kill},
    {"stop", l_kill},
    {"complete", l_complete},
    {"isActive", l_is_active},
    {"paused", l_paused},
    {"reversed", l_reversed},
    {"__gc", l_motion_gc},
    {NULL, NULL},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name gsap.to / from / fromTo / set, in a timeline or not
 * \{ */

enum { OP_TO, OP_FROM, OP_FROM_TO, OP_SET };

static const char *const g_op_names[2][4] = {
    {"gsap.to", "gsap.from", "gsap.fromTo", "gsap.set"},
    {"timeline:to", "timeline:from", "timeline:fromTo", "timeline:set"},
};

/* targets at `idx`, vars after it (fromTo: from-vars, then to-vars): the motion */
static BKMotion *gsap_motion(lua_State *L, int op, int idx, const Timing *defaults, int in_timeline)
{
  Parse p;
  BKNode *stack[TARGETS_ON_STACK];
  int count;
  parse_init(&p, g_op_names[in_timeline][op], FLAVOR_GSAP, defaults);
  BKNode **nodes = read_targets(L, idx, stack, &count, p.fn);
  p.probe = nodes[0];
  parse_slots(L, &p);
  const int vars = op == OP_FROM_TO ? idx + 2 : idx + 1;
  luaL_checktype(L, vars, LUA_TTABLE);
  read_table(L, vars, &p, ALLOW_PROPS | ALLOW_CALLBACKS | ALLOW_TARGETED, op == OP_FROM);
  if (op == OP_FROM_TO) {
    Parse f;
    parse_init(&f, p.fn, FLAVOR_GSAP, NULL);
    f.probe = nodes[0];
    luaL_checktype(L, idx + 1, LUA_TTABLE);
    read_table(L, idx + 1, &f, ONLY_PROPS, 0);
    merge_from(L, &p, &f);
  }
  if (p.ntracks == 0) {
    luaL_error(L, "%s: nothing to animate (no properties in the vars)", p.fn);
  }
  if (op == OP_SET) {
    p.t.v.duration = 0.0;
    p.t.set |= (uint64_t)1 << V_DURATION;
  }
  if ((op == OP_FROM || op == OP_FROM_TO) && !has(&p.t, V_IMMEDIATE)) {
    p.t.v.immediate = 1; /* GSAP's immediateRender for from() and fromTo() */
  }
  parse_finish(L, &p);
  return make_motion(L, &p, nodes, count);
}

#define GSAP_OP(name, op) \
  static int name(lua_State *L) \
  { \
    push_tween(L, gsap_motion(L, op, 1, NULL, 0)); \
    return 1; \
  }
GSAP_OP(l_gsap_to, OP_TO)
GSAP_OP(l_gsap_from, OP_FROM)
GSAP_OP(l_gsap_from_to, OP_FROM_TO)
GSAP_OP(l_gsap_set, OP_SET)
#undef GSAP_OP

/** \} */

/* -------------------------------------------------------------------- */
/** \name Timelines
 * \{ */

static LTimeline *check_timeline(lua_State *L, int idx)
{
  LTimeline *t = (LTimeline *)luaL_checkudata(L, idx, LUABUI_TIMELINE_MT);
  if (!t->m) {
    luaL_error(L, "bui: this timeline was released");
  }
  return t;
}

/* GSAP's position parameter: a string ("<0.2", "label+=1", "-=0.5"), a number (absolute), or
 * nothing (the end) */
static const char *read_position(lua_State *L, int idx, char *buf, size_t size)
{
  if (lua_isnoneornil(L, idx)) {
    return "";
  }
  if (lua_type(L, idx) == LUA_TNUMBER) {
    snprintf(buf, size, "%.17g", lua_tonumber(L, idx));
    return buf;
  }
  return luaL_checkstring(L, idx);
}

/* gsap.timeline({defaults = {...}, repeat_, yoyo, paused, onComplete, ...}) */
static int l_gsap_timeline(lua_State *L)
{
  Parse p;
  parse_init(&p, "gsap.timeline", FLAVOR_GSAP, NULL);
  parse_slots(L, &p);
  read_table(L, 1, &p, ALLOW_CALLBACKS | TIMELINE_VARS, 0);
  Parse d;
  int has_defaults = 0;
  if (lua_istable(L, 1) && lua_getfield(L, 1, "defaults") != LUA_TNIL) {
    parse_init(&d, "gsap.timeline defaults", FLAVOR_GSAP, NULL);
    read_table(L, -1, &d, 0, 0);
    has_defaults = 1;
  }
  BKMotionVars v = p.t.v;
  parse_callbacks(L, &p, &v);
  LTimeline *t = (LTimeline *)lua_newuserdatauv(L, sizeof(LTimeline), 0);
  t->m = BK_timeline(&v);
  t->has_defaults = has_defaults;
  if (has_defaults) {
    t->defaults = d.t;
  }
  luaL_setmetatable(L, LUABUI_TIMELINE_MT);
  return 1;
}

/* tl:to(targets, vars, position) ...: the tween placed at `position`; returns tl */
static int timeline_op(lua_State *L, int op)
{
  LTimeline *tl = check_timeline(L, 1);
  char buf[32];
  const char *pos = read_position(L, op == OP_FROM_TO ? 5 : 4, buf, sizeof(buf));
  lua_pushvalue(L, 1);
  const int self = lua_gettop(L);
  BKMotion *m = gsap_motion(L, op, 2, tl->has_defaults ? &tl->defaults : NULL, 1);
  BK_timeline_add(tl->m, m, pos);
  BK_motion_release(m); /* the timeline holds it */
  lua_settop(L, self);
  return 1;
}

static int l_tl_to(lua_State *L) { return timeline_op(L, OP_TO); }
static int l_tl_from(lua_State *L) { return timeline_op(L, OP_FROM); }
static int l_tl_from_to(lua_State *L) { return timeline_op(L, OP_FROM_TO); }
static int l_tl_set(lua_State *L) { return timeline_op(L, OP_SET); }

/* tl:add(tween_or_timeline, position) */
static int l_tl_add(lua_State *L)
{
  LTimeline *tl = check_timeline(L, 1);
  BKMotion *child = check_motion(L, 2);
  if (child == tl->m) {
    return luaL_error(L, "timeline:add: a timeline can't hold itself");
  }
  char buf[32];
  BK_timeline_add(tl->m, child, read_position(L, 3, buf, sizeof(buf)));
  lua_settop(L, 1);
  return 1;
}

/* tl:addLabel(name, position) */
static int l_tl_add_label(lua_State *L)
{
  LTimeline *tl = check_timeline(L, 1);
  char buf[32];
  BK_timeline_label(tl->m, luaL_checkstring(L, 2), read_position(L, 3, buf, sizeof(buf)));
  lua_settop(L, 1);
  return 1;
}

/* a function (and its parameters) as a timeline call, owned by it */
static void add_call(lua_State *L, BKMotion *tl, int fn, int params, const char *pos)
{
  luaL_checktype(L, fn, LUA_TFUNCTION);
  if (params) {
    luaL_checktype(L, params, LUA_TTABLE);
  }
  Calls *c = calls_new(L);
  lua_pushvalue(L, fn);
  c->ref[CB_CALL] = luaL_ref(L, LUA_REGISTRYINDEX);
  if (params) {
    lua_pushvalue(L, params);
    c->args = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  BK_timeline_call(tl, on_call, c, calls_free, pos);
}

/* tl:call(fn, position) or tl:call(fn, {params}, position) (GSAP's order) */
static int l_tl_call(lua_State *L)
{
  LTimeline *tl = check_timeline(L, 1);
  const int params = lua_istable(L, 3) ? 3 : 0;
  char buf[32];
  const char *pos = read_position(L, params ? 4 : 3, buf, sizeof(buf));
  add_call(L, tl->m, 2, params, pos);
  lua_settop(L, 1);
  return 1;
}

/* tl:labelTime(name): where a label is (nil: none) */
static int l_tl_label_time(lua_State *L)
{
  const double t = BK_timeline_label_time(check_timeline(L, 1)->m, luaL_checkstring(L, 2));
  if (t < 0.0) {
    return 0;
  }
  lua_pushnumber(L, t);
  return 1;
}

static const luaL_Reg g_timeline_methods[] = {
    {"to", l_tl_to},
    {"from", l_tl_from},
    {"fromTo", l_tl_from_to},
    {"set", l_tl_set},
    {"add", l_tl_add},
    {"addLabel", l_tl_add_label},
    {"call", l_tl_call},
    {"labelTime", l_tl_label_time},
    {NULL, NULL},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name The rest of gsap: delayedCall, killTweensOf, registerEase, ticker
 * \{ */

/* gsap.delayedCall(delay, fn, {params}) */
static int l_gsap_delayed_call(lua_State *L)
{
  const double delay = luaL_checknumber(L, 1);
  BKMotionVars v = BK_motion_vars_default();
  BKMotion *tl = BK_timeline(&v);
  push_tween(L, tl); /* first: an error below lets the collector release it */
  char pos[32];
  snprintf(pos, sizeof(pos), "%.17g", delay);
  add_call(L, tl, 2, lua_istable(L, 3) ? 3 : 0, pos);
  return 1;
}

/* gsap.killTweensOf(targets [, "value,color"]) */
static int l_gsap_kill_tweens_of(lua_State *L)
{
  BKNode *stack[TARGETS_ON_STACK];
  int count;
  BKNode **nodes = read_targets(L, 1, stack, &count, "gsap.killTweensOf");
  const char *props = luaL_optstring(L, 2, NULL);
  for (int i = 0; i < count; i++) {
    if (!props) {
      BK_anim_kill_target(nodes[i], -1);
      continue;
    }
    for (const char *s = props; *s;) {
      while (*s == ',' || *s == ' ') {
        s++;
      }
      char name[64];
      size_t n = 0;
      while (*s && *s != ',' && *s != ' ' && n < sizeof(name) - 1) {
        name[n++] = *s++;
      }
      name[n] = '\0';
      if (n) {
        BK_anim_kill_target(nodes[i], BK_anim_prop(name));
      }
    }
  }
  return 0;
}

/* gsap.registerEase(name, ease): a name BK_ease_parse finds from then on */
static int l_gsap_register_ease(lua_State *L)
{
  const char *name = luaL_checkstring(L, 1);
  const BKEase e = read_ease(L, 2, "gsap.registerEase");
  BK_ease_register(name, &e);
  return 0;
}

/* gsap.parseEase(name)(t): an ease's value at t, for drawing or checking a curve */
static int l_ease_eval(lua_State *L)
{
  const BKEase *e = (const BKEase *)lua_touserdata(L, lua_upvalueindex(1));
  lua_pushnumber(L, BK_ease_eval(e, (float)luaL_checknumber(L, 1)));
  return 1;
}

static int l_gsap_parse_ease(lua_State *L)
{
  const BKEase e = read_ease(L, 1, "gsap.parseEase");
  BKEase *u = (BKEase *)lua_newuserdatauv(L, sizeof(BKEase), 0);
  *u = e;
  lua_pushcclosure(L, l_ease_eval, 1);
  return 1;
}

/* gsap.ticker.tick(now): advance every motion to `now` (seconds, increasing). The kit ticks in
 * its frames; a host with its own clock (or a headless test) ticks here after
 * gsap.ticker.useHostClock(true). Returns whether something still animates. */
static int l_ticker_tick(lua_State *L)
{
  lua_pushboolean(L, BK_anim_tick(luaL_checknumber(L, 1)));
  return 1;
}

static int l_ticker_use_host_clock(lua_State *L)
{
  BK_anim_set_host_clock(lua_toboolean(L, 1));
  return 0;
}

/* gsap.ticker.lagSmoothing(threshold, adjusted) (0: off) */
static int l_ticker_lag_smoothing(lua_State *L)
{
  BK_anim_set_lag_smoothing(luaL_checknumber(L, 1), luaL_optnumber(L, 2, 0.033));
  return 0;
}

/* gsap.globalTimeScale(scale): every motion's speed */
static int l_gsap_global_time_scale(lua_State *L)
{
  BK_anim_set_time_scale(luaL_checknumber(L, 1));
  return 0;
}

static const luaL_Reg g_gsap[] = {
    {"to", l_gsap_to},
    {"from", l_gsap_from},
    {"fromTo", l_gsap_from_to},
    {"set", l_gsap_set},
    {"timeline", l_gsap_timeline},
    {"delayedCall", l_gsap_delayed_call},
    {"killTweensOf", l_gsap_kill_tweens_of},
    {"registerEase", l_gsap_register_ease},
    {"parseEase", l_gsap_parse_ease},
    {"globalTimeScale", l_gsap_global_time_scale},
    {NULL, NULL},
};

static const luaL_Reg g_ticker[] = {
    {"tick", l_ticker_tick},
    {"useHostClock", l_ticker_use_host_clock},
    {"lagSmoothing", l_ticker_lag_smoothing},
    {NULL, NULL},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name motion.animate, motion.stagger
 * \{ */

/* motion.animate(targets, {value = 1, color = "#f80"}, {duration, ease, type = "spring", ...}) */
static int l_motion_animate(lua_State *L)
{
  Parse p;
  BKNode *stack[TARGETS_ON_STACK];
  int count;
  parse_init(&p, "motion.animate", FLAVOR_MOTION, NULL);
  BKNode **nodes = read_targets(L, 1, stack, &count, p.fn);
  p.probe = nodes[0];
  luaL_checktype(L, 2, LUA_TTABLE);
  parse_slots(L, &p);
  read_table(L, 2, &p, ONLY_PROPS, 0);
  read_table(L, 3, &p, ALLOW_CALLBACKS | ALLOW_TARGETED, 0);
  if (p.ntracks == 0) {
    return luaL_error(L, "%s: nothing to animate (no properties)", p.fn);
  }
  parse_finish(L, &p);
  push_tween(L, make_motion(L, &p, nodes, count));
  return 1;
}

/* motion.stagger(each, {from = "last", ...}): a stagger for a transition's delay */
static int l_motion_stagger(lua_State *L)
{
  const lua_Number each = luaL_checknumber(L, 1);
  lua_newtable(L);
  if (lua_istable(L, 2)) {
    lua_pushnil(L);
    while (lua_next(L, 2)) {
      if (lua_type(L, -2) == LUA_TSTRING && strcmp(lua_tostring(L, -2), "startDelay") == 0) {
        return luaL_error(L, "motion.stagger: startDelay isn't supported; give the transition a delay");
      }
      lua_pushvalue(L, -2);
      lua_insert(L, -2);
      lua_rawset(L, -4);
    }
  }
  lua_pushnumber(L, each);
  lua_setfield(L, -2, "each");
  return 1;
}

static const luaL_Reg g_motion[] = {
    {"animate", l_motion_animate},
    {"stagger", l_motion_stagger},
    {NULL, NULL},
};

/** \} */

/* a build without the feature `anim`: every call says so (it never silently does nothing) */
static int l_unavailable(lua_State *L)
{
  return luaL_error(L, "bui: this build has no animation (the feature `anim`, BUI_ANIM)");
}

/* the table of `fns` on top of the stack (each l_unavailable without the engine) */
static void new_lib(lua_State *L, const luaL_Reg *fns)
{
  lua_newtable(L);
  for (; fns->name; fns++) {
    lua_pushcfunction(L, BK_anim_available() ? fns->func : l_unavailable);
    lua_setfield(L, -2, fns->name);
  }
}

/* bui.gsap and bui.motion into the module table on top of the stack */
void luabui_register_anim(lua_State *L)
{
  /* the handles: controls for both, the timeline's own methods on top */
  luaL_newmetatable(L, LUABUI_TWEEN_MT);
  luaL_setfuncs(L, g_controls, 0);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  luaL_newmetatable(L, LUABUI_TIMELINE_MT);
  luaL_setfuncs(L, g_controls, 0);
  luaL_setfuncs(L, g_timeline_methods, 0);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  /* the state closing silences the callbacks of motions that outlive it */
  if (lua_getfield(L, LUA_REGISTRYINDEX, LUABUI_ANIM_GUARD) == LUA_TNIL) {
    lua_newuserdatauv(L, 1, 0);
    lua_newtable(L);
    lua_pushcfunction(L, l_guard_gc);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, LUABUI_ANIM_GUARD);
  }
  lua_pop(L, 1);

  new_lib(L, g_gsap);
  new_lib(L, g_ticker);
  lua_setfield(L, -2, "ticker");
  lua_pushboolean(L, BK_anim_available());
  lua_setfield(L, -2, "available");
  lua_setfield(L, -2, "gsap");
  new_lib(L, g_motion);
  lua_setfield(L, -2, "motion");
}
