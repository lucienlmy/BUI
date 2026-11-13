/* CSS cascade: declarations become a computed `Style`.
 *
 * The property table is deliberately a subset of what the kit can draw:
 * layout (block/flex/grid), box edges, colors, typography, borders and
 * cursors. Properties the kit cannot represent (shadows, transforms,
 * transitions, absolute positioning, media queries) are parsed and ignored;
 * see the D README for the supported list.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.css.style;

import std.algorithm.sorting : sort;

import bui.css.color : Color, tryParseColor;
import bui.css.selectors : CssSubject, Selector, matchesSelector, parseSelectorList;
import bui.css.tokens : Declaration, Length, LengthUnit, Token, TokenKind,
    asciiLower, findTop, isSpace, matchingParen, parseDeclarations, resolveLength,
    splitTop, stripComments, tokenText, trim, tryLength, tryLengthShorthand;

/// How an element participates in layout.
enum Display {
    none,
    block,
    inline_,
    inlineBlock,
    flex,
    inlineFlex,
    grid,
    inlineGrid,
    listItem,
    contents,
    table,
    tableRow,
    tableCell,
}

/// The main axis of a flexible container.
enum FlexDirection {
    row,
    rowReverse,
    column,
    columnReverse,
}

/// A pointer shape; mapped to `BK_CURSOR_*` when the node is realised.
enum Cursor {
    default_,
    ewResize,
    nsResize,
    text,
    crosshair,
    hand,
    move,
    none_,
    neswResize,
    nwseResize,
    notAllowed,
    wait,
    progress,
}

/// The computed values of the properties the renderer understands.
struct Style {
    /* -- inherited --------------------------------------------------------- */
    /// Text color.
    Color color = Color(0.0f, 0.0f, 0.0f, 1.0f);
    /// Font size in logical pixels.
    float fontSize = 16.0f;
    /// 100..900.
    int fontWeight = 400;
    /// `0` left, `1` center, `2` right (`BK_ALIGN_*`).
    int textAlign;
    /// Custom properties (`--name`) with `var()` already resolved.
    const(Token)[][string] custom;
    /// Per-element family; the kit only has a global UI font, kept for
    /// introspection.
    string fontFamily;
    /// Parsed but not drawn by the kit's text renderer.
    bool italic;

    /* -- box --------------------------------------------------------------- */
    /// `visibility`; inherited.
    bool visible = true;
    Display display = Display.block;
    FlexDirection flexDirection = FlexDirection.column;
    int gridColumns = 1;
    float rowGap;
    float columnGap;
    /// Top, right, bottom, left in pixels.
    float[4] padding;
    /// Top, right, bottom, left in pixels.
    float[4] margin;
    /// A positive width/height is a fixed size; 0 or less is automatic.
    float width;
    float height;
    float flexGrow;
    Color background = Color(0.0f, 0.0f, 0.0f, 0.0f);
    bool hasBackground;
    Color borderColor = Color(0.0f, 0.0f, 0.0f, 0.0f);
    float borderWidth;
    int cursor = -1;
    /// `BK_FIT_*`, from `object-fit`.
    int objectFit = 1;
}

/// One style rule.
struct Rule {
    Selector[] selectors;
    Declaration[] declarations;
    int order;
}

/// A rule and the selector that matched.
struct RuleMatch {
    size_t rule;
    size_t selector;
}

private bool specGreater(const Selector a, const Selector b) {
    if (a.ids != b.ids) {
        return a.ids > b.ids;
    }
    if (a.classes != b.classes) {
        return a.classes > b.classes;
    }
    return a.types > b.types;
}

private size_t matchingBrace(string text, size_t open) {
    int depth = 0;
    char quote = '\0';
    size_t i = open;
    while (i < text.length) {
        const char c = text[i];
        if (quote != '\0') {
            if (c == '\\') {
                ++i;
            }
            else if (c == quote) {
                quote = '\0';
            }
        }
        else if (c == '"' || c == '\'') {
            quote = c;
        }
        else if (c == '{') {
            ++depth;
        }
        else if (c == '}') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
        ++i;
    }
    return text.length;
}

