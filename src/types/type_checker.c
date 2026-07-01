#include "types.h"
#include "comptime.h"
#include "taint_analysis.h"
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
    TypeChecker* checker = malloc(sizeof(TypeChecker));
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

    // M11-types-const-integrate (part A): set up a comptime context so
    // that is_comptime const-decl RHS expressions can be routed through
    // comptime_eval_expression. The wrapper owns the type-level scaffold
    // (registered TypeFunctions, computed-type cache); the inner raw
    // context is what comptime_eval_expression actually consumes. Both
    // are torn down in type_checker_free.
    ComptimeContext* raw = comptime_context_new(NULL);
    checker->comptime_type_ctx = raw ? comptime_type_context_new(raw) : NULL;

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
        free(pkg);
        pkg = next;
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

    Package* pkg = malloc(sizeof(Package));
    if (!pkg) return NULL;

    pkg->import_path = str_dup(import_path);
    pkg->name = str_dup(name);
    pkg->exports = scope_new(NULL);
    pkg->state = 0;  // unvisited
    pkg->next = checker->packages;
    checker->packages = pkg;

    return pkg;
}

// Copy every capitalised (A-Z leading) top-level symbol of `pkg_scope` into
// `exports`. Each export is a FRESH Variable (variable_new) so that pkg_scope
// and exports never co-own a Variable node — variable_free frees only the name
// (str_dup'd copy) and the comptime value, never the shared Type*, so sharing
// the Type* pointer across the two scopes is safe.
void package_export_filter(Scope* pkg_scope, Scope* exports) {
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
    package_export_filter(checker->current_scope, pkg->exports);

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
static int declare_function_signature(TypeChecker* checker, FuncDeclNode* func) {
    size_t param_count = 0;
    for (ASTNode* p = func->params; p; p = p->next) {
        if (p->type == AST_VAR_DECL) param_count++;
    }
    Type** param_types = NULL;
    if (param_count > 0) {
        param_types = calloc(param_count, sizeof(Type*));
        size_t idx = 0;
        for (ASTNode* p = func->params; p; p = p->next) {
            if (p->type != AST_VAR_DECL) continue;
            VarDeclNode* pd = (VarDeclNode*)p;
            Type* pt = pd->type ? type_from_ast(checker, pd->type)
                                : type_checker_get_builtin(checker, TYPE_INT32);
            param_types[idx++] = pt;
        }
    }
    Type* return_type = func->return_type
        ? type_from_ast(checker, func->return_type)
        : type_checker_get_builtin(checker, TYPE_VOID);

    Type* func_type = type_function(param_types, param_count, return_type);
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
        if (!scope_add_variable(checker->current_scope, func_var)) {
            type_error(checker, func->base.pos, "Function '%s' already declared", func->name);
            variable_free(func_var);
            return 0;
        }
    }
    return 1;
}

