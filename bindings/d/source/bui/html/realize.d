/* Realisation: computed CSS becomes kit nodes.
 *
 * The kit has no CSS engine and no flex/grid layout, so this module maps the
 * supported subset onto what the kit really offers:
 *
 *   - a `frame` draws the background and hosts children with Tk's pack/grid;
 *   - per-side padding and margins are pack insets (`padx0`/`pady1` ...);
 *   - `display: flex` maps to a pack row/column, `grid` to a frame grid;
 *   - `table` maps to a frame grid with one cell per `td`/`th`;
 *   - text becomes `label` nodes, widgets become their kit controls.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.html.realize;

import std.conv : to;

import bui.css.color : Color, tryParseColor;
import bui.css.style : Cursor, Display, FlexDirection, Style, Stylesheet;
import bui.errors : BuiException;
import bui.html.element : Element, bindEvents;
import bui.html.tags : TagKind, tagKind, uaCss;
import bui.html.text : asciiLower, collapseTrimmed, collapseWhitespace, trimEnd, trimStart;
import bui.kit;
import bui.node : Node, NodeEvent;
import bui.theme : scale, themeColor;
import bui.video : Video;

/// One realised document: a CSS cascade over an `Element` tree, mounted as a
/// kit node tree.
final class Document {
    private {
        Element root_;
        Node rootNode_;
        Stylesheet sheet_;
        Style base_;
        float viewportW_ = 800.0f;
        float viewportH_ = 600.0f;
    }

    /// Build `root` with the user-agent sheet plus `css`.
    this(Element root, string css = null) {
        if (root is null) {
            throw new BuiException("Document", "no root element");
        }
        root_ = root;

        sheet_ = Stylesheet.parse(uaCss);
        if (css.length) {
            sheet_.add(css);
        }
        collectStyleSheets(root, sheet_);

        base_ = Style.init;
        base_.fontSize = 11.0f;
        base_.fontWeight = 400;
        base_.visible = true;
        float[4] textColor;
        if (themeColor("text", textColor)) {
            base_.color = Color(textColor[0], textColor[1], textColor[2], textColor[3]);
        }
        else {
            base_.color = Color(0.9f, 0.9f, 0.9f, 1.0f);
        }

        computeStyles(root_, base_);
        rootNode_ = realizeElement(root_);
        if (rootNode_ is null) {
            throw new BuiException("Document", "the root element is not rendered");
        }
        // A root has no parent packer, so its CSS margin becomes an outer
        // frame's insets (the user-agent `body { margin: 8px; }` included).
        const Style rootStyle = root_.computed;
        if (hasMargin(rootStyle)) {
            auto outer = new Node("frame");
            setPads(rootNode_, px(rootStyle.margin[3]), px(rootStyle.margin[0]),
                     px(rootStyle.margin[1]), px(rootStyle.margin[2]));
            rootNode_.pack("side", 0).pack("fill", 3).pack("expand", 1);
            outer.add(rootNode_);
            rootNode_ = outer;
        }
    }

    /// The kit root, ready for `new Context(root)`.
    Node node() {
        return rootNode_;
    }

    /// The element tree.
    Element root() {
        return root_;
    }

    /// The document title from `<title>`, when present.
    string title() {
        auto element = root_.queryFirst("title");
        return element is null ? "" : element.textContent().idup;
    }

    /// The parsed sheet (user-agent rules plus the document's CSS).
    Stylesheet stylesheet() {
        return sheet_;
    }

    /// Set the viewport used by `vw`/`vh`/`%`, then restyle.
    Document setViewport(float width, float height) {
        viewportW_ = width;
        viewportH_ = height;
        restyle();
        return this;
    }

    /// Re-run the cascade and apply the visual properties (colours,
    /// typography, visibility, cursor, requested size) to the realised nodes.
    ///
    /// Layout structure (padding, display, grid shape) is fixed when the
    /// document is built; changing those in a dynamic state needs a rebuild.
    void restyle() {
        computeStyles(root_, base_);
        applyVisual(root_);
    }

    /// The first element matching `selector`.
    Element find(string selector) {
        return root_.queryFirst(selector);
    }

    /// Every element matching `selector`.
    Element[] findAll(string selector) {
        return root_.query(selector);
    }

    private void computeStyles(Element element, Style parent) {
        element.computed = sheet_.compute(element, parent, element.inlineStyle,
                                           11.0f, viewportW_, viewportH_);
        if (element.onStateChange is null) {
            element.onStateChange = () {
                restyle();
            };
        }
        foreach (child; element.children()) {
            computeStyles(child, element.computed);
        }
    }

    private void collectStyleSheets(Element element, Stylesheet sheet) {
        if (element.tag == "style") {
            const string text = element.textContent();
            if (text.length) {
                sheet.add(text);
            }
        }
        foreach (child; element.children()) {
            collectStyleSheets(child, sheet);
        }
    }

    private void applyVisual(Element element) {
        const Style style = element.computed;
        if (element.outerNode_ !is null) {
            Node outer = element.outerNode_;
            outer.set("visible", style.visible && !element.hasAttr("hidden"));
            if (style.cursor >= 0) {
                outer.set("cursor", cursorCode(style.cursor));
            }
            const int width = px(style.width);
            const int height = px(style.height);
            if (width > 0) {
                outer.geom("reqwidth", width);
            }
            if (height > 0) {
                outer.geom("reqheight", height);
            }
            if (element.backgroundNode_ !is null) {
                element.backgroundNode_.set("bg", style.background.toArray());
            }
        }
        if (element.kitNode_ !is null) {
            applyTextStyle(element.kitNode_, style);
        }
        foreach (child; element.children()) {
            applyVisual(child);
        }
    }
}

/// Build and realise `root` with the user-agent sheet plus `css`.
Document document(Element root, string css = null) {
    return new Document(root, css);
}

/// Build and realise `root`, returning the kit node.
Node realize(Element root, string css = null) {
    return (new Document(root, css)).node;
}

/* -- realisation ---------------------------------------------------------- */

