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
InterfaceConstraint* interface_constraint_copy(const InterfaceConstraint* constraint);

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

// Constraint solving
int solve_constraints(ConstraintInferenceEngine* engine);
int unify_types_with_constraints(ConstraintInferenceEngine* engine, Type* type1, Type* type2);
Type* substitute_type_variables(ConstraintInferenceEngine* engine, Type* type);

// Concept-based generics
ConceptDefinition* concept_definition_new(const char* name, Position pos);
void concept_definition_free(ConceptDefinition* concept);
int concept_add_requirement(ConceptDefinition* concept, InterfaceConstraint* requirement);
int concept_add_super_concept(ConceptDefinition* concept, ConceptDefinition* super_concept);
int type_satisfies_concept(Type* type, ConceptDefinition* concept, TypeChecker* checker);

// Higher-kinded type support
HigherKindedType* higher_kinded_type_new(HigherKindedTypeKind kind, Type* type_constructor);
void higher_kinded_type_free(HigherKindedType* hkt);
int higher_kinded_type_apply(HigherKindedType* hkt, Type* argument);
Type* higher_kinded_type_instantiate(HigherKindedType* hkt, Type** arguments, size_t arg_count);

// Type-level programming
TypeLevelComputation* type_level_computation_new(TypeLevelComputationKind kind, const char* name);
void type_level_computation_free(TypeLevelComputation* computation);
Type* evaluate_type_level_computation(TypeLevelComputation* computation, TypeChecker* checker);
int type_level_computation_is_const_evaluable(TypeLevelComputation* computation);

// Protocol-oriented programming
ProtocolDefinition* protocol_definition_new(const char* name, Position pos);
void protocol_definition_free(ProtocolDefinition* protocol);
int protocol_add_required_method(ProtocolDefinition* protocol, InterfaceMethod* method);
int protocol_add_default_method(ProtocolDefinition* protocol, InterfaceMethod* method);
int protocol_add_associated_type(ProtocolDefinition* protocol, Type* associated_type);

ProtocolConformance* protocol_conformance_new(Type* type, ProtocolDefinition* protocol, Position pos);
void protocol_conformance_free(ProtocolConformance* conformance);
int protocol_conformance_add_method_implementation(ProtocolConformance* conformance, InterfaceMethod* implementation);

// Automatic conformance detection
int infer_protocol_conformance(Type* type, ProtocolDefinition* protocol, TypeChecker* checker);
int check_retroactive_conformance(Type* type, ProtocolDefinition* protocol, TypeChecker* checker);

// Enhanced interface creation with automatic inference
Type* type_interface_enhanced(InterfaceMethod* methods, size_t method_count, ConstraintSet* constraints);
int type_implements_interface_auto(Type* type, Type* interface_type, TypeChecker* checker);

// Integration with existing type checker
int enhanced_interface_system_init(TypeChecker* checker);
void enhanced_interface_system_cleanup(TypeChecker* checker);
int type_check_with_constraint_inference(TypeChecker* checker, ASTNode* node);

// Utility functions
const char* constraint_kind_to_string(ConstraintKind kind);
const char* type_variable_kind_to_string(TypeVariableKind kind);
const char* higher_kinded_type_kind_to_string(HigherKindedTypeKind kind);
void print_constraint_set(const ConstraintSet* set);
void print_type_variable(const TypeVariable* var);

// =============================================================================
// Registry Structures  
// =============================================================================

// Concept registry for managing concepts
struct ConceptRegistry {
    ConceptDefinition** concepts;       // Array of concept definitions
    size_t concept_count;               // Number of concepts
    size_t capacity;                    // Capacity of concepts array
};

// Higher-kinded type registry
struct HKTRegistry {
    HigherKindedType** hkts;           // Array of higher-kinded types
    size_t hkt_count;                  // Number of HKTs
    size_t capacity;                   // Capacity of HKTs array
};

// Protocol registry for managing protocols
struct ProtocolRegistry {
    ProtocolDefinition** protocols;    // Array of protocol definitions
    ProtocolConformance** conformances; // Array of protocol conformances
    size_t protocol_count;             // Number of protocols
    size_t conformance_count;          // Number of conformances
    size_t protocol_capacity;          // Capacity of protocols array
    size_t conformance_capacity;       // Capacity of conformances array
};

#endif // INTERFACE_SYSTEM_H
