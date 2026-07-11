#include "types.h"
#include "comptime.h"
#include "embedding.h"
#include "lane_ownership.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Helper function to duplicate strings (per-file str_dup idiom under -std=c23).
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

// Type checker initialization and cleanup

TypeChecker* type_checker_new(void) {
    TypeChecker* checker = xmalloc(sizeof(TypeChecker));
    if (!checker) return NULL;

    checker->current_scope = scope_new(NULL);
    checker->next_scope_id = 1;
    checker->builtin_types = NULL;
    checker->current_file = NULL;
    checker->error_count = 0;
    checker->warning_count = 0;
    checker->type_cache = NULL;
    checker->type_cache_size = 0;
    checker->type_cache_capacity = 0;

    // stdlib Phase 0: package registry empty; NULL current_package == main.
    checker->packages = NULL;
    checker->current_package = NULL;

    // Function generics Task 6: no recorded generic-call instantiations yet.
    checker->instantiations = NULL;

    // Comptime value params Task 3: no recorded comptime-call instantiations yet.
    checker->comptime_instantiations = NULL;

    // gofmt-syntax-b Task 3: not inside any switch/select clause body yet.
    checker->fallthrough_ctx = FALLTHROUGH_CTX_NONE;

    // Codegen-hardening R1-TC: zero every counter in the consolidated
    // per-function scratch struct (literal_stack_len, active_type_param_
    // count, label_count, goto_label_count, arena_chain_depth) in one shot —
    // mirrors the six individual zero-inits this replaces. Array CONTENTS
    // (label_names, goto_label_names, goto_label_arena_depth, ...) are only
    // ever read up to their paired counter, so they need no upfront init;
    // memset is used anyway (over leaving them uninitialized) since it's no
    // more expensive and leaves no ambiguity for a future field added here.
    memset(&checker->tc_fctx, 0, sizeof(checker->tc_fctx));

    // M11-types-const-integrate (part A): set up a comptime context so
    // that is_comptime const-decl RHS expressions can be routed through
    // comptime_eval_expression. The wrapper owns the type-level scaffold
    // (registered TypeFunctions, computed-type cache); the inner raw
    // context is what comptime_eval_expression actually consumes. Both
    // are torn down in type_checker_free.
    ComptimeContext* raw = comptime_context_new(NULL);
    checker->comptime_type_ctx = raw ? comptime_type_context_new(raw) : NULL;

    // P3.6 (method values): no selector is being checked as a call callee
    // at construction time — see the field's doc comment (types.h).
    checker->selector_call_position = 0;

    type_checker_init_builtins(checker);

    return checker;
}

void type_checker_free(TypeChecker* checker) {
    if (!checker) return;

    scope_free(checker->current_scope);

    // Free builtin types
    if (checker->builtin_types) {
        for (int i = 0; i < TYPE_COUNT; i++) {
            if (checker->builtin_types[i]) {
                type_free(checker->builtin_types[i]);
            }
        }
        free(checker->builtin_types);
    }

    // Free type cache
    if (checker->type_cache) {
        for (size_t i = 0; i < checker->type_cache_size; i++) {
            if (checker->type_cache[i]) {
                type_free(checker->type_cache[i]);
            }
        }
        free(checker->type_cache);
    }

    // comptime_type_context_free does NOT free the inner raw context —
    // that's the caller's responsibility. Order: capture the raw pointer,
    // free the wrapper, free the raw.
    if (checker->comptime_type_ctx) {
        ComptimeContext* raw = checker->comptime_type_ctx->comptime_ctx;
        comptime_type_context_free(checker->comptime_type_ctx);
        if (raw) comptime_context_free(raw);
    }

    // Free the imported-package registry. Each Package owns its two strings and
    // its exports scope; scope_free tears down the fresh Variable copies (their
    // shared Type* pointers are NOT owned by the Variable, so no double-free).
    Package* pkg = checker->packages;
    while (pkg) {
        Package* next = pkg->next;
        free(pkg->import_path);
        free(pkg->name);
        scope_free(pkg->exports);
        // P6 M1: the package AST whose ownership compile_resolved_packages
        // transferred here (NULL if the graph still owns it). Freed last, after
        // the exports scope whose Variables' func_decl_node pointed into it —
        // scope_free frees only the Variable copies, never the AST nodes they
        // reference, so the order is not load-bearing, only tidy.
        ast_node_free(pkg->owned_ast);
        free(pkg);
        pkg = next;
    }

    // Function generics Task 6: free the instantiation-record list itself
    // (fn is a Scope-owned Variable* freed by scope_free above; args[i] are
    // Type* pointers owned by the type checker/interning system, same
    // non-ownership as everywhere else in this function — only the array and
    // the list nodes are this list's own allocations).
    //
    // Comptime+generic composition (sub-project 2): comptime_values is this
    // same node's second malloc'd payload (0/NULL for a generic-only seed,
    // so free(NULL) is a safe no-op) — freed here alongside args so a
    // composed seed's two payloads are torn down exactly once, together,
    // never independently.
    GenericInstantiation* inst = checker->instantiations;
    while (inst) {
        GenericInstantiation* next = inst->next;
        free(inst->args);
        free(inst->comptime_values);
        free(inst);
        inst = next;
    }

    // Comptime value params Task 3: free the comptime-instantiation-record
    // list itself, mirroring the GenericInstantiation teardown just above —
    // fn is a Scope-owned Variable* freed by scope_free above; only the
    // values array and the list nodes are this list's own allocations.
    ComptimeInstantiation* cinst = checker->comptime_instantiations;
    while (cinst) {
        ComptimeInstantiation* next = cinst->next;
        free(cinst->values);
        free(cinst);
        cinst = next;
    }

    free(checker->current_file);
    free(checker);
}

// Linear search of the package registry by canonical import path.
Package* type_checker_find_package(TypeChecker* checker, const char* import_path) {
    if (!checker || !import_path) return NULL;
    for (Package* pkg = checker->packages; pkg; pkg = pkg->next) {
        if (pkg->import_path && strcmp(pkg->import_path, import_path) == 0) {
            return pkg;
        }
    }
    return NULL;
}

// Create a package namespace and push it onto the registry. Strings are copied
// (str_dup); the exports scope starts empty and is filled by the caller via
// package_export_filter once the package body has been checked.
Package* type_checker_add_package(TypeChecker* checker, const char* import_path, const char* name) {
    if (!checker || !import_path || !name) return NULL;

    Package* pkg = xmalloc(sizeof(Package));
    if (!pkg) return NULL;

    pkg->import_path = str_dup(import_path);
    pkg->name = str_dup(name);
    pkg->exports = scope_new(NULL);
    pkg->state = 0;  // unvisited
    pkg->owned_ast = NULL;  // P6 M1: set by compile_resolved_packages on success
    pkg->next = checker->packages;
    checker->packages = pkg;

    return pkg;
}

// Copy every capitalised (A-Z leading) top-level symbol of `pkg_scope` into
// `exports`. Each export is a FRESH Variable (variable_new) so that pkg_scope
// and exports never co-own a Variable node — variable_free frees only the name
// (str_dup'd copy) and the comptime value, never the shared Type*, so sharing
// the Type* pointer across the two scopes is safe.
//
// P6 M1 (comptime-wall lift): the copy also carries `func_decl_node` and
// `owner_pkg` (== `owner`). The FuncDeclNode back-reference is what lets a
// `pkg.Fill(4, ...)` selector call — which resolves to THIS export copy as its
// callee (expression_checker.c) — capture its comptime argument and record a
// monomorphization seed that survives to main-pass codegen: the package's own
// inner-scope Fill Variable is freed by scope_pop (goo.c) right after the
// package is codegen'd, but this export copy lives on the Package's `exports`
// scope (TypeChecker-owned) and the FuncDeclNode it points at lives on the
// PkgGraph AST, so both outlast the package scope teardown. `owner_pkg` then
// package-qualifies the instance symbol at monomorphization time. For a
// non-function export `func_decl_node` is NULL (a no-op copy); `owner_pkg` is
// set on every export uniformly (only ever read for function seeds).
void package_export_filter(Scope* pkg_scope, Scope* exports, struct Package* owner) {
    if (!pkg_scope || !exports) return;

    for (Variable* v = pkg_scope->variables; v; v = v->next) {
        if (!v->name || v->name[0] < 'A' || v->name[0] > 'Z') {
            continue;  // only exported (capitalised) top-level symbols
        }
        Variable* copy = variable_new(v->name, v->type, v->declared_pos);
        if (!copy) continue;
        copy->is_initialized = v->is_initialized;
        copy->mutability = v->mutability;
        copy->is_builtin = v->is_builtin;
        copy->func_decl_node = v->func_decl_node;
        copy->owner_pkg = owner;
        if (!scope_add_variable(exports, copy)) {
            // Duplicate name already present in exports — discard the copy.
            variable_free(copy);
        }
    }
}

void type_checker_init_builtins(TypeChecker* checker) {
    if (!checker) return;
    
    checker->builtin_types = malloc(sizeof(Type*) * TYPE_COUNT);
    if (!checker->builtin_types) return;
    
    memset(checker->builtin_types, 0, sizeof(Type*) * TYPE_COUNT);
    
    // Initialize builtin types
    checker->builtin_types[TYPE_VOID] = type_void();
    checker->builtin_types[TYPE_BOOL] = type_bool();
    checker->builtin_types[TYPE_INT8] = type_int(8, 1);
    checker->builtin_types[TYPE_INT16] = type_int(16, 1);
    checker->builtin_types[TYPE_INT32] = type_int(32, 1);
    checker->builtin_types[TYPE_INT64] = type_int(64, 1);
    checker->builtin_types[TYPE_UINT8] = type_int(8, 0);
    checker->builtin_types[TYPE_UINT16] = type_int(16, 0);
    checker->builtin_types[TYPE_UINT32] = type_int(32, 0);
    checker->builtin_types[TYPE_UINT64] = type_int(64, 0);
    checker->builtin_types[TYPE_FLOAT32] = type_float(32);
    checker->builtin_types[TYPE_FLOAT64] = type_float(64);
    checker->builtin_types[TYPE_STRING] = type_string_type();
    checker->builtin_types[TYPE_CHAR] = type_char();
    checker->builtin_types[TYPE_POISON] = type_poison();

    // Add built-in functions to the global scope
    type_checker_add_builtin_functions(checker);
}

Type* type_checker_get_builtin(TypeChecker* checker, TypeKind kind) {
    if (!checker || !checker->builtin_types || kind >= TYPE_COUNT) {
        return NULL;
    }
    return checker->builtin_types[kind];
}

// v1 `error` = `?*int8` (a nullable pointer). Single source of truth so the
// `error` keyword, the n,err destructure, and errors.New stay in lockstep —
// Phase 6's real error struct / `.Error()` changes only this.
Type* type_checker_error_type(TypeChecker* checker) {
    Type* t = type_nullable(type_pointer(type_checker_get_builtin(checker, TYPE_INT8)));
    // Phase 6 Task 3: tag the nullable so `.Error()` dispatch (type checker +
    // codegen) can recognize "this is the error type" without re-deriving its
    // shape. type_nullable() always auto-derives a name (e.g. "?*int8" here),
    // so it is never NULL at this point — overwrite it unconditionally rather
    // than guarding on !t->name (which would never fire).
    if (t) {
        free(t->name);
        t->name = strdup("error");
    }
    return t;
}

// Go 1.18+ predeclared `any` = the empty interface (`interface{}`) — exactly
// the Type the AST_INTERFACE_TYPE case below builds for a bodyless
// `interface {}` (0 methods, unnamed). No special satisfaction/dispatch
// handling is needed beyond that: every concrete type trivially implements
// zero methods. Interface-typed map keys (Task 2, AST_MAP_TYPE gate below)
// admit `any` the same as any other TYPE_INTERFACE key. A fresh Type is
// built on every call, mirroring AST_INTERFACE_TYPE (which does the same for
// every `interface{}` occurrence) — codegen never keys anything off this
// Type's pointer IDENTITY, only off its (empty) method list and the
// vtable's STRING name, which defaults to "iface" for any unnamed interface
// (codegen_interface_vtable, interface_codegen.c) — so two distinct `any`
// Type instances never diverge in codegen.
static Type* type_checker_any_type(void) {
    Type* result = type_new(TYPE_INTERFACE);
    if (!result) return NULL;
    result->data.interface.methods = NULL;
    result->data.interface.method_count = 0;
    result->data.interface.name = NULL;
    result->data.interface.is_synthesized = 0;
    result->data.interface.source_concept = NULL;
    return result;
}

void type_checker_add_builtin_functions(TypeChecker* checker) {
    if (!checker || !checker->current_scope) return;
    
    // make_chan(type, capacity) -> chan type
    // For now, treat make_chan as a generic function - we'll handle it specially in expression checking
    Type* make_chan_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]);
    Variable* make_chan_var = variable_new("make_chan", make_chan_type, (Position){0, 0, 0, "builtin"});
    if (make_chan_var) {
        make_chan_var->is_builtin = 1;
        make_chan_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, make_chan_var);
    }
    
    // println(args...) -> void
    Type* println_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]); // variadic
    Variable* println_var = variable_new("println", println_type, (Position){0, 0, 0, "builtin"});
    if (println_var) {
        println_var->is_builtin = 1;
        println_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, println_var);
    }
    
    // print(args...) -> void
    Type* print_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]); // variadic
    Variable* print_var = variable_new("print", print_type, (Position){0, 0, 0, "builtin"});
    if (print_var) {
        print_var->is_builtin = 1;
        print_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, print_var);
    }

    // panic(v) -> void (does not return; codegen lowers to goo_panic). Marked
    // variadic so the signature check is skipped — the single arg may be any
    // type (a string message, or an error value whose message is extracted).
    Type* panic_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]);
    panic_type->data.function.is_variadic = 1;
    Variable* panic_var = variable_new("panic", panic_type, (Position){0, 0, 0, "builtin"});
    if (panic_var) {
        panic_var->is_builtin = 1;
        panic_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, panic_var);
    }

    // len(slice|array|string) -> int  (single-arg builtin; the codegen
    // path dispatches on the arg's TypeKind)
    Type* len_type = type_function(NULL, 0, checker->builtin_types[TYPE_INT64]); // Go: len -> int (64-bit)
    Variable* len_var = variable_new("len", len_type, (Position){0, 0, 0, "builtin"});
    if (len_var) {
        len_var->is_builtin = 1;
        len_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, len_var);
    }

    // delete(m, k) -> void. Removes k from map m (no-op if absent); codegen
    // lowers to goo_map_delete_sv. Registered like panic (void-returning,
    // predeclared) so the bare identifier resolves before the call is
    // special-cased in type_check_call_expr.
    Type* delete_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]);
    Variable* delete_var = variable_new("delete", delete_type, (Position){0, 0, 0, "builtin"});
    if (delete_var) {
        delete_var->is_builtin = 1;
        delete_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, delete_var);
    }

    // close(ch) -> void (P3.1). Registered like delete (void-returning,
    // predeclared) so the bare identifier resolves before the call is
    // special-cased in type_check_call_expr; codegen lowers it to
    // goo_chan_close.
    Type* close_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]);
    Variable* close_var = variable_new("close", close_type, (Position){0, 0, 0, "builtin"});
    if (close_var) {
        close_var->is_builtin = 1;
        close_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, close_var);
    }

    // recover (P3.5, user decision 2026-07-10: minimum v1 scope). Registered
    // ONLY so `recover()` resolves and reaches the clean v1-unsupported
    // rejection in type_check_call_expr — without this it dies earlier with
    // the misleading "Undefined variable 'recover'". Full panic unwinding
    // is post-v1; every call is rejected there.
    Type* recover_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]);
    Variable* recover_var = variable_new("recover", recover_type, (Position){0, 0, 0, "builtin"});
    if (recover_var) {
        recover_var->is_builtin = 1;
        recover_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, recover_var);
    }

    // make(map[K]V[, hint]) / make([]T, n[, cap]) -> map/slice value.
    // Registered like delete/panic (void-returning stub signature is
    // irrelevant — the call is always special-cased below) so the bare
    // identifier resolves rather than tripping "Undefined variable 'make'"
    // in any path that looks it up before the special case fires.
    Type* make_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]);
    Variable* make_var = variable_new("make", make_type, (Position){0, 0, 0, "builtin"});
    if (make_var) {
        make_var->is_builtin = 1;
        make_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, make_var);
    }

    // cap(slice) -> int and append(slice, elem) -> slice. Both are
    // special-cased in type_check_call_expr (cap returns int; append's
    // result type is the first arg's slice type); registered here so the
    // bare identifiers resolve consistently with len.
    Type* cap_type = type_function(NULL, 0, checker->builtin_types[TYPE_INT64]); // Go: cap -> int (64-bit)
    Variable* cap_var = variable_new("cap", cap_type, (Position){0, 0, 0, "builtin"});
    if (cap_var) {
        cap_var->is_builtin = 1;
        cap_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, cap_var);
    }
    Type* append_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]);
    Variable* append_var = variable_new("append", append_type, (Position){0, 0, 0, "builtin"});
    if (append_var) {
        append_var->is_builtin = 1;
        append_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, append_var);
    }

    // copy(dst, src) -> int. Real checking (dst/src compatibility, incl. the
    // copy(dst []byte, src string) case) lives in the dedicated arm in
    // type_check_call_expr, mirroring append; registered here so the bare
    // identifier resolves consistently with len/cap/append.
    Type* copy_type = type_function(NULL, 0, checker->builtin_types[TYPE_INT64]); // Go: copy -> int (64-bit)
    Variable* copy_var = variable_new("copy", copy_type, (Position){0, 0, 0, "builtin"});
    if (copy_var) {
        copy_var->is_builtin = 1;
        copy_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, copy_var);
    }

    // error(msg) -> !T: constructs the error case of the enclosing function's
    // error-union return type. Registered here so the bare identifier resolves
    // like len/cap/append; the call itself is special-cased in
    // type_check_call_expr (requires exactly one string arg, only valid inside
    // a function returning !T).
    Type* error_type = type_function(NULL, 0, checker->builtin_types[TYPE_VOID]);
    Variable* error_var = variable_new("error", error_type, (Position){0, 0, 0, "builtin"});
    if (error_var) {
        error_var->is_builtin = 1;
        error_var->is_initialized = 1;
        scope_add_variable(checker->current_scope, error_var);
    }

    // NOTE (stdlib Phase 0, Task 4): TYPE_PACKAGE markers for the stdlib-shim
    // packages ({fmt,os,strings,math,strconv,errors}) are NO LONGER seeded here
    // unconditionally. They are now seeded CONDITIONALLY on a real `import` by
    // the driver (src/compiler/goo.c) via type_checker_seed_package_marker,
    // carrying a Package*. This matches Go semantics (a symbol is in scope only
    // if its package is imported) and unifies stdlib + user-package marker
    // handling through one code path. Backward compat is preserved: no `.goo`
    // program uses a stdlib selector without importing its package, so a
    // no-import program is unaffected.
}

// Seed a TYPE_PACKAGE marker for an imported package into the current scope,
// carrying the resolved Package* so selector resolution can reach its exports.
// Single seeding path for BOTH conditional stdlib-shim markers and real user
// packages (goo.c), replacing the old always-on seeding above. A duplicate name
// (e.g. the same package imported twice) is freed and ignored. Returns the
// marker Variable on success, NULL otherwise.
Variable* type_checker_seed_package_marker(TypeChecker* checker,
                                           const char* name, Package* package) {
    if (!checker || !checker->current_scope || !name) return NULL;
    Type* pkg_type = type_new(TYPE_PACKAGE);
    Variable* marker = variable_new(name, pkg_type, (Position){0, 0, 0, "import"});
    if (!marker) return NULL;
    marker->is_builtin = 1;
    marker->is_initialized = 1;
    marker->package = package;  // resolved Package* (may be NULL for pure markers)
    if (!scope_add_variable(checker->current_scope, marker)) {
        variable_free(marker);  // duplicate import of same name — harmless
        return NULL;
    }
    return marker;
}

// Scope management

void scope_push(TypeChecker* checker) {
    if (!checker) return;
    
    Scope* new_scope = scope_new(checker->current_scope);
    if (new_scope) {
        new_scope->scope_id = checker->next_scope_id++;
        checker->current_scope = new_scope;
    }
}

void scope_pop(TypeChecker* checker) {
    if (!checker || !checker->current_scope) return;
    
    Scope* old_scope = checker->current_scope;
    checker->current_scope = old_scope->parent;
    scope_free(old_scope);
}

Variable* type_checker_lookup_variable(TypeChecker* checker, const char* name) {
    if (!checker || !checker->current_scope) return NULL;
    return scope_lookup_variable(checker->current_scope, name);
}

// P4.3 (packages-B): see the doc comment on the declaration (types.h) for the
// full rationale. Dispatch is decided by the receiver type's OWNER, never by
// which scope happens to resolve the bare mangled name first (review-fix,
// CRITICAL): a main-package method with the same receiver-type name AND
// method name ("Point__Scale") used to hijack cross-package dispatch because
// the bare current-scope lookup ran first.
//
//   - owner set, owner != current_package (a cross-package receiver): the
//     owning package's exports are the ONLY legitimate source — Go's rule
//     is that methods on a package's type can only be defined in that
//     package, so NO fallback to the current scope exists (a bare hit there
//     is by construction a different, same-named type's method, or an
//     out-of-package method declaration Go itself would reject). Gated on
//     the METHOD name being exported (see the declaration comment for why
//     the mangled name's own leading case is not sufficient).
//   - owner == current_package (a package's own body checking/codegen'ing
//     calls on its own types): the method Variable lives in the still-pushed
//     package scope under the bare name — exports aren't even populated
//     until the whole body has been checked (package_export_filter runs
//     last) — so the current-scope lookup is the correct source, and
//     unexported methods are correctly callable intra-package.
//   - owner NULL (a main-declared or anonymous/builtin receiver): current
//     scope, today's behavior.
Variable* type_checker_lookup_method(TypeChecker* checker, Type* recv_type,
                                      const char* method_name, const char* mangled_name) {
    if (!checker || !mangled_name) return NULL;
    struct Package* owner = type_receiver_owner_package(recv_type);
    if (owner && owner != checker->current_package) {
        if (!method_name || method_name[0] < 'A' || method_name[0] > 'Z') return NULL;
        return scope_lookup_variable(owner->exports, mangled_name);
    }
    return type_checker_lookup_variable(checker, mangled_name);
}

// Function generics Task 3: active-type-param stack. Pushed by
// declare_function_signature and type_check_function_decl before resolving a
// generic function's param/return/body types, popped on every return path of
// both (see their callers below) so a leaked type param can't leak into a
// sibling function checked afterward.
void type_checker_push_type_param(TypeChecker* checker, Type* tp) {
    if (!checker || !tp) return;
    if (checker->tc_fctx.active_type_param_count < 32)
        checker->tc_fctx.active_type_params[checker->tc_fctx.active_type_param_count++] = tp;
}

void type_checker_pop_type_params(TypeChecker* checker, size_t to_count) {
    if (!checker) return;
    checker->tc_fctx.active_type_param_count = to_count;
}

// Function generics Task 6: record one resolved generic-call instantiation,
// pushed onto the head of checker->instantiations for the monomorphizer
// (Task 9) to consume after type checking finishes. See type_check_generic_call
// (expression_checker.c) for the sole caller today. Takes ownership of `args` —
// the caller must pass an array it does not otherwise hold a reference to (the
// caller's own copy, independent of e.g. the same call's CallExprNode.type_args)
// and must not free or reuse it afterward. `call_site` is stored for a possible
// future per-callsite diagnostic; the field is otherwise unused by Task 9.
//
// Comptime+generic composition (sub-project 2): `comptime_values`/
// `comptime_value_n` extend this recorder for a composed call (function
// declares both `[T]` and `comptime` params) — same ownership-transfer
// contract as `args`, independent allocation, freed alongside it in
// type_checker_free. A generic-only call passes NULL/0 (GenericInstantiation's
// documented 0/NULL default for a generic-only seed) — malloc(0)-then-free
// and a bare NULL both behave identically for `free`, so the on-error
// `free(comptime_values)` paths below are safe regardless of which the
// caller passed.
void type_check_record_instantiation(TypeChecker* checker, Variable* fn,
                                     Type** args, size_t n,
                                     int64_t* comptime_values, size_t comptime_value_n,
                                     struct ASTNode* call_site) {
    (void)call_site;
    if (!checker || !fn) { free(args); free(comptime_values); return; }
    GenericInstantiation* inst = xmalloc(sizeof(GenericInstantiation));
    if (!inst) { free(args); free(comptime_values); return; }
    inst->fn = fn;
    inst->args = args;
    inst->n = n;
    inst->comptime_values = comptime_values;
    inst->comptime_value_n = comptime_value_n;
    inst->next = checker->instantiations;
    checker->instantiations = inst;
}

// Comptime value params Task 3: record one resolved comptime-call
// instantiation, pushed onto the head of checker->comptime_instantiations
// for the monomorphizer (codegen_monomorphize, monomorphize.c) to consume
// after type checking finishes. Mirrors type_check_record_instantiation
// immediately above, one axis over — see that function's doc comment for
// the shared ownership/ordering rationale. Sole caller: type_check_call_expr
// (expression_checker.c), once per call site with comptime_value_arg_count > 0.
void type_check_record_comptime_instantiation(TypeChecker* checker, Variable* fn,
                                               int64_t* values, size_t n,
                                               struct ASTNode* call_site) {
    (void)call_site;
    if (!checker || !fn) { free(values); return; }
    ComptimeInstantiation* inst = xmalloc(sizeof(ComptimeInstantiation));
    if (!inst) { free(values); return; }
    inst->fn = fn;
    inst->values = values;
    inst->n = n;
    inst->next = checker->comptime_instantiations;
    checker->comptime_instantiations = inst;
}

// Innermost-first: a nested function (were that ever legal) would shadow an
// outer type param of the same name, matching ordinary scope lookup.
Type* type_checker_lookup_type_param(TypeChecker* checker, const char* name) {
    if (!checker || !name) return NULL;
    for (size_t i = checker->tc_fctx.active_type_param_count; i-- > 0; ) {
        Type* tp = checker->tc_fctx.active_type_params[i];
        if (tp && tp->data.type_param.name &&
            strcmp(tp->data.type_param.name, name) == 0)
            return tp;
    }
    return NULL;
}

// Register a synthetic, codegen-introduced binding in the current type-checker
// scope. The defer lowering (codegen) snapshots each deferred call's arguments
// at the defer site and rewrites those argument AST nodes to synthetic
// identifiers (`__goo_deferN_argM`). When the deferred call is re-emitted at
// function exit, codegen re-runs the type checker over the rewritten call to
// recover its return type; without a binding for these synthetic names the
// checker would report a spurious "Undefined variable" for each one (the real
// snapshot value lives only in codegen's value table). Declaring the name with
// its actual snapshotted type lets re-checking resolve cleanly and pass the
// callee's argument-compatibility check. Idempotent: refreshes the type if the
// name already exists (the same synthetic name can recur across functions).
void type_checker_declare_synthetic(TypeChecker* checker, const char* name, Type* type) {
    if (!checker || !checker->current_scope || !name) return;

    Variable* existing = scope_lookup_variable(checker->current_scope, name);
    if (existing) {
        if (type) existing->type = type;
        existing->is_initialized = 1;
        return;
    }

    Variable* var = variable_new(name, type, (Position){0, 0, 0, "defer-snapshot"});
    if (!var) return;
    var->is_builtin = 1;       // synthetic, not user-shadowable
    var->is_initialized = 1;
    scope_add_variable(checker->current_scope, var);
}

// Type checking entry points

// Register one function/method signature without checking its body (defined
// below, near type_check_function_decl). Used by the two-pass declaration walk
// so every top-level signature is in scope before any body is checked —
// enabling forward references between functions.
static int declare_function_signature(TypeChecker* checker, FuncDeclNode* func);

// P2.3/T1: register an empty TYPE_STRUCT/TYPE_ENUM shell for every top-level
// struct/enum `type` decl before pass 1 resolves any body (defined below,
// near type_check_type_decl — it hoists that function's own per-decl shell
// primitive). Enables forward and mutual type references the same way
// declare_function_signature enables forward function references.
static int declare_type_shells(TypeChecker* checker, ASTNode* decls);

int type_check_program(TypeChecker* checker, ASTNode* program) {
    if (!checker || !program) return 0;

    // A lexical error (e.g. a malformed char literal '', '\z', or an
    // unterminated 'a) is mapped to an unknown token and SILENTLY SKIPPED by the
    // Bison bridge, so the parse can succeed with the bad token simply gone and
    // the program would otherwise compile to a running binary. Refuse to emit
    // code when the lexer flagged any such error — this is the clean rejection.
    // The lexer already printed a positioned diagnostic for each one.
    extern int goo_lexer_error_count;
    if (goo_lexer_error_count > 0) {
        return 0;
    }

    if (program->type != AST_PROGRAM) {
        type_error(checker, program->pos, "Expected program node");
        return 0;
    }
    
    ProgramNode* prog = (ProgramNode*)program;
    
    // Type check imports
    if (prog->imports) {
        ASTNode* import = prog->imports;
        while (import) {
            // TODO: Handle imports
            import = import->next;
        }
    }
    
    // Pre-pass: register every top-level function in the comptime engine
    // context so an `is_comptime` const RHS like `fib(10)` can resolve user-
    // defined calls regardless of decl ordering. Mirrors the type checker's
    // own forward-reference handling in type_check_function_decl (which adds
    // the function to scope before walking its body). Without this, the
    // engine's lookup_func returns NULL and comptime const evaluation falls
    // through to the codegen's "must be compile-time constant" rejection.
    if (prog->decls && checker->comptime_type_ctx
                    && checker->comptime_type_ctx->comptime_ctx) {
        ComptimeContext* ctx = checker->comptime_type_ctx->comptime_ctx;
        for (ASTNode* d = prog->decls; d; d = d->next) {
            if (d->type == AST_FUNC_DECL) {
                FuncDeclNode* func = (FuncDeclNode*)d;
                if (func->name) {
                    comptime_context_bind_func(ctx, func->name, d);
                }
            }
        }
    }

    // P2.3/T1: pre-pass — register every top-level struct/enum's empty shell
    // BEFORE pass 1 resolves any declaration body, so a forward reference
    // (`type A struct { b *B }` declared before `type B`) or mutual pair
    // (A<->B pointer fields, either order) finds its target already in scope.
    // Must run before pass 1 (not folded into it) because pass 1 resolves
    // bodies interleaved with registrations in source order — exactly the
    // ordering this pre-pass exists to break.
    if (!declare_type_shells(checker, prog->decls)) return 0;

    // Two-pass declaration walk (Go package-scope semantics: a function body may
    // reference any function declared anywhere in the file, not just above it).
    //   Pass 1: register every declaration's SIGNATURE in source order — types,
    //   vars and consts fully (type_check_declaration), function/method
    //   signatures via declare_function_signature. Processing in order means a
    //   signature still resolves the types declared before it, exactly as Go
    //   requires.
    //   Pass 2: check function BODIES, with every sibling signature now visible.
    for (ASTNode* decl = prog->decls; decl; decl = decl->next) {
        if (decl->type == AST_FUNC_DECL) {
            if (!declare_function_signature(checker, (FuncDeclNode*)decl)) return 0;
        } else if (!type_check_declaration(checker, decl)) {
            return 0;
        }
    }
    for (ASTNode* decl = prog->decls; decl; decl = decl->next) {
        if (decl->type == AST_FUNC_DECL) {
            if (!type_check_function_decl(checker, decl)) return 0;
        }
    }

    // P6 M1 Task 5: lanes-specific ownership checks (lane_ownership.c,
    // Component 4 of docs/superpowers/specs/2026-07-11-p6-lanes-m1-design.md).
    // Deliberately run AFTER pass 2, not folded into it: Task 6's obligations
    // 3/4 (extending this same walk) need every FuncLitNode.captured_names
    // fully populated, which only holds once every function body has been
    // type-checked (see docs/superpowers/specs/2026-07-11-p6-lanes-m1-spike-
    // findings.md Section 2.0).
    if (!lane_ownership_check_program(checker, program)) return 0;

    return checker->error_count == 0;
}

