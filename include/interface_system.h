#ifndef INTERFACE_SYSTEM_H
#define INTERFACE_SYSTEM_H

#include "types.h"
#include "ast.h"
#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct ConstraintSet ConstraintSet;
typedef struct InterfaceConstraint InterfaceConstraint;
typedef struct ConceptDefinition ConceptDefinition;
typedef struct ProtocolDefinition ProtocolDefinition;
typedef struct TypeVariable TypeVariable;
typedef struct ConstraintInferenceEngine ConstraintInferenceEngine;
typedef struct ConceptRegistry ConceptRegistry;
typedef struct HKTRegistry HKTRegistry;
typedef struct ProtocolRegistry ProtocolRegistry;

// Enhanced interface system components for Task #22

// =============================================================================
// Higher-Kinded Type Definitions (Task 22.3)
// =============================================================================

// Higher-kinded type kinds
typedef enum {
    HKT_TYPE,                   // Regular type: *
    HKT_TYPE_TO_TYPE,           // Type constructor: * -> *
    HKT_TYPE_TO_TYPE_TO_TYPE,   // Binary type constructor: * -> * -> *  
    HKT_CONSTRAINT,             // Constraint kind
    HKT_ROW,                    // Row kind (for extensible records)
    HKT_EFFECT                  // Effect kind (for effect systems)
} HigherKindedTypeKind;

// Higher-kinded type information
typedef struct {
    HigherKindedTypeKind kind;          // Kind of this HKT
    char* name;                         // Name of the HKT (e.g., "Option", "Vec")
    Type* type_constructor;             // The type constructor (e.g., Vec, Option)
    Type** type_arguments;              // Applied type arguments
    size_t arity;                       // Number of type parameters
    char* kind_signature;               // String representation of kind (e.g., "* -> * -> *")
} HigherKindedType;

// =============================================================================
// Registry Structures for Task 22 Enhanced Interface System
// =============================================================================

// Concept registry for managing concept definitions
struct ConceptRegistry {
    ConceptDefinition** concepts;      // Array of concept definitions
    size_t concept_count;              // Number of registered concepts
    size_t capacity;                   // Capacity of the concepts array
};

// Higher-kinded type registry for managing HKT constructors
struct HKTRegistry {
    HigherKindedType** hkts;           // Array of higher-kinded type constructors
    size_t hkt_count;                  // Number of registered HKTs
    size_t capacity;                   // Capacity of the HKTs array
};

// Protocol conformance information
typedef struct {
    Type* conforming_type;              // Type that conforms to the protocol
    ProtocolDefinition* protocol;       // The protocol being conformed to
    InterfaceMethod* method_implementations; // Method implementations
    Type** associated_type_bindings;    // Bindings for associated types
    int is_auto_generated;              // Whether this conformance was auto-generated
    int is_retroactive;                 // Whether this is retroactive conformance
    Position conformance_pos;           // Where conformance was declared/inferred
} ProtocolConformance;

// Protocol registry for managing protocol definitions
struct ProtocolRegistry {
    ProtocolDefinition** protocols;        // Array of protocol definitions
    size_t protocol_count;                 // Number of registered protocols
    size_t protocol_capacity;              // Capacity of the protocols array
    ProtocolConformance** conformances;    // Array of protocol conformances
    size_t conformance_count;              // Number of registered conformances
    size_t conformance_capacity;           // Capacity of the conformances array
};

// =============================================================================
// 22.1: Automatic Constraint Inference System
// =============================================================================

// Type variable kinds for generics
typedef enum {
    TYPE_VAR_GENERIC,           // Regular generic type variable T
    TYPE_VAR_CONST,             // Const generic variable (e.g., N: usize) 
    TYPE_VAR_LIFETIME,          // Lifetime variable (for advanced borrowing)
    TYPE_VAR_HIGHER_KINDED,     // Higher-kinded type variable (e.g., F<_>)
    TYPE_VAR_ASSOCIATED         // Associated type variable
} TypeVariableKind;

// Type variable representing generic parameters
struct TypeVariable {
    char* name;                 // Variable name (T, U, N, etc.)
    TypeVariableKind kind;      // Kind of type variable
    Type* bound_type;           // Bound type if constrained
    ConstraintSet* constraints; // Set of constraints on this variable
    int is_inferred;            // Whether this was automatically inferred
    Position declared_pos;      // Where this variable was declared
    struct TypeVariable* next;  // For linked lists
};

