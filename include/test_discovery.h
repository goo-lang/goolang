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

// Build the source of the synthesized _testmain.goo for `package_name`:
//
//   package <package_name>
//
//   import "testing"
//
//   func main() {
//   	testing.Run("TestAdd", TestAdd)
//   	testing.Summary()
//   }
//
// The package clause names the package UNDER TEST, not "main" — the entry
// package's clause need not be "main", and matching it is what lets the
// generated file see the package's own identifiers.
//
// Each test is passed as a VALUE, not by name. That is the load-bearing choice
// of the whole design: the runtime owns the call frame, so it can setjmp before
// invoking a test and longjmp out of t.Fatal. A generated main that called each
// test directly could mark a failure but could not stop one.
//
// Returns a malloc'd buffer the caller owns, or NULL on allocation failure.
char* test_discovery_build_main(const char* package_name, const TestList* tests);

#endif // TEST_DISCOVERY_H
