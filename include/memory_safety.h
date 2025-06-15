#ifndef MEMORY_SAFETY_H
#define MEMORY_SAFETY_H

#include "ast.h"
#include "types.h"
#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct FlowSensitiveAnalyzer FlowSensitiveAnalyzer;
typedef struct OwnershipState OwnershipState;
typedef struct ControlFlowGraph ControlFlowGraph;
typedef struct BasicBlock BasicBlock;
typedef struct ValueState ValueState;
typedef struct ReferenceManager ReferenceManager;
typedef struct LifetimeScope LifetimeScope;
typedef struct BorrowTracker BorrowTracker;

// Memory safety analysis kinds
typedef enum {
    MEMORY_ANALYSIS_OWNERSHIP,
    MEMORY_ANALYSIS_LIFETIME,
    MEMORY_ANALYSIS_ESCAPE,
    MEMORY_ANALYSIS_RESOURCE,
    MEMORY_ANALYSIS_LAYOUT
} MemoryAnalysisKind;

// Value state in flow-sensitive analysis
typedef enum {
    VALUE_STATE_UNINITIALIZED,
    VALUE_STATE_INITIALIZED,
    VALUE_STATE_MOVED,
    VALUE_STATE_BORROWED_IMMUTABLE,
    VALUE_STATE_BORROWED_MUTABLE,
    VALUE_STATE_PARTIAL_MOVE,      // Some fields moved, others intact
    VALUE_STATE_CONDITIONALLY_MOVED // Moved in some code paths
} ValueStateKind;

// Move/copy decision
typedef enum {
    TRANSFER_KIND_COPY,
    TRANSFER_KIND_MOVE,
    TRANSFER_KIND_BORROW_IMMUTABLE,
    TRANSFER_KIND_BORROW_MUTABLE,
    TRANSFER_KIND_AUTO_INFERRED    // Let analyzer decide
} TransferKind;

// Escape analysis results
typedef enum {
    ESCAPE_NONE,           // Value doesn't escape current scope
    ESCAPE_FUNCTION,       // Value escapes through function return
    ESCAPE_CLOSURE,        // Value captured by closure
    ESCAPE_GLOBAL,         // Value assigned to global
    ESCAPE_THREAD,         // Value sent to another thread
    ESCAPE_UNKNOWN         // Conservative assumption
} EscapeKind;

// Allocation strategy based on escape analysis
typedef enum {
    ALLOC_STRATEGY_STACK,          // Stack allocation (no escape)
    ALLOC_STRATEGY_HEAP,           // Heap allocation (escapes)
    ALLOC_STRATEGY_REGION,         // Region-based allocation
    ALLOC_STRATEGY_THREAD_LOCAL,   // Thread-local allocation
    ALLOC_STRATEGY_GLOBAL,         // Global/static allocation
    ALLOC_STRATEGY_DEFER           // Defer decision to runtime
} AllocationStrategy;

// Object lifetime classification
typedef enum {
    LIFETIME_IMMEDIATE,            // Lives only within expression
    LIFETIME_LOCAL,               // Lives within current function
    LIFETIME_PARAMETER,           // Passed as function parameter
    LIFETIME_RETURN,              // Returned from function
    LIFETIME_CLOSURE_CAPTURE,     // Captured by closure
    LIFETIME_GLOBAL,              // Global lifetime
    LIFETIME_UNKNOWN              // Cannot determine
} ObjectLifetime;

// Escape context information
typedef struct EscapeContext {
    ASTNode* escape_site;         // Where the escape occurs
    ASTNode* target_function;     // Function where object escapes to
    EscapeKind escape_kind;       // Type of escape
    ObjectLifetime lifetime;      // Object lifetime classification
    AllocationStrategy strategy;  // Recommended allocation strategy
    size_t call_depth;           // Call stack depth when escape occurs
    int is_conditional;          // Escape happens conditionally
    double escape_probability;   // Probability of escape (0.0-1.0)
    
    struct EscapeContext* next;  // For linked lists
} EscapeContext;

