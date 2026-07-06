#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include "types.h"
#include "runtime.h"
#include <stddef.h>

// LLVM C API includes (only if LLVM is available)
#ifdef __has_include
#if __has_include(<llvm-c/Core.h>)
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#define LLVM_AVAILABLE 1
#else
#define LLVM_AVAILABLE 0
#endif
#else
// Fallback for older compilers
#define LLVM_AVAILABLE 0
#endif

// Forward declarations
typedef struct CodeGenerator CodeGenerator;
typedef struct FunctionInfo FunctionInfo;
typedef struct ValueInfo ValueInfo;

#if LLVM_AVAILABLE
// Deferred global initializer (Task 2 / var-init cluster): a package-level
// `var y = x` (or any initializer needing an identifier, call, or other
// non-constant-foldable expression) cannot be evaluated at true module scope
// — codegen_generate_expression may issue LLVMBuildXxx calls that require a
// positioned builder, which module scope does not have (the SIGSEGV this
// task fixes). Such initializers are queued here during
// codegen_generate_var_decl's module-scope pass and evaluated, in
// declaration order, inside a synthesized goo.global_init() function called
// as user main's first instruction. See codegen_generate_global_init_function
// (function_codegen.c). `global`/`expr`/`declared_type` are all borrowed
// (owned by the module / AST / type system respectively) — the array itself
// owns no memory beyond its own storage.
typedef struct {
    LLVMValueRef global;   // already-created global, zero/nil-initialized
    struct ASTNode* expr;  // initializer expression to evaluate later
    Type* declared_type;   // the var's declared/inferred Goo type
    Position pos;          // for diagnostics
} DeferredGlobalInit;

// LLVM-based code generator
struct CodeGenerator {
    LLVMContextRef context;
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMTargetMachineRef target_machine;
    
    // Current function being generated
    LLVMValueRef current_function;
    FunctionInfo* current_function_info;
    
    // Symbol tables
    ValueInfo** value_table;     // Maps variable names to LLVM values
    size_t value_table_size;
    size_t value_table_capacity;
    // High-water mark captured on codegen_enter_function. Used by
    // codegen_exit_function to truncate the value table back to its
    // pre-function size so per-function locals don't leak across
    // function boundaries and produce "Referring to an instruction
    // in another function" verifier errors on later lookups.
    size_t value_table_function_start;
    
    // Type cache for LLVM types
    LLVMTypeRef* type_cache;
    size_t type_cache_size;
    size_t type_cache_capacity;

    // Struct-type cache: maps Goo Type* -> LLVMTypeRef for named struct
    // types. Pre-populated with an opaque struct before resolving fields so
    // that recursive pointer fields (`next *Node`) can reference the opaque
    // type without infinite recursion in codegen_get_struct_type.
    const Type** struct_cache_keys;
    LLVMTypeRef* struct_cache_vals;
    size_t struct_cache_size;
    size_t struct_cache_cap;

    // Loop-context stack for break/continue targets (depth-bounded; nesting
    // deeper than 32 is rejected with a codegen error).
    LLVMBasicBlockRef loop_break_bb[32];
    LLVMBasicBlockRef loop_continue_bb[32];
    int loop_depth;

    // Error reporting
    char* current_file;
    int error_count;
    int warning_count;
    
    // Target information
    char* target_triple;
    char* target_cpu;
    char* target_features;
    
    // WebAssembly-specific configuration
    int wasm_configured;
    int is_wasm_target;

    // Deferred global initializers awaiting evaluation in goo.global_init()
    // (Task 2 / var-init cluster). Growable array; see DeferredGlobalInit.
    DeferredGlobalInit* deferred_global_inits;
    size_t deferred_global_init_count;
    size_t deferred_global_init_capacity;

    // Closures Branch B, Task 1: module-unique counter for func-literal LLVM
    // symbol names (`__goo_lit_<n>`, codegen_generate_func_lit in
    // function_codegen.c). Tail-appended per the no-header-deps convention
    // (ast.h's M10 comment) — the Makefile lacks header dependencies, so a
    // mid-struct insertion would shift every field after it and silently
    // miscompile any translation unit rebuilt without `make clean`.
    // codegen_new (codegen.c) is outside this task's file allowlist and so
    // does not explicitly zero this field; that is safe (not a bug) because
    // `unsigned long` has no trap representation (an uninitialized read
    // yields some unspecified-but-valid value, never undefined behavior) and
    // codegen_generate_func_lit only ever POST-INCREMENTs it, so every
    // literal in one compilation still gets a distinct suffix regardless of
    // the field's starting value — merely not guaranteed to start at 0.
    // Recorded as a follow-up: an explicit `codegen->func_lit_counter = 0;`
    // in codegen_new would make generated names deterministic across runs.
    unsigned long func_lit_counter;