// stdlib Phase 0 (Task 4): type-check one imported package in its own scope,
// then publish its exported (A-Z) top-level symbols into pkg->exports.
//
// See the LIFETIME CONTRACT note on the declaration in types.h: on success this
// leaves the package scope pushed and current_package set so the caller can
// codegen the package (with cross-package name mangling) before tearing the
// scope down. This deliberately mirrors type_check_program's decl loop rather
// than calling it, because we must NOT re-run the lexer-error guard (already
// checked for the whole build) and must run inside the pushed package scope.
int type_check_package(TypeChecker* checker, Package* pkg, ASTNode* program) {
    if (!checker || !pkg || !program) return 0;

    // Push the package scope and set current_package FIRST, before any failable
    // work, so that EVERY return past this point leaves exactly one scope pushed
    // and current_package set — the caller unconditionally scope_pop()s and
    // clears current_package, so the push/pop must always balance.
    checker->current_package = pkg;
    scope_push(checker);

    if (program->type != AST_PROGRAM) {
        type_error(checker, program->pos, "Expected program node");
        return 0;
    }

    ProgramNode* prog = (ProgramNode*)program;

    // Mirror type_check_program's comptime pre-pass so an intra-package
    // `is_comptime` const RHS can resolve forward-declared calls.
    if (prog->decls && checker->comptime_type_ctx
                    && checker->comptime_type_ctx->comptime_ctx) {
        ComptimeContext* ctx = checker->comptime_type_ctx->comptime_ctx;
        for (ASTNode* d = prog->decls; d; d = d->next) {
            if (d->type == AST_FUNC_DECL) {
                FuncDeclNode* func = (FuncDeclNode*)d;
                if (func->name) {
                    comptime_context_bind_func(ctx, func->name, d);
                }
            }
        }
    }

    // P2.3/T1: mirror type_check_program's shell pre-pass so forward and
    // mutual struct/enum references resolve inside a vendored stdlib package
    // too — real upstream Go source relies on this pervasively.
    if (!declare_type_shells(checker, prog->decls)) {
        return 0;  // scope/current_package left set; caller aborts the build
    }

    // Two-pass declaration walk (same as type_check_program): register all
    // signatures in source order (pass 1), then check function bodies (pass 2),
    // so a package function may reference another regardless of declaration
    // order — the case that pervades real upstream Go source (e.g.
    // bits.LeadingZeros calls Len, defined far below it).
    for (ASTNode* decl = prog->decls; decl; decl = decl->next) {
        if (decl->type == AST_FUNC_DECL) {
            if (!declare_function_signature(checker, (FuncDeclNode*)decl)) {
                return 0;  // scope/current_package left set; caller aborts the build
            }
        } else if (!type_check_declaration(checker, decl)) {
            return 0;  // scope/current_package left set; caller aborts the build
        }
    }
    for (ASTNode* decl = prog->decls; decl; decl = decl->next) {
        if (decl->type == AST_FUNC_DECL) {
            if (!type_check_function_decl(checker, decl)) {
                return 0;  // scope/current_package left set; caller aborts the build
            }
        }
    }

    // Publish the package's exported (capitalised) top-level symbols so
    // cross-package selector resolution (Task 5) can reach them by name.
    package_export_filter(checker->current_scope, pkg->exports, pkg);

    return checker->error_count == 0;
}

int type_check_declaration(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl) return 0;
    
    switch (decl->type) {
        case AST_FUNC_DECL:
            return type_check_function_decl(checker, decl);
        case AST_VAR_DECL:
            return type_check_var_decl(checker, decl);
        case AST_CONST_DECL:
            return type_check_const_decl(checker, decl);
        case AST_TYPE_DECL:
            return type_check_type_decl(checker, decl);
        case AST_CONCEPT_DECL:
            return type_check_concept_decl(checker, decl);
        default:
            type_error(checker, decl->pos, "Unknown declaration type");
            return 0;
    }
}

// A result field is a *named* result (P3-5) iff its name is not one of the
// synthetic `_0`, `_1`, ... placeholders the parser assigns to anonymous
// tuple-return fields. Synthetic = `_` followed by one or more digits and
// nothing else.
int is_synthetic_result_name(const char* n) {
    if (!n || n[0] != '_' || n[1] == '\0') return 0;
    for (const char* p = n + 1; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

// Register one function's signature (its Type and a scope Variable) WITHOUT
// checking its body. Factored out of type_check_function_decl so the hoist
// pre-pass (hoist_function_signatures) can register every top-level plain
// function up front — letting bodies forward-reference functions declared later
// in the file or in a later file of the same package (Go's package-scope
// visibility: declaration order is irrelevant). A method is registered under
// its mangled name "T__m" so selector resolution (p.m) and the call site find
// it via a plain variable lookup; the receiver is params[0] (spliced by the
// parser), so func_type already carries it. Returns 1 on success, 0 on a
// duplicate definition in the current scope.
// Function generics Task 4: records into `seen[]` (indexed by type-param
// index) whether each type param appears anywhere in `t`. Used to enforce the
// Tier A "every type param must be inferable from a parameter type" rule —
// walked over every parameter's Type after the signature's param_types are
// built. Mirrors the union field names actually declared on Type (types.h):
// array.element_type, slice.element_type, map.key_type/value_type,
// pointer.pointee_type, function.param_types/param_count/return_type.
static void mark_type_params_used(const Type* t, int* seen, size_t n) {
    if (!t) return;
    switch (t->kind) {
        case TYPE_PARAM:
            if (t->data.type_param.index >= 0 &&
                (size_t)t->data.type_param.index < n)
                seen[t->data.type_param.index] = 1;
            return;
        case TYPE_SLICE:   mark_type_params_used(t->data.slice.element_type, seen, n); return;
        case TYPE_POINTER: mark_type_params_used(t->data.pointer.pointee_type, seen, n); return;
        case TYPE_ARRAY:   mark_type_params_used(t->data.array.element_type, seen, n); return;
        case TYPE_MAP:
            mark_type_params_used(t->data.map.key_type, seen, n);
            mark_type_params_used(t->data.map.value_type, seen, n);
            return;
        case TYPE_FUNCTION:
            for (size_t i = 0; i < t->data.function.param_count; i++)
                mark_type_params_used(t->data.function.param_types[i], seen, n);
            mark_type_params_used(t->data.function.return_type, seen, n);
            return;
        default: return;
    }
}

static int declare_function_signature(TypeChecker* checker, FuncDeclNode* func) {
    // v1: package-level `func init()` is a HARD REJECT with a clear message.
    // Before this guard the declaration compiled but the initializer was
    // SILENTLY never executed (Go runs it before main) — the worst
    // divergence class. Methods named init (func (t T) init()) stay legal,
    // as in Go. Real init support (declaration order, multiple inits,
    // imports-first) is post-v1; spec fixture pkg_init_func pins the reject.
    if (!func->receiver && func->name && strcmp(func->name, "init") == 0) {
        type_error(checker, func->base.pos,
                   "init functions are not supported in v1 (the body would "
                   "silently never run); initialize package-level vars in "
                   "their declarations or call a setup function from main");
        return 0;
    }

    size_t param_count = 0;
    for (ASTNode* p = func->params; p; p = p->next) {
        if (p->type == AST_VAR_DECL) param_count++;
    }

    // Function generics Task 3: push this function's type params BEFORE any
    // type_from_ast call below (param types, return type) so a bare `T` in
    // the signature (including inside `[]T`) resolves instead of erroring
    // "Unknown type 'T'". Popped on every return path of this function —
    // see the matching type_checker_pop_type_params before each `return`.
    size_t saved_tp = checker->tc_fctx.active_type_param_count;
    if (func->type_params) {
        int idx = 0;
        for (ASTNode* tp = func->type_params; tp; tp = tp->next) {
            VarDeclNode* g = (VarDeclNode*)tp;
            // Tier B: resolve the bound (constraint) once per group. It must be
            // an interface — a named interface, or `any` (the 0-method
            // interface). A non-interface bound is rejected here.
            Type* bound = g->type ? type_from_ast(checker, g->type) : NULL;
            if (!bound || bound->kind != TYPE_INTERFACE) {
                type_error(checker, func->base.pos,
                           "type constraint must be an interface");
                type_checker_pop_type_params(checker, saved_tp);
                return 0;
            }
            for (size_t i = 0; i < g->name_count; i++)
                type_checker_push_type_param(checker,
                    type_param(g->names[i], idx++, bound));
        }
    }

    // Comptime+generic composition (sub-project 2): the blanket "comptime
    // parameters are not yet supported together with type parameters" wall
    // that used to live here is LIFTED — a function may now declare BOTH
    // `[T]` type params and `comptime` value params (design doc, "Surface &
    // semantics"); `type_check_generic_call` (expression_checker.c) captures
    // the comptime axis at composed call sites, closing the exact gap this
    // wall used to guard against (a generic callee's comptime param falling
    // through as an ordinary runtime int with no compile-time-constant
    // requirement). What remains narrower: a comptime parameter's own
    // DECLARED type must be a plain concrete type — `comptime n T` (typed BY
    // a type parameter, or containing one, e.g. `comptime n []T`) is
    // rejected, because a value whose comptime evaluation depends on the
    // type axis is out of scope for this composition (design doc, decision
    // 1/5). Type params were already pushed above, so a bare `T` here
    // resolves to its TYPE_PARAM Type instead of "Unknown type '<name>'";
    // type_contains_type_param (expression_checker.c, exposed non-static for
    // this reuse) walks the resolved shape structurally rather than the
    // AST, so `comptime n []T` and `comptime n *T` are caught the same way.
    if (func->type_params) {
        for (ASTNode* p = func->params; p; p = p->next) {
            if (p->type != AST_VAR_DECL) continue;
            VarDeclNode* pd = (VarDeclNode*)p;
            if (!pd->is_comptime_param || !pd->type) continue;
            Type* pt = type_from_ast(checker, pd->type);
            if (pt && type_contains_type_param(pt)) {
                type_error(checker, p->pos,
                    "comptime parameter type cannot be a type parameter (not yet supported)");
                type_checker_pop_type_params(checker, saved_tp);
                return 0;
            }
        }
    }

    // Comptime value params Task 3 (fix round 1): comptime parameters are
    // rejected on METHOD declarations too, same shape as the generics
    // rejection just above. The monomorphization machinery (codegen.c's
    // skip-guard, monomorphize.c's worklist, call_codegen.c's rewiring) is
    // deliberately scoped to PLAIN functions for now — a comptime-param
    // method would instead take the ordinary single-emission path, where
    // the template body-check's placeholder binding (see the
    // is_comptime_param block in type_check_function_decl below) is baked
    // into any `[n]int` type PERMANENTLY: `func (s S) Fill(comptime n int)`
    // with `var buf [n]int` compiled clean and then panicked at runtime
    // with a 1-element array. Rejecting beats miscompiling. NOTE: this
    // deliberately supersedes the earlier Task 2-era behavior where a
    // comptime-param method's direct call "worked" (with n as a plain
    // runtime parameter) — method specialization is a documented follow-up
    // (reviewed controller decision, Task 3 fix round 1).
    if (func->receiver) {
        for (ASTNode* p = func->params; p; p = p->next) {
            if (p->type != AST_VAR_DECL) continue;
            VarDeclNode* pd = (VarDeclNode*)p;
            if (pd->is_comptime_param) {
                type_error(checker, p->pos,
                    "comptime parameters are not yet supported on methods");
                type_checker_pop_type_params(checker, saved_tp);
                return 0;
            }
        }
    }

    // P6 M1 (comptime-wall lift): comptime parameters on a package function
    // ARE now supported (the DECLARATION wall that used to reject them here is
    // gone). The three lifetime fronts the original wall guarded against are
    // each resolved: (a)/(c) a `pkg.Fill(4, ...)` selector call from importing
    // code resolves to the function's EXPORT COPY, which package_export_filter
    // now stamps with `func_decl_node` (the template, living on the surviving
    // PkgGraph AST) and `owner_pkg` (for package-qualified instance mangling,
    // `goo_pkg__<pkg>__<base>__n<v>` — comptime_instantiate, monomorphize.c),
    // so the main-pass monomorphizer resolves and emits the instance without
    // touching the torn-down package scope; (b) a SAME-package INTERNAL comptime
    // call — whose bare-name callee is the inner-scope Variable that scope_pop
    // frees before the main-pass worklist runs — is still rejected, but now
    // precisely at the recording site (expression_checker.c, gated on the
    // callee's NULL owner_pkg) rather than by banning the declaration outright.
    // The runtime-arg-across-packages wall (`must be a compile-time constant`)
    // is unchanged: it fires at the call site (type_check_capture_comptime_arg),
    // not here.

    // Task 2: a variadic parameter (`name ...T`) must be the LAST parameter
    // (Go: "can only use ... with final parameter in list"). Check this
    // BEFORE building param_types below — an earlier variadic param there
    // would otherwise just get its element type silently instead of []T,
    // producing a confusing downstream error instead of a clean rejection.
    {
        size_t idx = 0, last_idx = param_count - 1;
        for (ASTNode* p = func->params; p; p = p->next) {
            if (p->type != AST_VAR_DECL) continue;
            VarDeclNode* pd = (VarDeclNode*)p;
            if (pd->is_variadic_param && idx != last_idx) {
                type_error(checker, p->pos,
                           "variadic parameter must be the final parameter");
                type_checker_pop_type_params(checker, saved_tp);
                return 0;
            }
            idx++;
        }
    }

    Type** param_types = NULL;
    int is_variadic = 0;
    // Fix 2: does ANY parameter carry `comptime`? Walked alongside
    // param_types below (methods included — the receiver is never a
    // AST_VAR_DECL param here, so it can't itself be comptime) and stamped
    // onto func_type once built, below.
    int has_comptime_params = 0;
    if (param_count > 0) {
        param_types = calloc(param_count, sizeof(Type*));
        size_t idx = 0;
        for (ASTNode* p = func->params; p; p = p->next) {
            if (p->type != AST_VAR_DECL) continue;
            VarDeclNode* pd = (VarDeclNode*)p;
            Type* pt = pd->type ? type_from_ast(checker, pd->type)
                                : type_checker_get_builtin(checker, TYPE_INT32);
            // The BODY sees a variadic param as a slice of the element type
            // (Go's slice-sugar model — NOT C varargs); wrap it here so the
            // signature's param_types[last] is TYPE_SLICE of T. Call sites
            // key off func_type->data.function.is_variadic (set below) to
            // pack trailing args into that slice.
            if (pd->is_variadic_param && pt) {
                pt = type_slice(pt);
                is_variadic = 1;
            }
            if (pd->is_comptime_param) has_comptime_params = 1;
            param_types[idx++] = pt;
        }
    }

    // Function generics Task 4: enforce the Tier A generic-declaration
    // invariants once the signature's param_types are built (and the type
    // params are still pushed onto checker->tc_fctx.active_type_params from above).
    size_t tpn = 0;
    if (func->type_params) {
        tpn = checker->tc_fctx.active_type_param_count - saved_tp;
        // Every type param must appear in a parameter type (inference-only rule).
        int used[32] = {0};
        for (size_t i = 0; i < param_count; i++)
            mark_type_params_used(param_types[i], used, tpn);
        // recover the param-group names for the diagnostic
        {
            int idx = 0;
            for (ASTNode* tp = func->type_params; tp; tp = tp->next) {
                VarDeclNode* g = (VarDeclNode*)tp;
                for (size_t i = 0; i < g->name_count; i++, idx++) {
                    if (!used[idx]) {
                        type_error(checker, func->base.pos,
                            "type parameter %s is never used in a parameter; cannot be inferred",
                            g->names[i]);
                        type_checker_pop_type_params(checker, saved_tp);
                        return 0;
                    }
                }
            }
        }
    }

    Type* return_type = func->return_type
        ? type_from_ast(checker, func->return_type)
        : type_checker_get_builtin(checker, TYPE_VOID);
    // Fix round 6 (M-r5c): a declared return type that fails to resolve
    // (type_from_ast has already printed the positioned diagnostic — e.g.
    // "array length must be a constant expression" for `[n]int` in return
    // position, which is signature-scoped and cannot see the body's
    // comptime-param binding) is a pass-1 failure HERE, not a
    // register-with-NULL-return-and-continue: previously the function was
    // registered anyway and pass 2's body check re-resolved the same bad
    // node, printing the identical diagnostic a second time.
    if (func->return_type && !return_type) {
        type_checker_pop_type_params(checker, saved_tp);
        return 0;
    }

    Type* func_type = type_function(param_types, param_count, return_type);
    if (func_type) {
        func_type->data.function.is_variadic = is_variadic;
        func_type->data.function.has_comptime_params = has_comptime_params;
    }
    char* mangled = NULL;
    const char* reg_name = func->name;
    if (func->receiver) {
        VarDeclNode* recv = (VarDeclNode*)func->receiver;
        Type* recv_type = recv->type ? type_from_ast(checker, recv->type) : NULL;
        const char* tn = type_receiver_name(recv_type);
        if (tn) {
            mangled = type_method_mangled_name(tn, func->name);
            if (mangled) reg_name = mangled;
        }
    }
    Variable* func_var = variable_new(reg_name, func_type, func->base.pos);
    free(mangled);  // variable_new copied the name
    if (func_var) {
        func_var->is_initialized = 1;
        // Comptime value params Task 2: back-reference to this FuncDeclNode
        // for EVERY function/method (unlike generic_decl below, which stays
        // NULL for non-generic functions) — the call-argument checker needs
        // it to find each parameter's is_comptime_param flag.
        func_var->func_decl_node = (struct ASTNode*)func;
        // Function generics Task 4: mark this Variable as a generic template
        // so codegen (predeclare AND the declaration loop) can skip it — a
        // template is only ever emitted per concrete instantiation by the
        // monomorphizer (M3), never directly.
        if (func->type_params) {
            func_var->is_generic = 1;
            func_var->generic_decl = (struct ASTNode*)func;
            func_var->type_param_count = tpn;
        }
        if (!scope_add_variable(checker->current_scope, func_var)) {
            type_error(checker, func->base.pos, "Function '%s' already declared", func->name);
            variable_free(func_var);
            type_checker_pop_type_params(checker, saved_tp);
            return 0;
        }
    }
    type_checker_pop_type_params(checker, saved_tp);
    return 1;
}

// gofmt-syntax-b Task 2 (P1.6): structural-only pre-pass that records the
// name of every AST_LABEL_STMT reachable from `stmt` via statement-position
// children ONLY — it never follows an expression field, so it naturally
// never descends into a func-literal body (a closure can only be reached
// from a statement via an expression, e.g. a var-decl initializer or an
// expr-stmt), giving each function/literal its own independent label
// namespace for free, with no explicit boundary check needed. Populates
// checker->tc_fctx.goto_label_names (capped at 64, silently — T1's own label_count
// pass already reports "too many labels" for real overflows) so `goto`,
// checked later in the SAME normal walk this pre-pass runs ahead of, can
// see labels that are declared textually AFTER the goto (forward
// references, legal in Go). Deliberately does not report duplicate-label
// errors or do any type checking — that is unchanged, existing work done
// by the main walk's own AST_LABEL_STMT case (T1) at declaration order.
void type_check_collect_goto_labels(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt) return;
    switch (stmt->type) {
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)stmt;
            for (ASTNode* s = block->statements; s; s = s->next) {
                type_check_collect_goto_labels(checker, s);
            }
            return;
        }
        case AST_IF_STMT: {
            IfStmtNode* iff = (IfStmtNode*)stmt;
            type_check_collect_goto_labels(checker, iff->then_stmt);
            type_check_collect_goto_labels(checker, iff->else_stmt);
            return;
        }
        case AST_IF_LET_STMT: {
            IfLetStmtNode* il = (IfLetStmtNode*)stmt;
            type_check_collect_goto_labels(checker, il->then_stmt);
            type_check_collect_goto_labels(checker, il->else_stmt);
            return;
        }
        case AST_FOR_STMT: {
            ForStmtNode* f = (ForStmtNode*)stmt;
            type_check_collect_goto_labels(checker, f->body);
            return;
        }
        case AST_SWITCH_STMT: {
            SwitchStmtNode* sw = (SwitchStmtNode*)stmt;
            for (ASTNode* c = sw->cases; c; c = c->next) {
                CaseClauseNode* clause = (CaseClauseNode*)c;
                for (ASTNode* s = clause->body; s; s = s->next) {
                    type_check_collect_goto_labels(checker, s);
                }
            }
            return;
        }
        case AST_TYPE_SWITCH: {
            TypeSwitchNode* ts = (TypeSwitchNode*)stmt;
            for (ASTNode* c = ts->cases; c; c = c->next) {
                TypeCaseNode* clause = (TypeCaseNode*)c;
                for (ASTNode* s = clause->body; s; s = s->next) {
                    type_check_collect_goto_labels(checker, s);
                }
            }
            return;
        }
        case AST_SELECT_STMT: {
            SelectStmtNode* sel = (SelectStmtNode*)stmt;
            for (ASTNode* c = sel->cases; c; c = c->next) {
                SelectCaseNode* clause = (SelectCaseNode*)c;
                for (ASTNode* s = clause->body; s; s = s->next) {
                    type_check_collect_goto_labels(checker, s);
                }
            }
            return;
        }
        case AST_UNSAFE_STMT: {
            UnsafeStmtNode* u = (UnsafeStmtNode*)stmt;
            type_check_collect_goto_labels(checker, u->body);
            return;
        }
        case AST_ARENA_BLOCK: {
            // arena-goto fix: push this block onto the shared arena_chain
            // scratch stack (types.h doc comment) for the duration of the
            // body walk, so any AST_LABEL_STMT reached inside records this
            // block (and its ancestors) as part of its arena-nesting path.
            ArenaBlockNode* a = (ArenaBlockNode*)stmt;
            int pushed = checker->tc_fctx.arena_chain_depth < 16;
            if (pushed) {
                checker->tc_fctx.arena_chain[checker->tc_fctx.arena_chain_depth] = a;
                checker->tc_fctx.arena_chain_depth++;
            }
            type_check_collect_goto_labels(checker, a->body);
            if (pushed) checker->tc_fctx.arena_chain_depth--;
            return;
        }
        case AST_COMPTIME_BLOCK: {
            ComptimeBlockNode* c = (ComptimeBlockNode*)stmt;
            type_check_collect_goto_labels(checker, c->body);
            return;
        }
        case AST_LABEL_STMT: {
            LabelStmtNode* label = (LabelStmtNode*)stmt;
            int already = 0;
            for (size_t i = 0; i < checker->tc_fctx.goto_label_count; i++) {
                if (checker->tc_fctx.goto_label_names[i] && label->name &&
                    strcmp(checker->tc_fctx.goto_label_names[i], label->name) == 0) {
                    already = 1;
                    break;
                }
            }
            if (!already && checker->tc_fctx.goto_label_count < 64) {
                // arena-goto fix: snapshot the CURRENT arena_chain (this
                // label's arena-nesting path) alongside its name — see
                // types.h's goto_label_arena_chain doc comment. Same index
                // as the name just below, so goto_label_arena_depth[i]/
                // goto_label_arena_chain[i] always correspond to
                // goto_label_names[i].
                size_t idx = checker->tc_fctx.goto_label_count;
                size_t depth = checker->tc_fctx.arena_chain_depth;
                if (depth > 16) depth = 16;  // defensive; cannot happen (push caps at 16)
                for (size_t k = 0; k < depth; k++) {
                    checker->tc_fctx.goto_label_arena_chain[idx][k] = checker->tc_fctx.arena_chain[k];
                }
                checker->tc_fctx.goto_label_arena_depth[idx] = depth;
                checker->tc_fctx.goto_label_names[checker->tc_fctx.goto_label_count++] = label->name;
            }
            type_check_collect_goto_labels(checker, label->stmt);
            return;
        }
        default:
            return;  // no statement-position children to walk
    }
}

int type_check_function_decl(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl || decl->type != AST_FUNC_DECL) return 0;

    FuncDeclNode* func = (FuncDeclNode*)decl;

    // Function generics Task 3: push this function's type params before the
    // return-type lookup just below (may reference `T`) and before the body
    // is checked. Symmetric with declare_function_signature's push; popped
    // right before this function's own `return result` below.
    size_t saved_tp = checker->tc_fctx.active_type_param_count;
    if (func->type_params) {
        int idx = 0;
        for (ASTNode* tp = func->type_params; tp; tp = tp->next) {
            VarDeclNode* g = (VarDeclNode*)tp;
            // Tier B: resolve the bound so the body-check pass sees the same
            // constraint declare_function_signature already validated (and
            // pushed for the signature pass). The signature pass already
            // rejected a non-interface bound before the body is ever checked,
            // so resolving it again here cannot fail.
            Type* bound = g->type ? type_from_ast(checker, g->type) : NULL;
            for (size_t i = 0; i < g->name_count; i++)
                type_checker_push_type_param(checker,
                    type_param(g->names[i], idx++, bound));
        }
    }

    // Body-checking pass only. The function's signature (its scope Variable, and
    // for a method its receiver mangling) was already registered by pass 1 of
    // the declaration walk (declare_function_signature) — re-registering here
    // would report a forward-referenceable function as a duplicate of its own
    // entry. We only need the return type for the body's return context.
    Type* return_type = func->return_type
        ? type_from_ast(checker, func->return_type)
        : type_checker_get_builtin(checker, TYPE_VOID);

    // Create new scope for function
    scope_push(checker);
    // Closures Task 2: mark this as a function-boundary scope so a nested
    // func literal's identifier resolution (type_check_identifier,
    // expression_checker.c) can detect crossing OUT of it as a capture.
    checker->current_scope->is_function_boundary = 1;

    // Track the return type so context-sensitive builtins (e.g. error()) can
    // look it up without walking the AST upward.
    Type* saved_return_type = checker->current_return_type;
    checker->current_return_type = return_type;

    // Add function parameters to the function scope
    if (func->params) {
        ASTNode* param = func->params;
        while (param) {
            if (param->type == AST_VAR_DECL) {
                VarDeclNode* param_decl = (VarDeclNode*)param;
                
                // Get parameter type
                Type* param_type = NULL;
                if (param_decl->type) {
                    param_type = type_from_ast(checker, param_decl->type);
                } else {
                    param_type = type_checker_get_builtin(checker, TYPE_INT32); // Default type
                }
                // Task 2: the body sees a variadic param as []T, matching the
                // signature built by declare_function_signature above.
                if (param_decl->is_variadic_param && param_type) {
                    param_type = type_slice(param_type);
                }

                if (param_type) {
                    for (size_t i = 0; i < param_decl->name_count; i++) {
                        Variable* param_var = variable_new(param_decl->names[i], param_type, param_decl->base.pos);
                        if (param_var) {
                            param_var->is_initialized = 1; // Parameters are always initialized
                            // Closures Task 2: backref to the declaring
                            // VarDeclNode so a capture of this param can
                            // stamp is_captured for codegen's promotion pass.
                            param_var->decl_node = (struct ASTNode*)param_decl;

                            // Comptime value params Task 3: bind the SAME
                            // fields `comptime const` sets (type_check_const_decl,
                            // above) so goo_fold_const_int_ctx resolves this
                            // param inside the body — e.g. `var buf [n]int`'s
                            // length, or any other compile-time-constant use.
                            // This is the TEMPLATE body-check pass (run once,
                            // before any call site is known): a placeholder
                            // value is all that's needed here for type
                            // validity. The monomorphizer
                            // (codegen_generate_comptime_function_instance,
                            // monomorphize.c) rebinds this same field set to
                            // the REAL per-instance value on its own copy of
                            // this Variable during codegen, which is what
                            // the emitted array length/constant actually
                            // uses — this placeholder never reaches codegen.
                            if (param_decl->is_comptime_param) {
                                param_var->has_const_int_value = 1;
                                param_var->const_int_value = 1;
                                param_var->comptime_value = comptime_value_new(COMPTIME_VALUE_INT);
                                if (param_var->comptime_value) {
                                    param_var->comptime_value->int_value = 1;
                                }
                            }

                            scope_add_variable(checker->current_scope, param_var);
                        }
                    }
                }
            }
            param = param->next;
        }
    }

    // Named return parameters (P3-5): register each named result as a
    // zero-initialized in-scope local so the body can assign to it
    // (`x = ...`) and a bare `return` is valid. The parser encodes named
    // results as an inline StructTypeNode whose fields carry the user
    // names; anonymous tuple results use synthetic `_N` names (skipped).
    if (func->return_type && func->return_type->type == AST_STRUCT_TYPE) {
        StructTypeNode* st = (StructTypeNode*)func->return_type;
        for (ASTNode* f = st->fields; f; f = f->next) {
            if (f->type != AST_VAR_DECL) continue;
            VarDeclNode* fd = (VarDeclNode*)f;
            if (fd->name_count == 0 || !fd->names) continue;
            if (is_synthetic_result_name(fd->names[0])) continue;
            Type* ft = fd->type ? type_from_ast(checker, fd->type) : NULL;
            if (!ft) continue;
            Variable* rv = variable_new(fd->names[0], ft, fd->base.pos);
            if (rv) {
                rv->is_initialized = 1;  // zero-initialized per Go semantics
                // Closures Task 2: backref for capture stamping (see the
                // param loop above).
                rv->decl_node = (struct ASTNode*)fd;
                scope_add_variable(checker->current_scope, rv);
            }
        }
    }

    // Codegen-hardening R1-TC: labels/goto-labels are function-scoped —
    // start this function's body with empty registries (save/restore, not a
    // bare reset, so a func-literal body-check nested INSIDE another
    // function's body-check — reachable only via mutual recursion through
    // the AST, not in practice today, but symmetric with
    // current_return_type's own save/restore just above) can never leak
    // into or out of this function. tc_fctx_save snapshots the WHOLE
    // per-function scratch struct in one assignment (active_type_params is
    // ALREADY pushed above and untouched by tc_fctx_reset, so restoring it
    // here is a no-op — the real unwind is type_checker_pop_type_params
    // below); tc_fctx_reset zeroes label_count/goto_label_count only (see
    // its own doc comment, types.h) — arena_chain_depth needs no explicit
    // reset here, unlike at a func-literal boundary, since it is already 0
    // between sibling top-level functions.
    TcFunctionContext saved_tcfctx;
    tc_fctx_save(&saved_tcfctx, &checker->tc_fctx);
    tc_fctx_reset(&checker->tc_fctx);
    if (func->body) {
        type_check_collect_goto_labels(checker, func->body);
    }

    // gofmt-syntax-b Task 3: a function body starts OUTSIDE any switch/
    // select clause, regardless of what construct (if any) lexically
    // encloses this function declaration — save/restore mirrors
    // label_count's own independent-namespace convention directly above.
    FallthroughContext saved_fallthrough_ctx = checker->fallthrough_ctx;
    checker->fallthrough_ctx = FALLTHROUGH_CTX_NONE;

    // Type check function body
    int result = 1;
    if (func->body) {
        result = type_check_statement(checker, func->body);
    }

    // P2.4: missing-return analysis. Only a value-returning function needs
    // its body to end in a terminating statement (Go: a void function may
    // always fall off the end) — checked only once the body itself passed
    // ordinary type-checking, so a genuine type error inside the body is
    // reported instead of being masked by this diagnostic. Positioned at
    // func->body's own pos, which get_current_position() (parser_actions.c)
    // stamps at the `LBRACE statement_list RBRACE` reduction — i.e. at (or
    // immediately after) the function's closing brace. This makes codegen's
    // ret-zero fallback (function_codegen.c, "Add return if missing")
    // unreachable for well-typed user code — that fallback itself is
    // intentionally left in place (load-bearing for the terminator-blind
    // LLVM plumbing), not removed by this task.
    if (result && func->body && return_type && return_type->kind != TYPE_VOID) {
        if (!stmt_is_terminating(func->body)) {
            type_error(checker, func->body->pos, "missing return");
            result = 0;
        }
    }

    checker->fallthrough_ctx = saved_fallthrough_ctx;
    tc_fctx_restore(&checker->tc_fctx, &saved_tcfctx);
    checker->current_return_type = saved_return_type;
    scope_pop(checker);
    type_checker_pop_type_params(checker, saved_tp);
    return result;
}

