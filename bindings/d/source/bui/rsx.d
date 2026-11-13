/* `rsx!`: HTML markup that becomes `Element` code at compile time.
 *
 * ```d
 * auto page = mixin(rsx!q{
 *     <body class="app">
 *         <h1>Hello</h1>
 *         <p>Count: {count}</p>
 *         <button onclick={increment}>+1</button>
 *     </body>
 * });
 * ```
 *
 * The parser runs in CTFE and lowers each element to a chain of
 * `bui.html.element.makeElement` calls, so the generated tree has the same
 * types, event handlers and CSS support as the tag constructors. `{expr}`
 * interpolates a D expression as a child (an `Element`, a string or any
 * value `toText` accepts) or as an attribute value; event attributes
 * (`onclick`, `onchange`, ...) take a D delegate.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.rsx;

public import bui.html.element;

import bui.html.text : asciiLower, collapseWhitespace, isHtmlSpace;

/// The markup lowered to an `Element` expression.
template rsx(string source) {
    enum string rsx = buildRsx(source);
}

private immutable string[] voidTags = [
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
    "param", "source", "track", "wbr",
];

private immutable string[] rawTags = ["style", "script", "textarea", "title"];

private immutable string[] booleanAttributes = [
    "allowfullscreen", "async", "autofocus", "autoplay", "checked", "controls",
    "default", "defer", "disabled", "formnovalidate", "hidden", "inert", "loop",
    "multiple", "muted", "nomodule", "novalidate", "open", "playsinline",
    "readonly", "required", "reversed", "selected",
];

private string buildRsx(string source) {
    size_t i;
    string expression;
    int roots;
    while (i < source.length) {
        skipSpace(source, i);
        if (i >= source.length) {
            break;
        }
        if (startsWith(source, i, "<!--")) {
            skipComment(source, i);
            continue;
        }
        if (startsWithIgnoreCase(source, i, "<!doctype")) {
            skipPast(source, i, '>');
            continue;
        }
        if (source[i] != '<') {
            throw new Exception("rsx: text outside an element");
        }
        if (i + 1 < source.length && source[i + 1] == '/') {
            throw new Exception("rsx: unexpected closing tag");
        }
        expression = parseElement(source, i);
        ++roots;
    }
    if (roots != 1) {
        throw new Exception("rsx: expected exactly one root element");
    }
    return expression;
}

private string parseElement(string source, ref size_t i) {
    assert(source[i] == '<');
    ++i;

    const size_t nameStart = i;
    while (i < source.length && isNameChar(source[i])) {
        ++i;
    }
    if (i == nameStart) {
        throw new Exception("rsx: empty tag name");
    }
    const string tag = asciiLower(source[nameStart .. i]);

    string expression = "bui.html.element.makeElement!\"";
    expression ~= tag;
    expression ~= "\"()";

    // Attributes.
    while (i < source.length) {
        skipSpace(source, i);
        if (i >= source.length) {
            throw new Exception("rsx: unterminated tag <" ~ tag ~ ">");
        }
        if (source[i] == '>') {
            ++i;
            break;
        }
        if (source[i] == '/' && i + 1 < source.length && source[i + 1] == '>') {
            i += 2;
            return expression;
        }
        const size_t attributeStart = i;
        while (i < source.length && isAttributeChar(source[i])) {
            ++i;
        }
        if (i == attributeStart) {
            throw new Exception("rsx: expected an attribute name in <" ~ tag ~ ">");
        }
        const string name = asciiLower(source[attributeStart .. i]);
        skipSpace(source, i);

        if (i < source.length && source[i] == '=') {
            ++i;
            skipSpace(source, i);
            if (i >= source.length) {
                throw new Exception("rsx: missing value for attribute " ~ name);
            }
            if (source[i] == '"' || source[i] == '\'') {
                const char quote = source[i++];
                const size_t valueStart = i;
                while (i < source.length && source[i] != quote) {
                    ++i;
                }
                if (i >= source.length) {
                    throw new Exception("rsx: unterminated value for attribute " ~ name);
                }
                const string value = decodeEntities(source[valueStart .. i]);
                ++i;
                expression ~= staticAttribute(name, value);
            }
            else if (source[i] == '{') {
                const string value = readBraceExpression(source, i);
                expression ~= dynamicAttribute(name, value);
            }
            else {
                const size_t valueStart = i;
                while (i < source.length && !isHtmlSpace(source[i]) &&
                       source[i] != '>' &&
                       !(source[i] == '/' && i + 1 < source.length && source[i + 1] == '>')) {
                    ++i;
                }
                const string value = decodeEntities(source[valueStart .. i]);
                expression ~= staticAttribute(name, value);
            }
        }
        else {
            // A boolean attribute: `disabled`, `checked`, `hidden`, ...
            expression ~= ".attr(" ~ dQuote(name) ~ ")";
        }
    }

    if (isVoid(tag)) {
        return expression;
    }

    if (isRaw(tag)) {
        const size_t close = findClosingTag(source, i, tag);
        const string content = source[i .. close];
        if (tag == "style") {
            expression ~= ".append(" ~ dQuote(content) ~ ")";
        }
        else if (tag != "script") {
            // textarea and title are RCDATA: entities decode, tags do not.
            expression ~= ".append(" ~ dQuote(decodeEntities(content)) ~ ")";
        }
        // `script` is metadata: the tree keeps no code.
        i = close;
        i += 2 + tag.length; // `</tag`
        skipSpace(source, i);
        if (i >= source.length || source[i] != '>') {
            throw new Exception("rsx: malformed closing tag </" ~ tag ~ ">");
        }
        ++i;
        return expression;
    }

    // Content.
    while (i < source.length) {
        if (startsWith(source, i, "<!--")) {
            skipComment(source, i);
            continue;
        }
        if (startsWith(source, i, "</")) {
            const size_t closeStart = i;
            i += 2;
            const size_t closeNameStart = i;
            while (i < source.length && isNameChar(source[i])) {
                ++i;
            }
            const string closing = asciiLower(source[closeNameStart .. i]);
            skipSpace(source, i);
            if (i >= source.length || source[i] != '>') {
                i = closeStart;
                throw new Exception("rsx: malformed closing tag in <" ~ tag ~ ">");
            }
            ++i;
            if (closing != tag) {
                throw new Exception("rsx: expected </" ~ tag ~ ">, found </" ~ closing ~ ">");
            }
            return expression;
        }
        if (source[i] == '<') {
            expression ~= ".append(" ~ parseElement(source, i) ~ ")";
            continue;
        }
        if (source[i] == '{') {
            expression ~= ".append(" ~ readBraceExpression(source, i) ~ ")";
            continue;
        }
        const size_t textStart = i;
        while (i < source.length && source[i] != '<' && source[i] != '{') {
            ++i;
        }
        const string text = collapseWhitespace(decodeEntities(source[textStart .. i]));
        if (text.length) {
            expression ~= ".append(" ~ dQuote(text) ~ ")";
        }
    }
    throw new Exception("rsx: missing </" ~ tag ~ ">");
}

private string staticAttribute(string name, string value) {
    return attributeCall(name, dQuote(value), false);
}

private string dynamicAttribute(string name, string expression) {
    const string event = eventMethod(name);
    if (event.length) {
        return "." ~ event ~ "(" ~ expression ~ ")";
    }
    if (isBooleanAttribute(name)) {
        return ".flagAttr(" ~ dQuote(name) ~ ", " ~ expression ~ ")";
    }
    return attributeCall(name, "bui.html.element.toText(" ~ expression ~ ")", true);
}

private string attributeCall(string name, string argument, bool dynamic) {
    const string event = eventMethod(name);
    if (event.length) {
        if (!dynamic) {
            throw new Exception("rsx: event attribute " ~ name ~ " needs {expression}");
        }
        return "." ~ event ~ "(" ~ argument ~ ")";
    }
    switch (name) {
    case "id":
        return ".id(" ~ argument ~ ")";
    case "class":
        return ".class_(" ~ argument ~ ")";
    case "style":
        return ".style(" ~ argument ~ ")";
    default:
        return ".attr(" ~ dQuote(name) ~ ", " ~ argument ~ ")";
    }
}

/// The `Element` method for an HTML event attribute, or empty when the
/// attribute is not an event we bind.
private string eventMethod(string name) {
    switch (name) {
    case "onclick":
        return "onClick";
    case "ondblclick":
        return "onDoubleClick";
    case "onmousedown":
    case "onpointerdown":
        return "onPress";
    case "onmouseup":
    case "onpointerup":
        return "onRelease";
    case "onchange":
    case "oninput":
        return "onChange";
    case "onsubmit":
        return "onSubmit";
    case "ontoggle":
        return "onToggle";
    case "onkeydown":
    case "onkeyup":
    case "onkeypress":
        return "onKey";
    case "onwheel":
        return "onWheel";
    default:
        return null;
    }
}

private bool isVoid(string tag) {
    foreach (value; voidTags) {
        if (value == tag) {
            return true;
        }
    }
    return false;
}

private bool isRaw(string tag) {
    foreach (value; rawTags) {
        if (value == tag) {
            return true;
        }
    }
    return false;
}

private bool isBooleanAttribute(string name) {
    foreach (value; booleanAttributes) {
        if (value == name) {
            return true;
        }
    }
    return false;
}

private bool isNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':' || c == '.';
}

private bool isAttributeChar(char c) {
    return isNameChar(c) || c == '@';
}

private void skipSpace(string source, ref size_t i) {
    while (i < source.length && isHtmlSpace(source[i])) {
        ++i;
    }
}

private bool startsWith(string source, size_t at, string prefix) {
    if (at + prefix.length > source.length) {
        return false;
    }
    return source[at .. at + prefix.length] == prefix;
}

private bool startsWithIgnoreCase(string source, size_t at, string prefix) {
    if (at + prefix.length > source.length) {
        return false;
    }
    foreach (k, c; prefix) {
        char value = source[at + k];
        if (value >= 'A' && value <= 'Z') {
            value = cast(char) (value + ('a' - 'A'));
        }
        char wanted = c;
        if (wanted >= 'A' && wanted <= 'Z') {
            wanted = cast(char) (wanted + ('a' - 'A'));
        }
        if (value != wanted) {
            return false;
        }
    }
    return true;
}

private void skipComment(string source, ref size_t i) {
    const size_t end = findFrom(source, i + 4, "-->");
    if (end == size_t.max) {
        throw new Exception("rsx: unterminated comment");
    }
    i = end + 3;
}

private void skipPast(string source, ref size_t i, char marker) {
    while (i < source.length && source[i] != marker) {
        ++i;
    }
    if (i >= source.length) {
        throw new Exception("rsx: unterminated declaration");
    }
    ++i;
}

private size_t findFrom(string source, size_t from, string needle) {
    if (needle.length == 0 || from > source.length ||
        from + needle.length > source.length) {
        return size_t.max;
    }
    foreach (i; from .. source.length - needle.length + 1) {
        if (source[i .. i + needle.length] == needle) {
            return i;
        }
    }
    return size_t.max;
}

private size_t findClosingTag(string source, size_t from, string tag) {
    string needle = "</" ~ tag;
    foreach (i; from .. source.length) {
        if (startsWithIgnoreCase(source, i, needle)) {
            return i;
        }
    }
    throw new Exception("rsx: missing </" ~ tag ~ ">");
}

/// Read a balanced `{...}` and return the D expression inside.
private string readBraceExpression(string source, ref size_t i) {
    assert(source[i] == '{');
    ++i;
    const size_t start = i;
    int depth = 1;
    while (i < source.length) {
        const char c = source[i];
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++i;
            while (i < source.length && source[i] != quote) {
                if (source[i] == '\\') {
                    ++i;
                }
                ++i;
            }
            if (i >= source.length) {
                throw new Exception("rsx: unterminated string in {...}");
            }
            ++i;
        }
        else if (c == '`') {
            ++i;
            while (i < source.length && source[i] != '`') {
                ++i;
            }
            if (i >= source.length) {
                throw new Exception("rsx: unterminated string in {...}");
            }
            ++i;
        }
        else if (c == '/' && i + 1 < source.length && source[i + 1] == '/') {
            while (i < source.length && source[i] != '\n') {
                ++i;
            }
        }
        else if (c == '/' && i + 1 < source.length && source[i + 1] == '*') {
            const size_t end = findFrom(source, i + 2, "*/");
            if (end == size_t.max) {
                throw new Exception("rsx: unterminated comment in {...}");
            }
            i = end + 2;
        }
        else if (c == '{') {
            ++depth;
            ++i;
        }
        else if (c == '}') {
            --depth;
            if (depth == 0) {
                const string expression = source[start .. i];
                ++i;
                return expression;
            }
            ++i;
        }
        else {
            ++i;
        }
    }
    throw new Exception("rsx: unterminated {...}");
}

