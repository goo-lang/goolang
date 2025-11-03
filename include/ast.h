#ifndef AST_H
#define AST_H

#include "token.h"
#include <stddef.h>

// Forward declarations
typedef struct ASTNode ASTNode;
typedef struct Type Type;

// AST Node Types
typedef enum {
    // Declarations
    AST_PROGRAM,
    AST_PACKAGE_DECL,
    AST_IMPORT_SPEC,
    AST_FUNC_DECL,
    AST_VAR_DECL,
    AST_CONST_DECL,
    AST_TYPE_DECL,
    AST_CONCEPT_DECL,       // concept definition
    AST_HKT_PARAM,          // higher-kinded type parameter
    
    // Statements
    AST_BLOCK_STMT,
    AST_EXPR_STMT,
    AST_IF_STMT,
    AST_IF_LET_STMT,
    AST_FOR_STMT,
    AST_RANGE_STMT,
    AST_RETURN_STMT,
    AST_BREAK_STMT,
    AST_CONTINUE_STMT,
    AST_DEFER_STMT,
    AST_GO_STMT,
    AST_SELECT_STMT,
    AST_SELECT_CASE,
    AST_SWITCH_STMT,
    AST_CASE_CLAUSE,
    AST_DEFAULT_CLAUSE,
    AST_UNSAFE_STMT,
    AST_ASM_STMT,
    
    // Expressions
    AST_IDENTIFIER,
    AST_LITERAL,
    AST_COMPOSITE_LIT,
    AST_BINARY_EXPR,
    AST_UNARY_EXPR,
    AST_POSTFIX_EXPR,
    AST_CALL_EXPR,
    AST_INDEX_EXPR,
    AST_SELECTOR_EXPR,
    AST_SLICE_EXPR,
    AST_TYPE_ASSERT_EXPR,
    AST_PAREN_EXPR,
    
    // Types
    AST_BASIC_TYPE,
    AST_ARRAY_TYPE,
    AST_SLICE_TYPE,
    AST_MAP_TYPE,
    AST_CHAN_TYPE,
    AST_FUNC_TYPE,
    AST_INTERFACE_TYPE,
    AST_STRUCT_TYPE,
    AST_TUPLE_TYPE,
    AST_POINTER_TYPE,
    AST_REFERENCE_TYPE,
    
    // Goo Extensions
    AST_ERROR_UNION_TYPE,   // !T
    AST_NULLABLE_TYPE,      // ?T
    AST_TRY_EXPR,          // try expression
    AST_CATCH_EXPR,        // catch expression
    AST_COMPTIME_BLOCK,    // comptime { ... }
    AST_OWNERSHIP_QUAL,    // owned, borrowed, shared
    AST_UNSAFE_PTR_TYPE,   // *unsafe.Pointer
    AST_PTR_ARITHMETIC,    // pointer arithmetic expressions
    AST_PTR_DEREF,         // @ptr (explicit dereference)
    AST_ADDR_OF,           // &var (address-of)
    AST_PORT_IO,           // port I/O operations
    AST_MMIO_ACCESS,       // memory-mapped I/O access
    AST_EXTERN_DECL,       // extern function declaration
    AST_ATTRIBUTE,         // @attribute
    AST_VOLATILE_EXPR,     // volatile memory access
    AST_PARALLEL_FOR,      // parallel for loop
    AST_PARALLEL_REDUCE,   // parallel reduction
    AST_BARRIER_CALL,      // barrier synchronization
    AST_ATOMIC_EXPR,       // atomic operation
    AST_THREAD_LOCAL_DECL, // thread-local variable declaration
    AST_MATCH_EXPR,        // match expression
    AST_MATCH_CASE,        // match case clause
    AST_PATTERN,           // pattern in match case
    AST_GUARD_CONDITION,   // guard condition (if clause in pattern)
    AST_KERNEL_DECL,       // GPU kernel function declaration
    AST_KERNEL_LAUNCH,     // GPU kernel launch expression
    AST_GPU_MEMORY_ALLOC,  // GPU memory allocation
    AST_GPU_MEMORY_COPY,   // GPU memory copy operation
    AST_GPU_SYNC,          // GPU synchronization
    AST_GPU_INTRINSIC,     // GPU-specific intrinsic function
    
    // Contract Programming Support
    AST_CONTRACT_CLAUSE,   // generic contract clause
    AST_REQUIRES_CLAUSE,   // requires precondition
    AST_ENSURES_CLAUSE,    // ensures postcondition
    AST_INVARIANT_CLAUSE,  // loop invariant
    AST_ASSERT_STMT,       // assertion statement
    AST_ASSUME_STMT,       // assumption statement
    AST_CONTRACT_BLOCK,    // block of contract clauses
    
    // WebAssembly Support
    AST_WASM_EXPORT,       // WebAssembly export declaration
    AST_WASM_IMPORT,       // WebAssembly import declaration
    AST_WASM_MEMORY,       // WebAssembly linear memory declaration
    AST_WASM_TABLE,        // WebAssembly table declaration
    AST_WASM_GLOBAL,       // WebAssembly global variable
    AST_WASM_TYPE,         // WebAssembly type declaration
    AST_WASM_START,        // WebAssembly start function
    AST_WASM_ELEM,         // WebAssembly element segment
    AST_WASM_DATA,         // WebAssembly data segment
    AST_JS_INTEROP,        // JavaScript interop call
    AST_DOM_ACCESS,        // DOM API access
    
    AST_NODE_COUNT
} ASTNodeType;