private size_t skipAtRule(string text, size_t start) {
    size_t i = start + 1;
    char quote = '\0';
    int depth = 0;
    while (i < text.length) {
        const char c = text[i];
        if (quote != '\0') {
            if (c == '\\') {
                ++i;
            }
            else if (c == quote) {
                quote = '\0';
            }
        }
        else if (c == '"' || c == '\'') {
            quote = c;
        }
        else if (c == '(') {
            ++depth;
        }
        else if (c == ')') {
            --depth;
        }
        else if (depth == 0 && c == ';') {
            return i + 1;
        }
        else if (depth == 0 && c == '{') {
            const size_t close = matchingBrace(text, i);
            return close < text.length ? close + 1 : close;
        }
        ++i;
    }
    return text.length;
}

/// A parsed stylesheet with a small subject-key index.
///
/// Rules are kept in source order; matching a subject gathers the id, class,
/// tag and universal buckets, then checks the selectors. The cascade uses
/// `!important`, then specificity, then source order.
final class Stylesheet {
    private Rule[] rules_;
    private size_t[][string] byId_;
    private size_t[][string] byClass_;
    private size_t[][string] byTag_;
    private size_t[] universal_;
    private int order_;

    /// Parse `css`; the same instance can be extended with `add`.
    static Stylesheet parse(string css) {
        auto sheet = new Stylesheet();
        sheet.add(css);
        return sheet;
    }

    /// Append rules from `css`.
    void add(string css) {
        const string text = stripComments(css);
        size_t i = 0;
        while (i < text.length) {
            while (i < text.length && isSpace(text[i])) {
                ++i;
            }
            if (i >= text.length) {
                break;
            }
            if (text[i] == '@') {
                i = skipAtRule(text, i);
                continue;
            }
            const ptrdiff_t relative = findTop(text[i .. $], '{');
            if (relative < 0) {
                break;
            }
            const size_t open = i + cast(size_t) relative;
            const size_t close = matchingBrace(text, open);
            const string selectorText = trim(text[i .. open]);
            const string body = text[open + 1 .. close < text.length ? close : text.length];
            auto selectors = parseSelectorList(selectorText, false);
            if (selectors.length) {
                auto declarations = parseDeclarations(body);
                if (declarations.length) {
                    addRule(selectors, declarations);
                }
            }
            i = close < text.length ? close + 1 : close;
        }
    }

    /// Append one already-parsed rule.
    void addRule(Selector[] selectors, Declaration[] declarations) {
        if (selectors.length == 0 || declarations.length == 0) {
            return;
        }
        const size_t index = rules_.length;
        rules_ ~= Rule(selectors, declarations, order_++);
        foreach (selector; selectors) {
            if (selector.compounds.length == 0 || selector.pseudoElement) {
                continue;
            }
            const subject = selector.compounds[$ - 1];
            if (subject.id.length) {
                byId_[subject.id] ~= index;
            }
            else if (subject.classes.length) {
                foreach (name; subject.classes) {
                    byClass_[name] ~= index;
                }
            }
            else if (subject.tag.length) {
                byTag_[subject.tag] ~= index;
            }
            else {
                universal_ ~= index;
            }
        }
    }

    /// The rules that match `subject`, with the best selector of each rule.
    RuleMatch[] matching(CssSubject subject) const {
        RuleMatch[] result;
        if (rules_.length == 0) {
            return result;
        }
        bool[] seen = new bool[rules_.length];
        size_t[] indices;
        void gather(const size_t[] bucket) {
            foreach (index; bucket) {
                if (!seen[index]) {
                    seen[index] = true;
                    indices ~= index;
                }
            }
        }

        const string id = subject.cssId();
        if (id.length && id in byId_) {
            gather(byId_[id]);
        }
        foreach (name; subject.cssClasses()) {
            if (name in byClass_) {
                gather(byClass_[name]);
            }
        }
        const string tag = subject.cssTag();
        if (tag.length && tag in byTag_) {
            gather(byTag_[tag]);
        }
        gather(universal_);

        // Buckets are appended in source order, but a subject can match
        // several; sort before walking.
        indices.sort();
        foreach (index; indices) {
            const Rule* rule = &rules_[index];
            size_t best = size_t.max;
            foreach (j, selector; rule.selectors) {
                if (matchesSelector(subject, selector) &&
                    (best == size_t.max || specGreater(selector, rule.selectors[best]))) {
                    best = j;
                }
            }
            if (best != size_t.max) {
                result ~= RuleMatch(index, best);
            }
        }
        return result;
    }

