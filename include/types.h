#ifndef TYPES_H
#define TYPES_H

#include "ast.h"
#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct Type Type;
typedef struct TypeChecker TypeChecker;
typedef struct Scope Scope;

// Type kinds
typedef enum {
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_INT8,
    TYPE_INT16,
    TYPE_INT32,
    TYPE_INT64,
    TYPE_UINT8,
    TYPE_UINT16,
    TYPE_UINT32,
    TYPE_UINT64,
    TYPE_FLOAT32,
    TYPE_FLOAT64,
    TYPE_STRING,
    TYPE_CHAR,
    
    // Compound types
    TYPE_ARRAY,
    TYPE_SLICE,
    TYPE_MAP,
    TYPE_CHANNEL,
    TYPE_FUNCTION,
    TYPE_POINTER,
    TYPE_REFERENCE,
    TYPE_STRUCT,
    TYPE_ENUM,
    TYPE_INTERFACE,
    
    // Goo extensions
    TYPE_ERROR_UNION,
    TYPE_NULLABLE,
    TYPE_QUALIFIED,  // Type with ownership/mutability qualifiers
    TYPE_CONCEPT,    // Concept type
    
    // Higher-kinded types
    TYPE_PARAM,      // Type parameter (e.g., T in Vec<T>)
    TYPE_PARAM_HKT,  // Higher-kinded type parameter (e.g., F in F<_>)
    TYPE_CONSTRUCTOR,// Type constructor (e.g., Vec, Option)
    TYPE_APPLICATION,// Type application (e.g., Vec<int>)

    // Package marker: the type of an imported package identifier
    // (e.g., the `fmt` in `fmt.Println(...)`). Selector access against
    // this kind resolves through the stdlib symbol table.
    TYPE_PACKAGE,

    TYPE_UNKNOWN,
    TYPE_COUNT
} TypeKind;

// Use the OwnershipKind from ast.h to avoid conflicts

// Mutability kinds
typedef enum {
    MUTABILITY_IMMUTABLE,
    MUTABILITY_MUTABLE
} MutabilityKind;

// Channel patterns (from AST)
// Already defined in ast.h as ChannelPattern

// Base type structure
struct Type {
    TypeKind kind;
    size_t size;        // Size in bytes
    size_t align;       // Alignment in bytes
    char* name;         // Type name (for debugging/error messages)
    
    // Type-specific data
    union {
        // Array type
        struct {
            Type* element_type;
            size_t length;
            // Comptime value params (fix round 4): set when this array
            // type's LENGTH expression references a comptime parameter
            // (stamped by type_from_ast's AST_ARRAY_TYPE case and the
            // array-literal checker via goo_expr_references_comptime_param).
            // In a comptime TEMPLATE body, `length` is then the placeholder
            // — const-index and literal-count validation defer to instance
            // time (the checker skips its upper-bound rejection; codegen's
            // index paths re-check against the instance's re-derived REAL
            // length and hard-fail on a genuine violation). 0 for every
            // ordinary array, whose template-time validation is unchanged.
            // Tail-appended within the union member (type_new memsets the
            // whole Type, so it starts 0 on every construction path).
            int comptime_length;
        } array;
        
        // Slice type
        struct {
            Type* element_type;
        } slice;
        
        // Map type
        struct {
            Type* key_type;
            Type* value_type;
        } map;
        
        // Channel type
        struct {
            Type* element_type;
            ChannelPattern pattern;
            char* endpoint;  // Optional network endpoint
        } channel;
        
        // Function type
        struct {
            Type** param_types;
            size_t param_count;
            Type* return_type;
            int is_variadic;
            // Concept constraints for generic functions
            struct ConceptDefinition** concept_constraints;
            size_t concept_constraint_count;
            // Fix 2 (comptime-param functions are not first-class values):
            // set by declare_function_signature whenever the FuncDeclNode
            // this signature was built from has ANY is_comptime_param
            // parameter (methods included). A function/method with this set
            // may only be the CALLEE of a direct call (identifier, method
            // selector on a concrete receiver, or package selector) — never
            // used as a plain value (interface satisfaction, var-decl init,
            // assignment, argument, return, composite storage). Without this
            // gate, aliasing such a function into a value strips away its
            // func_decl_node back-reference (a fresh Variable — e.g. a var,
            // a parameter, an interface method slot — has none), so
            // type_check_call_expr's per-argument comptime-constant check
            // (which walks checked_callee->func_decl_node) can never fire
            // again for that alias: a runtime value silently reaches the
            // comptime slot. New Type structs are calloc/memset-zeroed
            // (type_new), so every OTHER creation site defaults this to 0
            // with no explicit initialization needed.
            int has_comptime_params;
        } function;
        
        // Pointer type
        struct {
            Type* pointee_type;
        } pointer;
        
        // Reference type  
        struct {
            Type* referenced_type;
            int is_mutable;
        } reference;
        
        // Struct type
        struct {
            struct StructField* fields;
            size_t field_count;
            char* name;
        } struct_type;

        // Enum (tagged union) type
        struct {
            char* name;
            struct EnumVariant* variants;
            size_t variant_count;
        } enum_type;

        // Interface type
        struct {
            struct InterfaceMethod* methods;
            size_t method_count;
            char* name;
            // Metadata for synthesized interfaces
            int is_synthesized;
            struct ConceptDefinition* source_concept;
        } interface;
        
        // Error union type (!T)
        struct {
            Type* value_type;
            Type* error_type;  // Usually a standard error type
        } error_union;
        
        // Nullable type (?T)
        struct {
            Type* base_type;
        } nullable;
        
