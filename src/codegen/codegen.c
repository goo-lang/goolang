#include "codegen.h"
#include "block_escape.h"
#include "param_escape.h"
#include "value_scope.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

// P4.3 (packages-B): the single source of truth for the `goo_pkg__<pkg>__
// <base>` mangling scheme, taking the package NAME directly rather than
// reading it off checker->current_package. codegen_package_symbol_name
// (below) covers the DEFINITION side, where the current package being
// codegen'd IS the owner. Cross-package method CALL/VALUE sites need the
// opposite direction: codegen'ing main (current_package == NULL) but
// calling a method whose RECEIVER type is owned by some OTHER, already-
// compiled package (type_receiver_owner_package, types.h) — this variant
// lets those call sites build the exact same symbol from that package's own
// name. Returns NULL if either argument is NULL/empty; malloc'd, caller frees.
char* codegen_pkg_mangled_symbol(const char* pkg_name, const char* base) {
    if (!pkg_name || !pkg_name[0] || !base) return NULL;
    size_t need = strlen("goo_pkg__") + strlen(pkg_name) + strlen("__")
                + strlen(base) + 1;
    char* out = malloc(need);
    if (!out) return NULL;
    snprintf(out, need, "goo_pkg__%s__%s", pkg_name, base);
    return out;
}

// stdlib Phase 0 (Task 4): compute the LLVM symbol name for a top-level symbol
// emitted while codegenning a non-main package. For the main package
// (checker->current_package == NULL) this returns NULL and callers keep the
// BARE name, so the no-import path is byte-identical. Otherwise it returns a
// malloc'd `goo_pkg__<pkg>__<base>` (caller frees). `base` is the type-checker
// lookup key — the bare function name for a plain/top-level function, or the
// method-mangled `T__m` for a method — so BOTH plain functions and methods (and
// any future top-level symbol) get the package prefix and never collide with
// main's bare symbols. This is the single source of truth for cross-package
// mangling, shared by function_codegen.c and error_union_codegen.c.
char* codegen_package_symbol_name(TypeChecker* checker, const char* base) {
    if (!checker || !checker->current_package
                 || !checker->current_package->name || !base) {
        return NULL;
    }
    return codegen_pkg_mangled_symbol(checker->current_package->name, base);
}

// Code generator initialization and cleanup

// WebAssembly target configuration
#if LLVM_AVAILABLE
static int codegen_configure_wasm_target(CodeGenerator* codegen) {
    if (!codegen) return 0;
    
    // Configure WebAssembly-specific settings
    // For example, set the correct calling convention, enable SIMD, etc.
    // This is a placeholder for actual WebAssembly target configuration
    Position pos = {0, 0, 0, "codegen"};
    codegen_warning(codegen, pos, "Configuring WebAssembly target - this is a placeholder");
    return 1;
}
#endif

CodeGenerator* codegen_new(const char* module_name __attribute__((unused))) {
#if LLVM_AVAILABLE
    CodeGenerator* codegen = xmalloc(sizeof(CodeGenerator));
    if (!codegen) return NULL;
    
    // Initialize LLVM core
    codegen->context = LLVMContextCreate();
    codegen->module = LLVMModuleCreateWithNameInContext(module_name, codegen->context);
    codegen->builder = LLVMCreateBuilderInContext(codegen->context);
    
    codegen->target_machine = NULL;
    codegen->current_function = NULL;
    codegen->current_function_info = NULL;
    
    // Initialize symbol tables
    codegen->value_table = NULL;
    codegen->value_table_size = 0;
    codegen->value_table_capacity = 0;
    
    codegen->type_cache = NULL;
    codegen->type_cache_size = 0;
    codegen->type_cache_capacity = 0;

    codegen->struct_cache_keys = NULL;
    codegen->struct_cache_vals = NULL;
    codegen->struct_cache_size = 0;
    codegen->struct_cache_cap = 0;

    codegen->structeq_cache_keys = NULL;
    codegen->structeq_cache_vals = NULL;
    codegen->structeq_cache_size = 0;
    codegen->structeq_cache_cap = 0;
    codegen->structeq_counter = 0;

    codegen->typeeq_cache_keys = NULL;
    codegen->typeeq_cache_vals = NULL;
    codegen->typeeq_cache_size = 0;
    codegen->typeeq_cache_cap = 0;
    codegen->typeeq_counter = 0;
    codegen->uncmpeq_fn = NULL;

    // Loop-context stack (break/continue targets) starts empty.
    codegen->cfctx.loop_depth = 0;

    // gofmt-syntax-b Task 1: no label pending a push yet. loop_label/
    // loop_is_loop need no init — every slot < loop_depth is written before
    // it is ever read (set at push time, same convention as
    // loop_break_bb/loop_continue_bb above, which also start uninitialized).
    codegen->cfctx.pending_label = NULL;

    // gofmt-syntax-b Task 2: no goto labels registered yet (per-function
    // reset happens in codegen_enter_function, since — unlike loop_depth —
    // this table isn't push/pop self-balancing within a function).
    codegen->cfctx.goto_label_count = 0;

    // gofmt-syntax-b Task 3: fallthrough-target stack starts empty — push/
    // pop self-balances within codegen_generate_switch_stmt, same
    // convention as loop_depth above.
    codegen->cfctx.fallthrough_depth = 0;

    // Arena-regions Task 3: arena stack starts empty — codegen_emit_alloc
    // stays on the goo_alloc path until Task 6's `arena{}` lowering pushes.
    codegen->arena_depth = 0;
    // Arena-regions Task 7c: no analysis result until codegen_generate_
    // program runs param_escape_analyze/block_escape_analyze over the
    // program it's about to emit.
    codegen->block_escape = NULL;

    // Error reporting
    codegen->current_file = NULL;
    codegen->error_count = 0;
    codegen->warning_count = 0;
    
    // Target information
    codegen->target_triple = NULL;
    codegen->target_cpu = NULL;
    codegen->target_features = NULL;

    // P3.10: driver overwrites this from -O right after codegen_new.
    codegen->opt_level = 0;

    // P3.11: driver overwrites these from -l/--link right after codegen_new.
    codegen->link_libs = NULL;
    codegen->link_lib_count = 0;

    // WebAssembly configuration
    codegen->wasm_configured = 0;
    codegen->is_wasm_target = 0;

    // Deferred global initializers (Task 2 / var-init cluster) — empty
    // until codegen_generate_var_decl's module-scope path defers one.
    codegen->deferred_global_inits = NULL;
    codegen->deferred_global_init_count = 0;
    codegen->deferred_global_init_capacity = 0;

    // Function-generics Task 8: substitution environment — unset (NULL/0)
    // until Task 9/10 populate it around a monomorphized instantiation's
    // codegen. NULL here makes codegen_resolve_type identity and TYPE_PARAM
    // unreachable in codegen_type_to_llvm on the non-generic path.
    codegen->active_subst = NULL;
    codegen->active_subst_n = 0;

    // Function-generics Task 9: no override until codegen_generate_function_
    // instance installs one around a single instantiation's stamping.
    codegen->symbol_override = NULL;

    // Comptime value params Task 3: no active comptime instance until
    // codegen_generate_comptime_function_instance installs one around a
    // single instantiation's stamping (monomorphize.c) — mirrors
    // active_subst/active_subst_n just above, one axis over.
    codegen->active_comptime_values = NULL;
    codegen->active_comptime_value_n = 0;

    // Declare runtime functions
    codegen_declare_runtime_functions(codegen);
    
    // Configure WebAssembly-specific features if targeting WASM
    if (codegen->target_triple && strstr(codegen->target_triple, "wasm32")) {
        codegen->is_wasm_target = 1;
        codegen_declare_wasm_runtime_functions(codegen);
        codegen_configure_wasm_concurrency(codegen);
    }
    
    return codegen;
#else
    CodeGenerator* codegen = xmalloc(sizeof(CodeGenerator));
    if (!codegen) return NULL;
    
    codegen->error_message = strdup("LLVM support not available in this build");
    codegen->llvm_unavailable = 1;
    codegen->current_file = NULL;
    codegen->error_count = 0;
    codegen->warning_count = 0;
    return codegen;
#endif
}

void codegen_free(CodeGenerator* codegen) {
    if (!codegen) return;
    
#if LLVM_AVAILABLE
    if (codegen->builder) {
        LLVMDisposeBuilder(codegen->builder);
    }
    if (codegen->module) {
        LLVMDisposeModule(codegen->module);
    }
    if (codegen->context) {
        LLVMContextDispose(codegen->context);
    }
    if (codegen->target_machine) {
        LLVMDisposeTargetMachine(codegen->target_machine);
    }
    
    // Free symbol tables
    if (codegen->value_table) {
        for (size_t i = 0; i < codegen->value_table_size; i++) {
            if (codegen->value_table[i]) {
                value_info_free(codegen->value_table[i]);
            }
        }
        free(codegen->value_table);
    }
    
    if (codegen->type_cache) {
        free(codegen->type_cache);
    }

    free(codegen->struct_cache_keys);
    free(codegen->struct_cache_vals);

    free(codegen->structeq_cache_keys);
    free(codegen->structeq_cache_vals);

    free(codegen->typeeq_cache_keys);
    free(codegen->typeeq_cache_vals);

    // The deferred-init array holds borrowed pointers (globals owned by the
    // module, expressions by the AST, types by the type system) — free only
    // the array itself.
    free(codegen->deferred_global_inits);

    free(codegen->current_file);
    free(codegen->target_triple);
    free(codegen->target_cpu);
    free(codegen->target_features);

    // Arena-regions Task 7c: free the block-escape analysis result computed
    // at codegen_generate_program entry (borrowed AST-node pointers inside
    // it are not owned, so this frees only the result's own storage).
    if (codegen->block_escape) {
        block_escape_result_free(codegen->block_escape);
    }
#else
    free(codegen->error_message);
    free(codegen->current_file);
#endif
    
    free(codegen);
}

int codegen_set_target(CodeGenerator* codegen __attribute__((unused)), const char* triple __attribute__((unused)), const char* cpu __attribute__((unused)), const char* features __attribute__((unused))) {
    if (!codegen) return 0;
    
#if LLVM_AVAILABLE
    if (triple) {
        free(codegen->target_triple);
        codegen->target_triple = strdup(triple);
    }
    if (cpu) {
        free(codegen->target_cpu);
        codegen->target_cpu = strdup(cpu);
    }
    if (features) {
        free(codegen->target_features);
        codegen->target_features = strdup(features);
    }
    return 1;
#else
    return 0;
#endif
}

int codegen_initialize_target(CodeGenerator* codegen) {
    if (!codegen) return 0;
    
#if LLVM_AVAILABLE
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    
    // Initialize WebAssembly target
    LLVMInitializeWebAssemblyTarget();
    LLVMInitializeWebAssemblyTargetInfo();
    LLVMInitializeWebAssemblyTargetMC();
    LLVMInitializeWebAssemblyAsmPrinter();
    
    // Set target triple for module
    if (codegen->target_triple) {
        LLVMSetTarget(codegen->module, codegen->target_triple);
        
        // Configure WebAssembly-specific features
        if (strstr(codegen->target_triple, "wasm32")) {
            codegen_configure_wasm_target(codegen);
        }
    } else {
        char* default_triple = LLVMGetDefaultTargetTriple();
        LLVMSetTarget(codegen->module, default_triple);
        LLVMDisposeMessage(default_triple);
    }
    
    return 1;
#else
    return 0;
#endif
}

