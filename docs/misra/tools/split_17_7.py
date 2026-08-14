#!/usr/bin/env python3
"""Measure the Rule 17.7 split for deviation D-02.

D-02 waives discarding the result of a named list of functions and keeps the
rule in force for everything else. The policy is only honest once the split is
measured, so this reads the scan's file:line:column, takes the identifier at
that column, and reports how many findings fall on each side.
"""
import collections
import re
import sys

# The functions D-02 waives. Everything else stays a violation to fix.
WAIVED = {
    "printf", "fprintf", "vfprintf", "vprintf",
    "snprintf", "vsnprintf",
    "puts", "fputs", "fputc", "putchar", "fflush",
    "memcpy", "memmove", "memset", "strcpy", "strcat", "strncpy", "strncat",
}
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

# Bison output is reported separately under MISRA 6.8, so it must not be
# counted here either, or the split will not add up to the policy total.
GENERATED = ("src/parser/parser.tab.c", "src/parser/parser.tab.h",
             "src/parser/parser.y")


def main():
    scan, root = sys.argv[1], sys.argv[2]
    waived, kept, unresolved = collections.Counter(), collections.Counter(), 0
    kept_sites = collections.defaultdict(list)
    cache = {}

    for raw in open(scan, encoding="utf-8", errors="replace"):
        p = raw.rstrip("\n").split("|")
        if len(p) != 4 or p[3] != "misra-c2012-17.7":
            continue
        path, line, col = p[0], int(p[1]), int(p[2])
        if path in GENERATED:
            continue
        if path not in cache:
            try:
                cache[path] = open("%s/%s" % (root, path), encoding="utf-8",
                                   errors="replace").read().splitlines()
            except OSError:
                cache[path] = None
        src = cache[path]
        if not src or not (1 <= line <= len(src)):
            unresolved += 1
            continue
        text = src[line - 1]
        # cppcheck points at the call's opening token; take the identifier that
        # starts at or before that column.
        best = None
        for m in IDENT.finditer(text):
            if m.start() <= col - 1:
                best = m
            else:
                break
        if best is None:
            unresolved += 1
            continue
        fn = best.group()
        if fn in WAIVED:
            waived[fn] += 1
        else:
            kept[fn] += 1
            if len(kept_sites[fn]) < 2:
                kept_sites[fn].append("%s:%d" % (path, line))

    total = sum(waived.values()) + sum(kept.values()) + unresolved
    print("Rule 17.7 findings: %d" % total)
    print("  waived by D-02 (the listed functions) : %d" % sum(waived.values()))
    print("  KEPT - real violations to fix         : %d" % sum(kept.values()))
    print("  column did not resolve to an identifier: %d" % unresolved)
    print("\nwaived, by function:")
    for fn, n in waived.most_common(12):
        print("  %-24s %5d" % (fn, n))
    print("\nkept, by function (these are the Phase 1 work):")
    for fn, n in kept.most_common(20):
        print("  %-24s %5d   %s" % (fn, n, ", ".join(kept_sites[fn])))


if __name__ == "__main__":
    main()
