/* BUI stable C API (api/bui_c_api.h): version and render-sink plumbing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.capi;

public import bui.gpu : BuiGpuSink;

extern(C) {

void bui_get_version(int* r_major, int* r_minor, int* r_patch);
const(char)* bui_version_string();

void bui_set_render_sink(const(BuiGpuSink)* sink);
const(BuiGpuSink)* bui_get_render_sink();

void bui_set_viewport(int width, int height);

} // extern(C)