// Main code generation entry point
int codegen_generate_program(CodeGenerator* codegen, TypeChecker* checker, ASTNode* program) {
    if (!codegen || !checker || !program) return 0;
    
#if !LLVM_AVAILABLE
    codegen_error(codegen, program->pos, "LLVM support not available");
    return 0;
#endif
    
    if (program->type != AST_PROGRAM) {
        codegen_error(codegen, program->pos, "Expected program node");
        return 0;
    }

    // Arena-regions Task 7c: run the param-escape (7a) and block-escape (7b)
    // analyses over the SAME `program` AST codegen is about to emit from, so
    // every alloc-site ASTNode* pointer codegen_emit_alloc later passes to
    // codegen_arena_eligible matches a decision recorded here by identity.
    // codegen_generate_program may run once per package (main pass plus one
    // pass per imported package into this same module) — the re-entry guard
    // frees any prior result before overwriting so repeat calls don't leak.
    // Analysis is an optimization, not a correctness precondition: if either
    // step returns NULL (allocation failure), leave block_escape NULL and
    // CONTINUE — codegen_arena_eligible then treats every site as escaping
    // (heap), which is the same fail-safe default this gate already has for
    // every unclassified site. Do NOT abort codegen on analysis failure.
    if (codegen->block_escape) {
        block_escape_result_free(codegen->block_escape);
        codegen->block_escape = NULL;
    }
    ParamEscapeResult* pe = param_escape_analyze(program);
    codegen->block_escape = block_escape_analyze(program, pe); // does NOT retain pe
    param_escape_result_free(pe);

    // Initialize target
    if (!codegen_initialize_target(codegen)) {
        codegen_error(codegen, program->pos, "Failed to initialize target");
        return 0;
    }
    
    ProgramNode* prog = (ProgramNode*)program;
    
    // Generate imports (TODO)
    if (prog->imports) {
        ASTNode* import = prog->imports;
        while (import) {
            // TODO: Handle imports
            import = import->next;
        }
    }
    
    // Forward-reference pre-pass: declare all plain-function prototypes before
    // emitting any body, so a call to a function defined later in the file
    // resolves (call sites use LLVMGetNamedFunction). Mirrors the type checker's
    // hoist_function_signatures.
    if (!codegen_predeclare_functions(codegen, checker, prog->decls)) {
        return 0;
    }

    // Task 2 / var-init cluster: pre-create goo.global_init's prototype
    // (no body yet) BEFORE any function body is generated. Without this, a
    // package-level `var` that ends up deferred but appears textually AFTER
    // `func main` would mean the call inserted into main's prologue (see
    // codegen_generate_function_decl's is_entry_main branch) can't find the
    // symbol yet — LLVMGetNamedFunction would return NULL at that point,
    // since the actual body-filling pass (codegen_generate_global_init_
    // function below) only runs after every declaration, including main
    // itself, has been generated. The prototype-now/body-later split lets
    // main call it regardless of declaration order.
    //
    // MAIN PACKAGE ONLY (Task 2b): codegen_generate_program also runs once
    // per imported package into this same module. Packages can never defer
    // (codegen_generate_var_decl rejects package-scope deferral cleanly),
    // but the pre-pass OVER-approximates — it runs before any declaration
    // is generated, so it cannot resolve const identifiers, and a goostd
    // lookup table like utf8's `first` (elements are const identifiers)
    // would be misread as "needs init". A package pass would then create —
    // and its fill call below would body-fill — a goo.global_init that
    // collides with main's own. Gate both the prototype and the fill on
    // the main pass.
    int is_main_pass = (checker->current_package == NULL);
    if (is_main_pass && codegen_program_needs_global_init(prog->decls)) {
        LLVMTypeRef void_ty = LLVMVoidTypeInContext(codegen->context);
        LLVMTypeRef fn_ty = LLVMFunctionType(void_ty, NULL, 0, 0);
        LLVMAddFunction(codegen->module, "goo.global_init", fn_ty);
    }

    // Function-generics Task 9: stamp every recorded generic instantiation's
    // specialized function BEFORE the body-emitting declaration loop below.
    // Ordering is load-bearing, not cosmetic: main's body is emitted INSIDE
    // that loop, and (once Task 10 rewires call sites) will call a mangled
    // instance symbol like `Id__int64` — that symbol must already exist in
    // the module by the time main's body is generated, exactly like the
    // plain-function predeclare pass above exists so a call to a
    // later-defined function resolves. Running the pass here, right after
    // that predeclare pass, also means an instance body that itself calls an
    // ordinary concrete function finds that callee's prototype already in
    // place.
    //
    // MAIN PACKAGE ONLY: checker->instantiations accumulates across every
    // package + main pass sharing this one TypeChecker (compile_resolved_
    // packages, goo.c) — by the time main's codegen_generate_program call
    // reaches here, every package has already been type-checked (and
    // codegen'd), so the full instantiation list is already present. Gating
    // on is_main_pass, mirroring the goo.global_init prototype gate just
    // above, runs the worklist exactly once instead of once per package (the
    // LLVMGetNamedFunction dedup inside codegen_monomorphize would make a
    // repeat harmless, but there is no reason to redo the work).
    if (is_main_pass && !codegen_monomorphize(codegen, checker)) {
        return 0;
    }

    // Generate declarations
    if (prog->decls) {
        ASTNode* decl = prog->decls;
        while (decl) {
            if (!codegen_generate_declaration(codegen, checker, decl)) {
                return 0;
            }
            decl = decl->next;
        }
    }

    // Fill in goo.global_init's body from whatever was deferred while
    // generating the declarations above. Must run after the loop: every
    // global and every function this initializer might call (e.g. `var w =
    // double(x)`) needs to already exist in the module. Main pass only —
    // see the prototype gate above.
    if (is_main_pass && !codegen_generate_global_init_function(codegen, checker)) {
        return 0;
    }

    return codegen->error_count == 0;
}

// Comptime value params Task 3: does this (non-method) function declaration
// carry any `comptime` parameter? Scoped to plain functions only — a
// comptime-param METHOD is intentionally left on the ordinary single-
// emission codegen path for now (its `n` stays an actual runtime parameter,
// same as before this task): Part B's call-rewiring (call_codegen.c) only
// rewrites a bare identifier call site today, so skipping a method's bare
// emission here without a matching call-site rewrite would leave any
// `obj.M(4)` call resolving to nothing. Not a regression — comptime-param
// methods already worked exactly this way (single emission, runtime `n`)
// before this task; this only changes PLAIN functions. See
// monomorphize.c's comptime worklist doc comment for the same scoping.
static int func_decl_has_comptime_param(FuncDeclNode* fd) {
    for (ASTNode* p = fd->params; p; p = p->next) {
        if (p->type != AST_VAR_DECL) continue;
        if (((VarDeclNode*)p)->is_comptime_param) return 1;
    }
    return 0;
}

int codegen_generate_declaration(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl) {
    if (!codegen || !checker || !decl) return 0;

    switch (decl->type) {
        case AST_FUNC_DECL:
            // Function generics Task 4: a generic function template
            // (type_params != NULL) is never emitted directly — it is
            // monomorphized per concrete instantiation (M3). Skip here.
            if (((FuncDeclNode*)decl)->type_params) return 1;
            // Comptime value params Task 3: likewise, a plain (non-method)
            // function with a `comptime` parameter is monomorphized per
            // distinct value tuple (codegen_monomorphize's comptime
            // worklist, monomorphize.c) rather than emitted once under its
            // bare name. See func_decl_has_comptime_param's doc comment for
            // why methods are excluded from this skip.
            if (!((FuncDeclNode*)decl)->receiver &&
                func_decl_has_comptime_param((FuncDeclNode*)decl)) return 1;
            return codegen_generate_function_decl(codegen, checker, decl);
        case AST_VAR_DECL:
            return codegen_generate_var_decl(codegen, checker, decl);
        case AST_CONST_DECL:
            return codegen_generate_const_decl(codegen, checker, decl);
        case AST_CONCEPT_DECL:
            // Concepts are compile-time only and don't generate runtime code
            return 1;
        case AST_TYPE_DECL:
            // Type declarations (e.g. `type Point struct { x int }`)
            // are compile-time only — the named type is registered with
            // the type checker so subsequent references resolve. No
            // runtime artifact is emitted.
            return 1;
        default:
            codegen_error(codegen, decl->pos, "Unknown declaration type for code generation");
            return 0;
    }
}

// Error reporting
void codegen_error(CodeGenerator* codegen, Position pos, const char* format, ...) {
    if (!codegen) return;
    
    fprintf(stderr, "Error at %s:%d:%d: ", pos.filename ? pos.filename : "<unknown>", pos.line, pos.column);
    
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    
    fprintf(stderr, "\n");
    codegen->error_count++;
}

void codegen_warning(CodeGenerator* codegen, Position pos, const char* format, ...) {
    if (!codegen) return;
    
    fprintf(stderr, "Warning at %s:%d:%d: ", pos.filename ? pos.filename : "<unknown>", pos.line, pos.column);
    
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    
    fprintf(stderr, "\n");
    codegen->warning_count++;
}

#if LLVM_AVAILABLE

// Value management
ValueInfo* value_info_new(const char* name, LLVMValueRef llvm_value, Type* goo_type) {
    ValueInfo* info = xmalloc(sizeof(ValueInfo));
    if (!info) return NULL;
    
    info->name = name ? strdup(name) : NULL;
    info->llvm_value = llvm_value;
    info->goo_type = goo_type;
    info->is_lvalue = 0;
    info->is_moved = 0;
    info->is_initialized = 1;
    
    return info;
}

void value_info_free(ValueInfo* info) {
    if (!info) return;
    free(info->name);
    free(info);
}

ValueInfo* codegen_lookup_value(CodeGenerator* codegen, const char* name) {
    if (!codegen || !name) return NULL;

    // Search from the end so the most-recently added binding shadows earlier
    // ones.  This gives correct LIFO (innermost-scope-wins) semantics without
    // a full scope stack.  It also fixes the multiple-catch-block bug where
    // each `catch e {}` adds a new entry for `e`; FIFO would always return the
    // first entry (wrong alloca), whereas LIFO correctly returns the latest.
    for (size_t i = codegen->value_table_size; i > 0; i--) {
        ValueInfo* info = codegen->value_table[i - 1];
        if (info && info->name && strcmp(info->name, name) == 0) {
            return info;
        }
    }

    return NULL;
}

int codegen_add_value(CodeGenerator* codegen, ValueInfo* info) {
    if (!codegen || !info) return 0;
    
    // Resize table if needed
    if (codegen->value_table_size >= codegen->value_table_capacity) {
        size_t new_capacity = codegen->value_table_capacity == 0 ? 16 : codegen->value_table_capacity * 2;
        ValueInfo** new_table = realloc(codegen->value_table, sizeof(ValueInfo*) * new_capacity);
        if (!new_table) return 0;
        
        codegen->value_table = new_table;
        codegen->value_table_capacity = new_capacity;
    }
    
    codegen->value_table[codegen->value_table_size++] = info;
    return 1;
}

// Function management  
FunctionInfo* function_info_new(const char* name, LLVMValueRef function, Type* goo_type) {
    FunctionInfo* info = xmalloc(sizeof(FunctionInfo));
    if (!info) return NULL;
    
    info->name = name ? strdup(name) : NULL;
    info->function = function;
    info->function_type = LLVMGetElementType(LLVMTypeOf(function));
    info->goo_type = goo_type;
    
    info->entry_block = NULL;
    info->exit_block = NULL;
    info->return_value = NULL;
    
    info->locals = NULL;
    info->local_count = 0;
    info->local_capacity = 0;

    info->named_result_names = NULL;
    info->named_result_count = 0;

    info->deferred_calls = NULL;
    info->deferred_count = 0;
    info->deferred_capacity = 0;

    info->defer_stack_mode = 0;
    info->defer_frame = NULL;

    return info;
}

void function_info_free(FunctionInfo* info) {
    if (!info) return;
    
    free(info->name);

    if (info->named_result_names) {
        for (size_t i = 0; i < info->named_result_count; i++)
            free(info->named_result_names[i]);
        free(info->named_result_names);
    }

    // The deferred-call array holds borrowed AST node pointers (owned by the
    // parse tree), so free only the array, not the nodes.
    free(info->deferred_calls);

    if (info->locals) {
        for (size_t i = 0; i < info->local_count; i++) {
            if (info->locals[i]) {
                value_info_free(info->locals[i]);
            }
        }
        free(info->locals);
    }
    
    free(info);
}

