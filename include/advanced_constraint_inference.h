#ifndef ADVANCED_CONSTRAINT_INFERENCE_H
#define ADVANCED_CONSTRAINT_INFERENCE_H

#include "types/constraint_inference.h"
#include "types.h"
#include "ast.h"

// =============================================================================
// Task 22.6: Extended Automatic Constraint Inference for Advanced Use Cases
// =============================================================================

typedef struct ConstraintInferenceEngine ConstraintInferenceEngine;
typedef struct TypeVariable TypeVariable;
typedef struct Constraint Constraint;
typedef struct ConstraintSet ConstraintSet;
typedef struct UnificationContext UnificationContext;

// Forward declarations
typedef struct HigherOrderConstraint HigherOrderConstraint;
typedef struct ConstraintHint ConstraintHint;
typedef struct AdvancedConstraintSolver AdvancedConstraintSolver;
typedef struct VariadicTypePattern VariadicTypePattern;
typedef struct NestedGenericPattern NestedGenericPattern;
typedef struct ConstraintOptimizer ConstraintOptimizer;

// =============================================================================
// Higher-Order Function and Callback Support
// =============================================================================

// Higher-order constraint kinds for callbacks and function types
typedef enum {
    HO_CONSTRAINT_CALLBACK,         // T: Fn(Args) -> Return
    HO_CONSTRAINT_ASYNC_CALLBACK,   // T: AsyncFn(Args) -> Future<Return>
    HO_CONSTRAINT_CLOSURE,          // T: FnOnce/FnMut/Fn + captures
    HO_CONSTRAINT_GENERATOR,        // T: Generator<Yield, Return>
    HO_CONSTRAINT_COROUTINE,        // T: Coroutine<Args, Yield, Return>
    HO_CONSTRAINT_ITERATOR_ADAPTER, // T: Iterator -> Iterator transformation
    HO_CONSTRAINT_FOLD_FUNCTION,    // T: Fold<Acc, Item> -> Acc
    HO_CONSTRAINT_MAP_FUNCTION,     // T: Map<Item> -> NewItem
    HO_CONSTRAINT_FILTER_FUNCTION,  // T: Filter<Item> -> bool
    HO_CONSTRAINT_COMBINATOR        // T: Combinator pattern
} HigherOrderConstraintKind;

// Higher-order constraint information
struct HigherOrderConstraint {
    HigherOrderConstraintKind kind;     // Kind of higher-order constraint
    Type* function_type;                // Function type being constrained
    Type** parameter_types;             // Parameter types
    size_t parameter_count;
    Type* return_type;                  // Return type
    Type** captured_types;              // Types captured by closures
    size_t captured_count;
    char* capture_mode;                 // "by_value", "by_ref", "by_mut_ref"
    int is_async;                       // Whether function is async
    int is_generator;                   // Whether function is a generator
    ConstraintSet* nested_constraints;  // Constraints on function body
    Position source_pos;
    struct HigherOrderConstraint* next;
};

// =============================================================================
// Complex Generic Patterns Support
// =============================================================================

// Variadic type parameter patterns
struct VariadicTypePattern {
    char* name;                         // Pattern name (e.g., "...Args")
    TypeVariable element_kind;      // Kind of each element
    size_t min_count;                   // Minimum number of elements
    size_t max_count;                   // Maximum number of elements (0 = unlimited)
    ConstraintSet* element_constraints; // Constraints on each element
    Type** bound_types;                 // Bound types if inferred
    size_t bound_count;                 // Number of bound types
    Position declared_pos;
};

// Nested generic patterns (e.g., Vec<Option<T>>, Map<K, Vec<V>>)
struct NestedGenericPattern {
    char* pattern_name;                 // Pattern identifier
    Type* outer_constructor;            // Outer type constructor (Vec, Map, etc.)
    Type* inner_constructor;            // Inner type constructor (Option, Vec, etc.)
    size_t nesting_depth;               // How deep the nesting goes
    TypeVariable** type_variables;      // Type variables at each level
    size_t variable_count;
    ConstraintSet** level_constraints;  // Constraints at each nesting level
    Position pattern_pos;
};