        // Qualified type (with ownership/mutability)
        struct {
            Type* base_type;
            OwnershipKind ownership;
            MutabilityKind mutability;
        } qualified;
        
        // Concept type
        struct {
            char* name;
            ASTNode* type_params;    // Type parameters
            ASTNode* requirements;   // Requirements/constraints
        } concept;
        
        // Type parameter (e.g., T in Vec<T>)
        struct {
            char* name;              // Parameter name
            int index;               // Parameter index in the context
            Type* constraint;        // Optional constraint/bound
        } type_param;
        
        // Higher-kinded type parameter (e.g., F in F<_>)
        struct {
            char* name;              // Parameter name
            int index;               // Parameter index
            size_t arity;            // Number of type parameters expected
            char* kind_signature;    // Kind signature (e.g., "* -> *")
            Type* constraint;        // Optional constraint
        } hkt_param;
        
        // Type constructor (e.g., Vec, Option)
        struct {
            char* name;              // Constructor name
            size_t arity;            // Number of type parameters
            Type** params;           // Type parameters (for generic constructors)
            size_t param_count;      // Number of parameters
            char* kind_signature;    // Kind signature
        } constructor;
        
        // Type application (e.g., Vec<int>)
        struct {
            Type* constructor;       // The type constructor
            Type** arguments;        // Applied type arguments
            size_t arg_count;        // Number of arguments
        } application;
    } data;
};

// Struct field
typedef struct StructField {
    char* name;
    Type* type;
    size_t offset;
    OwnershipKind ownership;
    MutabilityKind mutability;
    // Struct embedding: 1 iff this member was declared anonymously (`Base` /
    // `*Base`). Promotion (src/types/embedding.c) recurses only into flagged
    // members. Appended at the tail — rebuild needs `make clean`.
    int is_embedded;
} StructField;

// One variant of a tagged union: a named payload struct + its discriminant.
typedef struct EnumVariant {
    char* name;
    Type* payload;   // a TYPE_STRUCT (0+ fields)
    int tag;         // discriminant index, 0-based in declaration order
} EnumVariant;

// Interface method
typedef struct InterfaceMethod {
    char* name;
    Type* type;  // Function type
    struct InterfaceMethod* next;  // For linked lists
} InterfaceMethod;

// Variable information for type checking
typedef struct Variable {
    char* name;
    Type* type;
    OwnershipKind ownership;
    MutabilityKind mutability;
    int is_moved;           // For ownership tracking
    int is_borrowed;        // Currently borrowed
    int borrow_count;       // Number of active borrows
    int is_initialized;
    int is_builtin;         // Built-in function/variable
    Position declared_pos;
    // M11-types-const-integrate: comptime-evaluated value attached to
    // is_comptime constants. NULL for non-comptime variables and for
    // comptime constants whose RHS the engine cannot yet evaluate
    // (e.g., user-defined function calls — see M11-engine-recursion).
    // Owned by the Variable; freed in variable_free.
    struct ComptimeValue* comptime_value;
    // stdlib Phase 0: for a TYPE_PACKAGE marker Variable (an imported package
    // identifier such as `mypkg`), this points at the resolved Package whose
    // `exports` scope holds its A-Z top-level symbols, so cross-package
    // selector resolution (Task 5) can reach them. NULL for every ordinary
    // variable and for the hardcoded stdlib markers (handled by the shim).
    // NOT owned — the Package is owned by TypeChecker.packages.
    struct Package* package;
    struct Variable* next;  // For linked list in scope
    // Closures Task 2: the declaring VarDeclNode (or NULL for a binding not
    // backed by one — an if-let bind, a for-range loop variable, or a
    // 2-value `a, b := x, y` short decl/MultiAssignNode). Set at
    // registration by every VarDeclNode-backed binding site (function/
    // literal params, named return params, ordinary var-decls —
    // type_checker.c and expression_checker.c) AND by codegen's own mirror
    // re-registrations of the SAME declarations (function_codegen.c —
    // codegen re-invokes type_check_* internally during its own emission
    // pass and needs an identical, not just equivalent, capture-detection
    // outcome on re-resolution). type_check_identifier's capture path
    // (expression_checker.c) stamps is_captured on this node so codegen's
    // promotion pass can find it; a NULL (or non-AST_VAR_DECL) decl_node
    // when a capture is detected is rejected with a clear error instead of
    // silently under-promoting (see type_checker_record_capture).
    // Tail-appended per the no-header-deps convention (ast.h's M10 note).
    struct ASTNode* decl_node;
    // Closures Task 2: set alongside decl_node's VarDeclNode->is_captured
    // (type_checker_record_capture) when a nested func literal reads or
    // writes this variable. Codegen reads the VarDeclNode flag (the AST
    // survives into codegen; this Variable does not), so this field exists
    // primarily for checker-side introspection/diagnostics parity with the
    // VarDeclNode flag it mirrors.
    int is_captured;
    // Closures Task 2 (loop-variable capture fix): set for a variable
    // declared BY a loop construct — the 3-clause for's init declaration
    // (`for i := 0; ...`, including a multi-name `for i, j := 0, 1;`) and
    // for-range key/value bindings (`for k, v := range ...`) — at
    // type_check_for_stmt's registration/marking sites (type_checker.c).
    // Capturing such a variable in a closure is REJECTED by
    // type_checker_record_capture (expression_checker.c): modern Go (1.22+)
    // gives each iteration its OWN variable, but this compiler's promotion
    // model has exactly one slot per declaration, which would silently
    // compute pre-1.22 shared-slot results (the classic loop-capture bug)
    // — reject-rather-than-deviate, per the #101 precedent. Per-iteration
    // slots are the recorded follow-up; the `i := i` body-local copy is the
    // supported workaround (an ordinary local — capturable, and its slot is
    // minted per execution of the declaration; see
    // codegen_alloc_local_promoted's decl-site allocation note,
    // function_codegen.c). Tail-appended per the no-header-deps convention.
    int is_loop_var;
    // fix/const-array-length: for a const Variable (type_check_const_decl)
    // whose RHS folds to a compile-time integer via goo_fold_const_int_ctx,
    // the folded value is cached here so a later array-length reference
    // (`[N]int`, `[N+1]int`) can resolve the real length instead of the
    // pre-existing silent placeholder-10 fallback. has_const_int_value is 0
    // for every non-const variable and for any const whose RHS doesn't fold
    // to an integer (e.g. a string const, or a call). Tail-appended per the
    // no-header-deps convention.
    int has_const_int_value;
    uint64_t const_int_value;
    // Function generics Task 4: marks this Variable as a generic function
    // TEMPLATE (its FuncDeclNode has type_params != NULL). Set by
    // declare_function_signature alongside registration. generic_decl is the
    // back-reference codegen's monomorphizer (M3) will instantiate from;
    // type_param_count is the total number of type-param names across all
    // groups (tpn in declare_function_signature). is_generic is 0 and
    // generic_decl is NULL for every ordinary (non-generic) function/variable.
    int is_generic;
    struct ASTNode* generic_decl;
    size_t type_param_count;
    // Comptime value parameters (Task 2): the FuncDeclNode this Variable was
    // registered from (declare_function_signature), for EVERY function/method
    // Variable — not just generic templates (contrast generic_decl, which
    // stays NULL here). Lets the call-argument checker (expression_checker.c)
    // walk the callee's real parameter list and read each VarDeclNode's
    // is_comptime_param flag, positionally matched against param_types[idx]
    // (recv_offset already accounts for a spliced method receiver). NULL for
    // every non-function Variable (ordinary locals, consts, package markers).
    // Not owned — the FuncDeclNode is owned by the AST, freed independently.
    struct ASTNode* func_decl_node;
} Variable;

