//! `rsx!`: JSX-style markup lowered to blenderui's builder calls.
//!
//! The lowering is uniform, so nothing is special-cased per element or
//! attribute; the compiler checks the result against the real API:
//!
//! | markup                         | becomes                                   |
//! |--------------------------------|-------------------------------------------|
//! | `<div ...>`                    | `html::div()` (any tag; `<my-el>` -> `Div::new("my-el")`) |
//! | `<Card a={x}>..</Card>`        | `Card::__builder().a(x).children(vec![..])` (components; `Option` props may be left out) |
//! | `name="lit"` / bare `name`     | `.attr("name", "lit")` (`class`, `style`, `id`: their methods) |
//! | `name={expr}`                  | `.name(expr)`: any builder method (`checked`, `w`, `hover` ...) |
//! | `onClick={|| ..}`              | `.on_click(|_, _, _| ..)` (missing listener params added) |
//! | `"text"`, `{expr}`, `{...it}`  | `.child(..)`, `.children(it)`             |
//! | `{for x in it { <li/> }}`      | `.children(it.into_iter().map(..))`       |
//! | `{if c { <a/> } else { .. }}`  | `.children(if c { Some(..) } else { .. })` |
//! | `{match v { p => <a/>, .. }}`  | `.child(match v { p => .., .. })`         |
//! | `<>..</>`                      | `display: contents` wrapper               |
//! | `<motion.div ..>`              | `html::div()`: every element takes Motion's props |
//! | `animate={{ x: 100 }}`         | `.animate(vars!{ x: 100 })` (`{{ }}`: a vars object) |
//! | `animate="visible"`            | `.animate("visible")` (a variant; also initial, exit, while*) |

use proc_macro::{Delimiter, Group, Spacing, TokenStream, TokenTree};

type R<T> = Result<T, String>;

pub fn rsx(input: TokenStream) -> TokenStream {
    let toks: Vec<TokenTree> = input.into_iter().collect();
    match markup(&toks) {
        Ok(ts) => ts,
        Err(e) => format!("::core::compile_error!({e:?})").parse().unwrap(),
    }
}

fn ts(s: &str) -> TokenStream {
    s.parse().expect("rsx!: scaffold")
}

fn is(t: Option<&TokenTree>, c: char) -> bool {
    matches!(t, Some(TokenTree::Punct(p)) if p.as_char() == c)
}

/// One or more nodes; several become a fragment.
fn markup(toks: &[TokenTree]) -> R<TokenStream> {
    let mut p = P { t: toks, i: 0 };
    let mut nodes = Vec::new();
    while p.i < p.t.len() {
        nodes.push(p.node()?);
    }
    Ok(match nodes.len() {
        1 => match nodes.pop() {
            Some(Child::One(e)) => e,
            Some(other) => fragment(vec![other]),
            None => unreachable!(),
        },
        _ => fragment(nodes),
    })
}

fn fragment(nodes: Vec<Child>) -> TokenStream {
    let mut out = ts("::blenderui::html::div().style(\"display: contents\")");
    for n in nodes {
        out.extend(n.call());
    }
    out
}

/// A child, as the call that adds it.
enum Child {
    One(TokenStream),
    Many(TokenStream),
}

impl Child {
    fn call(self) -> TokenStream {
        let (m, e) = match self {
            Child::One(e) => ("child", e),
            Child::Many(e) => ("children", e),
        };
        let mut out = ts(&format!(".{m}"));
        out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, e))]);
        out
    }
}

struct P<'a> {
    t: &'a [TokenTree],
    i: usize,
}