int codegen_enter_function(CodeGenerator* codegen, FunctionInfo* func_info) {
    if (!codegen || !func_info) return 0;

    codegen->current_function = func_info->function;
    codegen->current_function_info = func_info;
    // Capture the current value table position — anything added past
    // this point belongs to this function and gets cleared on exit.
    // Codegen hardening R2a: the mark is read via vscope_enter, but kept in
    // this field (not a local) because codegen_generate_func_lit's nested-
    // emission save/restore needs to snapshot/restore it across a nested
    // codegen_enter_function/codegen_exit_function pair — see
    // include/value_scope.h.
    codegen->value_table_function_start = vscope_enter(codegen);

    // gofmt-syntax-b Task 2: this function's goto-label table starts empty
    // — the previous function's labels/blocks must not leak in (blocks are
    // created once and never popped, unlike the loop stack, so this reset
    // has to be explicit; see ControlFlowContext's doc comment,
    // codegen_cfctx.h). Codegen hardening R1: cfctx_reset is exactly
    // `cfctx->goto_label_count = 0` — same effect as before, named so a
    // future per-function-reset addition to ControlFlowContext has one
    // call site to extend instead of a field write to remember to add here.
    cfctx_reset(&codegen->cfctx);

    return 1;
}

void codegen_exit_function(CodeGenerator* codegen) {
    if (!codegen) return;

    // Truncate the value table back to its pre-function size so this
    // function's locals don't leak into the next function's lookups.
    // Per-info free isn't done here because value_info_free's call
    // pattern in this codebase is inconsistent — the entries stay
    // logically dead and will be overwritten by future adds.
    vscope_exit(codegen, codegen->value_table_function_start);

    codegen->current_function = NULL;
    codegen->current_function_info = NULL;
}

// Helper functions
LLVMValueRef codegen_create_alloca(CodeGenerator* codegen, LLVMTypeRef type, const char* name) {
    if (!codegen || !type) return NULL;

    return LLVMBuildAlloca(codegen->builder, type, name);
}

// Map slot classification (spec 2026-07-04-func-map-values): inline types
// ride the i64 slot as a cast; everything else (funcvals, strings, floats,
// structs, slices, interfaces, ...) is heap-boxed and the slot holds the
// box pointer. TYPE_MAP/TYPE_CHAN are opaque runtime pointers — inline.
// Non-static: the write/literal sites (expression_codegen.c) and the
// lvalue guard (Task 4) consult it too; declared in codegen.h.
int codegen_map_value_is_inline(Type* v) {
    if (!v) return 0;
    return type_is_integer(v) || v->kind == TYPE_BOOL || v->kind == TYPE_CHAR ||
           v->kind == TYPE_POINTER || v->kind == TYPE_MAP || v->kind == TYPE_CHANNEL;
}

LLVMValueRef codegen_map_value_to_slot(CodeGenerator* codegen, LLVMValueRef value, Type* value_type) {
    if (!codegen || !value || !value_type) return NULL;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(codegen->context);
    if (value_type->kind == TYPE_POINTER || value_type->kind == TYPE_MAP ||
        value_type->kind == TYPE_CHANNEL) {
        return LLVMBuildPtrToInt(codegen->builder, value, i64, "map_slot");
    }
    if (!codegen_map_value_is_inline(value_type)) {
        // Boxed value: goo_alloc(sizeof V), store, slot = box pointer.
        // ABI size via LLVMSizeOf — padding-correct (chan-padded lesson).
        // Boxes leak on overwrite/delete by decision (no GC yet; same as
        // closure envs and interface boxes).
        LLVMTypeRef vt = codegen_type_to_llvm(codegen, value_type);
        if (!vt) return NULL;
        LLVMValueRef size = LLVMSizeOf(vt);
        LLVMValueRef box = codegen_emit_alloc(codegen, size, ALLOC_KIND_DEFAULT, NULL);
        LLVMBuildStore(codegen->builder, value, box);
        return LLVMBuildPtrToInt(codegen->builder, box, i64, "map_slot");
    }
    // Sign-extend a SIGNED integer when widening into the slot, so a negative
    // value narrower than the slot keeps its sign (e.g. the i32 literal -1
    // into a map[string]int64 must read back as -1, not 4294967295). Integer
    // literals are always emitted i32, so this widening is real. Unsigned,
    // bool, and char zero-extend. The read truncates back to V's width.
    LLVMBool is_signed = (value_type->kind >= TYPE_INT8 && value_type->kind <= TYPE_INT64);
    return LLVMBuildIntCast2(codegen->builder, value, i64, is_signed, "map_slot");
}

LLVMValueRef codegen_map_slot_to_value(CodeGenerator* codegen, LLVMValueRef slot, Type* value_type) {
    if (!codegen || !slot || !value_type) return NULL;
    LLVMTypeRef vt = codegen_type_to_llvm(codegen, value_type);
    if (!vt) return NULL;
    if (value_type->kind == TYPE_POINTER || value_type->kind == TYPE_MAP ||
        value_type->kind == TYPE_CHANNEL) {
        return LLVMBuildIntToPtr(codegen->builder, slot, vt, "map_val");
    }
    if (!codegen_map_value_is_inline(value_type)) {
        // Boxed value with the ZERO GUARD: goo_map_get_sv returns 0 on a
        // missing key, and loading through slot 0 would be a null deref.
        // slot == 0  →  V's zero value (nil funcval / "" string / zero struct);
        // otherwise  →  load the value out of the box (copy semantics).
        LLVMTypeRef i64 = LLVMInt64TypeInContext(codegen->context);
        LLVMValueRef is_miss = LLVMBuildICmp(codegen->builder, LLVMIntEQ, slot,
                                             LLVMConstInt(i64, 0, 0), "map_miss");
        LLVMBasicBlockRef cur = LLVMGetInsertBlock(codegen->builder);
        LLVMValueRef fn = LLVMGetBasicBlockParent(cur);
        LLVMBasicBlockRef load_bb =
            LLVMAppendBasicBlockInContext(codegen->context, fn, "map_unbox");
        LLVMBasicBlockRef done_bb =
            LLVMAppendBasicBlockInContext(codegen->context, fn, "map_unbox_done");
        LLVMBuildCondBr(codegen->builder, is_miss, done_bb, load_bb);
        LLVMPositionBuilderAtEnd(codegen->builder, load_bb);
        LLVMValueRef box = LLVMBuildIntToPtr(
            codegen->builder, slot,
            LLVMPointerTypeInContext(codegen->context, 0), "map_box");
        LLVMValueRef loaded = LLVMBuildLoad2(codegen->builder, vt, box, "map_boxed_val");
        LLVMBuildBr(codegen->builder, done_bb);
        LLVMPositionBuilderAtEnd(codegen->builder, done_bb);
        LLVMValueRef phi = LLVMBuildPhi(codegen->builder, vt, "map_val");
        LLVMValueRef zero = LLVMConstNull(vt);
        LLVMValueRef inc_vals[2] = { zero, loaded };
        LLVMBasicBlockRef inc_bbs[2] = { cur, load_bb };
        LLVMAddIncoming(phi, inc_vals, inc_bbs, 2);
        return phi;
    }
    return LLVMBuildIntCast2(codegen->builder, slot, vt, /*isSigned=*/0, "map_val");
}

// Map key kind for goo_map_new_sv: STRING(0) for string keys, INLINE(1) for
// integer/uint/bool/rune/byte/pointer, STRUCT(2) for a struct key compared
// via the map's synthesized per-field comparator (goo.structeq.<id>, see
// codegen_get_or_emit_struct_key_eq below), IFACE(3) for an interface-typed
// key compared via the runtime's goo_iface_key_eq (Task 2). Matches
// runtime.h's GOO_MAPKEY_* enum.
int codegen_map_key_kind(Type* key_type) {
    if (key_type && key_type->kind == TYPE_INTERFACE) return 3 /*GOO_MAPKEY_IFACE*/;
    if (key_type && key_type->kind == TYPE_STRUCT) return 2 /*GOO_MAPKEY_STRUCT*/;
    return (key_type && key_type->kind == TYPE_STRING) ? 0 /*GOO_MAPKEY_STRING*/ : 1 /*INLINE*/;
}

// Wrap a raw `char*` into the goo string aggregate `{i8*, i64}` via
// goo_string_new (copies the bytes, so the binding stays valid independent of
// the source) — the same construction the map-range loop already used inline
// for its string key (statement_codegen.c, pre-Task-2). Centralized here so
// codegen_map_slot_to_key can share it. Falls back to declaring the extern if
// codegen_declare_runtime_functions hasn't run yet (shouldn't happen in
// practice, but mirrors the defensive LLVMGetNamedFunction-or-declare pattern
// used throughout this codebase, e.g. codegen_alloc_local's goo_alloc lookup).
LLVMValueRef codegen_string_from_cstr(CodeGenerator* codegen, LLVMValueRef cptr) {
    if (!codegen || !cptr) return NULL;
    LLVMContextRef ctx = codegen->context;
    LLVMValueRef mkstr_fn = LLVMGetNamedFunction(codegen->module, "goo_string_new");
    if (!mkstr_fn) {
        LLVMTypeRef ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
        LLVMTypeRef string_type = LLVMStructTypeInContext(ctx, (LLVMTypeRef[]){ ptr_type, i64 }, 2, 0);
        LLVMTypeRef mkstr_params[] = { ptr_type };
        LLVMTypeRef mkstr_fn_type = LLVMFunctionType(string_type, mkstr_params, 1, 0);
        mkstr_fn = LLVMAddFunction(codegen->module, "goo_string_new", mkstr_fn_type);
    }
    LLVMValueRef args[1] = { cptr };
    return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(mkstr_fn),
                          mkstr_fn, args, 1, "kstr_wrap");
}

// Get-or-declare the libc `strcmp(const char*, const char*) -> int` extern,
// mirroring codegen_string_from_cstr's LLVMGetNamedFunction-or-declare
// pattern above. Used by codegen_get_or_emit_struct_key_eq for STRING struct
// fields. The final link already pulls in libc (codegen_emit_executable
// shells out to `clang ... -lm -lpthread`, which links libc by default), so
// declaring the extern here (never defining it) resolves at link time like
// any other libc call.
static LLVMValueRef codegen_get_or_declare_strcmp(CodeGenerator* codegen) {
    LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "strcmp");
    if (fn) return fn;
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
    LLVMTypeRef params[2] = { i8p, i8p };
    LLVMTypeRef fnty = LLVMFunctionType(i32, params, 2, 0);
    return LLVMAddFunction(codegen->module, "strcmp", fnty);
}

// Single funnel point for every heap allocation the compiler emits: new(T),
// &StructLiteral{}, slice-literal backing, closure environments,
// escape/capture-promoted locals, map value/key boxing, interface boxing, and
// go-statement argument boxing all used to inline their own copy of "get or
// declare goo_alloc, LLVMBuildCall2, use the raw pointer" — nine near-identical
// copies of the same idiom (see git history pre-dating this helper for the
// scattered originals). Consolidated here so a later arena-region task has
// exactly ONE place to branch `kind` on instead of nine.
//
// Arena-regions Task 3: push `arena` onto codegen->arena_stack. Silently
// drops the push past the fixed depth (matches the loop-context stack's
// depth-32 cap style above) rather than growing — arena nesting this deep
// is not an expected real-world shape. Nothing calls this yet; Task 6's
// `arena{}` block lowering is the first caller.
void codegen_arena_push(CodeGenerator* codegen, LLVMValueRef arena) {
    if (!codegen) return;
    size_t cap = sizeof(codegen->arena_stack) / sizeof(codegen->arena_stack[0]);
    if ((size_t)codegen->arena_depth >= cap) return;
    // Record the loop nesting at push time so a break/continue can free only
    // the arenas pushed inside the loop it exits (see arena_loop_depth's doc).
    codegen->arena_loop_depth[codegen->arena_depth] = codegen->cfctx.loop_depth;
    codegen->arena_stack[codegen->arena_depth++] = arena;
}

// Arena-regions Task 3: pop the innermost active arena. No-op when already
// empty.
void codegen_arena_pop(CodeGenerator* codegen) {
    if (!codegen || codegen->arena_depth <= 0) return;
    codegen->arena_depth--;
}