// Function signature analysis for escape
typedef struct FunctionEscapeInfo {
    char* function_name;         // Function name
    ASTNode* function_node;      // Function AST node
    
    // Parameter escape information
    EscapeKind* param_escape;    // Escape status for each parameter
    AllocationStrategy* param_alloc_strategy; // Allocation strategy per param
    size_t param_count;
    
    // Return value escape information
    EscapeKind return_escape;    // How return value escapes
    ObjectLifetime return_lifetime; // Return value lifetime
    
    // Call graph information
    struct FunctionEscapeInfo** callees; // Functions this calls
    size_t callee_count;
    struct FunctionEscapeInfo** callers; // Functions that call this
    size_t caller_count;
    
    // Analysis metadata
    int is_analyzed;             // Has been analyzed
    int is_recursive;            // Is part of recursive call chain
    int has_side_effects;        // Modifies global state
    size_t complexity_score;     // Analysis complexity estimate
    
    struct FunctionEscapeInfo* next; // For linked lists
} FunctionEscapeInfo;

// Interprocedural escape analyzer
typedef struct EscapeAnalyzer {
    ReferenceManager* reference_manager; // Parent reference manager
    
    // Function analysis
    FunctionEscapeInfo** functions;    // All analyzed functions
    size_t function_count;
    size_t function_capacity;
    
    // Call graph
    ASTNode** call_sites;              // All function call sites
    size_t call_site_count;
    size_t call_site_capacity;
    
    // Escape contexts
    EscapeContext** escape_contexts;   // All escape contexts found
    size_t context_count;
    size_t context_capacity;
    
    // Analysis configuration
    int enable_region_analysis;        // Enable region-based allocation
    int aggressive_stack_allocation;   // Prefer stack over heap
    int optimize_for_size;             // Optimize for memory usage
    int enable_probabilistic_analysis; // Use escape probabilities
    
    // Analysis statistics
    size_t functions_analyzed;
    size_t escapes_detected;
    size_t stack_allocations_recommended;
    size_t heap_allocations_required;
    size_t region_allocations_created;
    
    // Error tracking
    int error_count;
    int warning_count;
} EscapeAnalyzer;

// Reference management types
typedef enum {
    REFERENCE_KIND_SHARED,     // Immutable reference (&T)
    REFERENCE_KIND_MUTABLE,    // Mutable reference (&mut T)
    REFERENCE_KIND_WEAK,       // Weak reference (doesn't keep target alive)
    REFERENCE_KIND_SLICE,      // Array/slice reference (&[T])
    REFERENCE_KIND_STRING,     // String slice reference (&str)
    REFERENCE_KIND_CLOSURE     // Closure capture reference
} ReferenceKind;

// Reference validity tracking
typedef enum {
    REFERENCE_VALID,           // Reference is currently valid
    REFERENCE_INVALIDATED,     // Reference has been invalidated
    REFERENCE_CONDITIONALLY_VALID, // Valid in some code paths
    REFERENCE_UNKNOWN          // Validity cannot be determined
} ReferenceValidity;

// Lifetime scope types
typedef enum {
    LIFETIME_SCOPE_FUNCTION,   // Function-scoped lifetime
    LIFETIME_SCOPE_BLOCK,      // Block-scoped lifetime  
    LIFETIME_SCOPE_LOOP,       // Loop iteration lifetime
    LIFETIME_SCOPE_CONDITIONAL,// If/else branch lifetime
    LIFETIME_SCOPE_GLOBAL,     // Global/static lifetime
    LIFETIME_SCOPE_INFERRED    // Compiler-inferred lifetime
} LifetimeScopeKind;

// Smart pointer types for automatic management
typedef enum {
    SMART_PTR_BOX,            // Unique ownership (Box<T>)
    SMART_PTR_RC,             // Reference counted (Rc<T>)
    SMART_PTR_ARC,            // Atomic reference counted (Arc<T>)
    SMART_PTR_WEAK,           // Weak reference (Weak<T>)
    SMART_PTR_CELL,           // Interior mutability (Cell<T>)
    SMART_PTR_REFCELL         // Runtime borrow checking (RefCell<T>)
} SmartPointerKind;

