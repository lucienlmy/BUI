("{" @open
  "}" @close)

("<" @open
  "/>" @close)

("<" @open
  ">" @close)

("</" @open
  ">" @close)

((element
  (open_tag) @open
  (close_tag) @close)
  (#set! newline.only))
