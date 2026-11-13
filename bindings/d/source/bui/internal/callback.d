/* One heap object for a D callback passed to the kit as a `void*` reference.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.internal.callback;

import bui.internal.refs : ensureRefHooks;

/// The kit retains this while a menu, key binding or tree item can call it.
final class CallbackBox {
    void delegate() dg;

    this(void delegate() dg) {
        ensureRefHooks();
        this.dg = dg;
    }

    void call() {
        if (dg !is null) {
            dg();
        }
    }
}