// Value state tracking
struct ValueState {
    char* name;                    // Variable name
    ValueStateKind state;          // Current state
    TransferKind recommended_transfer; // Recommended transfer operation
    EscapeKind escape_status;      // Where/how value escapes
    size_t last_use_position;      // Last use in program order
    size_t definition_position;    // Where value was defined
    int ref_count;                 // Number of outstanding references
    int is_closure_capture;        // Captured by closure
    
    // Path-sensitive information
    struct ValueState** path_states;  // States in different paths
    size_t path_count;
    
    struct ValueState* next;       // For linked lists
};

// Basic block in control flow graph
struct BasicBlock {
    size_t id;
    ASTNode** statements;          // Statements in this block
    size_t statement_count;
    
    struct BasicBlock** predecessors;
    size_t predecessor_count;
    struct BasicBlock** successors;
    size_t successor_count;
    
    OwnershipState* entry_state;   // Ownership state at block entry
    OwnershipState* exit_state;    // Ownership state at block exit
    
    int visited;                   // For traversal algorithms
    int dominates_exit;            // For lifetime analysis
};

// Control flow graph
struct ControlFlowGraph {
    BasicBlock** blocks;
    size_t block_count;
    size_t block_capacity;
    
    BasicBlock* entry_block;
    BasicBlock* exit_block;
    
    // Dominator tree information
    BasicBlock** dominators;       // dominators[i] = immediate dominator of block i
    BasicBlock*** dominated_sets;  // dominated_sets[i] = blocks dominated by block i
    
    // Loop information
    BasicBlock*** loops;           // Natural loops in the function
    size_t loop_count;
};

// Ownership state at a program point
struct OwnershipState {
    ValueState** values;           // All tracked values
    size_t value_count;
    size_t value_capacity;
    
    // Statistics for optimization decisions
    size_t copy_operations;
    size_t move_operations;
    size_t borrow_operations;
    
    struct OwnershipState* parent; // For nested scopes
};

// Flow-sensitive ownership analyzer
struct FlowSensitiveAnalyzer {
    TypeChecker* type_checker;
    ControlFlowGraph* cfg;
    
    // Analysis state
    OwnershipState** block_states; // State for each basic block
    int* worklist;                 // Blocks needing reanalysis
    size_t worklist_size;
    
    // Analysis configuration
    int aggressive_optimization;   // Enable aggressive move inference
    int safety_first;             // Prefer safety over performance
    int debug_mode;               // Generate debug information
    
    // Statistics and metrics
    size_t total_values_analyzed;
    size_t moves_inferred;
    size_t copies_required;
    size_t borrows_inferred;
    size_t unsafe_patterns_found;
    
    // Error tracking
    int error_count;
    int warning_count;
};

// Reference tracking information
typedef struct ReferenceInfo {
    char* name;                    // Reference variable name
    char* target_name;             // Target variable name
    ReferenceKind kind;            // Type of reference
    ReferenceValidity validity;    // Current validity status
    size_t creation_position;      // Where reference was created
    size_t last_use_position;      // Last time reference was used
    LifetimeScope* scope;          // Lifetime scope
    int is_moved_into;             // Reference target was moved
    int invalidation_count;        // Number of times invalidated
    
    struct ReferenceInfo* next;    // For linked lists
} ReferenceInfo;

// Lifetime scope tracking
struct LifetimeScope {
    size_t scope_id;               // Unique scope identifier
    LifetimeScopeKind kind;        // Type of scope
    size_t start_position;         // Scope start position
    size_t end_position;           // Scope end position
    
    ReferenceInfo** references;   // References created in this scope
    size_t reference_count;
    size_t reference_capacity;
    
    struct LifetimeScope* parent;  // Parent scope
    struct LifetimeScope** children; // Child scopes
    size_t child_count;
    size_t child_capacity;
    
    // Scope metadata
    int is_loop_scope;             // Contains loop iterations
    int may_break_early;           // May exit before natural end
    int has_conditional_paths;     // Contains if/else branches
};