// Channel patterns for Goo extensions
typedef enum {
    CHAN_PATTERN_BASIC,
    CHAN_PATTERN_PUB,
    CHAN_PATTERN_SUB,
    CHAN_PATTERN_REQ,
    CHAN_PATTERN_REP,
    CHAN_PATTERN_PUSH,
    CHAN_PATTERN_PULL
} ChannelPattern;

// Ownership kinds for Goo extensions
typedef enum {
    OWNERSHIP_NONE,
    OWNERSHIP_OWNED,
    OWNERSHIP_BORROWED,
    OWNERSHIP_SHARED
} OwnershipKind;

// Pattern matching types
typedef enum {
    PATTERN_LITERAL,        // literal pattern (e.g., 42, "hello", true)
    PATTERN_IDENTIFIER,     // variable binding pattern (e.g., x, name)
    PATTERN_WILDCARD,       // wildcard pattern (e.g., _)
    PATTERN_DESTRUCTURE,    // destructuring pattern (e.g., Person{Name: name})
    PATTERN_TYPE,           // type pattern (e.g., err: SomeError)
    PATTERN_OR              // alternative patterns (e.g., 1 | 2 | 3)
} PatternType;

// GPU memory types
typedef enum {
    GPU_MEMORY_GLOBAL,      // Global device memory
    GPU_MEMORY_SHARED,      // Shared memory (block-local)
    GPU_MEMORY_CONSTANT,    // Constant memory
    GPU_MEMORY_LOCAL,       // Local/private memory
    GPU_MEMORY_TEXTURE      // Texture memory
} GPUMemoryType;

// GPU execution contexts
typedef enum {
    GPU_CONTEXT_HOST,       // Host (CPU) execution
    GPU_CONTEXT_DEVICE,     // Device (GPU) execution
    GPU_CONTEXT_KERNEL      // Kernel function context
} GPUExecutionContext;

// GPU target architectures
typedef enum {
    GPU_TARGET_NVPTX,       // NVIDIA PTX
    GPU_TARGET_AMDGPU,      // AMD GPU
    GPU_TARGET_SPIRV,       // SPIR-V (Vulkan/OpenCL)
    GPU_TARGET_METAL        // Metal Shading Language
} GPUTargetArch;

// WebAssembly target environments
typedef enum {
    WASM_ENV_BROWSER,       // Browser environment
    WASM_ENV_NODE,          // Node.js environment
    WASM_ENV_WASI,          // WASI (WebAssembly System Interface)
    WASM_ENV_EMBEDDED       // Embedded/standalone runtime
} WasmEnvironment;

// WebAssembly value types
typedef enum {
    WASM_TYPE_I32,          // 32-bit integer
    WASM_TYPE_I64,          // 64-bit integer
    WASM_TYPE_F32,          // 32-bit float
    WASM_TYPE_F64,          // 64-bit float
    WASM_TYPE_V128,         // 128-bit vector (SIMD)
    WASM_TYPE_FUNCREF,      // Function reference
    WASM_TYPE_EXTERNREF     // External reference
} WasmValueType;

// JavaScript interop types
typedef enum {
    JS_INTEROP_CALL,        // Call JavaScript function
    JS_INTEROP_GET,         // Get JavaScript property
    JS_INTEROP_SET,         // Set JavaScript property
    JS_INTEROP_NEW,         // Create JavaScript object
    JS_INTEROP_TYPEOF       // Get typeof JavaScript value
} JSInteropType;

// Base AST node structure
struct ASTNode {
    ASTNodeType type;
    Position pos;
    Type* node_type;        // Type information (filled during type checking)
    struct ASTNode* next;   // For linked lists of nodes
};

// Program (root node)
typedef struct {
    ASTNode base;
    char* package_name;
    struct ASTNode* imports;    // List of import specs
    struct ASTNode* decls;      // List of declarations
} ProgramNode;

// Package declaration
typedef struct {
    ASTNode base;
    char* name;
} PackageDeclNode;

// Import specification
typedef struct {
    ASTNode base;
    char* path;
    char* alias;    // Optional alias
} ImportSpecNode;

