/* Every HTML element as a constructor, and the user-agent stylesheet.
 *
 * The tag list is the WHATWG index: box elements are laid out from CSS,
 * widgets become real kit controls with Blender's interaction, replaced
 * elements carry intrinsic content, and metadata never renders.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
module bui.html.tags;

import bui.html.element : Element, makeElement;

/// How an element is realised.
enum TagKind {
    /// Laid out from CSS (most elements).
    box,
    /// A real kit widget: Blender's control with Blender's interaction.
    widget,
    /// Replaced content with an intrinsic size (`img`, `canvas`, ...).
    replaced,
    /// Metadata: not rendered (`head`, `style`, ...).
    metadata,
    /// Line break / break opportunity.
    break_,
}

private immutable string[] boxTags = [
    "html", "body", "article", "section", "nav", "aside", "h1", "h2", "h3",
    "h4", "h5", "h6", "hgroup", "header", "footer", "address", "p", "hr",
    "pre", "blockquote", "ol", "ul", "menu", "li", "dl", "dt", "dd", "figure",
    "figcaption", "main", "search", "div", "a", "em", "strong", "small", "s",
    "cite", "q", "dfn", "abbr", "ruby", "rt", "rp", "data", "time", "code",
    "var", "samp", "kbd", "sub", "sup", "i", "b", "u", "mark", "bdi", "bdo",
    "span", "ins", "del", "picture", "table", "caption", "colgroup", "col",
    "tbody", "thead", "tfoot", "tr", "td", "th", "form", "label", "fieldset",
    "legend", "details", "summary", "dialog", "noscript", "slot", "output",
    "map", "selectedcontent", "svg", "math",
];

private immutable string[] widgetTags = [
    "button", "input", "select", "option", "optgroup", "textarea", "progress",
    "meter", "datalist",
];

private immutable string[] replacedTags = [
    "img", "iframe", "embed", "object", "video", "audio", "canvas",
];

private immutable string[] metadataTags = [
    "head", "title", "base", "link", "meta", "style", "script", "template",
    "source", "track", "area",
];

private immutable string[] breakTags = ["br", "wbr"];

/* -- constructors --------------------------------------------------------- */

private string constructorName(string tag) {
    // `template` and `body` are D keywords, `object` is D's implicitly
    // imported module, and `select` is `bui.tree.select`. Those take a
    // trailing underscore; every other tag keeps its HTML name.
    if (tag == "template") {
        return "template_";
    }
    if (tag == "body") {
        return "body_";
    }
    if (tag == "object") {
        return "object_";
    }
    if (tag == "select") {
        return "select_";
    }
    return tag;
}

private string tagConstructors() {
    string code;
    foreach (group; [boxTags, widgetTags, replacedTags, metadataTags, breakTags]) {
        foreach (tag; group) {
            code ~= "/// The `<" ~ tag ~ ">` element.\n";
            code ~= "Element " ~ constructorName(tag) ~
                    "(Args...)(Args args) { return makeElement!\"" ~ tag ~ "\"(args); }\n";
        }
    }
    return code;
}

mixin(tagConstructors());

/// A custom element (`custom("my-widget")`), laid out like a box.
Element custom(string tag) {
    return new Element(tag);
}

/// How `tag` is realised. Unknown tags are boxes, as in browsers.
TagKind tagKind(string tag) {
    return tagKindOf(tag);
}

private TagKind tagKindOf(string tag) {
    foreach (value; boxTags) {
        if (value == tag) {
            return TagKind.box;
        }
    }
    foreach (value; widgetTags) {
        if (value == tag) {
            return TagKind.widget;
        }
    }
    foreach (value; replacedTags) {
        if (value == tag) {
            return TagKind.replaced;
        }
    }
    foreach (value; metadataTags) {
        if (value == tag) {
            return TagKind.metadata;
        }
    }
    foreach (value; breakTags) {
        if (value == tag) {
            return TagKind.break_;
        }
    }
    return TagKind.box;
}

/// The user-agent stylesheet: the browser's structural defaults drawn in
/// Blender's theme (the kit supplies colours, so the sheet stays neutral).
enum string uaCss = `
html { display: block; }
body { display: block; margin: 8px; }
address, article, aside, blockquote, dd, details, dialog, div, dl, dt, fieldset,
figcaption, figure, footer, form, h1, h2, h3, h4, h5, h6, header, hgroup, hr,
legend, main, menu, nav, ol, p, pre, search, section, ul, noscript, optgroup,
option { display: block; }
head, title, base, link, meta, style, script, template, source, track, area,
datalist, wbr, map, [hidden], input[type=hidden], col, colgroup { display: none; }
li, summary { display: list-item; }
a, abbr, b, bdi, bdo, cite, code, data, dfn, em, i, kbd, label, mark, output,
q, ruby, rt, rp, s, samp, small, span, strong, sub, sup, time, u, var {
    display: inline;
}
p, dl, pre { margin: 0.6em 0; }
blockquote, figure { margin: 0.6em 0; }
h1 { font-size: 2em; margin: 0.5em 0; font-weight: bold; }
h2 { font-size: 1.5em; margin: 0.5em 0; font-weight: bold; }
h3 { font-size: 1.17em; margin: 0.6em 0; font-weight: bold; }
h4 { font-size: 1em; margin: 0.8em 0; font-weight: bold; }
h5 { font-size: 0.83em; margin: 1em 0; font-weight: bold; }
h6 { font-size: 0.67em; margin: 1.2em 0; font-weight: bold; }
b, strong, th, legend, dt, caption { font-weight: bold; }
i, em, cite, dfn, var, address { font-style: italic; }
small { font-size: smaller; }
code, kbd, samp, pre, var { font-family: monospace; }
pre { background-color: rgba(127, 127, 127, 0.12); padding: 6px 8px; }
mark { background-color: #e6a23c; color: #1d1d1d; }
a { cursor: pointer; }
ul, menu { padding-left: 1.5em; margin: 0.6em 0; }
ol { padding-left: 1.5em; margin: 0.6em 0; }
dd { margin-left: 1.5em; }
hr { margin: 8px 0; }
table { display: grid; }
thead, tbody, tfoot { display: block; }
tr { display: block; }
td, th { display: block; padding: 2px 6px; }
th { text-align: center; font-weight: bold; }
caption { display: block; text-align: center; }
fieldset { margin: 4px 0; padding: 6px 10px 8px; border: 1px solid rgba(127, 127, 127, 0.5); }
legend { padding: 0 2px; margin-bottom: 4px; }
details { margin: 2px 0; }
summary { cursor: pointer; font-weight: bold; padding: 2px 0; }
dialog { padding: 12px 16px; border: 1px solid rgba(127, 127, 127, 0.6); }
dialog:not([open]) { display: none; }
img, video, iframe, embed, object, audio, canvas, svg, math { display: inline-block; }
button, input, select, textarea, progress, meter { display: inline-block; }
audio:not([controls]) { display: none; }
label { cursor: default; }
output { display: inline; }
`;