// Borrow tracking for conflict detection
struct BorrowTracker {
    char* target_name;             // Variable being borrowed
    ReferenceInfo** borrowers;     // Active borrowers
    size_t borrower_count;
    size_t borrower_capacity;
    
    int has_mutable_borrow;        // Has active mutable borrow
    int immutable_borrow_count;    // Number of immutable borrows
    size_t last_mutation_position; // Last time target was mutated
    
    struct BorrowTracker* next;    // For linked lists
};

// Smart reference manager
struct ReferenceManager {
    FlowSensitiveAnalyzer* flow_analyzer; // Parent flow analyzer
    
    LifetimeScope* current_scope;  // Current lifetime scope
    LifetimeScope* global_scope;   // Global scope
    size_t next_scope_id;          // Next scope ID to assign
    
    ReferenceInfo** all_references; // All references being tracked
    size_t reference_count;
    size_t reference_capacity;
    
    BorrowTracker** borrow_trackers; // Borrow conflict tracking
    size_t tracker_count;
    size_t tracker_capacity;
    
    // Configuration
    int enable_weak_references;    // Support weak references
    int enable_smart_pointers;     // Generate smart pointer code
    int strict_lifetime_checking;  // Strict vs permissive checking
    
    // Statistics
    size_t references_created;
    size_t references_invalidated;
    size_t borrow_conflicts_detected;
    size_t lifetime_errors_prevented;
    
    // Error tracking
    int error_count;
    int warning_count;
};

// Flow-sensitive analysis functions
FlowSensitiveAnalyzer* flow_analyzer_new(TypeChecker* type_checker);
void flow_analyzer_free(FlowSensitiveAnalyzer* analyzer);

// Control flow graph construction
ControlFlowGraph* cfg_build(ASTNode* function_body);
void cfg_free(ControlFlowGraph* cfg);
BasicBlock* cfg_add_block(ControlFlowGraph* cfg);
void cfg_add_edge(BasicBlock* from, BasicBlock* to);
void cfg_compute_dominators(ControlFlowGraph* cfg);
void cfg_build_from_ast(ControlFlowGraph* cfg, ASTNode* node, BasicBlock* current_block, BasicBlock* exit_block);

// Value state management
ValueState* value_state_new(const char* name, ValueStateKind initial_state);
void value_state_free(ValueState* state);
ValueState* value_state_copy(const ValueState* state);
void value_state_merge(ValueState* target, const ValueState* source);

// Ownership state management
OwnershipState* ownership_state_new(OwnershipState* parent);
void ownership_state_free(OwnershipState* state);
OwnershipState* ownership_state_copy(const OwnershipState* state);
void ownership_state_merge(OwnershipState* target, const OwnershipState* source);

ValueState* ownership_state_lookup(OwnershipState* state, const char* name);
void ownership_state_update(OwnershipState* state, const char* name, ValueStateKind new_state);
void ownership_state_add_value(OwnershipState* state, ValueState* value);

// Flow-sensitive analysis algorithms
int flow_analyze_function(FlowSensitiveAnalyzer* analyzer, ASTNode* function);
int flow_analyze_block(FlowSensitiveAnalyzer* analyzer, BasicBlock* block);
int flow_analyze_statement(FlowSensitiveAnalyzer* analyzer, ASTNode* stmt, OwnershipState* state);
int flow_analyze_expression(FlowSensitiveAnalyzer* analyzer, ASTNode* expr, OwnershipState* state);

// Transfer operation inference
TransferKind infer_transfer_operation(FlowSensitiveAnalyzer* analyzer, 
                                     const char* var_name, 
                                     ASTNode* usage_context,
                                     OwnershipState* state);

// Move/copy optimization
int optimize_move_operations(FlowSensitiveAnalyzer* analyzer, ASTNode* function);
int can_safely_move(FlowSensitiveAnalyzer* analyzer, const char* var_name, 
                   size_t position, OwnershipState* state);
int has_future_uses(FlowSensitiveAnalyzer* analyzer, const char* var_name, 
                   size_t position, BasicBlock* current_block);