impl P<'_> {
    fn peek(&self) -> Option<&TokenTree> {
        self.t.get(self.i)
    }

    fn node(&mut self) -> R<Child> {
        match self.peek() {
            Some(TokenTree::Punct(p)) if p.as_char() == '<' => self.element().map(Child::One),
            Some(TokenTree::Literal(l)) => {
                let l = l.clone();
                self.i += 1;
                Ok(Child::One(TokenStream::from(TokenTree::Literal(l))))
            }
            Some(TokenTree::Group(g)) if g.delimiter() == Delimiter::Brace => {
                let g = g.clone();
                self.i += 1;
                braced(&g)
            }
            Some(TokenTree::Ident(_)) => {
                // bare text: words up to the next tag or brace, joined by spaces
                let mut words = Vec::new();
                while let Some(t @ (TokenTree::Ident(_) | TokenTree::Punct(_) | TokenTree::Literal(_))) = self.peek() {
                    if is(Some(t), '<') {
                        break;
                    }
                    words.push(t.to_string());
                    self.i += 1;
                }
                Ok(Child::One(ts(&format!("{:?}", join_words(&words)))))
            }
            Some(t) => Err(format!("rsx!: unexpected `{t}`")),
            None => Err("rsx!: unexpected end".into()),
        }
    }

    /// `name` or `name-with-dashes` (tags and attributes).
    fn name(&mut self) -> R<String> {
        // pieces joined by `-` / `:` (and `@` / `*` starting a piece): `data-x`,
        // `md:hover:class`, `2xl:class`, `@md:class`, `*:class`, `data-[state=open]:class`
        let mut s = String::new();
        let mut want_piece = true;
        let mut last_end: Option<(usize, usize)> = None;
        loop {
            let t = self.peek().cloned();
            // pieces of one name touch in the source: `h-1 -mt-2` is two names
            if let (Some(end), Some(t)) = (last_end, &t) {
                let start = t.span().start();
                if (start.line(), start.column()) != end {
                    break;
                }
            }
            if let Some(t) = &t {
                let e = t.span().end();
                last_end = Some((e.line(), e.column()));
            }
            match t {
                Some(TokenTree::Ident(i)) if want_piece => s.push_str(&i.to_string()),
                Some(TokenTree::Literal(l)) if want_piece && !l.to_string().starts_with('"') => s.push_str(&l.to_string()),
                Some(TokenTree::Group(g)) if want_piece && g.delimiter() == Delimiter::Bracket => {
                    s.push('[');
                    s.push_str(&g.stream().to_string().replace(' ', ""));
                    s.push(']');
                }
                // `-mt-2`, `!p-4`: a sign before the first piece
                Some(TokenTree::Punct(p)) if matches!(p.as_char(), '-' | '!') && s.is_empty() => {
                    s.push(p.as_char());
                    self.i += 1;
                    continue;
                }
                // `w-1/2`, `bg-sky-500/50`, `text-sm/6` (not the `/` of `/>`)
                Some(TokenTree::Punct(p)) if p.as_char() == '/' && !want_piece && !s.is_empty()
                    && !is(self.t.get(self.i + 1), '>') => {
                    s.push('/');
                    self.i += 1;
                    want_piece = true;
                    continue;
                }
                // `p-4!`: important
                Some(TokenTree::Punct(p)) if p.as_char() == '!' && !want_piece && !s.is_empty() => {
                    s.push('!');
                    self.i += 1;
                    break;
                }
                Some(TokenTree::Punct(p)) if matches!(p.as_char(), '@' | '*') && want_piece => {
                    s.push(p.as_char());
                    self.i += 1;
                    continue; // still expecting the rest of the piece (or a separator after `*`)
                }
                Some(TokenTree::Punct(p)) if matches!(p.as_char(), '-' | ':') && !s.is_empty() && !want_piece => {
                    s.push(p.as_char());
                    self.i += 1;
                    want_piece = true;
                    continue;
                }
                // `motion.div`: Motion's elements
                Some(TokenTree::Punct(p)) if p.as_char() == '.' && s == "motion" && !want_piece => {
                    s.push('.');
                    self.i += 1;
                    want_piece = true;
                    continue;
                }
                Some(TokenTree::Punct(p)) if p.as_char() == ':' && s.ends_with('*') => {
                    s.push(':');
                    self.i += 1;
                    continue;
                }
                _ => break,
            }
            self.i += 1;
            want_piece = false;
        }
        if s.is_empty() || s.ends_with(['-', ':', '/']) {
            return Err(format!("rsx!: expected a name, found `{}`",
                               self.peek().map(|t| t.to_string()).unwrap_or_default()));
        }
        Ok(s.trim_start_matches("r#").to_string())
    }

    fn element(&mut self) -> R<TokenStream> {
        self.i += 1; // <
        if is(self.peek(), '>') {
            // fragment <>...</>
            self.i += 1;
            let kids = self.children_until("")?;
            return Ok(fragment(kids));
        }
        let written = self.name()?;
        // `<motion.div>` is `<div>`: every element takes Motion's props
        let tag = written.strip_prefix("motion.").unwrap_or(&written).to_string();
        // Blender's widget components (`<Button>`, `<NumberSlider>` ...) are
        // elements: their attributes work like on any HTML element
        let blender = BLENDER_COMPONENTS.contains(&tag.as_str());
        let component = !blender && (tag.chars().next().is_some_and(|c| c.is_ascii_uppercase())
            || !tag.contains('-') && !tag.contains(':') && !HTML_TAGS.contains(&tag.as_str()));
        let mut base = if component {
            TokenStream::new()
        } else if blender {
            ts(&format!("::blenderui::components::{tag}::__builder()"))
        } else if tag.contains('-') || tag.contains(':') {
            ts(&format!("::blenderui::element::Div::new({tag:?})"))
        } else {
            ts(&format!("::blenderui::html::{tag}()"))
        };
        let mut fields = TokenStream::new(); // component props: `.name(value)` builder calls
        let mut key: Option<TokenStream> = None; // `key=`: hook identity
        let mut classes: Vec<String> = Vec::new(); // bare utility attributes
        loop {
            match self.peek() {
                Some(TokenTree::Punct(p)) if p.as_char() == '/' => {
                    self.i += 1;
                    if !is(self.peek(), '>') {
                        return Err(format!("rsx!: expected `>` after `/` in <{tag}>"));
                    }
                    self.i += 1;
                    if !classes.is_empty() {
                        base.extend(ts(&format!(".class({:?})", classes.join(" "))));
                    }
                    return Ok(if component { component_expr(&tag, fields, Vec::new(), key) } else { keyed(base, key) });
                }
                Some(TokenTree::Punct(p)) if p.as_char() == '>' => {
                    self.i += 1;
                    break;
                }
                Some(TokenTree::Group(g)) if g.delimiter() == Delimiter::Brace && !component => {
                    // `{...spread}` attributes: a builder closure `{|el| el.w(px(3.))}`
                    let g = g.clone();
                    self.i += 1;
                    let mut call = TokenStream::from(TokenTree::Group(Group::new(Delimiter::Parenthesis, g.stream())));
                    call.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, std::mem::take(&mut base)))]);
                    base = call;
                }
                _ => {
                    let mut name = self.name()?;
                    let value = if is(self.peek(), '=') {
                        self.i += 1;
                        match self.peek() {
                            Some(t @ (TokenTree::Literal(_) | TokenTree::Group(_))) => {
                                let (obj, is_object) = vars_object(t)?;
                                // `style={{ x: mv }}`: Motion's style (values read every frame)
                                if name == "style" && is_object {
                                    name = "motionStyle".into();
                                }
                                self.i += 1;
                                Some(obj)
                            }
                            _ => return Err(format!("rsx!: `{name}=` needs \"text\" or {{expr}}")),
                        }
                    } else {
                        None
                    };
                    if value.is_none() && !component && !MOTION_FLAGS.contains(&name.as_str()) {
                        match bare(&name)? {
                            Some(class) => {
                                classes.push(class);
                                continue;
                            }
                            None => {}
                        }
                    }
                    if name == "key" || component {
                        // component setters take `impl Into<T>`: literals go in as they are
                        let v = match value {
                            Some(TokenTree::Group(g)) => g.stream(),
                            Some(t) => TokenStream::from(t),
                            None => ts("true"),
                        };
                        if name == "key" {
                            key = Some(v);
                        } else {
                            fields.extend(ts(&format!(".{}", name.replace('-', "_"))));
                            fields.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, v))]);
                        }
                    } else {
                        base.extend(attribute(&name, value)?);
                    }
                }
            }
        }
        if !classes.is_empty() {
            base.extend(ts(&format!(".class({:?})", classes.join(" "))));
        }
        let kids = self.children_until(&written)?;
        if component {
            return Ok(component_expr(&tag, fields, kids, key));
        }
        for k in kids {
            base.extend(k.call());
        }
        Ok(keyed(base, key))
    }

    /// Children up to `</tag>` (or `</>` for fragments).
    fn children_until(&mut self, tag: &str) -> R<Vec<Child>> {
        let mut kids = Vec::new();
        loop {
            if is(self.peek(), '<') && is(self.t.get(self.i + 1), '/') {
                self.i += 2;
                if !tag.is_empty() {
                    let close = self.name()?;
                    if close != tag {
                        return Err(format!("rsx!: <{tag}> closed by </{close}>"));
                    }
                }
                if !is(self.peek(), '>') {
                    return Err(format!("rsx!: expected `>` in </{tag}>"));
                }
                self.i += 1;
                return Ok(kids);
            }
            if self.peek().is_none() {
                return Err(format!("rsx!: <{tag}> is never closed"));
            }
            kids.push(self.node()?);
        }
    }
}

