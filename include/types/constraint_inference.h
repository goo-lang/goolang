#ifndef GOO_CONSTRAINT_INFERENCE_H
#define GOO_CONSTRAINT_INFERENCE_H

#include "types.h"
#include "ast.h"
#include "errors/error.h"
#include <stddef.h>
#include <stdbool.h>

// Forward declarations
typedef struct ConstraintInferenceEngine ConstraintInferenceEngine;
typedef struct TypeVariable TypeVariable;
typedef struct Constraint Constraint;
typedef struct ConstraintSet ConstraintSet;
typedef struct UnificationContext UnificationContext;

// Type of constraints
typedef enum {
    CONSTRAINT_EQUALITY,           // T1 = T2
    CONSTRAINT_SUBTYPE,           // T1 <: T2 (subtyping)
    CONSTRAINT_IMPLEMENTS,        // T implements Interface
    CONSTRAINT_HAS_METHOD,        // T has method M
    CONSTRAINT_HAS_FIELD,         // T has field F
    CONSTRAINT_CALLABLE,          // T is callable with signature S
    CONSTRAINT_INDEXABLE,         // T is indexable with key K returning V
    CONSTRAINT_ITERABLE,          // T is iterable yielding element E
    CONSTRAINT_COMPARABLE,        // T supports comparison operators
    CONSTRAINT_ARITHMETIC,        // T supports arithmetic operators
    CONSTRAINT_NUMERIC,           // T is a numeric type
    CONSTRAINT_CONVERTIBLE,       // T can be converted to U
    CONSTRAINT_SIZED,             // T has known size at compile time
    CONSTRAINT_SENDABLE,          // T can be sent through channels
    CONSTRAINT_COPYABLE,          // T can be copied
    CONSTRAINT_MOVABLE,           // T can be moved
    CONSTRAINT_DROPPABLE,         // T has a custom destructor
    CONSTRAINT_DEFAULT,           // T has a default/zero value
    CONSTRAINT_SERIALIZABLE,      // T can be serialized
    CONSTRAINT_HASHABLE,          // T can be hashed
    CONSTRAINT_GENERIC_INSTANCE,  // T is an instance of generic G with args A
    CONSTRAINT_KIND_CONSTRAINT,   // T has specific kind (*, * -> *, etc.)
    CONSTRAINT_DEPENDENCY         // Constraint depends on other constraints
} ConstraintKind;

// Priority levels for constraint resolution
typedef enum {
    CONSTRAINT_PRIORITY_HIGHEST = 0,   // Built-in type relationships
    CONSTRAINT_PRIORITY_HIGH = 1,      // Explicit type annotations
    CONSTRAINT_PRIORITY_MEDIUM = 2,    // Method calls and operations
    CONSTRAINT_PRIORITY_LOW = 3,       // Inference from context
    CONSTRAINT_PRIORITY_LOWEST = 4     // Default assumptions
} ConstraintPriority;

// Type variable - represents unknown types during inference
struct TypeVariable {
    int id;                           // Unique identifier
    char* name;                       // Optional name for debugging
    Type* resolved_type;              // NULL if not yet resolved
    ConstraintSet* constraints;       // Constraints on this type variable
    
    // Source information
    Position source_pos;              // Where this type variable originated
    char* source_context;             // Description of context (e.g., "parameter x")
    
    // Variance and bounds
    bool is_contravariant;            // Used in function parameter positions
    bool is_covariant;                // Used in return positions
    Type* upper_bound;                // Upper type bound (T <: UpperBound)
    Type* lower_bound;                // Lower type bound (LowerBound <: T)
    
    // Lifetime for memory management
    int ref_count;
    struct TypeVariable* next;        // For linked list storage
};

// Individual constraint
struct Constraint {
    ConstraintKind kind;
    ConstraintPriority priority;
    
    // The types/type variables involved in this constraint
    union {
        // Binary constraints (equality, subtyping, etc.)
        struct {
            Type* left;               // Can be TypeVariable or concrete Type
            Type* right;
        } binary;
        
