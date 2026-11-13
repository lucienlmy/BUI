/* BUI from D: an RSX page with CSS, a counter and hover styling.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
import bui;
import std.conv : to;

void main() {
    int count;
    Element counter;

    auto page = mixin(rsx!`
        <body class="app">
            <style>
                .app { background: #1e1e1e; padding: 12px; color: #e6e6e6; }
                h1 { font-size: 20px; margin: 0 0 8px 0; }
                .row { display: flex; flex-direction: row; gap: 6px; }
                button { flex: 1; }
                button:hover { color: #8fd4ff; }
            </style>
            <h1>BUI RSX</h1>
            <p id="count">Count: {count}</p>
            <div class="row">
                <button onclick={() { ++count; counter.setText("Count: " ~ to!string(count)); }}>Add</button>
                <button onclick={() { count = 0; counter.setText("Count: 0"); }}>Reset</button>
                <button onclick={() { quit(); }}>Quit</button>
            </div>
        </body>
    `);

    auto document = new Document(page);
    counter = document.find("#count");

    auto context = new Context(document.node());
    auto window = new Window(context, "BUI RSX", 360, 180);
    window.show();
    run();

    window.free();
    context.free();
}
