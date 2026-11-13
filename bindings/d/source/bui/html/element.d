/* The element tree: HTML-like nodes with attributes, text, children and
 * handlers, built by the tag constructors in `bui.html.tags` or by `rsx!`.
 *
 * `Element` implements `bui.css.selectors.CssSubject`, so the CSS cascade can
 * match tags, ids, classes, attributes and structural pseudo-classes without
 * knowing anything about the HTML layer. `Element.states` carries the dynamic
 * pseudo-classes (`hover`, `active`, `focus`, ...); the application updates
 * them and calls `Document.restyle`.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.html.element;

import std.traits : isSomeString;

import bui.css.selectors : CssSubject, Selector, matchesSelector, parseSelectorList;
import bui.css.style : Style;
import bui.errors : BuiException;
import bui.html.text : asciiLower, isHtmlSpace;
import bui.kit : BK_EV_CHANGE, BK_EV_CLICK, BK_EV_DOUBLE, BK_EV_ENTER, BK_EV_KEY,
    BK_EV_LEAVE, BK_EV_PRESS, BK_EV_RELEASE, BK_EV_SUBMIT,
    BK_EV_TOGGLE, BK_EV_WHEEL;
import bui.node : Node, NodeEvent, NodeHandler;

/// One piece of an element's content: text or a child element.
package struct Content {
    Element element;
    string text;
}

/// An HTML element before it is realised as kit nodes.
final class Element : CssSubject {
    /// Lowercase tag name (`"div"`, `"my-widget"`, ...).
    string tag;

    private {
        string id_;
        string[] classes_;
        string[string] attrs_;
        string inlineStyle_;
        Content[] contents_;
        Element[] elements_;
        Element parent_;
        bool[string] states_;

        package Style computed;
        package Node kitNode_;
        package Node outerNode_;
        package Node flowNode_;
        package Node backgroundNode_;

        void delegate() click_;
        void delegate(string) change_;
        void delegate(bool) toggle_;
        void delegate() submit_;
        void delegate() press_;
        void delegate() release_;
        void delegate() doubleClick_;
        void delegate(string) key_;
        void delegate(double, double) wheel_;
        NodeHandler event_;
    }

    /// Set by `Document`: refresh the visual style after a state change.
    package void delegate() onStateChange;

    /// Create an element with an explicit tag. Prefer the constructors in
    /// `bui.html.tags`.
    this(string tag) {
        if (tag.length == 0) {
            throw new BuiException("Element", "empty tag");
        }
        this.tag = asciiLower(tag);
    }

    /* -- attributes -------------------------------------------------------- */

    /// Set the id (`#id`).
    Element id(const(char)[] value) {
        id_ = value.idup;
        return this;
    }

    /// Add one or more classes, separated by whitespace.
    Element class_(const(char)[] names) {
        size_t start = 0;
        while (start < names.length) {
            while (start < names.length && isHtmlSpace(names[start])) {
                ++start;
            }
            size_t end = start;
            while (end < names.length && !isHtmlSpace(names[end])) {
                ++end;
            }
            if (end > start) {
                const string name = names[start .. end].idup;
                if (!hasClass(name)) {
                    classes_ ~= name;
                }
            }
            start = end;
        }
        return this;
    }

    /// Set an attribute. `id`, `class` and `style` update their dedicated
    /// fields so selectors and the cascade see them.
    Element attr(const(char)[] name, const(char)[] value) {
        const string key = asciiLower(name.idup);
        if (key == "id") {
            return id(value);
        }
        if (key == "class") {
            return class_(value);
        }
        if (key == "style") {
            return style(value);
        }
        attrs_[key] = value.idup;
        return this;
    }

    /// Set a boolean attribute (`disabled`, `checked`, `selected`, ...).
    Element attr(const(char)[] name) {
        return attr(name, "");
    }

    /// The inline style (`style="..."`).
    Element style(const(char)[] css) {
        inlineStyle_ = css.idup;
        return this;
    }

    /// The attribute value, or empty when absent.
    string attrValue(string name) const {
        return attributeValue(name);
    }

    /// Whether the attribute is present (including boolean attributes).
    bool hasAttr(string name) const {
        if (name == "id") {
            return id_.length != 0;
        }
        if (name == "class") {
            return classes_.length != 0;
        }
        if (name == "style") {
            return inlineStyle_.length != 0;
        }
        return (name in attrs_) !is null;
    }

    /* -- content ----------------------------------------------------------- */

    /// Append text. Consecutive calls concatenate, as in a text node.
    Element text(const(char)[] value) {
        if (value.length == 0) {
            return this;
        }
        if (contents_.length && contents_[$ - 1].element is null) {
            contents_[$ - 1].text ~= value;
        }
        else {
            contents_ ~= Content(null, value.idup);
        }
        return this;
    }

    /// Replace the direct text with `value`. A realised text leaf updates its
    /// label immediately; text inside a box with element children needs the
    /// document rebuilt to be laid out again.
    Element setText(const(char)[] value) {
        Content[] kept;
        foreach (content; contents_) {
            if (content.element !is null) {
                kept ~= content;
            }
        }
        if (value.length) {
            kept ~= Content(null, value.idup);
        }
        contents_ = kept;
        if (kitNode_ !is null && kitNode_.typeName() == "label") {
            kitNode_.set("text", value);
        }
        return this;
    }

    /// Append a child element.
    Element child(Element element) {
        if (element is null) {
            return this;
        }
        if (element is this) {
            throw new BuiException("Element.child", "an element cannot contain itself");
        }
        element.detach();
        contents_ ~= Content(element, null);
        elements_ ~= element;
        element.parent_ = this;
        return this;
    }

    /// Append several children.
    Element children(Element[] list) {
        foreach (element; list) {
            child(element);
        }
        return this;
    }

    /// Append text or elements chosen at run time.
    Element add(const(char)[] value) {
        return text(value);
    }

    /// Append a child chosen at run time.
    Element add(Element element) {
        return child(element);
    }

    /// Append text, an element or an array of them; the engine behind
    /// `rsx!` content and the tag constructors' arguments.
    Element append(T)(T value) {
        addArgument(this, value);
        return this;
    }

    /// Set a boolean attribute when `on`, remove it otherwise.
    Element flagAttr(const(char)[] name, bool on) {
        const string key = asciiLower(name.idup);
        if (on) {
            return attr(key);
        }
        if (key == "id") {
            id_ = null;
        }
        else if (key == "class") {
            classes_ = null;
        }
        else if (key == "style") {
            inlineStyle_ = null;
        }
        else {
            attrs_.remove(key);
        }
        return this;
    }

    /// The direct text, without descendants.
    string textContent() const {
        string result;
        foreach (content; contents_) {
            if (content.element is null) {
                result ~= content.text;
            }
        }
        return result;
    }

    /// The text of this element and all its descendants, in document order.
    string descendantText() const {
        string result;
        foreach (content; contents_) {
            result ~= content.element is null ? content.text
                                              : content.element.descendantText();
        }
        return result;
    }

    /// The direct child elements.
    Element[] children() {
        return elements_;
    }

    /// The parent element, or null at the root.
    Element parent() {
        return parent_;
    }

    /// The realised kit node (the widget itself for a widget, the label for
    /// text, the outer frame for a decorated box). Null before realisation.
    Node node() {
        return kitNode_;
    }

    /// The node inserted into the parent, which differs from `node` when the
    /// element needed a background or border wrapper.
    Node outerNode() {
        return outerNode_ is null ? kitNode_ : outerNode_;
    }

    /// The computed style after `Document` has run the cascade.
    Style style() {
        return computed;
    }

    /// A copy of the inline style text.
    string inlineStyle() const {
        return inlineStyle_;
    }

    /* -- dynamic states ---------------------------------------------------- */

    /// Set a dynamic pseudo-class state (`hover`, `active`, `focus`, ...).
    Element setState(string name, bool on) {
        states_[name] = on;
        return this;
    }

    /// Whether a state is set.
    bool state(string name) const {
        if (name in states_) {
            return states_[name];
        }
        // HTML attributes imply a few states.
        if (name == "disabled" || name == "checked" || name == "selected" ||
            name == "required" || name == "read-only") {
            return hasAttr(name);
        }
        if (name == "enabled" || name == "optional" || name == "read-write") {
            return !hasAttr(name == "enabled" ? "disabled" : name == "optional" ? "required" : "readonly");
        }
        return false;
    }

    /* -- events ------------------------------------------------------------ */

    /// A click (or activation) on the element or a widget.
    Element onClick(void delegate() handler) {
        click_ = handler;
        return this;
    }

    /// A value change: text edits, sliders, dropdowns.
    Element onChange(void delegate(string) handler) {
        change_ = handler;
        return this;
    }

    /// A checkbox/radio toggle.
    Element onToggle(void delegate(bool) handler) {
        toggle_ = handler;
        return this;
    }

    /// A form submit event.
    Element onSubmit(void delegate() handler) {
        submit_ = handler;
        return this;
    }

    /// Pointer press/release/double-click.
    Element onPress(void delegate() handler) {
        press_ = handler;
        return this;
    }

    Element onRelease(void delegate() handler) {
        release_ = handler;
        return this;
    }

    Element onDoubleClick(void delegate() handler) {
        doubleClick_ = handler;
        return this;
    }

    /// A key event, with the key text.
    Element onKey(void delegate(string) handler) {
        key_ = handler;
        return this;
    }

    /// A wheel event, with the deltas.
    Element onWheel(void delegate(double, double) handler) {
        wheel_ = handler;
        return this;
    }

    /// Every kit event, unfiltered.
    Element onEvent(NodeHandler handler) {
        event_ = handler;
        return this;
    }

    /* -- CssSubject -------------------------------------------------------- */

    override string cssTag() {
        return tag;
    }

    override string cssId() {
        return id_;
    }

    override string[] cssClasses() {
        return classes_;
    }

    override bool cssHasClass(string name) {
        return hasClass(name);
    }

    override bool cssHasAttr(string name) {
        return hasAttr(name);
    }

    override string cssAttr(string name) {
        return attributeValue(name);
    }

    private string attributeValue(string name) const {
        if (name == "id") {
            return id_;
        }
        if (name == "class") {
            string result;
            foreach (i, value; classes_) {
                if (i) {
                    result ~= " ";
                }
                result ~= value;
            }
            return result;
        }
        if (name == "style") {
            return inlineStyle_;
        }
        return attrs_.get(name, "");
    }

    override bool cssState(string name) {
        return state(name);
    }

    override CssSubject cssParent() {
        return parent_;
    }

    override CssSubject cssPrevSibling() {
        if (parent_ is null) {
            return null;
        }
        foreach (i, element; parent_.elements_) {
            if (element is this) {
                return i ? parent_.elements_[i - 1] : null;
            }
        }
        return null;
    }

    override CssSubject cssNextSibling() {
        if (parent_ is null) {
            return null;
        }
        foreach (i, element; parent_.elements_) {
            if (element is this) {
                return i + 1 < parent_.elements_.length ? parent_.elements_[i + 1] : null;
            }
        }
        return null;
    }

    override int cssPosition() {
        if (parent_ is null) {
            return 1;
        }
        foreach (i, element; parent_.elements_) {
            if (element is this) {
                return cast(int) (i + 1);
            }
        }
        return 1;
    }

    override int cssSiblingCount() {
        return parent_ is null ? 1 : cast(int) parent_.elements_.length;
    }

    override int cssTypePosition() {
        if (parent_ is null) {
            return 1;
        }
        int position;
        foreach (element; parent_.elements_) {
            if (element.tag == tag) {
                ++position;
            }
            if (element is this) {
                return position;
            }
        }
        return position;
    }

    override int cssTypeCount() {
        if (parent_ is null) {
            return 1;
        }
        int count;
        foreach (element; parent_.elements_) {
            if (element.tag == tag) {
                ++count;
            }
        }
        return count;
    }

    override bool cssIsEmpty() {
        if (elements_.length) {
            return false;
        }
        return textContent().length == 0;
    }

    /* -- queries ----------------------------------------------------------- */

    /// Every descendant (and this element) matching `selector`, in document
    /// order.
    Element[] query(string selector) {
        Selector[] selectors = parseSelectorList(selector);
        Element[] result;
        if (selectors.length == 0) {
            return result;
        }
        void walk(Element element) {
            foreach (candidate; selectors) {
                if (matchesSelector(element, candidate)) {
                    result ~= element;
                    break;
                }
            }
            foreach (child; element.elements_) {
                walk(child);
            }
        }
        walk(this);
        return result;
    }

    /// The first match, or null.
    Element queryFirst(string selector) {
        auto matches = query(selector);
        return matches.length ? matches[0] : null;
    }

    /// The first match by id.
    Element byId(string id) {
        return queryFirst("#" ~ id);
    }

    /* -- package internals ------------------------------------------------- */

    package Content[] contents() {
        return contents_;
    }

    package bool hasClass(string name) const {
        foreach (value; classes_) {
            if (value == name) {
                return true;
            }
        }
        return false;
    }

    private void detach() {
        if (parent_ is null) {
            return;
        }
        foreach (i, content; parent_.contents_) {
            if (content.element is this) {
                parent_.contents_ = parent_.contents_[0 .. i] ~ parent_.contents_[i + 1 .. $];
                break;
            }
        }
        foreach (i, element; parent_.elements_) {
            if (element is this) {
                parent_.elements_ = parent_.elements_[0 .. i] ~ parent_.elements_[i + 1 .. $];
                break;
            }
        }
        parent_ = null;
    }
}