fn join_words(words: &[String]) -> String {
    let mut s = String::new();
    for w in words {
        let glue = !s.is_empty() && !matches!(w.as_str(), "," | "." | "!" | "?" | ";" | ":" | "'" | ")")
            && !s.ends_with(['(', '\'']);
        if glue {
            s.push(' ');
        }
        s.push_str(w);
    }
    s
}

/// `key={k}`: hooks under this element are identified by `k`, not position.
fn keyed(el: TokenStream, key: Option<TokenStream>) -> TokenStream {
    match key {
        None => el,
        Some(k) => {
            let mut args = k;
            args.extend(ts(","));
            args.extend(el);
            let mut out = ts("::blenderui::hooks::keyed");
            out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, args))]);
            out
        }
    }
}

/// `<Card a={x}>kids</Card>` -> `build(Card::__builder().a(x).children(vec![..]))`:
/// `Option` props and `children` may be left out (`#[component]` and
/// `#[derive(Props)]` generate the builder).
fn component_expr(tag: &str, mut fields: TokenStream, kids: Vec<Child>, key: Option<TokenStream>) -> TokenStream {
    if !kids.is_empty() {
        // { let mut v = Vec::new(); v.push(..); v.extend(..); v }
        let mut body = ts("let mut __v: ::std::vec::Vec<::blenderui::element::AnyElement> = ::std::vec::Vec::new();");
        for k in kids {
            match k {
                Child::One(e) => {
                    body.extend(ts("__v.push"));
                    body.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, any(e)))]);
                }
                Child::Many(e) => {
                    let mut it = ts("::core::iter::IntoIterator::into_iter");
                    it.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, e))]);
                    it.extend(ts(".map(::blenderui::element::IntoElement::into_any_element)"));
                    body.extend(ts("__v.extend"));
                    body.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, it))]);
                }
            }
            body.extend(ts(";"));
        }
        body.extend(ts("__v"));
        fields.extend(ts(".children"));
        fields.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis,
            TokenStream::from(TokenTree::Group(Group::new(Delimiter::Brace, body)))))]);
    }
    let mut chain = ts(&format!("{tag}::__builder()"));
    chain.extend(fields);
    let mut out = ts("::blenderui::hooks::build");
    out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, chain))]);
    keyed(out, key)
}