// Closures Task 2: cap on simultaneously-open func-literal nesting tracked by
// TypeChecker.literal_stack (below) — see that field's doc comment.
#define GOO_CLOSURE_MAX_NESTING 64

// Scope for variable and type tracking
struct Scope {
    Variable* variables;
    struct Scope* parent;
    int scope_id;
    // Closures Task 2: set for the scope pushed as a function's OR a func
    // literal's own body (type_check_function_decl / type_check_func_lit,
    // and their codegen mirrors in function_codegen.c) — never for a plain
    // nested block scope (if/for/switch/etc — an ordinary scope_push leaves
    // this 0). type_check_identifier (expression_checker.c) walks ->parent
    // counting these to detect a capture: a resolution that has to leave at
    // least one such scope before finding its binding. Tail-appended per
    // the no-header-deps convention (ast.h's M10 note).
    int is_function_boundary;
};

// Imported package namespace. Each imported package owns an `exports` scope
// holding fresh Variable copies of its capitalised (A-Z) top-level symbols.
// `state` drives cycle detection during resolution: 0=unvisited, 1=in-progress,
// 2=done. `import_path`/`name` are owned (str_dup'd) and freed in
// type_checker_free.
typedef struct Package {
    char* import_path;      // canonical import path (owned)
    char* name;             // package identifier used at call sites (owned)
    Scope* exports;         // fresh Variable copies of exported symbols (owned)
    int state;              // 0=unvisited 1=in-progress 2=done
    struct Package* next;   // intrusive list link
} Package;

// Forward declarations for enhanced interface system
struct ConstraintInferenceEngine;
struct ConceptRegistry;
struct HKTRegistry;
struct ProtocolRegistry;

// Forward declaration for compile-time integration
struct ComptimeContext;
struct ComptimeValue;

// Compile-time type computation result
typedef struct ComptimeTypeResult {
    Type* type;                     // Computed type
    struct ComptimeValue* value;    // Associated compile-time value (if any)
    int is_valid;                   // Whether computation succeeded
    char* error_message;            // Error message if computation failed
} ComptimeTypeResult;

// Type-level function for compile-time evaluation
typedef struct TypeFunction {
    char* name;                     // Function name
    Type** param_types;             // Parameter types
    size_t param_count;             // Number of parameters
    Type* return_type;              // Return type (may be computed)
    ASTNode* body;                  // Function body for compile-time evaluation
    int is_comptime_only;           // Can only be called at compile time
    struct TypeFunction* next;      // For linked lists
} TypeFunction;

// Compile-time type context for the type checker
typedef struct ComptimeTypeContext {
    struct ComptimeContext* comptime_ctx;  // Main comptime context
    TypeFunction* type_functions;          // Available type-level functions
    Type** computed_types;                 // Cache of computed types
    size_t computed_type_count;
    size_t computed_type_capacity;
} ComptimeTypeContext;

