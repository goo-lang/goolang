#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Task 7: type-argument mangling for monomorphized generic instances. These
// helpers are pure name-computation (no LLVM/codegen state touched) so they
// can be unit-probed standalone; Tasks 9-10 call them from the monomorphization
// worklist to name each concrete instantiation (e.g. `Id[int]` -> `Id__int`).

// Per-file str_dup idiom (see src/types/types.c and friends) rather than a
// project-wide symbol: no header in this codebase declares a shared
// non-static str_dup, so every translation unit that wants one defines its
// own static copy. NULL-safe (mirrors the other copies), even though every
// call site below already guards against a NULL argument.
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) memcpy(dup, str, len + 1);
    return dup;
}

// A nameable, unique-enough token for a concrete type, suitable for splicing
// into an LLVM symbol name. Recurses through pointer/slice wrappers so
// `*[]int` -> "ptr_slice_int"; every other kind falls to the scalar/struct
// name via type_receiver_name, with type_to_string as a fallback.
//
// Both `type_receiver_name` and `type_to_string` are read from
// src/types/types.c: type_receiver_name returns `type->name` for non-struct
// kinds, which is NULL only if some Type was built without going through the
// standard type_int/type_bool/type_float/type_string_type/type_char
// constructors (those all populate `name`); type_to_string never returns
// NULL (it falls back to "?"/"struct"/"null"). So the `n ? n : type_to_string(t)`
// fallback below already guarantees a non-NULL token for every scalar this
// compiler can construct — the literal "T" is a last-resort belt-and-braces
// default in case a future Type kind reaches this path with both unset.
char* codegen_type_mangle_token(const Type* t) {
    if (!t) return str_dup("void");
    switch (t->kind) {
        case TYPE_POINTER: {
            char* inner = codegen_type_mangle_token(t->data.pointer.pointee_type);
            char* out = malloc(strlen(inner) + 5);
            sprintf(out, "ptr_%s", inner); free(inner); return out;
        }
        case TYPE_SLICE: {
            char* inner = codegen_type_mangle_token(t->data.slice.element_type);
            char* out = malloc(strlen(inner) + 7);
            sprintf(out, "slice_%s", inner); free(inner); return out;
        }
        default: {
            const char* n = type_receiver_name(t);
            if (!n) n = type_to_string(t); // fallback for scalars (never NULL)
            return str_dup(n ? n : "T");
        }
    }
}

// `base` + `{args[0..n)}` -> `base__tok0__tok1...`, e.g. `Map` + {int,string}
// -> `Map__int__string`. Caller frees the result.
char* codegen_mangle_instance(const char* base, Type* const* args, size_t n) {
    size_t cap = strlen(base) + 1;
    char** toks = calloc(n ? n : 1, sizeof(char*));
    for (size_t i = 0; i < n; i++) { toks[i] = codegen_type_mangle_token(args[i]); cap += strlen(toks[i]) + 2; }
    char* out = malloc(cap); strcpy(out, base);
    for (size_t i = 0; i < n; i++) { strcat(out, "__"); strcat(out, toks[i]); free(toks[i]); }
    free(toks); return out;
}

// Task 9: stamp one concrete instantiation. See the doc comment at the
// declaration (include/codegen.h) for the two-substitution contract; the
// non-LLVM stub build has neither `active_subst`/`symbol_override` on
// CodeGenerator nor a real codegen_generate_function_decl to reuse, so it
// just reports failure (mirrors codegen_generate_function_decl's own
// !LLVM_AVAILABLE stub branch, function_codegen.c).
int codegen_generate_function_instance(CodeGenerator* codegen, TypeChecker* checker,
                                       FuncDeclNode* tmpl, const char* sym,
                                       Type** args, size_t n) {
#if !LLVM_AVAILABLE
    (void)tmpl; (void)sym; (void)args; (void)n;
    codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !tmpl || !sym) return 0;

    // Requirement 1 (checker side): push tmpl's type-param names onto the
    // checker's active-type-param stack, index-matched exactly like
    // declare_function_signature's own push (src/types/type_checker.c) — the
    // SAME construction, `type_param(name, idx++, NULL)` per name across
    // every type-param group. This does NOT bind a concrete type; it only
    // lets a raw AST type node inside the template that calls
    // type_from_ast(checker, ...) on a bare param name (e.g. the return-type
    // node re-resolved at function_codegen.c's codegen_generate_function_decl,
    // independently of the Variable's already-typed signature) resolve to a
    // TYPE_PARAM instead of erroring "Unknown type 'T'". Popped unconditionally
    // before returning, on every path, so a failed instantiation can't leak
    // type params into whatever the worklist stamps next.
    size_t saved_tp = checker->active_type_param_count;
    {
        int idx = 0;
        for (ASTNode* tp = tmpl->type_params; tp; tp = tp->next) {
            VarDeclNode* g = (VarDeclNode*)tp;
            for (size_t i = 0; i < g->name_count; i++)
                type_checker_push_type_param(checker,
                    type_param(g->names[i], idx++, NULL));
        }
    }

    // Requirement 1 (codegen side): the VALUE binding. codegen_type_to_llvm's
    // TYPE_PARAM case (Task 8) resolves a TYPE_PARAM's index through
    // active_subst to the concrete arg — this is what actually lowers `T` to
    // e.g. `i64` for the signature/body the checker-side push above merely
    // allowed to be looked up as a TYPE_PARAM in the first place.
    Type** saved_subst = codegen->active_subst;
    size_t saved_subst_n = codegen->active_subst_n;
    const char* saved_override = codegen->symbol_override;
    codegen->active_subst = args;
    codegen->active_subst_n = n;
    // Requirement — the rename: forces the emitted LLVM symbol to `sym`
    // (e.g. `Id__int64`) instead of the template's bare name, so the same
    // FuncDeclNode can be lowered more than once under distinct symbols.
    codegen->symbol_override = sym;

    // codegen_generate_function_decl is called DIRECTLY here (not through
    // codegen_generate_declaration), so the Task 4 "skip generic template"
    // guard — `if (((FuncDeclNode*)decl)->type_params) return 1;` in
    // codegen_generate_declaration (codegen.c) — is never reached; that
    // guard exists precisely to stop the ordinary declaration loop from
    // emitting the template directly, not to block this deliberate
    // per-instance call.
    int ok = codegen_generate_function_decl(codegen, checker, (ASTNode*)tmpl);

    codegen->symbol_override = saved_override;
    codegen->active_subst = saved_subst;
    codegen->active_subst_n = saved_subst_n;
    type_checker_pop_type_params(checker, saved_tp);

    return ok;
#endif
}

// Task 9: the worklist driver. See the doc comment at the declaration
// (include/codegen.h) for the dedup/no-op contract.
int codegen_monomorphize(CodeGenerator* codegen, TypeChecker* checker) {
    if (!codegen || !checker) return 0;
#if LLVM_AVAILABLE
    for (GenericInstantiation* it = checker->instantiations; it; it = it->next) {
        char* sym = codegen_mangle_instance(it->fn->name, it->args, it->n);
        // Emit-once: the same {fn,args} tuple may have been recorded more
        // than once (repeated calls with identical inferred type args), and
        // an already-present symbol means some earlier worklist entry (or a
        // future generic-calls-generic pre-walk) already stamped it.
        if (!LLVMGetNamedFunction(codegen->module, sym)) {
            if (!codegen_generate_function_instance(codegen, checker,
                    (FuncDeclNode*)it->fn->generic_decl, sym, it->args, it->n)) {
                free(sym);
                return 0;
            }
        }
        free(sym);
    }
#endif
    return codegen->error_count == 0;
}