// Arena-regions Task 3: the innermost active arena's SSA pointer, or NULL
// when no `arena{}` block is currently active. codegen_emit_alloc uses this
// to decide whether to route to goo_arena_alloc or goo_alloc.
LLVMValueRef codegen_arena_current(CodeGenerator* codegen) {
    if (!codegen || codegen->arena_depth <= 0) return NULL;
    return codegen->arena_stack[codegen->arena_depth - 1];
}

// Arena-regions Task 7c: the testable seam. True iff an allocation for
// `alloc_site` should be routed to the active arena — an arena must be on
// the stack, `kind` must be the (only, today) arena-eligible kind, AND the
// site must not be classified as escaping its enclosing arena block.
// block_escape_site_escapes returns true on a NULL/unknown site, so a NULL
// alloc_site (every call site 7c does not yet classify) or a site outside
// any arena block falls through to false here -> codegen_emit_alloc's heap
// path, unchanged. Touches only arena_stack/arena_depth/block_escape, never
// the builder/module, so it is safe to call on a lightweight CodeGenerator
// built without codegen_new (see arena_routing_test.c).
bool codegen_arena_eligible(CodeGenerator* codegen, ASTNode* alloc_site, AllocKind kind) {
    return codegen_arena_current(codegen) != NULL
        && kind == ALLOC_KIND_DEFAULT
        && !block_escape_site_escapes(codegen ? codegen->block_escape : NULL, alloc_site);
}

// `alloc_site` (7c) is the AST node this allocation originates from; NULL
// for any call site block_escape.c does not yet classify — always falls
// through to heap via codegen_arena_eligible's NULL-site miss contract.
// With an empty arena stack (true for every program until Task 6 ships
// `arena{}` syntax) this is byte-identical to the pre-arena goo_alloc-only
// path (verified by the unchanged golden suite).
LLVMValueRef codegen_emit_alloc(CodeGenerator* codegen, LLVMValueRef size, AllocKind kind, ASTNode* alloc_site) {
    if (!codegen || !size) return NULL;
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef size_type = LLVMInt64TypeInContext(ctx);

    LLVMValueRef current_arena = codegen_arena_current(codegen);
    if (codegen_arena_eligible(codegen, alloc_site, kind)) {
        LLVMTypeRef arena_params[] = { ptr_type, size_type };
        LLVMTypeRef arena_alloc_ty = LLVMFunctionType(ptr_type, arena_params, 2, 0);
        LLVMValueRef arena_alloc_fn = LLVMGetNamedFunction(codegen->module, "goo_arena_alloc");
        if (!arena_alloc_fn) {
            arena_alloc_fn = LLVMAddFunction(codegen->module, "goo_arena_alloc", arena_alloc_ty);
        }
        LLVMValueRef args[] = { current_arena, size };
        return LLVMBuildCall2(codegen->builder, arena_alloc_ty, arena_alloc_fn, args, 2, "arena_alloc");
    }

    LLVMTypeRef alloc_ty = LLVMFunctionType(ptr_type, &size_type, 1, 0);
    LLVMValueRef alloc_fn = LLVMGetNamedFunction(codegen->module, "goo_alloc");
    if (!alloc_fn) alloc_fn = LLVMAddFunction(codegen->module, "goo_alloc", alloc_ty);
    return LLVMBuildCall2(codegen->builder, alloc_ty, alloc_fn, &size, 1, "alloc");
}

// Struct-typed map keys (Task 2): synthesize (or return the cached) per-field
// equality comparator for `struct_type` — `i32 @goo.structeq.<id>(i64 a, i64
// b)`. Cast both slots to `struct_type*`, then for every DECLARED field (in
// order, so LLVM struct-field index == Goo field index — codegen_get_struct_
// type builds them 1:1):
//   - string             : extract field 0 (char*) from both loaded string
//                           aggregates, `strcmp(...) == 0`.
//   - nested struct       : GEP the field's ADDRESS in both operands,
//                           PtrToInt, and call THAT type's comparator
//                           (recursive codegen_get_or_emit_struct_key_eq,
//                           emitting the nested one first if not cached yet).
//   - float32/float64     : `fcmp oeq` (Go `==`; a NaN field is therefore
//                           never retrievable, matching Go — not a bug).
//   - everything else     : `icmp eq` (int/uint/bool/char/pointer — the only
//                           other kinds struct_is_comparable_key admits,
//                           type_checker.c).
// Short-circuits on the first mismatch (branches to a single shared "ret 0"
// block); falling through every field's compare reaches "ret 1". Cached by
// struct Type* IDENTITY (not name) so this is emitted exactly once per
// distinct type — including two independent anonymous struct types, which
// would otherwise collide if the cache were keyed by name (both have none).
// Returns NULL (no partial function left registered beyond an empty body —
// callers must treat any NULL as synthesis failure) if `struct_type` isn't a
// struct or a field's LLVM type can't be resolved.
LLVMValueRef codegen_get_or_emit_struct_key_eq(CodeGenerator* codegen, TypeChecker* checker,
                                               Type* struct_type) {
    if (!codegen || !struct_type || struct_type->kind != TYPE_STRUCT) return NULL;

    for (size_t i = 0; i < codegen->structeq_cache_size; i++) {
        if (codegen->structeq_cache_keys[i] == struct_type) {
            return codegen->structeq_cache_vals[i];
        }
    }

    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);

    LLVMTypeRef sty = codegen_type_to_llvm(codegen, struct_type);
    if (!sty) return NULL;
    LLVMTypeRef sptr = LLVMPointerType(sty, 0);

    char fname[64];
    snprintf(fname, sizeof(fname), "goo.structeq.%lu", codegen->structeq_counter++);
    LLVMTypeRef param_types[2] = { i64, i64 };
    LLVMTypeRef fnty = LLVMFunctionType(i32, param_types, 2, 0);
    LLVMValueRef fn = LLVMAddFunction(codegen->module, fname, fnty);
    if (!fn) return NULL;

    // Insert into the cache BEFORE emitting the body: a nested struct field
    // referring back to an ancestor type (mutual struct-in-struct nesting)
    // would otherwise recurse forever instead of reusing this in-progress
    // function — mirrors codegen_get_struct_type's opaque-struct-first
    // insertion for the identical reason (type_mapping.c).
    if (codegen->structeq_cache_size == codegen->structeq_cache_cap) {
        size_t new_cap = codegen->structeq_cache_cap ? codegen->structeq_cache_cap * 2 : 8;
        const Type** new_keys = realloc(codegen->structeq_cache_keys, new_cap * sizeof(const Type*));
        if (!new_keys) return NULL;
        codegen->structeq_cache_keys = new_keys;
        LLVMValueRef* new_vals = realloc(codegen->structeq_cache_vals, new_cap * sizeof(LLVMValueRef));
        if (!new_vals) return NULL;
        codegen->structeq_cache_vals = new_vals;
        codegen->structeq_cache_cap = new_cap;
    }
    codegen->structeq_cache_keys[codegen->structeq_cache_size] = struct_type;
    codegen->structeq_cache_vals[codegen->structeq_cache_size] = fn;
    codegen->structeq_cache_size++;

    // Emit the body, saving/restoring the caller's insert point (mirrors
    // build_thunk, interface_codegen.c) — this function may be synthesized
    // mid-expression (a map creation site inside some other function's body).
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(codegen->builder);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, fn, "entry");
    LLVMBasicBlockRef ret1_bb = LLVMAppendBasicBlockInContext(ctx, fn, "structeq.ret1");
    LLVMBasicBlockRef ret0_bb = LLVMAppendBasicBlockInContext(ctx, fn, "structeq.ret0");

    LLVMPositionBuilderAtEnd(codegen->builder, entry);
    LLVMValueRef a = LLVMGetParam(fn, 0);
    LLVMValueRef b = LLVMGetParam(fn, 1);
    LLVMValueRef pa = LLVMBuildIntToPtr(codegen->builder, a, sptr, "structeq.pa");
    LLVMValueRef pb = LLVMBuildIntToPtr(codegen->builder, b, sptr, "structeq.pb");

    size_t fc = struct_type->data.struct_type.field_count;
    LLVMBasicBlockRef cur_bb = entry;
    for (size_t i = 0; i < fc; i++) {
        LLVMPositionBuilderAtEnd(codegen->builder, cur_bb);
        Type* ft = struct_type->data.struct_type.fields[i].type;

        LLVMValueRef eq_bit;
        if (ft && ft->kind == TYPE_STRING) {
            LLVMTypeRef fllty = codegen_type_to_llvm(codegen, ft);
            if (!fllty) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }
            LLVMValueRef fa = LLVMBuildLoad2(codegen->builder, fllty,
                LLVMBuildStructGEP2(codegen->builder, sty, pa, (unsigned)i, "fa.ptr"), "fa");
            LLVMValueRef fb = LLVMBuildLoad2(codegen->builder, fllty,
                LLVMBuildStructGEP2(codegen->builder, sty, pb, (unsigned)i, "fb.ptr"), "fb");
            LLVMValueRef ca = LLVMBuildExtractValue(codegen->builder, fa, 0, "ca");
            LLVMValueRef cb = LLVMBuildExtractValue(codegen->builder, fb, 0, "cb");
            LLVMValueRef strcmp_fn = codegen_get_or_declare_strcmp(codegen);
            LLVMValueRef sargs[2] = { ca, cb };
            LLVMValueRef r = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(strcmp_fn),
                                            strcmp_fn, sargs, 2, "strcmp.r");
            eq_bit = LLVMBuildICmp(codegen->builder, LLVMIntEQ, r, LLVMConstInt(i32, 0, 0), "streq");
        } else if (ft && ft->kind == TYPE_STRUCT) {
            LLVMValueRef nested_eq = codegen_get_or_emit_struct_key_eq(codegen, checker, ft);
            if (!nested_eq) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }
            LLVMValueRef fa_ptr = LLVMBuildStructGEP2(codegen->builder, sty, pa, (unsigned)i, "nfa.ptr");
            LLVMValueRef fb_ptr = LLVMBuildStructGEP2(codegen->builder, sty, pb, (unsigned)i, "nfb.ptr");
            LLVMValueRef na = LLVMBuildPtrToInt(codegen->builder, fa_ptr, i64, "nfa.i64");
            LLVMValueRef nb = LLVMBuildPtrToInt(codegen->builder, fb_ptr, i64, "nfb.i64");
            LLVMValueRef nargs[2] = { na, nb };
            LLVMValueRef r = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(nested_eq),
                                            nested_eq, nargs, 2, "nested.eq");
            // r is the nested comparator's own i32 (1 == equal), not a raw
            // 3-way compare — != 0 means "equal", matching this function's
            // own return convention.
            eq_bit = LLVMBuildICmp(codegen->builder, LLVMIntNE, r, LLVMConstInt(i32, 0, 0), "nested.eqbit");
        } else if (ft && (ft->kind == TYPE_FLOAT32 || ft->kind == TYPE_FLOAT64)) {
            LLVMTypeRef fllty = codegen_type_to_llvm(codegen, ft);
            if (!fllty) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }
            LLVMValueRef fa = LLVMBuildLoad2(codegen->builder, fllty,
                LLVMBuildStructGEP2(codegen->builder, sty, pa, (unsigned)i, "fa.ptr"), "fa");
            LLVMValueRef fb = LLVMBuildLoad2(codegen->builder, fllty,
                LLVMBuildStructGEP2(codegen->builder, sty, pb, (unsigned)i, "fb.ptr"), "fb");
            eq_bit = LLVMBuildFCmp(codegen->builder, LLVMRealOEQ, fa, fb, "feq");
        } else {
            // int/uint/bool/char/pointer.
            LLVMTypeRef fllty = codegen_type_to_llvm(codegen, ft);
            if (!fllty) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }
            LLVMValueRef fa = LLVMBuildLoad2(codegen->builder, fllty,
                LLVMBuildStructGEP2(codegen->builder, sty, pa, (unsigned)i, "fa.ptr"), "fa");
            LLVMValueRef fb = LLVMBuildLoad2(codegen->builder, fllty,
                LLVMBuildStructGEP2(codegen->builder, sty, pb, (unsigned)i, "fb.ptr"), "fb");
            eq_bit = LLVMBuildICmp(codegen->builder, LLVMIntEQ, fa, fb, "eq");
        }

        if (i + 1 < fc) {
            LLVMBasicBlockRef next_bb =
                LLVMAppendBasicBlockInContext(ctx, fn, "structeq.next");
            LLVMBuildCondBr(codegen->builder, eq_bit, next_bb, ret0_bb);
            cur_bb = next_bb;
        } else {
            LLVMBuildCondBr(codegen->builder, eq_bit, ret1_bb, ret0_bb);
        }
    }
    if (fc == 0) {
        // Empty struct: every instance compares equal.
        LLVMPositionBuilderAtEnd(codegen->builder, entry);
        LLVMBuildBr(codegen->builder, ret1_bb);
    }

    LLVMPositionBuilderAtEnd(codegen->builder, ret1_bb);
    LLVMBuildRet(codegen->builder, LLVMConstInt(i32, 1, 0));
    LLVMPositionBuilderAtEnd(codegen->builder, ret0_bb);
    LLVMBuildRet(codegen->builder, LLVMConstInt(i32, 0, 0));

    if (saved) {
        LLVMPositionBuilderAtEnd(codegen->builder, saved);
    }
    return fn;
}

