// T4: print the release plan for a .goo file.
//
// The observable form of release_decision.c's verdict. Increment A shipped the
// decision with a unit table; this is how you ask it about a REAL file, which is
// what you want when codegen emits no release and you need to know whether the
// plan refused or the emission missed.
//
// Not a gate. Build line: see README.md in this directory.
#include "parser.h"
#include "ast.h"
#include "types.h"
#include "release_decision.h"
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

    ReleasePlan* plan = release_plan_analyze(ast_root);
    if (!plan) { fprintf(stderr, "release_plan_analyze returned NULL\n"); return 1; }

    int releasable = 0;
    for (size_t i = 0; i < plan->count; i++) {
        ReleasePlanFunction* pf = &plan->functions[i];
        printf("\nfunc %s — %zu locals\n", pf->function_name, pf->count);
        for (size_t j = 0; j < pf->count; j++) {
            ReleaseVerdict v = pf->decisions[j].verdict;
            if (v == RELEASE_OK) releasable++;
            printf("  %-12s %-22s %s\n",
                   pf->decisions[j].local_name,
                   release_verdict_name(v),
                   v == RELEASE_OK ? "<- RELEASE" : "");
        }
    }
    printf("\n%d releasable local(s)\n", releasable);

    release_plan_free(plan);
    return 0;
}
