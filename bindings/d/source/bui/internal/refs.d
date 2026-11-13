/* Reference-count hooks for host objects stored in the C kit.
 *
 * The kit keeps `void*` references to host objects (a node's wrapper, a menu
 * item's callback, a tree item) and calls the hooks installed here when it
 * takes and drops them. D objects are not visible to the C side, so a retain
 * makes the object a GC root and the matching release removes it. This is the
 * same contract Python's binding uses with INCREF/DECREF.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.internal.refs;

import core.memory : GC;
import bui.kit : BK_set_ref_hooks;

private __gshared {
    size_t[void*] g_counts;
    bool g_installed;
}

/// Install the hooks once, before the first reference reaches the kit.
void ensureRefHooks() {
    if (g_installed) {
        return;
    }
    g_installed = true;
    BK_set_ref_hooks(&buiRefRetain, &buiRefRelease);
}

private extern(C) void buiRefRetain(void* ref_) {
    if (ref_ is null) {
        return;
    }
    auto p = ref_ in g_counts;
    if (p is null) {
        g_counts[ref_] = 1;
        GC.addRoot(ref_);
    }
    else {
        ++*p;
    }
}

private extern(C) void buiRefRelease(void* ref_) {
    if (ref_ is null) {
        return;
    }
    auto p = ref_ in g_counts;
    if (p is null || *p == 0) {
        return;
    }
    if (--*p == 0) {
        g_counts.remove(ref_);
        GC.removeRoot(ref_);
    }
}

/// How many times the kit retains `ref_` right now.
size_t refCount(void* ref_) {
    auto p = ref_ in g_counts;
    return p is null ? 0 : *p;
}