// Constraint types that can be inferred
typedef enum {
    CONSTRAINT_IMPLEMENTS,      // T: Display, T implements Display
    CONSTRAINT_SUBTYPE,         // T <: U, T is subtype of U  
    CONSTRAINT_EQUALITY,        // T = U, T equals U
    CONSTRAINT_SIZE,            // T: Sized, T has known size at compile time
    CONSTRAINT_COPY,            // T: Copy, T can be copied
    CONSTRAINT_CLONE,           // T: Clone, T can be cloned
    CONSTRAINT_SEND,            // T: Send, T can be sent between threads
    CONSTRAINT_SYNC,            // T: Sync, T can be shared between threads
    CONSTRAINT_DEFAULT,         // T: Default, T has default value
    CONSTRAINT_PARTIAL_EQ,      // T: PartialEq, T supports equality comparison
    CONSTRAINT_PARTIAL_ORD,     // T: PartialOrd, T supports ordering comparison
    CONSTRAINT_HASH,            // T: Hash, T can be hashed
    CONSTRAINT_DEBUG,           // T: Debug, T can be formatted for debugging
    CONSTRAINT_DISPLAY,         // T: Display, T can be formatted for users
    CONSTRAINT_SERIALIZABLE,    // T: Serializable, T can be serialized
    CONSTRAINT_ITERATOR,        // T: Iterator, T is an iterator
    CONSTRAINT_INTO,            // T: Into<U>, T can be converted into U
    CONSTRAINT_FROM,            // T: From<U>, T can be created from U
    CONSTRAINT_TRY_FROM,        // T: TryFrom<U>, T can be fallibly created from U
    CONSTRAINT_TRY_INTO,        // T: TryInto<U>, T can be fallibly converted into U
    CONSTRAINT_AS_REF,          // T: AsRef<U>, T can be referenced as U
    CONSTRAINT_AS_MUT,          // T: AsMut<U>, T can be mutably referenced as U
    CONSTRAINT_DEREF,           // T: Deref<Target=U>, T can be dereferenced to U
    CONSTRAINT_DEREF_MUT,       // T: DerefMut<Target=U>, T can be mutably dereferenced to U
    CONSTRAINT_INDEX,           // T: Index<Idx, Output=U>, T supports indexing
    CONSTRAINT_INDEX_MUT,       // T: IndexMut<Idx, Output=U>, T supports mutable indexing
    CONSTRAINT_ARITHMETIC,      // T: Add + Sub + Mul + Div, T supports arithmetic
    CONSTRAINT_NUMERIC,         // T: Numeric, T is a numeric type
    CONSTRAINT_INTEGRAL,        // T: Integral, T is an integral type
    CONSTRAINT_FLOATING,        // T: Floating, T is a floating-point type
    CONSTRAINT_CALLABLE,        // T: Fn(Args) -> Return, T is callable
    CONSTRAINT_ASYNC_CALLABLE,  // T: AsyncFn(Args) -> Future<Return>, T is async callable
    CONSTRAINT_GENERATOR,       // T: Generator<Yield, Return>, T is a generator
    CONSTRAINT_CONST_EVAL,      // T: ConstEval, T can be evaluated at compile time
    CONSTRAINT_CONST_SIZE,      // T: ConstSize<N>, T has compile-time size N
    CONSTRAINT_HIGHER_KINDED,   // F: HKT<_>, F is a higher-kinded type
    CONSTRAINT_ASSOCIATED_TYPE, // T::Item: U, associated type constraint
    CONSTRAINT_LIFETIME,        // 'a: 'b, lifetime constraint  
    CONSTRAINT_MEMORY_LAYOUT,   // T: Layout<align=N, size=M>, memory layout constraint
    CONSTRAINT_PROTOCOL         // T: Protocol, protocol conformance constraint
} ConstraintKind;

// Individual constraint on a type
struct InterfaceConstraint {
    ConstraintKind kind;            // Kind of constraint
    Type* constrained_type;         // Type being constrained (usually a type variable)
    Type* target_type;              // Target type for the constraint (if applicable)
    char* protocol_name;            // Protocol/interface name (for CONSTRAINT_IMPLEMENTS)
    char* associated_type_name;     // Associated type name (for CONSTRAINT_ASSOCIATED_TYPE)
    void* constraint_data;          // Additional constraint-specific data
    int is_auto_inferred;           // Whether this constraint was automatically inferred
    int is_resolved;                // Whether this constraint has been resolved
    Position source_pos;            // Where this constraint originated
    struct InterfaceConstraint* next; // For linked lists
};

// Set of constraints
struct ConstraintSet {
    InterfaceConstraint* constraints;   // List of constraints
    size_t count;                       // Number of constraints
    int is_satisfied;                   // Whether all constraints are satisfied
    char* error_message;                // Error message if constraints can't be satisfied
};

// Constraint inference engine
struct ConstraintInferenceEngine {
    TypeChecker* type_checker;          // Associated type checker
    TypeVariable* type_variables;       // Currently active type variables
    ConstraintSet* active_constraints;  // Currently active constraint set
    int inference_depth;                // Current inference recursion depth
    int max_inference_depth;            // Maximum allowed inference depth
    
    // Inference statistics
    size_t constraints_inferred;        // Number of constraints automatically inferred
    size_t type_variables_inferred;     // Number of type variables inferred
    size_t substitutions_made;          // Number of type substitutions made
    
    // Caches for performance
    Type** inferred_types_cache;        // Cache of inferred types
    size_t cache_size;
    size_t cache_capacity;
};

// =============================================================================
// 22.2: Concept-Based Generics Framework  
// =============================================================================

