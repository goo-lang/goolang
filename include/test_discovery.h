#ifndef TEST_DISCOVERY_H
#define TEST_DISCOVERY_H

#include <stddef.h>
#include "ast.h"

// `goo test` test discovery: find the `func TestXxx(t *testing.T)` functions a
// package declares, and reject a Test-named function that has the wrong shape.
//
// This module deliberately depends on ast.h ALONE — no TypeChecker, no codegen.
// Discovery runs before type_checker_new(), because the synthesized
// _testmain.goo it feeds must be parsed and appended to the package's file list
// before the stdlib marker-seeding loop walks that list. Taking a dependency on
// the checker here would force discovery after that point and break the
// ordering synthesis needs.

typedef struct {
    char** names;    // strdup'd test function names, in declaration order
    size_t count;
} TestList;

// Collect the tests declared by `programs` (each an AST_PROGRAM root, one per
// file). `file_names[i]` names `programs[i]` and is used only for diagnostics.
//
// Callers must pass ONLY the package's *_test.go / *_test.goo files. Go scans
// no others, and scanning an ordinary file would reject a `func TestHelper(x
// int)` that `goo build` accepts — the same source would then compile or fail
// depending on the subcommand.
//
// Returns 1 on success, with `out` populated (possibly with zero tests).
// Returns 0 if a Test-named function has the wrong signature, with the
// diagnostic already written to stderr and `out` left empty.
int test_discovery_collect(ASTNode** programs, size_t program_count,
                           const char** file_names, TestList* out);

void test_list_free(TestList* l);

#endif // TEST_DISCOVERY_H
