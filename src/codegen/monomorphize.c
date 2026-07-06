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
    // SAME construction, `type_param(name, idx++, bound)` per name across
    // every type-param group, `bound` re-resolved from the group's AST
    // constraint node exactly as declare_function_signature does. This does
    // NOT bind a concrete type; it lets a raw AST type node inside the
    // template that calls type_from_ast(checker, ...) on a bare param name
    // (e.g. the return-type node re-resolved at function_codegen.c's
    // codegen_generate_function_decl, independently of the Variable's
    // already-typed signature) resolve to a TYPE_PARAM instead of erroring
    // "Unknown type 'T'".
    //
    // Function generics Tier B: the bound MUST be carried here (not NULL) —
    // codegen re-invokes type_check_* on template body expressions during
    // this instance's codegen (e.g. call_codegen.c's method-call block
    // needs a bounded-T method call's type to compute the call's return
    // ValueInfo), and type_check_selector_expr's TYPE_PARAM branch only
    // resolves a method against the bound's interface method set — a NULL
    // bound sends it to the "Selector on non-struct" rejection instead,
    // which starves the return value of its Type and crashes downstream
    // codegen (LLVMTypeOf on a NULL value). Popped unconditionally before
    // returning, on every path, so a failed instantiation can't leak type
    // params into whatever the worklist stamps next.
    size_t saved_tp = checker->active_type_param_count;
    {
        int idx = 0;
        for (ASTNode* tp = tmpl->type_params; tp; tp = tp->next) {
            VarDeclNode* g = (VarDeclNode*)tp;
            Type* bound = g->type ? type_from_ast(checker, g->type) : NULL;
            for (size_t i = 0; i < g->name_count; i++)
                type_checker_push_type_param(checker,
                    type_param(g->names[i], idx++, bound));
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

// Task 10 (Part A — transitivity): does any of args[0..n) still contain an
// unbound TYPE_PARAM anywhere in its structure? Recurses through the same
// Tier-A shapes type_substitute/unify_types do (slice element / pointer
// pointee / function param+return); every other Type kind is a concrete
// leaf. Two uses below: (1) filter checker->instantiations down to only the
// CONCRETE seeds at worklist start — a symbolic entry (e.g. `Id`'s call
// recorded from inside `Twice`'s still-uninstantiated body, bound to
// `Twice`'s own TYPE_PARAM rather than a real type) would hand
// codegen_generate_function_instance an unbound TYPE_PARAM, which
// codegen_type_to_llvm treats as an internal error (Task 8) — such entries
// are instead rediscovered CONCRETELY once their enclosing template is
// itself dequeued (see the nested-call walk below); (2) verify a nested
// call's just-substituted args are fully concrete before enqueuing it.
static int args_contain_typeparam(Type* const* args, size_t n) {
    for (size_t i = 0; i < n; i++) {
        Type* t = args[i];
        if (!t) continue;
        switch (t->kind) {
            case TYPE_PARAM:
                return 1;
            case TYPE_SLICE: {
                Type* elem = t->data.slice.element_type;
                if (args_contain_typeparam(&elem, 1)) return 1;
                break;
            }
            case TYPE_POINTER: {
                Type* pointee = t->data.pointer.pointee_type;
                if (args_contain_typeparam(&pointee, 1)) return 1;
                break;
            }
            case TYPE_FUNCTION: {
                if (args_contain_typeparam(t->data.function.param_types,
                                           t->data.function.param_count))
                    return 1;
                Type* ret = t->data.function.return_type;
                if (args_contain_typeparam(&ret, 1)) return 1;
                break;
            }
            default:
                break; // concrete leaf (scalar, struct, string, ...)
        }
    }
    return 0;
}

// One discovered call site (an AST_CALL_EXPR with type_arg_count > 0) inside
// a generic template body, collected by collect_generic_calls below. A
// throwaway singly-linked list, freed by mono_free_calls right after each
// dequeued worklist item's nested-call discovery pass.
typedef struct MonoCallRef {
    CallExprNode* call;
    struct MonoCallRef* next;
} MonoCallRef;

static void mono_push_call(MonoCallRef** head, CallExprNode* call) {
    MonoCallRef* n = malloc(sizeof(MonoCallRef));
    if (!n) return; // best-effort: OOM here just misses this call site, not a crash
    n->call = call;
    n->next = *head;
    *head = n;
}

static void mono_free_calls(MonoCallRef* head) {
    while (head) {
        MonoCallRef* next = head->next;
        free(head);
        head = next;
    }
}

// Task 10 (Part A): a minimal recursive AST walker collecting every
// AST_CALL_EXPR with type_arg_count > 0 reachable from `node` (a single node
// or a next-chained statement/expression list). No shared project-wide AST
// visitor exists to reuse (grepped the tree before writing this). Descends
// every statement/expression kind a Tier-A generic body can plausibly
// contain; an unhandled kind (type nodes, GPU/WASM/contract-only nodes,
// etc.) is a silent no-descend rather than an error — missing a nested
// generic call there only means it is not pre-stamped, which surfaces as a
// NULL callee at Part B's call-rewiring site (call_codegen.c), not a silent
// miscompile.
static void collect_generic_calls(ASTNode* node, MonoCallRef** out) {
    for (ASTNode* n = node; n; n = n->next) {
        switch (n->type) {
            case AST_BLOCK_STMT:
                collect_generic_calls(((BlockStmtNode*)n)->statements, out);
                break;
            case AST_EXPR_STMT:
                collect_generic_calls(((ExprStmtNode*)n)->expr, out);
                break;
            case AST_IF_STMT: {
                IfStmtNode* s = (IfStmtNode*)n;
                collect_generic_calls(s->condition, out);
                collect_generic_calls(s->then_stmt, out);
                collect_generic_calls(s->else_stmt, out);
                break;
            }
            case AST_IF_LET_STMT: {
                IfLetStmtNode* s = (IfLetStmtNode*)n;
                collect_generic_calls(s->nullable_expr, out);
                collect_generic_calls(s->then_stmt, out);
                collect_generic_calls(s->else_stmt, out);
                break;
            }
            case AST_FOR_STMT: {
                ForStmtNode* s = (ForStmtNode*)n;
                collect_generic_calls(s->init, out);
                collect_generic_calls(s->condition, out);
                collect_generic_calls(s->post, out);
                collect_generic_calls(s->range_expr, out);
                collect_generic_calls(s->body, out);
                break;
            }
            case AST_RETURN_STMT:
                collect_generic_calls(((ReturnStmtNode*)n)->values, out);
                break;
            case AST_DEFER_STMT:
                collect_generic_calls(((DeferStmtNode*)n)->call, out);
                break;
            case AST_GO_STMT:
                collect_generic_calls(((GoStmtNode*)n)->call, out);
                break;
            case AST_SELECT_STMT:
                collect_generic_calls(((SelectStmtNode*)n)->cases, out);
                break;
            case AST_SELECT_CASE: {
                SelectCaseNode* s = (SelectCaseNode*)n;
                collect_generic_calls(s->comm, out);
                collect_generic_calls(s->body, out);
                break;
            }
            case AST_SWITCH_STMT: {
                SwitchStmtNode* s = (SwitchStmtNode*)n;
                collect_generic_calls(s->tag, out);
                collect_generic_calls(s->cases, out);
                break;
            }
            case AST_CASE_CLAUSE: {
                CaseClauseNode* s = (CaseClauseNode*)n;
                collect_generic_calls(s->exprs, out);
                collect_generic_calls(s->body, out);
                break;
            }
            case AST_TYPE_SWITCH: {
                TypeSwitchNode* s = (TypeSwitchNode*)n;
                collect_generic_calls(s->expr, out);
                collect_generic_calls(s->cases, out);
                break;
            }
            case AST_TYPE_CASE:
                collect_generic_calls(((TypeCaseNode*)n)->body, out);
                break;
            case AST_UNSAFE_STMT:
                collect_generic_calls(((UnsafeStmtNode*)n)->body, out);
                break;
            case AST_COMPTIME_BLOCK:
                collect_generic_calls(((ComptimeBlockNode*)n)->body, out);
                break;
            case AST_VAR_DECL:
                collect_generic_calls(((VarDeclNode*)n)->values, out);
                break;
            case AST_CONST_DECL:
                collect_generic_calls(((ConstDeclNode*)n)->values, out);
                break;
            case AST_MULTI_ASSIGN:
                collect_generic_calls(((MultiAssignNode*)n)->values, out);
                break;
            case AST_CALL_EXPR: {
                CallExprNode* c = (CallExprNode*)n;
                if (c->type_arg_count > 0) mono_push_call(out, c);
                collect_generic_calls(c->function, out);
                collect_generic_calls(c->args, out);
                break;
            }
            case AST_INDEX_EXPR: {
                IndexExprNode* e = (IndexExprNode*)n;
                collect_generic_calls(e->expr, out);
                collect_generic_calls(e->index, out);
                break;
            }
            case AST_SLICE_INDEX_EXPR: {
                SliceIndexExprNode* e = (SliceIndexExprNode*)n;
                collect_generic_calls(e->expr, out);
                collect_generic_calls(e->low, out);
                collect_generic_calls(e->high, out);
                break;
            }
            case AST_SELECTOR_EXPR:
                collect_generic_calls(((SelectorExprNode*)n)->expr, out);
                break;
            case AST_BINARY_EXPR: {
                BinaryExprNode* e = (BinaryExprNode*)n;
                collect_generic_calls(e->left, out);
                collect_generic_calls(e->right, out);
                break;
            }
            case AST_UNARY_EXPR:
                collect_generic_calls(((UnaryExprNode*)n)->operand, out);
                break;
            case AST_POSTFIX_EXPR:
                collect_generic_calls(((PostfixExprNode*)n)->operand, out);
                break;
            case AST_TRY_EXPR:
                collect_generic_calls(((TryExprNode*)n)->expr, out);
                break;
            case AST_CATCH_EXPR: {
                CatchExprNode* e = (CatchExprNode*)n;
                collect_generic_calls(e->expr, out);
                collect_generic_calls(e->catch_body, out);
                break;
            }
            case AST_ADDR_OF:
                collect_generic_calls(((AddrOfNode*)n)->operand, out);
                break;
            case AST_PTR_DEREF:
                collect_generic_calls(((PtrDerefNode*)n)->pointer, out);
                break;
            case AST_TYPE_ASSERT_EXPR:
            case AST_TYPE_ASSERT:
                collect_generic_calls(((TypeAssertNode*)n)->expr, out);
                break;
            case AST_PAREN_EXPR: { // map literal — see MapLitNode's doc comment, ast.h
                MapLitNode* m = (MapLitNode*)n;
                collect_generic_calls(m->keys, out);
                collect_generic_calls(m->values, out);
                break;
            }
            case AST_SLICE_EXPR: // slice literal — see SliceLitNode's doc comment, ast.h
                collect_generic_calls(((SliceLitNode*)n)->elements, out);
                break;
            case AST_ARRAY_LITERAL:
                collect_generic_calls(((ArrayLitNode*)n)->elements, out);
                break;
            case AST_KEYED_ELEMENT: {
                KeyedElementNode* e = (KeyedElementNode*)n;
                collect_generic_calls(e->key, out);
                collect_generic_calls(e->value, out);
                break;
            }
            case AST_STRUCT_LITERAL:
                collect_generic_calls(((StructLiteralNode*)n)->field_values, out);
                break;
            case AST_FUNC_LIT:
                collect_generic_calls(((FuncLitNode*)n)->body, out);
                break;
            case AST_SLICE_CONVERSION:
                collect_generic_calls(((SliceConvNode*)n)->operand, out);
                break;
            default:
                break; // identifiers/literals/type nodes: no children to descend
        }
    }
}

// Every mangled symbol this monomorphization run has either fully stamped OR
// started stamping (see mono_instantiate: added BEFORE recursing into a
// template's own nested dependencies). Doubles as both the dedup set (skip a
// repeated {fn,args} record) and the cycle guard (a self- or mutually-
// recursive generic instantiation is found already-in-flight here and
// treated as a no-op rather than recursing forever — see mono_instantiate's
// doc comment for why that is safe).
typedef struct MonoSeen {
    char* sym;
    struct MonoSeen* next;
} MonoSeen;

static int mono_seen_has(MonoSeen* seen, const char* sym) {
    for (; seen; seen = seen->next) {
        if (strcmp(seen->sym, sym) == 0) return 1;
    }
    return 0;
}

// Defensive bound on the number of DISTINCT {template,args} instantiations a
// single compile may produce, guarding against pathological/runaway generic
// recursion (e.g. a mutual-generic chain that keeps minting new concrete
// type instantiations). Not expected to bind on any real Tier-A program.
#define MONO_INSTANTIATION_CAP 4096

// Task 10 (Part A): recursively ensure `tmpl_var` is instantiated under
// `args`/`n`, instantiating every nested generic-calls-generic dependency
// FIRST — children before parents — so each nested instance's LLVM symbol
// already exists by the time this template's OWN body (which calls it by
// name) is actually emitted by the codegen_generate_function_instance call
// at the bottom. This ordering is why recursion (not a FIFO worklist) is
// used: a breadth-first queue that discovers-then-enqueues a nested item and
// only stamps it on a LATER iteration would stamp the OUTER template first,
// leaving its nested callee's symbol missing when the outer body is
// emitted — Part B's call-rewiring (call_codegen.c) would then find no
// matching LLVMGetNamedFunction and silently fall through to the ordinary
// bare-name lookup, which fails ("Undefined identifier") since the generic
// template itself is never emitted under its bare name (Task 4). Discovery
// itself (the AST walk + type_substitute below) never touches the LLVM
// builder, so recursing into it before this item's own stamp call is always
// safe regardless of builder position.
//
// `owns_args` distinguishes a SEEDED call (args borrowed from a
// checker->instantiations record, owned by the TypeChecker — must NOT be
// freed here) from a nested-discovery call (args freshly malloc'd by the
// type_substitute loop below — owned by this call, freed before returning).
//
// `seen` is marked for this {tmpl_var,args} BEFORE recursing into nested
// discovery: a self-recursive generic call (`Fact(n-1)` inside `Fact[T]`,
// same T) resolves to the SAME {tmpl_var,args} tuple currently being
// processed, is found already-in-flight, and is skipped as a no-op here —
// which is correct, not a miss, because codegen_generate_function_instance /
// codegen_generate_function_decl declare the LLVM function value (via
// symbol_override) BEFORE emitting its body, exactly like any other
// recursive function; the self-call resolves once that body is actually
// emitted, by the same Part B lookup every other call site uses.
static int mono_instantiate(CodeGenerator* codegen, TypeChecker* checker,
                             MonoSeen** seen, size_t* stamped_count,
                             Variable* tmpl_var, Type** args, size_t n,
                             int owns_args) {
    char* sym = codegen_mangle_instance(tmpl_var->name, args, n);
    if (LLVMGetNamedFunction(codegen->module, sym) || mono_seen_has(*seen, sym)) {
        free(sym);
        if (owns_args) free(args);
        return 1; // already stamped, or already in flight (cycle guard) — no-op
    }
    if (*stamped_count >= MONO_INSTANTIATION_CAP) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"},
                      "monomorphization exceeded instantiation cap (possible runaway generic recursion)");
        free(sym);
        if (owns_args) free(args);
        return 0;
    }
    (*stamped_count)++;

    MonoSeen* sn = malloc(sizeof(MonoSeen));
    if (!sn) {
        free(sym);
        if (owns_args) free(args);
        return 0;
    }
    sn->sym = sym; // ownership moves to the seen list; freed once by the top-level driver
    sn->next = *seen;
    *seen = sn;

    FuncDeclNode* tmpl = (FuncDeclNode*)tmpl_var->generic_decl;

    MonoCallRef* calls = NULL;
    collect_generic_calls(tmpl->body, &calls);
    int ok = 1;
    for (MonoCallRef* nc = calls; nc && ok; nc = nc->next) {
        CallExprNode* ncall = nc->call;
        if (!ncall->function || ncall->function->type != AST_IDENTIFIER) continue;
        Variable* nested_var = type_checker_lookup_variable(checker,
            ((IdentifierNode*)ncall->function)->name);
        if (!nested_var || !nested_var->is_generic || !nested_var->generic_decl) continue;

        size_t nn = ncall->type_arg_count;
        Type** resolved = nn ? malloc(sizeof(Type*) * nn) : NULL;
        if (nn && !resolved) { ok = 0; break; }
        int concrete = 1;
        for (size_t i = 0; i < nn; i++) {
            // Maps the OUTER template's TYPE_PARAMs (args, indexed exactly
            // like tmpl_var's own type params) onto this nested call's
            // recorded type args — this is exactly how a symbolic entry
            // (e.g. `Id` called with `Twice`'s own T) becomes concrete once
            // `Twice` itself is instantiated.
            resolved[i] = type_substitute(ncall->type_args[i], args, n);
            if (!resolved[i]) { concrete = 0; break; }
        }
        if (concrete) concrete = !args_contain_typeparam(resolved, nn);
        if (!concrete) { free(resolved); continue; }

        if (!mono_instantiate(codegen, checker, seen, stamped_count,
                              nested_var, resolved, nn, 1)) {
            ok = 0;
        }
    }
    mono_free_calls(calls);

    if (ok) {
        ok = codegen_generate_function_instance(codegen, checker, tmpl, sym, args, n);
    }
    if (owns_args) free(args);
    return ok;
}