// Function generics Task 6: one resolved call-site instantiation, appended by
// type_check_record_instantiation (type_checker.c) whenever a call through a
// generic Variable (fn->is_generic) has its type arguments successfully
// inferred. `fn` is the generic template Variable itself (fn->generic_decl is
// the FuncDeclNode the monomorphizer instantiates from); `args[i]` is the
// concrete Type bound to fn's type-param index i, `n` == fn->type_param_count.
// `args` is a malloc'd COPY of the call site's inferred bindings, independent
// of that same call's CallExprNode.type_args (expression_checker.c's
// type_check_generic_call builds both from one calloc'd array but hands each
// side its own copy) — this list's node and its `args` array are owned and
// freed together by type_checker_free, entirely independent of the AST's own
// teardown (ast_node_free), so neither free path depends on the other's
// ordering or reasons about a shared pointer. Intrusive singly-linked list,
// head at TypeChecker.instantiations (below); the same {fn,args} tuple may be
// recorded more than once for repeated calls with the same type arguments —
// deduplication is the monomorphizer's job (Task 9), not this recorder's.
//
// Comptime+generic composition (sub-project 2): `comptime_values`/
// `comptime_value_n` carry the comptime axis for a function that declares
// BOTH `[T]` type params and `comptime` value params — a composed call's
// SINGLE seed on this list carries both payloads, keyed together (see
// type_check_generic_call, expression_checker.c). A generic-only function
// (no comptime params) always records `comptime_value_n == 0`,
// `comptime_values == NULL` here — the plain ComptimeInstantiation list
// below is untouched by generic calls and continues to serve ONLY
// non-generic comptime-param functions (see that struct's doc comment for
// which functions land on which list). Like `args` above, `comptime_values`
// is a malloc'd COPY the caller hands off ownership of; freed alongside
// `args` in type_checker_free, same non-double-free reasoning. Appended at
// the STRUCT TAIL per the no-header-deps convention (ast.h's note) — a
// mid-list field insertion would shift every field after it.
typedef struct GenericInstantiation {
    Variable* fn;
    Type** args;
    size_t n;
    struct GenericInstantiation* next;
    int64_t* comptime_values;
    size_t comptime_value_n;
} GenericInstantiation;

// Comptime value params Task 3: one resolved call-site instantiation for a
// comptime-parameterized function, appended by
// type_check_record_comptime_instantiation (type_checker.c) whenever a
// direct call to a has_comptime_params function type-checks successfully.
// Mirrors GenericInstantiation (the type-arg axis) but keys on int64_t
// comptime VALUES instead of concrete Types. Kept as its own list for a
// PLAIN (non-generic) comptime-parameterized function. UPDATE (sub-project 2,
// comptime+generic composition): the two axes CAN now coexist on one
// FuncDeclNode (`func kernel[T any](comptime n int, data T)`), but a
// composed function's instantiations never land here — they're recorded on
// GenericInstantiation's comptime_values/comptime_value_n tail fields
// instead (type_check_generic_call, expression_checker.c), since a call
// through a generic Variable is dispatched there entirely, never reaching
// this list's sole recording site (type_check_call_expr, gated on a plain
// non-generic identifier callee). So the split is still exact — every
// FuncDeclNode's instantiations live on exactly one of the two lists, keyed
// on whether it declares `[T]` type params — and neither axis's consumer
// (mono_instantiate's type-substitution walk vs. the comptime worklist in
// monomorphize.c) has to guard against the other's fields being
// absent/meaningless for the entries it actually walks. `fn` is the
// plain-function Variable itself (fn->func_decl_node is the FuncDeclNode the
// monomorphizer instantiates from — see that field's own doc comment: unlike
// generic_decl, it is set for EVERY function, not just templates). `values`/
// `n` are a malloc'd COPY of the call site's CallExprNode.comptime_value_args,
// independently owned and freed with this list (type_checker_free), same
// ownership split GenericInstantiation uses for its own `args`. The same
// {fn,values} tuple may be recorded more than once for repeated calls with
// identical comptime arguments — dedup is the monomorphizer's job (mirrors
// GenericInstantiation here too).
typedef struct ComptimeInstantiation {
    Variable* fn;
    int64_t* values;
    size_t n;
    struct ComptimeInstantiation* next;
} ComptimeInstantiation;

// Type checker state
struct TypeChecker {
    Scope* current_scope;
    int next_scope_id;
    Type** builtin_types;   // Array of builtin types
    
    // Error reporting
    char* current_file;
    int error_count;
    int warning_count;
    
    // Type cache for performance
    Type** type_cache;
    size_t type_cache_size;
    size_t type_cache_capacity;
    
    // Enhanced interface system components
    struct ConstraintInferenceEngine* constraint_engine;
    struct ConceptRegistry* concept_registry;
    struct HKTRegistry* hkt_registry;
    struct ProtocolRegistry* protocol_registry;
    
    // Compile-time type computation support
    ComptimeTypeContext* comptime_type_ctx;

    // Return type of the enclosing function — set when entering a function body
    // so that context-sensitive builtins (e.g. error()) can look it up.
    Type* current_return_type;

    // Imported-package registry (stdlib Phase 0 scaffolding). `packages` is the
    // head of a linked list of resolved packages; `current_package` is the
    // package whose body is being checked (NULL == the main package). Both are
    // NULL until Task 3 wires import resolution in.
    Package* packages;
    Package* current_package;

    // Closures Task 2: stack of func literals currently being body-checked
    // (innermost = last, index literal_stack_len-1), pushed/popped by
    // type_check_func_lit (expression_checker.c) around its own body check,
    // symmetric with (and immediately alongside) that function's existing
    // scope_push/scope_pop. Used by type_check_identifier's capture path
    // (type_checker_record_capture) to record a capture on EVERY literal
    // between a boundary-crossing reference and its declaring scope — the
    // classic nested-closure "missing middle env" bug: an inner literal's
    // own env is populated correctly, but each INTERMEDIATE literal must
    // ALSO carry the name through its own env to relay the slot pointer
    // inward. A fixed array (not malloc'd) since literal nesting depth is
    // bounded by source structure, not runtime data — no realistic program
    // nests closures anywhere near GOO_CLOSURE_MAX_NESTING deep; a program
    // that does silently stops recording further capture relaying past the
    // cap rather than crashing (see type_check_func_lit's push). Tail-
    // appended per the no-header-deps convention (ast.h's M10 note).
    struct ASTNode* literal_stack[GOO_CLOSURE_MAX_NESTING];
    size_t literal_stack_len;

