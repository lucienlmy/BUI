/* Whitespace and ASCII helpers shared by the HTML realiser and the `rsx!`
 * parser, so both collapse text the same way.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.html.text;

/// Whitespace in HTML's sense (space, tab, line breaks, form feed).
bool isHtmlSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/// Collapse whitespace runs to one space, keeping one space at each edge:
/// `" a  b "` stays `" a b "`. The layout pass trims the edges that touch a
/// block or the container, and keeps them between inline items.
string collapseWhitespace(string text) {
    size_t first = size_t.max;
    size_t last;
    foreach (i, c; text) {
        if (!isHtmlSpace(c)) {
            if (first == size_t.max) {
                first = i;
            }
            last = i;
        }
    }
    if (first == size_t.max) {
        return text.length ? " " : "";
    }
    char[] out_;
    if (first > 0) {
        out_ ~= ' ';
    }
    bool pending;
    foreach (c; text[first .. last + 1]) {
        if (isHtmlSpace(c)) {
            pending = true;
        }
        else {
            if (pending) {
                out_ ~= ' ';
                pending = false;
            }
            out_ ~= c;
        }
    }
    if (last + 1 < text.length) {
        out_ ~= ' ';
    }
    return cast(string) out_;
}

string trimStart(string text) {
    size_t i;
    while (i < text.length && isHtmlSpace(text[i])) {
        ++i;
    }
    return text[i .. $];
}

string trimEnd(string text) {
    size_t end = text.length;
    while (end > 0 && isHtmlSpace(text[end - 1])) {
        --end;
    }
    return text[0 .. end];
}

/// Collapsed and trimmed: the plain text of a leaf, a widget label or a
/// preformatted block drawn as one string.
string collapseTrimmed(string text) {
    return trimEnd(trimStart(collapseWhitespace(text)));
}

/// Lowercase ASCII letters only; tags and CSS identifiers are ASCII.
string asciiLower(string text) {
    char[] out_ = new char[text.length];
    foreach (i, c; text) {
        out_[i] = c >= 'A' && c <= 'Z' ? cast(char) (c + ('a' - 'A')) : c;
    }
    return cast(string) out_;
}
