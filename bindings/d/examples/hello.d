/* BUI from D: a slider and a button in a native window.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
import bui;
import std.stdio;

void main() {
    auto root = new Node("frame");
    root.set("padding", 8);

    auto size = new Node("slider");
    size.set("label", "Size");
    size.set("min", 0);
    size.set("max", 10);
    size.set("value", 2.5);
    size.onChange((NodeEvent e) {
        writeln("size = ", e.node.num("value"));
    });
    root.add(size);
    size.pack("fill", 1);

    auto apply = new Node("button");
    apply.set("text", "Apply");
    apply.onClick((NodeEvent) {
        writeln("clicked");
        quit();
    });
    root.add(apply);
    apply.pack("fill", 1).pack("pady0", 6);

    auto context = new Context(root);
    auto window = new Window(context, "BUI from D", 320, 120);
    window.show();
    run();

    window.free();
    context.free();
    root.free();
}