// Function declaration
typedef struct {
    ASTNode base;
    char* name;
    struct ASTNode* params;     // Parameter list
    struct ASTNode* return_type; // Return type (can be error union)
    struct ASTNode* body;       // Function body
    int is_comptime;           // Goo extension: comptime function
    int is_unsafe;             // Goo extension: unsafe function
    // Method receiver (for methods)
    char* receiver_name;       // Receiver variable name (e.g., "c")
    struct ASTNode* receiver_type; // Receiver type (e.g., Counter or *Counter)
    // Named return parameters (Go feature)
    struct ASTNode* named_returns; // List of VarDeclNode for named returns
} FuncDeclNode;

// Variable declaration
typedef struct {
    ASTNode base;
    char** names;              // Variable names
    size_t name_count;
    struct ASTNode* type;      // Type specification
    struct ASTNode* values;    // Initial values
    OwnershipKind ownership;   // Goo extension: ownership qualifier
    int is_short_decl;         // True for := declarations
} VarDeclNode;

// Constant declaration
typedef struct {
    ASTNode base;
    char** names;
    size_t name_count;
    struct ASTNode* type;
    struct ASTNode* values;
    int is_comptime;           // Goo extension: compile-time constant
} ConstDeclNode;

// Type declaration
typedef struct {
    ASTNode base;
    char* name;
    struct ASTNode* type;
} TypeDeclNode;

// Concept declaration
typedef struct {
    ASTNode base;
    char* name;                    // Concept name
    struct ASTNode* type_params;   // Type parameters (linked list)
    struct ASTNode* requirements;  // Requirements block (contains constraints)
} ConceptDeclNode;

// Block statement
typedef struct {
    ASTNode base;
    struct ASTNode* statements; // List of statements
} BlockStmtNode;

// Expression statement
typedef struct {
    ASTNode base;
    struct ASTNode* expr;
} ExprStmtNode;

// If statement
typedef struct {
    ASTNode base;
    struct ASTNode* condition;
    struct ASTNode* then_stmt;
    struct ASTNode* else_stmt;  // Optional
} IfStmtNode;

// If-let statement for nullable unwrapping
typedef struct {
    ASTNode base;
    char* var_name;             // Variable name to bind the unwrapped value
    struct ASTNode* nullable_expr; // Expression that evaluates to nullable type
    struct ASTNode* then_stmt;  // Block to execute if not null
    struct ASTNode* else_stmt;  // Optional else block
} IfLetStmtNode;

// For statement
typedef struct {
    ASTNode base;
    struct ASTNode* init;       // Optional initialization
    struct ASTNode* condition;  // Optional condition
    struct ASTNode* post;       // Optional post statement
    struct ASTNode* body;
} ForStmtNode;

// Range statement (for i, v := range expr)
typedef struct {
    ASTNode base;
    char* index_var;           // Index variable name (can be "_" for unused)
    char* value_var;           // Value variable name (can be "_" or NULL for index-only)
    struct ASTNode* range_expr; // Expression to range over (array, slice, etc.)
    struct ASTNode* body;      // Loop body
} RangeStmtNode;

// Switch statement
typedef struct {
    ASTNode base;
    struct ASTNode* tag;        // Optional tag expression to switch on (NULL for tagless switch)
    struct ASTNode* cases;      // List of case clauses
} SwitchStmtNode;

// Case clause for switch statement
typedef struct {
    ASTNode base;
    struct ASTNode* values;     // List of expressions to match (NULL for default case)
    struct ASTNode* body;       // Case body (list of statements)
    int is_default;             // 1 if this is the default case
} CaseClauseNode;

// Return statement
typedef struct {
    ASTNode base;
    struct ASTNode* values;     // Return values
} ReturnStmtNode;

// Go statement
typedef struct {
    ASTNode base;
    struct ASTNode* call;       // Function call to execute as goroutine
} GoStmtNode;

// Select statement
typedef struct {
    ASTNode base;
    struct ASTNode* cases;      // List of case clauses
} SelectStmtNode;

// Select case clause
typedef struct {
    ASTNode base;
    struct ASTNode* comm;       // Communication operation (send/recv)
    struct ASTNode* body;       // Case body
} SelectCaseNode;

// Defer statement  
typedef struct {
    ASTNode base;
    struct ASTNode* call;       // Function call to defer
} DeferStmtNode;

// Unsafe statement
typedef struct {
    ASTNode base;
    struct ASTNode* body;       // Block statement containing unsafe operations
} UnsafeStmtNode;

// Inline assembly statement
typedef struct {
    ASTNode base;
    char* assembly_code;        // Assembly code string
    struct ASTNode* outputs;    // Output operands (optional)
    struct ASTNode* inputs;     // Input operands (optional)
    char** clobbers;            // Clobbered registers (optional)
    size_t clobber_count;
} AsmStmtNode;

// Identifier
typedef struct {
    ASTNode base;
    char* name;
} IdentifierNode;

// Literal
typedef struct {
    ASTNode base;
    TokenType literal_type;
    char* value;
} LiteralNode;

