; Everything inside `{ ... }` that is not nested markup or a string is Rust.
((rust_text) @injection.content
  (#set! injection.language "rust"))

((comment) @injection.content
  (#set! injection.language "comment"))