    /// The number of parsed rules.
    size_t ruleCount() const {
        return rules_.length;
    }

    /// Cascade the matching rules and `inlineStyle` into a computed style.
    /// `parent` supplies inherited values; `rootFontSize` resolves `rem` and
    /// the viewport resolves `vw`/`vh`.
    Style compute(CssSubject subject, Style parent, string inlineStyle = null,
                  float rootFontSize = 16.0f, float viewportW = 0.0f,
                  float viewportH = 0.0f) const {
        Style style = parent;
        resetNonInherited(style, parent);
        style.custom = parent.custom.dup;

        Applied[] applied;
        long sequence;
        foreach (match; matching(subject)) {
            const Rule* rule = &rules_[match.rule];
            const Selector selector = rule.selectors[match.selector];
            foreach (i, declaration; rule.declarations) {
                applied ~= Applied(&rule.declarations[i], selector.ids, selector.classes,
                                   selector.types, sequence++);
            }
        }
        if (inlineStyle.length) {
            // Parse once: the pointers must stay valid until the cascade is
            // applied, and the array must not be rebuilt per declaration.
            const Declaration[] inlineDeclarations = parseDeclarations(inlineStyle);
            foreach (i, declaration; inlineDeclarations) {
                // Inline styles beat every selector, as in CSS.
                applied ~= Applied(&inlineDeclarations[i], 1_000_000, 0, 0, sequence++);
            }
        }
        applied.sort!((a, b) {
            if (a.declaration.important != b.declaration.important) {
                return !a.declaration.important;
            }
            if (a.ids != b.ids) {
                return a.ids < b.ids;
            }
            if (a.classes != b.classes) {
                return a.classes < b.classes;
            }
            if (a.types != b.types) {
                return a.types < b.types;
            }
            return a.sequence < b.sequence;
        });

        // Custom properties first: every later var() sees the cascaded set.
        const(Token)[][string] env;
        foreach (key, value; style.custom) {
            env[key] = value;
        }
        foreach (entry; applied) {
            const string property = entry.declaration.property;
            if (property.length > 2 && property[0] == '-' && property[1] == '-') {
                bool ok;
                Token[] resolved = resolveVars(entry.declaration.value, env, 0, ok);
                if (ok) {
                    env[property] = resolved;
                }
            }
        }
        style.custom = env;

        // Font properties first so `em` and `ex` in other declarations use
        // the element's final font size, not a half-applied one.
        foreach (entry; applied) {
            if (isFontProperty(entry.declaration.property)) {
                applyDeclaration(style, *entry.declaration, parent, env,
                                 rootFontSize, viewportW, viewportH);
            }
        }
        foreach (entry; applied) {
            if (!isFontProperty(entry.declaration.property)) {
                applyDeclaration(style, *entry.declaration, parent, env,
                                 rootFontSize, viewportW, viewportH);
            }
        }
        return style;
    }
}

private struct Applied {
    const(Declaration)* declaration;
    uint ids;
    uint classes;
    uint types;
    long sequence;
}

private bool isFontProperty(string property) {
    return property == "font" || property == "font-size" ||
           property == "font-weight" || property == "font-style" ||
           property == "font-family" || property == "color" ||
           property == "text-align";
}

