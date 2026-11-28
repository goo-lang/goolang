#!/usr/bin/env python3
"""Print the Goo reserved words that Go source may use as identifiers.

    python3 tools/goo_reserved.py [src/lexer/token.c]

One source of truth: the keyword table of the Goo lexer. A word counts when it
is in that table and is neither a Go keyword (a Go file cannot use it as an
identifier) nor a name that Go source uses as itself (true, false, nil and
unsafe). tools/vendor_go.py renames each printed word, and
scripts/vendor_drift_probe.sh accepts a rename row only for a printed word.
"""
import os
import re
import sys

GO_KEYWORDS = {
    "break", "case", "chan", "const", "continue", "default", "defer", "else",
    "fallthrough", "for", "func", "go", "goto", "if", "import", "interface",
    "map", "package", "range", "return", "select", "struct", "switch", "type",
    "var",
}
# Predeclared names and the unsafe package: Go source uses them as themselves,
# so a rename would change meaning. Stage 1 refuses an unsafe import anyway.
KEEP = {"true", "false", "nil", "unsafe"}


def reserved(token_c):
    text = open(token_c, encoding="utf-8").read()
    start = text.index("keywords[] = {")
    end = text.index("{NULL, TOKEN_UNKNOWN}", start)
    words = re.findall(r'^\s*\{"([A-Za-z_][A-Za-z0-9_]*)",\s*TOKEN_', text[start:end], re.M)
    if not words:
        raise SystemExit("goo_reserved: no keyword found in " + token_c)
    return sorted(set(words) - GO_KEYWORDS - KEEP)


if __name__ == "__main__":
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "src", "lexer", "token.c")
    print("\n".join(reserved(path)))