private struct Box {
    Node outer;
    Node flow;
    Node background;
}

private struct Item {
    Node node;
    Style style;
    bool text;
}

private Node realizeElement(Element element) {
    if (element.computed.display == Display.none) {
        return null;
    }
    final switch (tagKind(element.tag)) {
    case TagKind.metadata:
        return null;
    case TagKind.break_:
        return null;
    case TagKind.replaced:
        return realizeReplaced(element);
    case TagKind.widget:
        return realizeWidget(element);
    case TagKind.box:
        return realizeBox(element);
    }
}

private bool isTable(Element element) {
    return element.tag == "table";
}

private Node realizeBox(Element element) {
    const Style style = element.computed;

    if (element.tag == "hr") {
        auto separator = new Node("separator");
        separator.set("line", 1);
        finish(element, separator, separator, null);
        return separator;
    }

    if (isTable(element)) {
        return realizeTable(element);
    }

    // A box whose content is only text is one label; this is the common
    // `<p>`, `<h1>`, `<span>` and `<a>` case.
    Element[] children = element.children();
    bool hasText;
    foreach (content; element.contents()) {
        if (content.element is null && collapseTrimmed(content.text).length) {
            hasText = true;
            break;
        }
    }
    if (children.length == 0) {
        if (hasText) {
            const string text = collapseTrimmed(element.textContent());
            auto label = textNode(text, style);
            auto box = decorateLeaf(element, label);
            finish(element, label, box.outer, box.background);
            return box.outer;
        }
        // An empty box still needs a node for width/height/background.
        auto box = makeBox(element);
        finish(element, box.outer, box.outer, box.background);
        return box.outer;
    }

    auto box = makeBox(element);
    realizeContents(element, box.flow);
    finish(element, box.outer, box.outer, box.background);
    return box.outer;
}

