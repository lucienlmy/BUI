/* CSS colors: the named colors, hex, and the rgb()/hsl() functions.
 *
 * Channels are straight (non-premultiplied) alpha in 0..1, which is what the
 * kit's `BK_set_color` expects. `currentcolor` and the CSS-wide keywords are
 * resolved by the cascade, so parsing them here reports "not a color".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.css.color;

/// A color with straight alpha, channels in 0..1.
struct Color {
    float r;
    float g;
    float b;
    float a = 1.0f;

    /// `0xRRGGBB`, opaque.
    static Color rgb(uint hex) {
        return Color(
            ((hex >> 16) & 0xff) / 255.0f,
            ((hex >> 8) & 0xff) / 255.0f,
            (hex & 0xff) / 255.0f,
            1.0f);
    }

    /// `0xRRGGBBAA`.
    static Color rgba(uint hex) {
        return Color(
            ((hex >> 24) & 0xff) / 255.0f,
            ((hex >> 16) & 0xff) / 255.0f,
            ((hex >> 8) & 0xff) / 255.0f,
            (hex & 0xff) / 255.0f);
    }

    /// Fully transparent black.
    static Color transparent() {
        return Color(0.0f, 0.0f, 0.0f, 0.0f);
    }

    /// The same color with alpha `value`.
    Color alpha(float value) const {
        return Color(r, g, b, value);
    }

    /// True when the color cannot change a pixel.
    bool isTransparent() const {
        return a <= 0.0f;
    }

    /// The RGBA array the kit takes.
    float[4] toArray() const {
        return [r, g, b, a];
    }
}

private struct NamedColor {
    string name;
    uint rgb;
}

/// The 148 CSS named colors (CSS Color Module Level 4).
private immutable NamedColor[] namedColors = [
    NamedColor("aliceblue", 0xf0f8ff), NamedColor("antiquewhite", 0xfaebd7),
    NamedColor("aqua", 0x00ffff), NamedColor("aquamarine", 0x7fffd4),
    NamedColor("azure", 0xf0ffff), NamedColor("beige", 0xf5f5dc),
    NamedColor("bisque", 0xffe4c4), NamedColor("black", 0x000000),
    NamedColor("blanchedalmond", 0xffebcd), NamedColor("blue", 0x0000ff),
    NamedColor("blueviolet", 0x8a2be2), NamedColor("brown", 0xa52a2a),
    NamedColor("burlywood", 0xdeb887), NamedColor("cadetblue", 0x5f9ea0),
    NamedColor("chartreuse", 0x7fff00), NamedColor("chocolate", 0xd2691e),
    NamedColor("coral", 0xff7f50), NamedColor("cornflowerblue", 0x6495ed),
    NamedColor("cornsilk", 0xfff8dc), NamedColor("crimson", 0xdc143c),
    NamedColor("cyan", 0x00ffff), NamedColor("darkblue", 0x00008b),
    NamedColor("darkcyan", 0x008b8b), NamedColor("darkgoldenrod", 0xb8860b),
    NamedColor("darkgray", 0xa9a9a9), NamedColor("darkgreen", 0x006400),
    NamedColor("darkgrey", 0xa9a9a9), NamedColor("darkkhaki", 0xbdb76b),
    NamedColor("darkmagenta", 0x8b008b), NamedColor("darkolivegreen", 0x556b2f),
    NamedColor("darkorange", 0xff8c00), NamedColor("darkorchid", 0x9932cc),
    NamedColor("darkred", 0x8b0000), NamedColor("darksalmon", 0xe9967a),
    NamedColor("darkseagreen", 0x8fbc8f), NamedColor("darkslateblue", 0x483d8b),
    NamedColor("darkslategray", 0x2f4f4f), NamedColor("darkslategrey", 0x2f4f4f),
    NamedColor("darkturquoise", 0x00ced1), NamedColor("darkviolet", 0x9400d3),
    NamedColor("deeppink", 0xff1493), NamedColor("deepskyblue", 0x00bfff),
    NamedColor("dimgray", 0x696969), NamedColor("dimgrey", 0x696969),
    NamedColor("dodgerblue", 0x1e90ff), NamedColor("firebrick", 0xb22222),
    NamedColor("floralwhite", 0xfffaf0), NamedColor("forestgreen", 0x228b22),
    NamedColor("fuchsia", 0xff00ff), NamedColor("gainsboro", 0xdcdcdc),
    NamedColor("ghostwhite", 0xf8f8ff), NamedColor("gold", 0xffd700),
    NamedColor("goldenrod", 0xdaa520), NamedColor("gray", 0x808080),
    NamedColor("green", 0x008000), NamedColor("greenyellow", 0xadff2f),
    NamedColor("grey", 0x808080), NamedColor("honeydew", 0xf0fff0),
    NamedColor("hotpink", 0xff69b4), NamedColor("indianred", 0xcd5c5c),
    NamedColor("indigo", 0x4b0082), NamedColor("ivory", 0xfffff0),
    NamedColor("khaki", 0xf0e68c), NamedColor("lavender", 0xe6e6fa),
    NamedColor("lavenderblush", 0xfff0f5), NamedColor("lawngreen", 0x7cfc00),
    NamedColor("lemonchiffon", 0xfffacd), NamedColor("lightblue", 0xadd8e6),
    NamedColor("lightcoral", 0xf08080), NamedColor("lightcyan", 0xe0ffff),
    NamedColor("lightgoldenrodyellow", 0xfafad2), NamedColor("lightgray", 0xd3d3d3),
    NamedColor("lightgreen", 0x90ee90), NamedColor("lightgrey", 0xd3d3d3),
    NamedColor("lightpink", 0xffb6c1), NamedColor("lightsalmon", 0xffa07a),
    NamedColor("lightseagreen", 0x20b2aa), NamedColor("lightskyblue", 0x87cefa),
    NamedColor("lightslategray", 0x778899), NamedColor("lightslategrey", 0x778899),
    NamedColor("lightsteelblue", 0xb0c4de), NamedColor("lightyellow", 0xffffe0),
    NamedColor("lime", 0x00ff00), NamedColor("limegreen", 0x32cd32),
    NamedColor("linen", 0xfaf0e6), NamedColor("magenta", 0xff00ff),
    NamedColor("maroon", 0x800000), NamedColor("mediumaquamarine", 0x66cdaa),
    NamedColor("mediumblue", 0x0000cd), NamedColor("mediumorchid", 0xba55d3),
    NamedColor("mediumpurple", 0x9370db), NamedColor("mediumseagreen", 0x3cb371),
    NamedColor("mediumslateblue", 0x7b68ee), NamedColor("mediumspringgreen", 0x00fa9a),
    NamedColor("mediumturquoise", 0x48d1cc), NamedColor("mediumvioletred", 0xc71585),
    NamedColor("midnightblue", 0x191970), NamedColor("mintcream", 0xf5fffa),
    NamedColor("mistyrose", 0xffe4e1), NamedColor("moccasin", 0xffe4b5),
    NamedColor("navajowhite", 0xffdead), NamedColor("navy", 0x000080),
    NamedColor("oldlace", 0xfdf5e6), NamedColor("olive", 0x808000),
    NamedColor("olivedrab", 0x6b8e23), NamedColor("orange", 0xffa500),
    NamedColor("orangered", 0xff4500), NamedColor("orchid", 0xda70d6),
    NamedColor("palegoldenrod", 0xeee8aa), NamedColor("palegreen", 0x98fb98),
    NamedColor("paleturquoise", 0xafeeee), NamedColor("palevioletred", 0xdb7093),
    NamedColor("papayawhip", 0xffefd5), NamedColor("peachpuff", 0xffdab9),
    NamedColor("peru", 0xcd853f), NamedColor("pink", 0xffc0cb),
    NamedColor("plum", 0xdda0dd), NamedColor("powderblue", 0xb0e0e6),
    NamedColor("purple", 0x800080), NamedColor("rebeccapurple", 0x663399),
    NamedColor("red", 0xff0000), NamedColor("rosybrown", 0xbc8f8f),
    NamedColor("royalblue", 0x4169e1), NamedColor("saddlebrown", 0x8b4513),
    NamedColor("salmon", 0xfa8072), NamedColor("sandybrown", 0xf4a460),
    NamedColor("seagreen", 0x2e8b57), NamedColor("seashell", 0xfff5ee),
    NamedColor("sienna", 0xa0522d), NamedColor("silver", 0xc0c0c0),
    NamedColor("skyblue", 0x87ceeb), NamedColor("slateblue", 0x6a5acd),
    NamedColor("slategray", 0x708090), NamedColor("slategrey", 0x708090),
    NamedColor("snow", 0xfffafa), NamedColor("springgreen", 0x00ff7f),
    NamedColor("steelblue", 0x4682b4), NamedColor("tan", 0xd2b48c),
    NamedColor("teal", 0x008080), NamedColor("thistle", 0xd8bfd8),
    NamedColor("tomato", 0xff6347), NamedColor("turquoise", 0x40e0d0),
    NamedColor("violet", 0xee82ee), NamedColor("wheat", 0xf5deb3),
    NamedColor("white", 0xffffff), NamedColor("whitesmoke", 0xf5f5f5),
    NamedColor("yellow", 0xffff00), NamedColor("yellowgreen", 0x9acd32),
];

private bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

private string trimText(string text) {
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

private string lowerText(string text) {
    char[] out_ = new char[text.length];
    foreach (i, c; text) {
        out_[i] = c >= 'A' && c <= 'Z' ? cast(char) (c + ('a' - 'A')) : c;
    }
    return cast(string) out_;
}

private int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

private bool hexColor(string digits, out Color color) {
    enum int[] lengths = [3, 4, 6, 8];
    bool known = false;
    foreach (len; lengths) {
        known = known || digits.length == len;
    }
    if (!known) {
        return false;
    }
    uint value = 0;
    foreach (c; digits) {
        const int d = hexDigit(c);
        if (d < 0) {
            return false;
        }
        value = (value << 4) | cast(uint) d;
    }
    if (digits.length == 3 || digits.length == 4) {
        uint expanded = 0;
        for (int i = 0; i < digits.length; ++i) {
            const uint nibble = (value >> (4 * (digits.length - 1 - i))) & 0xf;
            expanded = (expanded << 8) | (nibble * 0x11);
        }
        value = expanded;
        if (digits.length == 3) {
            value = (value << 8) | 0xff;
        }
    }
    else if (digits.length == 6) {
        value = (value << 8) | 0xff;
    }
    color = Color.rgba(value);
    return true;
}

/// One numeric component of a color function; percentages are normalized.
private struct NumberArg {
    double value;
    bool percent;
}

/// Split function arguments on commas and whitespace, keeping `/` as a marker.
private NumberArg[] colorArgs(string inner, bool[] slashes) {
    NumberArg[] out_;
    size_t i = 0;
    while (i < inner.length) {
        const char c = inner[i];
        if (isSpace(c) || c == ',') {
            ++i;
        }
        else if (c == '/') {
            slashes ~= true;
            ++i;
        }
        else {
            const size_t start = i;
            if (inner[i] == '+' || inner[i] == '-') {
                ++i;
            }
            while (i < inner.length &&
                   ((inner[i] >= '0' && inner[i] <= '9') || inner[i] == '.')) {
                ++i;
            }
            if (i == start || (i == start + 1 && (inner[start] == '+' || inner[start] == '-'))) {
                // A keyword such as `none`: skip it.
                while (i < inner.length && !isSpace(inner[i]) && inner[i] != ',' && inner[i] != '/') {
                    ++i;
                }
                continue;
            }
            bool percent = false;
            if (i < inner.length && inner[i] == '%') {
                percent = true;
                ++i;
            }
            double value = 0.0;
            bool negative = false;
            size_t k = start;
            if (inner[k] == '+' || inner[k] == '-') {
                negative = inner[k] == '-';
                ++k;
            }
            double scale = 1.0;
            bool dot = false;
            while (k < i && (inner[k] != '%')) {
                if (inner[k] == '.') {
                    dot = true;
                    scale = 1.0;
                }
                else if (inner[k] >= '0' && inner[k] <= '9') {
                    if (dot) {
                        scale /= 10.0;
                        value += (inner[k] - '0') * scale;
                    }
                    else {
                        value = value * 10.0 + (inner[k] - '0');
                    }
                }
                ++k;
            }
            out_ ~= NumberArg(negative ? -value : value, percent);
        }
    }
    return out_;
}

private double clamp01(double v) {
    return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
}

private double absValue(double v) {
    return v < 0.0 ? -v : v;
}

private void hslToRgb(double h, double s, double l, out double r, out double g, out double b) {
    h = h / 360.0;
    h -= cast(long) h;
    if (h < 0.0) {
        h += 1.0;
    }
    const double c = (1.0 - absValue(2.0 * l - 1.0)) * s;
    const double hp = h * 6.0;
    const double x = c * (1.0 - absValue((hp % 2.0) - 1.0));
    double rr, gg, bb;
    switch (cast(int) hp % 6) {
    case 0:
        rr = c; gg = x; bb = 0.0;
        break;
    case 1:
        rr = x; gg = c; bb = 0.0;
        break;
    case 2:
        rr = 0.0; gg = c; bb = x;
        break;
    case 3:
        rr = 0.0; gg = x; bb = c;
        break;
    case 4:
        rr = x; gg = 0.0; bb = c;
        break;
    default:
        rr = c; gg = 0.0; bb = x;
        break;
    }
    const double m = l - c / 2.0;
    r = rr + m;
    g = gg + m;
    b = bb + m;
}

private bool parseColorFunction(string name, string inner, out Color color) {
    bool[] slashes;
    auto args = colorArgs(inner, slashes);
    if (args.length < 3) {
        return false;
    }
    double alpha = 1.0;
    if (args.length >= 4) {
        alpha = args[3].percent ? args[3].value / 100.0 : args[3].value;
    }
    alpha = clamp01(alpha);
    if (name == "rgb" || name == "rgba") {
        double r = args[0].percent ? args[0].value * 2.55 : args[0].value;
        double g = args[1].percent ? args[1].value * 2.55 : args[1].value;
        double b = args[2].percent ? args[2].value * 2.55 : args[2].value;
        color = Color(cast(float) clamp01(r / 255.0),
                      cast(float) clamp01(g / 255.0),
                      cast(float) clamp01(b / 255.0),
                      cast(float) alpha);
        return true;
    }
    if (name == "hsl" || name == "hsla") {
        const double h = args[0].value;
        const double s = args[1].percent ? args[1].value / 100.0 : clamp01(args[1].value);
        const double l = args[2].percent ? args[2].value / 100.0 : clamp01(args[2].value);
        double r, g, b;
        hslToRgb(h, clamp01(s), clamp01(l), r, g, b);
        color = Color(cast(float) r, cast(float) g, cast(float) b, cast(float) alpha);
        return true;
    }
    return false;
}

/// Parse a CSS color. Returns false for invalid colors and for `currentcolor`
/// or a CSS-wide keyword (the cascade resolves those).
bool tryParseColor(string text, out Color color) {
    const string s = trimText(text);
    if (s.length == 0) {
        return false;
    }
    const string lower = lowerText(s);
    if (lower == "transparent") {
        color = Color.transparent();
        return true;
    }
    if (lower == "currentcolor" || lower == "inherit" || lower == "initial" ||
        lower == "unset" || lower == "revert" || lower == "revert-layer") {
        return false;
    }
    if (s[0] == '#') {
        return hexColor(s[1 .. $], color);
    }
    size_t open;
    while (open < s.length && s[open] != '(') {
        ++open;
    }
    if (open < s.length && s[$ - 1] == ')') {
        return parseColorFunction(lowerText(trimText(s[0 .. open])),
                                  s[open + 1 .. $ - 1], color);
    }
    foreach (named; namedColors) {
        if (named.name == lower) {
            color = Color.rgb(named.rgb);
            return true;
        }
    }
    return false;
}
