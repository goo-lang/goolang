// T4 condition-2 probe: is there ANY signal that separates an owned local from
// a borrowed one?
//
// dump_local_escape.c answers "does this local outlive its function". This one
// answers the OTHER question T4 needs, which include/local_escape.h says the
// pass deliberately does not answer:
//
//     "This module answers 'does the value outlive F', NOT 'does the local own
//      the value'. A release consumer needs BOTH."
//
// ADR 0004 proposed origin-emptiness as the ownership test. Measured on
// ownership_shapes.goo, that does not work: local_escape's slots ARE local
// indices, a local keeps its own bit for life, so every local's taint is
// non-empty by construction and the test is constant-false. Worse, a local
// borrows from a PARAMETER, and parameters are not in that slot space at all.
//
// So this probe checks the level where the fact might survive:
// ParamEscapeSummary.return_escapes, documented as "does F return a value
// derived from one of its own params?". That is the borrowed-result relation,
// stated per callee instead of per local.
//
// Not a gate. Evidence for whether T4 can be built on the passes as they stand.
#include "parser.h"
#include "ast.h"
#include "types.h"
#include "param_escape.h"
#include "local_escape.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s FILE.goo\n", argv[0]); return 2; }
    char* src = slurp(argv[1]);

    if (parse_input(src, argv[1]) != 0 || !ast_root) {
        fprintf(stderr, "parse failed\n");
        return 1;
    }
    TypeChecker* checker = type_checker_new();
    if (checker) type_check_program(checker, ast_root);  // rc ignored, as the unit tests do

    ParamEscapeResult* pe = param_escape_analyze(ast_root);
    if (!pe) { fprintf(stderr, "param_escape_analyze returned NULL\n"); return 1; }

    printf("=== per-CALLEE summaries (param_escape) ===\n");
    printf("%-14s %-8s %-16s %s\n", "func", "params", "return_escapes", "reading");
    for (size_t i = 0; i < pe->count; i++) {
        const ParamEscapeSummary* s = &pe->summaries[i];
        printf("%-14s %-8zu %-16s %s\n",
               s->function_name, s->param_count,
               s->return_escapes ? "true" : "false",
               s->return_escapes
                   ? "result MAY alias an argument -> caller does NOT own it"
                   : "result derives from no argument -> caller owns it");
    }

    LocalEscapeResult* le = local_escape_analyze(ast_root, pe);
    if (le) {
        printf("\n=== per-LOCAL verdicts (local_escape), for contrast ===\n");
        for (size_t i = 0; i < le->count; i++) {
            LocalEscapeSummary* s = &le->summaries[i];
            if (s->local_count == 0) continue;
            printf("func %s\n", s->function_name);
            for (size_t j = 0; j < s->local_count; j++) {
                printf("  %-10s %s\n", s->local_names[j],
                       s->escapes[j] ? "escapes" : "does not escape");
            }
        }
        local_escape_result_free(le);
    }

    param_escape_result_free(pe);
    return 0;
}
