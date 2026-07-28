// T0 probe: run the REAL local_escape pass over a real .goo file and print
// every local's verdict. Not a gate — a one-off measurement, so that T4's
// expected payoff comes from the pass itself rather than from reading it.
#include "parser.h"
#include "ast.h"
#include "types.h"
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
    if (checker) type_check_program(checker, ast_root);  // rc ignored, as the unit test does

    ParamEscapeResult* summaries = param_escape_analyze(ast_root);
    LocalEscapeResult* r = local_escape_analyze(ast_root, summaries);
    if (!r) { fprintf(stderr, "local_escape_analyze returned NULL\n"); return 1; }

    printf("param summaries: %s\n", summaries ? "present" : "NULL (all calls treated as retaining)");
    for (size_t i = 0; i < r->count; i++) {
        LocalEscapeSummary* s = &r->summaries[i];
        printf("\nfunc %s — %zu locals\n", s->function_name, s->local_count);
        for (size_t j = 0; j < s->local_count; j++) {
            printf("  %-10s %s\n", s->local_names[j],
                   s->escapes[j] ? "ESCAPES  (no release)" : "does not escape  <- RELEASE CANDIDATE");
        }
    }
    return 0;
}