// Interface-typed map keys, Task 1 (vtable ABI shift): synthesize (or return
// the cached) per-concrete-type VALUE-equality comparator. It is stored as the
// eq_fn field of the per-type descriptor that interface vtable slot 0 points at
// (codegen_get_or_emit_type_desc, interface_codegen.c) —
// `i32 @goo.typeeq.<id>(i64 a, i64 b)`. `a`/`b` are the two interface `data`
// words for the SAME concrete type: the runtime only ever calls vtable
// slot 0 after a vtable-pointer match (goo_iface_key_eq, a later task), so
// both data words are guaranteed to be that one concrete's boxed
// representation, exactly mirroring codegen_interface_box's two shapes:
//   - TYPE_POINTER: boxed by aliasing (data IS the pointer, no heap copy) ->
//     icmp eq the two i64 words directly.
//   - integer/bool/char: boxed as a heap copy -> inttoptr each word to T*,
//     load, icmp eq.
//   - float32/float64: same shape, fcmp oeq (a NaN field is therefore never
//     equal to itself, matching Go/IEEE-754 — not a bug).
//   - string: inttoptr each word to the `{i8*,i64}` string aggregate, load,
//     extractvalue the char* (field 0), strcmp == 0.
//   - struct: delegates STRAIGHT to codegen_get_or_emit_struct_key_eq (#129)
//     — its i32(i64,i64)-over-ptr-to-heap-copy signature already matches a
//     struct concrete's boxing exactly; not re-synthesized, and NOT cached
//     in typeeq_cache_keys/vals (structeq_cache_keys/vals already memoizes
//     it, so a second lookup here would just be a wasted linear scan ahead
//     of the one structeq_cache already does).
//   - slice/map/func (uncomparable dynamic types, reachable through
//     `map[any]V` once a later task admits interface map keys): the single
//     shared `i32 @goo.uncmpeq(i64,i64)` stub — calls
//     `goo_panic("comparing uncomparable map key type")` and is
//     `unreachable` (Go-faithful: Go panics comparing/hashing an
//     uncomparable dynamic value, not a compile error). Any other concrete
//     kind not yet reachable as an interface concrete (TYPE_ARRAY,
//     TYPE_ENUM, TYPE_CHANNEL, ...) falls back to the same stub rather than
//     miscompiling or crashing the compiler.
// Cached by concrete Type* IDENTITY (typeeq_cache_keys/vals), mirroring
// codegen_get_or_emit_struct_key_eq's cache. Saves/restores the caller's
// builder insert position around emitting a new fn's body — codegen_
// interface_vtable may synthesize this mid-expression (a vtable built while
// generating some other function's body). Returns NULL on failure (the
// concrete's LLVM type can't be resolved, or `goo_panic` isn't declared in
// the module yet for the uncomparable-stub path).
LLVMValueRef codegen_get_or_emit_type_eq(CodeGenerator* codegen, TypeChecker* checker,
                                         Type* concrete) {
    if (!codegen || !concrete) return NULL;

    // Struct concretes delegate directly — no synthesis, no typeeq_cache
    // entry (see comment above).
    if (concrete->kind == TYPE_STRUCT) {
        return codegen_get_or_emit_struct_key_eq(codegen, checker, concrete);
    }

    for (size_t i = 0; i < codegen->typeeq_cache_size; i++) {
        if (codegen->typeeq_cache_keys[i] == concrete) {
            return codegen->typeeq_cache_vals[i];
        }
    }

    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);

    int scalar_kind = (concrete->kind == TYPE_POINTER || type_is_integer(concrete) ||
                       concrete->kind == TYPE_BOOL || concrete->kind == TYPE_CHAR ||
                       concrete->kind == TYPE_FLOAT32 || concrete->kind == TYPE_FLOAT64 ||
                       concrete->kind == TYPE_STRING);

    if (!scalar_kind) {
        // Uncomparable (slice/map/func) or any not-yet-reachable concrete
        // kind: the single shared panic-stub, emitted at most once.
        if (codegen->uncmpeq_fn) return codegen->uncmpeq_fn;

        LLVMValueRef panic_fn = LLVMGetNamedFunction(codegen->module, "goo_panic");
        if (!panic_fn) return NULL;

        LLVMTypeRef param_types[2] = { i64, i64 };
        LLVMTypeRef fnty = LLVMFunctionType(i32, param_types, 2, 0);
        LLVMValueRef fn = LLVMAddFunction(codegen->module, "goo.uncmpeq", fnty);
        if (!fn) return NULL;
        // Register before emitting the body: this stub is shared by every
        // uncomparable concrete, so a second caller (still inside this same
        // synthesis) must see it already exists rather than trying to
        // LLVMAddFunction the same symbol name twice.
        codegen->uncmpeq_fn = fn;

        LLVMBasicBlockRef saved = LLVMGetInsertBlock(codegen->builder);
        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, fn, "entry");
        LLVMPositionBuilderAtEnd(codegen->builder, entry);
        LLVMValueRef msg = LLVMBuildGlobalStringPtr(
            codegen->builder, "comparing uncomparable map key type", "uncmpeq_msg");
        LLVMValueRef args[1] = { msg };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(panic_fn), panic_fn, args, 1, "");
        LLVMBuildUnreachable(codegen->builder);
        if (saved) {
            LLVMPositionBuilderAtEnd(codegen->builder, saved);
        }
        return fn;
    }

    char fname[64];
    snprintf(fname, sizeof(fname), "goo.typeeq.%lu", codegen->typeeq_counter++);
    LLVMTypeRef param_types[2] = { i64, i64 };
    LLVMTypeRef fnty = LLVMFunctionType(i32, param_types, 2, 0);
    LLVMValueRef fn = LLVMAddFunction(codegen->module, fname, fnty);
    if (!fn) return NULL;

    // Register in the cache before emitting the body, mirroring structeq's
    // insert-before-body ordering (harmless here — a scalar/string/pointer
    // eq body never recurses back into this same concrete — kept only for
    // consistency with the sibling cache).
    if (codegen->typeeq_cache_size == codegen->typeeq_cache_cap) {
        size_t new_cap = codegen->typeeq_cache_cap ? codegen->typeeq_cache_cap * 2 : 8;
        const Type** new_keys = realloc(codegen->typeeq_cache_keys, new_cap * sizeof(const Type*));
        if (!new_keys) return NULL;
        codegen->typeeq_cache_keys = new_keys;
        LLVMValueRef* new_vals = realloc(codegen->typeeq_cache_vals, new_cap * sizeof(LLVMValueRef));
        if (!new_vals) return NULL;
        codegen->typeeq_cache_vals = new_vals;
        codegen->typeeq_cache_cap = new_cap;
    }
    codegen->typeeq_cache_keys[codegen->typeeq_cache_size] = concrete;
    codegen->typeeq_cache_vals[codegen->typeeq_cache_size] = fn;
    codegen->typeeq_cache_size++;

    LLVMBasicBlockRef saved = LLVMGetInsertBlock(codegen->builder);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(codegen->builder, entry);
    LLVMValueRef a = LLVMGetParam(fn, 0);
    LLVMValueRef b = LLVMGetParam(fn, 1);

    LLVMValueRef eq_bit;
    if (concrete->kind == TYPE_POINTER) {
        eq_bit = LLVMBuildICmp(codegen->builder, LLVMIntEQ, a, b, "typeeq.ptreq");
    } else if (concrete->kind == TYPE_STRING) {
        LLVMTypeRef strty = codegen_type_to_llvm(codegen, concrete);
        if (!strty) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }
        LLVMTypeRef strptr = LLVMPointerType(strty, 0);
        LLVMValueRef pa = LLVMBuildIntToPtr(codegen->builder, a, strptr, "typeeq.pa");
        LLVMValueRef pb = LLVMBuildIntToPtr(codegen->builder, b, strptr, "typeeq.pb");
        LLVMValueRef va = LLVMBuildLoad2(codegen->builder, strty, pa, "typeeq.va");
        LLVMValueRef vb = LLVMBuildLoad2(codegen->builder, strty, pb, "typeeq.vb");
        LLVMValueRef ca = LLVMBuildExtractValue(codegen->builder, va, 0, "typeeq.ca");
        LLVMValueRef cb = LLVMBuildExtractValue(codegen->builder, vb, 0, "typeeq.cb");
        LLVMValueRef strcmp_fn = codegen_get_or_declare_strcmp(codegen);
        LLVMValueRef sargs[2] = { ca, cb };
        LLVMValueRef r = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(strcmp_fn),
                                        strcmp_fn, sargs, 2, "typeeq.strcmp");
        eq_bit = LLVMBuildICmp(codegen->builder, LLVMIntEQ, r, LLVMConstInt(i32, 0, 0), "typeeq.streq");
    } else {
        // integer/bool/char/float32/float64: deref both words as T*.
        LLVMTypeRef ty = codegen_type_to_llvm(codegen, concrete);
        if (!ty) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }
        LLVMTypeRef typtr = LLVMPointerType(ty, 0);
        LLVMValueRef pa = LLVMBuildIntToPtr(codegen->builder, a, typtr, "typeeq.pa");
        LLVMValueRef pb = LLVMBuildIntToPtr(codegen->builder, b, typtr, "typeeq.pb");
        LLVMValueRef va = LLVMBuildLoad2(codegen->builder, ty, pa, "typeeq.va");
        LLVMValueRef vb = LLVMBuildLoad2(codegen->builder, ty, pb, "typeeq.vb");
        if (concrete->kind == TYPE_FLOAT32 || concrete->kind == TYPE_FLOAT64) {
            eq_bit = LLVMBuildFCmp(codegen->builder, LLVMRealOEQ, va, vb, "typeeq.feq");
        } else {
            eq_bit = LLVMBuildICmp(codegen->builder, LLVMIntEQ, va, vb, "typeeq.ieq");
        }
    }

    LLVMValueRef result32 = LLVMBuildZExt(codegen->builder, eq_bit, i32, "typeeq.r");
    LLVMBuildRet(codegen->builder, result32);

    if (saved) {
        LLVMPositionBuilderAtEnd(codegen->builder, saved);
    }
    return fn;
}

