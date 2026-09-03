// The shim signature table's three string-only predicates.
//
// This suite links NEITHER the parser NOR the type checker. It is the first
// one in the repository that manages that: every other suite here parses a
// Goo source string first. shim_signature_is_known_call,
// _is_non_retaining and _returns_owned_elems take two `const char*` and
// consult a static table, so a test needs the table and nothing else.
//
// shim_signature_lookup is deliberately NOT covered. It takes a TypeChecker*
// and builds Types, so testing it would drag the checker back in and this
// target would stop being what it is.
//
// WHY THIS TABLE DESERVES ITS OWN SUITE. release_decision.h states the rule
// it feeds: a call to a C shim is released "iff
// shim_signature_is_non_retaining". A wrong 1 there is a use-after-free and a
// wrong 0 is a leak. The column's own comment in shim_signatures.c calls a 1
// a TRUST assertion that must be justified against the runtime body. Nothing
// downstream can tell a wrong 1 from a right one.
//
// THE ROWS ARE REAL. Every expectation below was read off the table, not
// remembered: fmt.Println at shim_signatures.c:89, os.ReadFile at :125,
// strings.Contains at :172, strings.Split at :176.
//
// PROVEN ABLE TO FAIL, by mutating the table rather than the accessors:
//   strings.Split non_retaining 1 -> 0   reddens row 4 alone
//   strings.Split ret SLICE -> STRING    reddens row 8 alone, which is what
//                                        proves the accessor's return-kind
//                                        guard is live and observed here
//
// ONE MEASURED LIMIT, recorded so nobody rediscovers it. DELETING that guard
// outright is INVISIBLE to this suite: every row is green with the line
// removed. The guard exists to make a `returns_owned_elems` 1 inert on a row
// that returns no slice, and no row in the table spells that combination
// today, so there is nothing for its absence to change. Catching it would need
// a deliberately wrong row, which would then have to ship. If such a row ever
// appears for a real reason, add the case here.

#include "shim_signatures.h"
#include "../goo_check.h"
#include <stdio.h>

typedef enum { Q_KNOWN, Q_NON_RETAINING, Q_OWNED_ELEMS } Query;

typedef struct {
    int         row;
    const char* description;
    Query       query;
    const char* pkg;
    const char* name;
    int         expected;
} TestRow;

static const char* query_name(Query q) {
    switch (q) {
        case Q_KNOWN:         return "shim_signature_is_known_call";
        case Q_NON_RETAINING: return "shim_signature_is_non_retaining";
        case Q_OWNED_ELEMS:   return "shim_signature_returns_owned_elems";
    }
    return "?";
}

static int ask(Query q, const char* pkg, const char* name) {
    switch (q) {
        case Q_KNOWN:         return shim_signature_is_known_call(pkg, name);
        case Q_NON_RETAINING: return shim_signature_is_non_retaining(pkg, name);
        case Q_OWNED_ELEMS:   return shim_signature_returns_owned_elems(pkg, name);
    }
    return -1;
}

static TestRow rows[] = {
    // ---------------- membership ----------------
    { 1, "a real pair is a known call",
      Q_KNOWN, "strings", "Split", 1 },
    { 2, "a real package with an unknown member is not a known call",
      Q_KNOWN, "strings", "NoSuchFunction", 0 },
    { 3, "an unknown package is not a known call",
      Q_KNOWN, "nosuchpackage", "Split", 0 },

    // ---------------- non-retaining: the ARC release rule ----------------
    // A wrong 1 on any of these frees memory another owner still holds.
    { 4, "strings.Split is audited non-retaining",
      Q_NON_RETAINING, "strings", "Split", 1 },
    { 5, "strings.Contains is audited non-retaining",
      Q_NON_RETAINING, "strings", "Contains", 1 },
    { 6, "os.ReadFile is NOT marked non-retaining -- an unaudited row is 0",
      Q_NON_RETAINING, "os", "ReadFile", 0 },
    { 7, "an unknown pair gets the conservative 0, not a table miss",
      Q_NON_RETAINING, "nosuchpackage", "Whatever", 0 },

    // ---------------- owned elements ----------------
    // Separate from non-retaining, and neither implies the other. Rows 8 and 9
    // are the pair that proves they are separate: Contains is non-retaining
    // (row 5) and does not own elements.
    { 8, "strings.Split owns the elements of the slice it returns",
      Q_OWNED_ELEMS, "strings", "Split", 1 },
    { 9, "strings.Contains owns no elements -- it returns a bool",
      Q_OWNED_ELEMS, "strings", "Contains", 0 },
    { 10, "fmt.Println owns no elements -- it returns void",
      Q_OWNED_ELEMS, "fmt", "Println", 0 },
    { 11, "an unknown pair owns no elements",
      Q_OWNED_ELEMS, "nosuchpackage", "Whatever", 0 },
};

int main(void) {
    size_t nrows = sizeof(rows) / sizeof(rows[0]);
    goo_check_expect((int)nrows + 1);

    // Row 0 before the table: NULL must not crash and must answer
    // conservatively. Every accessor runs shim_signature_find first, so this
    // is one row rather than three, but all three are asked.
    goo_check_row(0, "a NULL package or name answers conservatively");
    goo_check(shim_signature_is_known_call(NULL, "Split") == 0,
              "is_known_call(NULL, \"Split\") is 0");
    goo_check(shim_signature_is_non_retaining("strings", NULL) == 0,
              "is_non_retaining(\"strings\", NULL) is 0");
    goo_check(shim_signature_returns_owned_elems(NULL, NULL) == 0,
              "returns_owned_elems(NULL, NULL) is 0");

    for (size_t r = 0; r < nrows; r++) {
        TestRow* row = &rows[r];
        goo_check_row(row->row, row->description);

        int got = ask(row->query, row->pkg, row->name);
        char ctx[320];
        snprintf(ctx, sizeof(ctx), "row %d: %s(\"%s\", \"%s\") = %d, expected %d",
                 row->row, query_name(row->query), row->pkg, row->name,
                 got, row->expected);
        goo_check(got == row->expected, ctx);
    }

    return goo_check_done("shim_signatures");
}