/// Escape `text` as a D double-quoted string literal.
private string dQuote(string text) {
    char[] out_ = ['"'];
    foreach (c; text) {
        switch (c) {
        case '"':
            out_ ~= "\\\"";
            break;
        case '\\':
            out_ ~= "\\\\";
            break;
        case '\n':
            out_ ~= "\\n";
            break;
        case '\r':
            out_ ~= "\\r";
            break;
        case '\t':
            out_ ~= "\\t";
            break;
        default:
            if (c < 0x20) {
                out_ ~= "\\x";
                out_ ~= hexDigit(c >> 4);
                out_ ~= hexDigit(c & 0x0F);
            }
            else {
                out_ ~= c;
            }
            break;
        }
    }
    out_ ~= '"';
    return cast(string) out_;
}

private char hexDigit(int value) {
    return cast(char) (value < 10 ? '0' + value : 'a' + value - 10);
}

/// Decode the HTML entities that appear in markup; unknown entities stay
/// literal. `&nbsp;` is U+00A0, which HTML does not collapse as whitespace.
private string decodeEntities(string text) {
    if (findFrom(text, 0, "&") == size_t.max) {
        return text;
    }
    char[] out_;
    size_t i;
    while (i < text.length) {
        if (text[i] != '&') {
            out_ ~= text[i++];
            continue;
        }
        const size_t semicolon = findFrom(text, i + 1, ";");
        if (semicolon == size_t.max || semicolon - i > 12) {
            out_ ~= text[i++];
            continue;
        }
        const string entity = text[i + 1 .. semicolon];
        uint codepoint;
        if (entity.length && entity[0] == '#') {
            if (!parseCodepoint(entity[1 .. $], codepoint)) {
                out_ ~= text[i++];
                continue;
            }
        }
        else if (!namedEntity(entity, codepoint)) {
            out_ ~= text[i++];
            continue;
        }
        appendCodepoint(out_, codepoint);
        i = semicolon + 1;
    }
    return cast(string) out_;
}