// Interface-typed map keys (Task 2): box a concrete key value into the map's
// declared TYPE_INTERFACE key type before codegen_map_key_to_slot packs it —
// mirrors the box-before-slot pattern every interface-typed map VALUE site
// already applies via codegen_interface_box (expression_codegen.c's map
// literal and `m[k] = v` arms). Every call site that packs a user-supplied
// key (assignment, plain/comma-ok read, delete, compound-assign RMW, map
// literal entries) calls this FIRST, then passes the (possibly rewritten)
// key_val on to codegen_map_key_to_slot.
//
// A no-op (returns 1 immediately) unless key_type is TYPE_INTERFACE and the
// key expression's OWN static type is a different, non-interface concrete —
// covering both existing behaviors unchanged: (a) every non-interface-keyed
// map (the overwhelming majority of call sites) skips this entirely; (b) a
// key whose static type is ALREADY that interface (`var s any = 5; m[s] = v`)
// falls straight through to codegen_map_key_to_slot's own is_lvalue reload,
// which is correct there since the map's declared key type and the key
// expression's type are the same interface struct type.
//
// Why this can't be folded into codegen_map_key_to_slot itself: that
// function's is_lvalue reload loads `key_val->llvm_value` using the
// DECLARED key type `kt`'s LLVM type, assuming the storage it points at
// already has that type. For a concrete key into an interface-keyed map
// (e.g. `m[1] = 10` where m is map[any]int), that assumption is false — `1`
// is an i64 rvalue (or an i64* alloca if it came from a variable), not a
// `{vtable,data}` pair. Boxing must happen — and any is_lvalue reload of the
// CONCRETE value must happen using the CONCRETE type — strictly before
// codegen_map_key_to_slot ever sees kt=TYPE_INTERFACE, hence a separate
// pre-step rather than an arm inside that function.
//
// Mutates `key_val` in place (llvm_value/goo_type/is_lvalue) on success.
// Returns 1 on success (including the no-op cases above), 0 on failure
// (already reported via codegen_error).
int codegen_box_map_key_if_needed(CodeGenerator* codegen, TypeChecker* checker,
                                  ValueInfo* key_val, Type* key_type, Position pos) {
    if (!key_val || !key_type || key_type->kind != TYPE_INTERFACE) return 1;
    if (!key_val->goo_type || key_val->goo_type->kind == TYPE_INTERFACE) return 1;

    LLVMValueRef raw = key_val->llvm_value;
    if (key_val->is_lvalue) {
        LLVMTypeRef llvm_t = codegen_type_to_llvm(codegen, key_val->goo_type);
        if (!llvm_t) return 0;
        raw = LLVMBuildLoad2(codegen->builder, llvm_t, raw, "mapkey_concrete_load");
    }
    LLVMValueRef boxed = codegen_interface_box(codegen, checker, key_type, key_val->goo_type, raw);
    if (!boxed) {
        codegen_error(codegen, pos, "failed to box map key into interface key type");
        return 0;
    }
    key_val->llvm_value = boxed;
    key_val->goo_type = key_type;
    key_val->is_lvalue = 0;
    return 1;
}

// Pack a key value into the i64 slot. STRING: extract the char* and PtrToInt
// (NOT codegen_map_value_to_slot — that heap-boxes strings, which would break
// strcmp identity: two equal-content string keys must PtrToInt to the SAME
// comparable representation the runtime's key_kind==STRING arm strcmp's,
// not two distinct box addresses). INLINE: reuse the value-slot packer (its
// inline arm is exactly PtrToInt/ZExt/SExt/bitcast — safe to share because
// map keys are restricted to inline-eligible types; Task 3's typecheck gate
// enforces that).
//
// `key_val` carries is_lvalue: a key expression that is itself an lvalue
// (slice/array element `a[i]`, struct field `p.X`, string byte-index `s[i]`
// — codegen_generate_index_expr/codegen_generate_selector_expr always return
// those as is_lvalue=1 with llvm_value the element/field ADDRESS, not the
// value) must be loaded to its rvalue here before packing — mirrored from
// the value/RHS load every value-slot call site already does. Without this
// load, INLINE keys would sext/zext a `ptr` (LLVM verifier failure) and
// STRING keys would ExtractValue a raw address instead of the string
// aggregate (compiler SIGSEGV).
LLVMValueRef codegen_map_key_to_slot(CodeGenerator* codegen, TypeChecker* checker,
                                     ValueInfo* key_val, Type* key_type) {
    (void)checker;
    if (!codegen || !key_val || !key_val->llvm_value) return NULL;
    Type* kt = key_type ? key_type : key_val->goo_type;
    LLVMValueRef raw = key_val->llvm_value;
    if (key_val->is_lvalue && kt) {
        LLVMTypeRef llvm_kt = codegen_type_to_llvm(codegen, kt);
        if (llvm_kt) {
            raw = LLVMBuildLoad2(codegen->builder, llvm_kt, raw, "mapkey_load");
        }
    }
    LLVMTypeRef i64 = LLVMInt64TypeInContext(codegen->context);
    if (kt && kt->kind == TYPE_STRING) {
        // string aggregate {i8*, i64}: take field 0 (the char*), int-ize it.
        LLVMValueRef cptr = LLVMBuildExtractValue(codegen->builder, raw, 0, "kstr_ptr");
        return LLVMBuildPtrToInt(codegen->builder, cptr, i64, "kstr_slot");
    }
    if (kt && kt->kind == TYPE_STRUCT) {
        // Heap-copy the struct key (mirrors codegen_map_value_to_slot's
        // boxed-value arm): two DISTINCT copies with equal fields must
        // still hit the same map entry, which the synthesized
        // goo.structeq.<id> comparator (not pointer identity) provides —
        // the slot only needs to carry an address the comparator can
        // dereference. `raw` is already the loaded struct VALUE (composite
        // literals and identifiers both yield rvalues here; the is_lvalue
        // load above already ran for lvalue key expressions).
        LLVMTypeRef sty = codegen_type_to_llvm(codegen, kt);
        if (!sty) return NULL;
        LLVMValueRef size = LLVMSizeOf(sty);
        LLVMValueRef mem = codegen_emit_alloc(codegen, size, ALLOC_KIND_DEFAULT, NULL);
        LLVMBuildStore(codegen->builder, raw, mem);
        return LLVMBuildPtrToInt(codegen->builder, mem, i64, "skey_slot");
    }
    if (kt && kt->kind == TYPE_INTERFACE) {
        // Interface-typed map keys (Task 2): heap-copy the boxed `{vtable,
        // data}` value — mirrors the struct arm immediately above exactly
        // (an interface key is, ABI-wise, just another two-pointer
        // aggregate). `raw` is already the loaded interface VALUE: either
        // the key expression's own static type was already TYPE_INTERFACE
        // (is_lvalue load above used kt == the interface struct type, which
        // is correct), or the caller pre-boxed a concrete key into this
        // interface via codegen_box_map_key_if_needed before reaching here
        // (every call site that packs a user-supplied key does this — see
        // that helper's doc comment). Two DISTINCT boxes of the SAME dynamic
        // type+value must still hit the same map entry, which
        // goo_iface_key_eq (not pointer identity) provides — the slot only
        // needs to carry an address the comparator can dereference.
        LLVMTypeRef ifty = codegen_type_to_llvm(codegen, kt);
        if (!ifty) return NULL;
        LLVMValueRef size = LLVMSizeOf(ifty);
        LLVMValueRef mem = codegen_emit_alloc(codegen, size, ALLOC_KIND_DEFAULT, NULL);
        LLVMBuildStore(codegen->builder, raw, mem);
        return LLVMBuildPtrToInt(codegen->builder, mem, i64, "ikey_slot");
    }
    return codegen_map_value_to_slot(codegen, raw, kt);
}

// Inverse for range key binding. STRING: slot holds a char* (the runtime
// stores string keys as their raw pointer, per key_kind==STRING); rebuild the
// goo string aggregate via codegen_string_from_cstr, mirroring exactly what
// the range loop's key-bind step did before this change (statement_codegen.c:
// goo_string_new(key_cstr)). INLINE: reuse the value unpacker.
LLVMValueRef codegen_map_slot_to_key(CodeGenerator* codegen, LLVMValueRef slot, Type* key_type) {
    if (!codegen || !slot) return NULL;
    if (key_type && key_type->kind == TYPE_STRING) {
        LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
        LLVMValueRef cptr = LLVMBuildIntToPtr(codegen->builder, slot, i8p, "kstr_ptr");
        return codegen_string_from_cstr(codegen, cptr);
    }
    if (key_type && key_type->kind == TYPE_STRUCT) {
        LLVMTypeRef sty = codegen_type_to_llvm(codegen, key_type);
        if (!sty) return NULL;
        LLVMValueRef sp = LLVMBuildIntToPtr(codegen->builder, slot,
                                           LLVMPointerType(sty, 0), "skey_ptr");
        return LLVMBuildLoad2(codegen->builder, sty, sp, "skey_val");
    }
    if (key_type && key_type->kind == TYPE_INTERFACE) {
        // Interface-typed map keys (Task 2): inverse of codegen_map_key_to_
        // slot's TYPE_INTERFACE arm — IntToPtr the slot back to a pointer to
        // the boxed `{vtable, data}` value and load it. Used by range-over-
        // map's key binding (statement_codegen.c).
        LLVMTypeRef ifty = codegen_type_to_llvm(codegen, key_type);
        if (!ifty) return NULL;
        LLVMValueRef ip = LLVMBuildIntToPtr(codegen->builder, slot,
                                           LLVMPointerType(ifty, 0), "ikey_ptr");
        return LLVMBuildLoad2(codegen->builder, ifty, ip, "ikey_val");
    }
    return codegen_map_slot_to_value(codegen, slot, key_type);
}

LLVMValueRef codegen_create_entry_alloca(CodeGenerator* codegen, LLVMTypeRef type, const char* name) {
    if (!codegen || !type || !codegen->current_function_info) return NULL;
    
    // Save current position
    LLVMBasicBlockRef current_block = LLVMGetInsertBlock(codegen->builder);
    
    // Move to beginning of entry block
    LLVMBasicBlockRef entry = codegen->current_function_info->entry_block;
    if (!entry) {
        entry = LLVMGetEntryBasicBlock(codegen->current_function);
    }
    
    LLVMValueRef first_instruction = LLVMGetFirstInstruction(entry);
    if (first_instruction) {
        LLVMPositionBuilderBefore(codegen->builder, first_instruction);
    } else {
        LLVMPositionBuilderAtEnd(codegen->builder, entry);
    }
    
    // Create alloca
    LLVMValueRef alloca = LLVMBuildAlloca(codegen->builder, type, name);
    
    // Restore position
    if (current_block) {
        LLVMPositionBuilderAtEnd(codegen->builder, current_block);
    }
    
    return alloca;
}

LLVMBasicBlockRef codegen_create_block(CodeGenerator* codegen, const char* name) {
    if (!codegen || !codegen->current_function) return NULL;
    
    return LLVMAppendBasicBlockInContext(codegen->context, codegen->current_function, name);
}

void codegen_set_insert_point(CodeGenerator* codegen, LLVMBasicBlockRef block) {
    if (!codegen || !block) return;
    
    LLVMPositionBuilderAtEnd(codegen->builder, block);
}

#endif

// Output generation stubs (to be implemented)
int codegen_emit_llvm_ir(CodeGenerator* codegen __attribute__((unused)), const char* filename __attribute__((unused))) {
#if LLVM_AVAILABLE
    if (!codegen || !filename) return 0;
    
    char* error_message = NULL;
    if (LLVMPrintModuleToFile(codegen->module, filename, &error_message)) {
        fprintf(stderr, "Error writing LLVM IR: %s\n", error_message);
        LLVMDisposeMessage(error_message);
        return 0;
    }
    
    return 1;
#else
    return 0;
#endif
}

int codegen_emit_object_file(CodeGenerator* codegen, const char* filename) {
#if LLVM_AVAILABLE
    if (!codegen || !filename) return 0;
    
    // Verify module first
    if (!codegen_verify_module(codegen)) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Module verification failed");
        return 0;
    }
    
    // Initialize target if not already done
    if (!codegen->target_machine) {
        char* error_message = NULL;
        
        // Get target triple
        char* target_triple = codegen->target_triple;
        if (!target_triple) {
            target_triple = LLVMGetDefaultTargetTriple();
        }
        
        // Get target from triple
        LLVMTargetRef target;
        if (LLVMGetTargetFromTriple(target_triple, &target, &error_message)) {
            codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Failed to get target: %s", error_message);
            LLVMDisposeMessage(error_message);
            if (!codegen->target_triple) LLVMDisposeMessage(target_triple);
            return 0;
        }
        
        // Create target machine
        const char* cpu = codegen->target_cpu ? codegen->target_cpu : "generic";
        const char* features = codegen->target_features ? codegen->target_features : "";
        
        codegen->target_machine = LLVMCreateTargetMachine(
            target, target_triple, cpu, features,
            LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault
        );
        
        if (!codegen->target_machine) {
            codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Failed to create target machine");
            if (!codegen->target_triple) LLVMDisposeMessage(target_triple);
            return 0;
        }
        
        // Set target data layout
        LLVMTargetDataRef target_data = LLVMCreateTargetDataLayout(codegen->target_machine);
        char* data_layout = LLVMCopyStringRepOfTargetData(target_data);
        LLVMSetDataLayout(codegen->module, data_layout);
        LLVMDisposeMessage(data_layout);
        LLVMDisposeTargetData(target_data);
        
        if (!codegen->target_triple) {
            LLVMDisposeMessage(target_triple);
        }
    }
    
    // Emit object file
    char* error_message = NULL;
    if (LLVMTargetMachineEmitToFile(codegen->target_machine, codegen->module, 
                                   (char*)filename, LLVMObjectFile, &error_message)) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Failed to emit object file: %s", error_message);
        LLVMDisposeMessage(error_message);
        return 0;
    }
    
    return 1;