// Composite literal (e.g., Person{age: 30})
typedef struct {
    ASTNode base;
    struct ASTNode* type;           // Type being initialized
    char** field_names;             // Field names
    struct ASTNode** field_values;  // Field value expressions
    size_t field_count;             // Number of fields
} CompositeLitNode;

// Binary expression
typedef struct {
    ASTNode base;
    struct ASTNode* left;
    TokenType operator;
    struct ASTNode* right;
} BinaryExprNode;

// Unary expression
typedef struct {
    ASTNode base;
    TokenType operator;
    struct ASTNode* operand;
} UnaryExprNode;

// Postfix expression (for operators like forced unwrap !)
typedef struct {
    ASTNode base;
    struct ASTNode* operand;
    TokenType operator;
} PostfixExprNode;

// Function call
typedef struct {
    ASTNode base;
    struct ASTNode* function;
    struct ASTNode* args;       // Argument list
} CallExprNode;

// Index expression
typedef struct {
    ASTNode base;
    struct ASTNode* expr;
    struct ASTNode* index;
} IndexExprNode;

// Selector expression (dot notation)
typedef struct {
    ASTNode base;
    struct ASTNode* expr;
    char* selector;
} SelectorExprNode;

// Basic type
typedef struct {
    ASTNode base;
    char* name;
} BasicTypeNode;

// Array type
typedef struct {
    ASTNode base;
    struct ASTNode* length;     // Array length expression
    struct ASTNode* element_type;
} ArrayTypeNode;

// Slice type
typedef struct {
    ASTNode base;
    struct ASTNode* element_type;
} SliceTypeNode;

// Map type
typedef struct {
    ASTNode base;
    struct ASTNode* key_type;
    struct ASTNode* value_type;
} MapTypeNode;

// Struct type
typedef struct {
    ASTNode base;
    char** field_names;
    struct ASTNode** field_types;
    size_t field_count;
} StructTypeNode;

// Tuple type (for multiple return values)
typedef struct {
    ASTNode base;
    struct ASTNode** element_types;
    size_t element_count;
} TupleTypeNode;

// Channel type
typedef struct {
    ASTNode base;
    struct ASTNode* element_type;
    ChannelPattern pattern;     // Goo extension: channel pattern
    char* endpoint;             // Goo extension: network endpoint
} ChanTypeNode;

// Function type
typedef struct {
    ASTNode base;
    struct ASTNode* params;
    struct ASTNode* return_type;
} FuncTypeNode;

// Pointer type
typedef struct {
    ASTNode base;
    struct ASTNode* element_type;
} PointerTypeNode;

// Reference type (for borrowing)
typedef struct {
    ASTNode base;
    struct ASTNode* element_type;
    int is_mutable;  // mut &T vs &T
} ReferenceTypeNode;

// Unsafe pointer type (*unsafe.Pointer)
typedef struct {
    ASTNode base;
    struct ASTNode* element_type;  // Optional element type
} UnsafePtrTypeNode;

// Goo Extensions

// Error union type (!T)
typedef struct {
    ASTNode base;
    struct ASTNode* value_type;
    struct ASTNode* error_type; // Optional, defaults to standard error
} ErrorUnionTypeNode;

// Nullable type (?T)
typedef struct {
    ASTNode base;
    struct ASTNode* base_type;
} NullableTypeNode;

// Try expression
typedef struct {
    ASTNode base;
    struct ASTNode* expr;
} TryExprNode;

// Catch expression
typedef struct {
    ASTNode base;
    struct ASTNode* expr;
    char* error_var;            // Error variable name
    struct ASTNode* catch_body;
} CatchExprNode;

// Compile-time block
typedef struct {
    ASTNode base;
    struct ASTNode* body;
} ComptimeBlockNode;

// Pointer arithmetic expression
typedef struct {
    ASTNode base;
    struct ASTNode* pointer;    // Base pointer expression
    struct ASTNode* offset;     // Offset expression  
    TokenType operation;        // +, -, etc.
} PtrArithmeticNode;

// Pointer dereference (@ptr)
typedef struct {
    ASTNode base;
    struct ASTNode* pointer;    // Pointer to dereference
} PtrDerefNode;

// Address-of (&var)
typedef struct {
    ASTNode base;
    struct ASTNode* operand;    // Variable/expression to take address of
} AddrOfNode;

// Port I/O operation
typedef struct {
    ASTNode base;
    struct ASTNode* port;       // Port number
    struct ASTNode* value;      // Value to write (NULL for read)
    int is_input;               // 1 for input (read), 0 for output (write)
    int size;                   // 1, 2, 4 bytes
} PortIONode;

// Memory-mapped I/O access
typedef struct {
    ASTNode base;
    struct ASTNode* address;    // Memory address
    struct ASTNode* value;      // Value to write (NULL for read)
    int is_volatile;            // 1 if volatile access
    int size;                   // Access size in bytes
} MMIOAccessNode;