// P4-3: does `concrete`'s method set satisfy interface `iface`? A concrete type
// implements an interface iff, for every interface method, it has a method
// registered under its mangled name "T__m" whose signature matches (params after
// the receiver, plus the return type). The empty interface is satisfied by every
// type. On failure returns 0 and writes the offending method name + reason to
// *method_out / *reason_out (for a clear "X does not implement Y" diagnostic).
int type_interface_satisfied(TypeChecker* checker, Type* iface,
                             Type* concrete, const char** method_out,
                             const char** reason_out) {
    if (!iface || iface->kind != TYPE_INTERFACE || !concrete) return 0;

    const char* tn = type_receiver_name(concrete);
    for (InterfaceMethod* im = iface->data.interface.methods; im; im = im->next) {
        // No nameable receiver type → cannot have methods → missing.
        if (!tn) { *method_out = im->name; *reason_out = "missing"; return 0; }

        char* mangled = type_method_mangled_name(tn, im->name);
        // P4.3 review-fix (MAJOR): a package-owned concrete's methods live in
        // its declaring package's exports, not the current scope — without
        // the owner-routed lookup, `kinds.Rect` could never satisfy
        // `kinds.Shaper` from main ("missing method Area"). Touches ONLY
        // where method existence is resolved; the receiver-kind method-set
        // rules below (P2.1) and the RTTI implementer collector are
        // deliberately untouched (see recv-kind collector coupling note).
        Variable* mv = mangled
            ? type_checker_lookup_method(checker, concrete, im->name, mangled)
            : NULL;
        free(mangled);
        Type* impl = NULL;
        int via_embed = 0;
        int embed_via_pointer = 0;
        if (!mv || !mv->type || mv->type->kind != TYPE_FUNCTION) {
            // Not directly declared — promoted method via embedding? Also
            // through a POINTER to a struct: Go's *Outer method set includes
            // Outer's promoted methods (the #109 pair, part 2 — safe only now
            // that codegen boxes pointer concretes correctly; before that fix
            // this gate was the shield between users and the miscompile).
            Type* embed_root = concrete;
            if (embed_root->kind == TYPE_POINTER &&
                embed_root->data.pointer.pointee_type &&
                embed_root->data.pointer.pointee_type->kind == TYPE_STRUCT) {
                embed_root = embed_root->data.pointer.pointee_type;
            }
            Type* impl_via_embed = NULL;
            if (embed_root->kind == TYPE_STRUCT) {
                EmbedResult er = embedding_resolve(checker, embed_root, im->name);
                if (er.kind == EMBED_METHOD) {
                    impl_via_embed = er.type;
                    via_embed = 1;
                    embed_via_pointer = er.via_pointer;
                }
            }
            if (!impl_via_embed) {
                *method_out = im->name; *reason_out = "missing"; return 0;
            }
            impl = impl_via_embed;
        } else {
            impl = mv->type;
        }

        // Fix 2 (comptime-param functions are not first-class values): a
        // method with any `comptime` parameter cannot satisfy an interface
        // method — interface dispatch calls (`d.Do(x)`) never resolve a
        // concrete checked_callee, so type_check_call_expr's per-argument
        // comptime-constant check (which walks checked_callee->
        // func_decl_node) is never reached for them. Without this gate a
        // comptime-param method structurally satisfied a plain interface
        // method (matched by Type equality alone — is_comptime_param lives
        // only on the AST) and a runtime value reached the comptime slot in
        // total silence. reason_out="comptime" is a sentinel every caller of
        // type_interface_satisfied special-cases for a dedicated diagnostic
        // instead of the generic "does not implement" message. Fix round 2
        // note: since declare_function_signature now rejects comptime
        // parameters on method DECLARATIONS outright, this gate is
        // currently an unreachable backstop — kept so interface
        // satisfaction stays safe on its own terms if method declarations
        // are ever re-admitted (the method-specialization follow-up).
        if (impl->kind == TYPE_FUNCTION && impl->data.function.has_comptime_params) {
            *method_out = im->name; *reason_out = "comptime"; return 0;
        }

        // Receiver-kind soundness (Go method-set rule): a pointer-receiver
        // method is in the method set of *T only, not value T. Direct: reject a
        // value concrete for a pointer-receiver method. Embedded: a value outer
        // still holds the promoted method iff the embedding path crossed a
        // pointer field (embed_via_pointer). A pointer concrete is always fine.
        int concrete_is_ptr = (concrete->kind == TYPE_POINTER);
        int method_is_ptr_recv =
            impl->data.function.param_count >= 1 &&
            impl->data.function.param_types[0] &&
            impl->data.function.param_types[0]->kind == TYPE_POINTER;
        if (method_is_ptr_recv && !concrete_is_ptr &&
            (!via_embed || !embed_via_pointer)) {
            *method_out = im->name;
            *reason_out = "pointer-receiver";
            return 0;
        }

        // The registered method carries the receiver as params[0]; the interface
        // method's function type has no receiver. So a match requires
        // impl.param_count == want.param_count + 1 and the tails to be equal.
        Type* want = im->type;
        size_t want_params = want ? want->data.function.param_count : 0;
        if (impl->data.function.param_count != want_params + 1) {
            *method_out = im->name; *reason_out = "signature mismatch"; return 0;
        }
        for (size_t k = 0; k < want_params; k++) {
            if (!type_equals(impl->data.function.param_types[k + 1],
                             want->data.function.param_types[k])) {
                *method_out = im->name; *reason_out = "signature mismatch"; return 0;
            }
        }
        Type* want_ret = want ? want->data.function.return_type : NULL;
        if (want_ret && want_ret->kind != TYPE_VOID &&
            !type_equals(impl->data.function.return_type, want_ret)) {
            *method_out = im->name; *reason_out = "signature mismatch"; return 0;
        }
    }
    return 1;  // every method satisfied (or empty interface)
}

// Fix 2: see the declaration in types.h. One place for the
// comptime-method-can't-satisfy-an-interface wording so all three
// type_interface_satisfied callers (check_interface_assign below, the
// call-argument interface check in expression_checker.c, and the type-switch
// case check above) report it identically instead of falling through to
// their own generic "X does not implement Y (reason method M)" message,
// which would otherwise render the "comptime" sentinel as if it were a
// method name ("... (comptime method Do)").
void report_comptime_method_not_satisfied(TypeChecker* checker, Position pos,
                                          const char* method) {
    type_error(checker, pos,
        "method '%s' has comptime parameters and cannot satisfy an interface method",
        method ? method : "?");
}

// Fix 2: see the declaration in types.h. Deliberately does NOT walk into
// call->function — every caller of this helper is a VALUE-consuming site
// (var-decl init, assignment RHS, return expression, a non-callee call
// argument); the callee position of a direct call is checked separately
// (type_check_call_expr's existing checked_callee->func_decl_node walk) and
// must keep accepting a flagged type there.
int reject_comptime_function_value(TypeChecker* checker, ASTNode* src_expr,
                                   Type* t, Position pos, const char* context) {
    if (!t || t->kind != TYPE_FUNCTION || !t->data.function.has_comptime_params)
        return 1;
    const char* name = NULL;
    if (src_expr) {
        if (src_expr->type == AST_IDENTIFIER) {
            name = ((IdentifierNode*)src_expr)->name;
        } else if (src_expr->type == AST_SELECTOR_EXPR) {
            // A bound method value (`s.Fill`, no call) — the selector IS the
            // method name.
            name = ((SelectorExprNode*)src_expr)->selector;
        }
    }
    type_error(checker, pos,
        "function '%s' has comptime parameters and cannot be %s",
        name ? name : type_to_string(t), context ? context : "used as a value");
    return 0;
}

// P4-3: assignability into an interface-typed target. When `target` is an
// interface, accept iff `src` is that interface (or a concrete implementer);
// otherwise fall back to ordinary type_compatible. Emits the implementation
// diagnostic itself on failure (returns 0). `pos` anchors the error.
int check_interface_assign(TypeChecker* checker, Type* src, Type* target,
                           Position pos) {
    if (!target || target->kind != TYPE_INTERFACE) {
        return type_compatible(src, target);
    }

    // P2.8 FIX F1 (cascade-suppression completeness): a poisoned source
    // (bound to a previously failed declaration — see register_declared_
    // names_after_failure) must not spawn a SECOND diagnostic here. Single
    // choke point: every check_interface_assign caller (var-decl,
    // multi-assign, call arguments, struct/map/array-literal elements,
    // index-assign, select-case bind) routes through this one function, so
    // guarding this entry covers all of them uniformly — mirrors the
    // binary-op choke point's silent-pass pattern.
    if (src && type_is_poison(src)) return 1;
    // F3 fix: bare nil literal (TYPE_UNKNOWN, the sentinel type_check_literal
    // gives TOKEN_NIL) is Go's SIXTH nilable kind — interface — alongside
    // the five type_is_nilable_ref_kind covers. Must be short-circuited
    // BEFORE the does-not-implement check below, or `var i I = nil` /
    // `i = nil` are wrongly routed into type_interface_satisfied as if nil
    // were a concrete type ("nil does not implement I (missing method M)").
    // Return position already got this right via its own separate TYPE_
    // UNKNOWN guard (type_checker.c's return-statement check, above this
    // function); this brings var-init/assignment/struct-and-map-literal-
    // field/index-assign/type-switch-case (every check_interface_assign
    // caller) into agreement. Codegen already boxes a TYPE_UNKNOWN concrete
    // to the zero {NULL,NULL} interface value (codegen_interface_box's own
    // TYPE_UNKNOWN guard) — this is purely a type-check routing fix, no
    // codegen change needed.
    if (src && src->kind == TYPE_UNKNOWN) return 1;
    if (src && src->kind == TYPE_INTERFACE) return 1;  // interface→interface (v1: permissive)

    const char* method = NULL;
    const char* reason = NULL;
    if (type_interface_satisfied(checker, target, src, &method, &reason)) return 1;

    if (reason && strcmp(reason, "comptime") == 0) {
        report_comptime_method_not_satisfied(checker, pos, method);
        return 0;
    }
    const char* iname = target->data.interface.name ? target->data.interface.name
                                                    : "interface";
    const char* cname = src ? type_receiver_name(src) : NULL;
    type_error(checker, pos, "%s does not implement %s (%s method %s)",
               cname ? cname : type_to_string(src), iname,
               reason ? reason : "missing", method ? method : "?");
    return 0;
}

// Task 3 (constant representability): bridge into expression_checker.c's
// untyped-constant-literal adapters — see that file's adapt_var_decl_
// initializer doc comment for why a var-decl initializer needs its own
// entry point (it doesn't already route through the adapters the way a
// binary-expr operand, struct-literal field, or call argument does).
// Forward-declared here with its own `extern` prototype instead of via a
// shared header — this task's constraint set excludes header/parser
// changes, and this is the only external caller.
extern int adapt_var_decl_initializer(TypeChecker* checker, ASTNode* value, Type* declared);

// Registers a single var-decl name in scope, mirroring the bindings the
// success path has always set (ownership, is_initialized). Both the
// success path and the Task 3 failure-recovery path (below) construct a
// Variable for a var-decl name the same way, so this is the one place
// that does it. `emit_errors` controls whether a construction/
// redeclaration failure reports a diagnostic: the success path wants its
// existing "already declared" message, but the failure-recovery caller
// (register_declared_names_after_failure) must NOT add a new error on top
// of an already-failing declaration — that would defeat the point of the
// recovery.
static int bind_var_decl_name(TypeChecker* checker, VarDeclNode* var_decl,
                               const char* name, Type* type,
                               int is_initialized, int emit_errors) {
    // The blank identifier `_` is a discard, never a binding: it is never
    // registered in scope, so it may repeat freely in one scope
    // (`_, a := f(); _, b := g()`) and can never be read back as a value. Go
    // special-cases `_` this way; without this guard the second `_` collided
    // with the first ("Variable '_' already declared in this scope").
    if (name && strcmp(name, "_") == 0) {
        return 1;
    }

    Variable* var = variable_new(name, type, var_decl->base.pos);
    if (!var) {
        if (emit_errors) {
            type_error(checker, var_decl->base.pos, "Memory allocation failed");
        }
        return 0;
    }

    var->ownership = var_decl->ownership;
    var->is_initialized = is_initialized;
    // Closures Task 2: backref to the declaring VarDeclNode (see the
    // function/literal param registration sites for the same convention) —
    // this is the SINGLE registration path for every ordinary local/global
    // var-decl, including multi-name (`a, b := f()`) decls, which share one
    // VarDeclNode across all of their names.
    var->decl_node = (struct ASTNode*)var_decl;

    if (!scope_add_variable(checker->current_scope, var)) {
        if (emit_errors) {
            type_error(checker, var_decl->base.pos,
                      "Variable '%s' already declared in this scope", name);
        }
        variable_free(var);
        return 0;
    }
    return 1;
}

// Task 3 (stop the error cascade after a rejected declaration): a var-decl
// with an EXPLICIT type (`var b int8 = 300`) that fails type-checking its
// initializer still binds `b` in scope with the declared type before the
// caller returns failure. Without this, every downstream reference to `b`
// also fails as "Undefined variable 'b'" — one real error (the overflow)
// fans out into a cascade (see examples/cascade_reject.goo, and the
// cascade-reject-probe Makefile target). Compilation still fails overall
// (every caller of this still returns 0 right after); this only keeps the
// checker's scope state sane for the rest of the pass so later real
// errors — or their absence — are reported accurately instead of drowned
// out. Registration failures (OOM, genuine redeclaration) are swallowed
// via emit_errors=0: the declaration is already failing, and a failed
// recovery attempt should never inject a NEW error into that failure.
//
// P2.8 T4.2 (cascade suppression): a `:=` short decl has no explicit type to
// fall back on when its RHS fails — the task-3 report's `:=` residual this
// comment used to describe. Register the TYPE_POISON marker instead of
// nothing: the name resolves as a known variable for the rest of the pass
// (no more "Undefined variable" cascade), and the poison-aware choke points
// (type_check_binary_expr) propagate it silently instead of re-erroring.
// This never changes the ROOT diagnostic — that already fired above, and
// this function only runs afterward, on the recovery path — and never
// reaches codegen, since the failure already dooms type_check_program.
static void register_declared_names_after_failure(TypeChecker* checker,
                                                    VarDeclNode* var_decl,
                                                    Type* declared_type) {
    Type* fallback_type = declared_type
        ? declared_type
        : type_checker_get_builtin(checker, TYPE_POISON);
    if (!fallback_type) return;
    for (size_t i = 0; i < var_decl->name_count; i++) {
        bind_var_decl_name(checker, var_decl, var_decl->names[i],
                            fallback_type, 1, /*emit_errors=*/0);
    }
}

int type_check_var_decl(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl || decl->type != AST_VAR_DECL) return 0;

    VarDeclNode* var_decl = (VarDeclNode*)decl;

    // Get declared type
    Type* declared_type = NULL;
    if (var_decl->type) {
        declared_type = type_from_ast(checker, var_decl->type);
        if (!declared_type) {
            type_error(checker, var_decl->base.pos, "Invalid type in variable declaration");
            return 0;
        }
    }

    // Check initial values
    Type* inferred_type = NULL;
    if (var_decl->values) {
        // P2.8 T4.2 (cascade suppression): capture the error count BEFORE
        // checking the initializer so the generic "Invalid initializer
        // expression" wrapper below only fires when type_check_expression
        // returned NULL WITHOUT itself reporting anything specific (a
        // defensive fallback, so a silent failure is never left completely
        // undiagnosed). The common case — type_check_expression already
        // printed a precise cause ("Undefined variable 'f'", "Cannot call
        // non-function type", etc.) — must not ALSO get this generic
        // wrapper stacked on top of it: that redundant second line for the
        // SAME failure is exactly the kind of noise the recon's cascade
        // probe (`x := undefinedFn(); y := x + 1; println(y)`) counts
        // against "exactly one diagnostic" once y/println no longer cascade
        // on their own account (see the poison-registration branch below).
        int errors_before = checker->error_count;
        inferred_type = type_check_expression(checker, var_decl->values);
        if (!inferred_type) {
            if (checker->error_count == errors_before) {
                type_error(checker, var_decl->base.pos, "Invalid initializer expression");
            }
            // Task 3: an explicit-type decl (`var b T = <bad-rhs>`) still
            // binds `b:T` so downstream uses don't cascade into "Undefined
            // variable". A `:=` decl (declared_type == NULL here) has
            // nothing to fall back on: type_check_expression returned no
            // type at all, so there is no sound type to register — this
            // is the `:=` residual recorded in the task-3 report.
            register_declared_names_after_failure(checker, var_decl, declared_type);
            return 0;
        }
        // Fix 2 (comptime-param functions are not first-class values): a
        // comptime-parameterized function captured into a variable — typed
        // (`var f func(int,int) int = fill`) OR inferred (`f := fill`) —
        // strips away the func_decl_node back-reference that
        // type_check_call_expr's per-argument comptime-constant check relies
        // on (a var-decl'd Variable is never built from a FuncDeclNode), so a
        // later call through `f` would silently accept a runtime argument
        // into the comptime slot. Reject the capture itself, before that
        // Variable is ever bound. Checked before adapt_var_decl_initializer
        // below since that path is numeric-only and would no-op on a
        // function type anyway.
        if (!reject_comptime_function_value(checker, var_decl->values, inferred_type,
                                            var_decl->base.pos, "used as a value")) {
            register_declared_names_after_failure(checker, var_decl, declared_type);
            return 0;
        }
        // Task 3: reject an unrepresentable literal constant (`var b int8 =
        // 300`) BEFORE the compatibility check below, which permits int<->int
        // and int->float width mismatches unconditionally — only codegen's
        // later width-coerce step would otherwise narrow the value, and it
        // truncates/wraps rather than rejecting. declared_type may still be
        // NULL here (a `:=` short decl with no annotation) — nothing sized to
        // check against in that case.
        if (declared_type) {
            if (!adapt_var_decl_initializer(checker, var_decl->values, declared_type)) {
                // Task 3: this is the cascade probe's exact failure path
                // (`var b int8 = 300`; examples/cascade_reject.goo) —
                // register `b:int8` before failing so `fmt.Println(int(b))`
                // and `c := b` on later lines don't cascade.
                register_declared_names_after_failure(checker, var_decl, declared_type);
                return 0; // range violation; adapt_var_decl_initializer already emitted the error
            }
            // Adaptation may have re-stamped the initializer's node_type to a
            // narrower/wider type than the checker-default INT64/FLOAT64
            // (e.g. `var b int8 = -128` narrows to INT8) — read it back so
            // the compatibility check below sees the adapted type. A non-
            // adapted initializer (a typed variable/call RHS) is untouched,
            // so this is a no-op in that case.
            if (var_decl->values->node_type) inferred_type = var_decl->values->node_type;
        }
    }

    // Determine final type
    Type* final_type = declared_type;
    if (!final_type) {
        final_type = inferred_type;
    } else if (inferred_type) {
        // P2.8 FIX F1 (cascade-suppression completeness): an initializer
        // bound to a previously failed declaration (see
        // register_declared_names_after_failure — e.g. `y := f(); var x int
        // = y`) must not spawn a SECOND diagnostic here. There's no sound
        // value to compare against declared_type anyway, so accept it
        // silently: final_type stays declared_type (set above), skipping
        // both compatibility branches below.
        if (type_is_poison(inferred_type)) {
            // fall through: bind as declared_type, no diagnostic.
        } else if (declared_type->kind == TYPE_INTERFACE) {
            // Check compatibility. An interface-typed target accepts any
            // concrete implementer (P4-3); check_interface_assign emits its
            // own diagnostic.
            if (!check_interface_assign(checker, inferred_type, declared_type,
                                        var_decl->base.pos)) {
                register_declared_names_after_failure(checker, var_decl, declared_type);
                return 0;
            }
        } else if (!type_compatible(inferred_type, declared_type)) {
            // Fix round 6 (M-r5b): `var b [4]int = a` with a comptime-length
            // initializer — same length deferral as assignment
            // (type_check_assignment_op) and call arguments
            // (type_check_call_expr): the template-time length is the
            // placeholder, so compare only element types now; codegen's
            // var-decl init path enforces the real per-instance lengths
            // (instance-named rejection on a genuine mismatch). Ordinary
            // array initializers reject here exactly as before.
            int comptime_len_deferred =
                inferred_type->kind == TYPE_ARRAY &&
                declared_type->kind == TYPE_ARRAY &&
                (inferred_type->data.array.comptime_length ||
                 declared_type->data.array.comptime_length) &&
                type_equals(inferred_type->data.array.element_type,
                            declared_type->data.array.element_type);
            if (!comptime_len_deferred) {
                // P2.8 T4.3: the same remedy hint as the unhandled-!T check
                // below, appended here too — a raw !T assigned to a
                // mismatched CONCRETE declared type (`var n int = f()`, f()
                // returning !int) hits this generic incompatibility branch
                // rather than that one (final_type never becomes the error
                // union here — declared_type wins), so it needs its own
                // copy of the hint to point the user at try/catch/destructure
                // instead of leaving them to puzzle out a bare type mismatch.
                if (inferred_type->kind == TYPE_ERROR_UNION) {
                    type_error(checker, var_decl->base.pos,
                              "Cannot assign %s to %s — error union must be "
                              "handled: use try, catch, or v, err := destructuring",
                              type_to_string(inferred_type),
                              type_to_string(declared_type));
                } else {
                    type_error(checker, var_decl->base.pos,
                              "Cannot assign %s to %s",
                              type_to_string(inferred_type),
                              type_to_string(declared_type));
                }
                register_declared_names_after_failure(checker, var_decl, declared_type);
                return 0;
            }
        }
    }

    if (!final_type) {
        type_error(checker, var_decl->base.pos,
                  "Variable declaration must have either type or initializer");
        return 0;
    }

    // P2.8 T4.3 (unhandled error union at the binding): a single-name
    // binding WITH AN INITIALIZER whose FINAL type is still the raw error
    // union means the RHS was neither `try` nor `catch` (both unwrap to the
    // value type before final_type is computed above) nor a 2-name
    // destructure (that path is name_count==2, handled separately below, and
    // never reassigns final_type away from TYPE_ERROR_UNION even on its OWN
    // success — so this check must stay gated on name_count==1 or it would
    // misfire on a perfectly legitimate `v, err := f()`). Gated on
    // var_decl->values so a declare-only `var x !int` (no RHS at all — a
    // zero-initialized union, the Go-style zero-value idiom, and not what
    // the design doc's "RHS is not try/catch/destructure" phrasing targets)
    // is left alone. Verified pre-fix: the WITH-initializer form bound
    // SILENTLY, with no diagnostic at all — every later use of the name
    // would need its own unwrap that never got enforced.
    if (var_decl->name_count == 1 && var_decl->values &&
        final_type->kind == TYPE_ERROR_UNION) {
        type_error(checker, var_decl->base.pos,
                  "error union must be handled: use try, catch, or v, err := destructuring");
        // Poison rather than bind the raw union: a value the checker never
        // required anyone to unwrap must not silently flow into later
        // arithmetic/selector checks either (see TYPE_POISON's doc comment).
        register_declared_names_after_failure(checker, var_decl, NULL);
        return 0;
    }

    // comma-ok map read: `v, ok := m[k]` — synthesize {V, bool} so the
    // multi-LHS loop below binds name0→V and name1→bool. The single-LHS
    // path `v := m[k]` (name_count==1) is untouched and still yields bare V.
    if (var_decl->name_count == 2 && var_decl->is_short_decl &&
        var_decl->values && var_decl->values->type == AST_INDEX_EXPR) {
        // The base expression was already type-checked above; read back the
        // resolved type from the AST node instead of re-checking.
        ASTNode* base_expr = ((IndexExprNode*)var_decl->values)->expr;
        Type* base_type = base_expr->node_type;
        if (base_type && base_type->kind == TYPE_MAP) {
            Type* commaok_struct = type_new(TYPE_STRUCT);
            if (commaok_struct) {
                commaok_struct->data.struct_type.fields = calloc(2, sizeof(StructField));
                if (commaok_struct->data.struct_type.fields) {
                    commaok_struct->data.struct_type.field_count = 2;
                    commaok_struct->data.struct_type.name = NULL;
                    commaok_struct->data.struct_type.fields[0].name = strdup("v");
                    commaok_struct->data.struct_type.fields[0].type = base_type->data.map.value_type;
                    commaok_struct->data.struct_type.fields[0].offset = 0;
                    commaok_struct->data.struct_type.fields[0].ownership = OWNERSHIP_NONE;
                    commaok_struct->data.struct_type.fields[0].mutability = MUTABILITY_MUTABLE;
                    commaok_struct->data.struct_type.fields[1].name = strdup("ok");
                    commaok_struct->data.struct_type.fields[1].type = type_checker_get_builtin(checker, TYPE_BOOL);
                    commaok_struct->data.struct_type.fields[1].offset = 0;
                    commaok_struct->data.struct_type.fields[1].ownership = OWNERSHIP_NONE;
                    commaok_struct->data.struct_type.fields[1].mutability = MUTABILITY_MUTABLE;
                    final_type = commaok_struct;
                } else {
                    type_free(commaok_struct);
                }
            }
        }
    }

    // Task 2 of type assertions: comma-ok `v, ok := x.(T)` — sibling of the
    // comma-ok map read above, same {V, bool} synthesis. The single-value
    // result type was already computed and stamped by type_check_expression's
    // AST_TYPE_ASSERT case (called above via the initial
    // `type_check_expression(checker, var_decl->values)`); read it back
    // instead of re-checking, exactly as the map arm reads back base_type.
    if (var_decl->name_count == 2 && var_decl->is_short_decl &&
        var_decl->values && var_decl->values->type == AST_TYPE_ASSERT) {
        Type* v_type = var_decl->values->node_type;
        // Interface-target RTTI, Task 2: `v, ok := x.(I)` where I is an
        // interface now type-checks like any other comma-ok assertion — the
        // {v, bool} synthesis below is target-shape-agnostic (v's type is
        // whatever node_type resolved to, concrete or interface). Task 1
        // rejected this case here because its codegen counterpart
        // (function_codegen.c's comma-ok arm) only knew how to call
        // codegen_interface_assert_match (concrete targets only); Task 2's
        // codegen arm now branches on target->kind and calls
        // codegen_interface_target_match for an interface target instead,
        // so the reject-here guard that used to live in this spot is no
        // longer needed.
        if (v_type) {
            Type* commaok_struct = type_new(TYPE_STRUCT);
            if (commaok_struct) {
                commaok_struct->data.struct_type.fields = calloc(2, sizeof(StructField));
                if (commaok_struct->data.struct_type.fields) {
                    commaok_struct->data.struct_type.field_count = 2;
                    commaok_struct->data.struct_type.name = NULL;
                    commaok_struct->data.struct_type.fields[0].name = strdup("v");
                    commaok_struct->data.struct_type.fields[0].type = v_type;
                    commaok_struct->data.struct_type.fields[0].offset = 0;
                    commaok_struct->data.struct_type.fields[0].ownership = OWNERSHIP_NONE;
                    commaok_struct->data.struct_type.fields[0].mutability = MUTABILITY_MUTABLE;
                    commaok_struct->data.struct_type.fields[1].name = strdup("ok");
                    commaok_struct->data.struct_type.fields[1].type = type_checker_get_builtin(checker, TYPE_BOOL);
                    commaok_struct->data.struct_type.fields[1].offset = 0;
                    commaok_struct->data.struct_type.fields[1].ownership = OWNERSHIP_NONE;
                    commaok_struct->data.struct_type.fields[1].mutability = MUTABILITY_MUTABLE;
                    final_type = commaok_struct;
                } else {
                    type_free(commaok_struct);
                }
            }
        }
    }

    // comma-ok channel receive: `v, ok := <-ch` — sibling of the map/type-
    // assert arms above, same {V, bool} synthesis. `<-ch` is AST_UNARY_EXPR
    // with operator TOKEN_ARROW (parser.y: `ARROW unary_expr`); its already-
    // type-checked node_type (set by type_check_channel_receive_op via the
    // initial `type_check_expression(checker, var_decl->values)` above) is
    // just the channel's element type — read it back instead of re-checking,
    // exactly as the map/type-assert arms do. Without this synthesis,
    // final_type stays the bare element type and BOTH names bind to it below
    // (per_name_types stays NULL), so `ok` silently gets the element type
    // instead of bool (Task 5: this is the type-level half of the
    // double-goo_chan_recv miscompile — the codegen half is the comma-ok arm
    // in function_codegen.c).
    if (var_decl->name_count == 2 && var_decl->is_short_decl &&
        var_decl->values && var_decl->values->type == AST_UNARY_EXPR &&
        ((UnaryExprNode*)var_decl->values)->operator == TOKEN_ARROW) {
        Type* elem_type = var_decl->values->node_type;
        if (elem_type) {
            Type* commaok_struct = type_new(TYPE_STRUCT);
            if (commaok_struct) {
                commaok_struct->data.struct_type.fields = calloc(2, sizeof(StructField));
                if (commaok_struct->data.struct_type.fields) {
                    commaok_struct->data.struct_type.field_count = 2;
                    commaok_struct->data.struct_type.name = NULL;
                    commaok_struct->data.struct_type.fields[0].name = strdup("v");
                    commaok_struct->data.struct_type.fields[0].type = elem_type;
                    commaok_struct->data.struct_type.fields[0].offset = 0;
                    commaok_struct->data.struct_type.fields[0].ownership = OWNERSHIP_NONE;
                    commaok_struct->data.struct_type.fields[0].mutability = MUTABILITY_MUTABLE;
                    commaok_struct->data.struct_type.fields[1].name = strdup("ok");
                    commaok_struct->data.struct_type.fields[1].type = type_checker_get_builtin(checker, TYPE_BOOL);
                    commaok_struct->data.struct_type.fields[1].offset = 0;
                    commaok_struct->data.struct_type.fields[1].ownership = OWNERSHIP_NONE;
                    commaok_struct->data.struct_type.fields[1].mutability = MUTABILITY_MUTABLE;
                    final_type = commaok_struct;
                } else {
                    type_free(commaok_struct);
                }
            }
        }
    }

    // Store the type on the AST node for code generation
    var_decl->base.node_type = final_type;

    // Multi-LHS short var decl `a, b := f()` — RHS must produce a
    // TYPE_STRUCT with at least name_count fields, and each name
    // binds to the corresponding field's type. Codegen does the
    // ExtractValue destructuring; the type checker just records
    // each binding with the right per-field type.
    //
    // Guarded on var_decl->values (mirrors codegen_generate_var_decl's
    // identical guard in function_codegen.c): without it, a plain
    // no-initializer multi-name decl of a struct TYPE (`var p, q P` — no
    // RHS to destructure at all) was misread as a destructure, binding p/q
    // to P's FIELD types instead of P itself. Caught by multivar_probe.goo's
    // `var p, q P` case (decl-surface breadth task 1).
    Type** per_name_types = NULL;
    if (var_decl->name_count > 1 && final_type && final_type->kind == TYPE_STRUCT &&
        var_decl->values) {
        if (final_type->data.struct_type.field_count >= var_decl->name_count) {
            per_name_types = malloc(sizeof(Type*) * var_decl->name_count);
            if (per_name_types) {
                for (size_t i = 0; i < var_decl->name_count; i++) {
                    per_name_types[i] = final_type->data.struct_type.fields[i].type;
                }
            }
        }
    } else if (var_decl->name_count == 2 && var_decl->is_short_decl &&
               final_type && final_type->kind == TYPE_ERROR_UNION) {
        // Go-style error-union destructure: `n, err := <!T>`. name0 binds the
        // unwrapped value arm; name1 binds `error` — the same nullable pointer
        // (`?*int8`) that `error` resolves to in type_from_ast (see :1538), so
        // `err != nil` type-checks. Without this both names would bind to the
        // whole !T and the nil-compare would reject as `!int vs nil`.
        per_name_types = malloc(sizeof(Type*) * 2);
        if (per_name_types) {
            per_name_types[0] = final_type->data.error_union.value_type;
            per_name_types[1] = type_checker_error_type(checker);
        }
    }

    // Add variables to scope
    // Variables with an explicit type are zero-initialized at declaration
    // (Go-style default-value semantics) — even when no explicit
    // initializer expression is supplied. Without this, `var p Point`
    // would read as uninitialized when its fields are later accessed,
    // even though the struct's bytes are zeroed by the alloca.
    int is_initialized = (var_decl->values != NULL) || (declared_type != NULL);
    for (size_t i = 0; i < var_decl->name_count; i++) {
        Type* t = per_name_types ? per_name_types[i] : final_type;
        if (!bind_var_decl_name(checker, var_decl, var_decl->names[i], t,
                                 is_initialized, /*emit_errors=*/1)) {
            free(per_name_types);
            return 0;
        }
    }
    
    return 1;
}