    // Function generics Task 3: active generic type parameters, innermost
    // function's params on top. Consulted by type_from_ast so a bare `T` in
    // a generic function's signature/body resolves to its TYPE_PARAM Type
    // instead of "Unknown type 'T'". Pushed by declare_function_signature
    // and type_check_function_decl (popped on every return path of both —
    // a missed pop leaks type params into sibling functions). Fixed array,
    // not malloc'd: type-param-list nesting is bounded by source structure
    // (Tier A functions are small), same rationale as literal_stack above.
    Type* active_type_params[32];
    size_t active_type_param_count;

    // Function generics Task 6: head of the linked list of resolved
    // generic-call instantiations recorded by type_check_record_instantiation
    // during call checking. NULL until the first generic call type-checks
    // successfully. The monomorphizer (Task 9) walks this list after
    // type_check_program returns to know which concrete instantiations of
    // each generic function to emit; torn down in type_checker_free.
    GenericInstantiation* instantiations;

    // Comptime value params Task 3: head of the linked list of resolved
    // comptime-call instantiations recorded by
    // type_check_record_comptime_instantiation during call checking. NULL
    // until the first comptime call type-checks successfully. The
    // monomorphizer (codegen_monomorphize, monomorphize.c) walks this list
    // after type_check_program returns, alongside `instantiations` above, to
    // know which concrete value-specializations of each comptime-param
    // function to emit; torn down in type_checker_free.
    ComptimeInstantiation* comptime_instantiations;

    // gofmt-syntax-b Task 1 (P1.5): per-function flat label registry. Labels
    // are FUNCTION-scoped in Go (not block/lexical-scoped — the same name
    // used in two disjoint sibling blocks of one function is still a
    // duplicate), so a flat array reset at function entry (and around each
    // func-literal body, which gets its own independent label namespace) is
    // the right shape, not a stack. Fixed-size like active_type_params above
    // (source-bounded, not runtime data). type_check_statement's
    // AST_LABEL_STMT case appends here (positioned duplicate-label error on
    // a name already present); labeled break/continue's "does this label
    // exist and enclose me" check is a CODEGEN-time concern (mirrors the
    // existing unlabeled break/continue, which is likewise unchecked at
    // typecheck time — see type_check_statement's AST_BREAK_STMT case).
    char* label_names[64];
    Position label_positions[64];
    size_t label_count;
};

// Type creation functions
Type* type_new(TypeKind kind);
Type* type_void(void);
Type* type_bool(void);
Type* type_int(int bits, int is_signed);
Type* type_float(int bits);
Type* type_string_type(void);
Type* type_char(void);

Type* type_array(Type* element_type, size_t length);
Type* type_slice(Type* element_type);
Type* type_map(Type* key_type, Type* value_type);
Type* type_channel(Type* element_type, ChannelPattern pattern);
Type* type_function(Type** param_types, size_t param_count, Type* return_type);
Type* type_pointer(Type* pointee_type);
Type* type_reference(Type* referenced_type, int is_mutable);

// Goo extension types
Type* type_error_union(Type* value_type, Type* error_type);
Type* type_nullable(Type* base_type);
Type* type_qualified(Type* base_type, OwnershipKind ownership, MutabilityKind mutability);
Type* type_concept(const char* name);

// Higher-kinded type creation functions
Type* type_param(const char* name, int index, Type* constraint);
Type* type_param_hkt(const char* name, int index, size_t arity, const char* kind_signature, Type* constraint);
Type* type_constructor(const char* name, size_t arity, const char* kind_signature);
Type* type_application(Type* constructor, Type** arguments, size_t arg_count);

// Type operations
void type_free(Type* type);
Type* type_copy(const Type* type);
int type_equals(const Type* a, const Type* b);
int type_compatible(const Type* from, const Type* to);
const char* type_to_string(const Type* type);
size_t type_size(const Type* type);
size_t type_align(const Type* type);

// Method support: a method `func (T) m()` lowers to an ordinary function
// named "T__m" (see type_method_mangled_name). type_receiver_name extracts
// the receiver's struct name (unwrapping a *T pointer receiver) for mangling.
char* type_method_mangled_name(const char* type_name, const char* method_name);
const char* type_receiver_name(const Type* type);

// P4-3: does `concrete`'s method set satisfy interface `iface`? Returns 1 if so
// (or for the empty interface). On failure returns 0 and writes the offending
// method name and reason ("missing" / "signature mismatch" / "comptime" — a
// method with a `comptime` parameter, see report_comptime_method_not_satisfied
// below) to the out params.
int type_interface_satisfied(TypeChecker* checker, Type* iface,
                             Type* concrete, const char** method_out,
                             const char** reason_out);

// Fix 2 (comptime-param functions are not first-class values): shared
// diagnostic for the reason_out == "comptime" case above — every caller of
// type_interface_satisfied special-cases that reason with this instead of
// its own generic "X does not implement Y" message, so the wording stays in
// one place. `method` is the offending method name (method_out).
void report_comptime_method_not_satisfied(TypeChecker* checker, Position pos,
                                          const char* method);

