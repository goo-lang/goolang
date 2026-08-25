# The grammar is the genrule's input; the generated sources live in the
# sibling package //src/parser/gen. They are NOT generated here on purpose:
# src/parser/parser.tab.c is gitignored but Make writes it to disk, so a
# genrule declaring that name as an output would collide with a real file --
# on developer machines only, never on a clean checkout.
exports_files(["parser.y"])