int type_check_function_decl(TypeChecker* checker, ASTNode* decl) {
    if (!checker || !decl || decl->type != AST_FUNC_DECL) return 0;

    FuncDeclNode* func = (FuncDeclNode*)decl;

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
                
                if (param_type) {
                    for (size_t i = 0; i < param_decl->name_count; i++) {
                        Variable* param_var = variable_new(param_decl->names[i], param_type, param_decl->base.pos);
                        if (param_var) {
                            param_var->is_initialized = 1; // Parameters are always initialized
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
                scope_add_variable(checker->current_scope, rv);
            }
        }
    }

    // Type check function body
    int result = 1;
    if (func->body) {
        result = type_check_statement(checker, func->body);
    }

    checker->current_return_type = saved_return_type;
    scope_pop(checker);
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
        Variable* mv = mangled ? type_checker_lookup_variable(checker, mangled) : NULL;
        free(mangled);
        if (!mv || !mv->type || mv->type->kind != TYPE_FUNCTION) {
            *method_out = im->name; *reason_out = "missing"; return 0;
        }

        // The registered method carries the receiver as params[0]; the interface
        // method's function type has no receiver. So a match requires
        // impl.param_count == want.param_count + 1 and the tails to be equal.
        Type* impl = mv->type;
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

// P4-3: assignability into an interface-typed target. When `target` is an
// interface, accept iff `src` is that interface (or a concrete implementer);
// otherwise fall back to ordinary type_compatible. Emits the implementation
// diagnostic itself on failure (returns 0). `pos` anchors the error.
int check_interface_assign(TypeChecker* checker, Type* src, Type* target,
                           Position pos) {
    if (!target || target->kind != TYPE_INTERFACE) {
        return type_compatible(src, target);
    }
    if (src && src->kind == TYPE_INTERFACE) return 1;  // interface→interface (v1: permissive)

    const char* method = NULL;
    const char* reason = NULL;
    if (type_interface_satisfied(checker, target, src, &method, &reason)) return 1;

    const char* iname = target->data.interface.name ? target->data.interface.name
                                                    : "interface";
    const char* cname = src ? type_receiver_name(src) : NULL;
    type_error(checker, pos, "%s does not implement %s (%s method %s)",
               cname ? cname : type_to_string(src), iname,
               reason ? reason : "missing", method ? method : "?");
    return 0;
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
        inferred_type = type_check_expression(checker, var_decl->values);
        if (!inferred_type) {
            type_error(checker, var_decl->base.pos, "Invalid initializer expression");
            return 0;
        }
    }
    
    // Determine final type
    Type* final_type = declared_type;
    if (!final_type) {
        final_type = inferred_type;
    } else if (inferred_type) {
        // Check compatibility. An interface-typed target accepts any concrete
        // implementer (P4-3); check_interface_assign emits its own diagnostic.
        if (declared_type->kind == TYPE_INTERFACE) {
            if (!check_interface_assign(checker, inferred_type, declared_type,
                                        var_decl->base.pos)) {
                return 0;
            }
        } else if (!type_compatible(inferred_type, declared_type)) {
            type_error(checker, var_decl->base.pos,
                      "Cannot assign %s to %s",
                      type_to_string(inferred_type),
                      type_to_string(declared_type));
            return 0;
        }
    }
    
    if (!final_type) {
        type_error(checker, var_decl->base.pos,
                  "Variable declaration must have either type or initializer");
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
                commaok_struct->data.struct_type.fields = malloc(sizeof(StructField) * 2);
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

    // Store the type on the AST node for code generation
    var_decl->base.node_type = final_type;

    // Multi-LHS short var decl `a, b := f()` — RHS must produce a
    // TYPE_STRUCT with at least name_count fields, and each name
    // binds to the corresponding field's type. Codegen does the
    // ExtractValue destructuring; the type checker just records
    // each binding with the right per-field type.
    Type** per_name_types = NULL;
    if (var_decl->name_count > 1 && final_type && final_type->kind == TYPE_STRUCT) {
        if (final_type->data.struct_type.field_count >= var_decl->name_count) {
            per_name_types = malloc(sizeof(Type*) * var_decl->name_count);
            for (size_t i = 0; i < var_decl->name_count; i++) {
                per_name_types[i] = final_type->data.struct_type.fields[i].type;
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
    for (size_t i = 0; i < var_decl->name_count; i++) {
        Type* t = per_name_types ? per_name_types[i] : final_type;
        Variable* var = variable_new(var_decl->names[i], t, var_decl->base.pos);
        if (!var) {
            type_error(checker, var_decl->base.pos, "Memory allocation failed");
            free(per_name_types);
            return 0;
        }
        
        var->ownership = var_decl->ownership;
        // Variables with an explicit type are zero-initialized at
        // declaration (Go-style default-value semantics) — even when
        // no explicit initializer expression is supplied. Without
        // this, `var p Point` would read as uninitialized when its
        // fields are later accessed, even though the struct's bytes
        // are zeroed by the alloca.
        var->is_initialized = (var_decl->values != NULL) || (declared_type != NULL);
        
        if (!scope_add_variable(checker->current_scope, var)) {
            type_error(checker, var_decl->base.pos, 
                      "Variable '%s' already declared in this scope", var_decl->names[i]);
            variable_free(var);
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
            value_type = (folded <= 9223372036854775807ULL)
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

    // Add constants to scope (treated as immutable variables)
    for (size_t i = 0; i < const_decl->name_count; i++) {
        Variable* var = variable_new(const_decl->names[i], value_type, const_decl->base.pos);
        if (!var) {
            if (comptime_val) comptime_value_free(comptime_val);
            return 0;
        }

        var->mutability = MUTABILITY_IMMUTABLE;
        var->is_initialized = 1;
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
        shell = type_new(body_kind == AST_ENUM_TYPE ? TYPE_ENUM : TYPE_STRUCT);
        if (!shell) return 0;
        // Do NOT set shell->...name here; the existing tail stamping (below)
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
            return 1;  // Always valid
        case AST_GO_STMT:
            return type_check_go_stmt(checker, stmt);
        case AST_DEFER_STMT:
            return type_check_defer_stmt(checker, stmt);
        case AST_SELECT_STMT:
            return type_check_select_stmt(checker, stmt);
        case AST_SWITCH_STMT: {
            // Expression switch: type-check the tag, then every case
            // expression and clause body. Case bodies are raw statement
            // lists (linked via next), so they are walked here rather than
            // dispatched as blocks. Each clause gets its own scope, matching
            // Go's per-clause scoping.
            SwitchStmtNode* sw = (SwitchStmtNode*)stmt;
            int ok = 1;
            if (sw->tag && !type_check_expression(checker, sw->tag)) ok = 0;
            for (ASTNode* c = sw->cases; c; c = c->next) {
                CaseClauseNode* clause = (CaseClauseNode*)c;
                for (ASTNode* e = clause->exprs; e; e = e->next) {
                    if (!type_check_expression(checker, e)) ok = 0;
                }
                scope_push(checker);
                for (ASTNode* s = clause->body; s; s = s->next) {
                    if (!type_check_statement(checker, s)) ok = 0;
                }
                scope_pop(checker);
            }
            return ok;
        }
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
    
    // Condition must be boolean
    if (cond_type->kind != TYPE_BOOL) {
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
    // the body. Slice range is the supported case for M8; map/string
    // range deferred.
    if (for_stmt->range_expr) {
        Type* range_type = type_check_expression(checker, for_stmt->range_expr);
        if (!range_type) {
            scope_pop(checker);
            return 0;
        }
        Type* elem_type = NULL;
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
        } else {
            type_error(checker, for_stmt->range_expr->pos,
                      "for-range supported only on slice/array/string types");
            scope_pop(checker);
            return 0;
        }
        if (for_stmt->key_name) {
            Variable* kv = variable_new(for_stmt->key_name,
                                       type_checker_get_builtin(checker, TYPE_INT32),
                                       stmt->pos);
            if (kv) {
                kv->is_initialized = 1;
                scope_add_variable(checker->current_scope, kv);
            }
        }
        if (for_stmt->value_name && elem_type) {
            Variable* vv = variable_new(for_stmt->value_name, elem_type, stmt->pos);
            if (vv) {
                vv->is_initialized = 1;
                scope_add_variable(checker->current_scope, vv);
            }
        }
    }

    // Check initialization
    if (for_stmt->init) {
        if (!type_check_statement(checker, for_stmt->init)) {
            result = 0;
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
// expressions (`1 + 1`, `1 << 3`). Codegen materializes all of these as an i32
// constant and then SExt-WIDENS it to the declared integer return type (see the
// return-widening path in statement_codegen.c). Because codegen only widens —
// it never truncates — the caller must additionally gate on the declared return
// type being no narrower than the operand (see `int_const_widen`); a narrowing
// target (e.g. `return 65` into a byte) would otherwise reach the verifier as
// invalid IR.
static int is_untyped_int_const_expr(ASTNode* node) {
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
            if (ret_stmt->values->next) {
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

            // type_compatible() permits ANY numeric->numeric pair (it allows
            // implicit conversions), but a return value reaches codegen with NO
            // trunc/ext/fptosi inserted. So a numeric return whose machine
            // representation differs from the declared return type — a wider/
            // narrower integer (e.g. an int64 call result from an int function)
            // or a float into an integer (e.g. `return 3.9` from int) — would
            // slip past type_compatible and crash the LLVM verifier with a
            // return-operand mismatch. Reject those here. The sole exception
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
                int int_const_widen =
                    type_is_integer(expected) &&
                    type_size(expected) >= type_size(return_type) &&
                    is_untyped_int_const_expr(ret_stmt->values);
                if ((!same_kind || !same_width) && !int_const_widen) {
                    type_error(checker, stmt->pos,
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
    
    // Type check each case
    ASTNode* case_node = select_stmt->cases;
    while (case_node) {
        // TODO: Implement proper select case type checking
        // For now, just accept it as valid
        case_node = case_node->next;
    }
    
    return 1;
}

// Forward declarations for helper functions
Type* type_from_ast(TypeChecker* checker, ASTNode* type_node);
Type* type_check_expression(TypeChecker* checker, ASTNode* expr);

// Helper function to convert AST type nodes to Type structures
Type* type_from_ast(TypeChecker* checker, ASTNode* type_node) {
    if (!checker || !type_node) return NULL;
    
    switch (type_node->type) {
        case AST_IDENTIFIER: {
            // Handle type identifiers (for make_chan, etc.)
            IdentifierNode* ident = (IdentifierNode*)type_node;
            
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
            
            // TODO: Evaluate length expression
            size_t length = 10;  // Placeholder
            return type_array(element_type, length);
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
                if (f->type == AST_VAR_DECL) count++;
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
                if (fd->name_count == 0) continue;
                Type* ft = fd->type ? type_from_ast(checker, fd->type) : NULL;
                if (!ft) {
                    free(result->data.struct_type.fields);
                    free(result);
                    return NULL;
                }
                result->data.struct_type.fields[idx].name = strdup(fd->names[0]);
                result->data.struct_type.fields[idx].type = ft;
                result->data.struct_type.fields[idx].offset = total_size;
                total_size += ft->size ? ft->size : 8;
                if (ft->align > max_align) max_align = ft->align;
                idx++;
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
                if (m->type != AST_FUNC_DECL) continue;
                FuncDeclNode* fn = (FuncDeclNode*)m;

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
                InterfaceMethod* im = calloc(1, sizeof(InterfaceMethod));
                if (!im) return NULL;
                im->name = strdup(fn->name);
                im->type = method_fn;
                im->next = NULL;
                if (tail) tail->next = im; else head = im;
                tail = im;
                method_count++;
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
            // The runtime keys on strings only.
            if (key_type->kind != TYPE_STRING) {
                type_error(checker, type_node->pos,
                           "map key type must be string, got %s", type_to_string(key_type));
                return NULL;
            }
            // The value rides an 8-byte runtime slot, so V must be a scalar
            // that fits: an integer, bool, char, or pointer. Aggregates
            // (string, slice, struct, another map) don't fit and are rejected.
            if (!(type_is_integer(value_type) || value_type->kind == TYPE_POINTER
                  || value_type->kind == TYPE_BOOL || value_type->kind == TYPE_CHAR)) {
                type_error(checker, type_node->pos,
                           "map value type %s is not supported yet "
                           "(must be an integer, bool, char, or pointer)",
                           type_to_string(value_type));
                return NULL;
            }
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