/// Realise `element`'s contents into `flow`, choosing the layout from CSS.
private void realizeContents(Element element, Node flow) {
    Style style = element.computed;

    // Collapse whitespace first. A run that is only spaces is kept only
    // between two inline items, as in HTML.
    static struct Pending {
        Element element;
        string text;
        bool space;
    }
    Pending[] pending;
    foreach (content; element.contents()) {
        if (content.element !is null) {
            pending ~= Pending(content.element, null, false);
        }
        else {
            const string text = collapseWhitespace(content.text);
            if (text.length) {
                pending ~= Pending(null, text, text == " ");
            }
        }
    }

    Item[] items;
    foreach (i, entry; pending) {
        if (entry.element !is null) {
            Node node = realizeElement(entry.element);
            if (node !is null) {
                items ~= Item(node, entry.element.computed, false);
            }
            continue;
        }
        if (entry.space) {
            const bool previousInline = items.length != 0 &&
                (items[$ - 1].text || isInline(items[$ - 1].style));
            bool nextInline;
            foreach (next; pending[i + 1 .. $]) {
                if (next.element !is null) {
                    const Display display = next.element.computed.display;
                    if (display == Display.none) {
                        continue;
                    }
                    nextInline = isInline(next.element.computed);
                    break;
                }
                if (next.space) {
                    continue;
                }
                nextInline = true;
                break;
            }
            if (previousInline && nextInline) {
                items ~= Item(textNode(" ", style), style, true);
            }
            continue;
        }
        items ~= Item(textNode(entry.text, style), style, true);
    }

    // Text runs keep one space at each edge; trim the edges that touch the
    // container or a block item so blocks do not gain stray indentation.
    size_t kept;
    foreach (i, item; items) {
        if (!item.text) {
            items[kept++] = item;
            continue;
        }
        string value = item.node.str("text").idup;
        const bool lineStart = kept == 0 ||
            !(items[kept - 1].text || isInline(items[kept - 1].style));
        const bool lineEnd = i + 1 == items.length ||
            !(items[i + 1].text || isInline(items[i + 1].style));
        if (lineStart) {
            value = trimStart(value);
        }
        if (lineEnd) {
            value = trimEnd(value);
        }
        if (value.length == 0) {
            continue;
        }
        item.node.set("text", value);
        items[kept++] = item;
    }
    items = items[0 .. kept];
    if (items.length == 0) {
        return;
    }

    const Display display = style.display;
    if (display == Display.grid || display == Display.inlineGrid) {
        realizeGrid(flow, element, items);
        return;
    }

    const bool flex = display == Display.flex || display == Display.inlineFlex;
    const bool horizontal = flex
        ? (style.flexDirection == FlexDirection.row ||
           style.flexDirection == FlexDirection.rowReverse)
        : display == Display.inline_ || display == Display.inlineBlock;

    if (horizontal) {
        const bool reverse = flex && style.flexDirection == FlexDirection.rowReverse;
        size_t k = 0;
        foreach (item; items) {
            packItem(item, flow, reverse ? 3 : 2, false, flex && !item.text,
                     k ? px(style.columnGap) : 0, 0);
            ++k;
        }
        return;
    }

    // Block flow: consecutive inline items share a line box (a pack row),
    // block items stack vertically.
    const bool reverse = flex && style.flexDirection == FlexDirection.columnReverse;
    // A column flex container's items are blockified: no shared line boxes.
    bool inlineRun(Item item) {
        return !flex && (item.text || isInline(item.style));
    }
    size_t i = 0;
    bool first = true;
    while (i < items.length) {
        if (inlineRun(items[i])) {
            size_t end = i;
            while (end < items.length && inlineRun(items[end])) {
                ++end;
            }
            if (end - i == 1) {
                Item item = items[i];
                packItem(item, flow, reverse ? 1 : 0,
                         item.text || item.style.display == Display.inline_,
                         false, first ? 0 : px(style.rowGap), 0);
            }
            else {
                auto row = new Node("frame");
                size_t k = i;
                while (k < end) {
                    packItem(items[k], row, 2, false, false,
                             k == i ? 0 : px(style.columnGap), 0);
                    ++k;
                }
                row.pack("side", reverse ? 1 : 0).pack("fill", 1);
                flow.add(row);
                if (!first) {
                    row.geom("pady0", px(style.rowGap));
                }
                row.geom("padx0", px(style.margin[3]));
                row.geom("padx1", px(style.margin[1]));
            }
            i = end;
        }
        else {
            Item item = items[i];
            packItem(item, flow, reverse ? 1 : 0, item.style.width <= 0.0f,
                     item.style.flexGrow > 0.0f, first ? 0 : px(style.rowGap), 0);
            ++i;
        }
        first = false;
    }
}