// Concept definition (similar to C++20 concepts or Rust traits)
struct ConceptDefinition {
    char* name;                         // Concept name
    TypeVariable* type_parameters;      // Type parameters for the concept
    ConstraintSet* requirements;        // Requirements that must be satisfied
    InterfaceMethod* required_methods;  // Required methods
    Type** associated_types;            // Associated types
    size_t associated_type_count;
    ConceptDefinition** super_concepts; // Concepts this concept extends
    size_t super_concept_count;
    int is_auto_concept;                // Whether this is an auto-implemented concept
    Position defined_pos;               // Where this concept was defined
    struct ConceptDefinition* next;     // For linked lists
};

// =============================================================================
// 22.3: Higher-Kinded Type Support
// =============================================================================

// =============================================================================
// 22.4: Type-Level Programming Capabilities
// =============================================================================

// Type-level computation kinds
typedef enum {
    TYPE_LEVEL_CONST,           // Compile-time constant
    TYPE_LEVEL_FUNCTION,        // Type-level function
    TYPE_LEVEL_DEPENDENT,       // Dependent type
    TYPE_LEVEL_FAMILY,          // Type family
    TYPE_LEVEL_ASSOCIATED       // Associated type projection
} TypeLevelComputationKind;

// Type-level computation
typedef struct {
    TypeLevelComputationKind kind;      // Kind of computation
    char* name;                         // Name of the computation
    TypeVariable* parameters;           // Type parameters
    ASTNode* body;                      // Computation body (AST)
    Type* result_type;                  // Result type (if known)
    int is_const_evaluable;             // Whether this can be evaluated at compile time
} TypeLevelComputation;

// Forward declarations for advanced type-level programming
typedef struct TypeComputationCache TypeComputationCache;
typedef struct CachedTypeFamily CachedTypeFamily;
typedef struct TypeFamily TypeFamily;
typedef struct TypePattern TypePattern;
typedef struct TypeFamilyCase TypeFamilyCase;
typedef struct TypeLevelNat TypeLevelNat;
typedef struct PatternEnv PatternEnv;
typedef struct DependentType DependentType;
typedef struct DependentConstraint DependentConstraint;
typedef struct DependentVector DependentVector;
typedef struct TypeEvalContext TypeEvalContext;

// Type pattern kinds for type family matching
typedef enum {
    TYPE_PATTERN_WILDCARD,    // _
    TYPE_PATTERN_VARIABLE,    // T, N
    TYPE_PATTERN_CONSTRUCTOR, // Zero, Succ(n), Cons(h, t)
    TYPE_PATTERN_LITERAL,     // 42, "hello"
    TYPE_PATTERN_APPLICATION  // F<T>, Add<A, B>
} TypePatternKind;

// Dependent type kinds
typedef enum {
    DEPENDENT_ARRAY,        // Array with size dependent on value
    DEPENDENT_VECTOR,       // Vector with capacity dependent on value
    DEPENDENT_REFINEMENT,   // Refinement type
    DEPENDENT_PROOF,        // Proof type
    DEPENDENT_PATH,         // Path dependent type
    DEPENDENT_FAMILY        // Type family instance
} DependentTypeKind;

// =============================================================================
// 22.5: Protocol-Oriented Programming System
// =============================================================================

// Protocol definition (enhanced interface with automatic conformance)
struct ProtocolDefinition {
    char* name;                         // Protocol name
    TypeVariable* type_parameters;      // Generic type parameters
    InterfaceMethod* required_methods;  // Required methods
    InterfaceMethod* default_methods;   // Default method implementations
    Type** associated_types;            // Associated types
    size_t associated_type_count;
    ConstraintSet* where_clause;        // Where clause constraints
    ProtocolDefinition* inherited_protocols; // Inherited protocols
    size_t inherited_count;
    int allows_retroactive_conformance; // Whether retroactive conformance is allowed
    int is_auto_conformance;            // Whether conformance is automatically inferred
    Position defined_pos;               // Where this protocol was defined
    struct ProtocolDefinition* next;    // For linked lists
};

// =============================================================================
// Automatic Trait Bound Generation Data Structures
// =============================================================================

// Trait bound kinds
typedef enum {
    TRAIT_BOUND_SIMPLE,        // T: Display
    TRAIT_BOUND_PARAMETRIC,    // T: Into<U>
    TRAIT_BOUND_ASSOCIATED,    // T: Iterator<Item = U>
    TRAIT_BOUND_WHERE_CLAUSE,  // Complex where clause bound
    TRAIT_BOUND_LIFETIME       // Lifetime bound
} TraitBoundKind;

// Individual trait bound
typedef struct TraitBound {
    TraitBoundKind kind;              // Kind of trait bound
    char* type_param_name;            // Type parameter name (T, U, etc.)
    char* trait_name;                 // Trait name (Display, Clone, etc.)
    Type* type_parameter;             // Type parameter for parametric bounds
    char* associated_type_name;       // Associated type name
    Type* associated_type;            // Associated type
    int is_auto_generated;            // Whether this was automatically generated
    float confidence_score;           // Confidence in this bound (0.0-1.0)
    Position source_pos;              // Where this bound originated
    struct TraitBound* next;          // For linked lists
} TraitBound;