private void resetNonInherited(ref Style style, const Style parent) {
    style.display = Display.block;
    style.flexDirection = FlexDirection.column;
    style.gridColumns = 1;
    style.rowGap = 0.0f;
    style.columnGap = 0.0f;
    style.padding = [0.0f, 0.0f, 0.0f, 0.0f];
    style.margin = [0.0f, 0.0f, 0.0f, 0.0f];
    style.width = 0.0f;
    style.height = 0.0f;
    style.flexGrow = 0.0f;
    style.background = Color.transparent();
    style.hasBackground = false;
    style.borderColor = Color.transparent();
    style.borderWidth = 0.0f;
    style.cursor = -1;
    style.objectFit = 1;
    // `visible`, typography and custom properties stay inherited from
    // `parent`; the field initializers above only apply to a fresh Style.
    style.visible = parent.visible;
}

private bool tokenIsIdent(const Token token, string name) {
    return token.kind == TokenKind.identifier && token.text == name;
}

private float lengthPx(Length length, const Style style, float rootFontSize,
                       float viewportW, float viewportH, float percentBase = 0.0f) {
    return resolveLength(length, style.fontSize, rootFontSize, viewportW, viewportH,
                         percentBase);
}

private bool parseColorValue(Token[] value, out Color color) {
    return tryParseColor(tokenText(value), color);
}

private void parseBackground(ref Style style, Token[] value) {
    if (value.length == 1 && tokenIsIdent(value[0], "none")) {
        style.hasBackground = false;
        style.background = Color.transparent();
        return;
    }
    foreach (token; value) {
        Color color;
        if (tryParseColor(tokenText([token]), color)) {
            style.hasBackground = !color.isTransparent();
            style.background = color;
            return;
        }
    }
    // `background: transparent` is a color too, but keep it explicit.
    Color whole;
    if (tryParseColor(tokenText(value), whole)) {
        style.hasBackground = !whole.isTransparent();
        style.background = whole;
    }
}

private void parseBorder(ref Style style, Token[] value) {
    style.borderWidth = 0.0f;
    style.borderColor = Color.transparent();
    foreach (token; value) {
        Length length;
        if (tryLength([token], length)) {
            style.borderWidth = lengthPx(length, style, 16.0f, 0.0f, 0.0f);
            continue;
        }
        Color color;
        if (tryParseColor(tokenText([token]), color)) {
            style.borderColor = color;
        }
    }
    if (style.borderColor.isTransparent()) {
        style.borderColor = Color(0.5f, 0.5f, 0.5f, 1.0f);
    }
}

private Token[] resolveVars(const(Token)[] value, const(Token)[][string] env, int depth, out bool ok) {
    ok = true;
    if (depth > 32) {
        ok = false;
        return null;
    }
    Token[] out_;
    foreach (token; value) {
        if (token.kind == TokenKind.function_ && token.text == "var") {
            string name;
            if (token.args.length && token.args[0].length &&
                token.args[0][0].kind == TokenKind.identifier) {
                name = token.args[0][0].text;
            }
            if (name.length && name in env) {
                bool innerOk;
                Token[] replacement = resolveVars(env[name], env, depth + 1, innerOk);
                if (!innerOk) {
                    ok = false;
                    return null;
                }
                out_ ~= replacement;
            }
            else if (token.args.length > 1) {
                bool innerOk;
                Token[] fallback = resolveVars(token.args[1], env, depth + 1, innerOk);
                if (!innerOk) {
                    ok = false;
                    return null;
                }
                out_ ~= fallback;
            }
            else {
                ok = false;
                return null;
            }
        }
        else if (token.kind == TokenKind.function_) {
            Token copy = Token(token.kind, token.number, token.text);
            Token[][] args;
            foreach (argument; token.args) {
                bool innerOk;
                Token[] resolved = resolveVars(argument, env, depth + 1, innerOk);
                if (!innerOk) {
                    ok = false;
                    return null;
                }
                args ~= resolved;
            }
            copy.args = args;
            out_ ~= copy;
        }
        else {
            out_ ~= Token(token.kind, token.number, token.text);
        }
    }
    return out_;
}