// External function declaration
typedef struct {
    ASTNode base;
    char* name;                 // Function name
    char* abi;                  // ABI specification (e.g., "C", "stdcall")
    struct ASTNode* params;     // Parameter list
    struct ASTNode* return_type; // Return type
    char* library;              // Optional library name
} ExternDeclNode;

// Attribute node
typedef struct {
    ASTNode base;
    char* name;                 // Attribute name
    struct ASTNode* args;       // Optional attribute arguments
} AttributeNode;

// Volatile expression
typedef struct {
    ASTNode base;
    struct ASTNode* expr;       // Expression to make volatile
} VolatileExprNode;

// Parallel for loop
typedef struct {
    ASTNode base;
    struct ASTNode* init;       // Loop initialization
    struct ASTNode* condition;  // Loop condition
    struct ASTNode* increment;  // Loop increment
    struct ASTNode* body;       // Loop body
    char* schedule_type;        // "static", "dynamic", "guided", etc.
    int chunk_size;             // Work chunk size
} ParallelForNode;

// Parallel reduction
typedef struct {
    ASTNode base;
    struct ASTNode* array;      // Array to reduce
    struct ASTNode* init_value; // Initial value
    struct ASTNode* reduction_func; // Reduction function
    char* operation;            // "sum", "min", "max", "custom"
} ParallelReduceNode;

// Barrier synchronization call
typedef struct {
    ASTNode base;
    char* barrier_name;         // Optional barrier name
} BarrierCallNode;

// Atomic expression
typedef struct {
    ASTNode base;
    struct ASTNode* expr;       // Expression to make atomic
    char* operation;            // "add", "sub", "cas", "swap", etc.
    struct ASTNode* operand;    // Operand for atomic operation
} AtomicExprNode;

// Thread-local variable declaration
typedef struct {
    ASTNode base;
    char* name;                 // Variable name
    struct ASTNode* type;       // Variable type
    struct ASTNode* init_value; // Optional initial value
} ThreadLocalDeclNode;

// Match expression
typedef struct {
    ASTNode base;
    struct ASTNode* expr;       // Expression to match against
    struct ASTNode* cases;      // List of match cases
} MatchExprNode;

// Match case clause
typedef struct {
    ASTNode base;
    struct ASTNode* pattern;    // Pattern to match
    struct ASTNode* guard;      // Optional guard condition
    struct ASTNode* body;       // Case body statements
} MatchCaseNode;

// Pattern node
typedef struct {
    ASTNode base;
    PatternType pattern_type;   // Type of pattern
    union {
        struct {
            struct ASTNode* literal;    // For literal patterns
        } literal;
        struct {
            char* name;                 // For identifier patterns
            struct ASTNode* type;       // Optional type annotation
        } identifier;
        struct {
            char* type_name;            // For type patterns
            struct ASTNode* fields;     // For destructuring patterns
        } destructure;
        struct {
            struct ASTNode* patterns;   // For OR patterns (list of alternatives)
        } or_pattern;
    } data;
} PatternNode;

// Guard condition (if clause in pattern)
typedef struct {
    ASTNode base;
    struct ASTNode* condition;  // Boolean expression
} GuardConditionNode;

// GPU kernel function declaration
typedef struct {
    ASTNode base;
    char* name;                     // Kernel function name
    struct ASTNode* params;         // Parameter list
    struct ASTNode* return_type;    // Return type (usually void for kernels)
    struct ASTNode* body;           // Kernel body
    GPUTargetArch target_arch;      // Target GPU architecture
    int max_threads_per_block;      // Maximum threads per block
    int shared_memory_size;         // Shared memory requirements
    int is_inline;                  // Inline kernel flag
} KernelDeclNode;

// GPU kernel launch expression
typedef struct {
    ASTNode base;
    struct ASTNode* kernel_func;    // Kernel function to launch
    struct ASTNode* grid_dim;       // Grid dimensions (1D, 2D, or 3D)
    struct ASTNode* block_dim;      // Block dimensions (1D, 2D, or 3D)
    struct ASTNode* args;           // Kernel arguments
    struct ASTNode* shared_mem_size; // Dynamic shared memory size
    struct ASTNode* stream;         // CUDA stream (optional)
} KernelLaunchNode;

// GPU memory allocation
typedef struct {
    ASTNode base;
    struct ASTNode* size;           // Size in bytes
    struct ASTNode* element_type;   // Type of elements
    GPUMemoryType memory_type;      // Type of GPU memory
    int is_managed;                 // Unified memory flag
    int alignment;                  // Memory alignment requirements
} GPUMemoryAllocNode;

// GPU memory copy operation
typedef struct {
    ASTNode base;
    struct ASTNode* dest;           // Destination pointer
    struct ASTNode* src;            // Source pointer
    struct ASTNode* size;           // Size in bytes
    int direction;                  // 0=HostToDevice, 1=DeviceToHost, 2=DeviceToDevice
    int is_async;                   // Async copy flag
    struct ASTNode* stream;         // CUDA stream (optional)
} GPUMemoryCopyNode;