// =============================================================================
// Enhanced Error Reporting for Constraint Failures
// =============================================================================

// Constraint error kinds for detailed reporting
typedef enum {
    CONSTRAINT_ERROR_UNSATISFIABLE,     // Constraint cannot be satisfied
    CONSTRAINT_ERROR_AMBIGUOUS,         // Multiple possible solutions
    CONSTRAINT_ERROR_CIRCULAR,          // Circular dependency detected
    CONSTRAINT_ERROR_UNDERCONSTRAINED,  // Not enough constraints to infer type
    CONSTRAINT_ERROR_OVERCONSTRAINED,   // Contradictory constraints
    CONSTRAINT_ERROR_KIND_MISMATCH,     // Kind error in higher-kinded types
    CONSTRAINT_ERROR_VARIANCE,          // Variance error in type parameters
    CONSTRAINT_ERROR_LIFETIME,          // Lifetime constraint violation
    CONSTRAINT_ERROR_COHERENCE,         // Trait coherence violation
    CONSTRAINT_ERROR_ORPHAN_RULE        // Orphan rule violation
} ConstraintErrorKind;

// Detailed constraint error information
typedef struct ConstraintError {
    ConstraintErrorKind kind;           // Kind of error
    char* primary_message;              // Main error message
    char* detailed_explanation;         // Detailed explanation
    char** suggestions;                 // Suggested fixes
    size_t suggestion_count;
    Position primary_pos;               // Primary error location
    Position* secondary_positions;      // Related locations
    size_t secondary_count;
    Constraint* failing_constraint; // The constraint that failed
    Type* expected_type;                // Expected type (if applicable)
    Type* actual_type;                  // Actual type (if applicable)
    float confidence_score;             // Confidence in error diagnosis
    struct ConstraintError* next;
} ConstraintError;

// =============================================================================
// User-Guided Constraint Hints
// =============================================================================

// Constraint hint kinds
typedef enum {
    HINT_TYPE_ANNOTATION,               // Explicit type annotation
    HINT_TRAIT_BOUND,                   // Explicit trait bound
    HINT_ASSOCIATED_TYPE,               // Associated type hint
    HINT_LIFETIME_BOUND,                // Lifetime bound hint
    HINT_INFERENCE_PRIORITY,            // Priority for ambiguous inference
    HINT_DISAMBIGUATION,                // Disambiguation for overloaded functions
    HINT_VARIANCE_ANNOTATION,           // Variance annotation
    HINT_OPTIMIZATION_GUIDE             // Optimization hint for constraint solver
} ConstraintHintKind;

// User-provided constraint hint
struct ConstraintHint {
    ConstraintHintKind kind;            // Kind of hint
    char* target_identifier;           // Identifier being hinted
    Type* suggested_type;               // Suggested type
    char* trait_name;                   // Trait name for trait bounds
    char* associated_type_name;         // Associated type name
    int priority;                       // Priority level (1-10)
    char* disambiguation_context;       // Context for disambiguation
    Position hint_pos;                  // Where hint was provided
    int is_mandatory;                   // Whether hint is required
    struct ConstraintHint* next;
};

// =============================================================================
// Optimized Constraint Solver
// =============================================================================

// Solver strategy kinds
typedef enum {
    SOLVER_STRATEGY_BASIC,              // Basic constraint propagation
    SOLVER_STRATEGY_UNIFICATION,        // Unification-based solving
    SOLVER_STRATEGY_TABLEAUX,           // Tableaux method
    SOLVER_STRATEGY_SMT,                // SMT solver integration
    SOLVER_STRATEGY_GRAPH_BASED,        // Graph-based constraint solving
    SOLVER_STRATEGY_LAZY,               // Lazy constraint evaluation
    SOLVER_STRATEGY_INCREMENTAL         // Incremental constraint solving
} ConstraintSolverStrategy;

