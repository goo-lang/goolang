#!/usr/bin/env python3
"""Build a cppcheck misra.py --rule-texts file from Appendix A of a MISRA C:2012 PDF.

Input is `pdftotext -layout` output, where Appendix A keeps its table shape:

    Rule 17.1   Required    The features of <stdarg.h> shall not be used
    Rule 17.5   Advisory    The function argument corresponding to a parameter declared to have
                            an array type shall have an appropriate number of elements

The left margin of the table moves from page to page, and a page number can sit
on the same line as an entry, so nothing here may depend on absolute columns.

The result is derived from the user's own copy of the standard and is for local
tool use only. MISRA text is copyrighted: do not commit or redistribute it.
"""
import re
import sys
import unicodedata

START = "Appendix A Summary of guidelines"
END = "Appendix B Guideline attributes"

# Section headings from the table of contents (7.1-7.4, 8.1-8.22). They sit
# between table rows and must not be glued onto the preceding rule text.
HEADINGS = {
    "The implementation", "Compilation and build", "Requirements traceability",
    "Code design", "A standard C environment", "Unused code", "Comments",
    "Character sets and lexical conventions", "Identifiers", "Types",
    "Literals and constants", "Declarations and definitions", "Initialization",
    "The essential type model", "Pointer type conversions", "Expressions",
    "Side effects", "Control statement expressions", "Control flow",
    "Switch statements", "Functions", "Pointers and arrays",
    "Overlapping storage", "Preprocessing directives", "Standard libraries",
    "Resources",
}

# Rules the cppcheck addon checks that this edition of the standard does not
# define. They arrived in a later amendment.
LATER_AMENDMENT = {
    "1.4": "Not present in MISRA C:2012 Third edition first revision - added by "
           "a later amendment for C11/C18. Text unavailable from this PDF.",
    "21.21": "Not present in MISRA C:2012 Third edition first revision - added "
             "by a later amendment. Text unavailable from this PDF.",
}

ENTRY_RE = re.compile(
    r"^\s*(?:\d{1,3}\s+)?"                        # optional page number
    r"(?P<id>(?:Rule|Dir) \d+\.\d+)\s+"
    r"(?P<sev>Required|Advisory|Mandatory)\s+"
    r"(?P<text>\S.*)$")
# A running head or a bare page number splits a table row without ending it.
NOISE_RE = re.compile(r"^\s*(Appendix A[: ].*|\d{1,3})\s*$")
LEADING_PAGENUM_RE = re.compile(r"^\s*\d{1,3}\s{3,}")


def ascii_fold(s):
    """Replace the typographic quotes and dashes so the output stays pure ASCII."""
    table = {0x2018: "'", 0x2019: "'", 0x201c: '"', 0x201d: '"',
             0x2013: "-", 0x2014: "-", 0x2026: "...", 0xa0: " "}
    return unicodedata.normalize(
        "NFKD", s.translate(table)).encode("ascii", "ignore").decode()


def parse(lines):
    records, cur = [], None
    for raw in lines:
        line = ascii_fold(raw.lstrip("\f").rstrip())
        if not line.strip() or NOISE_RE.match(line):
            continue                      # keeps `cur` open across a page break
        m = ENTRY_RE.match(line)
        if m:
            cur = {"id": m["id"], "sev": m["sev"], "text": [m["text"].strip()]}
            records.append(cur)
            continue
        body = LEADING_PAGENUM_RE.sub("", line).strip()
        if body in HEADINGS:
            cur = None
            continue
        if cur is not None:
            cur["text"].append(body)
    return records


def check(records):
    """Fail loudly rather than emit a table with a silently mangled entry."""
    ids = [r["id"] for r in records]
    problems = []
    dupes = sorted({i for i in ids if ids.count(i) > 1})
    if dupes:
        problems.append("duplicate ids: " + ", ".join(dupes))
    for r in records:
        text = " ".join(r["text"])
        if not text:
            problems.append("%s has no text" % r["id"])
        elif "shall" not in text and "should" not in text:
            # Every MISRA guideline is a normative "shall"/"should" statement.
            problems.append("%s text looks truncated: %r" % (r["id"], text))
        for h in HEADINGS:
            if text.endswith(h):
                problems.append("%s absorbed the heading %r" % (r["id"], h))
    if problems:
        sys.exit("rule-text extraction failed:\n  " + "\n  ".join(problems))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, encoding="utf-8") as f:
        lines = f.readlines()

    # The appendix headings carry a form feed and page-dependent indentation,
    # so compare on whitespace-collapsed text.
    heads = [" ".join(l.split()) for l in lines]
    start = next(i for i, l in enumerate(heads) if l == START)
    end = next(i for i, l in enumerate(heads) if i > start and l == END)

    records = parse(lines[start + 1:end])
    check(records)

    with open(dst, "w", encoding="ascii") as out:
        out.write("Appendix A Summary of guidelines\n\n")
        for r in records:
            out.write("%s %s\n%s\n\n" % (r["id"], r["sev"], " ".join(r["text"])))
        # cppcheck's addon knows two rules this edition does not define. Without
        # an entry it reports them as missing rule texts, which hides a real
        # omission behind two expected ones. The text must start with a capital
        # letter: the addon's parser rejects a line that does not.
        for num in LATER_AMENDMENT:
            out.write("Rule %s Required\n%s\n\n" % (num, LATER_AMENDMENT[num]))

    rules = sum(1 for r in records if r["id"].startswith("Rule"))
    print("wrote %d guidelines (%d rules, %d directives) to %s"
          % (len(records), rules, len(records) - rules, dst))
    print("plus %d placeholders for rules added after this edition: %s"
          % (len(LATER_AMENDMENT), ", ".join(sorted(LATER_AMENDMENT))))


if __name__ == "__main__":
    main()
