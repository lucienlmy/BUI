/* Global look: scale, font, themes and Blender's icon list.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.theme;

import bui.internal.util : cslice, cstr;
import bui.kit;

/// Blender's resolution scale (1 = 20 px widgets).
void setScale(float scale) {
    BK_set_scale(scale);
}

/// The current scale.
float scale() {
    return BK_scale();
}

/// Blender's UI text shadows (drop shadow, soft one on panel titles); on by default.
void setTextShadow(bool on) {
    BK_set_text_shadow(on ? 1 : 0);
}

/// Whether UI text shadows are on.
bool textShadow() {
    return BK_text_shadow() != 0;
}

/// U.widget_unit in pixels.
int unit() {
    return BK_unit();
}

/// Replace the UI font family.
void setFont(string family) {
    BK_set_font(family.length ? cstr(family) : null);
}

/// The UI text size in points (Blender's 11 is the default).
void setFontSize(float points) {
    BK_set_font_size(points);
}

/// The current UI text size in points.
float fontSizePoints() {
    return BK_font_size_points();
}

/// Switch themes ("dark" or "light"); false when the name is unknown.
bool setTheme(string name) {
    return BK_set_theme(cstr(name)) != 0;
}

/// The active theme name ("dark" or "light").
string themeName() {
    return cslice(BK_theme_name()).idup;
}

/// A theme color as RGBA 0..1, for example "region_bg" or "regular.inner".
bool themeColor(string key, out float[4] rgba) {
    return BK_theme_color(cstr(key), rgba.ptr) != 0;
}

/// Register the themes in a theme file's JSON; how many were added, or -1 on error.
int loadTheme(string json) {
    return BK_theme_load_json(cstr(json));
}

/// Register the themes in a theme file; how many were added, or -1 on error.
int loadThemeFile(string path) {
    return BK_theme_load_file(cstr(path));
}

/// The error from the last `loadTheme` / `loadThemeFile`, or "".
string themeError() {
    return cslice(BK_theme_error()).idup;
}

/// Keys of the last load that BUI has no use for.
int themeIgnored() {
    return BK_theme_ignored();
}

/// The number of registered themes.
int themeCount() {
    return BK_theme_count();
}

/// The name of registered theme `index`, or "" past the end.
string themeAt(int index) {
    return cslice(BK_theme_at(index)).idup;
}

/// Set one color of the active theme until the next `setTheme`.
bool setThemeColor(string key, const float[4] rgba) {
    return BK_theme_set_color(cstr(key), rgba.ptr) != 0;
}

/// Set one number or boolean of the active theme until the next `setTheme`.
bool setThemeNum(string key, double value) {
    return BK_theme_set_num(cstr(key), value) != 0;
}

/// Read a number or boolean of the active theme.
bool themeNum(string key, out double value) {
    return BK_theme_num(cstr(key), &value) != 0;
}

/// A number that changes whenever a theme value does.
int themeGeneration() {
    return BK_theme_generation();
}

/// The built-in theme ("dark" or "light") the active one is based on.
string themeMode() {
    return cslice(BK_theme_mode()).idup;
}

/* -- icons ----------------------------------------------------------------- */

/// Blender's icon id for a name; 0 when unknown.
int iconId(string name) {
    return BK_icon_id(cstr(name));
}

/// The number of named icons.
int iconCount() {
    return BK_icon_count();
}

/// The name of icon `id`.
string iconName(int id) {
    return cslice(BK_icon_name(id)).idup;
}

/// The number of names in the listing (SVG icons, then BUI-only ones).
int iconListCount() {
    return BK_icon_list_count();
}

/// Listing entry `i`.
string iconListName(int i) {
    return cslice(BK_icon_list_name(i)).idup;
}