private void realizeGrid(Node flow, Element element, Item[] items) {
    const Style style = element.computed;
    int columns = style.gridColumns;
    if (columns < 1) {
        columns = 1;
    }
    const int columnGap = px(style.columnGap);
    const int rowGap = px(style.rowGap);
    foreach (i, item; items) {
        const int row = cast(int) i / columns;
        const int column = cast(int) i % columns;
        item.node.grid("row", row).grid("column", column);
        setInsets(item.node, item.style,
                  column ? columnGap : 0, row ? rowGap : 0);
        flow.add(item.node);
    }
    // Equal column weights: `fr` columns share spare width, like CSS.
    foreach (column; 0 .. columns) {
        flow.frameGridWeight(1, column, 1.0);
    }
}

/* -- tables --------------------------------------------------------------- */

private Node realizeTable(Element element) {
    auto box = makeBox(element);
    auto rows = new Element[](0);
    Element[] captions;
    collectRows(element, rows, captions);

    // Place every cell: a rowspan keeps its columns taken in the rows below.
    struct Placed {
        Element cell;
        int row;
        int column;
    }
    Placed[] placed;
    bool[][] taken = new bool[][](rows.length);
    int columns;
    foreach (r, row; rows) {
        int column;
        foreach (cell; row.children()) {
            if (cell.tag != "td" && cell.tag != "th") {
                continue;
            }
            while (column < taken[r].length && taken[r][column]) {
                ++column;
            }
            const int colspan = attrInt(cell, "colspan", 1) > 1 ? attrInt(cell, "colspan", 1) : 1;
            const int rowspan = attrInt(cell, "rowspan", 1) > 1 ? attrInt(cell, "rowspan", 1) : 1;
            foreach (below; r .. r + rowspan) {
                if (below >= rows.length) {
                    break;
                }
                if (taken[below].length < column + colspan) {
                    taken[below].length = column + colspan;
                }
                taken[below][column .. column + colspan] = true;
            }
            placed ~= Placed(cell, cast(int) r, column);
            column += colspan;
        }
        if (column > columns) {
            columns = column;
        }
    }
    foreach (row; taken) {
        if (row.length > columns) {
            columns = cast(int) row.length;
        }
    }
    if (columns < 1) {
        columns = 1;
    }

    // Captions pack above the grid; a pack frame cannot mix pack children
    // and grid children, so the grid gets a frame of its own when needed.
    Node gridFlow = box.flow;
    if (captions.length) {
        foreach (caption; captions) {
            Node node = realizeElement(caption);
            if (node !is null) {
                node.pack("side", 0).pack("fill", 1);
                box.flow.add(node);
            }
        }
        gridFlow = new Node("frame");
        gridFlow.pack("side", 0).pack("fill", 3).pack("expand", 1);
        box.flow.add(gridFlow);
    }

    const int columnGap = px(element.computed.columnGap);
    const int rowGap = px(element.computed.rowGap);
    foreach (place; placed) {
        Element cell = place.cell;
        Node node = realizeElement(cell);
        if (node is null) {
            continue;
        }
        const int colspan = attrInt(cell, "colspan", 1);
        const int rowspan = attrInt(cell, "rowspan", 1);
        node.grid("row", place.row).grid("column", place.column);
        if (colspan > 1) {
            node.grid("columnspan", colspan);
        }
        if (rowspan > 1) {
            node.grid("rowspan", rowspan);
        }
        setInsets(node, cell.computed, place.column > 0 ? columnGap : 0,
                  place.row > 0 ? rowGap : 0);
        gridFlow.add(node);
    }
    foreach (column; 0 .. columns) {
        gridFlow.frameGridWeight(1, column, 1.0);
    }
    element.flowNode_ = gridFlow;
    finish(element, box.outer, box.outer, box.background);
    return box.outer;
}

private void collectRows(Element element, ref Element[] rows, ref Element[] captions) {
    foreach (child; element.children()) {
        if (child.computed.display == Display.none || child.hasAttr("hidden")) {
            continue;
        }
        switch (child.tag) {
        case "tr":
            rows ~= child;
            break;
        case "thead":
        case "tbody":
        case "tfoot":
            collectRows(child, rows, captions);
            break;
        case "caption":
            captions ~= child;
            break;
        default:
            break;
        }
    }
}

/* -- widgets -------------------------------------------------------------- */