/// Build an element with tag `tag` from text, elements or arrays of them.
///
/// This is the engine behind every tag constructor: `div("a", span("b"))`.
Element makeElement(string tag, Args...)(Args args) {
    auto element = new Element(tag);
    foreach (arg; args) {
        addArgument(element, arg);
    }
    return element;
}

private void addArgument(T)(Element element, T arg) {
    static if (is(T : Element)) {
        element.child(arg);
    }
    else static if (isSomeString!T) {
        element.text(arg);
    }
    else static if (is(T : U[], U)) {
        foreach (item; arg) {
            addArgument(element, item);
        }
    }
    else {
        element.text(toText(arg));
    }
}

/// `text()` for values interpolated by `rsx!`: strings verbatim, everything
/// else through `std.conv.to!string`.
string toText(T)(T value) {
    static if (isSomeString!T) {
        return value.idup;
    }
    else {
        import std.conv : to;
        return to!string(value);
    }
}

/// Append the event handlers and state hooks to a realised node.
package void bindEvents(Element element, Node node, void delegate() restyle) {
    const bool wantsEvents = element.click_ !is null || element.change_ !is null ||
                             element.toggle_ !is null || element.submit_ !is null ||
                             element.press_ !is null || element.release_ !is null ||
                             element.doubleClick_ !is null || element.key_ !is null ||
                             element.wheel_ !is null || element.event_ !is null ||
                             restyle !is null;
    if (!wantsEvents) {
        return;
    }
    // Plain frames become hit-test targets only with this flag; widgets are
    // interactive by class.
    node.set("events", 1);

    if (element.click_ !is null) {
        node.on(BK_EV_CLICK, (NodeEvent) {
            element.click_();
        });
    }
    if (element.change_ !is null) {
        node.on(BK_EV_CHANGE, (NodeEvent event) {
            string value = event.node.str("value").idup;
            if (value.length == 0) {
                value = toText(event.node.num("value"));
            }
            element.change_(value);
        });
    }
    if (element.toggle_ !is null) {
        node.on(BK_EV_TOGGLE, (NodeEvent event) {
            element.toggle_(event.node.num("value") != 0);
        });
    }
    if (element.submit_ !is null) {
        node.on(BK_EV_SUBMIT, (NodeEvent) {
            element.submit_();
        });
    }
    if (element.press_ !is null) {
        node.on(BK_EV_PRESS, (NodeEvent) {
            element.press_();
        });
    }
    if (element.release_ !is null) {
        node.on(BK_EV_RELEASE, (NodeEvent) {
            element.release_();
        });
    }
    if (element.doubleClick_ !is null) {
        node.on(BK_EV_DOUBLE, (NodeEvent) {
            element.doubleClick_();
        });
    }
    if (element.key_ !is null) {
        node.on(BK_EV_KEY, (NodeEvent event) {
            element.key_(event.text.idup);
        });
    }
    if (element.wheel_ !is null) {
        node.on(BK_EV_WHEEL, (NodeEvent event) {
            element.wheel_(event.a, event.b);
        });
    }
    if (element.event_ !is null) {
        node.on(element.event_);
    }

    if (restyle !is null) {
        node.on(BK_EV_ENTER, (NodeEvent) {
            bool changed;
            for (auto at = element; at !is null; at = at.parent_) {
                if (!at.states_.get("hover", false)) {
                    at.states_["hover"] = true;
                    changed = true;
                }
            }
            if (changed) {
                restyle();
            }
        });
        node.on(BK_EV_LEAVE, (NodeEvent) {
            bool changed;
            for (auto at = element; at !is null; at = at.parent_) {
                if (at.states_.get("hover", false)) {
                    at.states_["hover"] = false;
                    changed = true;
                }
            }
            if (changed) {
                restyle();
            }
        });
        node.on(BK_EV_PRESS, (NodeEvent) {
            bool changed;
            for (auto at = element; at !is null; at = at.parent_) {
                if (!at.states_.get("active", false)) {
                    at.states_["active"] = true;
                    changed = true;
                }
            }
            if (changed) {
                restyle();
            }
        });
        node.on(BK_EV_RELEASE, (NodeEvent) {
            bool changed;
            for (auto at = element; at !is null; at = at.parent_) {
                if (at.states_.get("active", false)) {
                    at.states_["active"] = false;
                    changed = true;
                }
            }
            if (changed) {
                restyle();
            }
        });
    }
}