/// `hover:class="..."`, `md:dark:style="..."`, `hover:color="red"`,
/// `active:bg={red()}`: an attribute under Tailwind variants. Classes get the
/// prefix, CSS becomes arbitrary properties (`hover:[color:red]`), so every
/// Tailwind variant works through the same engine; typed `{expr}` values
/// use the native `.hover()` / `.active()` / `.focus()` refinements.
fn variant_attribute(name: &str, value: Option<TokenTree>) -> R<TokenStream> {
    let (variants, base) = name.rsplit_once(':').ok_or("rsx!: variant")?;
    let prefix = format!("{variants}:");
    let class_call = |s: String| ts(&format!(".class({s:?})"));
    // a CSS declaration list -> `v:[prop:value]` classes
    let decls_to_classes = |css: &str| -> String {
        css.split(';').filter_map(|d| {
            let (p, v) = d.split_once(':')?;
            let v = v.trim().replace('_', "\\_").replace(' ', "_");
            Some(format!("{prefix}[{}:{}]", p.trim(), v))
        }).collect::<Vec<_>>().join(" ")
    };
    let unquote = |l: &proc_macro::Literal| -> R<String> {
        let s = l.to_string();
        s.strip_prefix('"').and_then(|s| s.strip_suffix('"')).map(|s| s.replace("\\\"", "\""))
            .ok_or_else(|| format!("rsx!: `{name}` needs a string"))
    };
    Ok(match (base, value) {
        ("class", Some(TokenTree::Literal(l))) => {
            let v = unquote(&l)?;
            class_call(v.split_whitespace().map(|c| format!("{prefix}{c}")).collect::<Vec<_>>().join(" "))
        }
        ("style", Some(TokenTree::Literal(l))) => class_call(decls_to_classes(&unquote(&l)?)),
        // run time: prefix the classes / convert the CSS of an expression
        ("class" | "style", Some(TokenTree::Group(g))) => {
            let conv = if base == "class" {
                format!("__c.split_whitespace().map(|c| ::std::format!(\"{prefix}{{c}}\")).collect::<::std::vec::Vec<_>>().join(\" \")")
            } else {
                format!("__c.split(';').filter_map(|d| {{ let (p, v) = d.split_once(':')?; \
                         ::core::option::Option::Some(::std::format!(\"{prefix}[{{}}:{{}}]\", p.trim(), \
                         v.trim().replace('_', \"\\\\_\").replace(' ', \"_\"))) }}) \
                         .collect::<::std::vec::Vec<_>>().join(\" \")")
            };
            let mut arg = ts("&");
            arg.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, g.stream()))]);
            let mut body = ts("let __c: ::std::string::String = ::std::string::ToString::to_string");
            body.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, arg))]);
            body.extend(ts(&format!("; {conv}")));
            let mut out = ts(".class");
            out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis,
                TokenStream::from(TokenTree::Group(Group::new(Delimiter::Brace, body)))))]);
            out
        }
        // hover:color="red": a CSS property under the variants
        (prop, Some(TokenTree::Literal(l))) => class_call(decls_to_classes(&format!("{prop}: {}", unquote(&l)?))),
        // active:bg={red()}: a typed style method in the state's refinement
        (method, Some(TokenTree::Group(g))) if matches!(variants, "hover" | "active" | "focus") => {
            let mut inner = ts(&format!("|__s| __s.{}", method_name(method)));
            inner.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, g.stream()))]);
            let mut out = ts(&format!(".{variants}"));
            out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, inner))]);
            out
        }
        (_, Some(TokenTree::Group(_))) => {
            return Err(format!("rsx!: `{name}={{..}}`: typed values work under hover / active / focus; \
                                use `{variants}:class` or `{variants}:style` for other variants"));
        }
        _ => return Err(format!("rsx!: `{name}` needs a value")),
    })
}