// Set of trait bounds
typedef struct TraitBoundSet {
    TraitBound* bounds;               // List of trait bounds
    size_t count;                     // Number of bounds
    int is_optimized;                 // Whether bounds have been optimized
    char* generated_where_clause;     // Generated where clause string
} TraitBoundSet;

// =============================================================================
// Type-Level Programming Function Declarations
// =============================================================================

// Type-level computation management
TypeLevelComputation* type_level_computation_new(TypeLevelComputationKind kind, const char* name);
void type_level_computation_free(TypeLevelComputation* computation);
Type* evaluate_type_level_computation(TypeLevelComputation* computation, TypeChecker* checker);
int type_level_computation_is_const_evaluable(TypeLevelComputation* computation);

// Advanced type-level programming features
TypeLevelComputation* create_compile_time_constraint(const char* constraint_name, TypeVariable* type_var, ASTNode* condition_expr);
TypeLevelComputation* create_higher_order_type_function(const char* name, TypeVariable* func_param, TypeVariable* type_param);
Type* create_phantom_type(const char* name, Type* phantom_param);
Type* create_static_assert_type(const char* assertion_name, ASTNode* condition, const char* error_message);

// Type computation caching
TypeComputationCache* type_computation_cache_new(void);
void type_computation_cache_free(TypeComputationCache* cache);
Type* type_computation_cache_lookup(TypeComputationCache* cache, const char* signature);
int type_computation_cache_store(TypeComputationCache* cache, const char* signature, Type* result);

// Enhanced type families with caching
CachedTypeFamily* cached_type_family_new(TypeFamily* family);
void cached_type_family_free(CachedTypeFamily* cached);

// Type families and pattern matching
TypeFamily* type_family_new(const char* name);
void type_family_free(TypeFamily* family);
int type_family_add_case(TypeFamily* family, TypePattern* pattern, TypeLevelComputation* result);
Type* type_family_evaluate(TypeFamily* family, Type** arguments, size_t arg_count, TypeChecker* checker);

// Built-in type families
TypeFamily* create_add_type_family(void);
TypeFamily* create_mul_type_family(void);
TypeFamily* create_equal_type_family(void);

// Const generics support
TypeVariable* create_const_generic_parameter(const char* name, Type* const_type, Position pos);
Type* evaluate_const_generic(ASTNode* expr, TypeChecker* checker);

// Type-level natural numbers
TypeLevelNat* type_level_nat_zero(void);
TypeLevelNat* type_level_nat_succ(TypeLevelNat* n);
void type_level_nat_free(TypeLevelNat* nat);
TypeLevelNat* type_level_nat_add(TypeLevelNat* a, TypeLevelNat* b);

// Pattern matching for type families
TypePattern* type_pattern_new(TypePatternKind kind, const char* name);
void type_pattern_free(TypePattern* pattern);
int type_pattern_add_subpattern(TypePattern* pattern, TypePattern* subpattern);
int type_matches_pattern(Type* type, TypePattern* pattern, PatternEnv* env);

// Pattern environment management
PatternEnv* pattern_env_new(void);
void pattern_env_free(PatternEnv* env);
int pattern_env_bind(PatternEnv* env, const char* name, Type* type);

// Dependent types
DependentType* dependent_type_new(DependentTypeKind kind, const char* name, Type* base_type);
void dependent_type_free(DependentType* dep_type);
int dependent_type_add_constraint(DependentType* dep_type, const char* name, ASTNode* constraint_expr, Type* constrained_type);
TypeLevelComputation* create_dependent_type(const char* name, TypeVariable* value_param, ASTNode* type_expr);

// Compile-time array and matrix types
Type* create_compile_time_array_type(Type* element_type, TypeLevelNat* size);
Type* create_compile_time_matrix_type(Type* element_type, TypeLevelNat* rows, TypeLevelNat* cols);
Type* create_matrix_type(size_t rows, size_t cols, Type* element_type);
TypeLevelComputation* create_matrix_multiply_dimensions(TypeLevelComputation* left_rows, TypeLevelComputation* left_cols, TypeLevelComputation* right_rows, TypeLevelComputation* right_cols);

// Dependent vectors and advanced dependent types
DependentVector* dependent_vector_new(Type* element_type, size_t initial_capacity);
void dependent_vector_free(DependentVector* vec);
Type* dependent_vector_to_type(DependentVector* vec);
Type* create_dependent_vector_type(Type* element_type, ASTNode* length_expr, TypeChecker* checker);

// Proof types and verification
Type* create_proof_type(const char* proposition);
Type* create_safe_array_access_type(Type* array_type, ASTNode* index_expr, ASTNode* bounds_proof);

// Associated types and projections
TypeLevelComputation* create_associated_type_projection(const char* trait_name, const char* assoc_type_name, Type* self_type);
Type* resolve_associated_type_projection(TypeLevelComputation* projection, TypeChecker* checker);

