#!/usr/bin/env python3
"""Count unchecked pthread_* return values under src/.

This is the counting engine for scripts/pthread_return_probe.sh. It is a
standalone tool (not itself the gate) so the parenthesis-matching logic --
awkward in awk or sed -- lives in one place, in a language built for it.

COUNTING RULE (read this before changing anything below):
  FILES:  every *.c under src/, except *.tab.c (bison output) and lex.yy.c
          (flex output). Same file scope as scripts/assertion_density_probe.sh
          -- 68 files today.
  Comments (both // and /* */) are removed first. String and character
  literals are also blanked out (their content replaced with "x", quotes
  kept), so a paren or semicolon quoted inside a string can never be
  mistaken for real source punctuation.
  A PREPROCESSOR DIRECTIVE LINE (first non-blank character on the line is
  "#") is blanked the same way a comment is. This is a deliberate widening
  of the literal token rule below: this codebase gates almost every real
  pthread_* call in an "#ifdef GOO_PLATFORM_UNIX" block (always true on the
  Linux build this repo targets -- see include/runtime.h), so a call
  immediately after such a line is, in every case that matters here, the
  first real statement of that block, not a continuation of the directive.
  Without this widening the counter would see the directive's own last
  token (an identifier from the macro name) as the "token before", exclude
  almost every call in the repository, and measure close to nothing.
  A COUNTED CALL is a call to an identifier that begins with "pthread_",
  whose value is discarded. Its value is discarded when the call is a
  WHOLE EXPRESSION STATEMENT:
    - the token immediately before the identifier is ";", "{", "}", or a
      label colon (":"), or the identifier starts the file, AND
    - the matching closing parenthesis of the call is followed by ";"
      (skipping whitespace, including newlines -- so a call whose argument
      list spans more than one line is counted exactly like a one-line
      call: the parentheses are matched across lines).
  A call inside "if (", "while (", "return", an assignment, a comparison,
  another call's argument list, or right after a "(void)" cast is NOT
  counted -- in every one of those shapes the token before the identifier
  is something other than ";", "{", "}" or ":" (an operator, a keyword
  letter, or an opening "(" from the outer call/cast), so the same single
  rule above excludes all of them with no extra special-casing required.
  MISRA Rule 17.7 treats the "(void)" cast as compliant, and this rule
  agrees for the same reason.

Usage:
  pthread_return_count.py <root>
    <root> is a directory containing an src/ tree (a repo checkout, or a
    scratch copy for the probe's --self-test). Prints one line per file
    with a count above zero, "<path relative to root><TAB><count>", sorted
    by path, followed by "TOTAL<TAB><n>".
"""
import os
import re
import sys

IDENT_RE = re.compile(r"\bpthread_[A-Za-z0-9_]*\b")
PREV_OK = (";", "{", "}", ":")


def list_files(root):
    src = os.path.join(root, "src")
    out = []
    for dirpath, _dirnames, filenames in os.walk(src):
        for name in filenames:
            if not name.endswith(".c"):
                continue
            if name.endswith(".tab.c") or name == "lex.yy.c":
                continue
            out.append(os.path.join(dirpath, name))
    out.sort()
    return out


def strip_noise(text):
    """Blank out comments, string/char literal content, and preprocessor
    directive lines. The result is not required to be the same length as
    the input -- only the surviving punctuation and identifiers matter --
    but newlines are preserved so "first non-blank character on the line"
    stays correct for any directive line the scan has not yet reached.
    """
    out = []
    i = 0
    n = len(text)
    at_line_start = True
    while i < n:
        c = text[i]

        # Preprocessor directive: blank the rest of the line (and any
        # backslash-continued following lines), keep the newlines.
        if at_line_start and c == "#":
            j = i
            while j < n:
                if text[j] == "\n":
                    if j > i and text[j - 1] == "\\":
                        j += 1
                        continue
                    break
                j += 1
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
            at_line_start = True
            continue

        if c == "\n":
            out.append(c)
            i += 1
            at_line_start = True
            continue
        if c in (" ", "\t", "\r"):
            out.append(c)
            i += 1
            continue
        at_line_start = False

        # Line comment.
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = i + 2
            while j < n and text[j] != "\n":
                j += 1
            out.append(" " * (j - i))
            i = j
            continue

        # Block comment.
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = i + 2
            while j + 1 < n and not (text[j] == "*" and text[j + 1] == "/"):
                j += 1
            j = min(j + 2, n)
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
            continue

        # String literal.
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" and j + 1 < n else 1
            j = min(j + 1, n)
            inner = max(0, j - i - 2)
            out.append('"' + "x" * inner + '"' if j - i >= 2 else text[i:j])
            i = j
            continue

        # Char literal.
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" and j + 1 < n else 1
            j = min(j + 1, n)
            inner = max(0, j - i - 2)
            out.append("'" + "x" * inner + "'" if j - i >= 2 else text[i:j])
            i = j
            continue

        out.append(c)
        i += 1
    return "".join(out)


def prev_significant_char(text, idx):
    """The character immediately before index idx, skipping whitespace.
    None means the identifier starts the file.
    """
    i = idx - 1
    while i >= 0 and text[i].isspace():
        i -= 1
    return None if i < 0 else text[i]


def matching_close_and_next_char(text, open_idx):
    """Given the index of a call's opening '(', return
    (close_idx, next_significant_char) for its matching ')' -- matched
    across as many lines as needed. (None, None) if the parens never
    balance (should not happen in valid C).
    """
    depth = 0
    n = len(text)
    i = open_idx
    while i < n:
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                j = i + 1
                while j < n and text[j].isspace():
                    j += 1
                return i, (text[j] if j < n else None)
        i += 1
    return None, None


def count_text(text):
    cleaned = strip_noise(text)
    count = 0
    for m in IDENT_RE.finditer(cleaned):
        start, end = m.start(), m.end()
        j = end
        while j < len(cleaned) and cleaned[j].isspace():
            j += 1
        if j >= len(cleaned) or cleaned[j] != "(":
            continue  # not a call (a type name, a macro, a declaration, ...)
        open_idx = j
        prev = prev_significant_char(cleaned, start)
        if prev is not None and prev not in PREV_OK:
            continue  # value used or discarded on purpose (see header)
        close_idx, next_char = matching_close_and_next_char(cleaned, open_idx)
        if close_idx is None or next_char != ";":
            continue
        count += 1
    return count


def main():
    if len(sys.argv) != 2:
        print("usage: pthread_return_count.py <root>", file=sys.stderr)
        return 2
    root = sys.argv[1]
    total = 0
    rows = []
    for path in list_files(root):
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        n = count_text(text)
        if n > 0:
            rows.append((os.path.relpath(path, root), n))
            total += n
    for rel, n in rows:
        print(f"{rel}\t{n}")
    print(f"TOTAL\t{total}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