private Node realizeWidget(Element element) {
    const string tag = element.tag;
    switch (tag) {
    case "button":
        return realizeButton(element);
    case "input":
        return realizeInput(element);
    case "select":
        return realizeSelect(element);
    case "textarea":
        return realizeTextarea(element);
    case "progress":
        return realizeProgress(element);
    case "meter":
        return realizeMeter(element);
    case "option":
    case "optgroup":
        return realizeLeafWidget(element);
    default:
        // datalist and unknown widget tags: no control of their own.
        return null;
    }
}

private Node realizeButton(Element element) {
    auto button = new Node("button");
    const string text = collapseTrimmed(element.descendantText());
    button.set("text", text.length ? text : attrValue(element, "value", "Button"));
    return finishWidget(element, button);
}

/// Radios sharing a `name` under the same root are mutually exclusive:
/// turning one on turns the others off.
private void bindRadioGroup(Element element, Node node) {
    const string name = element.attrValue("name");
    node.on(BK_EV_TOGGLE, (NodeEvent event) {
        if (event.node.num("value") == 0) {
            return;
        }
        Element top = element;
        while (top.parent() !is null) {
            top = top.parent();
        }
        void turnOff(Element at) {
            if (at !is element && at.tag == "input" && at.hasAttr("name") &&
                at.attrValue("name") == name &&
                asciiLower(attrValue(at, "type", "text")) == "radio" &&
                at.node() !is null) {
                at.node().set("value", 0);
            }
            foreach (child; at.children()) {
                turnOff(child);
            }
        }
        turnOff(top);
    });
}

private Node realizeInput(Element element) {
    const string type = asciiLower(attrValue(element, "type", "text"));
    const string value = attrValue(element, "value", "");
    const string placeholder = attrValue(element, "placeholder", "");

    if (type == "hidden" || type == "image") {
        return null;
    }

    if (type == "checkbox" || type == "radio") {
        auto node = new Node(type == "radio" ? "radio" : "checkbox");
        node.set("value", element.hasAttr("checked") ? 1 : 0);
        if (type == "radio" && element.hasAttr("name")) {
            bindRadioGroup(element, node);
        }
        return finishWidget(element, node);
    }

    if (type == "number" || type == "range") {
        const bool range = type == "range";
        auto node = new Node(range ? "slider" : "number");
        const double minimum = attrDouble(element, "min", 0.0);
        const double maximum = attrDouble(element, "max", range ? 100.0 : 0.0);
        // Number fields stay unbounded unless the attributes say otherwise.
        const bool hasMin = range || element.hasAttr("min");
        const bool hasMax = range || element.hasAttr("max");
        if (hasMin) {
            node.set("min", minimum);
        }
        if (hasMax && (!hasMin || maximum > minimum)) {
            node.set("max", maximum);
        }
        node.set("value", attrDouble(element, "value", range ? (minimum + maximum) / 2.0 : 0.0));
        if (element.hasAttr("step")) {
            node.set("step", attrDouble(element, "step", 1.0));
        }
        return finishWidget(element, node);
    }

    if (type == "color") {
        auto node = new Node("swatch");
        Color color;
        if (tryParseColor(value, color)) {
            node.set("color", color.toArray());
        }
        return finishWidget(element, node);
    }

    if (type == "submit" || type == "reset" || type == "button") {
        auto node = new Node("button");
        node.set("text", value.length ? value : (type == "reset" ? "Reset" : "Submit"));
        return finishWidget(element, node);
    }

    if (type == "file") {
        auto node = new Node("button");
        node.set("text", "Browse\u2026");
        return finishWidget(element, node);
    }

    // text, search, email, password, url, tel, date, ...
    auto node = new Node(type == "search" ? "search" : "text");
    if (value.length) {
        node.set("value", value);
    }
    if (placeholder.length) {
        node.set("placeholder", placeholder);
    }
    if (type == "password") {
        node.set("mask", "*");
    }
    return finishWidget(element, node);
}

