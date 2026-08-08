// libFuzzer harness for the Goo parser.
//
// WHY THIS EXISTS. The parser is the one component that reads bytes somebody
// else wrote. `make verify-core` runs 197 hand-written gates over 810 fixtures,
// and every one of those fixtures was written by a person who was trying to
// express something. None of them is trying to break the parser. The coverage
// measurement on 2026-08-08 put the front end at 58.1% branch / 56.5% MC/DC,
// so a little under half of it has never run at all.
//
// WHY THIS IS CHEAP. The seam already existed and nothing had to be refactored
// to reach it. `parse_input(const char*, const char*)` is declared in
// include/parser.h, owns its lexer, resets the M10 disambiguation state on
// entry, and writes the global `ast_root` — src/compiler/goo.c:549 documents
// exactly this and relies on it to parse many files in one process. That is
// already the property a fuzzer needs, so this file is a caller and not a
// rewrite.
//
// WHAT COUNTS AS A FINDING. See tests/fuzz/README.md. In short: a crash counts
// when it reproduces on the shipped gcc `bin/goo`. A finding that appears only
// under the clang sanitizer build is a separate, lower-priority item.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "parser.h"

extern ASTNode* ast_root;

// Above this the run measures the allocator and the O(n) passes rather than
// the parser's decisions, and libFuzzer's own -max_len already defaults lower.
// Chosen so the largest fixture in examples/ (a few KB) is far inside it.
#define FUZZ_MAX_INPUT (64 * 1024)

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > FUZZ_MAX_INPUT) return 0;

    // parse_input takes a NUL-terminated string, and fuzz input may contain
    // embedded NULs. Copying rather than casting is also what lets ASan see a
    // read past the end: the copy is exactly size+1 bytes in its own
    // allocation, where the original buffer may be padded.
    char* buf = (char*)malloc(size + 1);
    if (!buf) return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    parse_input(buf, "fuzz_input.goo");

    // Free the AST even on a parse error. `ast_root` is a global that
    // parse_input writes, so without this every iteration leaks a whole tree
    // and the process reaches its RSS limit in minutes — which libFuzzer would
    // report as an OOM and blame on the last input, hiding whatever real
    // finding came next. Detaching (rather than only freeing) matches what
    // goo.c does, so a later parse cannot see a stale tree.
    if (ast_root) {
        ast_node_free(ast_root);
        ast_root = NULL;
    }

    free(buf);
    return 0;
}