/// Tags with an element function; any other lowercase tag is a component
/// (`<profile ..>` -> `profile { .. }`, as in zopra).
const HTML_TAGS: &[&str] = &[
    "html", "body", "article", "section", "nav", "aside", "h1", "h2", "h3", "h4", "h5", "h6", "hgroup",
    "header", "footer", "address", "p", "hr", "pre", "blockquote", "ol", "ul", "menu", "li", "dl", "dt", "dd",
    "figure", "figcaption", "main", "search", "div", "a", "em", "strong", "small", "s", "cite", "q", "dfn",
    "abbr", "ruby", "rt", "rp", "data", "time", "code", "var", "samp", "kbd", "sub", "sup", "i", "b", "u",
    "mark", "bdi", "bdo", "span", "ins", "del", "picture", "table", "caption", "colgroup", "col", "tbody",
    "thead", "tfoot", "tr", "td", "th", "form", "label", "fieldset", "legend", "details", "summary", "dialog",
    "noscript", "slot", "output", "map", "selectedcontent", "svg", "math", "button", "input", "select",
    "option", "optgroup", "textarea", "progress", "meter", "datalist", "img", "iframe", "embed", "object",
    "video", "audio", "head", "title", "base", "link", "meta", "style", "script", "template", "source",
    "track", "area", "br", "wbr",
];

/// Blender's widgets (`blenderui::components`): capitalized, used like elements.
const BLENDER_COMPONENTS: &[&str] = &[
    "Button", "Checkbox", "Radio", "NumberField", "NumberSlider", "TextField", "TextArea", "ColorSwatch",
    "Dropdown", "ProgressBar",
];

