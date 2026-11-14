//! `css!`: CSS declaration blocks checked and resolved at compile time.
//!
//! ```ignore
//! div().style(css!("display: flex; gap: 4px; border-left: 3px solid #4772b3"))
//! ```
//!
//! The block is split into declarations when the program compiles. Every
//! property becomes `const { Prop::named("...") }`, so an unknown property is
//! a compile error, and each value is tokenized once for the whole program.
//! The expansion is an array of `Decl`s, so nothing is parsed per frame.

use proc_macro::{TokenStream, TokenTree};

mod component;
mod rsx;
mod vars;

/// A function component (zopra / Leptos style): `#[component] fn card(title: &str) { rsx!{..} }`,
/// used as `<card title="x"/>`. See `component.rs`.
#[proc_macro_attribute]
pub fn component(_attr: TokenStream, item: TokenStream) -> TokenStream {
    component::component(item)
}

/// JSX-style markup: `rsx! { <div class="p-4" onClick={|| ..}>"hi" {name}</div> }`.
/// See `rsx.rs` for the (uniform) lowering to builder calls.
/// `#[derive(Props)]`: a props builder for a struct component, so `rsx!`
/// can leave out its `Option<T>` fields and `children`.
#[proc_macro_derive(Props)]
pub fn derive_props(item: TokenStream) -> TokenStream {
    component::derive_props(item)
}

#[proc_macro]
pub fn rsx(input: TokenStream) -> TokenStream {
    rsx::rsx(input)
}

/// A GSAP vars / Motion target object: `vars!{ x: 100, opacity: 0.5, duration: 1,
/// ease: "power2.out" }`, sorted out when the program compiles (rsx's `{{ .. }}`
/// is the same). See `vars.rs`.
#[proc_macro]
pub fn vars(input: TokenStream) -> TokenStream {
    vars::vars(input)
}

#[proc_macro]
pub fn css(input: TokenStream) -> TokenStream {
    match expand(input) {
        Ok(code) => code.parse().expect("css!: generated code"),
        Err(msg) => format!("::core::compile_error!({msg:?})").parse().unwrap(),
    }
}

/// `tw!("p-4 md:flex hover:bg-sky-500/50")`: Tailwind classes, checked when
/// the program compiles (a misspelled or unknown utility is a compile
/// error), expanding to the class string for `.class(...)`.
///
#[proc_macro]
pub fn tw(input: TokenStream) -> TokenStream {
    let mut tokens = input.into_iter();
    let lit = match (tokens.next(), tokens.next()) {
        (Some(TokenTree::Literal(l)), None) => l.to_string(),
        _ => return "::core::compile_error!(\"tw! takes one string literal\")".parse().unwrap(),
    };
    let Some(text) = unquote(&lit) else {
        return "::core::compile_error!(\"tw! takes one string literal\")".parse().unwrap();
    };
    let unknown: Vec<String> = text.split_whitespace().filter(|c| blenderui_tailwind::compile(c).is_none())
        .map(blenderui_tailwind::unknown_message).collect();
    if !unknown.is_empty() {
        let msg = format!("tw!: {}", unknown.join("; "));
        return format!("::core::compile_error!({msg:?})").parse().unwrap();
    }
    format!("{text:?}").parse().unwrap()
}

fn expand(input: TokenStream) -> Result<String, String> {
    let mut tokens = input.into_iter();
    let lit = match (tokens.next(), tokens.next()) {
        (Some(TokenTree::Literal(l)), None) => l.to_string(),
        _ => return Err("css! takes one string literal".into()),
    };
    let text = unquote(&lit).ok_or("css! takes one string literal")?;
    let mut decls = Vec::new();
    for part in split_top(&strip_comments(&text), ';') {
        let part = part.trim();
        if part.is_empty() {
            continue;
        }
        let (name, value) = part.split_once(':').ok_or_else(|| format!("css!: expected `property: value` in `{part}`"))?;
        let name = name.trim().to_ascii_lowercase();
        if name.is_empty() {
            return Err(format!("css!: missing property name in `{part}`"));
        }
        let mut value = value.trim();
        let mut important = false;
        if let Some(v) = value.strip_suffix("!important") {
            value = v.trim_end();
            important = true;
        }
        if value.is_empty() {
            return Err(format!("css!: missing value for `{name}`"));
        }
        // custom properties are interned at run time (once per call site)
        let prop = if name.starts_with("--") {
            format!("{{ static P: ::std::sync::OnceLock<::blenderui::css::Prop> = \
                     ::std::sync::OnceLock::new(); \
                     *P.get_or_init(|| ::blenderui::css::Prop::custom({name:?})) }}")
        } else {
            format!("const {{ ::blenderui::css::Prop::named({name:?}) }}")
        };
        decls.push(format!(
            "::blenderui::css::Decl {{ \
                prop: {prop}, \
                value: {{ \
                    static V: ::std::sync::OnceLock<::std::boxed::Box<[::blenderui::css::Tok]>> = \
                        ::std::sync::OnceLock::new(); \
                    ::blenderui::css::Value::Static(V.get_or_init(|| \
                        ::blenderui::css::tokenize({value:?}).into_boxed_slice())) \
                }}, \
                important: {important} \
            }}"
        ));
    }
    Ok(format!("[{}]", decls.join(", ")))
}

/// The contents of a (raw) string literal, escapes resolved.
fn unquote(lit: &str) -> Option<String> {
    if let Some(rest) = lit.strip_prefix('r') {
        let hashes = rest.len() - rest.trim_start_matches('#').len();
        let inner = &rest[hashes..];
        let inner = inner.strip_prefix('"')?;
        let inner = inner.strip_suffix(&"#".repeat(hashes))?.strip_suffix('"')?;
        return Some(inner.to_owned());
    }
    let inner = lit.strip_prefix('"')?.strip_suffix('"')?;
    let mut out = String::with_capacity(inner.len());
    let mut chars = inner.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        match chars.next()? {
            'n' => out.push('\n'),
            't' => out.push('\t'),
            'r' => out.push('\r'),
            '0' => out.push('\0'),
            '\n' => {
                // line continuation: skip leading whitespace on the next line
                let rest: String = chars.by_ref().skip_while(|c| c.is_whitespace()).collect();
                out.push_str(&unquote(&format!("\"{rest}\""))?);
                return Some(out);
            }
            other => out.push(other),
        }
    }
    Some(out)
}

fn strip_comments(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while let Some(start) = rest.find("/*") {
        out.push_str(&rest[..start]);
        rest = rest[start + 2..].find("*/").map(|e| &rest[start + 2 + e + 2..]).unwrap_or("");
    }
    out.push_str(rest);
    out
}

/// Split at `sep` outside parentheses and quotes.
fn split_top(s: &str, sep: char) -> Vec<&str> {
    let mut parts = Vec::new();
    let (mut depth, mut quote, mut start) = (0i32, None::<char>, 0);
    for (k, c) in s.char_indices() {
        match (quote, c) {
            (Some(q), _) if c == q => quote = None,
            (Some(_), _) => {}
            (None, '"' | '\'') => quote = Some(c),
            (None, '(' | '[') => depth += 1,
            (None, ')' | ']') => depth -= 1,
            (None, _) if c == sep && depth == 0 => {
                parts.push(&s[start..k]);
                start = k + c.len_utf8();
            }
            _ => {}
        }
    }
    parts.push(&s[start..]);
    parts
}