// Usage pattern analysis
int analyze_usage_patterns(FlowSensitiveAnalyzer* analyzer, ASTNode* function);
int detect_copy_patterns(FlowSensitiveAnalyzer* analyzer, const char* var_name, OwnershipState* state);
int detect_move_patterns(FlowSensitiveAnalyzer* analyzer, const char* var_name, OwnershipState* state);
int detect_borrow_patterns(FlowSensitiveAnalyzer* analyzer, const char* var_name, OwnershipState* state);

// Closure capture analysis
int analyze_closure_captures(FlowSensitiveAnalyzer* analyzer, ASTNode* closure_expr);
int requires_move_into_closure(FlowSensitiveAnalyzer* analyzer, const char* var_name, 
                              ASTNode* closure_body);

// Path-sensitive analysis
int analyze_conditional_moves(FlowSensitiveAnalyzer* analyzer, ASTNode* if_stmt, 
                             OwnershipState* state);
int merge_conditional_states(OwnershipState* then_state, OwnershipState* else_state, 
                            OwnershipState* result_state);

// Error and warning reporting
void flow_error(FlowSensitiveAnalyzer* analyzer, Position pos, const char* format, ...);
void flow_warning(FlowSensitiveAnalyzer* analyzer, Position pos, const char* format, ...);

// Utility functions
const char* value_state_to_string(ValueStateKind state);
const char* transfer_kind_to_string(TransferKind kind);
const char* escape_kind_to_string(EscapeKind kind);

// Integration with existing type checker
int integrate_flow_analysis(TypeChecker* type_checker, ASTNode* function);
void apply_ownership_decisions(TypeChecker* type_checker, FlowSensitiveAnalyzer* analyzer);

// Reference manager functions
ReferenceManager* reference_manager_new(FlowSensitiveAnalyzer* flow_analyzer);
void reference_manager_free(ReferenceManager* mgr);

// Lifetime scope management
LifetimeScope* reference_manager_enter_scope(ReferenceManager* mgr, LifetimeScopeKind kind, size_t start_pos);
void reference_manager_exit_scope(ReferenceManager* mgr, size_t end_pos);

// Reference creation and tracking
ReferenceInfo* reference_manager_create_reference(ReferenceManager* mgr, 
                                                 const char* ref_name,
                                                 const char* target_name,
                                                 ReferenceKind kind,
                                                 size_t position);
void reference_manager_use_reference(ReferenceManager* mgr, const char* ref_name, size_t position);
void reference_manager_invalidate_references(ReferenceManager* mgr, const char* target_name, size_t position);

// Safety checks
int reference_manager_can_move(ReferenceManager* mgr, const char* var_name, size_t position);

// Analysis functions
int reference_manager_analyze_expression(ReferenceManager* mgr, ASTNode* expr, size_t position);
int reference_manager_analyze_statement(ReferenceManager* mgr, ASTNode* stmt, size_t position);

// Code generation
int reference_manager_generate_cleanup_code(ReferenceManager* mgr, ASTNode* function);

// Statistics and debugging
void reference_manager_print_statistics(ReferenceManager* mgr);

// Debug and visualization
void flow_analyzer_dump_cfg(FlowSensitiveAnalyzer* analyzer, const char* filename);
void flow_analyzer_dump_states(FlowSensitiveAnalyzer* analyzer, const char* filename);
void flow_analyzer_print_statistics(FlowSensitiveAnalyzer* analyzer);

// =============================================================================
// Interprocedural Escape Analysis Functions (Task 19.3)
// =============================================================================

// Escape context management
EscapeContext* escape_context_new(ASTNode* escape_site, ASTNode* target_function, 
                                 EscapeKind kind, size_t call_depth);
void escape_context_free(EscapeContext* context);

// Function escape information management
FunctionEscapeInfo* function_escape_info_new(const char* function_name, ASTNode* function_node);
void function_escape_info_free(FunctionEscapeInfo* info);

// Escape analyzer management
EscapeAnalyzer* escape_analyzer_new(ReferenceManager* reference_manager);
void escape_analyzer_free(EscapeAnalyzer* analyzer);

