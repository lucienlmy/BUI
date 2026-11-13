/* CSS tokens, declarations and lengths.
 *
 * The tokenizer follows CSS Syntax Level 3 closely enough for values:
 * numbers with units and percentages, identifiers, strings, hashes,
 * functions (arguments split at top-level commas), commas, slashes and
 * delimiters. Selectors have their own parser because a `>` or `~` means a
 * combinator there, not a delimiter.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.css.tokens;

/// A CSS component value.
enum TokenKind {
    number,
    dimension,
    percentage,
    identifier,
    hash,
    string_,
    function_,
    comma,
    slash,
    delimiter,
}

/// One component value. `text` is the unit, identifier, hash, string,
/// function name or delimiter; `args` holds a function's arguments split at
/// top-level commas.
struct Token {
    TokenKind kind;
    double number;
    string text;
    Token[][] args;
}

package bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

package string trim(string text) {
    size_t first = 0;
    size_t last = text.length;
    while (first < last && isSpace(text[first])) {
        ++first;
    }
    while (last > first && isSpace(text[last - 1])) {
        --last;
    }
    return text[first .. last];
}

package string asciiLower(string text) {
    char[] out_ = new char[text.length];
    foreach (i, c; text) {
        out_[i] = c >= 'A' && c <= 'Z' ? cast(char) (c + ('a' - 'A')) : c;
    }
    return cast(string) out_;
}

package bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

package bool isNameStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '-' || c == '_' || cast(ubyte) c >= 0x80;
}

package bool isNameChar(char c) {
    return isNameStart(c) || isDigit(c);
}

package size_t scanName(string text, size_t start) {
    size_t i = start;
    while (i < text.length && isNameChar(text[i])) {
        ++i;
    }
    return i;
}

package size_t matchingParen(string text, size_t open) {
    int depth = 0;
    char quote = '\0';
    size_t i = open;
    while (i < text.length) {
        const char c = text[i];
        if (quote != '\0') {
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == quote) {
                quote = '\0';
            }
        }
        else {
            if (c == '"' || c == '\'') {
                quote = c;
            }
            else if (c == '(') {
                ++depth;
            }
            else if (c == ')') {
                --depth;
                if (depth == 0) {
                    return i;
                }
            }
        }
        ++i;
    }
    return text.length;
}

/// Split `text` at `separator` outside parentheses, brackets and quotes.
package string[] splitTop(string text, char separator) {
    string[] parts;
    size_t start = 0;
    int depth = 0;
    char quote = '\0';
    size_t i = 0;
    while (i < text.length) {
        const char c = text[i];
        if (quote != '\0') {
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == quote) {
                quote = '\0';
            }
        }
        else {
            if (c == '"' || c == '\'') {
                quote = c;
            }
            else if (c == '(') {
                ++depth;
            }
            else if (c == ')') {
                --depth;
            }
            else if (c == '[') {
                ++depth;
            }
            else if (c == ']') {
                --depth;
            }
            else if (c == separator && depth == 0) {
                parts ~= text[start .. i];
                start = i + 1;
            }
        }
        ++i;
    }
    parts ~= text[start .. $];
    return parts;
}

/// Find `needle` outside parentheses, brackets and quotes; -1 when absent.
package ptrdiff_t findTop(string text, char needle) {
    int depth = 0;
    char quote = '\0';
    size_t i = 0;
    while (i < text.length) {
        const char c = text[i];
        if (quote != '\0') {
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == quote) {
                quote = '\0';
            }
        }
        else {
            if (c == '"' || c == '\'') {
                quote = c;
            }
            else if (c == '(' || c == '[') {
                ++depth;
            }
            else if (c == ')' || c == ']') {
                --depth;
            }
            else if (c == needle && depth == 0) {
                return cast(ptrdiff_t) i;
            }
        }
        ++i;
    }
    return -1;
}

/// Remove `/* ... *\/` comments.
package string stripComments(string text) {
    string out_;
    size_t i = 0;
    while (i < text.length) {
        if (i + 1 < text.length && text[i] == '/' && text[i + 1] == '*') {
            size_t end = i + 2;
            while (end + 1 < text.length &&
                   !(text[end] == '*' && text[end + 1] == '/')) {
                ++end;
            }
            i = end + 1 < text.length ? end + 2 : text.length;
        }
        else {
            out_ ~= text[i];
            ++i;
        }
    }
    return out_;
}

private double parseNumber(string text) {
    size_t i = 0;
    bool negative = false;
    if (i < text.length && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    double value = 0.0;
    while (i < text.length && text[i] >= '0' && text[i] <= '9') {
        value = value * 10.0 + (text[i] - '0');
        ++i;
    }
    if (i < text.length && text[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < text.length && text[i] >= '0' && text[i] <= '9') {
            value += (text[i] - '0') * scale;
            scale /= 10.0;
            ++i;
        }
    }
    if (i < text.length && (text[i] == 'e' || text[i] == 'E')) {
        const size_t expStart = i;
        ++i;
        bool expNegative = false;
        if (i < text.length && (text[i] == '+' || text[i] == '-')) {
            expNegative = text[i] == '-';
            ++i;
        }
        int exponent = 0;
        bool any = false;
        while (i < text.length && text[i] >= '0' && text[i] <= '9') {
            exponent = exponent * 10 + (text[i] - '0');
            any = true;
            ++i;
        }
        if (!any) {
            i = expStart;
        }
        else {
            double factor = 1.0;
            foreach (_; 0 .. exponent) {
                factor *= 10.0;
            }
            value = expNegative ? value / factor : value * factor;
        }
    }
    return negative ? -value : value;
}

/// Tokenize a CSS value (no selectors or blocks).
Token[] tokenize(string text) {
    Token[] out_;
    size_t i = 0;
    while (i < text.length) {
        const char c = text[i];
        if (isSpace(c)) {
            ++i;
        }
        else if (c == ',') {
            out_ ~= Token(TokenKind.comma);
            ++i;
        }
        else if (c == '/') {
            out_ ~= Token(TokenKind.slash);
            ++i;
        }
        else if (c == '"' || c == '\'') {
            const size_t start = i + 1;
            size_t end = start;
            while (end < text.length && text[end] != c) {
                if (text[end] == '\\' && end + 1 < text.length) {
                    ++end;
                }
                ++end;
            }
            out_ ~= Token(TokenKind.string_, 0, text[start .. end]);
            i = end < text.length ? end + 1 : end;
        }
        else if (c == '#') {
            const size_t end = scanName(text, i + 1);
            out_ ~= Token(TokenKind.hash, 0, asciiLower(text[i + 1 .. end]));
            i = end;
        }
        else if (isDigit(c) ||
                 (c == '.' && i + 1 < text.length && isDigit(text[i + 1])) ||
                 ((c == '-' || c == '+') && i + 1 < text.length &&
                  (isDigit(text[i + 1]) || text[i + 1] == '.'))) {
            size_t start = i;
            if (text[i] == '+' || text[i] == '-') {
                ++i;
            }
            while (i < text.length && (isDigit(text[i]) || text[i] == '.')) {
                ++i;
            }
            if (i < text.length && (text[i] == 'e' || text[i] == 'E')) {
                size_t e = i + 1;
                if (e < text.length && (text[e] == '+' || text[e] == '-')) {
                    ++e;
                }
                if (e < text.length && isDigit(text[e])) {
                    i = e;
                    while (i < text.length && isDigit(text[i])) {
                        ++i;
                    }
                }
            }
            const double value = parseNumber(text[start .. i]);
            if (i < text.length && text[i] == '%') {
                out_ ~= Token(TokenKind.percentage, value);
                ++i;
            }
            else {
                const size_t unitEnd = scanName(text, i);
                if (unitEnd > i) {
                    out_ ~= Token(TokenKind.dimension, value,
                                  asciiLower(text[i .. unitEnd]));
                    i = unitEnd;
                }
                else {
                    out_ ~= Token(TokenKind.number, value);
                }
            }
        }
        else if (isNameStart(c)) {
            const size_t end = scanName(text, i);
            const string name = asciiLower(text[i .. end]);
            if (end < text.length && text[end] == '(') {
                const size_t close = matchingParen(text, end);
                const string inner = text[end + 1 .. close < text.length ? close : text.length];
                Token[][] args;
                if (name == "url") {
                    args ~= [Token(TokenKind.string_, 0, trim(inner))];
                }
                else {
                    foreach (part; splitTop(inner, ',')) {
                        args ~= tokenize(part);
                    }
                }
                out_ ~= Token(TokenKind.function_, 0, name, args);
                i = close < text.length ? close + 1 : close;
            }
            else {
                out_ ~= Token(TokenKind.identifier, 0, name);
                i = end;
            }
        }
        else {
            out_ ~= Token(TokenKind.delimiter, 0, text[i .. i + 1]);
            ++i;
        }
    }
    return out_;
}

/// A declaration's tokens; `important` is the `!important` flag.
struct Declaration {
    string property;
    Token[] value;
    bool important;
}

/// Parse a declaration block: `color: red; padding: 4px 8px`.
Declaration[] parseDeclarations(string block) {
    Declaration[] out_;
    foreach (part; splitTop(stripComments(block), ';')) {
        const ptrdiff_t colon = findTop(part, ':');
        if (colon < 0) {
            continue;
        }
        const string name = trim(part[0 .. cast(size_t) colon]);
        if (name.length == 0) {
            continue;
        }
        string value = trim(part[cast(size_t) colon + 1 .. $]);
        bool important = false;
        if (value.length >= 10) {
            const string tail = asciiLower(value[$ - 10 .. $]);
            if (tail == "!important") {
                important = true;
                value = trim(value[0 .. $ - 10]);
            }
        }
        out_ ~= Declaration(asciiLower(name), tokenize(value), important);
    }
    return out_;
}

/// A length as written: a number, a unit, or `auto`.
enum LengthUnit {
    px,
    em,
    rem,
    percentage,
    vw,
    vh,
    pt,
    pc,
    cm,
    mm,
    ch,
    ex,
    unitless,
    auto_,
}

/// A length, before it is resolved against a font size and viewport.
struct Length {
    double value;
    LengthUnit unit;
}

private bool unitFromName(string name, out LengthUnit unit) {
    switch (name) {
    case "px":
        unit = LengthUnit.px;
        return true;
    case "em":
        unit = LengthUnit.em;
        return true;
    case "rem":
        unit = LengthUnit.rem;
        return true;
    case "vw":
        unit = LengthUnit.vw;
        return true;
    case "vh":
        unit = LengthUnit.vh;
        return true;
    case "vmin":
        unit = LengthUnit.vw; // approximated: the smaller axis is not known here
        return true;
    case "vmax":
        unit = LengthUnit.vh;
        return true;
    case "pt":
        unit = LengthUnit.pt;
        return true;
    case "pc":
        unit = LengthUnit.pc;
        return true;
    case "cm":
        unit = LengthUnit.cm;
        return true;
    case "mm":
        unit = LengthUnit.mm;
        return true;
    case "ch":
        unit = LengthUnit.ch;
        return true;
    case "ex":
        unit = LengthUnit.ex;
        return true;
    case "%":
        unit = LengthUnit.percentage;
        return true;
    default:
        return false;
    }
}

/// A single length token; `auto` and `none` are `auto_`.
bool tryLength(Token[] value, out Length length) {
    if (value.length != 1) {
        return false;
    }
    const t = value[0];
    switch (t.kind) {
    case TokenKind.number:
        length = Length(t.number, LengthUnit.unitless);
        return true;
    case TokenKind.percentage:
        length = Length(t.number, LengthUnit.percentage);
        return true;
    case TokenKind.dimension:
        LengthUnit unit;
        if (!unitFromName(t.text, unit)) {
            return false;
        }
        length = Length(t.number, unit);
        return true;
    case TokenKind.identifier:
        if (t.text == "auto" || t.text == "none" || t.text == "normal") {
            length = Length(0.0, LengthUnit.auto_);
            return true;
        }
        return false;
    default:
        return false;
    }
}

/// Resolve a length to pixels. `percentBase` is the containing block size for
/// percentages; 0 means the percentage cannot be resolved and becomes 0.
float resolveLength(Length length, float fontSize, float rootFontSize,
                    float viewportW, float viewportH, float percentBase = 0.0f) {
    const double v = length.value;
    final switch (length.unit) {
    case LengthUnit.px:
    case LengthUnit.unitless:
    case LengthUnit.auto_:
        return cast(float) v;
    case LengthUnit.em:
        return cast(float) (v * fontSize);
    case LengthUnit.rem:
        return cast(float) (v * rootFontSize);
    case LengthUnit.percentage:
        return cast(float) (v * percentBase / 100.0);
    case LengthUnit.vw:
        return cast(float) (v * viewportW / 100.0);
    case LengthUnit.vh:
        return cast(float) (v * viewportH / 100.0);
    case LengthUnit.pt:
        return cast(float) (v * 96.0 / 72.0);
    case LengthUnit.pc:
        return cast(float) (v * 16.0);
    case LengthUnit.cm:
        return cast(float) (v * 96.0 / 2.54);
    case LengthUnit.mm:
        return cast(float) (v * 96.0 / 25.4);
    case LengthUnit.ch:
    case LengthUnit.ex:
        return cast(float) (v * fontSize * 0.5);
    }
    return 0.0f;
}

/// The CSS 1-to-4-value shorthand (top, right, bottom, left).
bool tryLengthShorthand(Token[] value, out Length[4] out_) {
    Length[4] sides;
    if (value.length == 0 || value.length > 4) {
        return false;
    }
    foreach (i, token; value) {
        Length one;
        if (!tryLength([token], one)) {
            return false;
        }
        switch (i) {
        case 0:
            sides[0] = one;
            break;
        case 1:
            sides[1] = one;
            break;
        case 2:
            sides[2] = one;
            break;
        default:
            sides[3] = one;
            break;
        }
    }
    switch (value.length) {
    case 1:
        sides[1] = sides[0];
        sides[2] = sides[0];
        sides[3] = sides[0];
        break;
    case 2:
        sides[2] = sides[0];
        sides[3] = sides[1];
        break;
    case 3:
        sides[3] = sides[1];
        break;
    case 4:
        break;
    default:
        return false;
    }
    out_ = sides;
    return true;
}

/// Rebuild CSS text from tokens (used for colors and keyword values).
string tokenText(Token[] tokens) {
    string out_;
    foreach (i, t; tokens) {
        if (i) {
            out_ ~= " ";
        }
        final switch (t.kind) {
        case TokenKind.number:
            out_ ~= formatNumber(t.number);
            break;
        case TokenKind.dimension:
            out_ ~= formatNumber(t.number) ~ t.text;
            break;
        case TokenKind.percentage:
            out_ ~= formatNumber(t.number) ~ "%";
            break;
        case TokenKind.identifier:
        case TokenKind.string_:
        case TokenKind.delimiter:
            out_ ~= t.text;
            break;
        case TokenKind.hash:
            out_ ~= "#" ~ t.text;
            break;
        case TokenKind.function_:
            out_ ~= t.text ~ "(";
            foreach (j, arg; t.args) {
                if (j) {
                    out_ ~= ", ";
                }
                out_ ~= tokenText(arg);
            }
            out_ ~= ")";
            break;
        case TokenKind.comma:
            out_ ~= ",";
            break;
        case TokenKind.slash:
            out_ ~= "/";
            break;
        }
    }
    return out_;
}

private string formatNumber(double value) {
    if (value == cast(long) value) {
        return formatInteger(cast(long) value);
    }
    const long whole = cast(long) value;
    double fraction = value - whole;
    if (fraction < 0.0) {
        fraction = -fraction;
    }
    string digits;
    foreach (_; 0 .. 6) {
        fraction *= 10.0;
        const int digit = cast(int) fraction;
        digits ~= cast(char) ('0' + digit);
        fraction -= digit;
        if (fraction <= 0.000001) {
            break;
        }
    }
    while (digits.length > 1 && digits[$ - 1] == '0') {
        digits = digits[0 .. $ - 1];
    }
    return formatInteger(whole) ~ "." ~ digits;
}

private string formatInteger(long value) {
    if (value == 0) {
        return "0";
    }
    bool negative = value < 0;
    ulong v = negative ? cast(ulong) (-(value + 1)) + 1 : cast(ulong) value;
    char[24] buf;
    size_t i = buf.length;
    while (v) {
        buf[--i] = cast(char) ('0' + v % 10);
        v /= 10;
    }
    if (negative) {
        buf[--i] = '-';
    }
    return cast(string) buf[i .. $].idup;
}
