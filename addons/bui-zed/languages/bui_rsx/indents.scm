(_
  "{"
  "}" @end) @indent

(open_tag
  ">" @end) @indent

(element
  (open_tag) @start
  (close_tag)? @end) @indent

(fragment
  "<>" @start
  "</>" @end) @indent