int type_check_const_decl(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl || decl->type != AST_CONST_DECL) return 0;
    
    ConstDeclNode* const_decl = (ConstDeclNode*)decl;
    
    // Constants must have values
    if (!const_decl->values) {
        type_error(checker, const_decl->base.pos, "Constant declaration must have initializer");
        return 0;
    }
    
    // Type check the value
    Type* value_type = type_check_expression(checker, const_decl->values);
    if (!value_type) {
        return 0;
    }
    
    // Check declared type if present
    Type* declared_type = NULL;
    if (const_decl->type) {
        declared_type = type_from_ast(checker, const_decl->type);
        if (!declared_type) {
            return 0;
        }
        
        if (!type_compatible(value_type, declared_type)) {
            type_error(checker, const_decl->base.pos,
                      "Cannot assign %s to %s",
                      type_to_string(value_type),
                      type_to_string(declared_type));
            return 0;
        }

        // Arc 5 (h): a TYPED integer const decl had NO representability
        // gate — type_compatible's any-int laxness accepted `const k int8 =
        // 300`, and the two codegen materialization paths then disagreed
        // about the value (a local const emitted at the fold's default
        // width and printed 300, the declared type ignored; a package const
        // emitted at the declared width and silently wrapped to 44). var
        // decls already rejected the literal shape (adapt_untyped_int_
        // operand's range check) — const decls were the asymmetric hole.
        // Judge the FOLDED RHS against the declared type via the shared
        // core (see check_const_int_expr_fits): unlike the chan-send gate
        // there is no kind-difference precondition, because `const k int64
        // = 18446744073709551615` is a same-kind, value-level hole (the
        // same bare-huge-literal rule the return gate enforces). Not-
        // applicable (0) — non-integer target, unfoldable RHS, comptime-
        // param-tainted — falls through unchanged.
        if (check_const_int_expr_fits(checker, const_decl->values,
                                      declared_type) < 0) {
            return 0;
        }
        value_type = declared_type;
    }

    // Compile-time integer constant folding: an untyped const whose RHS is a
    // pure integer constant expression takes its type from the folded value's
    // magnitude. Without this `const m = 1<<32 - 1` would be int32 (the width-
    // naive bitwise-op result type), but 4294967295 doesn't fit int32 — so a
    // later `var v uint64 = m` would wrongly see an int32. Matches the value
    // codegen emits (function_codegen.c folds the same expression).
    if (!const_decl->type) {
        uint64_t folded;
        if (goo_fold_const_int(const_decl->values, &folded)) {
            // Untyped int const default type is `int` (int64 here); a value past
            // int64's signed range takes uint64. Mirrors function_codegen.c.
            // The fold is 64-bit MODULAR, so a raw pattern > INT64_MAX is
            // ambiguous: a huge literal actually written >= 2^63 (MaxUint64)
            // or a genuinely NEGATIVE constant (`-5` folds to 0xFFFF...FB).
            // Disambiguate with the negated-shape check, the same convention
            // int_const_fits_expected's uint64 arm documents: negative-rooted
            // folds keep the signed default (int64 holding -5), only an
            // unnegated huge fold takes uint64. Same residual deviation as
            // there: pure-arithmetic negatives with no top-level minus
            // (`0 - 5`) still read as huge-positive and take uint64.
            value_type = (folded <= 9223372036854775807ULL ||
                          is_negated_int_const_expr(const_decl->values))
                             ? type_checker_get_builtin(checker, TYPE_INT64)
                             : type_checker_get_builtin(checker, TYPE_UINT64);
        }
    }

    // M11-types-const-integrate: if the const is marked `comptime`,
    // evaluate the RHS through the comptime engine and attach the
    // result to the Variable so codegen can read it (M11-codegen-const).
    // Engine call uses the raw context directly per the spike's
    // recommendation (lesson-1778811668-591661). Engine failure is
    // non-fatal here: function-call RHS will return null-value until
    // M11-engine-recursion lands, and codegen will fail at its
    // LLVMIsConstant check — same failure mode as before, just one
    // dispatch layer deeper.
    ComptimeValue* comptime_val = NULL;
    if (const_decl->is_comptime && checker->comptime_type_ctx
                                && checker->comptime_type_ctx->comptime_ctx) {
        ComptimeContext* raw_ctx = checker->comptime_type_ctx->comptime_ctx;
        ComptimeResult* res = comptime_eval_expression(raw_ctx, const_decl->values);
        if (res && res->value && !res->error) {
            comptime_val = comptime_value_copy(res->value);
        }
        if (res) comptime_result_free(res);
    }

    // fix/const-array-length: fold the RHS through the checker-aware folder
    // (identifiers matter — `const M = N + 1` should fold if N is already a
    // cached const) and cache the result on every Variable this decl
    // introduces, so a later array-length reference (`[N]int`, `[M]int`) can
    // resolve the real length instead of the placeholder-10 fallback. This is
    // independent of (and does not replace) the goo_fold_const_int call
    // above, which only decides int64-vs-uint64 for an untyped const's Type.
    uint64_t const_int_value = 0;
    int has_const_int_value = goo_fold_const_int_ctx(checker, const_decl->values, &const_int_value);

    // Add constants to scope (treated as immutable variables)
    for (size_t i = 0; i < const_decl->name_count; i++) {
        Variable* var = variable_new(const_decl->names[i], value_type, const_decl->base.pos);
        if (!var) {
            if (comptime_val) comptime_value_free(comptime_val);
            return 0;
        }

        var->mutability = MUTABILITY_IMMUTABLE;
        var->is_initialized = 1;
        var->has_const_int_value = has_const_int_value;
        var->const_int_value = const_int_value;
        if (comptime_val) {
            // First variable takes ownership of the original copy;
            // subsequent variables get their own copies. The multi-name
            // case (`comptime const X, Y int = 42`) is rare but legal.
            var->comptime_value = (i == 0) ? comptime_val : comptime_value_copy(comptime_val);
        }

        if (!scope_add_variable(checker->current_scope, var)) {
            type_error(checker, const_decl->base.pos,
                      "Constant '%s' already declared in this scope", const_decl->names[i]);
            variable_free(var);
            return 0;
        }
    }

    return 1;
}

// P2.3/T1: the pre-pass hoisted out of this function's own former inline
// shell-creation code (both type_check_program and type_check_package run it
// over the full decl list before pass 1 touches any body — see their
// declare_type_shells(checker, prog->decls) calls). Registering every
// top-level struct/enum shell up front, before ANY body resolves, is what
// makes forward references (`type A struct { b *B }` before `type B`) and
// mutual pairs (A<->B pointer fields, either declaration order) resolve:
// type_from_ast finds every sibling name already in scope no matter which
// decl is being processed.
//
// Duplicate top-level struct/enum names are caught HERE, on the second
// registration attempt — the identical diagnostic and codepath
// type_check_type_decl used to produce on its own (now-removed) inline
// shell-creation duplicate check, so existing reject fixtures are unaffected
// by moving the check earlier.
static int declare_type_shells(TypeChecker* checker, ASTNode* decls) {
    for (ASTNode* decl = decls; decl; decl = decl->next) {
        if (decl->type != AST_TYPE_DECL) continue;
        TypeDeclNode* td = (TypeDeclNode*)decl;
        if (!td->name || !td->type) continue;  // tolerate malformed, mirrors type_check_type_decl

        ASTNodeType body_kind = td->type->type;
        if (body_kind != AST_STRUCT_TYPE && body_kind != AST_ENUM_TYPE) continue;

        Type* shell = type_new(body_kind == AST_ENUM_TYPE ? TYPE_ENUM : TYPE_STRUCT);
        if (!shell) return 0;
        // Do NOT set shell->...name here; type_check_type_decl's tail stamping
        // runs after the tie-the-knot copy and sets the name on resolved(=shell).
        // Setting it now would cause a double-free: *shell=*resolved overwrites
        // the pointer with resolved's (possibly NULL) name, leaking our strdup.
        Variable* fwd = variable_new(td->name, shell, decl->pos);
        if (!fwd) { free(shell); return 0; }
        fwd->is_initialized = 1;
        fwd->is_builtin = 1;
        if (!scope_add_variable(checker->current_scope, fwd)) {
            type_error(checker, decl->pos, "Type '%s' already declared", td->name);
            variable_free(fwd);
            return 0;
        }
    }
    return 1;
}

int type_check_type_decl(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl || decl->type != AST_TYPE_DECL) return 0;

    TypeDeclNode* td = (TypeDeclNode*)decl;
    if (!td->name || !td->type) return 1;  // tolerate malformed

    // Forward-declare a shell for struct/enum bodies so that self-referential
    // pointer fields (e.g. `next *Node` inside `type Node struct{...}`) can
    // resolve the name during type_from_ast. Plain aliases keep the old flow.
    ASTNodeType body_kind = td->type->type;
    Type* shell = NULL;
    if (body_kind == AST_STRUCT_TYPE || body_kind == AST_ENUM_TYPE) {
        // P2.3/T1: declare_type_shells (the pre-pass every caller of this
        // function runs first — type_check_program / type_check_package,
        // the ONLY two callers of type_check_declaration, which is the ONLY
        // caller of type_check_type_decl) already registered this decl's
        // shell before pass 1 started. Reuse it — a direct walk of THIS
        // scope's own variable list only (never scope_lookup_variable's
        // parent-chasing walk, which could find an unrelated outer-scope
        // same-named type and corrupt it via the tie-the-knot copy below).
        // Every earlier decl in pass 1 must already have succeeded (pass 1
        // aborts the whole walk on its first failure) and no later decl has
        // run yet, so the only variable this lookup can find under this
        // exact name is our own pre-registered shell.
        for (Variable* v = checker->current_scope->variables; v; v = v->next) {
            if (strcmp(v->name, td->name) == 0) { shell = v->type; break; }
        }
        if (!shell) {
            // Defensive fallback (should be unreachable given the invariant
            // above): behaves exactly like the pre-T1 self-reference-only
            // primitive, in case some future caller reaches this function
            // without having run the pre-pass first.
            shell = type_new(body_kind == AST_ENUM_TYPE ? TYPE_ENUM : TYPE_STRUCT);
            if (!shell) return 0;
            Variable* fwd = variable_new(td->name, shell, decl->pos);
            if (!fwd) { free(shell); return 0; }
            fwd->is_initialized = 1;
            fwd->is_builtin = 1;
            if (!scope_add_variable(checker->current_scope, fwd)) {
                type_error(checker, decl->pos, "Type '%s' already declared", td->name);
                variable_free(fwd);
                return 0;
            }
        }
    }

    // Resolve the right-hand-side type expression to a concrete Type.
    // Self-referential pointers built inside type_from_ast will point to
    // shell (already in scope); after the tie-the-knot copy below, those
    // pointers will see the complete definition.
    Type* resolved = type_from_ast(checker, td->type);
    if (!resolved) {
        type_error(checker, decl->pos, "Cannot resolve type for '%s'", td->name);
        return 0;
    }

    if (shell) {
        // Tie the knot: copy the complete definition into the shell, preserving
        // the shell's address (already captured by self-referential pointers).
        // free() releases only the Type box wrapper; nested allocations now
        // belong to the shell. The name field is intentionally NOT set on shell
        // before this copy to avoid leaking a strdup'd pointer: *shell=*resolved
        // overwrites whatever was in shell, and the tail stamping below will
        // set the name on resolved(=shell) via the existing `if (!...name)`
        // guards, which handle the case where type_from_ast left name NULL.
        *shell = *resolved;
        free(resolved);
        resolved = shell;
    }

    // Stamp the declared name onto a struct type so methods and call sites
    // can recover it for name mangling (the resolved Type is shared via the
    // registered variable below, so this propagates to every use).
    if (resolved->kind == TYPE_STRUCT && !resolved->data.struct_type.name) {
        resolved->data.struct_type.name = strdup(td->name);
    }

    // Stamp the declared name onto an enum type, then register each variant
    // constructor name in scope so variant literals can resolve their parent enum.
    if (resolved->kind == TYPE_ENUM) {
        if (!resolved->data.enum_type.name)
            resolved->data.enum_type.name = strdup(td->name);
        for (size_t i = 0; i < resolved->data.enum_type.variant_count; i++) {
            const char* vname = resolved->data.enum_type.variants[i].name;
            Variable* ctor = variable_new(vname, resolved, decl->pos);
            if (!ctor) return 0;
            ctor->is_initialized = 1;
            ctor->is_builtin = 1;
            if (!scope_add_variable(checker->current_scope, ctor)) {
                // A duplicate variant name across enums is a conflict; report.
                type_error(checker, decl->pos,
                           "Enum variant '%s' already declared", vname);
                variable_free(ctor);
                return 0;
            }
        }
    }

    // Stamp the declared name onto an interface type so satisfaction
    // diagnostics read "Sq does not implement Shape" rather than "interface".
    if (resolved->kind == TYPE_INTERFACE && !resolved->data.interface.name) {
        resolved->data.interface.name = strdup(td->name);
    }

    // Stamp the declared name onto a named NON-struct/enum/interface type
    // (e.g. `type IntSlice []int`, `type MyInt int`). Those kinds carry no
    // kind-specific name field, so use the generic Type.name — which
    // type_receiver_name() already falls back to — enabling method mangling
    // (`IntSlice__M`), selector dispatch, and interface boxing on named types.
    //
    // type_slice/type_array/type_map pre-populate ->name with an internal
    // descriptor (e.g. "[]int") rather than NULL, so we cannot use !->name as
    // a guard. Shared scalar builtins (TYPE_INT32 etc.) are returned as
    // singletons by type_checker_get_builtin; mutating them would corrupt the
    // type table. We distinguish fresh allocations from shared singletons by
    // pointer identity:
    //   - resolved != builtin → fresh compound allocation (TYPE_SLICE/ARRAY/MAP
    //     are never registered in builtin_types; their slot is NULL from the
    //     memset in type_checker_init_builtins, so type_checker_get_builtin
    //     returns NULL and this branch always fires for them). Stamp in place.
    //   - resolved == builtin → shared scalar singleton. Clone it first so the
    //     singleton is not mutated, then stamp the clone's name and redirect
    //     resolved to the clone so downstream alias-registration uses the clone.
    if (resolved->kind != TYPE_STRUCT && resolved->kind != TYPE_ENUM &&
        resolved->kind != TYPE_INTERFACE) {
        Type* builtin = type_checker_get_builtin(checker, resolved->kind);
        if (resolved != builtin) {
            // Fresh compound allocation — stamp in place.
            free(resolved->name);
            resolved->name = strdup(td->name);
        } else {
            // Shared scalar singleton — clone to avoid corrupting the type table.
            Type* named_clone = type_copy(resolved);
            if (!named_clone) {
                type_error(checker, decl->pos, "Out of memory cloning type for '%s'", td->name);
                return 0;
            }
            free(named_clone->name);
            named_clone->name = strdup(td->name);
            resolved = named_clone;
        }
    }

    // P4.3 (packages-B): stamp the owning package onto the final Type object
    // (whichever path produced it above — the tied-knot shell, an in-place
    // compound stamp, or the cloned scalar singleton) so cross-package method
    // resolution can find it later (type_receiver_owner_package). NULL
    // (checker->current_package unset) for every type declared while
    // checking main — byte-identical to today's behavior for the no-import
    // path and for main's own types.
    resolved->owner_package = checker->current_package;

    // Register the named type alias only when we did NOT forward-declare a
    // shell (shell == NULL). When a shell was pre-registered above, td->name
    // is already bound in scope to resolved(=shell); re-registering would
    // conflict. For plain aliases (AST_BASIC_TYPE etc.), register as before.
    if (shell == NULL) {
        // a Variable whose `type` IS the named Type. AST_BASIC_TYPE lookup
        // in type_from_ast will recover it. This is a pragmatic shortcut
        // until a proper named-types table exists.
        Variable* alias = variable_new(td->name, resolved, decl->pos);
        if (!alias) return 0;
        alias->is_initialized = 1;
        alias->is_builtin = 1;  // not a real variable for use-tracking purposes
        if (!scope_add_variable(checker->current_scope, alias)) {
            type_error(checker, decl->pos, "Type '%s' already declared", td->name);
            variable_free(alias);
            return 0;
        }
    }
    return 1;
}

int type_check_concept_decl(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl || decl->type != AST_CONCEPT_DECL) return 0;
    
    ConceptDeclNode* concept = (ConceptDeclNode*)decl;
    
    // For now, just register the concept in the global scope
    // TODO: Implement full concept validation and constraint checking
    
    // Create a concept type to represent this concept definition
    Type* concept_type = type_concept(concept->name);
    Variable* concept_var = variable_new(concept->name, concept_type, concept->base.pos);
    if (concept_var) {
        concept_var->is_initialized = 1;
        if (!scope_add_variable(checker->current_scope, concept_var)) {
            type_error(checker, concept->base.pos, "Concept '%s' already declared", concept->name);
            variable_free(concept_var);
            return 0;
        }
    }
    
    return 1;
}

// F6: `a, b := v1, v2` / `a, b = v1, v2`. Two passes so RHS is evaluated in
// the pre-assignment scope (Go semantics): pass 1 type-checks every value
// (before any `:=` name comes into scope); pass 2 binds new names (`:=`) or
// validates assignment compatibility against existing lvalues (`=`).
int type_check_multi_assign(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_MULTI_ASSIGN) return 0;
    MultiAssignNode* ma = (MultiAssignNode*)stmt;

    Type* vtypes[2];   // v1 grammar produces exactly count==2
    size_t n = 0;
    for (ASTNode* v = ma->values; v && n < ma->count && n < 2; v = v->next) {
        Type* vt = type_check_expression(checker, v);
        if (!vt) return 0;
        // Fix 2 (comptime-param functions are not first-class values):
        // `a, b := fill, 1` / `a, b = fill, 1` capture fill's VALUE into a
        // Variable with no func_decl_node — the same alias bypass the
        // single-name var-decl / assignment guards close. Gated here on the
        // VALUE expression, before the target loop binds/stores anything.
        // A destructure RHS (`a, b = f()`) is a CALL value, whose own callee
        // was already checked by type_check_call_expr — its result type is
        // never a flagged function type unless `return fill` slipped out,
        // which the return-statement guard already rejects at f's decl.
        if (!reject_comptime_function_value(checker, v, vt, v->pos,
                                            "used as a value")) {
            return 0;
        }
        vtypes[n++] = vt;
    }

    // Destructuring assignment `a, b = f()`: a SINGLE multi-return value whose
    // result is a struct is spread across the targets (the call returns a
    // struct of the result types). Expand the one struct value into per-field
    // value types so the target loop binds/stores each field.
    if (n == 1 && ma->count == 2 && vtypes[0] &&
        vtypes[0]->kind == TYPE_STRUCT &&
        vtypes[0]->data.struct_type.field_count >= 2) {
        Type* s = vtypes[0];
        vtypes[0] = s->data.struct_type.fields[0].type;
        vtypes[1] = s->data.struct_type.fields[1].type;
        n = 2;
    }

    size_t i = 0;
    for (ASTNode* t = ma->targets; t; t = t->next, i++) {
        if (i >= n) {
            type_error(checker, stmt->pos,
                       "Multiple assignment: more targets than values");
            return 0;
        }
        Type* vt = vtypes[i];

        if (ma->is_short_decl) {
            if (t->type != AST_IDENTIFIER) {
                type_error(checker, t->pos, "Left side of := must be an identifier");
                return 0;
            }
            t->node_type = vt;
            // `_` is a discard, never bound (mirrors bind_var_decl_name), so it
            // can repeat and can never be read back as a value.
            if (strcmp(((IdentifierNode*)t)->name, "_") == 0) {
                continue;
            }
            Variable* var = variable_new(((IdentifierNode*)t)->name, vt, t->pos);
            if (var) {
                var->is_initialized = 1;
                // Tolerate redeclaration in the same scope (Go permits := when
                // at least one name is new); an existing binding keeps its slot.
                if (!scope_add_variable(checker->current_scope, var)) {
                    variable_free(var);
                }
            }
        } else {
            // Blank identifier `_` as a multi-assign target discards its value
            // (`_, a = f()`, `_, _ = f()`): it is not an lvalue to resolve, so
            // skip it — mirrors the single `_ = rhs` discard path. Without this
            // the target loop type-checks `_` as an expression and wrongly
            // reports "Undefined variable '_'".
            if (t->type == AST_IDENTIFIER &&
                strcmp(((IdentifierNode*)t)->name, "_") == 0) {
                continue;
            }
            // The grammar accepts any expression as a tuple-assign target;
            // enforce addressability here (mirrors type_check_assignment_op).
            // Lvalues are identifiers, index, selector, and deref (`*p`).
            if (t->type != AST_IDENTIFIER && t->type != AST_INDEX_EXPR &&
                t->type != AST_SELECTOR_EXPR &&
                !(t->type == AST_UNARY_EXPR &&
                  ((UnaryExprNode*)t)->operator == TOKEN_MULTIPLY)) {
                type_error(checker, t->pos,
                           "cannot assign to a non-addressable expression");
                return 0;
            }
            Type* tt = type_check_expression(checker, t);
            if (!tt) return 0;
            // P2.8 FIX F1 (cascade-suppression completeness): a value bound
            // to a previously failed declaration (see register_declared_
            // names_after_failure) must not spawn a SECOND diagnostic here.
            // There's no sound value to compare against tt anyway, so
            // accept it silently and move to the next target.
            if (type_is_poison(vt)) {
                continue;
            }
            // An interface-typed target accepts any concrete implementer
            // (P4-3) and any interface (permissive). check_interface_assign
            // emits its own "X does not implement Y" diagnostic; for non-
            // interface targets it falls back to type_compatible.
            if (tt->kind == TYPE_INTERFACE) {
                if (!check_interface_assign(checker, vt, tt, t->pos)) {
                    return 0;
                }
            } else if (!type_compatible(vt, tt)) {
                type_error(checker, t->pos,
                           "Cannot assign %s to %s in multiple assignment",
                           type_to_string(vt), type_to_string(tt));
                return 0;
            }
        }
    }
    return 1;
}

// gofmt-syntax-b Task 1, extracted for review Fix 5b: register one label in
// the function-wide label table (duplicate-checked against every label seen
// so far, capped at 64 — labels are function-scoped in Go, so the SAME name
// used twice anywhere in one function is a duplicate even across unrelated
// sibling blocks, not merely shadowing, which Go doesn't apply to labels at
// all). Returns 1 on success, 0 (with a type_error already emitted) on
// failure. Shared by type_check_statement's AST_LABEL_STMT case (the normal
// path) and type_check_switch_like_body's clause-final labeled-fallthrough
// unwrap below (Fix 5b) so both register labels identically — duplicating
// this bookkeeping inline in both places would be exactly the kind of
// two-copies-that-drift risk that caused Task 1's own label_stmt de-merge
// analysis to need forensic reconstruction (see conflict-ledger.md).
static int type_check_register_label(TypeChecker* checker, char* name, Position pos) {
    for (size_t i = 0; i < checker->tc_fctx.label_count; i++) {
        if (checker->tc_fctx.label_names[i] && name &&
            strcmp(checker->tc_fctx.label_names[i], name) == 0) {
            type_error(checker, pos, "duplicate label '%s'", name);
            return 0;
        }
    }
    if (checker->tc_fctx.label_count >= 64) {
        type_error(checker, pos, "too many labels in one function (max 64)");
        return 0;
    }
    checker->tc_fctx.label_names[checker->tc_fctx.label_count] = name;
    checker->tc_fctx.label_positions[checker->tc_fctx.label_count] = pos;
    checker->tc_fctx.label_count++;
    return 1;
}