// Validate assigning `src` into a `target`-typed location. For an interface
// target this accepts any concrete implementer (emitting "X does not implement
// Y" otherwise) and any interface (permissive); for a non-interface target it
// falls back to type_compatible. Returns 1 if the assignment is allowed.
int check_interface_assign(TypeChecker* checker, Type* src, Type* target,
                           Position pos);

// Fix 2 (comptime-param functions are not first-class values): a function
// with any `comptime` parameter may only be the CALLEE of a direct call
// (identifier, method selector on a concrete receiver, or package selector)
// — never captured as a plain value. `t` is the VALUE's type at a
// value-consuming site (var-decl init, assignment RHS, call argument, return
// expression, composite storage — NOT the callee position of a call, which
// must stay unchecked here). No-ops (returns 1) unless `t` is a TYPE_FUNCTION
// with has_comptime_params set. `src_expr`, if the identifier or bound-method
// selector the value came from, supplies the function's declared name for
// the diagnostic; pass NULL if unavailable (falls back to the type's
// rendered name). `context` is a short verb phrase completing "function 'x'
// has comptime parameters and cannot be <context>" (e.g. "used as a value",
// "passed as an argument", "returned"). Emits that diagnostic and returns 0
// on rejection.
int reject_comptime_function_value(TypeChecker* checker, struct ASTNode* src_expr,
                                   Type* t, Position pos, const char* context);

// Type checking utilities
int type_is_integer(const Type* type);
int type_is_float(const Type* type);
int type_is_numeric(const Type* type);
int type_is_signed(const Type* type);
int type_is_pointer_like(const Type* type);
int type_is_nullable(const Type* type);
int type_is_error_union(const Type* type);
int type_is_error(const Type* type);

// Type checker functions
TypeChecker* type_checker_new(void);
void type_checker_free(TypeChecker* checker);

// Package registry (stdlib Phase 0). find is a linear search by import path;
// add str_dup's both strings, allocates a fresh exports scope, and pushes onto
// the checker's package list. package_export_filter copies every A-Z-leading
// top-level symbol of `pkg_scope` into `exports` as a FRESH Variable (so the two
// scopes never share ownership of the same Variable node).
Package* type_checker_find_package(TypeChecker* checker, const char* import_path);
Package* type_checker_add_package(TypeChecker* checker, const char* import_path, const char* name);
void package_export_filter(Scope* pkg_scope, Scope* exports);

// Seed a TYPE_PACKAGE marker for an imported package into the current scope,
// carrying the resolved Package*. This is the SINGLE seeding path for both the
// conditional stdlib-shim markers (driver, on real `import`) and user-package
// markers, replacing the former always-on seeding in
// type_checker_add_builtin_functions. Returns the marker or NULL.
Variable* type_checker_seed_package_marker(TypeChecker* checker,
                                           const char* name, Package* package);

// Scope management
Scope* scope_new(Scope* parent);
void scope_free(Scope* scope);
void scope_push(TypeChecker* checker);
void scope_pop(TypeChecker* checker);

// Variable management
Variable* variable_new(const char* name, Type* type, Position pos);
void variable_free(Variable* var);
int scope_add_variable(Scope* scope, Variable* var);
Variable* scope_lookup_variable(Scope* scope, const char* name);
Variable* type_checker_lookup_variable(TypeChecker* checker, const char* name);

// Function generics Task 3: active-type-param stack (see TypeChecker's
// active_type_params field above). Push/pop bracket a function's signature
// and body checking; lookup is consulted by type_from_ast before it would
// otherwise report "Unknown type '<name>'".
void type_checker_push_type_param(TypeChecker* checker, Type* tp);
void type_checker_pop_type_params(TypeChecker* checker, size_t to_count);
Type* type_checker_lookup_type_param(TypeChecker* checker, const char* name);

// Function generics Task 5: standalone helpers for generic-call inference
// (Task 6 wires them in). type_substitute replaces every TYPE_PARAM of index
// i < n with bindings[i], recursing through slice/pointer/function (Tier-A
// scope; TYPE_ARRAY/TYPE_MAP are not recursed). unify_types structurally
// matches a possibly-TYPE_PARAM `param` against a concrete `arg`, inferring
// bindings; returns 0 on structural mismatch or a conflicting binding.
Type* type_substitute(Type* t, Type** bindings, size_t n);
int unify_types(Type* param, Type* arg, Type** bindings, size_t n);

// Fix round 7 (I-r6), exposed non-static for sub-project 2: does `t` contain
// a TYPE_PARAM anywhere in its structure (Tier-A recursion: slice/pointer/
// array/nullable/function)? Originally private to expression_checker.c's
// generic-call inference; declare_function_signature (type_checker.c) now
// reuses it for the `comptime n T` declaration wall (a comptime parameter
// typed by, or containing, a type parameter is rejected) instead of growing
// a near-duplicate walker in a second file.
int type_contains_type_param(const Type* t);

// Function generics Task 6: append {fn, args, n} to checker->instantiations.
// Takes ownership of `args` (the caller's calloc'd bindings array — do not
// free or reuse it after this call). `call_site` is the call expression that
// produced this instantiation; kept for a possible future per-callsite
// diagnostic (not read by Task 9's monomorphizer, which only needs fn/args/n).
//
// Comptime+generic composition (sub-project 2): `comptime_values`/
// `comptime_value_n` extend this same recorder to carry the comptime axis
// for a composed call (see GenericInstantiation's doc comment above) —
// takes ownership of `comptime_values` exactly like `args`. A generic-only
// call passes NULL/0, matching GenericInstantiation's 0/NULL default for a
// generic-only seed.
void type_check_record_instantiation(TypeChecker* checker, Variable* fn,
                                     Type** args, size_t n,
                                     int64_t* comptime_values, size_t comptime_value_n,
                                     struct ASTNode* call_site);

