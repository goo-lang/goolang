#ifndef DEPENDENT_TYPES_H
#define DEPENDENT_TYPES_H

#include "types.h"
#include "ast.h"
#include "panic_free.h"
#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct DependentTypeContext DependentTypeContext;
typedef struct TypeConstraint TypeConstraint;
typedef struct RefinementType RefinementType;
typedef struct DependentType DependentType;
typedef struct TypeParameter TypeParameter;
typedef struct ConstraintSolver ConstraintSolver;

// =============================================================================
// Type Parameter System
// =============================================================================

typedef enum {
    TYPE_PARAM_TYPE,        // Generic type parameter: T
    TYPE_PARAM_VALUE,       // Value parameter: N (compile-time constant)
    TYPE_PARAM_CONSTRAINT   // Constraint parameter: where T: Ord
} TypeParameterKind;

typedef struct TypeParameter {
    char* name;
    TypeParameterKind kind;
    Type* base_type;        // For value parameters, the type of the value
    struct TypeParameter* next;
    
    union {
        struct {
            // Type parameter data
            Type** bounds;          // Interface bounds: T: Display + Clone
            size_t bound_count;
        } type_param;
        
        struct {
            // Value parameter data
            int64_t value;          // Compile-time constant value
            int is_resolved;        // Whether value is known at compile time
        } value_param;
        
        struct {
            // Constraint parameter data
            TypeConstraint* constraint;
        } constraint_param;
    } data;
} TypeParameter;

// =============================================================================
// Type Constraints and Refinements
// =============================================================================

typedef enum {
    DEP_CONSTRAINT_RANGE,       // x >= min && x <= max
    DEP_CONSTRAINT_NON_ZERO,    // x != 0
    DEP_CONSTRAINT_POSITIVE,    // x > 0
    DEP_CONSTRAINT_NEGATIVE,    // x < 0
    DEP_CONSTRAINT_EVEN,        // x % 2 == 0
    DEP_CONSTRAINT_ODD,         // x % 2 == 1
    DEP_CONSTRAINT_SIZE_EQ,     // len(x) == n
    DEP_CONSTRAINT_SIZE_LE,     // len(x) <= n
    DEP_CONSTRAINT_SIZE_GE,     // len(x) >= n
    DEP_CONSTRAINT_VALID_INDEX, // 0 <= x < len(array)
    DEP_CONSTRAINT_DIVISIBLE,   // x % n == 0
    DEP_CONSTRAINT_CUSTOM       // User-defined constraint expression
} DependentConstraintType;

typedef struct TypeConstraint {
    DependentConstraintType type;
    char* name;             // Optional constraint name
    SymbolicExpression* expression; // Constraint as symbolic expression
    
    union {
        struct {
            int64_t min_value;
            int64_t max_value;
        } range;
        
        struct {
            int64_t size;
        } size;
        
        struct {
            int64_t divisor;
        } divisible;
        
        struct {
            char* target_array;     // Name of array for index validation
        } valid_index;
    } data;
    
    struct TypeConstraint* next;
} TypeConstraint;

// =============================================================================
// Dependent Types
// =============================================================================

typedef enum {
    DEPENDENT_BOUNDED_VEC,      // BoundedVec<T, N>
    DEPENDENT_BOUNDED_INT,      // BoundedInt<Min, Max>
    DEPENDENT_SIZED_ARRAY,      // Array<T, N>
    DEPENDENT_REFINED_TYPE,     // T where constraint
    DEPENDENT_INDEXED_TYPE,     // T[index_type]
    DEPENDENT_FUNCTION_TYPE     // (x: T) -> U where constraint(x)
} DependentTypeKind;

typedef struct DependentType {
    DependentTypeKind kind;
    char* name;
    Type* base_type;            // The underlying type
    TypeParameter* parameters;  // Type and value parameters
    TypeConstraint* constraints; // Refinement constraints
    
    union {
        struct {
            Type* element_type;     // T
            int64_t capacity;       // N (compile-time constant)
            int is_capacity_dynamic; // Whether N is a type parameter
            char* capacity_param;   // Name of capacity parameter if dynamic
        } bounded_vec;
        
        struct {
            int64_t min_value;      // Min
            int64_t max_value;      // Max
            int is_min_dynamic;     // Whether Min is a type parameter
            int is_max_dynamic;     // Whether Max is a type parameter
            char* min_param;        // Name of min parameter if dynamic
            char* max_param;        // Name of max parameter if dynamic
        } bounded_int;
        
        struct {
            Type* element_type;     // T
            int64_t size;          // N
            int is_size_dynamic;   // Whether N is a type parameter
            char* size_param;      // Name of size parameter if dynamic
        } sized_array;
        
        struct {
            Type* refined_type;     // The type being refined
            TypeConstraint* refinements; // Additional constraints
        } refined;
        
        struct {
            Type* base_type;        // T
            Type* index_type;       // Index type (usually BoundedInt)
        } indexed;
        
        struct {
            TypeParameter* params;  // Function parameters with constraints
            Type* return_type;      // Return type (may also be dependent)
            TypeConstraint* preconditions;  // Requirements on parameters
            TypeConstraint* postconditions; // Guarantees on return value
        } function;
    } data;
} DependentType;