        // Method constraint
        struct {
            Type* receiver_type;
            char* method_name;
            Type* method_signature;
            bool is_required;         // vs. optional method
        } method;
        
        // Field constraint
        struct {
            Type* struct_type;
            char* field_name;
            Type* field_type;
        } field;
        
        // Callable constraint
        struct {
            Type* callable_type;
            Type** arg_types;
            size_t arg_count;
            Type* return_type;
            bool is_variadic;
        } callable;
        
        // Indexable constraint
        struct {
            Type* container_type;
            Type* key_type;
            Type* value_type;
        } indexable;
        
        // Iterable constraint
        struct {
            Type* iterable_type;
            Type* element_type;
        } iterable;
        
        // Generic instance constraint
        struct {
            Type* instance_type;
            Type* generic_type;
            Type** type_args;
            size_t arg_count;
        } generic_instance;
        
        // Dependency constraint
        struct {
            Constraint** dependencies;
            size_t dep_count;
            bool all_required;        // true = AND, false = OR
        } dependency;
    } data;
    
    // Source information for error reporting
    Position source_pos;
    char* source_description;
    
    // Resolution state
    bool is_resolved;
    bool resolution_failed;
    char* failure_reason;
    
    // For linked lists and priority queues
    struct Constraint* next;
};

// Set of constraints
struct ConstraintSet {
    Constraint* constraints;          // Linked list of constraints
    size_t count;
    
    // Categorized constraints for efficient lookup
    Constraint* equality_constraints;
    Constraint* subtype_constraints;
    Constraint* method_constraints;
    Constraint* field_constraints;
    
    // Statistics
    size_t resolved_count;
    size_t failed_count;
};

// Unification context for constraint solving
struct UnificationContext {
    TypeVariable** type_vars;        // Array of all type variables
    size_t type_var_count;
    size_t type_var_capacity;
    
    ConstraintSet* global_constraints;
    ConstraintSet* local_constraints; // Constraints for current scope
    
    // Substitution map: TypeVariable -> Type
    struct {
        TypeVariable** vars;
        Type** types;
        size_t count;
        size_t capacity;
    } substitutions;
    
    // Constraint resolution state
    bool is_solving;
    int resolution_depth;
    int max_resolution_depth;        // Prevents infinite recursion
    
    // Error reporting
    ErrorContext* error_ctx;
    Position current_pos;
};

// Main constraint inference engine
struct ConstraintInferenceEngine {
    UnificationContext* unification_ctx;
    
    // Type variable management
    int next_type_var_id;
    TypeVariable* type_var_pool;     // Reusable type variables
    
    // Built-in constraint patterns
    struct {
        ConstraintSet* numeric_constraints;
        ConstraintSet* comparable_constraints;
        ConstraintSet* iterable_constraints;
        ConstraintSet* callable_constraints;
    } builtin_patterns;
    
    // Configuration
    struct {
        bool infer_return_types;
        bool infer_parameter_types;
        bool aggressive_inference;
        bool allow_implicit_conversions;
        int max_inference_depth;
    } config;
    
    // Statistics and profiling
    struct {
        size_t constraints_generated;
        size_t constraints_resolved;
        size_t inference_failures;
        double total_inference_time;
    } stats;
};

// Constraint inference engine creation and management
ConstraintInferenceEngine* constraint_inference_engine_new(ErrorContext* error_ctx);
void constraint_inference_engine_free(ConstraintInferenceEngine* engine);

// Type variable management
TypeVariable* type_variable_new(ConstraintInferenceEngine* engine, const char* name, Position pos);
void type_variable_free(TypeVariable* type_var);
TypeVariable* type_variable_copy(const TypeVariable* type_var);
bool type_variable_is_resolved(const TypeVariable* type_var);
Type* type_variable_get_resolved_type(const TypeVariable* type_var);