// Comptime value params Task 3: append {fn, values, n} to
// checker->comptime_instantiations. Takes ownership of `values` (a malloc'd
// copy the caller must not free or reuse afterward) — mirrors
// type_check_record_instantiation's ownership contract for `args` above,
// one axis over.
void type_check_record_comptime_instantiation(TypeChecker* checker, Variable* fn,
                                               int64_t* values, size_t n,
                                               struct ASTNode* call_site);

// Type checking entry points
int type_check_program(TypeChecker* checker, ASTNode* program);

// stdlib Phase 0 (Task 4): type-check one imported package's body. Sets
// checker->current_package = pkg, pushes a fresh package scope, runs the same
// declaration loop as type_check_program, then publishes the package's A-Z
// top-level symbols into pkg->exports via package_export_filter.
//
// LIFETIME CONTRACT (asymmetric BY DESIGN): on success this returns with the
// package scope STILL PUSHED and current_package STILL SET. The caller codegens
// the package while that scope is live (codegen recovers each function's
// signature by looking it up under its bare name in this scope) and only then
// calls scope_pop() and clears current_package. Popping here would hide the
// package's functions from codegen and drop their parameters.
int type_check_package(TypeChecker* checker, Package* pkg, ASTNode* program);
Type* type_check_expression(TypeChecker* checker, ASTNode* expr);
int type_check_statement(TypeChecker* checker, ASTNode* stmt);
int type_check_declaration(TypeChecker* checker, ASTNode* decl);

// Declaration type checking functions
int type_check_function_decl(TypeChecker* checker, ASTNode* decl);
int type_check_var_decl(TypeChecker* checker, ASTNode* decl);
int type_check_multi_assign(TypeChecker* checker, ASTNode* stmt);
int type_check_const_decl(TypeChecker* checker, ASTNode* decl);
int type_check_type_decl(TypeChecker* checker, ASTNode* decl);
int type_check_concept_decl(TypeChecker* checker, ASTNode* decl);

// Statement type checking functions
int type_check_block_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_expr_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_if_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_for_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_return_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_go_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_defer_stmt(TypeChecker* checker, ASTNode* stmt);
int type_check_select_stmt(TypeChecker* checker, ASTNode* stmt);
// Type assertions branch, Task 3: `switch [v :=] x.(type) { case … }`.
int type_check_type_switch_stmt(TypeChecker* checker, ASTNode* stmt);

// Helper functions
Type* type_from_ast(TypeChecker* checker, ASTNode* type_node);

// Named-return-parameter helper (P3-5): true iff `n` is a synthetic
// `_0`/`_1`/... placeholder for an anonymous tuple-return field (not a
// user-named result). Shared by the type checker and codegen.
int is_synthetic_result_name(const char* n);

// Expression type checking functions
Type* type_check_identifier(TypeChecker* checker, ASTNode* expr);
Type* type_check_literal(TypeChecker* checker, ASTNode* expr);
Type* type_check_binary_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_unary_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_call_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_make_chan_call(TypeChecker* checker, CallExprNode* call, ASTNode* expr);
Type* type_check_index_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_slice_index_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_selector_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_struct_literal(TypeChecker* checker, ASTNode* expr);
Type* type_check_match_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_try_expr(TypeChecker* checker, ASTNode* expr);
Type* type_check_catch_expr(TypeChecker* checker, ASTNode* expr);

// Expression helper functions
Type* type_check_arithmetic_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op, Position pos);
Type* type_check_comparison_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op, Position pos);
Type* type_check_channel_send_op(TypeChecker* checker, Type* channel_type, Type* value_type, Position pos);
Type* type_check_channel_receive_op(TypeChecker* checker, Type* channel_type, Position pos);
Type* type_check_logical_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op, Position pos);
Type* type_check_bitwise_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op, Position pos);
// `value_expr` is the RHS expression node (Fix 2: supplies a captured
// comptime-parameterized function's declared name for its rejection
// diagnostic — see reject_comptime_function_value); pass NULL if unavailable.
Type* type_check_assignment_op(TypeChecker* checker, ASTNode* target, Type* target_type, Type* value_type, ASTNode* value_expr, Position pos);

// Fold an integer constant expression at 128-bit precision (see
// expression_helpers.c). Returns 1 and sets *out on success; 0 if the
// expression is not a pure compile-time integer constant. Used by const-decl
// type-checking and codegen so masks like `1<<32 - 1` / `1<<64 - 1` evaluate to
// their true value instead of a width-truncated LLVM shift.
int goo_fold_const_int(ASTNode* expr, uint64_t* out);

// Checker-aware sibling of goo_fold_const_int: additionally resolves
// AST_IDENTIFIER by looking the name up in checker's scope chain and using
// its cached Variable->const_int_value (set by type_check_const_decl), and
// recurses through unary/binary operators WITH that same context so a
// const-expression built on a const-identifier (`[N+1]int`) folds too. Falls
// back to goo_fold_const_int for literals and anything else it doesn't
// special-case. Introduced for fix/const-array-length (array-length
// resolution); the original context-free goo_fold_const_int above is
// unchanged and keeps its existing (non-identifier) call sites.
int goo_fold_const_int_ctx(TypeChecker* checker, ASTNode* expr, uint64_t* out);