#else
    return 0;
#endif
}

// Resolve the runtime archive path independent of cwd so the compiler works
// when invoked from any directory. Order: $GOO_RUNTIME -> <exe-dir>/../lib ->
// cwd-relative fallback. Returns a pointer into a static buffer (single-threaded
// codegen; not reentrant — acceptable here).
static const char* goo_runtime_archive_path(void) {
    static char buf[4096];
    const char* env = getenv("GOO_RUNTIME");
    if (env && env[0] != '\0') { snprintf(buf, sizeof(buf), "%s", env); return buf; }
#ifdef __linux__
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char* slash = strrchr(exe, '/');        // dir-of-exe
        if (slash) {
            *slash = '\0';
            snprintf(buf, sizeof(buf), "%s/../lib/libgoo_runtime.a", exe);
            struct stat st;
            if (stat(buf, &st) == 0) return buf; // exists relative to the binary
        }
    }
#endif
    snprintf(buf, sizeof(buf), "lib/libgoo_runtime.a");  // fallback (repo-root cwd)
    return buf;
}

// Join an argv vector with spaces for a human-readable link-failure message
// ONLY — the real link invocation below never goes through a shell (that's
// the whole point of fork+execvp over system()), so this has no
// quoting/injection exposure; it exists purely to keep the diagnostic as
// informative as the old system()-command string was. Returns NULL on
// allocation failure (caller degrades to a generic message).
static char* codegen_join_argv(char* const argv[]) {
    size_t len = 0;
    for (int i = 0; argv[i]; i++) len += strlen(argv[i]) + 1;
    if (len == 0) return NULL;
    char* out = malloc(len);
    if (!out) return NULL;
    out[0] = '\0';
    for (int i = 0; argv[i]; i++) {
        strcat(out, argv[i]);
        if (argv[i + 1]) strcat(out, " ");
    }
    return out;
}

int codegen_emit_executable(CodeGenerator* codegen, const char* filename) {
#if LLVM_AVAILABLE
    if (!codegen || !filename) return 0;
    
    // Verify module first
    if (!codegen_verify_module(codegen)) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Module verification failed");
        return 0;
    }
    
    // Initialize target if not already done
    if (!codegen->target_machine) {
        char* error_message = NULL;
        
        // Get target triple
        char* target_triple = codegen->target_triple;
        if (!target_triple) {
            target_triple = LLVMGetDefaultTargetTriple();
        }
        
        // Get target from triple
        LLVMTargetRef target;
        if (LLVMGetTargetFromTriple(target_triple, &target, &error_message)) {
            codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Failed to get target: %s", error_message);
            LLVMDisposeMessage(error_message);
            if (!codegen->target_triple) LLVMDisposeMessage(target_triple);
            return 0;
        }
        
        // Create target machine
        const char* cpu = codegen->target_cpu ? codegen->target_cpu : "generic";
        const char* features = codegen->target_features ? codegen->target_features : "";

        // P3.10: -O0 (opt_level==0, the default) keeps LLVMCodeGenLevelDefault
        // exactly as before — the byte-identical contract. Only -O3 asks
        // the backend's own codegen for Aggressive; -O1/-O2 stay Default
        // (codegen_optimize's new-PM IR pipeline does the real work there).
        LLVMCodeGenOptLevel cg_opt_level = (codegen->opt_level >= 3)
            ? LLVMCodeGenLevelAggressive : LLVMCodeGenLevelDefault;
        codegen->target_machine = LLVMCreateTargetMachine(
            target, target_triple, cpu, features,
            cg_opt_level, LLVMRelocDefault, LLVMCodeModelDefault
        );
        
        if (!codegen->target_machine) {
            codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Failed to create target machine");
            if (!codegen->target_triple) LLVMDisposeMessage(target_triple);
            return 0;
        }
        
        // Set target data layout
        LLVMTargetDataRef target_data = LLVMCreateTargetDataLayout(codegen->target_machine);
        char* data_layout = LLVMCopyStringRepOfTargetData(target_data);
        LLVMSetDataLayout(codegen->module, data_layout);
        LLVMDisposeMessage(data_layout);
        LLVMDisposeTargetData(target_data);
        
        if (!codegen->target_triple) {
            LLVMDisposeMessage(target_triple);
        }
    }
    
    // Generate object file first
    char* object_filename = malloc(strlen(filename) + 3); // +2 for ".o", +1 for null terminator
    if (!object_filename) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Memory allocation failed");
        return 0;
    }
    sprintf(object_filename, "%s.o", filename);

    // Defensive backstop, not a live gate today: the verify at this
    // function's entry already rejects any verifier-visible IR, and nothing
    // mutates the module between there and here (target-machine setup only
    // stamps a data layout), so this second check can only fire if a future
    // refactor removes/reorders the entry check or adds IR mutation in
    // between. Know its limits either way: LLVMVerifyModule accepts some
    // shapes that still crash SelectionDAG — the error-union-of-nullable
    // (!?T) module SIGILLs in llvm::EVT::getExtendedSizeInBits despite
    // verifying clean. That class is closed upstream by the type checker's
    // !?T rejection, not here.
    char* verify_error_message = NULL;
    if (LLVMVerifyModule(codegen->module, LLVMReturnStatusAction, &verify_error_message)) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"},
                     "internal compiler error: generated invalid IR (please report): %s",
                     verify_error_message);
        LLVMDisposeMessage(verify_error_message);
        free(object_filename);
        return 0;
    }
    if (verify_error_message) {
        LLVMDisposeMessage(verify_error_message);
    }

    char* error_message = NULL;
    if (LLVMTargetMachineEmitToFile(codegen->target_machine, codegen->module,
                                   object_filename, LLVMObjectFile, &error_message)) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Failed to emit object file: %s", error_message);
        LLVMDisposeMessage(error_message);
        free(object_filename);
        return 0;
    }
    
    // Link object file to create executable.
    //
    // fork()+execvp() on an argv vector instead of system(link_command):
    // system() shells the whole command through /bin/sh, which (a) forced a
    // fixed 2048-byte buffer here and (b) word-splits any unquoted path
    // component — an output path containing a space silently broke (e.g.
    // `-o "build/dir with space/hello"` linked into three garbage argv
    // words). execvp takes argv entries directly: no shell, no buffer
    // limit, no quoting to get right. (P3.11)
#ifdef _WIN32
    // No POSIX fork()/execvp() on Windows, and Windows is out of the v1 test
    // surface (no CI runs it) — keep the original shell-based path here.
    char link_command[2048];
    snprintf(link_command, sizeof(link_command),
             "link.exe /OUT:%s %s %s /ENTRY:main /SUBSYSTEM:CONSOLE",
             filename, object_filename, goo_runtime_archive_path());
    int link_result = system(link_command);
    if (link_result != 0) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"},
                     "Linking failed with command: %s", link_command);
        remove(object_filename);   // don't leave a stray object behind
        free(object_filename);
        return 0;
    }
#else
#ifdef __APPLE__
    const char* linker_name = "clang";
#else
    const char* linker_name = "gcc"; // Linux and generic Unix
#endif

#ifdef __APPLE__
    char* link_triple = codegen->target_triple;
    int owns_link_triple = 0;
    if (!link_triple) {
        link_triple = LLVMGetDefaultTargetTriple();
        owns_link_triple = 1;
    }
#endif

    // argv layout: <linker> [-target <triple> | -no-pie] -o <exe> <obj>
    // <archive> [-l<userlib>]* -lm -lpthread NULL. 9 fixed non-NULL slots
    // covers the larger (__APPLE__) branch with room to spare on Linux;
    // + one slot per user lib + the NULL terminator.
    size_t fixed_slots = 9;
    size_t max_argv = fixed_slots + (size_t)codegen->link_lib_count + 1;
    char** argv = malloc(max_argv * sizeof(char*));
    char** lib_flags = codegen->link_lib_count
        ? calloc((size_t)codegen->link_lib_count, sizeof(char*))
        : NULL;
    if (!argv || (codegen->link_lib_count && !lib_flags)) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Memory allocation failed");
        free(argv);
        free(lib_flags);
#ifdef __APPLE__
        if (owns_link_triple) LLVMDisposeMessage(link_triple);
#endif
        remove(object_filename);
        free(object_filename);
        return 0;
    }

    size_t argn = 0;
    argv[argn++] = (char*)linker_name;
#ifdef __APPLE__
    argv[argn++] = "-target";
    argv[argn++] = link_triple;
#else
    // -no-pie: see the ordering/relocation comment preserved below.
    argv[argn++] = "-no-pie";
#endif
    argv[argn++] = "-o";
    argv[argn++] = (char*)filename;
    argv[argn++] = object_filename;
    argv[argn++] = (char*)goo_runtime_archive_path();

    // User-requested libs (-l flags collected by the driver) land AFTER the
    // runtime archive and BEFORE -lm/-lpthread: user libs may depend on
    // nothing of ours, and this preserves the archive-before-defaults
    // ordering the -lm/-lpthread comment below documents (newer binutils
    // enforces it).
    int lib_alloc_failed = 0;
    for (size_t i = 0; i < codegen->link_lib_count; i++) {
        size_t need = strlen("-l") + strlen(codegen->link_libs[i]) + 1;
        lib_flags[i] = malloc(need);
        if (!lib_flags[i]) { lib_alloc_failed = 1; break; }
        snprintf(lib_flags[i], need, "-l%s", codegen->link_libs[i]);
        argv[argn++] = lib_flags[i];
    }
    if (lib_alloc_failed) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Memory allocation failed");
        for (size_t i = 0; i < codegen->link_lib_count; i++) free(lib_flags[i]);
        free(lib_flags);
        free(argv);
#ifdef __APPLE__
        if (owns_link_triple) LLVMDisposeMessage(link_triple);
#endif
        remove(object_filename);
        free(object_filename);
        return 0;
    }

    // -lm/-lpthread must follow the archive (the runtime uses libm and
    // pthreads); newer binutils enforces this ordering and otherwise fails
    // with undefined references.
    argv[argn++] = "-lm";
    argv[argn++] = "-lpthread";
    argv[argn] = NULL;

    pid_t link_pid = fork();
    if (link_pid < 0) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"},
                     "Failed to fork for linking: %s", strerror(errno));
        for (size_t i = 0; i < codegen->link_lib_count; i++) free(lib_flags[i]);
        free(lib_flags);
        free(argv);
#ifdef __APPLE__
        if (owns_link_triple) LLVMDisposeMessage(link_triple);