private int parseFontWeight(Token[] value, int fallback) {
    foreach (token; value) {
        if (token.kind == TokenKind.number) {
            const int weight = cast(int) token.number;
            return weight < 100 ? 100 : weight > 900 ? 900 : weight;
        }
        if (token.kind == TokenKind.identifier) {
            switch (token.text) {
            case "normal":
                return 400;
            case "bold":
                return 700;
            case "bolder":
                return 700;
            case "lighter":
                return 300;
            default:
                break;
            }
        }
    }
    return fallback;
}

private float parseFontSize(Token[] value, const Style parent, float rootFontSize,
                            float viewportW, float viewportH) {
    Length length;
    if (tryLength(value, length)) {
        if (length.unit == LengthUnit.percentage) {
            return parent.fontSize * cast(float) (length.value / 100.0);
        }
        if (length.unit == LengthUnit.em) {
            return parent.fontSize * cast(float) length.value;
        }
        if (length.unit == LengthUnit.rem) {
            return rootFontSize * cast(float) length.value;
        }
        return resolveLength(length, parent.fontSize, rootFontSize, viewportW, viewportH);
    }
    if (value.length == 1 && value[0].kind == TokenKind.identifier) {
        switch (value[0].text) {
        case "xx-small":
            return 9.0f;
        case "x-small":
            return 10.0f;
        case "small":
            return 13.0f;
        case "medium":
            return 16.0f;
        case "large":
            return 18.0f;
        case "x-large":
            return 24.0f;
        case "xx-large":
            return 32.0f;
        case "smaller":
            return parent.fontSize * 0.83f;
        case "larger":
            return parent.fontSize * 1.2f;
        default:
            break;
        }
    }
    return parent.fontSize;
}

private int parseAlign(Token[] value) {
    foreach (token; value) {
        if (token.kind == TokenKind.identifier) {
            switch (token.text) {
            case "center":
                return 1;
            case "right":
            case "end":
                return 2;
            case "left":
            case "start":
            case "justify":
                return 0;
            default:
                break;
            }
        }
    }
    return 0;
}

private int parseCursor(Token[] value) {
    foreach (token; value) {
        if (token.kind != TokenKind.identifier) {
            continue;
        }
        switch (token.text) {
        case "default":
            return Cursor.default_;
        case "pointer":
        case "hand":
            return Cursor.hand;
        case "text":
            return Cursor.text;
        case "crosshair":
            return Cursor.crosshair;
        case "move":
            return Cursor.move;
        case "wait":
            return Cursor.wait;
        case "progress":
            return Cursor.progress;
        case "not-allowed":
            return Cursor.notAllowed;
        case "ew-resize":
            return Cursor.ewResize;
        case "ns-resize":
            return Cursor.nsResize;
        case "nesw-resize":
            return Cursor.neswResize;
        case "nwse-resize":
            return Cursor.nwseResize;
        case "none":
            return Cursor.none_;
        default:
            break;
        }
    }
    return -1;
}

private int parseObjectFit(Token[] value) {
    foreach (token; value) {
        if (token.kind != TokenKind.identifier) {
            continue;
        }
        switch (token.text) {
        case "fill":
            return 0;
        case "contain":
            return 1;
        case "cover":
            return 2;
        case "none":
            return 3;
        case "scale-down":
            return 4;
        default:
            break;
        }
    }
    return 1;
}