// gofmt-syntax-b Task 3 (P1.7): shared clause-body walker for the three
// switch-like constructs that host case-clause bodies (expression switch,
// type switch, select). `fallthrough` is legal ONLY when `kind` is
// FALLTHROUGH_CTX_EXPR_SWITCH, only as the LITERAL final statement of the
// body being walked here — not merely "somewhere in this clause": a
// fallthrough followed by more statements in the same body is caught
// below (`s->next != NULL`); one buried inside a nested block/if that is
// itself a direct statement of this body is caught by the
// AST_FALLTHROUGH_STMT case in type_check_statement instead, via the
// fallthrough_ctx this function sets for the duration of the walk (see
// that case's comment) — and only when `is_last_clause` is false (Go: the
// switch's final clause has nothing to fall through TO).
static int type_check_switch_like_body(TypeChecker* checker, ASTNode* body,
                                        FallthroughContext kind, int is_last_clause) {
    FallthroughContext saved = checker->fallthrough_ctx;
    checker->fallthrough_ctx = kind;
    int ok = 1;
    for (ASTNode* s = body; s; s = s->next) {
        if (s->type == AST_FALLTHROUGH_STMT) {
            if (kind != FALLTHROUGH_CTX_EXPR_SWITCH) {
                type_error(checker, s->pos,
                    kind == FALLTHROUGH_CTX_TYPE_SWITCH
                        ? "fallthrough statement is not permitted in a type switch"
                        : "fallthrough statement is not permitted in a select statement");
                ok = 0;
            } else if (s->next != NULL) {
                type_error(checker, s->pos,
                    "fallthrough statement must be the final statement in a case clause");
                ok = 0;
            } else if (is_last_clause) {
                type_error(checker, s->pos, "cannot fallthrough final case in switch");
                ok = 0;
            }
            continue;
        }
        // review Fix 5b: `L: fallthrough` (or multiply-nested
        // `L1: L2: fallthrough`) as a clause's LITERAL final statement is
        // valid Go — the label exists only so an earlier `goto L` inside
        // the SAME clause body can target it (unused labels are otherwise
        // rejected). A non-mutating peek first: chase the AST_LABEL_STMT
        // chain to see whether it bottoms out in AST_FALLTHROUGH_STMT
        // WITHOUT touching checker->label_* yet, so a chain that turns out
        // NOT to be fallthrough-terminated falls through unchanged to the
        // ordinary `type_check_statement(checker, s)` call below (which
        // registers it itself) with no risk of double-registering the same
        // label and raising a bogus "duplicate label" error.
        if (s->type == AST_LABEL_STMT) {
            ASTNode* inner = s;
            while (inner->type == AST_LABEL_STMT) {
                inner = ((LabelStmtNode*)inner)->stmt;
                if (!inner) break;
            }
            if (inner && inner->type == AST_FALLTHROUGH_STMT) {
                // Confirmed fallthrough-terminated: now it's safe to
                // register every label in the chain (the ordinary
                // type_check_statement path below is skipped for this
                // node via `continue`, so there is no double-registration
                // risk) and apply THIS walker's own final-statement rules
                // to the fallthrough directly. Routing the chain through
                // type_check_statement instead would recurse into ITS
                // AST_FALLTHROUGH_STMT case, which has no way to know the
                // label wrapping it is this walker's own direct, final
                // child — it would unconditionally reject with "must be
                // the final statement" (see that case's comment), which is
                // exactly the bug this unwrap fixes.
                for (ASTNode* lbl = s; lbl != inner; ) {
                    LabelStmtNode* label = (LabelStmtNode*)lbl;
                    if (!type_check_register_label(checker, label->name, lbl->pos)) ok = 0;
                    lbl = label->stmt;
                }
                if (kind != FALLTHROUGH_CTX_EXPR_SWITCH) {
                    type_error(checker, inner->pos,
                        kind == FALLTHROUGH_CTX_TYPE_SWITCH
                            ? "fallthrough statement is not permitted in a type switch"
                            : "fallthrough statement is not permitted in a select statement");
                    ok = 0;
                } else if (s->next != NULL) {
                    type_error(checker, inner->pos,
                        "fallthrough statement must be the final statement in a case clause");
                    ok = 0;
                } else if (is_last_clause) {
                    type_error(checker, inner->pos, "cannot fallthrough final case in switch");
                    ok = 0;
                }
                continue;
            }
        }
        if (!type_check_statement(checker, s)) ok = 0;
    }
    checker->fallthrough_ctx = saved;
    return ok;
}

int type_check_statement(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt) return 0;
    switch (stmt->type) {
        case AST_BLOCK_STMT:
            return type_check_block_stmt(checker, stmt);
        case AST_EXPR_STMT:
            return type_check_expr_stmt(checker, stmt);
        case AST_VAR_DECL:
            return type_check_var_decl(checker, stmt);
        case AST_CONST_DECL:
            // Local const inside a function body (`const n = 64`). Same checker
            // as a package-level const; the enclosing function scope holds it.
            return type_check_const_decl(checker, stmt);
        case AST_MULTI_ASSIGN:
            return type_check_multi_assign(checker, stmt);
        case AST_IF_STMT:
            return type_check_if_stmt(checker, stmt);
        case AST_IF_LET_STMT: {
            IfLetStmtNode* il = (IfLetStmtNode*)stmt;
            Type* nt = type_check_expression(checker, il->nullable_expr);
            if (!nt) return 0;
            if (nt->kind != TYPE_NULLABLE) {
                type_error(checker, stmt->pos, "if-let requires a nullable expression");
                return 0;
            }
            Type* inner = nt->data.nullable.base_type;
            scope_push(checker);
            if (il->var_name && inner) {
                Variable* v = variable_new(il->var_name, inner, stmt->pos);
                if (v) { v->is_initialized = 1; scope_add_variable(checker->current_scope, v); }
            }
            int ok = il->then_stmt ? type_check_statement(checker, il->then_stmt) : 1;
            scope_pop(checker);
            if (il->else_stmt) ok = ok && type_check_statement(checker, il->else_stmt);
            return ok;
        }
        case AST_FOR_STMT:
            return type_check_for_stmt(checker, stmt);
        case AST_RETURN_STMT:
            return type_check_return_stmt(checker, stmt);
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
            return 1;  // Always valid; loop-nesting is a codegen-time check
        case AST_BREAK_LABEL_STMT:
        case AST_CONTINUE_LABEL_STMT:
            // Whether the label exists AND encloses this break/continue is a
            // codegen-time check (stack walk over the pushed loop/break-scope
            // frames) — mirrors the bare-break/continue precedent directly
            // above, which likewise defers "outside loop" to codegen.
            return 1;
        case AST_FALLTHROUGH_STMT:
            // gofmt-syntax-b Task 3 (P1.7): reached only when `fallthrough`
            // is NOT the direct, final statement of a switch/type-switch/
            // select clause body — that position is special-cased in
            // type_check_switch_like_body above, which never recurses into
            // type_check_statement for such a node. Two remaining shapes
            // land here: truly outside any switch/select construct
            // (fallthrough_ctx == NONE), or nested one level deeper —
            // inside an if/for/block that is itself a direct statement of
            // a clause body (fallthrough_ctx still set, inherited through
            // the nested recursive dispatch). Go gives the nested shape
            // its own distinct wording ("fallthrough statement out of
            // place"); this folds it into the same "not final statement"
            // diagnostic family instead — nested is a species of "not
            // literally final" — matching the design's 5-way split (last
            // clause / not-last-statement / type switch / select /
            // outside switch), which type_check_switch_like_body's own
            // three error sites cover the rest of.
            if (checker->fallthrough_ctx == FALLTHROUGH_CTX_NONE) {
                type_error(checker, stmt->pos, "fallthrough statement outside switch");
            } else {
                type_error(checker, stmt->pos,
                    "fallthrough statement must be the final statement in a case clause");
            }
            return 0;
        case AST_LABEL_STMT: {
            // gofmt-syntax-b Task 1: registration extracted to
            // type_check_register_label (see its doc comment) — shared with
            // type_check_switch_like_body's clause-final labeled-fallthrough
            // unwrap (Fix 5b).
            LabelStmtNode* label = (LabelStmtNode*)stmt;
            if (!type_check_register_label(checker, label->name, stmt->pos)) return 0;
            return label->stmt ? type_check_statement(checker, label->stmt) : 1;
        }
        case AST_GOTO_STMT: {
            // gofmt-syntax-b Task 2: `goto L` — L must be one of this
            // function's labels (collected function-wide, forward refs
            // legal, by type_check_collect_goto_labels above). Unlike
            // labeled break/continue (T1, deferred to codegen), this is a
            // type-check-time check per the spec: goto's target is a pure
            // name lookup with no "does it enclose me" question — no
            // codegen control-flow state (loop/break-scope stack) is
            // needed to answer it, so there is no reason to defer it.
            GotoStmtNode* got = (GotoStmtNode*)stmt;
            int found = 0;
            size_t found_idx = 0;
            for (size_t i = 0; i < checker->tc_fctx.goto_label_count; i++) {
                if (checker->tc_fctx.goto_label_names[i] && got->label &&
                    strcmp(checker->tc_fctx.goto_label_names[i], got->label) == 0) {
                    found = 1;
                    found_idx = i;
                    break;
                }
            }
            if (!found) {
                type_error(checker, stmt->pos, "undefined label '%s'", got->label);
                return 0;
            }
            // arena-goto fix: reject a goto that would jump INTO an arena
            // block it is not already inside. Legal iff the label's
            // recorded arena-nesting path (snapshotted by the pre-pass,
            // types.h's goto_label_arena_chain) is a prefix of — or equal
            // to — this goto's OWN current arena_chain: that is exactly
            // the "goto only ever EXITS zero or more of its enclosing
            // arenas" shape codegen can free its way out of (mirrors
            // break/continue, which by construction can only ever target
            // an ENCLOSING frame). Anything else — the label nested in
            // MORE arenas than the goto, or in a different (sibling)
            // arena chain entirely — would require silently entering an
            // arena whose goo_arena_new never ran, which is exactly the
            // double-free/UAF SIGSEGV this check exists to close off.
            size_t label_depth = checker->tc_fctx.goto_label_arena_depth[found_idx];
            int arena_ok = label_depth <= checker->tc_fctx.arena_chain_depth;
            if (arena_ok) {
                for (size_t k = 0; k < label_depth; k++) {
                    if (checker->tc_fctx.goto_label_arena_chain[found_idx][k] != checker->tc_fctx.arena_chain[k]) {
                        arena_ok = 0;
                        break;
                    }
                }
            }
            if (!arena_ok) {
                type_error(checker, stmt->pos, "goto into arena block is not supported");
                return 0;
            }
            return 1;
        }
        case AST_GO_STMT:
            return type_check_go_stmt(checker, stmt);
        case AST_DEFER_STMT:
            return type_check_defer_stmt(checker, stmt);
        case AST_SELECT_STMT:
            return type_check_select_stmt(checker, stmt);
        case AST_SWITCH_STMT:
            return type_check_switch_stmt(checker, stmt);
        case AST_TYPE_SWITCH:
            return type_check_type_switch_stmt(checker, stmt);
        case AST_COMPTIME_BLOCK: {
            // M11-types-const-stub: minimum dispatch — treat the body as
            // ordinary statements. Engine engagement (real comptime
            // evaluation via comptime_type_evaluate) is M11-types-const-
            // integrate; this arm exists to close the "Unknown statement
            // type" failure mode for bare comptime { } blocks without
            // committing to integration depth before the spike.
            ComptimeBlockNode* cb = (ComptimeBlockNode*)stmt;
            if (!cb->body) return 1;
            return type_check_statement(checker, cb->body);
        }
        case AST_ARENA_BLOCK: {
            // arena-goto fix: push/pop the same arena_chain scratch stack
            // the goto_label_names pre-pass uses (types.h doc comment) so
            // a `goto` checked while inside this block's body sees it as
            // part of its own current arena-nesting path.
            ArenaBlockNode* ab = (ArenaBlockNode*)stmt;
            if (!ab->body) return 1;
            int pushed = checker->tc_fctx.arena_chain_depth < 16;
            if (pushed) {
                checker->tc_fctx.arena_chain[checker->tc_fctx.arena_chain_depth] = ab;
                checker->tc_fctx.arena_chain_depth++;
            }
            int ok = type_check_statement(checker, ab->body);
            if (pushed) checker->tc_fctx.arena_chain_depth--;
            return ok;
        }
        default:
            type_error(checker, stmt->pos, "Unknown statement type");
            return 0;
    }
}

int type_check_block_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_BLOCK_STMT) return 0;
    
    BlockStmtNode* block = (BlockStmtNode*)stmt;
    
    scope_push(checker);
    
    int result = 1;
    ASTNode* current = block->statements;
    while (current) {
        if (!type_check_statement(checker, current)) {
            result = 0;
        }
        current = current->next;
    }
    
    scope_pop(checker);
    return result;
}

int type_check_expr_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_EXPR_STMT) return 0;
    
    ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
    Type* type = type_check_expression(checker, expr_stmt->expr);
    return type != NULL;
}

int type_check_if_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_IF_STMT) return 0;
    
    IfStmtNode* if_stmt = (IfStmtNode*)stmt;
    
    // Check condition
    Type* cond_type = type_check_expression(checker, if_stmt->condition);
    if (!cond_type) return 0;
    
    // Condition must be boolean. P2.8 FIX F1 (cascade-suppression
    // completeness): a poisoned condition (bound to a previously failed
    // declaration — see register_declared_names_after_failure) must not
    // spawn a SECOND diagnostic here; silently accept it (mirroring the
    // binary-op choke point's silent-pass pattern) and let the then/else
    // branches still be checked for their own, unrelated errors.
    if (cond_type->kind != TYPE_BOOL && !type_is_poison(cond_type)) {
        type_error(checker, if_stmt->condition->pos,
                  "If condition must be boolean, got %s", type_to_string(cond_type));
        return 0;
    }
    
    // Check then branch
    if (!type_check_statement(checker, if_stmt->then_stmt)) {
        return 0;
    }
    
    // Check else branch if present
    if (if_stmt->else_stmt) {
        if (!type_check_statement(checker, if_stmt->else_stmt)) {
            return 0;
        }
    }
    
    return 1;
}

int type_check_for_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_FOR_STMT) return 0;

    ForStmtNode* for_stmt = (ForStmtNode*)stmt;

    scope_push(checker);

    int result = 1;

    // For-range: register the key as int and the value (if present)
    // as the element type of the range expression, both in scope for
    // the body. Slice/array/string/map/channel range are supported; other
    // ranged types are rejected below.
    if (for_stmt->range_expr) {
        Type* range_type = type_check_expression(checker, for_stmt->range_expr);
        if (!range_type) {
            scope_pop(checker);
            return 0;
        }
        Type* elem_type = NULL;
        // Key type defaults to the int32 index used by slice/array/string
        // range; TYPE_MAP overrides this below since a map's key is not an
        // index.
        Type* key_type = type_checker_get_builtin(checker, TYPE_INT32);
        if (range_type->kind == TYPE_SLICE) {
            elem_type = range_type->data.slice.element_type;
        } else if (range_type->kind == TYPE_ARRAY) {
            elem_type = range_type->data.array.element_type;
        } else if (range_type->kind == TYPE_STRING) {
            // F7: range over a string. v1 iterates BYTES (rune decoding is
            // deferred): key is the byte index, value is the byte widened to
            // int32 (rune today). int32 — not uint8 — so fmt.Println accepts
            // the value, and codegen zero-extends the i8 byte into it.
            elem_type = type_checker_get_builtin(checker, TYPE_INT32);
        } else if (range_type->kind == TYPE_MAP) {
            // Range over map: key/value bind to the map's OWN key/value
            // types, read off the Type itself rather than hardcoded — the
            // runtime now admits string/int/bool/char/pointer keys (see the
            // AST_MAP_TYPE comparability gate), and the checker never baked
            // in a single key type here.
            key_type = range_type->data.map.key_type;
            elem_type = range_type->data.map.value_type;
            if (!key_type || !elem_type) {
                type_error(checker, for_stmt->range_expr->pos,
                          "range over map: missing key/value type");
                scope_pop(checker);
                return 0;
            }
        } else if (range_type->kind == TYPE_CHANNEL) {
            // Range over channel: Go permits at most one iteration variable
            // (the received element) — there is no index to offer. The
            // grammar's single-var form (`for v := range ch`) parses `v`
            // into key_name, mirroring the slice/array/string index slot
            // (the grammar predates channel range and has no dedicated
            // production for it); that slot is reinterpreted here as the
            // received element by aliasing key_type to elem_type below, so
            // the generic key_name-binding code just past this if/else
            // chain does the right thing unmodified. The two-var form
            // (`for i, v := range ch`) always sets value_name — reject it
            // outright, matching Go's "permits only one iteration variable".
            if (for_stmt->value_name) {
                type_error(checker, for_stmt->range_expr->pos,
                          "range over channel permits at most one iteration variable");
                scope_pop(checker);
                return 0;
            }
            elem_type = range_type->data.channel.element_type;
            if (!elem_type) {
                type_error(checker, for_stmt->range_expr->pos,
                          "range over channel: missing element type");
                scope_pop(checker);
                return 0;
            }
            key_type = elem_type;
        } else {
            type_error(checker, for_stmt->range_expr->pos,
                      "for-range supported only on slice/array/string/map/channel types");
            scope_pop(checker);
            return 0;
        }
        if (for_stmt->key_name) {
            Variable* kv = variable_new(for_stmt->key_name, key_type, stmt->pos);
            if (kv) {
                kv->is_initialized = 1;
                kv->is_loop_var = 1;  // Closures Task 2: capture rejected (see Variable.is_loop_var)
                scope_add_variable(checker->current_scope, kv);
            }
        }
        if (for_stmt->value_name && elem_type) {
            Variable* vv = variable_new(for_stmt->value_name, elem_type, stmt->pos);
            if (vv) {
                vv->is_initialized = 1;
                vv->is_loop_var = 1;  // Closures Task 2: capture rejected (see Variable.is_loop_var)
                scope_add_variable(checker->current_scope, vv);
            }
        }
    }

    // Check initialization
    if (for_stmt->init) {
        if (!type_check_statement(checker, for_stmt->init)) {
            result = 0;
        } else {
            // Closures Task 2: mark the init clause's declared names as loop
            // variables so a closure capture of them is rejected (see
            // Variable.is_loop_var, types.h). The init decl routes through
            // the ORDINARY var-decl / multi-assign checkers above — which
            // have no idea they are inside a for header — so the marking
            // happens here, after the fact: the names were just registered
            // into the for's OWN scope (pushed at the top of this function),
            // so a direct walk of current_scope->variables (deliberately NOT
            // the parent-chasing scope_lookup_variable — an init decl that
            // failed to register must not mark a same-named OUTER variable)
            // finds exactly the loop-owned bindings.
            char** names = NULL;
            size_t name_count = 0;
            char* multi_names[8];  // for i, j := ... — targets are identifiers (v1)
            if (for_stmt->init->type == AST_VAR_DECL) {
                VarDeclNode* vd = (VarDeclNode*)for_stmt->init;
                names = vd->names;
                name_count = vd->name_count;
            } else if (for_stmt->init->type == AST_MULTI_ASSIGN &&
                       ((MultiAssignNode*)for_stmt->init)->is_short_decl) {
                MultiAssignNode* ma = (MultiAssignNode*)for_stmt->init;
                size_t n = 0;
                for (ASTNode* t = ma->targets; t && n < 8; t = t->next) {
                    if (t->type == AST_IDENTIFIER)
                        multi_names[n++] = ((IdentifierNode*)t)->name;
                }
                names = multi_names;
                name_count = n;
            }
            for (size_t i = 0; i < name_count; i++) {
                for (Variable* v = checker->current_scope->variables; v; v = v->next) {
                    if (strcmp(v->name, names[i]) == 0) { v->is_loop_var = 1; break; }
                }
            }
        }
    }

    // Check condition
    if (for_stmt->condition) {
        Type* cond_type = type_check_expression(checker, for_stmt->condition);
        if (!cond_type || cond_type->kind != TYPE_BOOL) {
            type_error(checker, for_stmt->condition->pos,
                      "For condition must be boolean");
            result = 0;
        }
    }
    
    // Check post statement
    if (for_stmt->post) {
        if (!type_check_statement(checker, for_stmt->post)) {
            result = 0;
        }
    }
    
    // Check body
    if (for_stmt->body) {
        if (!type_check_statement(checker, for_stmt->body)) {
            result = 0;
        }
    }
    
    scope_pop(checker);
    return result;
}

// Returns non-zero if `node` is an untyped integer constant expression: a bare
// integer literal, a unary minus applied to one (`-1`), or a binary/shift
// expression whose operands are themselves untyped integer constant
// expressions (`1 + 1`, `1 << 3`). Codegen materializes all of these as an
// integer constant and then coerces it to the declared integer return type
// (SExt to widen, constant-rebuild to narrow — see the return-coercion path in
// statement_codegen.c). Such a return coerces into any integer return type in
// which the constant is representable (Go representability rule); the caller
// (`int_const_coerce`) accepts both widening and narrowing targets, but only
// after int_const_fits_expected confirms the folded value fits — see there.
int is_untyped_int_const_expr(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_LITERAL)
        return ((LiteralNode*)node)->literal_type == TOKEN_INT;
    if (node->type == AST_UNARY_EXPR) {
        UnaryExprNode* u = (UnaryExprNode*)node;
        return u->operator == TOKEN_MINUS && u->operand &&
               is_untyped_int_const_expr(u->operand);
    }
    if (node->type == AST_BINARY_EXPR) {
        BinaryExprNode* b = (BinaryExprNode*)node;
        return b->left && b->right &&
               is_untyped_int_const_expr(b->left) &&
               is_untyped_int_const_expr(b->right);
    }
    return 0;
}

// Genuinely negative-rooted, per the AST SHAPE, not the fold's sign: is
// `node` a top-level unary MINUS? Sibling to is_untyped_int_const_expr just
// above and to expression_checker.c's check_conversion_operand_range /
// adapt_untyped_int_operand `negated` convention, but deliberately shallower
// — it does NOT recurse through nested unary or binary structure the way
// those adapters do, because it exists for exactly one purpose: telling
// int_const_fits_expected's uint64 arm apart from a same-bit-pattern
// non-negative fold (see that function's doc comment). `-1` (UnaryExprNode
// wrapping the literal) is negated; `0 - 1` (BinaryExprNode) is NOT, even
// though goo_fold_const_int folds both to the identical 64-bit pattern —
// that gap is `int_const_fits_expected`'s documented deviation.
int is_negated_int_const_expr(ASTNode* node) {
    return node && node->type == AST_UNARY_EXPR &&
           ((UnaryExprNode*)node)->operator == TOKEN_MINUS;
}

// Is `node` (the ORIGINAL, unfolded expression handed to int_const_fits_
// expected) a bare integer-literal LEAF — no unary negation, no binary
// arithmetic wrapping it? Sibling to is_negated_int_const_expr; feeds
// int_const_fits_expected's signed-target bare-huge-literal reject (fix
// round 4, correctness-burndown arc — see that function's doc comment for
// the full rationale). The discriminator has to be this AST-SHAPE check, not
// "folded raw > INT64_MAX" alone: legal constant arithmetic can fold into
// that identical 64-bit range (`0 - 5` -> raw = 0xFFFFFFFFFFFFFFFB, since
// -5's two's-complement encoding also has its top bit set) without being an
// unrepresentable value — Go accepts int8(0 - 5) == -5. Only a literal
// actually WRITTEN >= 2^63 in source (`18446744073709551615`) is
// unrepresentable in every signed width, including int64.
int is_bare_int_literal(ASTNode* node) {
    return node && node->type == AST_LITERAL &&
           ((LiteralNode*)node)->literal_type == TOKEN_INT;
}

// Go representability rule for return_stmt's int_const_coerce gate: `raw` is
// the constant's folded value (goo_fold_const_int's two's-complement 64-bit
// encoding — decoding it back through int64_t is well-defined on a C23
// two's-complement target, which this project requires). Returns non-zero iff
// that value fits `expected`'s [min,max] range: signed [-2^(w-1), 2^(w-1)-1]
// or unsigned [0, 2^w-1]. Mirrors expression_checker.c's literal_fits_type
// (same bounds, same Go-conformant "constant N overflows T" shape) but keyed
// off an already-folded 64-bit value rather than a single literal's text, so
// it also covers constant arithmetic (`1 + 200`) that is_untyped_int_const_expr
// admits but literal_fits_type cannot parse. int64 targets are satisfied by
// every value EXCEPT a bare literal >= 2^63 (see `bare_literal` below and the
// signed case table) — raw is already the exact 64-bit value for anything an
// int64 can hold, but a literal actually written >= 2^63 cannot be held by
// ANY signed width, int64 included. uint64 is NOT unconditionally satisfied
// either — see `negated` below.
//
// `negated` (is_negated_int_const_expr on the ORIGINAL, unfolded return
// expression — see the caller) feeds the uint/uint64 arm's rejection rule
// below: reject iff `sval < 0 && negated` — the fold's sign AND the AST
// shape, a CONJUNCTION, not either alone (fix round 3, correctness-burndown
// arc; round 2 used `negated` alone and over-rejected — see below).
//
// `bare_literal` (is_bare_int_literal on that same original, unfolded
// expression) feeds the SIGNED arms' rejection rule (fix round 4): reject
// unconditionally iff `bare_literal && raw > INT64_MAX` — see the signed
// case table and int_const_fits_expected's body below for the full
// rationale (in short: only a literal actually written >= 2^63 in source is
// unrepresentable in every signed width; arithmetic that folds into that
// same raw range, e.g. `0 - 5`, is legal and must not be caught by this).
//
// Why the conjunction, and why it's safe: goo_fold_const_int is 64-bit
// MODULAR (documented at its definition), so `sval < 0` alone is ambiguous
// ONLY inside [2^63, 2^64) — that's the one region where a legal top-of-range
// uint64 literal (e.g. `18446744073709551615`, MaxUint64) and a genuinely
// negative constant (e.g. `-1`) can share the identical bit pattern
// (0xFFFFFFFFFFFFFFFF) and thus the same negative `sval`. `negated` (a
// top-level-unary-minus AST check) is the one signal that disambiguates
// exactly there. Outside that region (`sval >= 0`) the fold is authoritative
// on its own and the shape check is never consulted — a double or
// parenthesized negation that folds back to non-negative (`- -1`, `-(1 - 2)`,
// both folding to +1) is accepted regardless of its unary-minus shape,
// fixing round 2's over-reject (round 2 checked `negated` alone, so any
// unary-minus-rooted expression rejected even when its fold was positive).
//
// Case table (uint64 target):
//   -1                      -> sval<0, negated     -> REJECT (correct)
//   18446744073709551615    -> sval<0, NOT negated -> ACCEPT (correct; MaxUint64)
//   - -1, -(1 - 2)          -> sval=+1 (>=0)        -> ACCEPT (correct; shape
//                                                      never consulted)
//   -(9223372036854775808)  -> folds to INT64_MIN's bit pattern (negating
//                              2^63 wraps to itself under 64-bit modular
//                              arithmetic), sval<0, negated -> REJECT
//                              (correct: mathematically negative)
//   0 - 1                   -> sval<0, NOT negated -> ACCEPT-and-wraps to
//                              18446744073709551615 — the ONE remaining
//                              documented deviation (under-reject only; Go
//                              would still reject this). Round 2's
//                              over-reject deviation (rejecting `- -1` etc.)
//                              is gone; only this pre-existing under-reject
//                              gap remains.
//
// Case table (SIGNED target, e.g. int8/int64 — fix round 4):
//   18446744073709551615    -> bare literal, raw > INT64_MAX -> REJECT (every
//                              signed width, including int64: no signed type
//                              can hold a value >= 2^63)
//   0 - 5 (-> int8)         -> raw = 0xFFFF...FB (> INT64_MAX!) but NOT a
//                              bare literal (BinaryExprNode) -> skips the new
//                              check, falls through to sval=-5 range test ->
//                              ACCEPT (correct: Go accepts int8(0 - 5) == -5;
//                              same documented-deviation class as the uint64
//                              `0 - 1` row above — arithmetic that folds into
//                              the huge-raw range is not itself huge)
//   -(9223372036854775808)  -> UnaryExprNode, not a bare literal -> skips the
//                              new check; sval=INT64_MIN accepted for int64,
//                              rejected for narrower widths (unchanged)
//
// Documented deviation (same class as this file's existing `^`-rooted
// deviation — see check_conversion_operand_range's doc comment):
// `is_negated_int_const_expr` only recognizes a top-level unary minus, so a
// PURE-ARITHMETIC negative result with no such literal minus sign — e.g.
// `return 0 - 1` into a uint64 — is not flagged `negated` and slips through
// as its two's-complement reinterpretation (18446744073709551615), where Go
// would still reject it (goo_fold_const_int's 64-bit-modular fold is what
// produces that reinterpretation). Out of scope for this fix, same as the
// `^`-rooted gap; narrower (uint8/16/32) targets are unaffected since they
// keep the unconditional `sval < 0` rejection below. The SAME class of gap
// exists in reverse for signed targets — a pure-arithmetic expression that
// folds to a value >= 2^63 (there is no such legal Go constant expression at
// this integer width without an explicit huge literal, so this is currently
// unreachable in practice, but is called out here for completeness) would
// likewise not be flagged by `bare_literal` and would fall through to the
// ordinary sval range checks below.
int int_const_fits_expected(uint64_t raw, Type* expected, int negated,
                             int bare_literal) {
    int64_t sval = (int64_t)raw;
    if (type_is_signed(expected)) {
        // A bare literal >= 2^63 as WRITTEN IN SOURCE (e.g.
        // `18446744073709551615`) folds to a negative `sval` under 64-bit
        // modular arithmetic (its top bit is set), which would otherwise
        // slip past every per-width range check below and reinterpret as a
        // small negative number (`return 18446744073709551615` into int8
        // returning -1) — a reject->accept regression vs. base dd11713's old
        // width gate (fix round 4, correctness-burndown arc, fable final
        // review). No signed width, not even int64, can represent a value
        // >= 2^63, so reject unconditionally here — before the per-width
        // switch, since every arm would need the identical guard otherwise.
        // Gated on `bare_literal` (an AST-shape check — see its doc comment)
        // rather than `!negated`, so compound arithmetic that folds into the
        // identical bit-pattern range (`0 - 5`) stays on the ordinary
        // sval-range path below instead of being wrongly rejected — see the
        // case table above.
        if (bare_literal && raw > (uint64_t)INT64_MAX) return 0;
        switch (expected->kind) {
            case TYPE_INT8:  return sval >= INT8_MIN  && sval <= INT8_MAX;
            case TYPE_INT16: return sval >= INT16_MIN && sval <= INT16_MAX;
            case TYPE_INT32: return sval >= INT32_MIN && sval <= INT32_MAX;
            default:         return 1; // int / int64
        }
    }
    switch (expected->kind) {
        // A negative constant can never satisfy a NARROWER unsigned target,
        // at any width — Go rejects `var x uint8 = -1` regardless of how the
        // negative value was produced (fold sign is trustworthy here: no
        // narrower unsigned width can ever legitimately reach 2^63+, so
        // there's no same-bit-pattern ambiguity to resolve via `negated`).
        case TYPE_UINT8:  return sval >= 0 && (uint64_t)sval <= UINT8_MAX;
        case TYPE_UINT16: return sval >= 0 && (uint64_t)sval <= UINT16_MAX;
        case TYPE_UINT32: return sval >= 0 && (uint64_t)sval <= UINT32_MAX;
        default:
            // uint / uint64: reject iff BOTH the fold is negative AND the
            // constant is syntactically negative-rooted (`sval < 0 &&
            // negated`) — see the conjunction rationale and case table
            // above. Otherwise the fold is authoritative and the full 64-bit
            // range [0, 2^64-1] is representable, including values >= 2^63
            // like MaxUint64.
            return !(sval < 0 && negated);
    }
}