    // Struct-typed map keys (Task 2): cache of synthesized per-field
    // equality comparators, Goo Type* -> the `goo.structeq.<id>` LLVM
    // function value. Keyed by TYPE IDENTITY (pointer equality), exactly
    // like struct_cache_keys/vals above — NOT by name, so two independent
    // anonymous struct types (no `struct_type.name`) each get their own
    // comparator instead of colliding. structeq_counter hands out the
    // unique `<id>` suffix for each newly-synthesized comparator's LLVM
    // symbol name; see codegen_get_or_emit_struct_key_eq (codegen.c).
    const Type** structeq_cache_keys;
    LLVMValueRef* structeq_cache_vals;
    size_t structeq_cache_size;
    size_t structeq_cache_cap;
    unsigned long structeq_counter;

    // Interface-typed map keys, Task 1 (vtable ABI shift): cache of
    // synthesized per-concrete-type value-equality comparators emitted at
    // interface vtable slot 0 (`goo.typeeq.<id>`), Goo Type* -> LLVM function
    // value. Keyed by TYPE IDENTITY, exactly like structeq_cache_keys/vals
    // above (NOT used for TYPE_STRUCT concretes — those delegate straight to
    // structeq_cache_keys/vals; see codegen_get_or_emit_type_eq, codegen.c).
    const Type** typeeq_cache_keys;
    LLVMValueRef* typeeq_cache_vals;
    size_t typeeq_cache_size;
    size_t typeeq_cache_cap;
    unsigned long typeeq_counter;
    // Single shared panic-stub eq (`goo.uncmpeq`) for uncomparable dynamic
    // concrete types (slice/map/func) boxed into an interface; emitted at
    // most once per module. NULL until first requested.
    LLVMValueRef uncmpeq_fn;
};

// Function information for code generation
struct FunctionInfo {
    char* name;
    LLVMValueRef function;
    LLVMTypeRef function_type;
    Type* goo_type;
    
    // Basic blocks
    LLVMBasicBlockRef entry_block;
    LLVMBasicBlockRef exit_block;
    LLVMValueRef return_value;  // Alloca for return value
    
    // Local variables
    ValueInfo** locals;
    size_t local_count;
    size_t local_capacity;

    // Named return parameters (P3-5). When the function declares
    // `(x int, y int)` results, these hold the result names in field
    // order; a bare `return` loads each named-result local and builds the
    // aggregate return value from them. NULL / 0 for ordinary functions.
    char** named_result_names;
    size_t named_result_count;

    // Deferred calls (P3-4). Each `defer <call>` pushes its call AST node
    // here in source order; at every function-exit path the calls are
    // emitted in reverse (LIFO) order immediately before the `ret`. MVP:
    // arguments are evaluated at exit time, which matches Go's defer-time
    // evaluation for the literal/simple-arg cases the probe covers.
    ASTNode** deferred_calls;
    size_t deferred_count;
    size_t deferred_capacity;
};

// Value information for variables and expressions
struct ValueInfo {
    char* name;
    LLVMValueRef llvm_value;
    Type* goo_type;
    int is_lvalue;          // Can be assigned to
    int is_moved;           // For ownership tracking
    int is_initialized;     // For null safety
};

#else
// Stub implementation when LLVM is not available
struct CodeGenerator {
    char* error_message;
    int llvm_unavailable;
    
    // Error reporting (needed for compatibility)
    char* current_file;
    int error_count;
    int warning_count;
};

struct FunctionInfo {
    char* name;
};

struct ValueInfo {
    char* name;
};
#endif

// Code generator creation and destruction
CodeGenerator* codegen_new(const char* module_name);
void codegen_free(CodeGenerator* codegen);

// Target configuration
int codegen_set_target(CodeGenerator* codegen, const char* triple, const char* cpu, const char* features);
int codegen_initialize_target(CodeGenerator* codegen);