// GPU synchronization
typedef struct {
    ASTNode base;
    int sync_type;                  // 0=DeviceSync, 1=StreamSync, 2=EventSync
    struct ASTNode* stream;         // Stream to sync (optional)
    struct ASTNode* event;          // Event to sync (optional)
} GPUSyncNode;

// GPU intrinsic function call
typedef struct {
    ASTNode base;
    char* intrinsic_name;           // Name of intrinsic (e.g., "blockIdx", "threadIdx")
    struct ASTNode* args;           // Arguments to intrinsic
    GPUExecutionContext context;   // Where this intrinsic is valid
} GPUIntrinsicNode;

// WebAssembly export declaration
typedef struct {
    ASTNode base;
    char* export_name;              // Name to export
    struct ASTNode* item;           // Item to export (function, memory, etc.)
    char* export_type;              // "func", "memory", "table", "global"
} WasmExportNode;

// WebAssembly import declaration
typedef struct {
    ASTNode base;
    char* module_name;              // Module to import from
    char* import_name;              // Name of imported item
    char* local_name;               // Local name to bind to
    char* import_type;              // "func", "memory", "table", "global"
    struct ASTNode* signature;     // Type signature for imports
} WasmImportNode;

// WebAssembly linear memory declaration
typedef struct {
    ASTNode base;
    struct ASTNode* min_pages;      // Minimum number of pages
    struct ASTNode* max_pages;      // Maximum number of pages (optional)
    int is_shared;                  // Shared memory flag
    int is_exported;                // Export flag
    char* export_name;              // Export name (if exported)
} WasmMemoryNode;

// WebAssembly table declaration
typedef struct {
    ASTNode base;
    WasmValueType element_type;     // Element type (funcref, externref)
    struct ASTNode* min_size;       // Minimum size
    struct ASTNode* max_size;       // Maximum size (optional)
    int is_exported;                // Export flag
    char* export_name;              // Export name (if exported)
} WasmTableNode;

// WebAssembly global variable
typedef struct {
    ASTNode base;
    char* name;                     // Global variable name
    WasmValueType value_type;       // Value type
    int is_mutable;                 // Mutability flag
    struct ASTNode* init_value;     // Initial value
    int is_exported;                // Export flag
    char* export_name;              // Export name (if exported)
} WasmGlobalNode;

// WebAssembly type declaration
typedef struct {
    ASTNode base;
    char* name;                     // Type name
    struct ASTNode* params;         // Parameter types
    struct ASTNode* results;        // Result types
} WasmTypeNode;

// WebAssembly start function
typedef struct {
    ASTNode base;
    struct ASTNode* function;       // Function to call at start
} WasmStartNode;

// WebAssembly element segment
typedef struct {
    ASTNode base;
    struct ASTNode* table_index;    // Table index
    struct ASTNode* offset;         // Offset expression
    struct ASTNode* elements;       // List of elements
} WasmElemNode;

// WebAssembly data segment
typedef struct {
    ASTNode base;
    struct ASTNode* memory_index;   // Memory index
    struct ASTNode* offset;         // Offset expression
    struct ASTNode* data;           // Data bytes
} WasmDataNode;

// JavaScript interop call
typedef struct {
    ASTNode base;
    JSInteropType interop_type;     // Type of interop operation
    char* object_name;              // JavaScript object name
    char* property_name;            // Property/method name (optional)
    struct ASTNode* args;           // Arguments for calls
    WasmEnvironment target_env;     // Target environment
} JSInteropNode;

// DOM API access
typedef struct {
    ASTNode base;
    char* api_name;                 // DOM API name (e.g., "document", "window")
    char* method_name;              // Method or property name
    struct ASTNode* args;           // Arguments
    int is_property;                // True for property access, false for method call
} DOMAccessNode;

// =============================================================================
// Contract Programming AST Nodes
// =============================================================================

// Contract clause (generic base for all contract types)
typedef struct {
    ASTNode base;
    struct ASTNode* condition;      // Boolean expression
    struct ASTNode* message;        // Optional error message
    char* description;              // Human-readable description
} ContractClauseNode;

// Requires clause (precondition)
typedef struct {
    ASTNode base;
    struct ASTNode* condition;      // Boolean expression
    struct ASTNode* message;        // Optional error message
    char* description;              // Human-readable description
} RequiresClauseNode;

// Ensures clause (postcondition)
typedef struct {
    ASTNode base;
    struct ASTNode* condition;      // Boolean expression
    struct ASTNode* return_var;     // Variable name for return value (optional)
    struct ASTNode* message;        // Optional error message
    char* description;              // Human-readable description
} EnsuresClauseNode;

