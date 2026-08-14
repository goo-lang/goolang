#!/usr/bin/env python3
"""Show the source lines behind every finding for one MISRA rule.

A rule count is only worth quoting once you have seen what triggered it. This
prints the most common call/token shapes on the flagged lines, so a count that
is really one tool blind spot repeated N times shows up as such.
"""
import collections
import re
import sys

CALL_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def main():
    scan, rule, root = sys.argv[1], sys.argv[2], sys.argv[3]
    want = "misra-c2012-" + rule

    hits = collections.defaultdict(list)   # file -> [line numbers]
    for raw in open(scan, encoding="utf-8", errors="replace"):
        parts = raw.rstrip("\n").split("|", 4)
        if len(parts) == 5 and parts[2] == want:
            try:
                hits[parts[0]].append(int(parts[1]))
            except ValueError:
                pass

    calls = collections.Counter()
    samples, total, unreadable = [], 0, 0
    for path, lines in sorted(hits.items()):
        try:
            src = open("%s/%s" % (root, path), encoding="utf-8",
                       errors="replace").read().splitlines()
        except OSError:
            unreadable += len(lines)
            continue
        for n in lines:
            total += 1
            if not (1 <= n <= len(src)):
                continue
            text = src[n - 1].strip()
            if len(samples) < 8:
                samples.append("%s:%d: %s" % (path, n, text[:90]))
            for name in CALL_RE.findall(text):
                calls[name] += 1

    print("rule %s: %d findings in %d files (%d in unreadable files)"
          % (rule, total, len(hits), unreadable))
    print("\nmost common call names on the flagged lines:")
    for name, n in calls.most_common(12):
        print("  %-32s %6d" % (name, n))
    print("\nsample lines:")
    for s in samples:
        print("  " + s)


if __name__ == "__main__":
    main()