// Performance optimization flags
typedef enum {
    OPT_CONSTRAINT_CACHING      = 1 << 0,  // Cache constraint solutions
    OPT_EARLY_TERMINATION       = 1 << 1,  // Early termination on conflicts
    OPT_CONSTRAINT_PROPAGATION  = 1 << 2,  // Aggressive constraint propagation
    OPT_TYPE_VARIABLE_ORDERING  = 1 << 3,  // Optimize type variable ordering
    OPT_PARALLEL_SOLVING        = 1 << 4,  // Parallel constraint solving
    OPT_INCREMENTAL_UPDATE      = 1 << 5,  // Incremental updates
    OPT_CONSTRAINT_SIMPLIFICATION = 1 << 6, // Constraint simplification
    OPT_HEURISTIC_PRUNING       = 1 << 7   // Heuristic-based pruning
} ConstraintOptimizerFlags;

// Advanced constraint solver
struct AdvancedConstraintSolver {
    ConstraintInferenceEngine* base_engine; // Base inference engine
    ConstraintSolverStrategy strategy;      // Solving strategy
    ConstraintOptimizerFlags optimization_flags; // Optimization settings
    
    // Performance tracking
    size_t constraints_solved;              // Number of constraints solved
    size_t unification_steps;               // Number of unification steps
    size_t backtrack_count;                 // Number of backtracks
    double solve_time_ms;                   // Total solving time in milliseconds
    
    // Solver state
    Constraint** constraint_queue; // Priority queue of constraints
    size_t queue_size;
    size_t queue_capacity;
    TypeVariable** variable_order;          // Optimal variable ordering
    size_t variable_count;
    
    // Caching and memoization
    void* solution_cache;                   // Cache of solved constraints
    void* unification_cache;                // Cache of unification results
    
    // Error tracking
    ConstraintError* errors;                // List of constraint errors
    size_t error_count;
};

// =============================================================================
// Integration with Language Features
// =============================================================================

// Integration contexts
typedef enum {
    INTEGRATION_ERROR_HANDLING,         // Integration with error unions (!T)
    INTEGRATION_NULLABLE_TYPES,         // Integration with nullable types (?T)
    INTEGRATION_OWNERSHIP_SYSTEM,       // Integration with ownership tracking
    INTEGRATION_LIFETIME_SYSTEM,        // Integration with lifetime analysis
    INTEGRATION_ASYNC_SYSTEM,           // Integration with async/await
    INTEGRATION_CONCURRENCY,            // Integration with channels/goroutines
    INTEGRATION_PATTERN_MATCHING,       // Integration with pattern matching
    INTEGRATION_MACRO_SYSTEM,           // Integration with macro expansion
    INTEGRATION_FOREIGN_FUNCTIONS       // Integration with foreign function interface
} IntegrationContext;

// Language feature integration data
typedef struct {
    IntegrationContext context;           // Context of integration
    Type* primary_type;                   // Primary type being integrated
    Type** related_types;                 // Related types in the integration
    size_t related_count;
    ConstraintSet* integration_constraints; // Constraints from integration
    char* feature_specific_data;          // Feature-specific data
    Position integration_pos;
} LanguageFeatureIntegration;

// =============================================================================
// Function Declarations
// =============================================================================

// Higher-order function constraint inference
HigherOrderConstraint* higher_order_constraint_new(HigherOrderConstraintKind kind, 
                                                   Type* function_type, Position pos);
void higher_order_constraint_free(HigherOrderConstraint* constraint);
int infer_higher_order_constraints(ConstraintInferenceEngine* engine, ASTNode* expr);
int infer_callback_constraints(ConstraintInferenceEngine* engine, ASTNode* callback_expr);
int infer_closure_constraints(ConstraintInferenceEngine* engine, ASTNode* closure_expr);
int infer_generator_constraints(ConstraintInferenceEngine* engine, ASTNode* generator_expr);

// Complex generic pattern support
VariadicTypePattern* variadic_type_pattern_new(const char* name, TypeVariable element_kind, Position pos);
void variadic_type_pattern_free(VariadicTypePattern* pattern);
int infer_variadic_constraints(ConstraintInferenceEngine* engine, VariadicTypePattern* pattern, ASTNode* usage);