/// HTML boolean attributes: bare, they stay attributes even where a
/// Tailwind utility has the same name (`hidden`).
const BOOLEAN_ATTRS: &[&str] = &[
    "allowfullscreen", "async", "autofocus", "autoplay", "checked", "controls", "default", "defer", "disabled",
    "formnovalidate", "hidden", "inert", "ismap", "itemscope", "loop", "multiple", "muted", "nomodule",
    "novalidate", "open", "playsinline", "readonly", "required", "reversed", "selected", "popover", "draggable",
    "contenteditable", "spellcheck", "translate", "styled", "audioeffects",
];

/// A bare attribute (`<div p-4 hover:bg-sky-500>`): Some(class) when it is
/// a Tailwind utility, None for a plain boolean attribute, an error with
/// suggestions when it looks like a misspelled utility.
fn bare(name: &str) -> R<Option<String>> {
    if BOOLEAN_ATTRS.contains(&name) || name.starts_with("data-") && !name.contains(':')
        || name.starts_with("aria-") && !name.contains(':')
    {
        return Ok(None);
    }
    if blenderui_tailwind::compile(name).is_some() {
        return Ok(Some(name.to_string()));
    }
    let looks_like_utility = name.contains(':') || name.contains('[') || name.contains('/')
        || !blenderui_tailwind::suggest(name).is_empty() && name.contains('-');
    if looks_like_utility {
        return Err(format!("rsx!: {}", blenderui_tailwind::unknown_message(name)));
    }
    Ok(None)
}

/// One attribute on an HTML element.
/// `{{ x: 100 }}`: a vars object (`vars!`, and true); anything else as it is.
fn vars_object(t: &TokenTree) -> R<(TokenTree, bool)> {
    if let TokenTree::Group(g) = t {
        let inner: Vec<TokenTree> = g.stream().into_iter().collect();
        if let (Delimiter::Brace, [TokenTree::Group(o)]) = (g.delimiter(), inner.as_slice()) {
            if o.delimiter() == Delimiter::Brace {
                return Ok((TokenTree::Group(Group::new(Delimiter::Brace, crate::vars::object(o.stream())?)), true));
            }
        }
    }
    Ok((t.clone(), false))
}

/// Motion's props that take a string (a variant's name; `drag="x"`, `layout="position"`).
const MOTION_TARGETS: &[&str] = &["initial", "animate", "exit", "whileHover", "whileTap", "whileFocus",
                                  "whileInView", "whileDrag", "drag", "layout"];

/// Motion's props that alone mean `true` (`<motion.div layout drag>`).
const MOTION_FLAGS: &[&str] = &["drag", "layout"];

fn attribute(name: &str, value: Option<TokenTree>) -> R<TokenStream> {
    if let (true, Some(TokenTree::Literal(l))) = (MOTION_TARGETS.contains(&name), &value) {
        let mut out = ts(&format!(".{}", method_name(name)));
        out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, TokenStream::from(TokenTree::Literal(l.clone()))))]);
        return Ok(out);
    }
    if value.is_none() && MOTION_FLAGS.contains(&name) {
        return Ok(ts(&format!(".{name}(true)")));
    }
    let ns = name.split(':').next().unwrap_or("");
    if name.contains(':') && !matches!(ns, "xlink" | "xml" | "xmlns") {
        return variant_attribute(name, value);
    }
    let lit = |s: &str| ts(&format!("{s:?}"));
    Ok(match value {
        // literals: the method for class / style / id, an attribute otherwise
        Some(TokenTree::Literal(l)) => {
            let m = match name {
                "class" | "className" => "class",
                "style" => "style",
                "id" | "key" => "id",
                _ => {
                    let mut out = ts(".attr");
                    let mut args = lit(name);
                    args.extend(ts(","));
                    args.extend([TokenTree::Literal(l)]);
                    out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, args))]);
                    return Ok(out);
                }
            };
            let mut out = ts(&format!(".{m}"));
            out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, TokenStream::from(TokenTree::Literal(l))))]);
            out
        }
        // expressions: the builder method of that name (snake_cased)
        Some(TokenTree::Group(g)) => {
            let method = method_name(name);
            if name.contains('-') || name.contains(':') || name == "for" || name == "htmlFor" {
                let name = if name == "htmlFor" { "for" } else { name };
                let mut args = lit(name);
                args.extend(ts(","));
                args.extend(g.stream());
                let mut out = ts(".attr");
                out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, args))]);
                return Ok(out);
            }
            let arg = if method.starts_with("on_") { listener(g.stream()) } else { g.stream() };
            let mut out = ts(&format!(".{method}"));
            out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, arg))]);
            out
        }
        None => {
            let mut args = lit(name);
            args.extend(ts(", \"\""));
            let mut out = ts(".attr");
            out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, args))]);
            out
        }
        Some(t) => return Err(format!("rsx!: bad value `{t}` for `{name}`")),
    })
}

