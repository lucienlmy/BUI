/* The CSS engine: colors, tokens, selectors and the cascade.
 *
 * The HTML layer (`bui.html`) builds a tree that implements `CssSubject`;
 * `Stylesheet.compute` turns the matching rules plus inline styles into a
 * `Style` that `bui.html.realize` applies to kit nodes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.css;

public import bui.css.color;
public import bui.css.selectors;
public import bui.css.style;
public import bui.css.tokens;