int type_check_return_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_RETURN_STMT) return 0;
    
    ReturnStmtNode* ret_stmt = (ReturnStmtNode*)stmt;

    // A value-less `return` from a function declared to return an error union
    // (!T) supplies neither an error(...) construction nor a value of T, so it
    // cannot satisfy the declared type. Reject it here rather than letting
    // codegen synthesize a zeroed error union (which a caller's catch would
    // never fire on, leaving the value half as garbage).
    if (!ret_stmt->values) {
        Type* expected = checker->current_return_type;
        if (expected && type_is_error_union(expected)) {
            Type* vt = expected->data.error_union.value_type;
            // `!void` (error-only, the Go `func() error` shape) is not yet
            // supported end-to-end — the success-variant codegen can't build a
            // void-valued union (invalid IR). Reject it cleanly until the
            // void-union codegen lands, rather than crash the verifier or
            // (worse) emit a zeroed union a caller's catch never fires on.
            if (vt && vt->kind == TYPE_VOID) {
                type_error(checker, stmt->pos,
                           "!void (error-only) functions are not yet supported "
                           "in v1; use !T with a value type for now");
                return 0;
            }
            // `!T` with a real value type: a bare return supplies neither an
            // error(...) construction nor a value of T.
            type_error(checker, stmt->pos,
                       "return type mismatch: bare return from a function "
                       "returning %s (expected a value of %s or an error(...) "
                       "construction)",
                       type_to_string(expected),
                       type_to_string(vt));
            return 0;
        }
        // Bare return from an UNNAMED multi-result function `func f() (int, int)`
        // is invalid in Go — a bare return is only allowed when results are
        // named. The parser tags such a result list as a TYPE_STRUCT whose
        // fields all carry synthetic `_N` names (a SINGLE named/unnamed result
        // is collapsed to a scalar in type_from_ast and never reaches here, and
        // a NAMED tuple keeps the user's field names). Reject it here rather
        // than letting codegen synthesize a zeroed aggregate (`return` would
        // silently yield 0,0 — a miscompile).
        if (expected && expected->kind == TYPE_STRUCT &&
            expected->data.struct_type.field_count > 0) {
            int all_synthetic = 1;
            for (size_t i = 0; i < expected->data.struct_type.field_count; i++) {
                if (!is_synthetic_result_name(expected->data.struct_type.fields[i].name)) {
                    all_synthetic = 0;
                    break;
                }
            }
            if (all_synthetic) {
                type_error(checker, stmt->pos,
                           "not enough return values: bare return is only allowed "
                           "when the function has named results");
                return 0;
            }
        }
        return 1;
    }

    // Type-check EVERY returned expression (the values are a ->next list for a
    // multi-value return), so codegen sees a resolved node_type on each — e.g.
    // the T(x) conversions in `return uint(a), uint(b)`. The single-value
    // compatibility logic below still keys off the first value / the tuple.
    for (ASTNode* v = ret_stmt->values; v; v = v->next) {
        if (!type_check_expression(checker, v)) return 0;
    }

    // Check return value if present
    {
        Type* return_type = type_check_expression(checker, ret_stmt->values);
        if (!return_type) return 0;

        // P2.8 T4.2 (cascade suppression): a poisoned value (bound to a
        // previously failed declaration — see
        // register_declared_names_after_failure) must not spawn a SECOND
        // diagnostic here either. Without this, `return x` on a poisoned
        // `x` fell through to the mismatch check below and leaked the
        // internal "<poisoned>" type name into a user-facing "return type
        // mismatch" message — found by this task's own try-precedence-hint
        // probe (`x := try f() + 1; return x`), not by the recon's original
        // narrower `+`/println probe shape.
        if (type_is_poison(return_type)) return 1;

        // Fix 2 (comptime-param functions are not first-class values):
        // `return fill` would hand the caller a Variable-less function VALUE
        // with no func_decl_node to check a later call's arguments against —
        // the same bypass the var-decl/assignment guards close, for the
        // return-value channel.
        if (!reject_comptime_function_value(checker, ret_stmt->values, return_type,
                                            stmt->pos, "returned")) {
            return 0;
        }

        // When the enclosing function returns an error union (!T), the returned
        // expression is valid iff it is an error(...) construction / another !T
        // forwarded whole (its resolved type is THE SAME error union) OR its
        // type is compatible with the union's value type T. A plain value of an
        // incompatible type (e.g. `return "str"` from an !int function) is a
        // clean type error here, before it can reach codegen / the verifier.
        //
        // The forward case must require type_equals against the enclosing union,
        // not merely type_is_error_union: a mismatched !T (e.g. forwarding an
        // !string out of an !int function) is otherwise waved through here and
        // then crashes the LLVM verifier with a return-operand type mismatch.
        // error(...) resolves to exactly current_return_type (see
        // expression_checker.c), so type_equals also accepts genuine
        // error(...) constructions.
        Type* expected = checker->current_return_type;
        if (expected && type_is_error_union(expected)) {
            Type* value_type = expected->data.error_union.value_type;
            int is_error_or_forward = type_equals(return_type, expected);
            int is_value = value_type && type_compatible(return_type, value_type);
            if (!is_error_or_forward && !is_value) {
                type_error(checker, stmt->pos,
                           "return type mismatch: cannot return %s from a function "
                           "returning %s (expected %s or an error(...) construction)",
                           type_to_string(return_type),
                           type_to_string(expected),
                           type_to_string(value_type));
                return 0;
            }
        } else if (expected) {
            // Non-error-union enclosing return type. Reject a returned value
            // whose type is incompatible with the declared return type here,
            // before it reaches codegen and crashes the LLVM verifier with a
            // "return type does not match operand type" mismatch.

            // A multi-value `return a, b` targets an anonymous multi-return
            // struct (parser lowers `(int, int)` to a TYPE_STRUCT). Only the
            // FIRST value is resolved by type_check_expression above, so a
            // single-type comparison against the struct would be a false
            // positive. Per-element multi-return checking is out of scope —
            // accept and let codegen build the aggregate.
            //
            // F2 fix (this is the acceptance point the finding traced to):
            // the ONE element that must still be checked is a field typed
            // `error` (the `(T, error)` convention's error slot) — v1's
            // error value is created exclusively by error(...)/errors.New/
            // fmt.Errorf (all resolve to type_checker_error_type()) or is
            // nil/a forwarded `error`-typed value; a user struct satisfying
            // Error() silently slipping in here was previously UNCHECKED
            // (this bypass returned 1 before any field was inspected), so it
            // reached codegen, got boxed as a plain interface value instead
            // of a goo_error_t*, and SIGSEGV'd at runtime the moment
            // anything read it back (catch's e.Error(), the destructure
            // path's err.Error(), and the new try tuple-propagation
            // readback in error_union_codegen.c all assume field 1 IS a
            // goo_error_t*). Reject at compile time instead. Every OTHER
            // field position keeps the pre-existing accept-and-let-codegen-
            // build-the-aggregate behavior — full per-element checking
            // stays out of scope.
            if (ret_stmt->values->next) {
                if (expected->kind == TYPE_STRUCT) {
                    ASTNode* v = ret_stmt->values;
                    size_t i = 0;
                    for (; v && i < expected->data.struct_type.field_count;
                         v = v->next, i++) {
                        Type* field_type = expected->data.struct_type.fields[i].type;
                        if (!type_is_error(field_type)) continue;

                        Type* vt = v->node_type;
                        if (!vt) continue;              // unresolved — don't risk a false positive
                        if (type_is_poison(vt)) continue;   // T4.2 cascade suppression
                        if (vt->kind == TYPE_UNKNOWN) continue;  // bare `nil`
                        if (type_is_error(vt)) continue;    // already the error
                                                             // interface: nil-typed
                                                             // forward, error(...),
                                                             // errors.New/fmt.Errorf

                        type_error(checker, v->pos,
                                   "custom error types are not supported in "
                                   "v1; construct errors with error(\"...\")");
                        return 0;
                    }
                }
                return 1;
            }

            // A value returned from a function with no declared return type
            // (void) is always a mismatch (Go: "too many return values").
            if (expected->kind == TYPE_VOID) {
                type_error(checker, stmt->pos,
                           "return type mismatch: cannot return a value from a "
                           "function with no return type");
                return 0;
            }

            // An unresolved operand type (TYPE_UNKNOWN — e.g. a bare nil or an
            // expression the checker couldn't pin down) is not something we can
            // soundly reject; defer rather than risk a false positive.
            if (return_type->kind == TYPE_UNKNOWN) {
                return 1;
            }

            // type_compatible() no longer permits ANY numeric->numeric pair —
            // it now rejects float->int specifically (T3's asymmetric fix for
            // the silent bit-store: `var i int64 = float32(2.5)` used to
            // reinterpret the float's raw bits instead of converting), while
            // still permitting int<->int width mismatches and int->float. A
            // return value reaches codegen with NO trunc/ext/fptosi inserted
            // for those still-permitted numeric mismatches. So a numeric
            // return whose machine representation differs from the declared
            // return type — a wider/narrower integer (e.g. an int64 call
            // result from an int function) — would slip past type_compatible
            // and crash the LLVM verifier with a return-operand mismatch.
            // Reject those here. The sole exception
            // codegen DOES coerce is an untyped integer constant expression
            // (literal `42`, or constant arithmetic like `1 + 1` / `1 << 3`)
            // returned into a WIDER-OR-EQUAL integer type (e.g. `return 42` or
            // `return 1 + 1` from an int64 function): codegen SExt-widens the
            // i32 constant to match. It does NOT truncate, so a NARROWING target
            // (e.g. `return 65` into a byte) gets no trunc and must still be
            // rejected here — gate the exemption on expected being no narrower
            // than the operand, mirroring codegen's widen-only behavior.
            if (type_is_numeric(return_type) && type_is_numeric(expected)) {
                int same_kind  = (type_is_float(return_type) == type_is_float(expected));
                int same_width = (type_size(return_type) == type_size(expected));
                // An untyped integer constant expression (`return 1`,
                // `return 1 + 1`) coerces into any integer return type in
                // which it is REPRESENTABLE (Go representability rule) —
                // WIDENING (`return 0` from an int64 fn) OR NARROWING
                // (`return 1` from an int8 fn), exactly as Go accepts an
                // untyped constant into a typed context. Codegen materializes
                // the constant directly at the declared return width (SExt to
                // widen, constant-rebuild to narrow — see the return-coercion
                // path in statement_codegen.c), so no machine-representation
                // mismatch reaches the verifier. The old gate additionally
                // required expected be no NARROWER than the operand's default
                // type (int64), which wrongly rejected every `return <literal>`
                // from a sub-int64 function (int8/16/32, uint8/16/32) — a
                // single-function false positive that the return-stmt's
                // lookahead position (get_current_position in parser.y points
                // at the NEXT decl) then mis-blamed on a later sibling, giving
                // the illusion of cross-decl poisoning.
                int int_const_coerce =
                    type_is_integer(expected) &&
                    is_untyped_int_const_expr(ret_stmt->values);

                // Representability gate: an untyped constant's default type
                // has no fixed width, but its VALUE does need to fit the
                // declared return type — Go rejects `return 300` from a
                // `func() int8` at compile time ("constant 300 overflows
                // int8"), not truncate it silently. Fold the constant here
                // (the same fold codegen's return-coercion block reaches for
                // via LLVMConstIntGetSExtValue/ZExtValue, just done on the AST
                // instead of the already-built LLVM constant) and reject
                // out-of-range values before they can reach that unchecked
                // narrowing rebuild. A fold failure (e.g. a divide-by-zero
                // constant subexpression) is left to fall through unchecked —
                // out of scope for this gate.
                if (int_const_coerce) {
                    uint64_t raw;
                    int negated = is_negated_int_const_expr(ret_stmt->values);
                    int bare_literal = is_bare_int_literal(ret_stmt->values);
                    if (goo_fold_const_int(ret_stmt->values, &raw) &&
                        !int_const_fits_expected(raw, expected, negated,
                                                  bare_literal)) {
                        type_error(checker, ret_stmt->values->pos,
                                   "constant %lld overflows %s",
                                   (long long)(int64_t)raw,
                                   type_to_string(expected));
                        return 0;
                    }
                }

                // Untyped int constant into a FLOAT return type (`return 1`
                // from a func() float64/float32 — correctness-followups arc
                // 3, task 3). Go's representability rule for constants:
                // EVERY integer value expressible in this AST shape
                // (is_untyped_int_const_expr — bare literal, unary minus over
                // one, or constant arithmetic) is representable as a
                // floating value in BOTH float64 and float32, because Go
                // defines rounding for a constant that exceeds the target
                // float type's PRECISION (not its range) — an int64-range
                // constant is always within range (float64 holds up to
                // ~1.8e308, float32 up to ~3.4e38; nothing an int64-shaped
                // literal can spell gets remotely close). So — unlike
                // int_const_coerce just above — there is no
                // int_const_fits_expected-style overflow rejection to run
                // here: accept unconditionally. This is a v1 SCOPE DECISION,
                // not a general untyped-float-constant implementation —
                // untyped FLOAT constants (`3.9`) have no folder
                // (`goo_fold_const_int` is int-only) and are NOT covered by
                // this gate. Narrowing an untyped FLOAT constant into
                // func() float32 (e.g. `return 3.9` from a float32 fn) is a
                // KNOWN FALSE-REJECT here (Go accepts: constant conversion
                // with rounding); out of scope here because no untyped-FLOAT
                // constant folder exists to reuse — backlog item, see the
                // mirror float-case-on-int-tag crash (see the arc's task
                // report).
                int float_const_coerce =
                    type_is_float(expected) &&
                    is_untyped_int_const_expr(ret_stmt->values);
                if (float_const_coerce) {
                    // Stamp the whole constant subtree to the float target
                    // type so codegen_generate_literal's existing cross-kind
                    // float-adaptation arm (TOKEN_INT case,
                    // expression_codegen.c — the same one `1 < g` against a
                    // float32 `g` already stamps into) emits an LLVMConstReal
                    // identical to what a float literal of this value would
                    // produce, instead of an int64 LLVMConstInt the return
                    // path's own width-coercion block (integer-only) has no
                    // conversion for.
                    stamp_int_const_expr_type(ret_stmt->values, expected);
                }

                if ((!same_kind || !same_width) &&
                    !int_const_coerce && !float_const_coerce) {
                    // Point at the returned value, not stmt->pos: a return
                    // statement's pos is the post-parse lookahead (the next
                    // decl's line), which mis-attributes the diagnostic.
                    Position epos = ret_stmt->values ? ret_stmt->values->pos
                                                      : stmt->pos;
                    type_error(checker, epos,
                               "return type mismatch: cannot return %s from a "
                               "function returning %s",
                               type_to_string(return_type),
                               type_to_string(expected));
                    return 0;
                }
            }

            // Returning a concrete implementer into an interface return type
            // (P4-3) — accept iff it satisfies the interface.
            if (expected->kind == TYPE_INTERFACE) {
                if (!check_interface_assign(checker, return_type, expected, stmt->pos)) {
                    return 0;
                }
            } else if (!type_compatible(return_type, expected)) {
                type_error(checker, stmt->pos,
                           "return type mismatch: cannot return %s from a "
                           "function returning %s",
                           type_to_string(return_type),
                           type_to_string(expected));
                return 0;
            }
        }
    }

    return 1;
}

// Retype an untyped-int-constant-rooted case expression — is_untyped_int_
// const_expr's exact shape (a bare literal, a unary MINUS through to one, or
// a binary op whose sides are both recursively const-rooted) — to `target`
// at EVERY level, not just the leaf literal. Mirrors expression_checker.c's
// adapt_untyped_int_operand (that function is `static` to its own file and
// wired only into binary-expr checking, so this is a same-shape companion
// kept local rather than a header edit for this one call site — same
// rationale as this file's pre-existing is_untyped_int_const_expr/
// int_const_fits_expected staying local instead of reaching into
// expression_checker.c). Only ever called AFTER int_const_fits_expected has
// confirmed representability (see type_check_switch_stmt below), so no
// range check happens here — pure stamping. codegen_generate_literal reads
// node_type off exactly the literal leaf to pick the emitted constant's
// LLVM width (see expression_codegen.c's TOKEN_INT arm), so stamping the
// whole subtree — not just top-level — is what makes a compound case
// expression (`-5`, `1+2`) emit at the switch tag's width, matching a bare
// literal case (`'\n'`, `300`).
void stamp_int_const_expr_type(ASTNode* node, Type* target) {
    if (!node) return;
    node->node_type = target;
    if (node->type == AST_UNARY_EXPR) {
        stamp_int_const_expr_type(((UnaryExprNode*)node)->operand, target);
    } else if (node->type == AST_BINARY_EXPR) {
        BinaryExprNode* b = (BinaryExprNode*)node;
        stamp_int_const_expr_type(b->left, target);
        stamp_int_const_expr_type(b->right, target);
    }
}

// Correctness arc 4 (j): the shared chan-send representability gate, deduped
// from the two send sinks that each carried an inline copy of arc 3's
// literal-only gate (type_check_channel_send_op in expression_checker.c and
// the select-comm send path in type_check_select_stmt below) — and extended
// to close the const-IDENTIFIER fail-open both copies shared: their
// is_untyped_int_const_expr admission is an AST-shape predicate that never
// matches AST_IDENTIFIER, so `const k = 300; ch <- k` into a chan int8 fell
// through to the blanket any-int type_compatible and the receiver printed 44.
// Admission is now "does the checker-aware folder fold it": goo_fold_const_
// int_ctx succeeds exactly when the expression is built entirely from integer
// literals and cached-const identifiers, a superset of the old shape
// predicate (it also folds ^/~ unaries the shape walk never admitted — those
// now get the same representability treatment instead of silently wrapping).
//
// int_const_fits_expected wants the negated/bare_literal AST-shape signals of
// the ORIGINAL expression (see its doc comment), which a folded identifier no
// longer has — reconstruction is by shape class:
//   - bare identifier: the resolved const's own type is authoritative (the
//     arc-4 T1 decl fix is what makes its signedness trustworthy for
//     negative folds): a signed const folding negative IS genuinely negative
//     (negated=1); an unsigned const holding a raw > INT64_MAX IS a huge
//     positive, unrepresentable in every signed width (bare_literal=1).
//   - pure literal shape (is_untyped_int_const_expr): arc 3's exact
//     conventions, bit-compatible — including the subtree stamp that makes
//     codegen emit the constant at the element width.
//   - compound with identifier leaves (`k + 100`): top-level shape checks
//     only, bare_literal=0 — inheriting the same documented under-reject
//     deviation class as `0 - 1` (a fold that lands in [2^63, 2^64) via
//     arithmetic reads as huge-positive). NOT stamped: identifier leaves
//     load at their variable's own width, so stamping the literal leaves
//     would fight adapt_untyped_int_operand's earlier stamp and hand binary
//     codegen mixed-width operands. Unstamped is safe — codegen_generate_
//     channel_send coerces the send value to the element width regardless,
//     and truncation of a representable value is exact.
//
// Returns 0 if the gate does not apply (caller falls through to its ordinary
// type_compatible check), 1 if the send is representable and handled, -1 if
// rejected (diagnostic already reported at value_expr's position).
//
// Arc 5 (h): the screen->fold->reconstruct->fit core moved to
// check_const_int_expr_fits below when the typed-const/var decl gates became
// its second and third consumers; this wrapper keeps only what is genuinely
// send-specific — the kind-DIFFERENCE precondition (a same-kind send cannot
// be unrepresentable: the decl gates have no such precondition because
// `const k int64 = 18446744073709551615` is a same-kind, value-level hole)
// and the pure-literal-shape stamp that makes codegen emit the constant at
// the element width.
int chan_send_const_int_gate(TypeChecker* checker, ASTNode* value_expr,
                             Type* value_type, Type* elem_type) {
    if (!checker || !value_expr || !value_type || !elem_type) return 0;
    if (!type_is_integer(elem_type) || !type_is_integer(value_type) ||
        elem_type->kind == value_type->kind)
        return 0;
    int fit = check_const_int_expr_fits(checker, value_expr, elem_type);
    if (fit > 0 && is_untyped_int_const_expr(value_expr))
        stamp_int_const_expr_type(value_expr, elem_type);
    return fit;
}

// The shared representability core for every ident-aware constant sink
// (chan-send gate above; typed-const and var decl gates, arc 5 item (h)):
// does `expr`, IF it is a compile-time integer constant the checker-aware
// folder can resolve, fit integer type `target`? Returns 0 when the gate
// simply does not apply — not a foldable constant, a non-integer target, or
// comptime-param-tainted — so callers fall back to their ordinary path; 1
// when the folded value fits; -1 when it does not (the "constant %lld
// overflows %s" diagnostic is emitted here, at expr's position, so all
// consumers report identically).
//
// Comptime-value-param screen (arc-4 review fix): inside a comptime
// function's TEMPLATE body the param is a Variable with a PLACEHOLDER
// const_int_value (1, bound purely for type-validity), which goo_fold_
// const_int_ctx resolves like any cached const — so without this screen
// the gate judged `ch <- 1000000 / n` from n=1 (hard-rejecting valid
// instances, or blessing invalid ones). Same guard the folder's other
// consumers use (see goo_expr_references_comptime_param's doc comment);
// such expressions fall back to the caller's path, judged per-instance by
// codegen as before these gates existed.
//
// int_const_fits_expected wants the negated/bare_literal AST-shape signals
// of the ORIGINAL expression (see its doc comment), which a folded
// identifier no longer has — reconstruction is by shape class:
//   - bare identifier: the resolved const's own type is authoritative (the
//     arc-4 T1 decl fix is what makes its signedness trustworthy for
//     negative folds): a signed const folding negative IS genuinely
//     negative (negated=1); an unsigned const holding a raw > INT64_MAX IS
//     a huge positive, unrepresentable in every signed width
//     (bare_literal=1).
//   - pure literal shape (is_untyped_int_const_expr): the arc-3
//     conventions, bit-compatible.
//   - compound with identifier leaves (`k + 100`): top-level shape checks
//     only, bare_literal=0 — inheriting the same documented under-reject
//     deviation class as `0 - 1` (a fold that lands in [2^63, 2^64) via
//     arithmetic reads as huge-positive).
int check_const_int_expr_fits(TypeChecker* checker, ASTNode* expr,
                              Type* target) {
    if (!checker || !expr || !target || !type_is_integer(target)) return 0;
    if (goo_expr_references_comptime_param(checker, expr)) return 0;
    uint64_t raw;
    if (!goo_fold_const_int_ctx(checker, expr, &raw)) return 0;

    int negated, bare_literal;
    if (expr->type == AST_IDENTIFIER) {
        Variable* var = type_checker_lookup_variable(
            checker, ((IdentifierNode*)expr)->name);
        int var_signed = var && var->type && type_is_signed(var->type);
        negated = var_signed && (int64_t)raw < 0;
        bare_literal = !var_signed && raw > (uint64_t)INT64_MAX;
    } else if (is_untyped_int_const_expr(expr)) {
        negated = is_negated_int_const_expr(expr);
        bare_literal = is_bare_int_literal(expr);
    } else {
        negated = is_negated_int_const_expr(expr);
        bare_literal = 0;
    }

    if (!int_const_fits_expected(raw, target, negated, bare_literal)) {
        type_error(checker, expr->pos, "constant %lld overflows %s",
                   (long long)(int64_t)raw, type_to_string(target));
        return -1;
    }
    return 1;
}

// Expression switch: type-check the tag, then every case expression and
// clause body. Case bodies are raw statement lists (linked via next), so
// they are walked here rather than dispatched as blocks. Each clause gets
// its own scope, matching Go's per-clause scoping.
//
// B3 fix (correctness-burndown arc 2, task 2): a switch is definitionally a
// chain of `tag == case` equality tests (see codegen_generate_switch_stmt's
// own doc comment), but until this fix the checker never related a case
// expression's type to the tag's at all — it merely type-checked each case
// expression IN ISOLATION. Two failure modes followed, both reaching
// codegen and crashing the LLVM verifier with a raw, unpositioned
// "Module verification failed" instead of a clean diagnostic:
//   1. A rune/int32 tag against a char-literal case (`'\a'`, `'\n'`, ...):
//      the lexer bridge emits every char/rune literal as a plain TOKEN_INT
//      carrying its decimal value (see lexer.c's `'\''` arm), which
//      type_check_literal defaults to int64 — the same untyped-constant
//      default a bare `10` gets. The tag stays i32 (rune); codegen ended up
//      comparing `icmp eq i32 %r, i64 10` (this bug's exact reported
//      symptom).
//   2. A wrong-KIND case value (`case "x":` on an int tag) or a non-constant
//      wrong-WIDTH case value (a typed int64 variable against a rune/int32
//      tag) sailed through unchecked and crashed codegen's switch lowering
//      with a mismatched-operand-type icmp the same way.
//
// Fix: for every case expression whose type's `kind` differs from the tag's,
// route it through the SAME machinery a `tag == case` comparison already
// uses. An untyped int constant (kind mismatch, const-rooted) unifies with
// the tag's type iff representable — reusing int_const_fits_expected/
// is_negated_int_const_expr, the exact representability gate Task 1 added
// for `return`'s untyped-constant coercion, so `case 300:` on an int8 tag
// still rejects as overflow instead of truncating. A non-constant int-kind
// mismatch, or any other kind mismatch (string/bool/etc. against a
// differently-kinded tag), is rejected via type_check_comparison_op — the
// exact function `==` itself calls, so the diagnostic ("Cannot compare
// incompatible types %s and %s") is worded identically to what `tag == case`
// would produce by hand.
//
// A tagless switch (`switch { case cond: }` — goostd/strconv.go's own
// workaround for this bug, see appendEscapedRune's comment) has no tag to
// unify against (sw->tag is NULL); tag_type stays NULL and every case
// expression's own (already-checked) bool type is left untouched, exactly
// as before this fix — the tagless-switch shape is not a wall this task
// adds.
//
// Correctness-followups arc 3, task 4 — two independent additions layered
// onto the fix above, both scoped to what this function can already see:
//
// Mandate A (duplicate constant case values): Go rejects two case clauses
// whose constant VALUES coincide, even after folding (`case 1:` and
// `case - -1:` both denote 1). This checker never tracked that at all —
// first-match-wins silently swallowed the second clause all the way to
// codegen. Fix: for an INTEGER-tag switch (type_is_integer(tag_type)),
// fold every case expression through the same goo_fold_const_int the
// int-int representability branch below already calls, and flag a second
// occurrence of an already-seen folded value. Scoped to integer constants
// only, matching goo_fold_const_int's own literal-only reach:
//   - STRING case duplicates (`case "a" + "b":` vs `case "ab":`) are NOT
//     detected here even though a value-level folder exists
//     (goo_fold_const_string, expression_helpers.c) — extending it would
//     need its own collision table (string equality, not integer identity)
//     and its own diagnostic shape, both outside this task's explicit
//     ask (the `%lld`-valued "duplicate case value" message below). Go
//     rejects string-case duplicates too; this is an honest scope gap,
//     not a claim that strings can't be compared.
//   - FLOAT-literal case duplicates (`case 2.5:` twice) are NOT detected:
//     no untyped-float constant folder exists in this codebase (only
//     goo_fold_const_int, integer-only — see type_check_return_stmt's
//     float_const_coerce comment above for the same documented gap), and
//     this task does not add one.
//   - An int-CONSTANT case under a FLOAT tag (`switch f float64 { case 2:
//     case 2: }`, task 3's stamped-to-float shape) is NOT covered: the
//     gate below requires type_is_integer(tag_type), so float-tag switches
//     skip dup detection entirely (first-match-wins, silently). Known
//     narrower-than-ideal scope — folding the still-integer-shaped case
//     ASTs would work mechanically, but float-tag dup semantics belong
//     with a future untyped-float folder so int-vs-float dup collisions
//     (`case 2:` vs `case 2.0:`) are decided once, not half-here.
// A duplicate's diagnostic fires at the SECOND occurrence with a
// cross-reference position, house style shared with ownership_checker.c's
// "Use of moved variable '%s' (moved at %s:%d:%d)".
//
// The dup-tracking table is a fixed-capacity flat array scoped to a single
// call of this function (stack-local, reset per switch — nested switches
// each get their own via ordinary recursion), matching lane_ownership.c's
// documented flat-table convention for small per-construct tables: no
// heap allocation, so xmalloc/xrealloc do not apply here; past the cap it
// silently stops tracking additional values (an under-detect — a missed
// duplicate, never a false one — the same safe direction lane_ownership.c
// chose for its own per-function tables).
//
// Mandate B (struct-typed switch tag wall): codegen_generate_switch_stmt
// (statement_codegen.c) special-cases string tags (goo_string_eq) and
// float tags (FCmp) but falls through to a plain LLVMBuildICmp(IntEQ) for
// anything else — including a struct-typed tag, which is neither an
// integer nor a pointer LLVM operand. Empirically confirmed at this arc's
// HEAD: `switch p { case other: }` (p, other both `Point` structs, a
// COMPARABLE struct type) crashes module verification with "Invalid
// operand types for ICmp instruction  %switch.cmp = icmp eq %Point ...",
// and a struct with a slice field (non-comparable) crashes identically —
// struct-tag switch lowering is simply unwritten, for comparable and
// non-comparable structs alike. This is a v1 WALL for an unimplemented
// feature, NOT Go parity: Go allows switching on a comparable struct
// value (and would itself reject a non-comparable one, or panic on `==`
// at runtime for the interface-boxed case). A future lowering task should
// REMOVE this wall — likely routing struct-tag comparison through the
// same field-by-field equality codegen.c's boxed-`any` equality path
// already builds for comparable structs — rather than treat the wall as
// spec.
int type_check_switch_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_SWITCH_STMT) return 0;

    SwitchStmtNode* sw = (SwitchStmtNode*)stmt;
    int ok = 1;
    Type* tag_type = NULL;
    if (sw->tag) {
        tag_type = type_check_expression(checker, sw->tag);
        if (!tag_type) ok = 0;
    }

    // Mandate B: wall off a struct-typed tag before any case is examined —
    // see the doc comment above. Falls through (does not early-return) so
    // clause bodies still get checked, matching this function's existing
    // cascade-suppression convention (a bad tag doesn't block reporting
    // unrelated errors inside case bodies).
    if (tag_type && !type_is_poison(tag_type) && tag_type->kind != TYPE_UNKNOWN &&
        tag_type->kind == TYPE_STRUCT) {
        if (type_struct_fields_comparable(tag_type)) {
            type_error(checker, sw->tag->pos,
                       "switch on a struct-typed value is not supported "
                       "(v1 limitation: struct-tag switch lowering is not "
                       "implemented; Go allows this for a comparable struct)");
        } else {
            type_error(checker, sw->tag->pos,
                       "switch on a struct-typed value is not supported "
                       "(%s is additionally non-comparable, so Go would "
                       "reject this too)",
                       type_to_string(tag_type));
        }
        ok = 0;
    }

    // Mandate A's per-switch dup table — see the doc comment above.
