/* CSS selectors: parsing, specificity and matching.
 *
 * A selector is stored left to right; matching walks right to left, the way
 * browsers do, so the subject's tag, class or id is checked first. The
 * `CssSubject` interface keeps this module independent of the HTML tree:
 * `bui.html.element.Element` is one implementation, a test can be another.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.css.selectors;

import bui.css.tokens : asciiLower, findTop, isDigit, isNameChar, isNameStart,
    isSpace, matchingParen, scanName, splitTop, trim;

/// What a selector can ask about an element.
interface CssSubject {
    string cssTag();
    string cssId();
    /// Every class, for the stylesheet's class index.
    string[] cssClasses();
    bool cssHasClass(string name);
    bool cssHasAttr(string name);
    string cssAttr(string name);
    bool cssState(string name);
    CssSubject cssParent();
    CssSubject cssPrevSibling();
    CssSubject cssNextSibling();
    /// 1-based position among element siblings.
    int cssPosition();
    /// Number of element siblings, including this one.
    int cssSiblingCount();
    /// 1-based position among siblings with the same tag.
    int cssTypePosition();
    /// Number of siblings with the same tag, including this one.
    int cssTypeCount();
    /// No element children and no text.
    bool cssIsEmpty();
}

/// A selector combinator.
enum Combinator {
    descendant,
    child,
    adjacent,
    sibling,
}

/// An attribute selector operator.
enum AttrOp {
    exists,
    equal,
    word,
    prefix,
    suffix,
    substring,
    dash,
}

/// `[name op value i]`.
struct AttrSelector {
    string name;
    AttrOp op;
    string value;
    bool caseInsensitive;
}

/// `:nth-child(an+b)` and its variants.
struct Nth {
    int a;
    int b;
    bool last;
    bool ofType;
}

/// A compound selector: `div.card#main[data-x]:hover:not(.hidden)`.
struct Compound {
    /// Lowercase tag, or empty for the universal selector.
    string tag;
    string id;
    string[] classes;
    AttrSelector[] attrs;
    string[] pseudos;
    Nth[] nths;
    Selector[] notSelectors;
    Selector[] isSelectors;
    Selector[] whereSelectors;
    /// A construct this engine can parse but not realise (`:has()`,
    /// pseudo-elements); a compound with it never matches.
    bool never;
}

/// A complex selector: compounds joined left to right.
struct Selector {
    Compound[] compounds;
    /// `combinators[i]` joins `compounds[i]` and `compounds[i + 1]`.
    Combinator[] combinators;
    uint ids;
    uint classes;
    uint types;
    /// `::before` and friends: parsed, never generated.
    bool pseudoElement;
}

private bool ieq(string a, string b) {
    if (a.length != b.length) {
        return false;
    }
    foreach (i, c; a) {
        const char ac = c >= 'A' && c <= 'Z' ? cast(char) (c + ('a' - 'A')) : c;
        const char bc = b[i] >= 'A' && b[i] <= 'Z' ? cast(char) (b[i] + ('a' - 'A')) : b[i];
        if (ac != bc) {
            return false;
        }
    }
    return true;
}

private bool parseInteger(string text, out int value) {
    string s = trim(text);
    if (s.length == 0) {
        return false;
    }
    size_t i = 0;
    bool negative = false;
    if (s[i] == '+' || s[i] == '-') {
        negative = s[i] == '-';
        ++i;
    }
    if (i >= s.length) {
        return false;
    }
    int v = 0;
    while (i < s.length && isDigit(s[i])) {
        v = v * 10 + (s[i] - '0');
        ++i;
    }
    if (i != s.length) {
        return false;
    }
    value = negative ? -v : v;
    return true;
}

private bool parseNth(string text, out Nth nth) {
    string s = asciiLower(trim(text));
    char[] compact;
    foreach (c; s) {
        if (!isSpace(c)) {
            compact ~= c;
        }
    }
    s = cast(string) compact;
    if (s == "odd") {
        nth = Nth(2, 1, false, false);
        return true;
    }
    if (s == "even") {
        nth = Nth(2, 0, false, false);
        return true;
    }
    const ptrdiff_t n = findTop(s, 'n');
    if (n < 0) {
        int value;
        if (!parseInteger(s, value)) {
            return false;
        }
        nth = Nth(0, value, false, false);
        return true;
    }
    const size_t at = cast(size_t) n;
    int a = 1;
    if (at > 0) {
        const string coef = s[0 .. at];
        if (coef == "+") {
            a = 1;
        }
        else if (coef == "-") {
            a = -1;
        }
        else if (!parseInteger(coef, a)) {
            return false;
        }
    }
    else {
        a = 1;
    }
    int b = 0;
    if (at + 1 < s.length) {
        string rest = s[at + 1 .. $];
        if (rest.length > 0 && (rest[0] == '+' || rest[0] == '-')) {
            // parseInteger accepts the sign
        }
        else {
            return false;
        }
        if (!parseInteger(rest, b)) {
            return false;
        }
    }
    nth = Nth(a, b, false, false);
    return true;
}

private bool nthOk(int a, int b, int position) {
    if (a == 0) {
        return position == b;
    }
    const int delta = position - b;
    if (delta % a != 0) {
        return false;
    }
    return delta / a >= 0;
}

private size_t compoundEnd(string text, size_t start) {
    int depth = 0;
    char quote = '\0';
    size_t i = start;
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
        else if (c == '"' || c == '\'') {
            quote = c;
        }
        else if (c == '[' || c == '(') {
            ++depth;
        }
        else if (c == ']' || c == ')') {
            --depth;
        }
        else if (depth == 0 && (isSpace(c) || c == '>' || c == '+' || c == '~')) {
            return i;
        }
        ++i;
    }
    return text.length;
}

private string unquote(string text) {
    string s = trim(text);
    if (s.length < 2) {
        return s;
    }
    const char first = s[0];
    if ((first == '"' || first == '\'') && s[$ - 1] == first) {
        return s[1 .. $ - 1];
    }
    return s;
}

private bool parseAttr(string inner, out AttrSelector attr) {
    string s = trim(inner);
    const ptrdiff_t eq = findTop(s, '=');
    if (eq < 0) {
        attr = AttrSelector(asciiLower(s), AttrOp.exists, "", false);
        return s.length > 0;
    }
    const size_t at = cast(size_t) eq;
    AttrOp op = AttrOp.equal;
    size_t nameEnd = at;
    if (at > 0) {
        switch (s[at - 1]) {
        case '~':
            op = AttrOp.word;
            nameEnd = at - 1;
            break;
        case '|':
            op = AttrOp.dash;
            nameEnd = at - 1;
            break;
        case '^':
            op = AttrOp.prefix;
            nameEnd = at - 1;
            break;
        case '$':
            op = AttrOp.suffix;
            nameEnd = at - 1;
            break;
        case '*':
            op = AttrOp.substring;
            nameEnd = at - 1;
            break;
        default:
            break;
        }
    }
    const string name = asciiLower(trim(s[0 .. nameEnd]));
    if (name.length == 0) {
        return false;
    }
    string rest = trim(s[at + 1 .. $]);
    bool ci = false;
    if (rest.length >= 2 && (rest[$ - 2] == ' ' || rest[$ - 2] == '\t')) {
        const char flag = rest[$ - 1];
        if (flag == 'i' || flag == 'I') {
            ci = true;
            rest = trim(rest[0 .. $ - 2]);
        }
        else if (flag == 's' || flag == 'S') {
            rest = trim(rest[0 .. $ - 2]);
        }
    }
    attr = AttrSelector(name, op, unquote(rest), ci);
    return true;
}

private Compound parseCompound(string text, out bool ok) {
    Compound c;
    size_t i = 0;
    ok = true;
    if (i < text.length && text[i] == '*') {
        ++i;
    }
    else if (i < text.length && isNameStart(text[i])) {
        const size_t end = scanName(text, i);
        c.tag = asciiLower(text[i .. end]);
        i = end;
    }
    while (i < text.length) {
        const char ch = text[i];
        if (ch == '#') {
            const size_t end = scanName(text, i + 1);
            c.id = text[i + 1 .. end];
            i = end;
        }
        else if (ch == '.') {
            const size_t end = scanName(text, i + 1);
            if (end == i + 1) {
                ok = false;
                return Compound.init;
            }
            c.classes ~= text[i + 1 .. end];
            i = end;
        }
        else if (ch == '[') {
            size_t close = i + 1;
            char quote = '\0';
            while (close < text.length) {
                if (quote != '\0') {
                    if (text[close] == '\\') {
                        ++close;
                    }
                    else if (text[close] == quote) {
                        quote = '\0';
                    }
                }
                else if (text[close] == '"' || text[close] == '\'') {
                    quote = text[close];
                }
                else if (text[close] == ']') {
                    break;
                }
                ++close;
            }
            if (close >= text.length) {
                ok = false;
                return Compound.init;
            }
            AttrSelector attr;
            if (!parseAttr(text[i + 1 .. close], attr)) {
                ok = false;
                return Compound.init;
            }
            c.attrs ~= attr;
            i = close + 1;
        }
        else if (ch == ':') {
            if (i + 1 < text.length && text[i + 1] == ':') {
                c.never = true;
                i += 2;
                i = scanName(text, i);
                if (i < text.length && text[i] == '(') {
                    const size_t close = matchingParen(text, i);
                    i = close < text.length ? close + 1 : close;
                }
                continue;
            }
            ++i;
            const size_t end = scanName(text, i);
            if (end == i) {
                ok = false;
                return Compound.init;
            }
            const string name = asciiLower(text[i .. end]);
            i = end;
            if (i < text.length && text[i] == '(') {
                const size_t close = matchingParen(text, i);
                const string arg = text[i + 1 .. close];
                i = close < text.length ? close + 1 : close;
                if (name == "not") {
                    auto list = parseSelectorList(arg, true);
                    if (list.length == 0) {
                        c.never = true;
                    }
                    c.notSelectors ~= list;
                }
                else if (name == "is") {
                    auto list = parseSelectorList(arg, true);
                    if (list.length == 0) {
                        c.never = true;
                    }
                    c.isSelectors ~= list;
                }
                else if (name == "where") {
                    auto list = parseSelectorList(arg, true);
                    if (list.length == 0) {
                        c.never = true;
                    }
                    c.whereSelectors ~= list;
                }
                else if (name == "nth-child" || name == "nth-last-child" ||
                         name == "nth-of-type" || name == "nth-last-of-type") {
                    Nth nth;
                    if (!parseNth(arg, nth)) {
                        c.never = true;
                    }
                    else {
                        nth.last = name == "nth-last-child" || name == "nth-last-of-type";
                        nth.ofType = name == "nth-of-type" || name == "nth-last-of-type";
                        c.nths ~= nth;
                    }
                }
                else if (name == "has") {
                    // `:has()` needs a descendant walk; parsed but never matched.
                    c.never = true;
                }
                else {
                    // Unknown functional pseudo-class: fail closed.
                    c.never = true;
                }
            }
            else {
                c.pseudos ~= name;
            }
        }
        else {
            ok = false;
            return Compound.init;
        }
    }
    return c;
}

private uint[3] maxSpecificity(const Selector selector) {
    return [selector.ids, selector.classes, selector.types];
}

private void computeSpecificity(ref Selector selector) {
    uint ids, classes, types;
    foreach (compound; selector.compounds) {
        if (compound.id.length) {
            ++ids;
        }
        classes += cast(uint) (compound.classes.length + compound.attrs.length +
                               compound.pseudos.length + compound.nths.length);
        if (compound.tag.length) {
            ++types;
        }
        foreach (nested; compound.notSelectors) {
            const m = maxSpecificity(nested);
            ids += m[0];
            classes += m[1];
            types += m[2];
        }
        foreach (nested; compound.isSelectors) {
            const m = maxSpecificity(nested);
            ids += m[0];
            classes += m[1];
            types += m[2];
        }
        // :where() contributes nothing, like CSS.
    }
    selector.ids = ids;
    selector.classes = classes;
    selector.types = types;
}

private Selector parseComplex(string text) {
    Selector selector;
    size_t i = 0;
    while (i < text.length) {
        while (i < text.length && isSpace(text[i])) {
            ++i;
        }
        if (i >= text.length) {
            break;
        }
        if (text[i] == '>' || text[i] == '+' || text[i] == '~') {
            if (selector.compounds.length == 0) {
                return Selector.init;
            }
            const Combinator comb = text[i] == '>' ? Combinator.child
                                   : text[i] == '+' ? Combinator.adjacent
                                   : Combinator.sibling;
            ++i;
            while (i < text.length && isSpace(text[i])) {
                ++i;
            }
            if (i >= text.length) {
                return Selector.init;
            }
            const size_t end = compoundEnd(text, i);
            bool ok;
            Compound compound = parseCompound(text[i .. end], ok);
            if (!ok) {
                return Selector.init;
            }
            selector.compounds ~= compound;
            selector.combinators ~= comb;
            i = end;
        }
        else {
            const size_t end = compoundEnd(text, i);
            bool ok;
            Compound compound = parseCompound(text[i .. end], ok);
            if (!ok) {
                return Selector.init;
            }
            if (selector.compounds.length) {
                if (selector.combinators.length == selector.compounds.length - 1) {
                    selector.combinators ~= Combinator.descendant;
                }
            }
            selector.compounds ~= compound;
            i = end;
        }
    }
    if (selector.compounds.length == 0 ||
        selector.combinators.length != selector.compounds.length - 1) {
        return Selector.init;
    }
    // A pseudo-element is only meaningful on the subject; we do not generate
    // one, so the whole selector is unusable.
    foreach (compound; selector.compounds) {
        if (compound.never) {
            selector.pseudoElement = true;
        }
    }
    computeSpecificity(selector);
    return selector;
}

/// Parse a comma-separated selector list. With `forgiving`, invalid entries
/// are skipped (`:is()`, `:where()`).
Selector[] parseSelectorList(string text, bool forgiving = false) {
    Selector[] selectors;
    foreach (part; splitTop(text, ',')) {
        const string one = trim(part);
        if (one.length == 0) {
            continue;
        }
        Selector selector = parseComplex(one);
        if (selector.compounds.length) {
            selectors ~= selector;
        }
        else if (!forgiving) {
            return [];
        }
    }
    return selectors;
}

private bool matchAttr(CssSubject subject, const AttrSelector attr) {
    if (!subject.cssHasAttr(attr.name)) {
        return false;
    }
    if (attr.op == AttrOp.exists) {
        return true;
    }
    string actual = subject.cssAttr(attr.name);
    string expected = attr.value;
    if (attr.caseInsensitive) {
        actual = asciiLower(actual);
        expected = asciiLower(expected);
    }
    final switch (attr.op) {
    case AttrOp.exists:
        return true;
    case AttrOp.equal:
        return actual == expected;
    case AttrOp.word:
        foreach (word; splitTop(actual, ' ')) {
            if (word.length && word == expected) {
                return true;
            }
        }
        return false;
    case AttrOp.prefix:
        return actual.length >= expected.length && actual[0 .. expected.length] == expected;
    case AttrOp.suffix:
        return actual.length >= expected.length &&
               actual[$ - expected.length .. $] == expected;
    case AttrOp.substring:
        if (expected.length == 0) {
            return true;
        }
        if (expected.length > actual.length) {
            return false;
        }
        foreach (i; 0 .. actual.length - expected.length + 1) {
            if (actual[i .. i + expected.length] == expected) {
                return true;
            }
        }
        return false;
    case AttrOp.dash:
        return actual == expected ||
               (actual.length > expected.length && actual[0 .. expected.length] == expected &&
                actual[expected.length] == '-');
    }
    return false;
}

private bool pseudoMatches(CssSubject subject, string name) {
    switch (name) {
    case "first-child":
        return subject.cssPosition() == 1;
    case "last-child":
        return subject.cssPosition() == subject.cssSiblingCount();
    case "only-child":
        return subject.cssSiblingCount() == 1;
    case "first-of-type":
        return subject.cssTypePosition() == 1;
    case "last-of-type":
        return subject.cssTypePosition() == subject.cssTypeCount();
    case "only-of-type":
        return subject.cssTypeCount() == 1;
    case "empty":
        return subject.cssIsEmpty();
    case "root":
        return subject.cssParent() is null;
    case "scope":
        return true;
    case "hover":
    case "active":
    case "focus":
    case "focus-visible":
    case "focus-within":
    case "disabled":
    case "enabled":
    case "checked":
    case "indeterminate":
    case "selected":
    case "required":
    case "optional":
    case "valid":
    case "invalid":
    case "read-only":
    case "read-write":
    case "placeholder-shown":
    case "default":
    case "defined":
    case "link":
    case "any-link":
    case "target":
    case "visited":
        return subject.cssState(name);
    default:
        return false;
    }
}

private bool matchCompound(CssSubject subject, const Compound compound) {
    if (compound.never) {
        return false;
    }
    if (compound.tag.length && !ieq(subject.cssTag(), compound.tag)) {
        return false;
    }
    if (compound.id.length && subject.cssId() != compound.id) {
        return false;
    }
    foreach (name; compound.classes) {
        if (!subject.cssHasClass(name)) {
            return false;
        }
    }
    foreach (attr; compound.attrs) {
        if (!matchAttr(subject, attr)) {
            return false;
        }
    }
    foreach (name; compound.pseudos) {
        if (!pseudoMatches(subject, name)) {
            return false;
        }
    }
    foreach (nth; compound.nths) {
        int position;
        if (nth.ofType) {
            position = nth.last
                ? subject.cssTypeCount() - subject.cssTypePosition() + 1
                : subject.cssTypePosition();
        }
        else {
            position = nth.last
                ? subject.cssSiblingCount() - subject.cssPosition() + 1
                : subject.cssPosition();
        }
        if (!nthOk(nth.a, nth.b, position)) {
            return false;
        }
    }
    foreach (nested; compound.notSelectors) {
        if (matchesSelector(subject, nested)) {
            return false;
        }
    }
    if (compound.isSelectors.length) {
        bool any;
        foreach (nested; compound.isSelectors) {
            if (matchesSelector(subject, nested)) {
                any = true;
                break;
            }
        }
        if (!any) {
            return false;
        }
    }
    if (compound.whereSelectors.length) {
        bool any;
        foreach (nested; compound.whereSelectors) {
            if (matchesSelector(subject, nested)) {
                any = true;
                break;
            }
        }
        if (!any) {
            return false;
        }
    }
    return true;
}

private bool matchAt(CssSubject subject, const Selector selector, size_t index) {
    if (subject is null || !matchCompound(subject, selector.compounds[index])) {
        return false;
    }
    if (index == 0) {
        return true;
    }
    final switch (selector.combinators[index - 1]) {
    case Combinator.child:
        return matchAt(subject.cssParent(), selector, index - 1);
    case Combinator.descendant:
        for (auto parent = subject.cssParent(); parent !is null; parent = parent.cssParent()) {
            if (matchAt(parent, selector, index - 1)) {
                return true;
            }
        }
        return false;
    case Combinator.adjacent:
        return matchAt(subject.cssPrevSibling(), selector, index - 1);
    case Combinator.sibling:
        for (auto sibling = subject.cssPrevSibling(); sibling !is null;
             sibling = sibling.cssPrevSibling()) {
            if (matchAt(sibling, selector, index - 1)) {
                return true;
            }
        }
        return false;
    }
    return false;
}

/// Whether `subject` matches `selector`. Selectors with pseudo-elements or
/// unsupported constructs never match.
bool matchesSelector(CssSubject subject, const Selector selector) {
    if (subject is null || selector.pseudoElement || selector.compounds.length == 0) {
        return false;
    }
    return matchAt(subject, selector, selector.compounds.length - 1);
}