// Comptime value params Task 3 (fix round 2): does `t` contain a TYPE_ARRAY
// anywhere in its structure (through nested arrays, slices, pointers,
// nullables)? Used by comptime-instance codegen (function_codegen.c /
// composite_codegen.c) to decide whether a template-cached Type must be
// re-derived from its AST type node under the instance's comptime-param
// binding — only array LENGTHS can differ per instance, so anything with no
// array anywhere can keep the cached Type object untouched.
int goo_type_contains_array(const Type* t);

// Comptime value params (fix round 6, C-r5): does `t` contain a
// comptime_length-FLAGGED array anywhere in its structure (recursing
// through array ELEMENTS as well as slices/pointers/nullables — `[2][n]int`
// is unflagged outside, flagged inside)? This — not the any-array walk
// above — is the correct gate for the comptime instance re-derivation
// sites: gating on ANY array re-derived types whose template resolution was
// never placeholder-tainted, so a block-local `const n = 3` SHADOWING the
// comptime param split-brained (the checker resolved the shadow's length,
// while codegen's mirror-scope re-derivation resolved the PARAM instead —
// wrong length, or a false instance-named rejection). An unflagged array's
// template type is already correct and must be left untouched.
int goo_type_contains_comptime_array(const Type* t);

// Comptime value params (fix round 6, M-r5c): set comptime_length on an
// array Type AND rewrite its display name so diagnostics render the
// comptime dimension as `[<param-name>]` (identifier length expression) or
// `[comptime]` (compound expression) instead of the template placeholder
// ("[1]int64"). The single stamping entry point — both stamp sites
// (type_from_ast's AST_ARRAY_TYPE case, the array-literal checker) call
// this instead of setting the flag directly.
void type_array_mark_comptime(Type* t, ASTNode* length_expr);

// Comptime value params (fix round 3/4): does `expr` contain any identifier
// that resolves, in checker's CURRENT scope, to a comptime PARAMETER
// (Variable whose decl_node is a VarDeclNode with is_comptime_param)?
// Recurses through the same expression shapes goo_fold_const_int_ctx folds.
// Three consumers: the comptime-argument capture site (expression_checker.c
// — keeps the const folder away from the enclosing param's placeholder),
// the array-literal checker (defer count-vs-length validation to instance
// time), and type_from_ast's AST_ARRAY_TYPE case (stamp
// Type.data.array.comptime_length so const-index validation defers too).
int goo_expr_references_comptime_param(TypeChecker* checker, ASTNode* expr);

// Fold a compile-time string constant — string literals joined by `+` — into a
// freshly malloc'd byte buffer. On success returns 1, writes *out (the buffer,
// which the caller must free) and *out_len (the byte length, embedded NULs
// preserved). Returns 0 for anything that is not a pure string-literal
// concatenation. Enables package/local `const` string tables built by `+`
// (e.g. the math/bits len8tab), which otherwise lower to a runtime
// goo_string_concat call that is not a compile-time constant.
int goo_fold_const_string(ASTNode* expr, char** out, size_t* out_len);

// Error reporting
void type_error(TypeChecker* checker, Position pos, const char* format, ...);
void type_warning(TypeChecker* checker, Position pos, const char* format, ...);

// Builtin types access
void type_checker_init_builtins(TypeChecker* checker);
void type_checker_add_builtin_functions(TypeChecker* checker);
Type* type_checker_get_builtin(TypeChecker* checker, TypeKind kind);

// The v1 representation of Go's `error` interface: a nullable pointer
// (`?*int8`). Single source of truth — the type checker (`error` keyword,
// the n,err destructure bind, errors.New) and codegen (destructure, errors.New)
// all build the error type through this, so Phase 6 (real error struct /
// `.Error()`) changes one place instead of five.
Type* type_checker_error_type(TypeChecker* checker);

// Channel helper functions
const char* channel_pattern_string(ChannelPattern pattern);

// Compile-time type integration functions
ComptimeTypeContext* comptime_type_context_new(struct ComptimeContext* comptime_ctx);
void comptime_type_context_free(ComptimeTypeContext* ctx);

// Type-level function management
TypeFunction* type_function_new(const char* name, Type** param_types, size_t param_count, 
                               Type* return_type, ASTNode* body);
void type_function_free(TypeFunction* func);
int comptime_type_register_function(ComptimeTypeContext* ctx, TypeFunction* func);
TypeFunction* comptime_type_lookup_function(ComptimeTypeContext* ctx, const char* name);

// Compile-time type computation
ComptimeTypeResult* comptime_type_evaluate(TypeChecker* checker, ASTNode* expr);
Type* comptime_type_call_function(TypeChecker* checker, const char* func_name, 
                                 struct ComptimeValue** args, size_t arg_count);
Type* comptime_type_from_value(struct ComptimeValue* value);

// Type checking with compile-time support
Type* type_check_comptime_expr(TypeChecker* checker, ASTNode* expr);
int type_check_comptime_block(TypeChecker* checker, ASTNode* block);

// Dependent type support with comptime values
Type* type_create_dependent(Type* base_type, struct ComptimeValue* constraint_value);
int type_validate_dependent_constraint(Type* dependent_type, struct ComptimeValue* value);

// Result management
ComptimeTypeResult* comptime_type_result_new(Type* type, struct ComptimeValue* value);
void comptime_type_result_free(ComptimeTypeResult* result);

// Built-in type-level functions
void comptime_type_register_builtins(ComptimeTypeContext* ctx);

// TypeChecker integration functions
int type_checker_init_comptime(TypeChecker* checker, struct ComptimeContext* comptime_ctx);
void type_checker_cleanup_comptime(TypeChecker* checker);

#endif // TYPES_H