// =============================================================================
// Refinement Types (Predefined Common Types)
// =============================================================================

typedef struct RefinementType {
    char* name;
    Type* base_type;
    TypeConstraint* constraint;
    struct RefinementType* next;
} RefinementType;

// =============================================================================
// Dependent Type Context
// =============================================================================

typedef struct DependentTypeContext {
    TypeChecker* type_checker;
    ConstraintSolver* solver;
    
    // Type management
    DependentType** dependent_types;
    size_t dependent_type_count;
    size_t dependent_type_capacity;
    
    // Refinement types registry
    RefinementType* refinement_types;
    
    // Type parameter environment
    TypeParameter* type_env;
    
    // Constraint tracking
    TypeConstraint** active_constraints;
    size_t constraint_count;
    size_t constraint_capacity;
    
    // Configuration
    int enable_dependent_types;
    int enable_refinement_types;
    int enable_constraint_inference;
    int strict_constraint_checking;
    
    // Statistics
    size_t types_checked;
    size_t constraints_verified;
    size_t constraints_failed;
    size_t type_instantiations;
} DependentTypeContext;

// =============================================================================
// Constraint Solver
// =============================================================================

typedef enum {
    SOLVER_RESULT_SATISFIED,
    SOLVER_RESULT_UNSATISFIED,
    SOLVER_RESULT_UNKNOWN,
    SOLVER_RESULT_TIMEOUT,
    SOLVER_RESULT_ERROR
} SolverResult;

typedef struct ConstraintSolver {
    DependentTypeContext* context;
    
    // Solver configuration
    int enable_smt_backend;
    int enable_interval_analysis;
    int enable_symbolic_execution;
    double timeout_seconds;
    
    // Constraint database
    TypeConstraint** known_constraints;
    size_t known_constraint_count;
    size_t known_constraint_capacity;
    
    // Solver statistics
    size_t queries_solved;
    size_t queries_timeout;
    size_t queries_failed;
    double total_solve_time;
} ConstraintSolver;

// =============================================================================
// Core API Functions
// =============================================================================

// Context management
DependentTypeContext* dependent_type_context_new(TypeChecker* type_checker);
void dependent_type_context_free(DependentTypeContext* context);

// Dependent type creation
DependentType* dependent_type_new(DependentTypeKind kind, const char* name);
void dependent_type_free(DependentType* type);

// Bounded vector type: BoundedVec<T, N>
DependentType* create_bounded_vec_type(Type* element_type, int64_t capacity);
DependentType* create_dynamic_bounded_vec_type(Type* element_type, const char* capacity_param);

// Bounded integer type: BoundedInt<Min, Max>
DependentType* create_bounded_int_type(int64_t min_value, int64_t max_value);
DependentType* create_dynamic_bounded_int_type(const char* min_param, const char* max_param);

// Sized array type: Array<T, N>
DependentType* create_sized_array_type(Type* element_type, int64_t size);

// Refinement type creation
RefinementType* create_refinement_type(const char* name, Type* base_type, TypeConstraint* constraint);
void refinement_type_free(RefinementType* type);

// Common refinement types
RefinementType* create_non_zero_int_type(void);
RefinementType* create_positive_int_type(void);
RefinementType* create_negative_int_type(void);
RefinementType* create_even_int_type(void);
RefinementType* create_valid_index_type(const char* array_name);

// =============================================================================
// Type Constraint System
// =============================================================================

// =============================================================================
// Type Constraint System  
// =============================================================================

// Type constraint creation and management
TypeConstraint* type_constraint_new(DependentConstraintType type);
void type_constraint_free(TypeConstraint* constraint);