// stdlib Phase 0 (Task 4): cross-package symbol mangling. Returns a malloc'd
// `goo_pkg__<pkg>__<base>` when codegenning a non-main package, or NULL for the
// main package (callers keep the bare `base`). Single source of truth shared by
// the plain-function and error-union codegen paths. Caller frees the result.
char* codegen_package_symbol_name(TypeChecker* checker, const char* base);

// Code generation entry points
int codegen_generate_program(CodeGenerator* codegen, TypeChecker* checker, ASTNode* program);
int codegen_generate_declaration(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl);
int codegen_generate_statement(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
ValueInfo* codegen_generate_expression(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

// Declaration generation
// Forward-reference pre-pass: declare every plain function's LLVM prototype in
// the module before any body is emitted, so a call to a function defined later
// resolves. Mirrors the type checker's hoist_function_signatures. Call once,
// before the body-emitting declaration loop. Returns 1 on success, 0 on failure.
int codegen_predeclare_functions(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decls);
int codegen_generate_function_decl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl);
int codegen_generate_var_decl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl);
// Task 2 / var-init cluster: does `decls` (a program's top-level declaration
// list) contain any package-level `var` whose initializer will be deferred
// to goo.global_init() (see codegen_generate_var_decl's module-scope path)?
// codegen_generate_program calls this BEFORE generating any function body,
// so goo.global_init's prototype can be pre-created — a call to it inserted
// into `main`'s prologue then resolves via LLVMGetNamedFunction regardless
// of main's position in source order relative to the deferred var(s).
int codegen_program_needs_global_init(ASTNode* decls);
// Synthesize (fill in the body of the already-prototyped) goo.global_init()
// from the deferred global-initializer list collected while generating every
// top-level declaration. No-op (returns 1) if codegen_program_needs_global_init
// found nothing to defer — the prototype was never created, so there is
// nothing to fill in. Must be called once, after every top-level declaration
// has been generated (codegen_generate_program) so every global and every
// function this initializer might reference already exists in the module.
int codegen_generate_global_init_function(CodeGenerator* codegen, TypeChecker* checker);
int codegen_generate_multi_assign(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
// Address of an assignable lvalue (identifier/field/index), unloaded.
ValueInfo* codegen_emit_lvalue_address(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
int codegen_generate_const_decl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl);

// Statement generation
int codegen_generate_block_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_expr_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_if_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_for_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_return_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_go_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_defer_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
// Emit the current function's registered defers in LIFO order before a `ret`.
void codegen_emit_deferred_calls(CodeGenerator* codegen, TypeChecker* checker);
int codegen_generate_select_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_switch_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
// Type assertions branch, Task 3: `switch [v :=] x.(type) { case … }`.
int codegen_generate_type_switch_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_unsafe_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_asm_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);

// Select statement helper functions
#if LLVM_AVAILABLE
LLVMTypeRef codegen_get_select_case_type(CodeGenerator* codegen);
LLVMValueRef codegen_get_select_function(CodeGenerator* codegen);
int codegen_setup_select_case(CodeGenerator* codegen, TypeChecker* checker, 
                              LLVMValueRef cases_array, size_t case_index, 
                              SelectCaseNode* select_case);
#endif

