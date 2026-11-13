/* Headless BUI from D: render a frame and drive a button without a window.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
import bui;
import std.stdio;

void main() {
    auto root = new Node("column");
    root.set("padding", 12);

    auto title = new Node("label");
    title.set("text", "BUI from D");
    root.add(title);

    int clicks;
    auto button = new Node("button");
    button.set("text", "Click me");
    button.onClick((NodeEvent) {
        ++clicks;
    });
    root.add(button);
    button.pack("fill", 1);

    auto context = new Context(root);
    auto raster = new Raster(320, 200);

    const regions = context.render(raster, true);
    writeln("rendered ", regions, " region(s) into ", raster.width, "x", raster.height,
            " (", raster.pixels.length, " pixels)");

    const rect = button.rect();
    context.press(rect[0] + rect[2] / 2, rect[1] + rect[3] / 2, 0);
    context.release(rect[0] + rect[2] / 2, rect[1] + rect[3] / 2, 0);
    writeln("clicks: ", clicks, "; needs render: ", context.needsRender());

    raster.free();
    context.free();
    root.free();
}