private bool namedEntity(string name, out uint codepoint) {
    switch (name) {
    case "amp":
        codepoint = '&';
        return true;
    case "lt":
        codepoint = '<';
        return true;
    case "gt":
        codepoint = '>';
        return true;
    case "quot":
        codepoint = '"';
        return true;
    case "apos":
        codepoint = '\'';
        return true;
    case "nbsp":
        codepoint = 0x00A0;
        return true;
    case "copy":
        codepoint = 0x00A9;
        return true;
    case "reg":
        codepoint = 0x00AE;
        return true;
    case "hellip":
        codepoint = 0x2026;
        return true;
    case "mdash":
        codepoint = 0x2014;
        return true;
    case "ndash":
        codepoint = 0x2013;
        return true;
    case "times":
        codepoint = 0x00D7;
        return true;
    default:
        return false;
    }
}

private bool parseCodepoint(string digits, out uint codepoint) {
    if (digits.length == 0) {
        return false;
    }
    int base = 10;
    size_t i;
    if (digits[0] == 'x' || digits[0] == 'X') {
        base = 16;
        i = 1;
    }
    if (i >= digits.length) {
        return false;
    }
    uint value;
    foreach (c; digits[i .. $]) {
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        }
        else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        }
        else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        }
        else {
            return false;
        }
        value = value * base + digit;
    }
    codepoint = value;
    return true;
}

private void appendCodepoint(ref char[] out_, uint codepoint) {
    if (codepoint <= 0x7F) {
        out_ ~= cast(char) codepoint;
    }
    else if (codepoint <= 0x7FF) {
        out_ ~= cast(char) (0xC0 | (codepoint >> 6));
        out_ ~= cast(char) (0x80 | (codepoint & 0x3F));
    }
    else if (codepoint <= 0xFFFF) {
        out_ ~= cast(char) (0xE0 | (codepoint >> 12));
        out_ ~= cast(char) (0x80 | ((codepoint >> 6) & 0x3F));
        out_ ~= cast(char) (0x80 | (codepoint & 0x3F));
    }
    else {
        out_ ~= cast(char) (0xF0 | (codepoint >> 18));
        out_ ~= cast(char) (0x80 | ((codepoint >> 12) & 0x3F));
        out_ ~= cast(char) (0x80 | ((codepoint >> 6) & 0x3F));
        out_ ~= cast(char) (0x80 | (codepoint & 0x3F));
    }
}