#endif
        remove(object_filename);
        free(object_filename);
        return 0;
    }

    if (link_pid == 0) {
        // Child: replace this process image with the linker. -no-pie: the
        // LLVM backend emits non-PIC objects (R_X86_64_32S relocs), but
        // distros like Ubuntu default gcc to -pie, which rejects them with
        // "relocation ... can not be used when making a PIE object". execvp
        // only returns on failure (e.g. linker not on PATH).
        execvp(argv[0], argv);
        _exit(127);
    }

    int link_status = 0;
    int link_failed;
    pid_t waited;
    // EINTR retry: a caught signal interrupting waitpid must not be
    // misread as a failed link.
    do {
        waited = waitpid(link_pid, &link_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        link_failed = 1;
    } else {
        link_failed = !(WIFEXITED(link_status) && WEXITSTATUS(link_status) == 0);
    }

    if (link_failed) {
        char* joined = codegen_join_argv(argv);
        codegen_error(codegen, (Position){0, 0, 0, "codegen"},
                     "Linking failed with command: %s", joined ? joined : "(link command)");
        free(joined);
    }

    for (size_t i = 0; i < codegen->link_lib_count; i++) free(lib_flags[i]);
    free(lib_flags);
    free(argv);
#ifdef __APPLE__
    if (owns_link_triple) LLVMDisposeMessage(link_triple);
#endif

    if (link_failed) {
        remove(object_filename);   // don't leave a stray object behind
        free(object_filename);
        return 0;
    }
#endif // _WIN32 / POSIX

    // Clean up object file
    remove(object_filename);
    free(object_filename);

    return 1;
#else
    return 0;
#endif
}

int codegen_optimize(CodeGenerator* codegen, int level) {
#if LLVM_AVAILABLE
    if (!codegen) return 0;
    // O0 is the byte-identical contract: skip entirely, no IR mutation,
    // no target machine built. Every fixture compiled without -O must see
    // exactly the same IR as before this function existed.
    if (level <= 0) return 1;

    // LLVMRunPasses needs a target machine (TTI-aware passes, e.g.
    // vectorization, consult it). codegen_emit_executable builds one
    // lazily using this exact triple/cpu/features recipe, but that runs
    // AFTER codegen_optimize (driver calls this before either emit path)
    // — so codegen->target_machine isn't available yet here. Build a
    // scratch one, use it only for the pass run, and dispose it; don't
    // restructure emit to share state for what is a one-shot compile-time
    // cost.
    char* error_message = NULL;
    char* target_triple = codegen->target_triple;
    int owns_triple = 0;
    if (!target_triple) {
        target_triple = LLVMGetDefaultTargetTriple();
        owns_triple = 1;
    }

    LLVMTargetRef target;
    if (LLVMGetTargetFromTriple(target_triple, &target, &error_message)) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"},
                     "Failed to get target for optimization: %s", error_message);
        LLVMDisposeMessage(error_message);
        if (owns_triple) LLVMDisposeMessage(target_triple);
        return 0;
    }

    const char* cpu = codegen->target_cpu ? codegen->target_cpu : "generic";
    const char* features = codegen->target_features ? codegen->target_features : "";
    // Only O3 asks the backend's own codegen for Aggressive; O1/O2 stay at
    // Default (the new-PM IR pipeline below is what does the real
    // optimization work at those levels — this mirrors the identical
    // mapping applied to the "real" target machine in
    // codegen_emit_executable).
    LLVMCodeGenOptLevel cg_level = (level >= 3) ? LLVMCodeGenLevelAggressive
                                                 : LLVMCodeGenLevelDefault;
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target, target_triple, cpu, features,
        cg_level, LLVMRelocDefault, LLVMCodeModelDefault);
    if (owns_triple) LLVMDisposeMessage(target_triple);

    if (!tm) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"},
                     "Failed to create target machine for optimization");
        return 0;
    }

    // The module has no datalayout yet — codegen_emit_executable is the only
    // other place that sets one, and it now runs AFTER this function. Without
    // it, layout-sensitive folds (e.g. InstCombine collapsing a struct GEP to
    // a raw byte offset) size and place fields using LLVM's generic default
    // alignment (i64 ABI-aligned to 4 bytes) instead of this target's real
    // x86-64 layout (i64 aligned to 8) — the GEP then freezes in the WRONG
    // (smaller) offset for any field after a narrower one (e.g. `int` after
    // `bool`), while the struct's actual in-memory layout at runtime still
    // follows the real target's rules. That mismatch is a silent
    // misaligned-read miscompile, not a missed optimization: confirmed by
    // reading back an unrelated pad byte as the value's low word (observed
    // as `value * 2^32` on affected fixtures). Set it from the same scratch
    // target machine before any pass runs so folds agree with reality.
    LLVMTargetDataRef opt_target_data = LLVMCreateTargetDataLayout(tm);
    char* opt_data_layout = LLVMCopyStringRepOfTargetData(opt_target_data);
    LLVMSetDataLayout(codegen->module, opt_data_layout);
    LLVMDisposeMessage(opt_data_layout);
    LLVMDisposeTargetData(opt_target_data);

    const char* pipeline = "default<O1>";
    if (level == 2) pipeline = "default<O2>";
    else if (level >= 3) pipeline = "default<O3>";

    LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
    LLVMErrorRef err = LLVMRunPasses(codegen->module, pipeline, tm, opts);
    LLVMDisposePassBuilderOptions(opts);
    LLVMDisposeTargetMachine(tm);

    if (err) {
        char* msg = LLVMGetErrorMessage(err);
        codegen_error(codegen, (Position){0, 0, 0, "codegen"},
                     "Optimization passes failed: %s", msg);
        LLVMDisposeErrorMessage(msg);
        return 0;
    }

    return 1;
#else
    (void)codegen;
    (void)level;
    return 1;
#endif
}

int codegen_verify_module(CodeGenerator* codegen __attribute__((unused))) {
#if LLVM_AVAILABLE
    if (!codegen) return 0;
    
    char* error_message = NULL;
    if (LLVMVerifyModule(codegen->module, LLVMReturnStatusAction, &error_message)) {
        fprintf(stderr, "Module verification failed: %s\n", error_message);
        LLVMDisposeMessage(error_message);
        return 0;
    }
    
    return 1;
#else
    return 0;
#endif
}

// Check if target is WebAssembly
int codegen_is_wasm_target(CodeGenerator* codegen) {
    if (!codegen || !codegen->target_triple) return 0;
    return strstr(codegen->target_triple, "wasm32") != NULL;
}

// Generate WebAssembly export attribute
int codegen_add_wasm_export(CodeGenerator* codegen, LLVMValueRef function, const char* export_name) {
    if (!codegen || !function || !export_name) return 0;
    if (!codegen_is_wasm_target(codegen)) return 1; // No-op for non-WASM targets
    
    // Add export attribute to function
    LLVMAttributeRef export_attr = LLVMCreateStringAttribute(codegen->context, 
                                                            "wasm-export-name", 16,
                                                            export_name, strlen(export_name));
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, export_attr);
    
    // Make function externally visible
    LLVMSetLinkage(function, LLVMExternalLinkage);
    
    return 1;
}

// Generate WebAssembly import attribute  
int codegen_add_wasm_import(CodeGenerator* codegen, LLVMValueRef function, 
                           const char* module_name, const char* import_name) {
    if (!codegen || !function || !module_name || !import_name) return 0;
    if (!codegen_is_wasm_target(codegen)) return 1; // No-op for non-WASM targets
    
    // Add import module attribute
    LLVMAttributeRef module_attr = LLVMCreateStringAttribute(codegen->context,
                                                           "wasm-import-module", 18,
                                                           module_name, strlen(module_name));
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, module_attr);
    
    // Add import name attribute
    LLVMAttributeRef name_attr = LLVMCreateStringAttribute(codegen->context,
                                                         "wasm-import-name", 16,
                                                         import_name, strlen(import_name));
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, name_attr);
    
    return 1;
}

// Generate WASM memory management functions
int codegen_declare_wasm_runtime_functions(CodeGenerator* codegen) {
    if (!codegen || !codegen_is_wasm_target(codegen)) return 1;
    
    // WebAssembly memory.grow and memory.size intrinsics
    LLVMTypeRef i32_type = LLVMInt32TypeInContext(codegen->context);
    
    // i32 @llvm.wasm.memory.grow.i32(i32 %memory_index, i32 %delta)
    LLVMTypeRef grow_params[] = { i32_type, i32_type };
    LLVMTypeRef grow_type = LLVMFunctionType(i32_type, grow_params, 2, 0);
    LLVMValueRef grow_func = LLVMAddFunction(codegen->module, "llvm.wasm.memory.grow.i32", grow_type);
    
    // i32 @llvm.wasm.memory.size.i32(i32 %memory_index)
    LLVMTypeRef size_params[] = { i32_type };
    LLVMTypeRef size_type = LLVMFunctionType(i32_type, size_params, 1, 0);
    LLVMValueRef size_func = LLVMAddFunction(codegen->module, "llvm.wasm.memory.size.i32", size_type);
    
    return 1;
}

// Configure concurrency for WebAssembly environment
#if LLVM_AVAILABLE
int codegen_configure_wasm_concurrency(CodeGenerator* codegen) {
    if (!codegen || !codegen_is_wasm_target(codegen)) return 1;
    
    // For single-threaded WASM, transform goroutines to async/await
    // This will be used during function generation to emit different code
    // for go statements in WASM vs native targets
    
    // Add JavaScript async/await helper imports
    LLVMTypeRef void_type = LLVMVoidTypeInContext(codegen->context);
    LLVMTypeRef ptr_type = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
    
    // Import JavaScript Promise handling functions
    LLVMTypeRef promise_params[] = { ptr_type };
    LLVMTypeRef promise_type = LLVMFunctionType(ptr_type, promise_params, 1, 0);
    LLVMValueRef create_promise = LLVMAddFunction(codegen->module, "js_create_promise", promise_type);
    LLVMValueRef resolve_promise = LLVMAddFunction(codegen->module, "js_resolve_promise", promise_type);
    
    // Add import attributes
    codegen_add_wasm_import(codegen, create_promise, "js", "create_promise");
    codegen_add_wasm_import(codegen, resolve_promise, "js", "resolve_promise");

    return 1;
}
#endif

// Coerce a VALUE to the target LLVM type using the source type's
// signedness — the single home for the width-coercion rule that was
// previously inlined (and repeatedly re-broken) at the var-decl,
// literal-element, append, and channel-send sites:
//   int -> int      : SExt/ZExt by src_signed when widening, Trunc when narrowing
//   int -> float    : SIToFP/UIToFP by src_signed
//   float -> float  : FPExt widening, FPTrunc narrowing
// Anything else (matching types, aggregates, pointers) returns v unchanged.
// REQUIRES a positioned builder — callers on constant/global paths must keep
// their LLVMConstInt/LLVMConstReal rebuilds instead.
#if LLVM_AVAILABLE
LLVMValueRef codegen_coerce_to_type(CodeGenerator* codegen, LLVMValueRef v,
                                    int src_signed, LLVMTypeRef to) {
    LLVMTypeRef from = LLVMTypeOf(v);
    if (from == to) return v;
    LLVMTypeKind fk = LLVMGetTypeKind(from), tk = LLVMGetTypeKind(to);
    int f_is_fp = (fk == LLVMFloatTypeKind || fk == LLVMDoubleTypeKind);
    int t_is_fp = (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind);
    if (fk == LLVMIntegerTypeKind && tk == LLVMIntegerTypeKind) {
        unsigned fb = LLVMGetIntTypeWidth(from), tb = LLVMGetIntTypeWidth(to);
        if (fb < tb) return src_signed
            ? LLVMBuildSExt(codegen->builder, v, to, "coerce_sext")
            : LLVMBuildZExt(codegen->builder, v, to, "coerce_zext");
        if (fb > tb) return LLVMBuildTrunc(codegen->builder, v, to, "coerce_trunc");
        return v;
    }
    if (fk == LLVMIntegerTypeKind && t_is_fp) return src_signed
        ? LLVMBuildSIToFP(codegen->builder, v, to, "coerce_sitofp")
        : LLVMBuildUIToFP(codegen->builder, v, to, "coerce_uitofp");
    if (f_is_fp && t_is_fp) {
        // float32<->float64 only; kind order: Float < Double.
        if (fk == LLVMFloatTypeKind && tk == LLVMDoubleTypeKind)
            return LLVMBuildFPExt(codegen->builder, v, to, "coerce_fpext");
        if (fk == LLVMDoubleTypeKind && tk == LLVMFloatTypeKind)
            return LLVMBuildFPTrunc(codegen->builder, v, to, "coerce_fptrunc");
    }
    return v;
}
#endif