private Node realizeSelect(Element element) {
    auto node = new Node("dropdown");
    string[] items;
    int selected;
    const string wanted = attrValue(element, "value", "");
    void collect(Element parent) {
        foreach (child; parent.children()) {
            if (child.tag == "option") {
                const string text = collapseTrimmed(child.textContent());
                const string value = attrValue(child, "value", text);
                items ~= text.length ? text : value;
                if (child.hasAttr("selected") || (wanted.length && value == wanted)) {
                    selected = cast(int) items.length - 1;
                }
            }
            else if (child.tag == "optgroup") {
                collect(child);
            }
        }
    }
    collect(element);
    if (items.length == 0) {
        const string placeholder = attrValue(element, "placeholder", "");
        if (placeholder.length) {
            items ~= placeholder;
        }
    }
    const(char)[][] options;
    foreach (item; items) {
        options ~= item;
    }
    node.setStrs("items", options);
    node.set("value", selected);
    return finishWidget(element, node);
}

private Node realizeTextarea(Element element) {
    auto node = new Node("textarea");
    const string text = attrValue(element, "value", element.textContent());
    if (text.length) {
        node.set("value", text);
    }
    const string placeholder = attrValue(element, "placeholder", "");
    if (placeholder.length) {
        node.set("placeholder", placeholder);
    }
    const int rows = attrInt(element, "rows", 3);
    const int cols = attrInt(element, "cols", 20);
    if (rows > 0) {
        node.geom("reqheight", rows * cast(int) (11.0f * scale() * 1.45f));
    }
    if (cols > 0) {
        node.geom("reqwidth", cols * cast(int) (7.0f * scale()));
    }
    return finishWidget(element, node);
}

private Node realizeProgress(Element element) {
    auto node = new Node("progress");
    const double max = attrDouble(element, "max", 1.0);
    if (element.hasAttr("value")) {
        node.set("factor", max > 0.0 ? attrDouble(element, "value", 0.0) / max : 0.0);
    }
    else {
        // No value: indeterminate, as in HTML.
        node.set("factor", -1.0);
    }
    const string label = collapseTrimmed(element.textContent());
    if (label.length) {
        node.set("text", label);
    }
    return finishWidget(element, node);
}

private Node realizeMeter(Element element) {
    auto node = new Node("meter");
    const double min = attrDouble(element, "min", 0.0);
    const double max = attrDouble(element, "max", 1.0);
    node.set("min", min);
    node.set("max", max);
    const double value = attrDouble(element, "value", 0.0);
    node.set("value", value < min ? min : value > max ? max : value);
    if (element.hasAttr("low")) {
        node.set("low", attrDouble(element, "low", min));
    }
    if (element.hasAttr("high")) {
        node.set("high", attrDouble(element, "high", max));
    }
    if (element.hasAttr("optimum")) {
        node.set("optimum", attrDouble(element, "optimum", (min + max) / 2.0));
    }
    const string label = collapseTrimmed(element.textContent());
    if (label.length) {
        node.set("text", label);
    }
    return finishWidget(element, node);
}

private Node realizeLeafWidget(Element element) {
    const string text = collapseTrimmed(element.textContent());
    if (text.length == 0) {
        return null;
    }
    auto label = textNode(text, element.computed);
    auto box = decorateLeaf(element, label);
    finish(element, label, box.outer, box.background);
    return box.outer;
}

private Node finishWidget(Element element, Node widget) {
    const Style style = element.computed;
    auto box = hasDecoration(style)
        ? makeBox(element)
        : Box(widget, widget, null);
    if (box.flow !is widget) {
        box.flow.add(widget);
        widget.pack("side", 0).pack("fill", 3);
    }
    finish(element, widget, box.outer, box.background);
    return box.outer;
}

/* -- replaced content ----------------------------------------------------- */

private Node realizeReplaced(Element element) {
    switch (element.tag) {
    case "img":
        return realizeImage(element);
    case "canvas":
        return realizeCanvas(element);
    case "video":
        return realizeVideo(element);
    case "iframe":
    case "embed":
    case "object": {
        auto frame = new Node("frame");
        const int width = attrInt(element, "width", 300);
        const int height = attrInt(element, "height", 150);
        frame.geom("reqwidth", width).geom("reqheight", height);
        finish(element, frame, frame, null);
        return frame;
    }
    default:
        return null;
    }
}

private Node realizeImage(Element element) {
    const string src = attrValue(element, "src", "");
    if (src.length == 0) {
        const string alt = attrValue(element, "alt", "");
        if (alt.length) {
            auto label = textNode(alt, element.computed);
            auto box = decorateLeaf(element, label);
            finish(element, label, box.outer, box.background);
            return box.outer;
        }
        auto frame = new Node("frame");
        finish(element, frame, frame, null);
        return frame;
    }
    auto image = new Node("image");
    image.setImageSource(src, element.computed.objectFit);
    applyWidthHeightAttrs(element, image);
    finish(element, image, image, null);
    return image;
}