// Invariant clause (loop invariant)
typedef struct {
    ASTNode base;
    struct ASTNode* condition;      // Boolean expression
    struct ASTNode* message;        // Optional error message
    char* description;              // Human-readable description
} InvariantClauseNode;

// Assert statement
typedef struct {
    ASTNode base;
    struct ASTNode* condition;      // Boolean expression
    struct ASTNode* message;        // Optional error message
    int is_debug_only;              // Only checked in debug builds
} AssertStmtNode;

// Assume statement (for optimization)
typedef struct {
    ASTNode base;
    struct ASTNode* condition;      // Boolean expression
    char* hint;                     // Optimization hint
} AssumeStmtNode;

// Contract block (group of contract clauses)
typedef struct {
    ASTNode base;
    struct ASTNode* clauses;        // List of contract clauses
} ContractBlockNode;

// =============================================================================
// Function declarations for AST manipulation
// =============================================================================
ASTNode* ast_node_new(ASTNodeType type, Position pos);
void ast_node_free(ASTNode* node);
ASTNode* ast_node_copy(const ASTNode* node);

// Contract constructors
ContractClauseNode* ast_contract_clause_new(ASTNode* condition, const char* description, Position pos);
RequiresClauseNode* ast_requires_clause_new(ASTNode* condition, const char* description, Position pos);
EnsuresClauseNode* ast_ensures_clause_new(ASTNode* condition, const char* return_var, const char* description, Position pos);
InvariantClauseNode* ast_invariant_clause_new(ASTNode* condition, const char* description, Position pos);
AssertStmtNode* ast_assert_stmt_new(ASTNode* condition, ASTNode* message, int is_debug_only, Position pos);
AssumeStmtNode* ast_assume_stmt_new(ASTNode* condition, const char* hint, Position pos);
ContractBlockNode* ast_contract_block_new(ASTNode* clauses, Position pos);

// Specific node constructors
ProgramNode* ast_program_new(Position pos);
PackageDeclNode* ast_package_decl_new(const char* name, Position pos);
ImportSpecNode* ast_import_spec_new(const char* path, const char* alias, Position pos);
FuncDeclNode* ast_func_decl_new(const char* name, Position pos);
ConceptDeclNode* ast_concept_decl_new(const char* name, Position pos);
VarDeclNode* ast_var_decl_new(Position pos);
IdentifierNode* ast_identifier_new(const char* name, Position pos);
LiteralNode* ast_literal_new(TokenType type, const char* value, Position pos);
BinaryExprNode* ast_binary_expr_new(ASTNode* left, TokenType op, ASTNode* right, Position pos);
UnaryExprNode* ast_unary_expr_new(TokenType op, ASTNode* operand, Position pos);
PostfixExprNode* ast_postfix_expr_new(ASTNode* operand, TokenType op, Position pos);
BlockStmtNode* ast_block_stmt_new(Position pos);
IfLetStmtNode* ast_if_let_stmt_new(const char* var_name, ASTNode* nullable_expr, ASTNode* then_stmt, ASTNode* else_stmt, Position pos);
GoStmtNode* ast_go_stmt_new(ASTNode* call, Position pos);
SelectStmtNode* ast_select_stmt_new(Position pos);
SelectCaseNode* ast_select_case_new(ASTNode* comm, ASTNode* body, Position pos);
DeferStmtNode* ast_defer_stmt_new(ASTNode* call, Position pos);
UnsafeStmtNode* ast_unsafe_stmt_new(ASTNode* body, Position pos);
AsmStmtNode* ast_asm_stmt_new(const char* assembly_code, Position pos);
IfStmtNode* ast_if_stmt_new(ASTNode* condition, ASTNode* then_stmt, ASTNode* else_stmt, Position pos);
ForStmtNode* ast_for_stmt_new(ASTNode* init, ASTNode* condition, ASTNode* post, ASTNode* body, Position pos);
RangeStmtNode* ast_range_stmt_new(const char* index_var, const char* value_var, ASTNode* range_expr, ASTNode* body, Position pos);
SwitchStmtNode* ast_switch_stmt_new(ASTNode* tag, ASTNode* cases, Position pos);
CaseClauseNode* ast_case_clause_new(ASTNode* values, ASTNode* body, int is_default, Position pos);

