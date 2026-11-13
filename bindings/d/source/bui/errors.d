/* The exception thrown when the C kit refuses a request (allocation failure,
 * an unknown node type, a bad handle).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.errors;

/// A BUI operation failed; the message says which one and why.
class BuiException : Exception {
    this(string operation, string reason = null) {
        super(reason.length ? operation ~ ": " ~ reason : operation);
    }
}