// Expression generation
ValueInfo* codegen_generate_identifier(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
#if LLVM_AVAILABLE
// Resolve a bare identifier to its module-level (non-local) function global,
// if any — an intra-package symbol goo_pkg__<pkg>__<name> first, then the
// bare name. Shared by codegen_generate_identifier's value-position fallback
// (expression_codegen.c, which wraps the result into the universal
// fat-pointer VALUE) and codegen_resolve_callee's direct-call bypass
// (call_codegen.c), which need the identical lookup but return it BARE.
LLVMValueRef codegen_lookup_global_function(CodeGenerator* codegen, TypeChecker* checker,
                                            const char* name);
#endif
ValueInfo* codegen_generate_literal(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
// Build a *constant* goo_string { i8* data, i64 len } value from `len` raw bytes
// (embedded NULs preserved). Builder-free, so it is valid at global scope — used
// by both string-literal codegen and folded const-string tables. The bytes are
// copied into a private unnamed_addr global constant array.
LLVMValueRef codegen_const_string_value(CodeGenerator* codegen, const char* bytes, size_t len);
ValueInfo* codegen_generate_binary_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_unary_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_call_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
// Resolve a call/go-statement's CALLEE expression. A bare identifier NOT
// shadowed by a local variable/parameter resolves to the BARE LLVM global
// function (the direct-call fast path — unconditionally unchanged from
// before the universal fat-pointer migration). Any other callee expression
// evaluates through the ordinary expression path and may yield the
// universal `{ fn_ptr, env_ptr }` pair. Used by codegen_generate_call_expr
// (call_codegen.c) and codegen_generate_go_stmt (statement_codegen.c) — see
// codegen_resolve_callee's definition for why the go-statement use is
// required, not optional.
ValueInfo* codegen_resolve_callee(CodeGenerator* codegen, TypeChecker* checker, ASTNode* callee_expr);
ValueInfo* codegen_generate_index_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
// Widen an integer index value to a signed-correct i64 offset (zero-extend for
// unsigned index types, sign-extend for signed). Prevents a narrow unsigned
// index (e.g. uint8 255) from sign-extending to -1 in an element GEP. Used by
// both the index read path and the index-assignment lvalue path.
LLVMValueRef codegen_widen_index(CodeGenerator* codegen, ValueInfo* idx);
// Emit a goo_bounds_check(index, length, file, line) call. The runtime fn
// panics if index >= length (negative indices SExt to a huge size_t and so
// also fail), aborting before any out-of-range read/write. The bounds test is
// inside the runtime fn, so no IR branching is emitted here. Internally
// re-widens `index` via codegen_widen_index-equivalent logic, so passing an
// already-i64 value is a safe no-op pass-through. Shared by every codegen
// site that needs a bounds-checked index (composite_codegen.c's index-read
// path and expression_codegen.c's slice-write lvalue path).
void codegen_emit_bounds_check(CodeGenerator* codegen, LLVMValueRef index,
                               LLVMValueRef length, ASTNode* expr);
// Coerce a VALUE to the target LLVM type using the source type's
// signedness — the single home for the width-coercion rule that was
// previously inlined (and repeatedly re-broken) at the var-decl,
// literal-element, append, and channel-send sites:
//   int -> int      : SExt/ZExt by src_signed when widening, Trunc when narrowing
//   int -> float    : SIToFP/UIToFP by src_signed
//   float -> float  : FPExt widening, FPTrunc narrowing
// Anything else (matching types, aggregates, pointers) returns v unchanged.
// REQUIRES a positioned builder — only valid on local/function codegen paths.
// Constant/global paths (e.g. folded literals) must keep their
// LLVMConstInt/LLVMConstReal rebuilds instead of calling this.
LLVMValueRef codegen_coerce_to_type(CodeGenerator* codegen, LLVMValueRef v,
                                    int src_signed, LLVMTypeRef to);
ValueInfo* codegen_generate_slice_index_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_selector_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_struct_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_array_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_slice_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_match(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
// Closures Branch B, Task 1: emit a func literal (function_codegen.c, beside
// codegen_get_func_thunk) as its own `__goo_lit_<n>` LLVM function and
// return the universal fat-pointer VALUE `{__goo_lit_n, NULL}`. See its
// definition for the full ambient-state save/restore discipline this
// mid-expression emission requires.
ValueInfo* codegen_generate_func_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
// Shared slice-construction core behind codegen_generate_slice_lit (`[]T{...}`)
// and the struct-literal named-slice path: build a slice struct
// { ptr, i64 len, i64 cap } from a next-chained ASTNode* element list and a
// resolved TYPE_SLICE type. Exposed (non-static) for Task 2's variadic
// call-site packing (call_codegen.c), which reuses this same lowering for the
// trailing-args-into-a-slice construction instead of duplicating it —
// `first_elem` there is the first trailing ARGUMENT node, not a literal's
// element list, but the shape (next-chained ASTNode*, targets slice_type) is
// identical.
ValueInfo* codegen_build_slice_from_elems(CodeGenerator* codegen, TypeChecker* checker,
                                          ASTNode* first_elem, Type* slice_type, Position pos);

// Interface codegen (P4-5): vtable construction, boxing, dynamic dispatch.
// `pointer_form` (Task 5): 0 builds/reuses the value-form global
// `goo.vtable.<concrete>.<iface>` (thunks against `concrete` directly); 1
// builds/reuses the DISTINCTLY-NAMED pointer-form global
// `goo.vtable.$ptr$<concrete>.<iface>` — same thunk slots (built against the
// same `concrete`, since a pointer box's `data` also points at a `concrete`),
// but a different global so a pointer-boxed `*T` and a value-boxed `T` no
// longer alias the same vtable address. See interface_codegen.c's callers
// (codegen_interface_box's two branches, codegen_interface_assert_match) for
// which form each site must request.
// Per-concrete-type descriptor reached behind interface vtable slot 0 (Go's
// itab->_type shape): a private-constant global `goo.typedesc.<T>` (or
// `goo.typedesc.$ptr$<T>` for pointer_form), layout { ptr eq_fn, ptr
// type_name, ptr fmt_fn }. eq_fn is field 0 so goo_iface_key_eq's slot-0 hop
// is a single extra deref; type_name is a C string (e.g. "int", "*Point");
// fmt_fn is null until a later task fills it. Name-deduped by concrete type,
// like the vtable globals — see interface_codegen.c.
LLVMValueRef codegen_get_or_emit_type_desc(CodeGenerator* codegen, TypeChecker* checker,
                                           Type* concrete, int pointer_form);
LLVMValueRef codegen_interface_vtable(CodeGenerator* codegen, TypeChecker* checker,
                                      Type* iface, Type* concrete, int pointer_form);
LLVMValueRef codegen_interface_box(CodeGenerator* codegen, TypeChecker* checker,
                                   Type* iface, Type* concrete, LLVMValueRef value);
ValueInfo* codegen_interface_dispatch(CodeGenerator* codegen, TypeChecker* checker,
                                      LLVMValueRef iface_val, Type* iface_type,
                                      const char* method_name,
                                      LLVMValueRef* args, size_t argc);
// Task 2 (type assertions): shared vtable-pointer-compare + unbox lowering
// for `x.(T)` (comma-ok and single-return) and Task 3's type switch. See
// interface_codegen.c's doc comments on each for the exact contract.
LLVMValueRef codegen_interface_assert_match(CodeGenerator* codegen, TypeChecker* checker,
                                            LLVMValueRef iface_val, Type* iface_type,
                                            Type* target, LLVMValueRef* data_out);
LLVMValueRef codegen_interface_assert_unbox(CodeGenerator* codegen, Type* target,
                                            LLVMValueRef data);

// Goo extension expression generation
ValueInfo* codegen_generate_try_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_catch_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_channel_send(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_channel_recv(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

// Unsafe operation expression generation
ValueInfo* codegen_generate_ptr_arithmetic(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_ptr_deref(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_addr_of(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_port_io(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_mmio_access(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

#if LLVM_AVAILABLE
// Type mapping functions
LLVMTypeRef codegen_type_to_llvm(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_basic_type(CodeGenerator* codegen, TypeKind kind);
LLVMTypeRef codegen_get_array_type(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_struct_type(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_function_type(CodeGenerator* codegen, const Type* type);
// Universal function-VALUE representation: the `{ ptr fn, ptr env }` pair
// (see the header comment at its definition, type_mapping.c) and the
// env-prepended LLVM function type used to call through it.
LLVMTypeRef codegen_get_funcval_pair_type(CodeGenerator* codegen);
LLVMTypeRef codegen_get_funcval_call_type(CodeGenerator* codegen, const Type* fn_type);
LLVMTypeRef codegen_get_pointer_type(CodeGenerator* codegen, const Type* type);

// Special Goo type mappings
LLVMTypeRef codegen_get_enum_type(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_error_union_type(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_nullable_type(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_channel_type(CodeGenerator* codegen, const Type* type);

// Value management
ValueInfo* value_info_new(const char* name, LLVMValueRef llvm_value, Type* goo_type);
void value_info_free(ValueInfo* info);
ValueInfo* codegen_lookup_value(CodeGenerator* codegen, const char* name);
int codegen_add_value(CodeGenerator* codegen, ValueInfo* info);

// Function management
FunctionInfo* function_info_new(const char* name, LLVMValueRef function, Type* goo_type);
void function_info_free(FunctionInfo* info);
int codegen_enter_function(CodeGenerator* codegen, FunctionInfo* func_info);
void codegen_exit_function(CodeGenerator* codegen);

// Helper functions
LLVMValueRef codegen_create_alloca(CodeGenerator* codegen, LLVMTypeRef type, const char* name);
LLVMValueRef codegen_create_entry_alloca(CodeGenerator* codegen, LLVMTypeRef type, const char* name);
LLVMValueRef codegen_alloc_local(CodeGenerator* codegen, LLVMTypeRef type, const char* name);
// Get-or-create the value-thunk for named function `name` (Goo type
// `fn_type`, LLVM global `named_fn`): `<name>.__thunk(env, params...) =
// named_fn(params...)`. See its definition (function_codegen.c) for the
// env-FIRST ABI contract and the #30/interface_codegen.c precedents mirrored.
LLVMValueRef codegen_get_func_thunk(CodeGenerator* codegen, TypeChecker* checker,
                                    Type* fn_type, LLVMValueRef named_fn,
                                    const char* name);

// Map values ride an 8-byte runtime slot (i64). Convert a value of the
// declared map value-type V to the slot (ptrtoint / zext-or-trunc) and back
// (inttoptr / trunc-or-ext). The type checker guarantees V fits the slot
// (integer/bool/char/pointer).
LLVMValueRef codegen_map_value_to_slot(CodeGenerator* codegen, LLVMValueRef value, Type* value_type);
LLVMValueRef codegen_map_slot_to_value(CodeGenerator* codegen, LLVMValueRef slot, Type* value_type);
int codegen_map_value_is_inline(Type* value_type);

// Map KEYS ride the same i64 slot, but pack/unpack differently from values:
// a string key must extract-and-PtrToInt its char* directly rather than
// going through codegen_map_value_to_slot, which heap-boxes strings (that
// would make each key a distinct box address and break the runtime's
// strcmp-based key identity). codegen_map_key_kind feeds goo_map_new_sv's
// key_kind argument at map-creation sites (GOO_MAPKEY_STRING=0/INLINE=1,
// include/runtime.h).
// `key_val` is the ValueInfo produced by generating the key expression, NOT
// a bare LLVMValueRef: an lvalue key (a[i], p.X, s[i] — index/selector reads
// always return is_lvalue=1 with llvm_value as the element/field ADDRESS,
// composite_codegen.c) must be loaded to its rvalue before packing, exactly
// like every value/RHS site loads its ValueInfo before slot-boxing. Passing
// the address straight through used to sext/zext a `ptr` (verifier failure)
// or, for string keys, ExtractValue a raw address (SIGSEGV). Threading the
// ValueInfo lets this one helper do that load for all 5 call sites.
// `key_type` may be NULL — falls back to key_val->goo_type.
int codegen_map_key_kind(Type* key_type);
// Interface-typed map keys (Task 2): box a concrete key into `key_type` when
// (and only when) key_type is TYPE_INTERFACE and the key expression's own
// type isn't already that interface — call this BEFORE codegen_map_key_to_
// slot at every call site that packs a user-supplied key (assignment,
// plain/comma-ok read, delete, RMW, map literal entries). See the doc
// comment at its definition (codegen.c) for why this can't be folded into
// codegen_map_key_to_slot itself. Mutates `key_val` in place on success;
// returns 1 (success, including every no-op case) or 0 (failure, already
// reported via codegen_error at `pos`).
int codegen_box_map_key_if_needed(CodeGenerator* codegen, TypeChecker* checker,
                                  ValueInfo* key_val, Type* key_type, Position pos);
LLVMValueRef codegen_map_key_to_slot(CodeGenerator* codegen, TypeChecker* checker,
                                     ValueInfo* key_val, Type* key_type);
LLVMValueRef codegen_map_slot_to_key(CodeGenerator* codegen, LLVMValueRef slot, Type* key_type);
// Struct-typed map keys (Task 2): get (or emit, on first request) the
// synthesized per-field equality comparator for `struct_type` — an LLVM
// function `i32 @goo.structeq.<id>(i64 a, i64 b)` that casts both i64 slots
// to `struct_type*` and compares every DECLARED field (int/bool/char/pointer
// via icmp eq, float32/float64 via fcmp oeq, string via strcmp on the char*
// field, nested struct fields via a recursive call to THAT type's
// comparator), returning 1 iff every field matches, else 0. Cached by struct
// Type* identity (see CodeGenerator's structeq_cache_keys/vals) so it is
// emitted exactly once per distinct struct type no matter how many map
// creation sites request it. Pass the result (bit-cast to the opaque `ptr`
// param type) as goo_map_new_sv's key_eq argument when the map's key type is
// a struct; NULL for every other key kind. Returns NULL on failure (e.g.
// `struct_type` isn't TYPE_STRUCT, or a field's LLVM type can't be resolved).
LLVMValueRef codegen_get_or_emit_struct_key_eq(CodeGenerator* codegen, TypeChecker* checker,
                                               Type* struct_type);
// Interface-typed map keys, Task 1 (vtable ABI shift): get (or synthesize,
// on first request) the per-concrete-type VALUE-equality comparator emitted
// at interface vtable slot 0 (codegen_interface_vtable, interface_codegen.c)
// — an LLVM function `i32 @goo.typeeq.<id>(i64 a, i64 b)` (or the shared
// `i32 @goo.uncmpeq(i64,i64)` panic stub for uncomparable dynamic types).
// `a`/`b` are the two interface `data` words for the SAME concrete type (the
// runtime only calls a vtable-slot-0 eq after a vtable-pointer match, so both
// data words are guaranteed to be that one concrete's boxed representation):
//   - TYPE_POINTER (aliased boxing: data IS the pointer) -> icmp eq the two
//     i64 words directly.
//   - integer/bool/char -> inttoptr each word to T*, load, icmp eq.
//   - float32/float64 -> same shape, fcmp oeq.
//   - string -> inttoptr each word to the `{i8*,i64}` aggregate, load,
//     extractvalue the char* (field 0), strcmp == 0.
//   - struct -> delegates directly to codegen_get_or_emit_struct_key_eq
//     (#129) — its ptr-to-heap-copy signature already matches; NOT cached
//     here (structeq_cache_keys/vals already memoizes it).
//   - slice/map/func (uncomparable) -> the single shared `goo.uncmpeq`
//     stub: calls `goo_panic("comparing uncomparable map key type")` and is
//     `unreachable` (Go-faithful — Go panics comparing an uncomparable
//     dynamic value). Any other concrete kind not yet reachable as an
//     interface concrete (TYPE_ARRAY, TYPE_ENUM, TYPE_CHANNEL, ...) falls
//     back to the same stub rather than miscompiling.
// Cached by concrete Type* IDENTITY, mirroring codegen_get_or_emit_struct_
// key_eq's cache. Saves/restores the caller's builder insert position.
// Returns NULL on failure (concrete's LLVM type can't be resolved, or
// `goo_panic` isn't declared in the module yet).
LLVMValueRef codegen_get_or_emit_type_eq(CodeGenerator* codegen, TypeChecker* checker,
                                         Type* concrete);
// Wrap a raw `char*` into the goo string aggregate `{i8*, i64}` via
// goo_string_new. Shared by codegen_map_slot_to_key's STRING arm and the
// map-range loop's key bind (statement_codegen.c).
LLVMValueRef codegen_string_from_cstr(CodeGenerator* codegen, LLVMValueRef cptr);
LLVMBasicBlockRef codegen_create_block(CodeGenerator* codegen, const char* name);
void codegen_set_insert_point(CodeGenerator* codegen, LLVMBasicBlockRef block);

// Conversion and casting
LLVMValueRef codegen_convert_value(CodeGenerator* codegen, LLVMValueRef value, 
                                 LLVMTypeRef from_type, LLVMTypeRef to_type);
int codegen_types_compatible(LLVMTypeRef from, LLVMTypeRef to);

// Error union helpers
LLVMValueRef codegen_create_error_union_value(CodeGenerator* codegen, LLVMTypeRef union_type,
                                            LLVMValueRef value, int is_error);
LLVMValueRef codegen_extract_error_union_value(CodeGenerator* codegen, LLVMValueRef union_value, int get_error);
LLVMValueRef codegen_check_error_union(CodeGenerator* codegen, LLVMValueRef union_value);

// Nullable type helpers
LLVMValueRef codegen_create_nullable_value(CodeGenerator* codegen, LLVMTypeRef nullable_type,
                                         LLVMValueRef value, int is_null);
LLVMValueRef codegen_extract_nullable_value(CodeGenerator* codegen, LLVMValueRef nullable_value);
LLVMValueRef codegen_check_nullable_null(CodeGenerator* codegen, LLVMValueRef nullable_value);

// Nullable codegen — used across expression_codegen.c, function_codegen.c,
// and call_codegen.c (default-nil locals/globals, reassignment, ?T arg wrap).
ValueInfo* codegen_generate_null_literal(CodeGenerator* codegen, TypeChecker* checker, Type* expected_type);
LLVMValueRef codegen_create_nullable_null(CodeGenerator* codegen, LLVMTypeRef nullable_type, Type* base_type);
LLVMValueRef codegen_create_nullable_with_value(CodeGenerator* codegen, LLVMTypeRef nullable_type,
                                               LLVMValueRef value, Type* value_type);
int codegen_generate_nullable_assignment(CodeGenerator* codegen, TypeChecker* checker,
                                        LLVMValueRef nullable_target, LLVMValueRef source_value,
                                        Type* target_type, Type* source_type, Position pos);

// Error union helpers
LLVMValueRef codegen_create_error_union_success(CodeGenerator* codegen, LLVMTypeRef union_type, 
                                               LLVMValueRef value, Type* value_type);
LLVMValueRef codegen_create_error_union_error(CodeGenerator* codegen, LLVMTypeRef union_type, 
                                             LLVMValueRef error_value);
LLVMValueRef codegen_error_union_is_error(CodeGenerator* codegen, LLVMValueRef error_union);
LLVMValueRef codegen_error_union_get_value(CodeGenerator* codegen, LLVMValueRef error_union);
LLVMValueRef codegen_error_union_get_error(CodeGenerator* codegen, LLVMValueRef error_union);

// Runtime function declarations
#if LLVM_AVAILABLE
LLVMValueRef codegen_declare_runtime_functions(CodeGenerator* codegen);
LLVMValueRef codegen_get_runtime_function(CodeGenerator* codegen, const char* name);
LLVMValueRef codegen_call_runtime_function(CodeGenerator* codegen, const char* name, 
                                          LLVMValueRef* args, unsigned arg_count);
#else
int codegen_declare_runtime_functions(CodeGenerator* codegen);
int codegen_get_runtime_function(CodeGenerator* codegen, const char* name);
int codegen_call_runtime_function(CodeGenerator* codegen, const char* name, 
                                  void* args, unsigned arg_count);
#endif

// Channel operation helpers
LLVMValueRef codegen_generate_channel_send_call(CodeGenerator* codegen, LLVMValueRef channel, LLVMValueRef value);
LLVMValueRef codegen_generate_channel_recv_call(CodeGenerator* codegen, LLVMValueRef channel);
ValueInfo* codegen_generate_make_chan_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

// Built-in function helpers
ValueInfo* codegen_generate_println_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_print_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

// Error return helper - works with LLVM types
LLVMValueRef codegen_generate_error_return(CodeGenerator* codegen, LLVMValueRef return_value, 
                                         Type* return_type, Type* function_return_type);

#else

// Stub version for when LLVM is not available
void* codegen_generate_error_return_stub(CodeGenerator* codegen, void* return_value, 
                                        Type* return_type, Type* function_return_type);

// Map the function name to the stub
#define codegen_generate_error_return codegen_generate_error_return_stub

#endif

// Output generation
int codegen_emit_llvm_ir(CodeGenerator* codegen, const char* filename);
int codegen_emit_object_file(CodeGenerator* codegen, const char* filename);
int codegen_emit_executable(CodeGenerator* codegen, const char* filename);
int codegen_optimize(CodeGenerator* codegen, int level);
int codegen_verify_module(CodeGenerator* codegen);

// WebAssembly target support
#if LLVM_AVAILABLE
int codegen_is_wasm_target(CodeGenerator* codegen);
int codegen_add_wasm_export(CodeGenerator* codegen, LLVMValueRef function, const char* export_name);
int codegen_add_wasm_import(CodeGenerator* codegen, LLVMValueRef function, 
                           const char* module_name, const char* import_name);
int codegen_declare_wasm_runtime_functions(CodeGenerator* codegen);
int codegen_configure_wasm_concurrency(CodeGenerator* codegen);
#endif

// Error reporting
void codegen_error(CodeGenerator* codegen, Position pos, const char* format, ...);
void codegen_warning(CodeGenerator* codegen, Position pos, const char* format, ...);

#endif // CODEGEN_H