/**
 * tree-sitter grammar for BUI `rsx!` markup (the body of the macro, without
 * the `rsx! { }` wrapper). Unlike tree-sitter-rstml it accepts bare Tailwind
 * attributes: `p-4`, `bg-sky-500`, `w-1/2`, `-mt-2`, `bg-[#4772b3]`,
 * `hover:bg-sky-600`, `md:dark:hover:class="..."`, `2xl:flex`, `@md:flex`.
 *
 * Rust inside `{ ... }` is NOT parsed here; it is exposed as `rust_text`
 * chunks that injections.scm hands to the Rust grammar.
 */
module.exports = grammar({
  name: "bui_rsx",

  extras: ($) => [/\s/, $.comment],

  rules: {
    source_file: ($) => repeat($._node),

    _node: ($) =>
      choice($.element, $.self_closing_element, $.fragment, $.brace_expr, $.string, $.text),

    element: ($) => seq($.open_tag, repeat($._node), $.close_tag),

    open_tag: ($) =>
      seq("<", field("name", $.tag_name), repeat($._attribute_item), ">"),

    close_tag: ($) => seq("</", field("name", $.tag_name), ">"),

    self_closing_element: ($) =>
      seq("<", field("name", $.tag_name), repeat($._attribute_item), "/>"),

    fragment: ($) => seq("<>", repeat($._node), "</>"),

    _attribute_item: ($) => choice($.attribute, $.brace_expr),

    // `name`, `name="text"` or `name={expr}`
    attribute: ($) =>
      seq(
        field("name", $.attribute_name),
        optional(seq("=", field("value", choice($.string, $.brace_expr)))),
      ),

    tag_name: (_) => /[A-Za-z_][A-Za-z0-9_\-:.]*/,

    // Tailwind-friendly: digits, `/`, `[...]`, `!`, `@`, `(`, `)`, `%`, `#`, `*`, `:`.
    // A `/` is only part of the name when it is not followed by `>` (self-closing).
    attribute_name: (_) =>
      token(
        seq(
          /[A-Za-z_@*\-\[&0-9]/,
          repeat(
            choice(/[^\s=<>{}"\/\[]/, /\/[^>\s=<>{}"\/]/, /\[[^\]\s]*\]/),
          ),
        ),
      ),

    string: (_) => token(seq('"', repeat(choice(/[^"\\]/, /\\./)), '"')),

    // Bare words as children: `<p>all done</p>`
    text: (_) => token(prec(-1, /[^<>{}"\s\/][^<{}"]*/)),

    // `{ ... }` with balanced braces. Nested markup (`{for x in xs { <li/> }}`)
    // is parsed as real nodes, everything else is `rust_text`.
    brace_expr: ($) =>
      seq(
        "{",
        repeat(
          choice(
            $.element,
            $.self_closing_element,
            $.fragment,
            $.brace_expr,
            $.rust_string,
            $.rust_text,
          ),
        ),
        "}",
      ),

    rust_string: (_) => token(seq('"', repeat(choice(/[^"\\]/, /\\./)), '"')),

    // Anything but braces, quotes and a `<` that starts a tag. A `<` followed
    // by a space or `=` (comparison) is plain Rust.
    rust_text: (_) => token(choice(/[^{}"<]+/, prec(1, /<[ =]/))),

    comment: (_) =>
      token(
        choice(seq("//", /[^\n]*/), seq("/*", /[^*]*\*+([^/*][^*]*\*+)*/, "/")),
      ),
  },
});