/// `onClick` -> `on_click`, `type` -> `input_type`, `className` -> `class`.
fn method_name(name: &str) -> String {
    match name {
        "type" => return "input_type".into(),
        "className" => return "class".into(),
        "key" => return "id".into(),
        _ => {}
    }
    let mut s = String::new();
    for (k, c) in name.chars().enumerate() {
        if c.is_ascii_uppercase() {
            if k > 0 {
                s.push('_');
            }
            s.push(c.to_ascii_lowercase());
        } else {
            s.push(c);
        }
    }
    if s.starts_with("on") && !s.starts_with("on_") && s.len() > 2 {
        s.insert(2, '_');
    }
    s
}

/// A listener closure with fewer than the three `(event, window, cx)`
/// parameters gets `_` for the missing ones: `|| ..`, `|e| ..` work.
fn listener(stream: TokenStream) -> TokenStream {
    let toks: Vec<TokenTree> = stream.into_iter().collect();
    let start = usize::from(matches!(toks.first(), Some(TokenTree::Ident(i)) if i.to_string() == "move"));
    let pipe = |k: usize| matches!(toks.get(k), Some(TokenTree::Punct(p)) if p.as_char() == '|');
    if !pipe(start) {
        return toks.into_iter().collect(); // a path or other expression: as is
    }
    // `||` is one Punct pair (joint) with nothing between
    let (params, body_at): (Vec<TokenTree>, usize) = if matches!(toks.get(start), Some(TokenTree::Punct(p)) if p.spacing() == Spacing::Joint)
        && pipe(start + 1)
    {
        (Vec::new(), start + 2)
    } else {
        let close = (start + 1..toks.len()).find(|&k| pipe(k)).unwrap_or(toks.len());
        (toks[start + 1..close].to_vec(), close + 1)
    };
    let given = if params.is_empty() {
        0
    } else {
        params.iter().filter(|t| matches!(t, TokenTree::Punct(p) if p.as_char() == ',')).count() + 1
    };
    let mut out: TokenStream = toks[..start].iter().cloned().collect();
    out.extend(ts("|"));
    out.extend(params);
    for k in given..3 {
        out.extend(ts(if k == 0 { "_" } else { ", _" }));
    }
    out.extend(ts("|"));
    out.extend(toks[body_at.min(toks.len())..].iter().cloned());
    out
}

/// `{...}` children: control flow, spreads, or an expression.
fn braced(g: &Group) -> R<Child> {
    let toks: Vec<TokenTree> = g.stream().into_iter().collect();
    let kw = |k: usize, s: &str| matches!(toks.get(k), Some(TokenTree::Ident(i)) if i.to_string() == s);
    if is(toks.first(), '.') && is(toks.get(1), '.') && is(toks.get(2), '.') {
        return Ok(Child::Many(toks[3..].iter().cloned().collect()));
    }
    if kw(0, "for") {
        // for PAT in ITER { BODY }
        let in_at = (1..toks.len()).find(|&k| kw(k, "in")).ok_or("rsx!: `for` without `in`")?;
        let body = match toks.last() {
            Some(TokenTree::Group(b)) if b.delimiter() == Delimiter::Brace => b.clone(),
            _ => return Err("rsx!: `for` needs a { body }".into()),
        };
        let pat: TokenStream = toks[1..in_at].iter().cloned().collect();
        let iter: TokenStream = toks[in_at + 1..toks.len() - 1].iter().cloned().collect();
        let mut out = ts("::core::iter::IntoIterator::into_iter");
        out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, iter))]);
        let mut closure = ts("move |");
        closure.extend(pat);
        closure.extend(ts("|"));
        closure.extend([TokenTree::Group(Group::new(Delimiter::Brace, any(body_expr(&body)?)))]);
        out.extend(ts(".map"));
        out.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, closure))]);
        return Ok(Child::Many(out));
    }
    if kw(0, "if") {
        return Ok(Child::Many(if_chain(&toks)?));
    }
    if kw(0, "match") {
        let arms = match toks.last() {
            Some(TokenTree::Group(b)) if b.delimiter() == Delimiter::Brace => b.clone(),
            _ => return Err("rsx!: `match` needs { arms }".into()),
        };
        let mut out: TokenStream = toks[..toks.len() - 1].iter().cloned().collect();
        out.extend([TokenTree::Group(Group::new(Delimiter::Brace, match_arms(&arms)?))]);
        return Ok(Child::One(out));
    }
    Ok(Child::One(g.stream()))
}