#define SWITCH_DUP_CASE_MAX_VALUES 64
    uint64_t dup_values[SWITCH_DUP_CASE_MAX_VALUES];
    Position dup_pos[SWITCH_DUP_CASE_MAX_VALUES];
    size_t dup_count = 0;

    for (ASTNode* c = sw->cases; c; c = c->next) {
        CaseClauseNode* clause = (CaseClauseNode*)c;
        for (ASTNode* e = clause->exprs; e; e = e->next) {
            Type* e_type = type_check_expression(checker, e);
            if (!e_type) { ok = 0; continue; }

            // No tag (tagless switch), or an unresolved/poisoned operand on
            // either side (T4.2 cascade-suppression convention used
            // throughout this file): defer rather than risk a false-positive
            // reject on something the checker couldn't pin down.
            if (!tag_type || type_is_poison(tag_type) || type_is_poison(e_type) ||
                tag_type->kind == TYPE_UNKNOWN || e_type->kind == TYPE_UNKNOWN) {
                continue;
            }

            // Mandate A: fold + dup-check BEFORE the kind-equality early-out
            // just below — the exact RED shape (`case 1:` then `case 1:` on
            // a plain int tag) has tag_type->kind == e_type->kind and would
            // otherwise never reach the fold at all. Gated on an
            // INTEGER-kind tag (see the doc comment's scope list); a
            // successful fold means `e` is one of goo_fold_const_int's
            // literal-or-constant-arithmetic shapes, so this never
            // misfires on a non-constant case expression.
            if (type_is_integer(tag_type)) {
                uint64_t raw;
                if (goo_fold_const_int(e, &raw)) {
                    size_t prev = dup_count; // index of a match, if any
                    for (size_t k = 0; k < dup_count; k++) {
                        if (dup_values[k] == raw) { prev = k; break; }
                    }
                    if (prev < dup_count) {
                        Position pp = dup_pos[prev];
                        type_error(checker, e->pos,
                                   "duplicate case value %lld (previous case "
                                   "at %s:%d:%d)",
                                   (long long)(int64_t)raw,
                                   pp.filename ? pp.filename : "<unknown>",
                                   pp.line, pp.column);
                        ok = 0;
                    } else if (dup_count < SWITCH_DUP_CASE_MAX_VALUES) {
                        dup_values[dup_count] = raw;
                        dup_pos[dup_count] = e->pos;
                        dup_count++;
                    }
                }
            }

            if (tag_type->kind == e_type->kind) continue; // already comparable today

            if (type_is_integer(tag_type) && type_is_integer(e_type)) {
                if (is_untyped_int_const_expr(e)) {
                    uint64_t raw;
                    int negated = is_negated_int_const_expr(e);
                    int bare_literal = is_bare_int_literal(e);
                    if (goo_fold_const_int(e, &raw) &&
                        !int_const_fits_expected(raw, tag_type, negated,
                                                  bare_literal)) {
                        type_error(checker, e->pos, "constant %lld overflows %s",
                                   (long long)(int64_t)raw, type_to_string(tag_type));
                        ok = 0;
                        continue;
                    }
                    // Representable (or the fold itself failed — left
                    // unchecked, the same documented gap int_const_fits_
                    // expected's other caller carries): stamp the whole
                    // case-expr subtree to the tag's type so codegen emits
                    // the constant at that width directly, closing the
                    // width-mismatched ICmp this fix targets.
                    stamp_int_const_expr_type(e, tag_type);
                    continue;
                }
                // Non-constant int-kind mismatch (e.g. a typed int64
                // variable used as a case value against a rune/int32 tag):
                // only an untyped constant adapts (Go representability
                // rule); a differently-sized TYPED value is not assignable
                // to the tag's type and must not reach codegen, which has
                // no lowering for a width-mismatched icmp.
                type_error(checker, e->pos,
                           "invalid case value: %s does not match switch "
                           "expression type %s",
                           type_to_string(e_type), type_to_string(tag_type));
                ok = 0;
                continue;
            }

            // Untyped int constant case against a FLOAT tag (`switch f {
            // case 1: }`, f float64/float32 — correctness-followups arc 3,
            // task 3; same v1 rule as type_check_return_stmt's
            // float_const_coerce above, see its doc comment for the
            // representability rationale). No int_const_fits_expected-style
            // overflow check applies (unlike the int-int branch just above)
            // — accept unconditionally and stamp so codegen's switch
            // lowering compares two floats (fcmp) instead of an int64 case
            // value against a float tag.
            if (type_is_float(tag_type) && is_untyped_int_const_expr(e)) {
                stamp_int_const_expr_type(e, tag_type);
                continue;
            }

            // Any other kind mismatch (e.g. a string case against an int tag)
            // — reject via the same check `tag == case` would perform.
            if (!type_check_comparison_op(checker, tag_type, e_type, TOKEN_EQ, e->pos)) {
                ok = 0;
            }
        }
        scope_push(checker);
        if (!type_check_switch_like_body(checker, clause->body,
                FALLTHROUGH_CTX_EXPR_SWITCH, c->next == NULL)) ok = 0;
        scope_pop(checker);
    }
    return ok;
}
#undef SWITCH_DUP_CASE_MAX_VALUES

int type_check_go_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_GO_STMT) return 0;
    
    GoStmtNode* go_stmt = (GoStmtNode*)stmt;
    
    // Type check the function call expression
    if (go_stmt->call) {
        Type* call_type = type_check_expression(checker, go_stmt->call);
        if (!call_type) return 0;
        
        // TODO: Validate that the expression is a function call
    }
    
    return 1;
}

int type_check_defer_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_DEFER_STMT) return 0;

    DeferStmtNode* defer_stmt = (DeferStmtNode*)stmt;

    // The grammar only produces `defer call_expr`, so `call` is a call
    // expression. Type-check it so argument types/arity are validated at the
    // defer site. Codegen snapshots the arguments here (defer-time evaluation,
    // Go semantics) and emits the call, guarded by a runtime active flag, at
    // each function-exit path.
    if (defer_stmt->call) {
        if (defer_stmt->call->type != AST_CALL_EXPR) {
            type_error(checker, stmt->pos, "defer requires a function call");
            return 0;
        }
        Type* call_type = type_check_expression(checker, defer_stmt->call);
        if (!call_type) return 0;
    }

    return 1;
}

int type_check_select_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_SELECT_STMT) return 0;

    SelectStmtNode* select_stmt = (SelectStmtNode*)stmt;

    int ok = 1;
    for (ASTNode* case_node = select_stmt->cases; case_node; case_node = case_node->next) {
        if (case_node->type != AST_SELECT_CASE) continue;
        SelectCaseNode* sc = (SelectCaseNode*)case_node;

        // Select-case bodies are a raw statement chain (grammar: CASE expression
        // COLON statement_list — no block wrapper), so unlike an if/for body
        // there is no nested type_check_block_stmt to push/pop a scope. Do it
        // here ourselves, matching the AST_SWITCH_STMT clause loop above
        // (per-case scoping, Go semantics).
        scope_push(checker);

        // comm == NULL is the default case — body only.
        if (sc->comm) {
            // gofmt-syntax-b Task 4 (P1.10): value-binding cases (`case v :=
            // <-ch:` / `case v = <-ch:`) and the always-rejected comma-ok
            // shape (`case v, ok := <-ch:`). Checked BEFORE the pre-existing
            // send/receive dispatch below so those two branches stay
            // byte-identical for every case this arc's fixtures already
            // exercise (bind_name == NULL, is_declare == 0 for all of them).
            if (sc->is_declare == -1) {
                // v1 scope cut (reworded after close() shipped in P3.1):
                // plumbing per-case ok status through goo_select_case_t and
                // the select lowering is deferred — rider R1 in the P3
                // sub-A design doc. The single-value form is Go-correct on
                // a closed channel (fires with the zero value), so the
                // workaround is a comma-ok receive outside select.
                type_error(checker, case_node->pos,
                           "select case 'v, ok :=' binding is not supported in v1; "
                           "use a comma-ok receive outside select to detect closure");
                ok = 0;
            } else if (sc->bind_name) {
                // The grammar accepts any `expression` after `:=`/`=` (kept
                // zero-new-surface); receive-ness is validated HERE, not in
                // the grammar, so the diagnostic can name the real problem.
                if (!(sc->comm->type == AST_UNARY_EXPR &&
                      ((UnaryExprNode*)sc->comm)->operator == TOKEN_ARROW)) {
                    type_error(checker, sc->comm->pos,
                               "select case must be a receive operation");
                    ok = 0;
                } else {
                    // Routes through the general expression checker exactly
                    // like the plain (unbound) receive branch below —
                    // validates channel-ness via type_check_channel_receive_op
                    // and stamps sc->comm->node_type to the element type,
                    // which this reuses directly as elem_type.
                    Type* elem_type = type_check_expression(checker, sc->comm);
                    if (!elem_type) {
                        ok = 0;
                    } else if (sc->is_declare) {
                        // `:=` — declare bind_name fresh, scoped to this
                        // case's body (the scope_push above/scope_pop below).
                        // `_` is a discard, like every other short-decl form.
                        if (strcmp(sc->bind_name, "_") != 0) {
                            Variable* var = variable_new(sc->bind_name, elem_type, case_node->pos);
                            if (var) {
                                var->is_initialized = 1;
                                if (!scope_add_variable(checker->current_scope, var)) {
                                    variable_free(var);
                                }
                            }
                        }
                    } else {
                        // `=` — bind_name must already be a declared,
                        // type-compatible variable in an enclosing scope
                        // (scope_lookup_variable walks the parent chain).
                        Variable* existing = scope_lookup_variable(checker->current_scope, sc->bind_name);
                        if (!existing) {
                            type_error(checker, case_node->pos,
                                       "select case: undefined variable '%s'", sc->bind_name);
                            ok = 0;
                        } else if (existing->type && existing->type->kind == TYPE_INTERFACE) {
                            // An interface-typed target accepts any concrete
                            // implementer (check_interface_assign emits its
                            // own diagnostic) — mirrors the ordinary `x = e`
                            // assignment path (type_check_assignment_op) so
                            // `case v = <-ch:` behaves the same as any other
                            // assignment into an interface variable.
                            if (!check_interface_assign(checker, elem_type, existing->type, case_node->pos)) {
                                ok = 0;
                            }
                        } else if (!type_compatible(elem_type, existing->type)) {
                            type_error(checker, case_node->pos,
                                       "select case: cannot assign %s to %s variable '%s'",
                                       type_to_string(elem_type), type_to_string(existing->type),
                                       sc->bind_name);
                            ok = 0;
                        }
                    }
                }
            } else if (sc->comm->type == AST_BINARY_EXPR &&
                ((BinaryExprNode*)sc->comm)->operator == TOKEN_ARROW) {
                // Send comm: ch <- value. codegen_setup_select_case (statement_
                // codegen.c) evaluates left/right individually rather than the
                // whole binary node, so we mirror that instead of routing the
                // comm through type_check_expression/type_check_channel_send_op
                // (whose "Cannot send ..." message the reject-probe doesn't
                // look for); "select send" identifies this diagnostic as the
                // select-specific one.
                BinaryExprNode* send = (BinaryExprNode*)sc->comm;
                Type* chan_t = type_check_expression(checker, send->left);
                if (!chan_t) {
                    ok = 0;
                } else if (chan_t->kind != TYPE_CHANNEL) {
                    type_error(checker, send->left->pos,
                               "select send requires a channel, got %s", type_to_string(chan_t));
                    ok = 0;
                } else {
                    Type* val_t = type_check_expression(checker, send->right);
                    Type* elem_t = chan_t->data.channel.element_type;
                    if (!val_t) {
                        ok = 0;
                    // Fix 2: sibling of the TOKEN_ARROW gate in
                    // type_check_binary_expr — this select-send comm path
                    // checks left/right individually and never routes
                    // through that case, so it needs its own gate.
                    } else if (!reject_comptime_function_value(
                                   checker, send->right, val_t,
                                   send->right->pos, "sent on a channel")) {
                        ok = 0;
                    // Task 1 (chan-send representability, arc 3; const-
                    // identifier extension, arc 4 item (j)): this comm path
                    // never routes through type_check_channel_send_op either
                    // (see the comment above this block), so it calls the
                    // same shared representability gate that site does —
                    // chan_send_const_int_gate, deduped from the two former
                    // inline copies (see its doc comment for the case
                    // classes). Not-applicable (0) falls through to the
                    // select-specific type_compatible diagnostic below.
                    } else {
                        int gate = chan_send_const_int_gate(checker, send->right,
                                                            val_t, elem_t);
                        if (gate < 0) {
                            ok = 0;
                        } else if (gate == 0 && !type_compatible(val_t, elem_t)) {
                            type_error(checker, send->right->pos,
                                       "select send: cannot use %s as %s channel element",
                                       type_to_string(val_t),
                                       type_to_string(elem_t));
                            ok = 0;
                        }
                    }
                }
            } else if (sc->comm->type == AST_UNARY_EXPR &&
                       ((UnaryExprNode*)sc->comm)->operator == TOKEN_ARROW) {
                // Receive comm: <-ch. The select_case grammar rule's comm slot is
                // an `expression`, not a `simple_stmt`, so a binding form like
                // `got := <-ch` cannot appear here — it's a plain unary receive.
                // Route through the general expression checker, which already
                // validates channel-ness via type_check_channel_receive_op and
                // annotates the channel operand (handles conversions etc. inside
                // it the same way any other expression would).
                if (!type_check_expression(checker, sc->comm)) ok = 0;
            } else {
                type_error(checker, sc->comm->pos,
                           "select case requires a channel send or receive operation");
                ok = 0;
            }
        }

        // Body: walk the statement chain like the AST_SWITCH_STMT clause loop
        // does (not a single type_check_statement dispatch — select-case bodies
        // are never wrapped in an AST_BLOCK_STMT). `fallthrough` is never
        // legal in a select case regardless of clause position, so
        // is_last_clause's value here is immaterial (kind !=
        // FALLTHROUGH_CTX_EXPR_SWITCH always wins first).
        if (!type_check_switch_like_body(checker, sc->body,
                FALLTHROUGH_CTX_SELECT, 0)) ok = 0;

        scope_pop(checker);
    }
    return ok;
}

// Forward declarations for helper functions
Type* type_from_ast(TypeChecker* checker, ASTNode* type_node);
Type* type_check_expression(TypeChecker* checker, ASTNode* expr);

// Type assertions branch, Task 3: duplicate-case-type comparison.
// `type_equals` (types.c) falls back to `default: return 1` (kind-only
// equality) for any kind it doesn't explicitly special-case — which
// includes TYPE_STRUCT, so it treats ANY two structs as equal (verified
// live: it falsely flagged Sq/Rect/Circle/Triangle as duplicates of each
// other in this task's own golden). This is a real, pre-existing gap;
// RECORDED rather than fixed here — type_equals is shared by several
// unrelated checkers (channel_checker.c, constraint_inference.c,
// protocol_oriented_programming.c) whose reliance on that fallback is out
// of this task's scope to audit. For case-type duplicate detection
// specifically, comparing by type_receiver_name (the SAME string that
// keys a concrete's vtable global, goo.vtable.<T>.<I> — see
// interface_codegen.c) is not just a workaround but the semantically
// correct notion of "same case" here: two case types are truly duplicates
// iff they'd resolve to the same vtable global at runtime. Falls back to
// type_equals for nameless kinds (builtins etc.), where its kind-only
// default is correct.
static int type_switch_case_type_same(Type* a, Type* b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return 0;
    const char* na = type_receiver_name(a);
    const char* nb = type_receiver_name(b);
    if (na || nb) return na && nb && strcmp(na, nb) == 0;
    return type_equals(a, b);
}

// Type assertions branch, Task 3: `switch [v :=] x.(type) { case T1: … case
// Tn, Tm: … default: … }`. Operand must be interface-typed (same "invalid
// type assertion: operand is not an interface type" rejection x.(T) uses).
// A CONCRETE case type must satisfy the operand interface
// (type_interface_satisfied — same "impossible type assertion" message
// shape as x.(T)); an INTERFACE case type (interface-target RTTI, Task 3 of
// that follow-on plan) skips that static check — satisfaction is verified
// at runtime by codegen_interface_target_match's closed-world vtable-
// descriptor chain instead. Duplicate case types anywhere in the switch are
// rejected, and at most one `default`. Bound var `v`'s type is the single
// case type in a single-type case (concrete OR interface), else the
// operand's own interface type (multi-type case, `default`, or a bare
// `case nil:` — nil has no Type* to bind to, so it never triggers the
// single-type rule even when it is the clause's only list entry),
// introduced into that case's OWN scope, mirroring the AST_SWITCH_STMT
// clause loop's per-case scope_push/scope_pop above.
int type_check_type_switch_stmt(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || stmt->type != AST_TYPE_SWITCH) return 0;

    TypeSwitchNode* tsw = (TypeSwitchNode*)stmt;

    Type* iface_type = type_check_expression(checker, tsw->expr);
    if (!iface_type) return 0;
    if (iface_type->kind != TYPE_INTERFACE) {
        type_error(checker, stmt->pos,
                   "invalid type assertion: operand is not an interface type");
        return 0;
    }
    const char* bind_name = tsw->bind_name ? ((IdentifierNode*)tsw->bind_name)->name : NULL;

    int ok = 1;
    int default_count = 0;
    // Dup-case-type tracking across the WHOLE switch (not just within one
    // clause) — Go rejects `case T: … case T:` in separate clauses exactly
    // like `case T, T:` in one. A fixed-size buffer is a deliberate
    // simplification (mirrors this file's existing bounded-buffer
    // convention, e.g. the for-stmt multi-name array above): realistic
    // switches never approach 128 case types, and overflow degrades to
    // "duplicate detection stops," never a crash.
    Type* seen_types[128];
    size_t seen_count = 0;
    int seen_nil = 0;

    for (ASTNode* c = tsw->cases; c; c = c->next) {
        TypeCaseNode* clause = (TypeCaseNode*)c;

        if (!clause->types) {
            default_count++;
            if (default_count > 1) {
                type_error(checker, c->pos, "multiple defaults in type switch");
                ok = 0;
            }
        }

        size_t case_type_count = 0;
        for (ASTNode* t = clause->types; t; t = t->next) case_type_count++;

        for (ASTNode* t = clause->types; t; t = t->next) {
            int is_nil = (t->type == AST_LITERAL && ((LiteralNode*)t)->literal_type == TOKEN_NIL);
            if (is_nil) {
                if (seen_nil) {
                    type_error(checker, t->pos, "duplicate case nil in type switch");
                    ok = 0;
                }
                seen_nil = 1;
                continue;
            }

            Type* case_type = type_from_ast(checker, t);
            if (!case_type) {
                type_error(checker, t->pos, "invalid type switch case: cannot resolve type");
                ok = 0;
                continue;
            }
            // Interface-target RTTI, Task 3: `case I:` where I is itself an
            // interface is runtime-checked (codegen_interface_target_match's
            // closed-world vtable-descriptor chain), not statically provable
            // the way a concrete case's type_interface_satisfied call below
            // is — skip that concrete-satisfaction rejection entirely for an
            // interface case type. Everything else (dup detection, node_type
            // stamp-back) still applies uniformly.
            if (case_type->kind != TYPE_INTERFACE) {
                const char* method = NULL;
                const char* reason = NULL;
                if (!type_interface_satisfied(checker, iface_type, case_type, &method, &reason)) {
                    if (reason && strcmp(reason, "comptime") == 0) {
                        report_comptime_method_not_satisfied(checker, t->pos, method);
                        ok = 0;
                        continue;
                    }
                    const char* iname = iface_type->data.interface.name
                                             ? iface_type->data.interface.name : "interface";
                    const char* cname = type_receiver_name(case_type);
                    type_error(checker, t->pos,
                        "impossible type assertion: %s does not implement %s (%s method %s)",
                        cname ? cname : type_to_string(case_type), iname,
                        reason ? reason : "missing", method ? method : "?");
                    ok = 0;
                    continue;
                }
            }
            int dup = 0;
            for (size_t i = 0; i < seen_count; i++) {
                if (type_switch_case_type_same(seen_types[i], case_type)) { dup = 1; break; }
            }
            if (dup) {
                const char* dname = type_receiver_name(case_type);
                type_error(checker, t->pos, "duplicate case type %s in type switch",
                          dname ? dname : type_to_string(case_type));
                ok = 0;
            } else if (seen_count < 128) {
                seen_types[seen_count++] = case_type;
            }

            // Read back by codegen instead of re-resolving (type_from_ast is
            // NOT idempotent-cheap for every type kind, and this mirrors the
            // "stamp once, read back" convention the comma-ok map/assert
            // paths already use).
            t->node_type = case_type;
        }

        scope_push(checker);
        if (bind_name) {
            Type* bind_type = iface_type;
            int single_concrete = (case_type_count == 1 && clause->types &&
                clause->types->type != AST_LITERAL);
            if (single_concrete) {
                bind_type = clause->types->node_type;
            }
            Variable* v = variable_new(bind_name, bind_type, c->pos);
            if (v) {
                v->is_initialized = 1;
                scope_add_variable(checker->current_scope, v);
            }
        }
        if (!type_check_switch_like_body(checker, clause->body,
                FALLTHROUGH_CTX_TYPE_SWITCH, c->next == NULL)) ok = 0;
        scope_pop(checker);
    }

    return ok;
}

// Struct-typed map keys (Task 2): is `t` (already known TYPE_STRUCT) usable
// as a map key? Recursively — every field must be a scalar
// (integer/bool/char/pointer), a float, a string, or a nested comparable
// struct. An ARRAY field is Go-legal (arrays are comparable if their element
// type is) but deferred to a later cycle: the synthesized comparator
// (codegen_get_or_emit_struct_key_eq) has no per-element loop yet. A
// slice/map/func/interface field is never comparable in Go. On rejection,
// `*why` is set to "array" or "noncomparable" so the AST_MAP_TYPE gate below
// can choose the matching diagnostic (deferred vs. permanently rejected).
// Returns 1 iff `t` is a valid map key type.
static int struct_is_comparable_key(Type* t, const char** why) {
    if (!t || t->kind != TYPE_STRUCT) return 0;
    for (size_t i = 0; i < t->data.struct_type.field_count; i++) {
        Type* f = t->data.struct_type.fields[i].type;
        if (!f) return 0;
        switch (f->kind) {
            case TYPE_STRING: case TYPE_BOOL: case TYPE_CHAR:
            case TYPE_FLOAT32: case TYPE_FLOAT64: case TYPE_POINTER:
                break;
            case TYPE_STRUCT:
                if (!struct_is_comparable_key(f, why)) return 0;
                break;
            case TYPE_ARRAY:
                *why = "array";   // Go-legal, deferred (v1 has no element loop)
                return 0;
            default:
                if (type_is_integer(f)) break;
                *why = "noncomparable";  // slice/map/func/interface/...
                return 0;
        }
    }
    return 1;
}