private Node realizeCanvas(Element element) {
    auto canvas = new Node("canvas");
    const int width = attrInt(element, "width", 300);
    const int height = attrInt(element, "height", 150);
    canvas.geom("reqwidth", width).geom("reqheight", height);
    finish(element, canvas, canvas, null);
    return canvas;
}

private Node realizeVideo(Element element) {
    const string src = attrValue(element, "src", "");
    auto video = new Node("video");
    if (src.length) {
        try {
            auto player = new Video(src);
            video.setVideo(player);
        }
        catch (Exception) {
            // An undecodable source leaves the empty video node; the poster
            // or CSS background still shows.
        }
    }
    applyWidthHeightAttrs(element, video);
    finish(element, video, video, null);
    return video;
}

private void applyWidthHeightAttrs(Element element, Node node) {
    const int width = attrInt(element, "width", 0);
    const int height = attrInt(element, "height", 0);
    if (width > 0) {
        node.geom("reqwidth", width);
    }
    if (height > 0) {
        node.geom("reqheight", height);
    }
}

/* -- boxes, decorations, packing ------------------------------------------ */

private bool hasDecoration(const Style style) {
    return (style.hasBackground && !style.background.isTransparent()) ||
           style.borderWidth >= 0.5f || hasPadding(style);
}

private bool hasPadding(const Style style) {
    foreach (side; style.padding) {
        if (side >= 0.5f) {
            return true;
        }
    }
    return false;
}

private bool hasMargin(const Style style) {
    foreach (side; style.margin) {
        if (side >= 0.5f) {
            return true;
        }
    }
    return false;
}

private Box makeBox(Element element) {
    const Style style = element.computed;
    Node frame = new Node("frame");
    Node outer = frame;
    Node background = frame;

    const int border = px(style.borderWidth);
    if (border > 0) {
        outer = new Node("frame");
        outer.set("bg", style.borderColor.toArray());
        if (style.hasBackground && !style.background.isTransparent()) {
            frame.set("bg", style.background.toArray());
        }
        background = frame;
        frame.pack("side", 0).pack("fill", 3).pack("expand", 1);
        setPads(frame, border, border, border, border);
        outer.add(frame);
    }
    else if (style.hasBackground && !style.background.isTransparent()) {
        frame.set("bg", style.background.toArray());
    }

    Node flow = frame;
    const int top = px(style.padding[0]);
    const int right = px(style.padding[1]);
    const int bottom = px(style.padding[2]);
    const int left = px(style.padding[3]);
    if (top == right && right == bottom && bottom == left) {
        if (top > 0) {
            frame.set("padding", top);
        }
    }
    else if (top || right || bottom || left) {
        flow = new Node("frame");
        flow.pack("side", 0).pack("fill", 3).pack("expand", 1);
        setPads(flow, left, top, right, bottom);
        frame.add(flow);
    }

    element.flowNode_ = flow;
    element.backgroundNode_ = background;
    return Box(outer, flow, background);
}

/// Wrap a text/leaf node when the element needs a background, border or
/// padding.
private Box decorateLeaf(Element element, Node leaf) {
    const Style style = element.computed;
    if (!hasDecoration(style)) {
        return Box(leaf, leaf, null);
    }
    auto box = makeBox(element);
    box.flow.add(leaf);
    leaf.pack("side", 0).pack("fill", 3);
    return box;
}

private void finish(Element element, Node semantic, Node outer, Node background) {
    element.kitNode_ = semantic;
    element.outerNode_ = outer;
    element.backgroundNode_ = background;

    const Style style = element.computed;
    outer.set("visible", style.visible && !element.hasAttr("hidden"));
    if (style.cursor >= 0) {
        outer.set("cursor", cursorCode(style.cursor));
    }
    const int width = px(style.width);
    const int height = px(style.height);
    if (width > 0) {
        outer.geom("reqwidth", width);
    }
    if (height > 0) {
        outer.geom("reqheight", height);
    }
    if (element.hasAttr("disabled")) {
        semantic.set("enabled", false);
    }
    applyTextStyle(semantic, style);
    bindEvents(element, semantic, element.onStateChange);
}