/// A body: markup when it starts with `<` (or is text), Rust otherwise.
fn body_expr(b: &Group) -> R<TokenStream> {
    let toks: Vec<TokenTree> = b.stream().into_iter().collect();
    if is(toks.first(), '<') {
        markup(&toks)
    } else {
        Ok(b.stream())
    }
}

fn any(e: TokenStream) -> TokenStream {
    let mut c = ts("::blenderui::element::IntoElement::into_any_element");
    c.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, e))]);
    c
}

/// `if c { A } else if d { B } else { C }` -> the same chain of `Some(..)` / `None`.
fn if_chain(toks: &[TokenTree]) -> R<TokenStream> {
    let mut out = TokenStream::new();
    let mut k = 0;
    let mut has_else = false;
    while k < toks.len() {
        match &toks[k] {
            TokenTree::Group(b) if b.delimiter() == Delimiter::Brace
                && toks.get(k + 1).is_none_or(|t| matches!(t, TokenTree::Ident(i) if i.to_string() == "else")) => {
                let mut some = ts("::core::option::Option::Some");
                some.extend([TokenTree::Group(Group::new(Delimiter::Parenthesis, any(body_expr(b)?)))]);
                out.extend([TokenTree::Group(Group::new(Delimiter::Brace, some))]);
                if matches!(toks.get(k - 1), Some(TokenTree::Ident(i)) if i.to_string() == "else") {
                    has_else = true;
                }
            }
            t => out.extend([t.clone()]),
        }
        k += 1;
    }
    if !has_else {
        out.extend(ts("else { ::core::option::Option::None }"));
    }
    Ok(out)
}

/// `p => <a/>, q => { <b/> }` -> each arm's value as an AnyElement.
fn match_arms(arms: &Group) -> R<TokenStream> {
    let toks: Vec<TokenTree> = arms.stream().into_iter().collect();
    let mut out = TokenStream::new();
    let mut k = 0;
    while k < toks.len() {
        // pattern up to `=>`
        let start = k;
        while k + 1 < toks.len() && !(is(toks.get(k), '=') && is(toks.get(k + 1), '>')) {
            k += 1;
        }
        if k + 1 >= toks.len() {
            return Err("rsx!: match arm without `=>`".into());
        }
        out.extend(toks[start..k + 2].iter().cloned());
        k += 2;
        // value up to the top-level `,` (markup elements are matched by tag)
        let vstart = k;
        let value: TokenStream = if let Some(TokenTree::Group(b)) = toks.get(k).filter(|_| {
            matches!(toks.get(k), Some(TokenTree::Group(b)) if b.delimiter() == Delimiter::Brace)
        }) {
            k += 1;
            body_expr(b)?
        } else if is(toks.get(k), '<') {
            let mut p = P { t: &toks, i: k };
            let e = p.element()?;
            k = p.i;
            e
        } else {
            while k < toks.len() && !is(toks.get(k), ',') {
                k += 1;
            }
            toks[vstart..k].iter().cloned().collect()
        };
        out.extend(any(value));
        out.extend(ts(","));
        if is(toks.get(k), ',') {
            k += 1;
        }
    }
    Ok(out)
}
