; Tags: lowercase = HTML element or lowercase #[component]; Capitalized = component
(tag_name) @tag

((tag_name) @type
  (#match? @type "^[A-Z]"))

(open_tag
  [
    "<"
    ">"
  ] @punctuation.bracket)

(close_tag
  [
    "</"
    ">"
  ] @punctuation.bracket)

(self_closing_element
  [
    "<"
    "/>"
  ] @punctuation.bracket)

(fragment
  [
    "<>"
    "</>"
  ] @punctuation.bracket)

; Attributes with a value: name="text", name={expr}, hover:class="..."
(attribute
  name: (attribute_name) @attribute
  "=" @punctuation.delimiter)

; Bare attributes are Tailwind utilities (p-4, hover:bg-sky-500, bg-[#4772b3])
; or HTML boolean attributes (disabled, checked, open, hidden)
(attribute
  name: (attribute_name) @string.special
  !value
  (#not-match? @string.special "^(disabled|checked|hidden|open|readonly|required|selected|autofocus|multiple)$"))

((attribute
  name: (attribute_name) @attribute)
  (#match? @attribute "^(disabled|checked|hidden|open|readonly|required|selected|autofocus|multiple)$"))

; Variant prefixed class/style: hover:class="...", md:dark:style="..."
((attribute
  name: (attribute_name) @keyword
  value: (_))
  (#match? @keyword ":(class|style|outline)$"))

; Component keys / special attributes
((attribute
  name: (attribute_name) @property
  value: (_))
  (#match? @property "^(key|id|class|className|style|type|for)$"))

; class="..." strings hold Tailwind utilities
(attribute
  name: (attribute_name) @_name
  value: (string) @string.special
  (#match? @_name "(^|:)(class|className)$"))

(string) @string

(text) @string

(rust_string) @string

[
  "{"
  "}"
] @punctuation.special

(comment) @comment