// Type evaluation context and engine
TypeEvalContext* type_eval_context_new(TypeChecker* checker);
void type_eval_context_free(TypeEvalContext* ctx);
int type_eval_context_add_family(TypeEvalContext* ctx, TypeFamily* family);
TypeLevelNat* type_eval_context_get_nat(TypeEvalContext* ctx, size_t value);
Type* evaluate_type_expression(TypeEvalContext* ctx, ASTNode* expr);
int type_eval_context_init_builtins(TypeEvalContext* ctx);
Type* evaluate_type_level_computation_full(TypeLevelComputation* computation, TypeEvalContext* ctx);
int is_compile_time_evaluable(TypeEvalContext* ctx, ASTNode* expr);

// Type-level computation utilities
const char* type_level_computation_kind_to_string(TypeLevelComputationKind kind);
void print_type_level_computation(const TypeLevelComputation* computation);
int type_level_computations_equivalent(const TypeLevelComputation* comp1, const TypeLevelComputation* comp2);
TypeLevelComputation* substitute_in_type_level_computation(TypeLevelComputation* computation, TypeVariable* from_var, Type* to_type);

// =============================================================================
// Protocol-Oriented Programming Function Declarations  
// =============================================================================

// Protocol definition management
ProtocolDefinition* protocol_definition_new(const char* name, Position pos);
void protocol_definition_free(ProtocolDefinition* protocol);
int protocol_add_required_method(ProtocolDefinition* protocol, InterfaceMethod* method);
int protocol_add_default_method(ProtocolDefinition* protocol, InterfaceMethod* method);
int protocol_add_associated_type(ProtocolDefinition* protocol, Type* associated_type);
int protocol_add_type_parameter(ProtocolDefinition* protocol, const char* param_name, TypeVariableKind kind, Position pos);
int protocol_add_where_clause(ProtocolDefinition* protocol, ConstraintSet* constraints);

// Protocol inheritance and composition
int protocol_add_inherited_protocol(ProtocolDefinition* protocol, ProtocolDefinition* inherited);
int protocol_inherits_from(ProtocolDefinition* protocol, ProtocolDefinition* ancestor);
InterfaceMethod* protocol_get_all_methods(ProtocolDefinition* protocol);

// Protocol conformance management
ProtocolConformance* protocol_conformance_new(Type* type, ProtocolDefinition* protocol, Position pos);
void protocol_conformance_free(ProtocolConformance* conformance);
int protocol_conformance_add_method_implementation(ProtocolConformance* conformance, InterfaceMethod* implementation);
int protocol_conformance_add_associated_type_binding(ProtocolConformance* conformance, size_t index, Type* binding);

// Protocol registry management
ProtocolRegistry* protocol_registry_new(void);
void protocol_registry_free(ProtocolRegistry* registry);
int protocol_registry_add_protocol(ProtocolRegistry* registry, ProtocolDefinition* protocol);
int protocol_registry_add_conformance(ProtocolRegistry* registry, ProtocolConformance* conformance);
ProtocolDefinition* protocol_registry_find_protocol(ProtocolRegistry* registry, const char* name);
ProtocolConformance* protocol_registry_find_conformance(ProtocolRegistry* registry, Type* type, ProtocolDefinition* protocol);

// Retroactive conformance support
int protocol_add_retroactive_conformance(ProtocolRegistry* registry, Type* type, ProtocolDefinition* protocol, Position pos);
int can_add_retroactive_conformance(Type* type, ProtocolDefinition* protocol, TypeChecker* checker);

// Automatic conformance inference
int infer_protocol_conformances(ProtocolRegistry* registry, Type* type, TypeChecker* checker);
int type_auto_conforms_to_protocol(Type* type, ProtocolDefinition* protocol, TypeChecker* checker);

// Protocol conformance checking
int type_satisfies_protocol_requirements(Type* type, ProtocolDefinition* protocol, TypeChecker* checker);
int type_satisfies_associated_type_requirements(Type* type, ProtocolDefinition* protocol, TypeChecker* checker);
int constraint_set_is_satisfied_for_type(ConstraintSet* constraints, Type* type, TypeChecker* checker);
int constraint_is_satisfied_for_type(InterfaceConstraint* constraint, Type* type, TypeChecker* checker);

// Helper functions for constraint checking
int type_has_unique_ownership(Type* type);
int type_supports_arithmetic(Type* type);

// Automatic conformance detection
int type_has_required_methods(Type* type, ProtocolDefinition* protocol, TypeChecker* checker);
int type_has_method(Type* type, const char* method_name, Type* method_signature, TypeChecker* checker);
int type_has_builtin_method(Type* type, const char* method_name);

// Standard protocol library
ProtocolDefinition* create_equatable_protocol(Position pos);
ProtocolDefinition* create_comparable_protocol(Position pos);
ProtocolDefinition* create_hashable_protocol(Position pos);
ProtocolDefinition* create_displayable_protocol(Position pos);
ProtocolDefinition* create_cloneable_protocol(Position pos);
ProtocolDefinition* create_iterator_protocol(Position pos);