// Function registration and call graph building
int escape_analyzer_register_function(EscapeAnalyzer* analyzer, const char* name, ASTNode* func_node);
FunctionEscapeInfo* escape_analyzer_find_function(EscapeAnalyzer* analyzer, const char* name);
int escape_analyzer_register_call_site(EscapeAnalyzer* analyzer, ASTNode* call_node);

// Escape analysis algorithms
EscapeKind analyze_expression_escape(EscapeAnalyzer* analyzer, ASTNode* expr, 
                                    FunctionEscapeInfo* current_function);
int analyze_statement_escape(EscapeAnalyzer* analyzer, ASTNode* stmt, 
                            FunctionEscapeInfo* current_function);
int analyze_function_escape(EscapeAnalyzer* analyzer, FunctionEscapeInfo* func_info);

// Interprocedural analysis driver
int escape_analyzer_analyze_program(EscapeAnalyzer* analyzer, ASTNode* program);
int collect_function_definitions(EscapeAnalyzer* analyzer, ASTNode* node);
int build_call_graph(EscapeAnalyzer* analyzer);
int collect_function_calls(EscapeAnalyzer* analyzer, ASTNode* node, FunctionEscapeInfo* caller);
int analyze_functions_topologically(EscapeAnalyzer* analyzer);
int propagate_escape_information(EscapeAnalyzer* analyzer);
int optimize_allocation_strategies(EscapeAnalyzer* analyzer);

// Allocation strategy determination
AllocationStrategy determine_allocation_strategy(EscapeAnalyzer* analyzer, 
                                               ASTNode* alloc_site,
                                               EscapeKind escape_kind);
ObjectLifetime determine_object_lifetime(EscapeAnalyzer* analyzer,
                                        ASTNode* object_site,
                                        EscapeKind escape_kind);

// Statistics and reporting
void escape_analyzer_print_statistics(EscapeAnalyzer* analyzer);
void escape_analyzer_print_function_info(EscapeAnalyzer* analyzer, const char* function_name);

// Utility functions
const char* object_lifetime_to_string(ObjectLifetime lifetime);
const char* allocation_strategy_to_string(AllocationStrategy strategy);

// Integration functions
int integrate_escape_analysis_with_type_checker(TypeChecker* type_checker, EscapeAnalyzer* analyzer);
int apply_escape_analysis_to_codegen(EscapeAnalyzer* analyzer, ASTNode* program);

// =============================================================================
// Memory Safety Integration with Type System Functions (Task 19.5)
// =============================================================================

// Forward declaration for memory safety context
typedef struct MemorySafetyContext MemorySafetyContext;

// Memory safety integration management
int integrate_memory_safety_with_type_checker(TypeChecker* type_checker);
void cleanup_memory_safety_integration(void);
MemorySafetyContext* get_memory_safety_context(void);

// Enhanced type checking with memory safety
Type* memory_safe_type_check_expression(TypeChecker* checker, ASTNode* expr);
int memory_safe_type_check_statement(TypeChecker* checker, ASTNode* stmt);

// Memory safety expression checking
Type* memory_safe_check_identifier_access(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type);
Type* memory_safe_check_field_access(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type);
Type* memory_safe_check_array_access(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type);
Type* memory_safe_check_function_call(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type);
Type* memory_safe_check_unary_operation(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type);
Type* memory_safe_check_binary_operation(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type);
Type* memory_safe_check_assignment(MemorySafetyContext* ctx, BinaryExprNode* assign, Type* expr_type);
Type* memory_safe_check_try_expression(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type);

// Memory safety statement checking
int memory_safe_check_variable_declaration(MemorySafetyContext* ctx, ASTNode* stmt);
int memory_safe_check_defer_statement(MemorySafetyContext* ctx, ASTNode* stmt);
int memory_safe_check_block_statement(MemorySafetyContext* ctx, ASTNode* stmt);
int memory_safe_check_if_let_statement(MemorySafetyContext* ctx, ASTNode* stmt);

// Helper functions for safety analysis
int is_null_checked_context(MemorySafetyContext* ctx, const char* var_name, Position pos);
int is_guaranteed_non_null(MemorySafetyContext* ctx, const char* var_name, Position pos);
int should_move_value(MemorySafetyContext* ctx, Variable* var, Position pos);

