// `goo test` test discovery. See include/test_discovery.h for why this module
// depends on ast.h alone.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_discovery.h"
#include "import_resolver.h"   // normalize_import_path

// Go's rule exactly (cmd/go/internal/load/test.go, isTest): the name begins
// with "Test" and the character after "Test" is not a lowercase letter. So
// TestAdd and Test_add are tests, bare Test is a test, and Testify is an
// ordinary function left alone.
//
// The name rule is what keeps the signature error honest. Without the
// lowercase exclusion an ordinary helper called Testable would be pulled in and
// then rejected for the wrong reason.
static int is_test_name(const char* n) {
    if (!n || strncmp(n, "Test", 4) != 0) return 0;
    char c = n[4];
    if (c == '\0') return 1;
    return !(c >= 'a' && c <= 'z');
}

// The name this file uses for the `testing` package: its alias when it wrote
// one, otherwise the import path. Derived per FILE, because imports are
// file-scoped (PR #226) — one file of a package may write `import tst
// "testing"` while its sibling writes the bare form.
//
// Returns NULL when the file does not import testing at all. That is NOT a
// reason to skip the file: see the call site.
static const char* testing_local_name(const ProgramNode* prog) {
    for (const ASTNode* imp = prog->imports; imp; imp = imp->next) {
        if (imp->type != AST_IMPORT_SPEC) continue;
        const ImportSpecNode* spec = (const ImportSpecNode*)imp;
        if (!spec->path) continue;
        const char* norm = normalize_import_path(spec->path);
        if (!norm || strcmp(norm, "testing") != 0) continue;
        return spec->alias ? spec->alias : spec->path;
    }
    return NULL;
}

// `func TestXxx(t *testing.T)` and nothing else: no results, no receiver, no
// type parameters, exactly one non-variadic parameter of type *testing.T.
//
// `testing_local` is NULL when the file imports no testing package, which makes
// the parameter check fail and the function get rejected. That is deliberate —
// a `func TestFoo()` in a _test file is a malformed test whichever way it is
// read, and Go reports it the same way.
static int has_test_signature(const FuncDeclNode* f, const char* testing_local) {
    if (f->return_type) return 0;
    if (f->receiver) return 0;
    // A generic test cannot be handed to testing.Run as a value: there is no
    // instantiation for the runner to name.
    if (f->type_params) return 0;

    // One VarDeclNode per parameter — the grammar has no grouped `a, b T` form
    // for parameters (parser.y's func_param, parser_actions.c's
    // func_param_new), so counting nodes counts arity.
    if (!f->params || f->params->next) return 0;
    if (f->params->type != AST_VAR_DECL) return 0;

    const VarDeclNode* p = (const VarDeclNode*)f->params;
    if (p->is_variadic_param) return 0;
    if (!p->type || p->type->type != AST_POINTER_TYPE) return 0;

    // A qualified type name is ONE BasicTypeNode carrying a `package` field —
    // NOT a selector expression over an identifier. See parser.y's
    // `identifier DOT identifier` arm of type_name.
    const ASTNode* pointee = ((const PointerTypeNode*)p->type)->element_type;
    if (!pointee || pointee->type != AST_BASIC_TYPE) return 0;

    const BasicTypeNode* bt = (const BasicTypeNode*)pointee;
    if (!bt->name || strcmp(bt->name, "T") != 0) return 0;
    if (!bt->package || !testing_local) return 0;
    return strcmp(bt->package, testing_local) == 0;
}

// Linear growth: a package declares tens of tests, not thousands, so the
// reallocation cost is irrelevant next to the clarity.
static int test_list_append(TestList* l, const char* name) {
    char** grown = realloc(l->names, (l->count + 1) * sizeof(char*));
    if (!grown) return 0;
    l->names = grown;

    char* copy = strdup(name);
    if (!copy) return 0;
    l->names[l->count++] = copy;
    return 1;
}

int test_discovery_collect(ASTNode** programs, size_t program_count,
                           const char** file_names, TestList* out) {
    if (!out) return 0;
    out->names = NULL;
    out->count = 0;
    if (!programs) return 1;

    for (size_t i = 0; i < program_count; i++) {
        if (!programs[i] || programs[i]->type != AST_PROGRAM) continue;
        const ProgramNode* prog = (const ProgramNode*)programs[i];
        const char* testing_local = testing_local_name(prog);
        const char* fallback_name =
            (file_names && file_names[i]) ? file_names[i] : "<unknown>";

        for (const ASTNode* d = prog->decls; d; d = d->next) {
            if (d->type != AST_FUNC_DECL) continue;
            const FuncDeclNode* f = (const FuncDeclNode*)d;
            if (!is_test_name(f->name)) continue;

            if (!has_test_signature(f, testing_local)) {
                // Stop at the FIRST bad shape. Continuing would stack
                // diagnostics from functions the author has not looked at yet
                // and bury the one that needs fixing.
                //
                // Position.filename is stamped by parse_input, so it normally
                // carries the name already; file_names is the fallback for a
                // node minted without one.
                fprintf(stderr,
                        "Test error at %s:%d:%d: %s must have signature func(t *testing.T)\n",
                        d->pos.filename ? d->pos.filename : fallback_name,
                        d->pos.line, d->pos.column, f->name);
                test_list_free(out);
                return 0;
            }

            if (!test_list_append(out, f->name)) {
                fprintf(stderr, "Test error: out of memory collecting test names\n");
                test_list_free(out);
                return 0;
            }
        }
    }
    return 1;
}

void test_list_free(TestList* l) {
    if (!l) return;
    for (size_t i = 0; i < l->count; i++) free(l->names[i]);
    free(l->names);
    l->names = NULL;
    l->count = 0;
}