// Interface method management
InterfaceMethod* interface_method_new(const char* name, Type* type);
void interface_method_free(InterfaceMethod* method);
InterfaceMethod* interface_method_copy(const InterfaceMethod* method);

// Protocol-oriented programming utilities
int protocol_registry_init_standard_protocols(ProtocolRegistry* registry);
int type_conforms_to_protocol(Type* type, ProtocolDefinition* protocol, ProtocolRegistry* registry, TypeChecker* checker);
ProtocolDefinition** get_type_conforming_protocols(Type* type, ProtocolRegistry* registry, TypeChecker* checker, size_t* count);
void print_protocol_definition(const ProtocolDefinition* protocol);

// =============================================================================
// Function Declarations
// =============================================================================

// Constraint inference engine functions
ConstraintInferenceEngine* constraint_inference_engine_new(TypeChecker* type_checker);
void constraint_inference_engine_free(ConstraintInferenceEngine* engine);

// Type variable management
TypeVariable* type_variable_new(const char* name, TypeVariableKind kind, Position pos);
void type_variable_free(TypeVariable* var);
TypeVariable* type_variable_copy(const TypeVariable* var);
int type_variable_add_constraint(TypeVariable* var, InterfaceConstraint* constraint);
TypeVariable* constraint_inference_engine_add_type_variable(ConstraintInferenceEngine* engine, 
                                                           const char* name, TypeVariableKind kind, Position pos);

// Constraint management
InterfaceConstraint* interface_constraint_new(ConstraintKind kind, Type* constrained_type, Position pos);
void interface_constraint_free(InterfaceConstraint* constraint);

// Helper functions for constraint management
InterfaceConstraint* interface_constraint_copy(const InterfaceConstraint* constraint);
int constraint_set_add(ConstraintSet* set, InterfaceConstraint* constraint);

ConstraintSet* constraint_set_new(void);
void constraint_set_free(ConstraintSet* set);
int constraint_set_add(ConstraintSet* set, InterfaceConstraint* constraint);
int constraint_set_merge(ConstraintSet* dest, const ConstraintSet* src);
int constraint_set_is_satisfied(const ConstraintSet* set, TypeChecker* checker);

// Automatic constraint inference
int infer_constraints_from_expression(ConstraintInferenceEngine* engine, ASTNode* expr);
int infer_constraints_from_function_call(ConstraintInferenceEngine* engine, ASTNode* call_expr);
int infer_constraints_from_binary_operation(ConstraintInferenceEngine* engine, ASTNode* binary_expr);
int infer_constraints_from_assignment(ConstraintInferenceEngine* engine, ASTNode* assignment);
int infer_constraints_from_return_statement(ConstraintInferenceEngine* engine, ASTNode* return_stmt);
int infer_constraints_from_arithmetic_context(ConstraintInferenceEngine* engine, Type* type, Position pos);
int infer_constraints_from_comparison_context(ConstraintInferenceEngine* engine, Type* type, Position pos);
int infer_constraints_from_usage_pattern(ConstraintInferenceEngine* engine, Type* type, const char* usage_pattern, Position pos);

// Constraint solving
int solve_constraints(ConstraintInferenceEngine* engine);
int propagate_constraints(ConstraintInferenceEngine* engine);
int unify_types_with_constraints(ConstraintInferenceEngine* engine, Type* type1, Type* type2);
Type* substitute_type_variables(ConstraintInferenceEngine* engine, Type* type);

// Concept-based generics
ConceptDefinition* concept_definition_new(const char* name, Position pos);
void concept_definition_free(ConceptDefinition* concept);
int concept_add_requirement(ConceptDefinition* concept, InterfaceConstraint* requirement);
int concept_add_super_concept(ConceptDefinition* concept, ConceptDefinition* super_concept);
int type_satisfies_concept(Type* type, ConceptDefinition* concept, TypeChecker* checker);

// Enhanced concept operations
int concept_add_method_requirement(ConceptDefinition* concept, const char* method_name, Type* method_signature, Position pos);
int concept_add_associated_type(ConceptDefinition* concept, Type* associated_type);
int concept_add_type_parameter(ConceptDefinition* concept, const char* param_name, TypeVariableKind kind, Position pos);
int concept_is_well_formed(ConceptDefinition* concept);
int concept_check_circular_dependencies(ConceptDefinition* concept, ConceptDefinition** visited, size_t visited_count);
int type_satisfies_concept_enhanced(Type* type, ConceptDefinition* concept, TypeChecker* checker);
int type_has_method(Type* type, const char* method_name, Type* method_signature, TypeChecker* checker);
int type_has_associated_type(Type* type, Type* associated_type, TypeChecker* checker);

// Concept composition and refinement
ConceptDefinition* create_concept_composition(const char* name, ConceptDefinition** base_concepts, size_t base_count, Position pos);
ConceptDefinition* create_concept_refinement(const char* name, ConceptDefinition* base_concept, InterfaceConstraint** additional_constraints, size_t constraint_count, Position pos);