// Specific constraint creators
TypeConstraint* create_range_constraint(int64_t min_value, int64_t max_value);
TypeConstraint* create_non_zero_constraint(void);
TypeConstraint* create_positive_constraint(void);
TypeConstraint* create_size_constraint(DependentConstraintType size_type, int64_t size);
TypeConstraint* create_valid_index_constraint(const char* array_name);

// =============================================================================
// Dependent Types
// =============================================================================

// Dependent type creation and management
DependentType* dependent_type_new(DependentTypeKind kind, const char* name);
void dependent_type_free(DependentType* type);

// Specific dependent type creators
DependentType* create_bounded_vec_type(Type* element_type, int64_t capacity);
DependentType* create_dynamic_bounded_vec_type(Type* element_type, const char* capacity_param);
DependentType* create_bounded_int_type(int64_t min_value, int64_t max_value);
DependentType* create_dynamic_bounded_int_type(const char* min_param, const char* max_param);
DependentType* create_sized_array_type(Type* element_type, int64_t size);

// =============================================================================
// Refinement Types
// =============================================================================

// Refinement type creation and management
RefinementType* create_refinement_type(const char* name, Type* base_type, TypeConstraint* constraint);
void refinement_type_free(RefinementType* type);

// Specific refinement type creators
RefinementType* create_non_zero_int_type(void);
RefinementType* create_positive_int_type(void);
RefinementType* create_negative_int_type(void);
RefinementType* create_even_int_type(void);
RefinementType* create_valid_index_type(const char* array_name);

// =============================================================================
// Type Checking Integration
// =============================================================================

// Type parameter handling
TypeParameter* type_parameter_new(TypeParameterKind kind, const char* name);
void type_parameter_free(TypeParameter* param);
int bind_type_parameter(DependentTypeContext* context, TypeParameter* param, Type* type);
int bind_value_parameter(DependentTypeContext* context, TypeParameter* param, int64_t value);

// Dependent type instantiation
Type* instantiate_dependent_type(DependentTypeContext* context, DependentType* dep_type,
                                TypeParameter* args);
int check_dependent_type_constraints(DependentTypeContext* context, DependentType* dep_type,
                                    Type* instance_type, ASTNode* expr);

// Type checking with dependent types
int check_bounded_vec_operations(DependentTypeContext* context, Type* vec_type, 
                                ASTNode* operation);
int check_bounded_int_operations(DependentTypeContext* context, Type* int_type,
                                ASTNode* operation);
int check_array_access_with_dependent_types(DependentTypeContext* context, 
                                           Type* array_type, Type* index_type, ASTNode* access);

// =============================================================================
// Constraint Solver
// =============================================================================

ConstraintSolver* constraint_solver_new(DependentTypeContext* context);
void constraint_solver_free(ConstraintSolver* solver);

SolverResult solve_constraint(ConstraintSolver* solver, TypeConstraint* constraint,
                             Type* type, ASTNode* expr);
SolverResult solve_constraint_set(ConstraintSolver* solver, TypeConstraint** constraints,
                                 size_t constraint_count, Type** types, ASTNode** exprs);

int constraint_entails(ConstraintSolver* solver, TypeConstraint* premise, TypeConstraint* conclusion);
int constraints_consistent(ConstraintSolver* solver, TypeConstraint** constraints, size_t count);

// =============================================================================
// Built-in Type Definitions
// =============================================================================

// Register built-in dependent and refinement types
void register_builtin_dependent_types(DependentTypeContext* context);
void register_builtin_refinement_types(DependentTypeContext* context);

// Common type lookups
DependentType* lookup_dependent_type(DependentTypeContext* context, const char* name);
RefinementType* lookup_refinement_type(DependentTypeContext* context, const char* name);

// =============================================================================
// Utility Functions
// =============================================================================

// Type constraint utilities
char* type_constraint_to_string(const TypeConstraint* constraint);
char* dependent_type_to_string(const DependentType* type);
char* refinement_type_to_string(const RefinementType* type);

const char* dependent_type_kind_to_string(DependentTypeKind kind);
const char* dependent_constraint_type_to_string(DependentConstraintType type);
const char* solver_result_to_string(SolverResult result);

// Statistics and debugging
void dependent_type_context_print_statistics(DependentTypeContext* context);
void constraint_solver_print_statistics(ConstraintSolver* solver);
void print_dependent_type_info(DependentType* type);
void print_type_constraint_info(TypeConstraint* constraint);

// Configuration
void dependent_type_context_enable_feature(DependentTypeContext* context, 
                                          const char* feature, int enable);
void constraint_solver_set_timeout(ConstraintSolver* solver, double timeout_seconds);

#endif // DEPENDENT_TYPES_H