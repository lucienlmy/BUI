/* The HTML layer: an element tree, tag constructors and CSS realisation.
 *
 * `bui.html.element` is the tree and its event handlers, `bui.html.tags`
 * has every standard element plus the user-agent stylesheet, and
 * `bui.html.realize` turns the styled tree into kit nodes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.html;

public import bui.html.element;
public import bui.html.realize;
public import bui.html.tags;