// Constraint creation
Constraint* constraint_new(ConstraintKind kind, ConstraintPriority priority, Position pos);
void constraint_free(Constraint* constraint);
Constraint* constraint_equality(Type* left, Type* right, ConstraintPriority priority, Position pos);
Constraint* constraint_subtype(Type* subtype, Type* supertype, ConstraintPriority priority, Position pos);
Constraint* constraint_implements(Type* type, Type* interface, ConstraintPriority priority, Position pos);
Constraint* constraint_has_method(Type* type, const char* method_name, Type* signature, 
                                  ConstraintPriority priority, Position pos);
Constraint* constraint_callable(Type* type, Type** arg_types, size_t arg_count, 
                               Type* return_type, ConstraintPriority priority, Position pos);

// Constraint set management
ConstraintSet* constraint_set_new(void);
void constraint_set_free(ConstraintSet* set);
bool constraint_set_add(ConstraintSet* set, Constraint* constraint);
bool constraint_set_remove(ConstraintSet* set, Constraint* constraint);
void constraint_set_merge(ConstraintSet* target, ConstraintSet* source);

// Constraint collection from AST
bool collect_constraints_from_function(ConstraintInferenceEngine* engine, ASTNode* func_node);
bool collect_constraints_from_expression(ConstraintInferenceEngine* engine, ASTNode* expr_node);
bool collect_constraints_from_statement(ConstraintInferenceEngine* engine, ASTNode* stmt_node);
bool collect_constraints_from_type_annotation(ConstraintInferenceEngine* engine, ASTNode* type_node);

// Core constraint inference algorithms
bool infer_constraints_for_function_call(ConstraintInferenceEngine* engine, ASTNode* call_node);
bool infer_constraints_for_binary_operation(ConstraintInferenceEngine* engine, ASTNode* binary_node);
bool infer_constraints_for_assignment(ConstraintInferenceEngine* engine, ASTNode* assign_node);
bool infer_constraints_for_return_statement(ConstraintInferenceEngine* engine, ASTNode* return_node);

// Constraint solving and unification
bool solve_constraints(ConstraintInferenceEngine* engine);
bool unify_types(UnificationContext* ctx, Type* type1, Type* type2);
bool apply_substitution(UnificationContext* ctx, TypeVariable* var, Type* type);
Type* resolve_type_variable(UnificationContext* ctx, TypeVariable* var);

// Built-in constraint patterns
void initialize_builtin_constraint_patterns(ConstraintInferenceEngine* engine);
bool match_numeric_pattern(Type* type);
bool match_comparable_pattern(Type* type);
bool match_iterable_pattern(Type* type, Type** element_type);
bool match_callable_pattern(Type* type, Type*** arg_types, size_t* arg_count, Type** return_type);

// Error reporting and diagnostics
void report_constraint_error(ConstraintInferenceEngine* engine, const char* message, Position pos);
void report_unification_error(UnificationContext* ctx, Type* type1, Type* type2, Position pos);
void report_missing_method_error(ConstraintInferenceEngine* engine, Type* type, 
                                const char* method_name, Position pos);

// Debugging and introspection
void print_constraint(const Constraint* constraint);
void print_constraint_set(const ConstraintSet* set);
void print_type_variable(const TypeVariable* var);
char* constraint_to_string(const Constraint* constraint);
char* type_variable_to_string(const TypeVariable* var);

// Utility functions
const char* constraint_kind_to_string(ConstraintKind kind);
const char* constraint_priority_to_string(ConstraintPriority priority);
bool is_type_variable(const Type* type);
TypeVariable* as_type_variable(Type* type);
Type* type_variable_as_type(TypeVariable* var);

// Integration with existing type system
bool integrate_inferred_constraints(TypeChecker* checker, ConstraintInferenceEngine* engine);
Type* finalize_inferred_type(ConstraintInferenceEngine* engine, TypeVariable* var);

#endif // GOO_CONSTRAINT_INFERENCE_H