// Statistics and reporting
void memory_safety_print_statistics(void);

// Configuration functions
void memory_safety_enable_feature(const char* feature, int enable);
int memory_safety_is_feature_enabled(const char* feature);

// =============================================================================
// Automatic Resource Management Functions (Task 19.4)
// =============================================================================

// Forward declarations for resource management
typedef struct ResourceManager ResourceManager;
typedef struct ResourceInfo ResourceInfo;
typedef struct CleanupAction CleanupAction;
typedef struct DeferInfo DeferInfo;
typedef struct ScopeCleanup ScopeCleanup;

// Resource type enums
typedef enum {
    RESOURCE_TYPE_FILE,
    RESOURCE_TYPE_NETWORK,
    RESOURCE_TYPE_MUTEX,
    RESOURCE_TYPE_MEMORY,
    RESOURCE_TYPE_THREAD,
    RESOURCE_TYPE_GPU_BUFFER,
    RESOURCE_TYPE_CUSTOM,
    RESOURCE_TYPE_UNKNOWN
} ResourceType;

typedef enum {
    RESOURCE_CONTEXT_DIRECT,
    RESOURCE_CONTEXT_FUNCTION_CALL,
    RESOURCE_CONTEXT_CONSTRUCTOR,
    RESOURCE_CONTEXT_ASSIGNMENT,
    RESOURCE_CONTEXT_PARAMETER
} ResourceContext;

typedef enum {
    CLEANUP_METHOD_FUNCTION_CALL,
    CLEANUP_METHOD_DESTRUCTOR,
    CLEANUP_METHOD_DEFER,
    CLEANUP_METHOD_RAII,
    CLEANUP_METHOD_CUSTOM
} CleanupMethod;

// Resource manager management
ResourceManager* resource_manager_new(TypeChecker* type_checker);
void resource_manager_free(ResourceManager* rm);

// Scope management
ScopeCleanup* resource_manager_enter_scope(ResourceManager* rm, ASTNode* scope_node);
void resource_manager_exit_scope(ResourceManager* rm);

// Resource tracking
int resource_manager_track_resource(ResourceManager* rm, const char* name, 
                                   ResourceType type, ASTNode* acquisition_site, Position pos);
ResourceInfo* resource_manager_find_resource(ResourceManager* rm, const char* name);
int resource_manager_mark_resource_moved(ResourceManager* rm, const char* name);
int resource_manager_mark_resource_borrowed(ResourceManager* rm, const char* name);

// Defer statement processing
int resource_manager_process_defer(ResourceManager* rm, ASTNode* defer_stmt, Position pos);

// Cleanup generation
int resource_manager_generate_scope_cleanup(ResourceManager* rm, ScopeCleanup* scope);
char* generate_cleanup_code(ResourceInfo* resource, CleanupMethod method);

// AST analysis
int resource_manager_analyze_statement(ResourceManager* rm, ASTNode* stmt);
int resource_manager_analyze_expression(ResourceManager* rm, ASTNode* expr);
int resource_manager_analyze_function(ResourceManager* rm, ASTNode* func_node);

// Resource type detection
ResourceType detect_resource_type(ResourceManager* rm, ASTNode* expr);
ResourceType get_resource_type_for_function(const char* func_name);

// Statistics and reporting
void resource_manager_print_statistics(ResourceManager* rm);
void resource_manager_print_resource_info(ResourceManager* rm, const char* resource_name);

// Utility functions
const char* resource_type_to_string(ResourceType type);
const char* resource_context_to_string(ResourceContext context);
const char* cleanup_method_to_string(CleanupMethod method);

// Error reporting
void resource_manager_error(ResourceManager* rm, Position pos, const char* format, ...);
void resource_manager_warning(ResourceManager* rm, Position pos, const char* format, ...);

// Integration functions
int integrate_resource_manager_with_type_checker(TypeChecker* type_checker, ResourceManager* rm);
int apply_resource_management_to_codegen(ResourceManager* rm, ASTNode* program);

#endif // MEMORY_SAFETY_H