// Task 9/10: the monomorphization driver. See the doc comment at the
// declaration (include/codegen.h) for the dedup/no-op contract. Task 10
// replaces Task 9's single linear pass over checker->instantiations with
// mono_instantiate's recursive discovery: seed with only the CONCRETE
// instantiations (args_contain_typeparam filters out the symbolic ones
// recorded from inside a not-yet-instantiated generic body — those are
// rediscovered concretely, transitively, by mono_instantiate's own nested-
// call walk once their enclosing template is itself seeded here).
int codegen_monomorphize(CodeGenerator* codegen, TypeChecker* checker) {
    if (!codegen || !checker) return 0;
#if LLVM_AVAILABLE
    MonoSeen* seen = NULL;
    size_t stamped_count = 0;
    int ok = 1;

    for (GenericInstantiation* it = checker->instantiations; it && ok; it = it->next) {
        if (args_contain_typeparam(it->args, it->n)) continue; // symbolic seed — rediscovered concretely below
        if (!mono_instantiate(codegen, checker, &seen, &stamped_count,
                              it->fn, it->args, it->n, 0)) {
            ok = 0;
        }
    }

    while (seen) {
        MonoSeen* s = seen;
        seen = s->next;
        free(s->sym);
        free(s);
    }

    if (!ok) return 0;
#endif
    return codegen->error_count == 0;
}