private void applyDeclaration(ref Style style, const Declaration declaration,
                              const Style parent, const(Token)[][string] env,
                              float rootFontSize, float viewportW, float viewportH) {
    bool ok;
    Token[] value = resolveVars(declaration.value, env, 0, ok);
    if (!ok) {
        return;
    }
    if (value.length == 1 && value[0].kind == TokenKind.identifier) {
        const string keyword = value[0].text;
        if (keyword == "inherit") {
            switch (declaration.property) {
            case "color":
                style.color = parent.color;
                return;
            case "font-size":
                style.fontSize = parent.fontSize;
                return;
            case "font-weight":
                style.fontWeight = parent.fontWeight;
                return;
            case "text-align":
                style.textAlign = parent.textAlign;
                return;
            case "visibility":
                style.visible = parent.visible;
                return;
            default:
                break;
            }
        }
        if (keyword == "initial" || keyword == "unset" || keyword == "revert") {
            // The common cases; unsupported properties already fall through.
            switch (declaration.property) {
            case "color":
                style.color = Color(0.0f, 0.0f, 0.0f, 1.0f);
                return;
            case "font-size":
                style.fontSize = 16.0f;
                return;
            case "font-weight":
                style.fontWeight = 400;
                return;
            case "text-align":
                style.textAlign = 0;
                return;
            case "display":
                style.display = Display.block;
                return;
            case "visibility":
                style.visible = true;
                return;
            default:
                break;
            }
        }
    }
    const string property = declaration.property;
    switch (property) {
    case "color":
        if (value.length == 1 && tokenIsIdent(value[0], "currentcolor")) {
            style.color = parent.color;
        }
        else {
            Color color;
            if (parseColorValue(value, color)) {
                style.color = color;
            }
        }
        break;
    case "background":
    case "background-color":
        parseBackground(style, value);
        break;
    case "font-size":
        style.fontSize = parseFontSize(value, parent, rootFontSize, viewportW, viewportH);
        if (style.fontSize < 1.0f) {
            style.fontSize = 1.0f;
        }
        break;
    case "font-weight":
        style.fontWeight = parseFontWeight(value, parent.fontWeight);
        break;
    case "font-style":
        foreach (token; value) {
            if (token.kind == TokenKind.identifier && token.text == "italic") {
                style.italic = true;
            }
        }
        break;
    case "font-family":
        style.fontFamily = tokenText(value);
        break;
    case "text-align":
        style.textAlign = parseAlign(value);
        break;
    case "display":
        foreach (token; value) {
            if (token.kind != TokenKind.identifier) {
                continue;
            }
            switch (token.text) {
            case "none":
                style.display = Display.none;
                break;
            case "block":
                style.display = Display.block;
                break;
            case "inline":
                style.display = Display.inline_;
                break;
            case "inline-block":
                style.display = Display.inlineBlock;
                break;
            case "flex":
                style.display = Display.flex;
                break;
            case "inline-flex":
                style.display = Display.inlineFlex;
                break;
            case "grid":
                style.display = Display.grid;
                break;
            case "inline-grid":
                style.display = Display.inlineGrid;
                break;
            case "list-item":
                style.display = Display.listItem;
                break;
            case "contents":
                style.display = Display.contents;
                break;
            case "table":
                style.display = Display.table;
                break;
            case "table-row":
                style.display = Display.tableRow;
                break;
            case "table-cell":
                style.display = Display.tableCell;
                break;
            default:
                break;
            }
        }
        break;
    case "flex-direction":
        foreach (token; value) {
            if (token.kind != TokenKind.identifier) {
                continue;
            }
            switch (token.text) {
            case "row":
                style.flexDirection = FlexDirection.row;
                break;
            case "row-reverse":
                style.flexDirection = FlexDirection.rowReverse;
                break;
            case "column":
                style.flexDirection = FlexDirection.column;
                break;
            case "column-reverse":
                style.flexDirection = FlexDirection.columnReverse;
                break;
            default:
                break;
            }
        }
        break;
    case "flex-grow":
        foreach (token; value) {
            if (token.kind == TokenKind.number) {
                style.flexGrow = cast(float) token.number;
            }
        }
        break;
    case "flex":
        foreach (token; value) {
            if (token.kind == TokenKind.number) {
                style.flexGrow = cast(float) token.number;
                break;
            }
            if (token.kind == TokenKind.identifier && token.text == "auto") {
                style.flexGrow = 1.0f;
                break;
            }
        }
        break;
    case "gap":
    case "grid-gap": {
        Length[4] sides;
        if (tryLengthShorthand(value, sides)) {
            style.rowGap = lengthPx(sides[0], style, rootFontSize, viewportW, viewportH);
            style.columnGap = lengthPx(sides[1], style, rootFontSize, viewportW, viewportH);
        }
        break;
    }
    case "row-gap":
    case "grid-row-gap": {
        Length length;
        if (tryLength(value, length)) {
            style.rowGap = lengthPx(length, style, rootFontSize, viewportW, viewportH);
        }
        break;
    }
    case "column-gap":
    case "grid-column-gap": {
        Length length;
        if (tryLength(value, length)) {
            style.columnGap = lengthPx(length, style, rootFontSize, viewportW, viewportH);
        }
        break;
    }
    case "padding":
    case "padding-top":
    case "padding-right":
    case "padding-bottom":
    case "padding-left": {
        Length[4] sides;
        if (property == "padding" && tryLengthShorthand(value, sides)) {
            foreach (i; 0 .. 4) {
                style.padding[i] = lengthPx(sides[i], style, rootFontSize, viewportW, viewportH);
            }
        }
        else {
            Length one;
            if (tryLength(value, one)) {
                const int index = property == "padding-top" ? 0
                                : property == "padding-right" ? 1
                                : property == "padding-bottom" ? 2 : 3;
                style.padding[index] = lengthPx(one, style, rootFontSize, viewportW, viewportH);
            }
        }
        break;
    }
    case "margin":
    case "margin-top":
    case "margin-right":
    case "margin-bottom":
    case "margin-left": {
        Length[4] sides;
        if (property == "margin" && tryLengthShorthand(value, sides)) {
            foreach (i; 0 .. 4) {
                style.margin[i] = lengthPx(sides[i], style, rootFontSize, viewportW, viewportH);
            }
        }
        else {
            Length one;
            if (tryLength(value, one)) {
                const int index = property == "margin-top" ? 0
                                : property == "margin-right" ? 1
                                : property == "margin-bottom" ? 2 : 3;
                style.margin[index] = lengthPx(one, style, rootFontSize, viewportW, viewportH);
            }
        }
        break;
    }
    case "width":
    case "min-width": {
        Length length;
        if (tryLength(value, length)) {
            const float px = lengthPx(length, style, rootFontSize, viewportW, viewportH);
            if (px > 0.0f && (property == "width" || px > style.width)) {
                style.width = px;
            }
        }
        break;
    }
    case "height":
    case "min-height": {
        Length length;
        if (tryLength(value, length)) {
            const float px = lengthPx(length, style, rootFontSize, viewportW, viewportH);
            if (px > 0.0f && (property == "height" || px > style.height)) {
                style.height = px;
            }
        }
        break;
    }
    case "border":
        parseBorder(style, value);
        break;
    case "border-width": {
        Length length;
        if (tryLength(value, length)) {
            style.borderWidth = lengthPx(length, style, rootFontSize, viewportW, viewportH);
        }
        break;
    }
    case "border-color":
        foreach (token; value) {
            Color color;
            if (tryParseColor(tokenText([token]), color)) {
                style.borderColor = color;
                break;
            }
        }
        break;
    case "border-style":
        // The kit draws a solid edge; other styles are accepted and ignored.
        break;
    case "cursor":
        style.cursor = parseCursor(value);
        break;
    case "visibility":
        foreach (token; value) {
            if (token.kind == TokenKind.identifier) {
                style.visible = token.text == "visible";
            }
        }
        break;
    case "grid-template-columns":
        style.gridColumns = 0;
        foreach (token; value) {
            if (token.kind == TokenKind.function_ && token.text == "repeat") {
                if (token.args.length && token.args[0].length &&
                    token.args[0][0].kind == TokenKind.number) {
                    style.gridColumns += cast(int) token.args[0][0].number;
                }
            }
            else if (token.kind == TokenKind.dimension ||
                     token.kind == TokenKind.percentage ||
                     (token.kind == TokenKind.identifier && token.text == "auto")) {
                style.gridColumns += 1;
            }
        }
        if (style.gridColumns < 1) {
            style.gridColumns = 1;
        }
        break;
    case "object-fit":
        style.objectFit = parseObjectFit(value);
        break;
    default:
        break;
    }
}