private Node textNode(string text, const Style style) {
    auto node = new Node("label");
    node.set("text", text);
    applyTextStyle(node, style);
    return node;
}

private void applyTextStyle(Node node, const Style style) {
    node.set("font_scale", style.fontSize / 11.0f);
    node.set("text_color", style.color.toArray());
    node.set("bold", style.fontWeight >= 600);
    switch (style.textAlign) {
    case 1:
        node.set("text_align", "center");
        break;
    case 2:
        node.set("text_align", "right");
        break;
    default:
        node.set("text_align", "left");
        break;
    }
}

private void packItem(Item item, Node parent, int side, bool fillX, bool fillY,
                      int leadingGap, int trailingGap) {
    const Style style = item.style;
    item.node.pack("side", side);
    int fill;
    if (fillX) {
        fill |= 1;
    }
    if (fillY) {
        fill |= 2;
    }
    if (fill) {
        item.node.pack("fill", fill);
    }
    if (style.flexGrow > 0.0f) {
        item.node.pack("expand", 1);
    }
    const int left = px(style.margin[3]) + (side >= 2 ? leadingGap : 0);
    const int right = px(style.margin[1]) + (side >= 2 ? trailingGap : 0);
    const int top = px(style.margin[0]) + (side <= 1 ? leadingGap : 0);
    const int bottom = px(style.margin[2]) + (side <= 1 ? trailingGap : 0);
    setPads(item.node, left, top, right, bottom);
    parent.add(item.node);
}

private void setInsets(Node node, const Style style, int extraX, int extraY) {
    setPads(node, px(style.margin[3]) + extraX, px(style.margin[0]) + extraY,
            px(style.margin[1]), px(style.margin[2]));
}

private void setPads(Node node, int left, int top, int right, int bottom) {
    if (left) {
        node.geom("padx0", left);
    }
    if (right) {
        node.geom("padx1", right);
    }
    if (top) {
        node.geom("pady0", top);
    }
    if (bottom) {
        node.geom("pady1", bottom);
    }
}

private int px(float cssPixels) {
    if (cssPixels <= 0.0f) {
        return 0;
    }
    return cast(int) (cssPixels * scale() + 0.5f);
}

/* -- attributes ----------------------------------------------------------- */

private string attrValue(Element element, string name, string fallback) {
    return element.hasAttr(name) ? element.attrValue(name) : fallback;
}

private int attrInt(Element element, string name, int fallback) {
    if (!element.hasAttr(name)) {
        return fallback;
    }
    try {
        return to!int(element.attrValue(name));
    }
    catch (Exception) {
        return fallback;
    }
}

private double attrDouble(Element element, string name, double fallback) {
    if (!element.hasAttr(name)) {
        return fallback;
    }
    try {
        return to!double(element.attrValue(name));
    }
    catch (Exception) {
        return fallback;
    }
}

private int cursorCode(int cursor) {
    final switch (cast(Cursor) cursor) {
    case Cursor.default_:
        return BK_CURSOR_DEFAULT;
    case Cursor.ewResize:
        return BK_CURSOR_EW;
    case Cursor.nsResize:
        return BK_CURSOR_NS;
    case Cursor.text:
        return BK_CURSOR_TEXT;
    case Cursor.crosshair:
        return BK_CURSOR_CROSSHAIR;
    case Cursor.hand:
        return BK_CURSOR_HAND;
    case Cursor.move:
        return BK_CURSOR_MOVE;
    case Cursor.none_:
        return BK_CURSOR_NONE;
    case Cursor.neswResize:
        return BK_CURSOR_NESW;
    case Cursor.nwseResize:
        return BK_CURSOR_NWSE;
    case Cursor.notAllowed:
        return BK_CURSOR_NOT_ALLOWED;
    case Cursor.wait:
        return BK_CURSOR_WAIT;
    case Cursor.progress:
        return BK_CURSOR_PROGRESS;
    }
}

private bool isInline(const Style style) {
    return style.display == Display.inline_ ||
           style.display == Display.inlineBlock ||
           style.display == Display.inlineFlex ||
           style.display == Display.inlineGrid;
}