// Advanced concept library
ConceptDefinition* create_functor_concept(Position pos);
ConceptDefinition* create_monad_concept(Position pos);
ConceptDefinition* create_serializable_concept(Position pos);

// Standard concept library
ConceptDefinition* create_numeric_concept(Position pos);
ConceptDefinition* create_comparable_concept(Position pos);
ConceptDefinition* create_copyable_concept(Position pos);
ConceptDefinition* create_displayable_concept(Position pos);
ConceptDefinition* create_iterator_concept(Position pos);
ConceptDefinition* create_container_concept(Position pos);

// Concept inference and instantiation
ConceptDefinition** infer_type_concepts(Type* type, TypeChecker* checker, size_t* concept_count);
Type* create_concept_constrained_function(ConceptDefinition* concepts[], size_t concept_count, Type** param_types, size_t param_count, Type* return_type);
int can_instantiate_generic_function(Type* generic_func_type, Type** type_arguments, size_t arg_count, ConceptDefinition** constraints, size_t constraint_count, TypeChecker* checker);
Type* instantiate_generic_function(Type* generic_func_type, Type** type_arguments, size_t arg_count);
int check_concept_constrained_call(Type* function_type, Type** arg_types, size_t arg_count, ConceptDefinition* concepts[], size_t concept_count, TypeChecker* checker);

// Concept utility functions
int concept_is_subtype_of(ConceptDefinition* sub_concept, ConceptDefinition* super_concept);
void print_concept_definition(const ConceptDefinition* concept);

// =============================================================================
// Task 22.7: Complete Concept-Based Generics Implementation
// =============================================================================

// Requires block functionality (22.7.1)
int extract_concept_requirements(ConceptDefinition* concept, ASTNode* requirements_ast, TypeChecker* checker);
InterfaceConstraint* interface_constraint_from_name(const char* name, Position pos);

// Automatic interface synthesis (22.7.2)
Type* synthesize_interface_from_concept(ConceptDefinition* concept, TypeChecker* checker);
int generate_common_operations(ConceptDefinition* concept, Type* target_type, TypeChecker* checker);
int concept_has_constraint(ConceptDefinition* concept, ConstraintKind kind);
int add_zero_operation(Type* type, TypeChecker* checker);
int add_copy_operation(Type* type, TypeChecker* checker);
int add_display_operation(Type* type, TypeChecker* checker);
int add_comparison_operations(Type* type, TypeChecker* checker);

// Enhanced concept conformance detection (22.7.3)
int auto_generate_concept_method(Type* type, InterfaceMethod* method, ConceptDefinition* concept, TypeChecker* checker);
int generate_default_method_implementation(Type* type, InterfaceMethod* method, TypeChecker* checker);

// Concept constraints in generic functions (22.7.5)
Type* create_concept_constrained_function_enhanced(const char* func_name, ConceptDefinition** concept_constraints, size_t constraint_count, Type** param_types, size_t param_count, Type* return_type, Position pos);
int validate_concept_constraints_on_instantiation(Type* func_type, Type** arg_types, size_t arg_count, TypeChecker* checker);

// Integration with constraint inference system (22.7.6)
int integrate_concepts_with_constraint_inference(ConceptDefinition* concept, ConstraintInferenceEngine* engine, TypeChecker* checker);
int register_concept_for_inference(ConstraintInferenceEngine* engine, ConceptDefinition* concept);
ConceptDefinition** infer_concept_constraints_from_usage(Type* type, ASTNode* usage_context, TypeChecker* checker, size_t* concept_count);

// AST usage analysis helpers
int ast_uses_arithmetic_operations(ASTNode* node, Type* target_type);
int ast_uses_comparison_operations(ASTNode* node, Type* target_type);
int ast_uses_copy_operations(ASTNode* node, Type* target_type);

// =============================================================================
// Task 22.3: Higher-Kinded Type Support Functions
// =============================================================================

// Higher-kinded type management
HigherKindedType* higher_kinded_type_new(HigherKindedTypeKind kind, Type* type_constructor);
void higher_kinded_type_free(HigherKindedType* hkt);
int higher_kinded_type_apply(HigherKindedType* hkt, Type* argument);
Type* higher_kinded_type_instantiate(HigherKindedType* hkt, Type** arguments, size_t arg_count);

// Kind inference and checking
HigherKindedTypeKind infer_type_kind(Type* type);
int kinds_compatible(HigherKindedTypeKind kind1, HigherKindedTypeKind kind2);
HigherKindedType* type_to_higher_kinded(Type* type);

// Common higher-kinded type constructors
HigherKindedType* create_option_hkt(void);
HigherKindedType* create_result_hkt(void);
HigherKindedType* create_vec_hkt(void);
HigherKindedType* create_map_hkt(void);
HigherKindedType* create_function_hkt(void);

// Higher-kinded type application and composition
HigherKindedType* partial_apply_hkt(HigherKindedType* hkt, Type* argument);
HigherKindedType* compose_hkt(HigherKindedType* outer, HigherKindedType* inner);

