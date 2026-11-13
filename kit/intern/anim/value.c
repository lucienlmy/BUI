/* Property ids and values as text. A property name is interned once into a small integer
 * (the transform shorthands and the common properties have fixed ids), so a frame compares
 * integers; "scaleX", "scale-x" and "scale_x" are one property, and GSAP's and Motion's
 * aliases ("rotation", "autoAlpha", "bg") are the property they mean. Values ("100px",
 * "#f80", "rgb(...)", "+=50") are read once, when an animation is made.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "anim_intern.h"

/* -------------------------------------------------------------------- */
/** \name Property ids
 * \{ */

static const char *const FIXED[BK_PROP__FIXED] = {
    NULL,     "x",       "y",       "z",     "scale",  "scaleX", "scaleY",          "rotate",
    "rotateX", "rotateY", "skewX",   "skewY", "opacity", "width", "height", "color",
    "backgroundColor", "value",
};
/* other spellings of a fixed property (normalized: lowercase, no '-' or '_') */
static const struct { const char *alias; int prop; } ALIASES[] = {
    {"rotation", BK_PROP_ROTATE},  {"rotationx", BK_PROP_ROTATE_X}, {"rotationy", BK_PROP_ROTATE_Y},
    {"rotatez", BK_PROP_ROTATE},   {"rotationz", BK_PROP_ROTATE},   {"translatex", BK_PROP_X},
    {"translatey", BK_PROP_Y},     {"translatez", BK_PROP_Z},       {"autoalpha", BK_PROP_OPACITY},
    {"alpha", BK_PROP_OPACITY},    {"bg", BK_PROP_BACKGROUND_COLOR}, {"background", BK_PROP_BACKGROUND_COLOR},
    {"textcolor", BK_PROP_COLOR},
};

static struct {
  char **names;   /* interned, by id - BK_PROP__FIXED */
  char **keys;    /* their normalized spellings */
  int n, cap;
} V;

/* lowercase, without '-' and '_' */
static void normalize(const char *s, char *out, size_t size)
{
  size_t k = 0;
  for (; *s && k + 1 < size; s++) {
    if (*s != '-' && *s != '_') {
      out[k++] = (char)tolower((unsigned char)*s);
    }
  }
  out[k] = '\0';
}

/* "scale-x" / "scale_x" as camelCase: the name properties are known by */
static char *camel(const char *s)
{
  char *out = BLI_strdup(s);
  size_t k = 0;
  bool up = false;
  for (const char *p = s; *p; p++) {
    if (*p == '-' || *p == '_') {
      up = k > 0;
      continue;
    }
    out[k++] = up ? (char)toupper((unsigned char)*p) : *p;
    up = false;
  }
  out[k] = '\0';
  return out;
}

int BK_anim_prop(const char *name)
{
  if (!name || !*name) {
    return 0;
  }
  char key[64];
  normalize(name, key, sizeof(key));
  for (int i = 1; i < BK_PROP__FIXED; i++) {
    char fixed[64];
    normalize(FIXED[i], fixed, sizeof(fixed));
    if (strcmp(key, fixed) == 0) {
      return i;
    }
  }
  for (size_t i = 0; i < sizeof(ALIASES) / sizeof(ALIASES[0]); i++) {
    if (strcmp(key, ALIASES[i].alias) == 0) {
      return ALIASES[i].prop;
    }
  }
  for (int i = 0; i < V.n; i++) {
    if (strcmp(key, V.keys[i]) == 0) {
      return BK_PROP__FIXED + i;
    }
  }
  if (V.n == V.cap) {
    V.cap = V.cap ? V.cap * 2 : 32;
    V.names = (char **)MEM_reallocN(V.names, sizeof(char *) * (size_t)V.cap);
    V.keys = (char **)MEM_reallocN(V.keys, sizeof(char *) * (size_t)V.cap);
  }
  V.names[V.n] = camel(name);
  V.keys[V.n] = BLI_strdup(key);
  return BK_PROP__FIXED + V.n++;
}

const char *BK_anim_prop_name(int prop)
{
  if (prop > 0 && prop < BK_PROP__FIXED) {
    return FIXED[prop];
  }
  const int i = prop - BK_PROP__FIXED;
  return i >= 0 && i < V.n ? V.names[i] : NULL;
}