// Goo extension constructors
ErrorUnionTypeNode* ast_error_union_type_new(ASTNode* value_type, Position pos);
NullableTypeNode* ast_nullable_type_new(ASTNode* base_type, Position pos);
TupleTypeNode* ast_tuple_type_new(ASTNode** element_types, size_t element_count, Position pos);
TryExprNode* ast_try_expr_new(ASTNode* expr, Position pos);
CatchExprNode* ast_catch_expr_new(ASTNode* expr, const char* error_var, ASTNode* catch_body, Position pos);
ComptimeBlockNode* ast_comptime_block_new(ASTNode* body, Position pos);
ChanTypeNode* ast_chan_type_new(ASTNode* element_type, ChannelPattern pattern, Position pos);
ReferenceTypeNode* ast_reference_type_new(ASTNode* element_type, int is_mutable, Position pos);
UnsafePtrTypeNode* ast_unsafe_ptr_type_new(ASTNode* element_type, Position pos);
PtrArithmeticNode* ast_ptr_arithmetic_new(ASTNode* pointer, TokenType operation, ASTNode* offset, Position pos);
PtrDerefNode* ast_ptr_deref_new(ASTNode* pointer, Position pos);
AddrOfNode* ast_addr_of_new(ASTNode* operand, Position pos);
PortIONode* ast_port_io_new(ASTNode* port, ASTNode* value, int is_input, int size, Position pos);
MMIOAccessNode* ast_mmio_access_new(ASTNode* address, ASTNode* value, int is_volatile, int size, Position pos);
ExternDeclNode* ast_extern_decl_new(const char* name, const char* abi, ASTNode* params, ASTNode* return_type, const char* library, Position pos);
AttributeNode* ast_attribute_new(const char* name, ASTNode* args, Position pos);
VolatileExprNode* ast_volatile_expr_new(ASTNode* expr, Position pos);
ParallelForNode* ast_parallel_for_new(ASTNode* init, ASTNode* condition, ASTNode* increment, ASTNode* body, const char* schedule_type, int chunk_size, Position pos);
ParallelReduceNode* ast_parallel_reduce_new(ASTNode* array, ASTNode* init_value, ASTNode* reduction_func, const char* operation, Position pos);
BarrierCallNode* ast_barrier_call_new(const char* barrier_name, Position pos);
AtomicExprNode* ast_atomic_expr_new(ASTNode* expr, const char* operation, ASTNode* operand, Position pos);
ThreadLocalDeclNode* ast_thread_local_decl_new(const char* name, ASTNode* type, ASTNode* init_value, Position pos);
MatchExprNode* ast_match_expr_new(ASTNode* expr, ASTNode* cases, Position pos);
MatchCaseNode* ast_match_case_new(ASTNode* pattern, ASTNode* guard, ASTNode* body, Position pos);
PatternNode* ast_pattern_new(PatternType pattern_type, Position pos);
GuardConditionNode* ast_guard_condition_new(ASTNode* condition, Position pos);
KernelDeclNode* ast_kernel_decl_new(const char* name, ASTNode* params, ASTNode* return_type, ASTNode* body, GPUTargetArch target_arch, Position pos);
KernelLaunchNode* ast_kernel_launch_new(ASTNode* kernel_func, ASTNode* grid_dim, ASTNode* block_dim, ASTNode* args, Position pos);
GPUMemoryAllocNode* ast_gpu_memory_alloc_new(ASTNode* size, ASTNode* element_type, GPUMemoryType memory_type, Position pos);
GPUMemoryCopyNode* ast_gpu_memory_copy_new(ASTNode* dest, ASTNode* src, ASTNode* size, int direction, Position pos);
GPUSyncNode* ast_gpu_sync_new(int sync_type, ASTNode* stream, ASTNode* event, Position pos);
GPUIntrinsicNode* ast_gpu_intrinsic_new(const char* intrinsic_name, ASTNode* args, GPUExecutionContext context, Position pos);

// WebAssembly constructors
WasmExportNode* ast_wasm_export_new(const char* export_name, ASTNode* item, const char* export_type, Position pos);
WasmImportNode* ast_wasm_import_new(const char* module_name, const char* import_name, const char* local_name, const char* import_type, ASTNode* signature, Position pos);
WasmMemoryNode* ast_wasm_memory_new(ASTNode* min_pages, ASTNode* max_pages, int is_shared, Position pos);
WasmTableNode* ast_wasm_table_new(WasmValueType element_type, ASTNode* min_size, ASTNode* max_size, Position pos);
WasmGlobalNode* ast_wasm_global_new(const char* name, WasmValueType value_type, int is_mutable, ASTNode* init_value, Position pos);
WasmTypeNode* ast_wasm_type_new(const char* name, ASTNode* params, ASTNode* results, Position pos);
WasmStartNode* ast_wasm_start_new(ASTNode* function, Position pos);
WasmElemNode* ast_wasm_elem_new(ASTNode* table_index, ASTNode* offset, ASTNode* elements, Position pos);
WasmDataNode* ast_wasm_data_new(ASTNode* memory_index, ASTNode* offset, ASTNode* data, Position pos);
JSInteropNode* ast_js_interop_new(JSInteropType interop_type, const char* object_name, const char* property_name, ASTNode* args, WasmEnvironment target_env, Position pos);
DOMAccessNode* ast_dom_access_new(const char* api_name, const char* method_name, ASTNode* args, int is_property, Position pos);

// Utility functions
void ast_add_child(ASTNode* parent, ASTNode* child);
const char* ast_node_type_string(ASTNodeType type);
void ast_print(const ASTNode* node, int indent);

#endif // AST_H