// HKT-based generic programming patterns
Type* functor_map(HigherKindedType* functor_type, Type* input_element, Type* output_element);
Type* monad_bind(HigherKindedType* monad_type, Type* input_element, Type* output_element);

// Utility functions
const char* higher_kinded_type_kind_to_string(HigherKindedTypeKind kind);
void print_higher_kinded_type(const HigherKindedType* hkt);
int hkt_is_fully_applied(const HigherKindedType* hkt);
size_t hkt_unbound_count(const HigherKindedType* hkt);

// =============================================================================
// Automatic Implementation Generation for Built-in Types
// =============================================================================

// Generate built-in HKT implementations
int generate_builtin_hkt_implementations(void);

// Check if a type has an auto-generated implementation
int has_auto_impl(const char* type_name, const char* concept_name);

// Get auto-generated methods
InterfaceMethod* get_auto_impl_methods(const char* type_name, const char* concept_name);

// Cleanup auto-generated implementations
void cleanup_auto_impls(void);

// =============================================================================
// Registry Management Functions
// =============================================================================

// Concept registry functions
ConceptRegistry* concept_registry_new(void);
void concept_registry_free(ConceptRegistry* registry);
ConceptDefinition* concept_registry_lookup(ConceptRegistry* registry, const char* name);
int concept_registry_register(ConceptRegistry* registry, ConceptDefinition* concept);

// HKT registry functions
HKTRegistry* hkt_registry_new(void);
void hkt_registry_free(HKTRegistry* registry);
HigherKindedType* hkt_registry_lookup(HKTRegistry* registry, const char* name);
int hkt_registry_register(HKTRegistry* registry, HigherKindedType* hkt);

// Protocol registry functions
ProtocolRegistry* protocol_registry_new(void);
void protocol_registry_free(ProtocolRegistry* registry);
ProtocolDefinition* protocol_registry_lookup(ProtocolRegistry* registry, const char* name);
int protocol_registry_register(ProtocolRegistry* registry, ProtocolDefinition* protocol);

// Protocol definition functions
ProtocolDefinition* protocol_definition_new(const char* name, Position pos);
void protocol_definition_free(ProtocolDefinition* protocol);

// Protocol conformance functions
ProtocolConformance* protocol_conformance_new(Type* type, ProtocolDefinition* protocol, Position pos);
void protocol_conformance_free(ProtocolConformance* conformance);

// Type checker integration
int type_checker_init_enhanced_interfaces(TypeChecker* checker);
void type_checker_cleanup_enhanced_interfaces(TypeChecker* checker);

// =============================================================================
// Trait Bound Generation System Functions
// =============================================================================

// Trait bound management
TraitBound* trait_bound_new(const char* type_param_name, Position source_pos);
void trait_bound_free(TraitBound* bound);
TraitBoundSet* trait_bound_set_new(void);
void trait_bound_set_free(TraitBoundSet* set);
int trait_bound_set_add(TraitBoundSet* set, TraitBound* bound);
int trait_bound_set_remove(TraitBoundSet* set, TraitBound* bound);

// Trait bound conversion and analysis
TraitBound* constraint_to_trait_bound(InterfaceConstraint* constraint, TypeVariable* type_var);
float calculate_bound_confidence(InterfaceConstraint* constraint);
int trait_bounds_contradict(TraitBound* bound1, TraitBound* bound2);
int is_trait_bound_redundant(TraitBound* bound, TraitBoundSet* set);
int trait_bound_implies(TraitBound* source, TraitBound* target);

// Trait bound optimization
void sort_trait_bounds_by_importance(TraitBoundSet* bounds);
float get_trait_importance_score(TraitBound* bound);

// Type system helper functions
Type* type_from_func_signature(FuncDeclNode* sig, TypeChecker* checker);
Type* type_from_ast_node(ASTNode* node, TypeChecker* checker);
int type_add_method(Type* type, const char* method_name, Type* method_signature, TypeChecker* checker);
Type* type_string_type(void);
Type* type_bool(void);
Type* type_reference_simple(Type* base_type);
Type* type_lookup_by_name(const char* name, TypeChecker* checker);
Type* type_unknown(void);
Type* type_void(void);

// =============================================================================
// Test Utility Functions
// =============================================================================

// Convert constraint kind to string representation
const char* constraint_kind_to_string(ConstraintKind kind);

// Convert type variable kind to string representation  
const char* type_variable_kind_to_string(TypeVariableKind kind);

// Generate trait bounds from constraints on a type variable
TraitBoundSet* generate_trait_bounds_from_constraints(ConstraintInferenceEngine* engine, TypeVariable* var);

// Generate where clause string from trait bounds
char* generate_where_clause_from_bounds(TraitBoundSet* bounds);

// Optimize trait bounds by removing duplicates and redundant constraints
void optimize_trait_bounds(TraitBoundSet* bounds);

// Validate that generated trait bounds are correct
bool validate_generated_trait_bounds(TraitBoundSet* bounds);

#endif // INTERFACE_SYSTEM_H
