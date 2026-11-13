/* Small shared helpers for the D binding: NUL-terminated C strings and
 * zero-copy slices of C strings.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.internal.util;

import core.stdc.string : strlen;
import std.string : toStringz;

/// A NUL-terminated copy view; `toStringz` returns the input when it already is.
const(char)* cstr(const(char)[] s) {
    return toStringz(s);
}

/// A slice over a C string, without copying; null becomes an empty slice.
const(char)[] cslice(const(char)* s) {
    if (s is null) {
        return null;
    }
    return s[0 .. strlen(s)];
}

/// A slice over `n` bytes of C memory, without copying.
const(ubyte)[] cbytes(const(ubyte)* p, size_t n) {
    if (p is null || n == 0) {
        return null;
    }
    return p[0 .. n];
}
