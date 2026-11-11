/* Kit nodes as animation targets, through their keyed properties: a property's name is its
 * key ("value", "factor", "min" ... for BK_set_num), colors go through BK_set_color
 * ("backgroundColor" is a frame's "bg"). The node redraws itself when a value changes, and
 * BK_node_free stops the motions of a node going away (no hold on it here).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdbool.h>

#include "BUI_anim.h"
#include "BUI_kit.h"

static bool is_color(int prop)
{
  return prop == BK_PROP_COLOR || prop == BK_PROP_BACKGROUND_COLOR;
}

static const char *key(int prop)
{
  return prop == BK_PROP_BACKGROUND_COLOR ? "bg" : BK_anim_prop_name(prop);
}

static int node_get(void *obj, int prop, float r_value[4], int *r_unit)
{
  BKNode *n = (BKNode *)obj;
  const char *k = key(prop);
  if (!k) {
    return 0;
  }
  if (is_color(prop)) {
    *r_unit = BK_UNIT_COLOR;
    return BK_get_color(n, k, r_value) ? 4 : 0;
  }
  *r_unit = BK_UNIT_NONE;
  r_value[0] = (float)BK_get_num(n, k);
  return 1;
}

static void node_set(void *obj, int prop, const float *value, int n, int unit)
{
  (void)unit; /* kit values are plain numbers */
  BKNode *node = (BKNode *)obj;
  const char *k = key(prop);
  if (!k) {
    return;
  }
  if (n == 4) {
    BK_set_color(node, k, value);
  }
  else {
    BK_set_num(node, k, value[0]);
  }
}

const BKAnimClass BK_anim_node = {node_get, node_set, NULL, NULL};

int BK_anim_node_has(BKNode *node, int prop, int n)
{
  const char *k = key(prop);
  if (!node || !k) {
    return 0;
  }
  float c[4];
  if (n == 4) {
    return BK_get_color(node, k, c);
  }
  /* a number the node doesn't have can't be read apart from a 0: writing it back tells */
  return BK_set_num(node, k, BK_get_num(node, k));
}
