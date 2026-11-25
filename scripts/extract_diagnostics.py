#!/usr/bin/env python3
"""Regenerate catalogue/diagnostics.tsv from the checker's type_error call
sites (src/types/*.c). type_error_union is a TYPE CONSTRUCTOR, not an
emitter, and is deliberately not matched. The format string is the third
argument (types.h:1338: type_error(checker, pos, format, ...)). Adjacent
string literals are joined the way the C compiler joins them.

Columns: id  file  format
The id is stable across edits: sha1 of (file basename, format) truncated to
8 hex chars, so a moved call keeps its id and a reworded one gets a new one.
Sorted by file then format so the file is diff-friendly."""
import glob, hashlib, re, sys

CALL = re.compile(r'\btype_error\s*\(', re.M)
STR = re.compile(r'"((?:[^"\\]|\\.)*)"')

def formats_in(src):
    """Every string literal group that can be the format argument of a
    type_error call. The third argument is usually one literal (or several
    adjacent ones, joined the way C joins them); two sites pass a ternary
    (`cond ? "a" : "b"`), so each literal GROUP inside the argument is its own
    row rather than the argument as a whole."""
    out = []
    for m in CALL.finditer(src):
        i = m.end()
        # skip two arguments (checker, pos) by counting top-level commas
        depth, args = 0, 0
        while i < len(src) and args < 2:
            c = src[i]
            if c in '([': depth += 1
            elif c in ')]': depth -= 1
            elif c == ',' and depth == 0: args += 1
            i += 1
        # the third argument: up to the next top-level ',' or the closing ')'
        j, depth = i, 0
        while j < len(src):
            c = src[j]
            if c == '"':
                e = STR.match(src, j); j = e.end() if e else j + 1; continue
            if c in '([': depth += 1
            elif c in ')]':
                if depth == 0: break
                depth -= 1
            elif c == ',' and depth == 0: break
            j += 1
        arg = src[i:j]
        # group literals separated only by whitespace (C concatenation)
        groups, k = [], 0
        while True:
            s1 = STR.search(arg, k)
            if not s1: break
            parts = [s1.group(1)]; k = s1.end()
            while True:
                ws = re.match(r'\s*', arg[k:]); s2 = STR.match(arg, k + ws.end())
                if not s2: break
                parts.append(s2.group(1)); k = s2.end()
            groups.append(''.join(parts))
        out.extend(groups)
    return out

def main():
    rows = []
    for path in sorted(glob.glob('src/types/*.c')):
        base = path.split('/')[-1]
        for fmt in formats_in(open(path).read()):
            ident = hashlib.sha1(f'{base}\0{fmt}'.encode()).hexdigest()[:8]
            rows.append((ident, base, fmt))
    rows = sorted(set(rows), key=lambda r: (r[1], r[2]))
    sys.stdout.write('id\tfile\tformat\n')
    for r in rows: sys.stdout.write('\t'.join(r) + '\n')

if __name__ == '__main__': main()