void bk_value_exit(void)
{
  for (int i = 0; i < V.n; i++) {
    MEM_freeN(V.names[i]);
    MEM_freeN(V.keys[i]);
  }
  MEM_SAFE_FREE(V.names);
  MEM_SAFE_FREE(V.keys);
  V.n = V.cap = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Colors (CSS: hex, rgb(), hsl(), names)
 * \{ */

/* The CSS named colors, as 0xRRGGBB. */
static const struct { const char *name; uint32_t rgb; } NAMED[] = {
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4},
    {"azure", 0xf0ffff}, {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000},
    {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff}, {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a},
    {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e},
    {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
    {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b},
    {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b},
    {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00},
    {"darkorchid", 0x9932cc}, {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f},
    {"darkslateblue", 0x483d8b}, {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f},
    {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3}, {"deeppink", 0xff1493},
    {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969}, {"dimgrey", 0x696969}, {"dodgerblue", 0x1e90ff},
    {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22}, {"fuchsia", 0xff00ff},
    {"gainsboro", 0xdcdcdc}, {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700}, {"goldenrod", 0xdaa520},
    {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xadff2f}, {"grey", 0x808080},
    {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c}, {"indigo", 0x4b0082},
    {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa}, {"lavenderblush", 0xfff0f5},
    {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6}, {"lightcoral", 0xf08080},
    {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3},
    {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a},
    {"lightseagreen", 0x20b2aa}, {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899},
    {"lightslategrey", 0x778899}, {"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0},
    {"lime", 0x00ff00}, {"limegreen", 0x32cd32}, {"linen", 0xfaf0e6}, {"magenta", 0xff00ff},
    {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa}, {"mediumblue", 0x0000cd},
    {"mediumorchid", 0xba55d3}, {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371},
    {"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc},
    {"mediumvioletred", 0xc71585}, {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa},
    {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead}, {"navy", 0x000080},
    {"oldlace", 0xfdf5e6}, {"olive", 0x808000}, {"olivedrab", 0x6b8e23}, {"orange", 0xffa500},
    {"orangered", 0xff4500}, {"orchid", 0xda70d6}, {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98},
    {"paleturquoise", 0xafeeee}, {"palevioletred", 0xdb7093}, {"papayawhip", 0xffefd5},
    {"peachpuff", 0xffdab9}, {"peru", 0xcd853f}, {"pink", 0xffc0cb}, {"plum", 0xdda0dd},
    {"powderblue", 0xb0e0e6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399}, {"red", 0xff0000},
    {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513}, {"salmon", 0xfa8072},
    {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57}, {"seashell", 0xfff5ee}, {"sienna", 0xa0522d},
    {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb}, {"slateblue", 0x6a5acd}, {"slategray", 0x708090},
    {"slategrey", 0x708090}, {"snow", 0xfffafa}, {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4},
    {"tan", 0xd2b48c}, {"teal", 0x008080}, {"thistle", 0xd8bfd8}, {"tomato", 0xff6347},
    {"turquoise", 0x40e0d0}, {"violet", 0xee82ee}, {"wheat", 0xf5deb3}, {"white", 0xffffff},
    {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00}, {"yellowgreen", 0x9acd32},
};

static int hexval(char c)
{
  return isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) >= 'a' && tolower((unsigned char)c) <= 'f') ? tolower((unsigned char)c) - 'a' + 10 : -1;
}

static bool parse_hex(const char *s, float rgba[4])
{
  const size_t n = strlen(s);
  if (n != 3 && n != 4 && n != 6 && n != 8) {
    return false;
  }
  int d[8];
  for (size_t i = 0; i < n; i++) {
    if ((d[i] = hexval(s[i])) < 0) {
      return false;
    }
  }
  const bool short_form = n <= 4;
  for (int c = 0; c < 4; c++) {
    if ((size_t)(short_form ? c : 2 * c) >= n) {
      rgba[c] = 1.0f;
      continue;
    }
    const int v = short_form ? d[c] * 17 : d[2 * c] * 16 + d[2 * c + 1];
    rgba[c] = (float)v / 255.0f;
  }
  return true;
}

static float hue_rgb(float p, float q, float t)
{
  t = t < 0.0f ? t + 1.0f : t > 1.0f ? t - 1.0f : t;
  return t < 1.0f / 6 ? p + (q - p) * 6.0f * t : t < 0.5f ? q : t < 2.0f / 3 ? p + (q - p) * (2.0f / 3 - t) * 6.0f : p;
}

/* rgb()/rgba()/hsl()/hsla(), comma or space separated, "/ alpha", percentages */
static bool parse_func(const char *s, float rgba[4])
{
  const bool hsl = strncmp(s, "hsl", 3) == 0;
  if (!hsl && strncmp(s, "rgb", 3) != 0) {
    return false;
  }
  const char *p = strchr(s, '(');
  if (!p) {
    return false;
  }
  p++;
  float v[4] = {0, 0, 0, 1};
  bool pct[4] = {false};
  int n = 0;
  while (*p && *p != ')' && n < 4) {
    while (*p == ' ' || *p == ',' || *p == '/') {
      p++;
    }
    char *end;
    const float f = strtof(p, &end);
    if (end == p || !isfinite(f)) {
      break;
    }
    pct[n] = *end == '%';
    v[n++] = f;
    p = pct[n - 1] ? end + 1 : end;
    while (isalpha((unsigned char)*p)) {
      p++; /* "deg" after a hue */
    }
  }
  if (n < 3) {
    return false;
  }
  rgba[3] = n == 4 ? (pct[3] ? v[3] / 100.0f : v[3]) : 1.0f;
  if (hsl) {
    const float h = fmodf(v[0], 360.0f) / 360.0f, sat = v[1] / 100.0f, l = v[2] / 100.0f;
    const float q = l < 0.5f ? l * (1.0f + sat) : l + sat - l * sat, pp = 2.0f * l - q;
    rgba[0] = hue_rgb(pp, q, h + 1.0f / 3);
    rgba[1] = hue_rgb(pp, q, h);
    rgba[2] = hue_rgb(pp, q, h - 1.0f / 3);
  }
  else {
    for (int c = 0; c < 3; c++) {
      rgba[c] = pct[c] ? v[c] / 100.0f : v[c] / 255.0f;
    }
  }
  for (int c = 0; c < 4; c++) {
    rgba[c] = BK_CLAMP(rgba[c], 0.0f, 1.0f);
  }
  return true;
}

static bool parse_color(const char *s, float rgba[4])
{
  if (*s == '#') {
    return parse_hex(s + 1, rgba);
  }
  char low[32];
  size_t k = 0;
  for (; s[k] && k + 1 < sizeof(low); k++) {
    low[k] = (char)tolower((unsigned char)s[k]);
  }
  low[k] = '\0';
  if (parse_func(low, rgba)) {
    return true;
  }
  if (strcmp(low, "transparent") == 0) {
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0f;
    return true;
  }
  for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++) {
    if (strcmp(low, NAMED[i].name) == 0) {
      rgba[0] = (float)((NAMED[i].rgb >> 16) & 255) / 255.0f;
      rgba[1] = (float)((NAMED[i].rgb >> 8) & 255) / 255.0f;
      rgba[2] = (float)(NAMED[i].rgb & 255) / 255.0f;
      rgba[3] = 1.0f;
      return true;
    }
  }
  return false;
}

/** \} */

int BK_anim_parse_value(const char *text, float r_value[4], int *r_unit, int *r_rel)
{
  *r_unit = BK_UNIT_NONE;
  *r_rel = 0;
  if (!text) {
    return 0;
  }
  while (*text == ' ') {
    text++;
  }
  if ((text[0] == '+' || text[0] == '-') && text[1] == '=') {
    *r_rel = text[0] == '+' ? 1 : -1;
    text += 2;
  }
  char *end;
  const float f = strtof(text, &end);
  if (end != text && !isfinite(f)) {
    return 0; /* "nan", "inf", "1e999": not a value */
  }
  if (end == text) {
    if (!*r_rel && parse_color(text, r_value)) {
      *r_unit = BK_UNIT_COLOR;
      return 4;
    }
    return 0;
  }
  static const struct { const char *suffix; int unit; } UNITS[] = {
      {"px", BK_UNIT_PX},   {"%", BK_UNIT_PERCENT}, {"rem", BK_UNIT_REM}, {"em", BK_UNIT_EM},
      {"vw", BK_UNIT_VW},   {"vh", BK_UNIT_VH},     {"deg", BK_UNIT_DEG}, {"rad", BK_UNIT_RAD},
      {"turn", BK_UNIT_TURN},
  };
  while (*end == ' ') {
    end++;
  }
  for (size_t i = 0; i < sizeof(UNITS) / sizeof(UNITS[0]); i++) {
    if (BLI_strcasecmp(end, UNITS[i].suffix) == 0) {
      *r_unit = UNITS[i].unit;
      break;
    }
  }
  if (*end && *r_unit == BK_UNIT_NONE) {
    return 0; /* "12abc" */
  }
  r_value[0] = f;
  return 1;
}