NestedGenericPattern* nested_generic_pattern_new(const char* name, Type* outer, Type* inner, Position pos);
void nested_generic_pattern_free(NestedGenericPattern* pattern);
int infer_nested_generic_constraints(ConstraintInferenceEngine* engine, NestedGenericPattern* pattern, ASTNode* usage);

// Enhanced error reporting
ConstraintError* constraint_error_new(ConstraintErrorKind kind, const char* message, Position pos);
void constraint_error_free(ConstraintError* error);
void constraint_error_add_suggestion(ConstraintError* error, const char* suggestion);
void constraint_error_add_secondary_position(ConstraintError* error, Position pos);
char* generate_detailed_constraint_error_report(ConstraintError* error);
void print_constraint_error_with_context(ConstraintError* error, TypeChecker* checker);

// User-guided constraint hints
ConstraintHint* constraint_hint_new(ConstraintHintKind kind, const char* target, Position pos);
void constraint_hint_free(ConstraintHint* hint);
int apply_constraint_hint(ConstraintInferenceEngine* engine, ConstraintHint* hint);
int parse_constraint_hint_from_annotation(const char* annotation, ConstraintHint** hints, size_t* hint_count);

// Advanced constraint solver
AdvancedConstraintSolver* advanced_constraint_solver_new(ConstraintInferenceEngine* base_engine, ConstraintSolverStrategy strategy);
void advanced_constraint_solver_free(AdvancedConstraintSolver* solver);
int advanced_constraint_solver_set_optimization_flags(AdvancedConstraintSolver* solver, ConstraintOptimizerFlags flags);
int advanced_constraint_solver_solve_advanced(AdvancedConstraintSolver* solver);
int advanced_constraint_solver_solve_incrementally(AdvancedConstraintSolver* solver, Constraint* new_constraint);

// Performance optimization functions
int optimize_constraint_order(AdvancedConstraintSolver* solver);
int prune_redundant_constraints(AdvancedConstraintSolver* solver);
int cache_constraint_solution(AdvancedConstraintSolver* solver, Constraint* constraint, Type* solution);
Type* lookup_cached_solution(AdvancedConstraintSolver* solver, Constraint* constraint);

// Language feature integration
LanguageFeatureIntegration* create_error_handling_integration(Type* error_union_type, Position pos);
LanguageFeatureIntegration* create_nullable_integration(Type* nullable_type, Position pos);
LanguageFeatureIntegration* create_ownership_integration(Type* owned_type, OwnershipKind ownership, Position pos);
LanguageFeatureIntegration* create_async_integration(Type* future_type, Position pos);
LanguageFeatureIntegration* create_concurrency_integration(Type* channel_type, Position pos);

int integrate_with_error_handling(ConstraintInferenceEngine* engine, LanguageFeatureIntegration* integration);
int integrate_with_nullable_types(ConstraintInferenceEngine* engine, LanguageFeatureIntegration* integration);
int integrate_with_ownership_system(ConstraintInferenceEngine* engine, LanguageFeatureIntegration* integration);
int integrate_with_async_system(ConstraintInferenceEngine* engine, LanguageFeatureIntegration* integration);
int integrate_with_concurrency(ConstraintInferenceEngine* engine, LanguageFeatureIntegration* integration);

// Advanced inference functions
int infer_constraints_from_pattern_match(ConstraintInferenceEngine* engine, ASTNode* pattern_expr);
int infer_constraints_from_async_expr(ConstraintInferenceEngine* engine, ASTNode* async_expr);
int infer_constraints_from_channel_operation(ConstraintInferenceEngine* engine, ASTNode* channel_expr);
int infer_constraints_from_error_propagation(ConstraintInferenceEngine* engine, ASTNode* try_expr);
int infer_constraints_from_ownership_transfer(ConstraintInferenceEngine* engine, ASTNode* move_expr);

// Utility functions
void print_constraint_solver_statistics(const AdvancedConstraintSolver* solver);
void print_advanced_constraint_information(const ConstraintInferenceEngine* engine);
int validate_advanced_constraint_system(ConstraintInferenceEngine* engine);

#endif // ADVANCED_CONSTRAINT_INFERENCE_H