// Helper function to convert AST type nodes to Type structures
Type* type_from_ast(TypeChecker* checker, ASTNode* type_node) {
    if (!checker || !type_node) return NULL;
    
    switch (type_node->type) {
        case AST_IDENTIFIER: {
            // Handle type identifiers (for make_chan, etc.)
            IdentifierNode* ident = (IdentifierNode*)type_node;

            // P4.2/B1 audit: a qualified `pkg.Type` name can never arrive
            // here. IdentifierNode wraps exactly ONE `identifier` token;
            // every producer of an AST_IDENTIFIER type-position node builds
            // it from a single bare `identifier`, never `identifier DOT
            // identifier` — that two-token shape is reachable only through
            // parser.y's type_name rule, which mints a BasicTypeNode (the
            // AST_BASIC_TYPE arm below), not an IdentifierNode. No
            // basic->package-style handling is needed or possible here.

            // Map basic type names to TypeKind
            if (strcmp(ident->name, "void") == 0) return type_checker_get_builtin(checker, TYPE_VOID);
            if (strcmp(ident->name, "bool") == 0) return type_checker_get_builtin(checker, TYPE_BOOL);
            if (strcmp(ident->name, "int8") == 0) return type_checker_get_builtin(checker, TYPE_INT8);
            if (strcmp(ident->name, "int16") == 0) return type_checker_get_builtin(checker, TYPE_INT16);
            if (strcmp(ident->name, "int32") == 0) return type_checker_get_builtin(checker, TYPE_INT32);
            if (strcmp(ident->name, "rune") == 0) return type_checker_get_builtin(checker, TYPE_INT32); // Go: rune = int32
            if (strcmp(ident->name, "int64") == 0) return type_checker_get_builtin(checker, TYPE_INT64);
            if (strcmp(ident->name, "int") == 0) return type_checker_get_builtin(checker, TYPE_INT64);  // Default int (Go: int is 64-bit here)
            if (strcmp(ident->name, "uint8") == 0) return type_checker_get_builtin(checker, TYPE_UINT8);
            if (strcmp(ident->name, "uint16") == 0) return type_checker_get_builtin(checker, TYPE_UINT16);
            if (strcmp(ident->name, "uint32") == 0) return type_checker_get_builtin(checker, TYPE_UINT32);
            if (strcmp(ident->name, "uint64") == 0) return type_checker_get_builtin(checker, TYPE_UINT64);
            if (strcmp(ident->name, "uint") == 0) return type_checker_get_builtin(checker, TYPE_UINT64);  // Default uint (Go: uint is 64-bit here)
            if (strcmp(ident->name, "float32") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT32);
            if (strcmp(ident->name, "float64") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT64);
            if (strcmp(ident->name, "float") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT64);  // Default float
            if (strcmp(ident->name, "string") == 0) return type_checker_get_builtin(checker, TYPE_STRING);
            if (strcmp(ident->name, "char") == 0) return type_checker_get_builtin(checker, TYPE_CHAR);
            if (strcmp(ident->name, "byte") == 0) return type_checker_get_builtin(checker, TYPE_UINT8);
            // F8: Go's `error` interface. v1 represents it as a nullable
            // pointer — nameable in signatures, accepts nil, and `== nil`
            // works. Method dispatch (`.Error()`) is deferred to Phase 6.
            if (strcmp(ident->name, "error") == 0)
                return type_checker_error_type(checker);
            // Go 1.18+ predeclared `any` = the empty interface — see
            // type_checker_any_type above. Interface-typed map keys (Task 2)
            // is the first feature that needs `any` to resolve as a type.
            if (strcmp(ident->name, "any") == 0)
                return type_checker_any_type();

            // Function generics Task 3: a bare `T` in a generic function's
            // signature/body may parse as AST_IDENTIFIER (this branch) or
            // AST_BASIC_TYPE (below) depending on context. Check the
            // active-type-param stack BEFORE the user-named-type lookup
            // below: a type parameter must shadow a package-level type of
            // the same name (Go semantics — `func Id[T any](x T) T` binds
            // `T` to the type parameter even if `type T struct{...}` exists
            // at package scope). `active_type_params` is empty outside a
            // generic declaration, so this is a no-op for ordinary
            // (non-generic) type resolution.
            Type* tp_ident = type_checker_lookup_type_param(checker, ident->name);
            if (tp_ident) return tp_ident;

            // User-defined named type (e.g. `new(Point)`): `type Foo ...` is
            // registered as a Variable whose `type` field is the named Type
            // (see the AST_BASIC_TYPE branch below). Exclude package/function
            // names, which are not user types.
            Variable* named = type_checker_lookup_variable(checker, ident->name);
            if (named && named->type &&
                named->type->kind != TYPE_PACKAGE &&
                named->type->kind != TYPE_FUNCTION) {
                return named->type;
            }

            type_error(checker, type_node->pos, "Unknown type '%s'", ident->name);
            return NULL;
        }
        case AST_BASIC_TYPE: {
            BasicTypeNode* basic = (BasicTypeNode*)type_node;

            // P4.2/B1: qualified type name `pkg.Type` (basic->package set by
            // parser.y's type_name: identifier DOT identifier arm). Resolve
            // the package marker, then look up `name` in ITS exports scope
            // — mirrors the value-selector resolution in
            // type_check_selector_expr (expression_checker.c), but for a
            // TYPE position instead of an expression position.
            if (basic->package) {
                Variable* pkg_marker = type_checker_lookup_variable(checker, basic->package);
                if (!pkg_marker || !pkg_marker->type || pkg_marker->type->kind != TYPE_PACKAGE) {
                    type_error(checker, type_node->pos, "Unknown package '%s'", basic->package);
                    return NULL;
                }
                // A hardcoded stdlib-shim package (fmt, os, math, errors) is
                // seeded with a real Package* but an EMPTY exports scope (no
                // source was ever type-checked into it — see
                // seed_imported_stdlib_markers/is_stdlib_shim_import in
                // goo.c) — so the lookup below cleanly misses for every shim
                // symbol and falls through to the same "no exported type"
                // diagnostic, rather than crashing. A source package's
                // exports scope only ever holds CAPITALISED top-level names
                // (package_export_filter), so a lowercase `basic->name`
                // (`shapes.point`) also misses here and gets the identical
                // diagnostic — no separate "unexported" message is needed.
                Variable* exp = pkg_marker->package
                    ? scope_lookup_variable(pkg_marker->package->exports, basic->name)
                    : NULL;
                // Guard against a VALUE export (an exported package-level
                // var/const, or a function) silently resolving as a type —
                // e.g. `var x shapes.Version` where Version is `var Version
                // int`. type_check_type_decl is the ONLY registration path
                // that marks its Variable is_builtin=1 while giving it a
                // named struct/enum/interface/alias Type (see its "not a
                // real variable for use-tracking purposes" comment);
                // ordinary var/const/func declarations (bind_var_decl_name,
                // declare_function_signature) never set is_builtin. This is
                // the same TYPE_PACKAGE/TYPE_FUNCTION exclusion the
                // unqualified lookup below already applies, PLUS is_builtin
                // — required here because an exported plain-typed value
                // (e.g. a float64/int/string var) would otherwise pass the
                // kind-only check and silently typecheck as that scalar
                // type. Known residual imprecision (not exercised by this
                // task's scope): an exported enum VARIANT CONSTRUCTOR is
                // also is_builtin=1 with kind==TYPE_ENUM (the enum's own
                // Type, not a function type — see type_check_type_decl), so
                // it is indistinguishable from the enum type's own name by
                // this check alone; no discriminator field exists on
                // Variable for this narrower case.
                if (!exp || !exp->type || !exp->is_builtin ||
                    exp->type->kind == TYPE_PACKAGE || exp->type->kind == TYPE_FUNCTION) {
                    type_error(checker, type_node->pos,
                               "Package '%s' has no exported type '%s'",
                               basic->package, basic->name);
                    return NULL;
                }
                // Return the SHARED Type* as-is — NEVER clone. Codegen's
                // struct-cache is keyed on Type* pointer identity
                // (type_mapping.c); sharing this exact pointer across every
                // `shapes.Point` use (and with shapes' OWN internal uses) is
                // what makes cross-package struct layouts agree on one LLVM
                // struct type instead of silently diverging (P4 sub-B design
                // doc recon).
                return exp->type;
            }

            // Map basic type names to TypeKind
            if (strcmp(basic->name, "void") == 0) return type_checker_get_builtin(checker, TYPE_VOID);
            if (strcmp(basic->name, "bool") == 0) return type_checker_get_builtin(checker, TYPE_BOOL);
            if (strcmp(basic->name, "int8") == 0) return type_checker_get_builtin(checker, TYPE_INT8);
            if (strcmp(basic->name, "int16") == 0) return type_checker_get_builtin(checker, TYPE_INT16);
            if (strcmp(basic->name, "int32") == 0) return type_checker_get_builtin(checker, TYPE_INT32);
            if (strcmp(basic->name, "rune") == 0) return type_checker_get_builtin(checker, TYPE_INT32); // Go: rune = int32
            if (strcmp(basic->name, "int64") == 0) return type_checker_get_builtin(checker, TYPE_INT64);
            if (strcmp(basic->name, "int") == 0) return type_checker_get_builtin(checker, TYPE_INT64);  // Default int (Go: int is 64-bit here)
            if (strcmp(basic->name, "uint8") == 0) return type_checker_get_builtin(checker, TYPE_UINT8);
            if (strcmp(basic->name, "uint16") == 0) return type_checker_get_builtin(checker, TYPE_UINT16);
            if (strcmp(basic->name, "uint32") == 0) return type_checker_get_builtin(checker, TYPE_UINT32);
            if (strcmp(basic->name, "uint64") == 0) return type_checker_get_builtin(checker, TYPE_UINT64);
            if (strcmp(basic->name, "uint") == 0) return type_checker_get_builtin(checker, TYPE_UINT64);  // Default uint (Go: uint is 64-bit here)
            if (strcmp(basic->name, "float32") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT32);
            if (strcmp(basic->name, "float64") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT64);
            if (strcmp(basic->name, "float") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT64);  // Default float
            if (strcmp(basic->name, "string") == 0) return type_checker_get_builtin(checker, TYPE_STRING);
            if (strcmp(basic->name, "char") == 0) return type_checker_get_builtin(checker, TYPE_CHAR);
            if (strcmp(basic->name, "byte") == 0) return type_checker_get_builtin(checker, TYPE_UINT8);
            // F8: Go's `error` interface — see the AST_IDENTIFIER branch above.
            if (strcmp(basic->name, "error") == 0)
                return type_checker_error_type(checker);
            // Go 1.18+ predeclared `any` — see the AST_IDENTIFIER branch above.
            if (strcmp(basic->name, "any") == 0)
                return type_checker_any_type();

            // Function generics Task 3: see the analogous check in the
            // AST_IDENTIFIER branch above — a bare `T` can arrive as either
            // node kind. Must run BEFORE the user-named-type lookup below
            // so a type parameter shadows a package-level type of the same
            // name (Go semantics); `active_type_params` is empty outside a
            // generic declaration, so ordinary type resolution is unaffected.
            Type* tp_basic = type_checker_lookup_type_param(checker, basic->name);
            if (tp_basic) return tp_basic;

            // User-defined named type? type_check_type_decl registers
            // `type Foo = ...` aliases by piggybacking on the variable
            // scope (Variable whose `type` field IS the named Type).
            // Exclude TYPE_PACKAGE / TYPE_FUNCTION — those names came
            // from stdlib-package vars or function decls, not from
            // user `type` declarations.
            Variable* named = type_checker_lookup_variable(checker, basic->name);
            if (named && named->type &&
                named->type->kind != TYPE_PACKAGE &&
                named->type->kind != TYPE_FUNCTION) {
                return named->type;
            }

            type_error(checker, type_node->pos, "Unknown type '%s'", basic->name);
            return NULL;
        }
        
        case AST_ARRAY_TYPE: {
            ArrayTypeNode* array = (ArrayTypeNode*)type_node;
            Type* element_type = type_from_ast(checker, array->element_type);
            if (!element_type) return NULL;

            // Bare fixed-array type annotations (`var arr [3]int`, `owned
            // [1024]char`, `var arr [N]int`, `var arr [N+1]int`) carry the
            // length as an AST expression, not a resolved size_t. This used
            // to only evaluate a plain integer literal and silently fall
            // back to a fixed placeholder (10) for anything else — every
            // const-identifier or const-expression length silently got the
            // SAME wrong capacity, which also made per-element bounds checks
            // meaningless (an OOB index against the real N could still be
            // < 10 and never trip). goo_fold_const_int_ctx resolves both a
            // literal and a const-identifier/const-expression (recursively,
            // via each const's cached folded value — see
            // type_check_const_decl); a length that isn't a compile-time
            // integer constant at all is now a clean type error instead of
            // a silent wrong length (mirrors the array-literal length check
            // in expression_checker.c).
            uint64_t length64 = 0;
            if (!array->length || !goo_fold_const_int_ctx(checker, array->length, &length64)) {
                type_error(checker, type_node->pos,
                           "array length must be a constant expression");
                return NULL;
            }
            // Fix round 3 (minor 3): a folded NEGATIVE length previously
            // wrapped to a huge size_t and hung the compiler downstream
            // (LLVM array/zero-init of ~2^64 elements). Both routes land
            // here: the pre-existing `const N = -1; var buf [N]int`, and —
            // one token away since comptime value params — a comptime
            // instance re-derivation with `fill(-1, ...)` and `[n]int` in
            // the body (the instance-bound mirror Variable folds to -1).
            // A negative comptime value NOT used as an array length stays
            // legal — this check is length-position-only.
            if ((int64_t)length64 < 0) {
                type_error(checker, type_node->pos,
                           "array length must be non-negative");
                return NULL;
            }
            size_t length = (size_t)length64;
            Type* arr_t = type_array(element_type, length);
            // Fix round 4: a length expression referencing a comptime
            // parameter folded through that param's binding — the TEMPLATE
            // placeholder during body check, the REAL value during instance
            // re-derivation. Stamp the type either way so const-index and
            // literal-count validation know this length is per-instance:
            // the checker defers its upper-bound checks
            // (type_check_index_expr), and codegen's index paths enforce
            // against the instance's re-derived length instead
            // (codegen_generate_index_expr / codegen_emit_lvalue_address).
            if (arr_t && goo_expr_references_comptime_param(checker, array->length)) {
                // M-r5c: flag + diagnostic-name rewrite in one place.
                type_array_mark_comptime(arr_t, array->length);
            }
            return arr_t;
        }
        
        case AST_SLICE_TYPE: {
            SliceTypeNode* slice = (SliceTypeNode*)type_node;
            Type* element_type = type_from_ast(checker, slice->element_type);
            if (!element_type) return NULL;
            return type_slice(element_type);
        }

        case AST_STRUCT_TYPE: {
            // Build a TYPE_STRUCT directly. The struct's fields chain
            // is a list of VarDeclNode entries (one per field), since
            // the parser reuses VarDeclNode for struct fields (same
            // shape as a func parameter — name + type, no value).
            StructTypeNode* st = (StructTypeNode*)type_node;
            size_t count = 0;
            for (ASTNode* f = st->fields; f; f = f->next) {
                // Grouped fields `X, Y T` are one VarDeclNode with
                // name_count>1 but yield one struct field per name, so
                // count names, not VarDeclNodes.
                if (f->type == AST_VAR_DECL) count += ((VarDeclNode*)f)->name_count;
            }
            // Named-result ABI (P3-5): a parser-synthesized result tuple with a
            // SINGLE field is `func f() (r int)` — the common Go named-result
            // form (`(err error)`, `(n int)`). Collapse it to the field's scalar
            // type so callers see a plain scalar (not a 1-field aggregate) and an
            // explicit `return n+1` type-checks against the scalar. The name is
            // still bound as an in-scope local by the AST_STRUCT_TYPE walks in
            // type_check_function_decl / function_codegen (they key off the AST
            // node, which stays a struct). A >=2-field tuple keeps its struct ABI.
            if (st->is_result_tuple && count == 1) {
                for (ASTNode* f = st->fields; f; f = f->next) {
                    if (f->type != AST_VAR_DECL) continue;
                    VarDeclNode* fd = (VarDeclNode*)f;
                    return fd->type ? type_from_ast(checker, fd->type) : NULL;
                }
            }
            Type* result = type_new(TYPE_STRUCT);
            if (!result) return NULL;
            result->data.struct_type.field_count = count;
            result->data.struct_type.fields = count ? calloc(count, sizeof(StructField)) : NULL;
            size_t idx = 0;
            size_t total_size = 0;
            size_t max_align = 1;
            for (ASTNode* f = st->fields; f; f = f->next) {
                if (f->type != AST_VAR_DECL) continue;
                VarDeclNode* fd = (VarDeclNode*)f;
                if (fd->name_count == 0) {
                    type_error(checker, f->pos,
                               "internal: struct field with no name survived parsing");
                    free(result->data.struct_type.fields);
                    free(result);
                    return NULL;
                }
                Type* ft = fd->type ? type_from_ast(checker, fd->type) : NULL;
                if (!ft) {
                    free(result->data.struct_type.fields);
                    free(result);
                    return NULL;
                }
                if (fd->is_embedded) {
                    // Embedded member: must be a named type or pointer to one;
                    // interface embedding is a deferred feature, not an error
                    // of the user's making — say so specifically.
                    Type* base_t = ft;
                    if (base_t->kind == TYPE_POINTER)
                        base_t = base_t->data.pointer.pointee_type;
                    if (base_t && base_t->kind == TYPE_INTERFACE) {
                        type_error(checker, f->pos,
                                   "embedded interface types are not yet supported");
                        free(result->data.struct_type.fields);
                        free(result);
                        return NULL;
                    }
                    int named = base_t &&
                        ((base_t->kind == TYPE_STRUCT && base_t->data.struct_type.name) ||
                         base_t->name);
                    if (!named) {
                        type_error(checker, f->pos,
                                   "embedded field '%s' must be a named type or pointer to a named type",
                                   fd->names[0]);
                        free(result->data.struct_type.fields);
                        free(result);
                        return NULL;
                    }
                }
                // One StructField per name. Grouped fields `X, Y T` share the
                // single `ft` type here — the same way two separate `int`
                // fields already share type_from_ast's result. Each name still
                // gets its own offset so the layout is identical to writing the
                // fields out one per line.
                for (size_t k = 0; k < fd->name_count; k++) {
                    result->data.struct_type.fields[idx].name = strdup(fd->names[k]);
                    result->data.struct_type.fields[idx].type = ft;
                    result->data.struct_type.fields[idx].offset = total_size;
                    result->data.struct_type.fields[idx].is_embedded = fd->is_embedded;
                    total_size += ft->size ? ft->size : 8;
                    idx++;
                }
                if (ft->align > max_align) max_align = ft->align;
            }
            for (size_t a = 0; a < idx; a++) {
                for (size_t b = a + 1; b < idx; b++) {
                    if (strcmp(result->data.struct_type.fields[a].name,
                               result->data.struct_type.fields[b].name) == 0) {
                        type_error(checker, type_node->pos,
                                   "duplicate field name '%s' in struct",
                                   result->data.struct_type.fields[a].name);
                        free(result->data.struct_type.fields);
                        free(result);
                        return NULL;
                    }
                }
            }
            result->size = total_size;
            result->align = max_align;
            return result;
        }

        case AST_ENUM_TYPE: {
            EnumTypeNode* en = (EnumTypeNode*)type_node;
            size_t vcount = 0;
            for (ASTNode* v = en->variants; v; v = v->next)
                if (v->type == AST_ENUM_VARIANT) vcount++;

            Type* result = type_new(TYPE_ENUM);
            if (!result) return NULL;
            result->data.enum_type.variant_count = vcount;
            result->data.enum_type.variants =
                vcount ? calloc(vcount, sizeof(EnumVariant)) : NULL;

            size_t max_payload = 0, max_align = 4;
            size_t idx = 0;
            for (ASTNode* v = en->variants; v; v = v->next) {
                if (v->type != AST_ENUM_VARIANT) continue;
                EnumVariantNode* vn = (EnumVariantNode*)v;

                // Build the payload TYPE_STRUCT from vn->fields (same walk
                // as the AST_STRUCT_TYPE case above).
                size_t fcount = 0;
                for (ASTNode* f = vn->fields; f; f = f->next)
                    if (f->type == AST_VAR_DECL) fcount++;
                Type* payload = type_new(TYPE_STRUCT);
                if (!payload) { free(result->data.enum_type.variants); free(result); return NULL; }
                payload->data.struct_type.field_count = fcount;
                payload->data.struct_type.fields =
                    fcount ? calloc(fcount, sizeof(StructField)) : NULL;
                payload->data.struct_type.name = strdup(vn->name);
                size_t off = 0, palign = 1, fidx = 0;
                for (ASTNode* f = vn->fields; f; f = f->next) {
                    if (f->type != AST_VAR_DECL) continue;
                    VarDeclNode* fd = (VarDeclNode*)f;
                    if (fd->name_count == 0) continue;
                    if (fd->is_embedded) {
                        type_error(checker, f->pos,
                                   "embedded fields are not supported in enum variants");
                        free(payload->data.struct_type.name);
                        free(payload->data.struct_type.fields);
                        free(payload);
                        free(result->data.enum_type.variants);
                        free(result);
                        return NULL;
                    }
                    Type* ft = fd->type ? type_from_ast(checker, fd->type) : NULL;
                    if (!ft) {
                        free(payload->data.struct_type.name);
                        free(payload->data.struct_type.fields);
                        free(payload);
                        free(result->data.enum_type.variants);
                        free(result);
                        return NULL;
                    }
                    payload->data.struct_type.fields[fidx].name = strdup(fd->names[0]);
                    payload->data.struct_type.fields[fidx].type = ft;
                    payload->data.struct_type.fields[fidx].offset = off;
                    off += ft->size ? ft->size : 8;
                    if (ft->align > palign) palign = ft->align;
                    fidx++;
                }
                payload->size = off;
                payload->align = palign;

                result->data.enum_type.variants[idx].name = strdup(vn->name);
                result->data.enum_type.variants[idx].payload = payload;
                result->data.enum_type.variants[idx].tag = (int)idx;
                if (off > max_payload) max_payload = off;
                if (palign > max_align) max_align = palign;
                idx++;
            }
            // Layout: { i32 tag, [max_payload x i8] }. Pad tag to payload align.
            size_t tag_slot = (max_align > 4) ? max_align : 4;
            result->size = tag_slot + max_payload;
            result->align = max_align;
            return result;
        }

        case AST_INTERFACE_TYPE: {
            // P4-2: build a TYPE_INTERFACE from the method-signature list. Each
            // method is a bodyless FuncDeclNode (name + params + return_type);
            // turn it into an InterfaceMethod carrying a function Type so P4-3/4-4
            // can check satisfaction and dispatch.
            InterfaceTypeNode* it = (InterfaceTypeNode*)type_node;
            InterfaceMethod* head = NULL;
            InterfaceMethod* tail = NULL;
            size_t method_count = 0;

            for (ASTNode* m = it->methods; m; m = m->next) {
                if (m->type == AST_IDENTIFIER) {
                    // Embedded interface `Reader`: resolve the name to a
                    // TYPE_INTERFACE and union its methods into this set.
                    // Resolution uses the same named-type lookup as a normal
                    // type reference, so the embedded interface must be
                    // declared before this one (forward refs unsupported in
                    // v1). Duplicate method names are caught by the scan after
                    // this loop; the shared function Type is reused, as the
                    // direct-method arm below also does.
                    IdentifierNode* emb = (IdentifierNode*)m;
                    Variable* named = type_checker_lookup_variable(checker, emb->name);
                    Type* et = (named && named->type) ? named->type : NULL;
                    if (!et) {
                        type_error(checker, m->pos,
                                   "undeclared embedded interface '%s'", emb->name);
                        return NULL;
                    }
                    if (et->kind != TYPE_INTERFACE) {
                        type_error(checker, m->pos,
                                   "embedded type '%s' is not an interface", emb->name);
                        return NULL;
                    }
                    for (InterfaceMethod* sm = et->data.interface.methods; sm; sm = sm->next) {
                        InterfaceMethod* im = xcalloc(1, sizeof(InterfaceMethod));
                        if (!im) return NULL;
                        im->name = strdup(sm->name);
                        im->type = sm->type;
                        im->next = NULL;
                        if (tail) tail->next = im; else head = im;
                        tail = im;
                        method_count++;
                    }
                    continue;
                }
                if (m->type != AST_FUNC_DECL) continue;
                FuncDeclNode* fn = (FuncDeclNode*)m;

                // Comptime-value params gap-fix: an interface method's
                // per-argument comptime check is never reached at a call
                // site — type_check_call_expr's is_comptime_param lookup
                // walks a concrete callee Variable's func_decl_node, which
                // an InterfaceMethod (no Variable) never has, so a comptime
                // parameter here would silently behave as an ordinary
                // runtime int for every implementer's call. Reject it here,
                // at interface-type build time, instead.
                for (ASTNode* p = fn->params; p; p = p->next) {
                    if (p->type != AST_VAR_DECL) continue;
                    if (((VarDeclNode*)p)->is_comptime_param) {
                        type_error(checker, p->pos,
                            "comptime parameters are not supported on interface methods");
                        // Fix round 3 (minor 4): free the InterfaceMethod
                        // list partially built for EARLIER members of this
                        // interface before erroring out (names are strdup'd
                        // and list nodes calloc'd above; the method's
                        // function Type is shared/checker-owned, never freed
                        // here). Scoped to THIS branch's error path only —
                        // the sibling pre-existing error paths leak
                        // identically and are deliberately left untouched.
                        while (head) {
                            InterfaceMethod* next_im = head->next;
                            free(head->name);
                            free(head);
                            head = next_im;
                        }
                        return NULL;
                    }
                }

                size_t pcount = 0;
                for (ASTNode* p = fn->params; p; p = p->next) {
                    if (p->type == AST_VAR_DECL) {
                        pcount += ((VarDeclNode*)p)->name_count
                                      ? ((VarDeclNode*)p)->name_count : 1;
                    }
                }
                Type** ptypes = pcount ? calloc(pcount, sizeof(Type*)) : NULL;
                size_t pidx = 0;
                for (ASTNode* p = fn->params; p; p = p->next) {
                    if (p->type != AST_VAR_DECL) continue;
                    VarDeclNode* pd = (VarDeclNode*)p;
                    Type* pt = pd->type ? type_from_ast(checker, pd->type) : NULL;
                    if (!pt) { free(ptypes); return NULL; }
                    // One declared type may cover several names (`i, j int`).
                    size_t n = pd->name_count ? pd->name_count : 1;
                    for (size_t k = 0; k < n && pidx < pcount; k++) ptypes[pidx++] = pt;
                }

                Type* ret = fn->return_type
                                ? type_from_ast(checker, fn->return_type)
                                : type_checker_get_builtin(checker, TYPE_VOID);
                if (!ret) { free(ptypes); return NULL; }

                Type* method_fn = type_function(ptypes, pcount, ret);
                free(ptypes);  // type_function copies what it needs

                // Build the InterfaceMethod inline (mirrors the struct/enum
                // cases building their types via type_new rather than pulling in
                // the concept/protocol subsystem helpers).
                InterfaceMethod* im = xcalloc(1, sizeof(InterfaceMethod));
                if (!im) return NULL;
                im->name = strdup(fn->name);
                im->type = method_fn;
                im->next = NULL;
                if (tail) tail->next = im; else head = im;
                tail = im;
                method_count++;
            }

            // Reject duplicate method names, whether directly declared or
            // pulled in by embedding. Go 1.14+ permits identical duplicates
            // from separate embeds; v1 rejects any name clash — a deliberate
            // simplification (see design), loosenable later.
            for (InterfaceMethod* a = head; a; a = a->next) {
                for (InterfaceMethod* b = a->next; b; b = b->next) {
                    if (strcmp(a->name, b->name) == 0) {
                        type_error(checker, type_node->pos,
                                   "duplicate method '%s' in interface", a->name);
                        return NULL;
                    }
                }
            }

            // Empty interface (`interface {}`) is valid — every type satisfies it.
            Type* result = type_new(TYPE_INTERFACE);
            if (!result) return NULL;
            result->data.interface.methods = head;
            result->data.interface.method_count = method_count;
            result->data.interface.name = NULL;  // stamped by type_decl if named
            result->data.interface.is_synthesized = 0;
            result->data.interface.source_concept = NULL;
            return result;
        }

        case AST_MAP_TYPE: {
            MapTypeNode* map = (MapTypeNode*)type_node;
            Type* key_type = type_from_ast(checker, map->key_type);
            Type* value_type = type_from_ast(checker, map->value_type);
            if (!key_type || !value_type) return NULL;
            // Comparability gate, not a string-only gate: the runtime keys
            // on an 8-byte int64 slot (string keys use strcmp, everything
            // else uses ==; see key_kind at codegen), so any type that fits
            // that slot and has well-defined equality is admitted. This
            // mirrors codegen_map_value_is_inline's kind set (int-family /
            // bool / char / pointer), plus TYPE_STRING which the runtime
            // has always supported.
            int key_ok = key_type->kind == TYPE_STRING || type_is_integer(key_type) ||
                         key_type->kind == TYPE_BOOL || key_type->kind == TYPE_CHAR ||
                         key_type->kind == TYPE_POINTER ||
                         key_type->kind == TYPE_INTERFACE;
            // Interface-typed key (interface-map-keys Task 2): interface
            // TYPES are always statically comparable in Go — admit
            // unconditionally, unlike struct keys below (whose comparability
            // depends on their field types). The uncomparable case here is a
            // DYNAMIC one (a `[]int`/func/map value stored in an `any` used
            // as a key) and is handled by the vtable slot-0 panic-stub
            // (codegen_get_or_emit_type_eq's uncomparable arm) at runtime,
            // Go-faithfully — NOT a compile error, so TYPE_INTERFACE must
            // NOT appear in the deferred-reject list below.
            //
            // Struct-typed key (Task 2, struct map keys): admit iff every
            // field is recursively comparable — struct_is_comparable_key
            // also tells us WHY a rejected struct was rejected (a deferred
            // array field vs. a permanently non-comparable slice/map/func
            // field), so the two-reason diagnostic below stays accurate for
            // structs too instead of collapsing both into "not yet
            // supported in v1".
            const char* struct_reject_why = NULL;
            if (!key_ok && key_type->kind == TYPE_STRUCT &&
                struct_is_comparable_key(key_type, &struct_reject_why)) {
                key_ok = 1;
            }
            if (!key_ok) {
                if (key_type->kind == TYPE_STRUCT && struct_reject_why &&
                    strcmp(struct_reject_why, "noncomparable") == 0) {
                    type_error(checker, type_node->pos,
                               "invalid map key type: struct has a non-comparable field "
                               "(slice/map/func/interface fields are never comparable)");
                    return NULL;
                }
                // Two-reason diagnostic: some rejected kinds are comparable
                // in Go and just not wired into the v1 slot runtime yet
                // (deferred); others are permanently non-comparable as map
                // keys in Go itself (slice/map/func). TYPE_INTERFACE is
                // deliberately absent here — it's unconditionally admitted
                // above (key_ok), never reaches this branch.
                if (key_type->kind == TYPE_STRUCT || key_type->kind == TYPE_FLOAT32 ||
                    key_type->kind == TYPE_FLOAT64 ||
                    key_type->kind == TYPE_ARRAY) {
                    type_error(checker, type_node->pos,
                               "map key type %s is not yet supported in v1 (comparable key "
                               "types so far: string, integers, bool, rune, byte, pointers)",
                               type_to_string(key_type));
                    return NULL;
                }
                type_error(checker, type_node->pos,
                           "invalid map key type %s (not comparable)", type_to_string(key_type));
                return NULL;
            }
            // Any value type is accepted: inline scalars ride the 8-byte
            // runtime slot directly; everything else is heap-boxed by the
            // codegen slot helpers (spec 2026-07-04-func-map-values).
            return type_map(key_type, value_type);
        }
        
        case AST_CHAN_TYPE: {
            ChanTypeNode* chan = (ChanTypeNode*)type_node;
            Type* element_type = type_from_ast(checker, chan->element_type);
            if (!element_type) return NULL;
            return type_channel(element_type, chan->pattern);
        }
        
        case AST_POINTER_TYPE: {
            PointerTypeNode* ptr = (PointerTypeNode*)type_node;
            Type* pointee_type = type_from_ast(checker, ptr->element_type);
            if (!pointee_type) return NULL;
            return type_pointer(pointee_type);
        }

        case AST_FUNC_TYPE: {
            // `func(T...) R` in a type position (param, var decl, return
            // type, field, slice elem, ...). params is a ->next chain of
            // VarDeclNode — the same shape as a func_decl's parameter list,
            // already run through reinterpret_grouped_names by the parser's
            // func_signature rule — so walk it exactly like
            // declare_function_signature's signature-building loop above,
            // including the #105 variadic-parameter model (last param's T
            // becomes []T with is_variadic=1 on the resulting function Type).
            FuncTypeNode* ft = (FuncTypeNode*)type_node;
            size_t param_count = 0;
            for (ASTNode* p = ft->params; p; p = p->next) {
                if (p->type == AST_VAR_DECL) param_count++;
            }

            Type** param_types = NULL;
            int is_variadic = 0;
            if (param_count > 0) {
                param_types = calloc(param_count, sizeof(Type*));
                if (!param_types) return NULL;
                size_t idx = 0;
                for (ASTNode* p = ft->params; p; p = p->next) {
                    if (p->type != AST_VAR_DECL) continue;
                    VarDeclNode* pd = (VarDeclNode*)p;
                    Type* pt = pd->type ? type_from_ast(checker, pd->type)
                                        : type_checker_get_builtin(checker, TYPE_INT32);
                    if (!pt) { free(param_types); return NULL; }
                    if (pd->is_variadic_param) {
                        pt = type_slice(pt);
                        is_variadic = 1;
                    }
                    param_types[idx++] = pt;
                }
            }

            Type* return_type = ft->return_type
                ? type_from_ast(checker, ft->return_type)
                : type_checker_get_builtin(checker, TYPE_VOID);
            if (ft->return_type && !return_type) { free(param_types); return NULL; }

            Type* func_type = type_function(param_types, param_count, return_type);
            if (func_type) func_type->data.function.is_variadic = is_variadic;
            free(param_types);  // type_function copies what it needs
            return func_type;
        }
        
        case AST_REFERENCE_TYPE: {
            ReferenceTypeNode* ref = (ReferenceTypeNode*)type_node;
            Type* referenced_type = type_from_ast(checker, ref->element_type);
            if (!referenced_type) return NULL;
            return type_reference(referenced_type, ref->is_mutable);
        }
        
        case AST_ERROR_UNION_TYPE: {
            ErrorUnionTypeNode* error_union = (ErrorUnionTypeNode*)type_node;
            Type* value_type = type_from_ast(checker, error_union->value_type);
            if (!value_type) return NULL;

            // P0.3: reject !?T (error union whose payload is nullable) here,
            // at the single chokepoint every !T spelling resolves through
            // (return types, params, var decls, struct fields, ...) —
            // codegen has no valid lowering for the nested tagged-struct
            // shape this produces and previously SIGILLed with zero
            // diagnostics (see the P0.2 verify-gate commit for the audit
            // trail). ?!int (nullable of error union) is unaffected: it
            // resolves through AST_NULLABLE_TYPE below, wrapping a
            // TYPE_ERROR_UNION value_type, which this check never sees.
            if (value_type->kind == TYPE_NULLABLE) {
                type_error(checker, type_node->pos,
                           "error union of nullable type is not supported in v1");
                return NULL;
            }

            Type* error_type = NULL;
            if (error_union->error_type) {
                error_type = type_from_ast(checker, error_union->error_type);
                if (!error_type) return NULL;
            }
            
            return type_error_union(value_type, error_type);
        }
        
        case AST_NULLABLE_TYPE: {
            NullableTypeNode* nullable = (NullableTypeNode*)type_node;
            Type* base_type = type_from_ast(checker, nullable->base_type);
            if (!base_type) return NULL;
            return type_nullable(base_type);
        }
        
        default:
            type_error(checker, type_node->pos, "Invalid type node");
            return NULL;
    }
}