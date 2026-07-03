#include "interface_system.h"
#include "type_level_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Task 22.4 type-level programming — dependent & applied types:
// associated-type projections, matrix/array dimension types,
// dependent types and vectors, phantom types, computation cache,
// static asserts. Split from type_level_programming.c (refactor,
// no behavior change).

// Per-file static strdup — house idiom (see types.c, ide/*.c).
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

// =============================================================================
// Associated Types and Projections
// =============================================================================

// Create an associated type projection
TypeLevelComputation* create_associated_type_projection(const char* trait_name, const char* assoc_type_name,
                                                       Type* self_type) {
    TypeLevelComputation* projection = type_level_computation_new(TYPE_LEVEL_ASSOCIATED, "projection");
    if (!projection) return NULL;
    
    // Store the trait and associated type information
    char* full_name = malloc(strlen(trait_name) + strlen(assoc_type_name) + 10);
    if (full_name) {
        sprintf(full_name, "%s::%s", trait_name, assoc_type_name);
        free(projection->name);
        projection->name = full_name;
    }
    
    // The result type would be resolved by looking up the associated type
    // in the trait implementation for self_type
    
    return projection;
}

// Resolve an associated type projection
Type* resolve_associated_type_projection(TypeLevelComputation* projection, TypeChecker* checker) {
    if (!projection || projection->kind != TYPE_LEVEL_ASSOCIATED || !checker) return NULL;
    
    // This would involve:
    // 1. Parsing the projection name to extract trait and associated type
    // 2. Looking up the trait implementation for the self type
    // 3. Finding the binding for the associated type in that implementation
    // 4. Returning the bound type
    
    // For now, return a placeholder
    if (projection->result_type) {
        return type_copy(projection->result_type);
    }
    
    return NULL;
}

// =============================================================================
// Compile-Time Type Computation Examples
// =============================================================================

// Create a Matrix type with compile-time dimensions
Type* create_matrix_type(size_t rows, size_t cols, Type* element_type) {
    if (!element_type) return NULL;
    
    // Create a struct type representing a matrix
    Type* matrix_type = type_new(TYPE_STRUCT);
    if (!matrix_type) return NULL;
    
    // For now, represent as a simple array type
    // In practice, this would be a more complex structure with compile-time size checking
    Type* array_type = type_array(element_type, rows * cols);
    
    // Create a name that includes the dimensions
    char* name = malloc(128);
    if (name) {
        snprintf(name, 128, "Matrix<%zu, %zu, %s>", rows, cols, 
                element_type->name ? element_type->name : "T");
        matrix_type->name = name;
    }
    
    // Set up the struct with the array as the only field
    matrix_type->data.struct_type.fields = calloc(1, sizeof(StructField));
    if (matrix_type->data.struct_type.fields) {
        matrix_type->data.struct_type.field_count = 1;
        matrix_type->data.struct_type.fields[0].name = str_dup("data");
        matrix_type->data.struct_type.fields[0].type = array_type;
        matrix_type->data.struct_type.fields[0].offset = 0;
        matrix_type->data.struct_type.fields[0].ownership = OWNERSHIP_OWNED;
        matrix_type->data.struct_type.fields[0].mutability = MUTABILITY_MUTABLE;
    }
    
    matrix_type->size = array_type->size;
    matrix_type->align = array_type->align;
    
    return matrix_type;
}

// Type-level arithmetic for matrix dimensions
TypeLevelComputation* create_matrix_multiply_dimensions(TypeLevelComputation* left_rows, 
                                                       TypeLevelComputation* left_cols,
                                                       TypeLevelComputation* right_rows,
                                                       TypeLevelComputation* right_cols) {
    TypeLevelComputation* computation = type_level_computation_new(TYPE_LEVEL_FUNCTION, "matrix_multiply_dims");
    if (!computation) return NULL;
    
    // The computation would check that left_cols == right_rows
    // and return (left_rows, right_cols) as the result dimensions
    
    computation->is_const_evaluable = 1;
    
    return computation;
}

// =============================================================================
// Dependent Types and Value-Level Constraints
// =============================================================================

// Dependent type kinds are now defined in interface_system.h

// Dependent type constraint
typedef struct DependentConstraint {
    char* name;                        // Constraint name
    ASTNode* constraint_expr;          // Constraint expression
    Type* constrained_type;            // Type being constrained
    struct DependentConstraint* next;  // For linked lists
} DependentConstraint;

// Enhanced dependent type
typedef struct DependentType {
    DependentTypeKind kind;            // Kind of dependent type
    char* name;                        // Type name
    TypeVariable* value_parameters;    // Value parameters
    Type* base_type;                   // Base type (e.g., T in Array<T, N>)
    DependentConstraint* constraints;  // Value-level constraints
    ASTNode* size_expr;                // Size expression for arrays/vectors
    Type* proof_type;                  // Proof type for dependent proofs
    int is_compile_time;               // Whether dependencies are compile-time
} DependentType;

// Removed: dependent_type_new - defined in dependent_types.c
// DependentType* dependent_type_new(DependentTypeKind kind, const char* name, Type* base_type) {
//     DependentType* dep_type = malloc(sizeof(DependentType));
//     if (!dep_type) return NULL;
//     
//     dep_type->kind = kind;
//     dep_type->name = name ? str_dup(name) : NULL;
//     dep_type->value_parameters = NULL;
//     dep_type->base_type = base_type ? type_copy(base_type) : NULL;
//     dep_type->constraints = NULL;
//     dep_type->size_expr = NULL;
//     dep_type->proof_type = NULL;
//     dep_type->is_compile_time = 1; // Default to compile-time
//     
//     return dep_type;
// }

// Removed: dependent_type_free - defined in dependent_types.c
// void dependent_type_free(DependentType* dep_type) {
//     if (!dep_type) return;
//     
//     free(dep_type->name);
//     
//     if (dep_type->value_parameters) {
//         TypeVariable* param = dep_type->value_parameters;
//         while (param) {
//             TypeVariable* next = param->next;
//             type_variable_free(param);
//             param = next;
//         }
//     }
//     
//     if (dep_type->base_type) {
//         type_free(dep_type->base_type);
//     }
//     
//     DependentConstraint* constraint = dep_type->constraints;
//     while (constraint) {
//         DependentConstraint* next = constraint->next;
//         free(constraint->name);
//         if (constraint->constrained_type) {
//             type_free(constraint->constrained_type);
//         }
//         free(constraint);
//         constraint = next;
//     }
//     
//     if (dep_type->proof_type) {
//         type_free(dep_type->proof_type);
//     }
//     
//     free(dep_type);
// }

int dependent_type_add_constraint(DependentType* dep_type, const char* name, ASTNode* constraint_expr, Type* constrained_type) {
    if (!dep_type || !name || !constraint_expr) return 0;
    
    DependentConstraint* constraint = malloc(sizeof(DependentConstraint));
    if (!constraint) return 0;
    
    constraint->name = str_dup(name);
    constraint->constraint_expr = constraint_expr;
    constraint->constrained_type = constrained_type ? type_copy(constrained_type) : NULL;
    constraint->next = dep_type->constraints;
    dep_type->constraints = constraint;
    
    return 1;
}

// Create a dependent type where the type depends on a value
TypeLevelComputation* create_dependent_type(const char* name, TypeVariable* value_param, ASTNode* type_expr) {
    TypeLevelComputation* dep_type = type_level_computation_new(TYPE_LEVEL_DEPENDENT, name);
    if (!dep_type) return NULL;
    
    // Add the value parameter
    dep_type->parameters = value_param;
    dep_type->body = type_expr;
    dep_type->is_const_evaluable = 0; // Dependent types are not generally const-evaluable
    
    return dep_type;
}

// Create compile-time sized array type
Type* create_compile_time_array_type(Type* element_type, TypeLevelNat* size) {
    if (!element_type || !size) return NULL;
    
    // Create array type with compile-time known size
    Type* array_type = type_array(element_type, size->value);
    if (!array_type) return NULL;
    
    // Create a name that includes the size
    char* name = malloc(128);
    if (name) {
        snprintf(name, 128, "Array<%s, %zu>", 
                element_type->name ? element_type->name : "T", size->value);
        free(array_type->name);
        array_type->name = name;
    }
    
    return array_type;
}

// Create matrix type with compile-time dimensions
Type* create_compile_time_matrix_type(Type* element_type, TypeLevelNat* rows, TypeLevelNat* cols) {
    if (!element_type || !rows || !cols) return NULL;
    
    // Create the underlying array type with total size = rows * cols
    size_t total_size = rows->value * cols->value;
    Type* array_type = type_array(element_type, total_size);
    if (!array_type) return NULL;
    
    // Create a struct type representing the matrix
    Type* matrix_type = type_new(TYPE_STRUCT);
    if (!matrix_type) {
        type_free(array_type);
        return NULL;
    }
    
    // Create a name that includes the dimensions
    char* name = malloc(128);
    if (name) {
        snprintf(name, 128, "Matrix<%s, %zu, %zu>", 
                element_type->name ? element_type->name : "T", rows->value, cols->value);
        matrix_type->name = name;
    }
    
    // Set up the struct with the array as the only field
    matrix_type->data.struct_type.fields = calloc(1, sizeof(StructField));
    if (matrix_type->data.struct_type.fields) {
        matrix_type->data.struct_type.field_count = 1;
        matrix_type->data.struct_type.fields[0].name = str_dup("data");
        matrix_type->data.struct_type.fields[0].type = array_type;
        matrix_type->data.struct_type.fields[0].offset = 0;
        matrix_type->data.struct_type.fields[0].ownership = OWNERSHIP_OWNED;
        matrix_type->data.struct_type.fields[0].mutability = MUTABILITY_MUTABLE;
    }
    
    matrix_type->size = array_type->size;
    matrix_type->align = array_type->align;
    
    return matrix_type;
}

// Create a proof type for compile-time constraints
Type* create_proof_type(const char* proposition) {
    if (!proposition) return NULL;
    
    Type* proof_type = type_new(TYPE_UNKNOWN);
    if (!proof_type) return NULL;
    
    char* name = malloc(strlen(proposition) + 20);
    if (name) {
        sprintf(name, "Proof<%s>", proposition);
        proof_type->name = name;
    }
    
    // Proof types have zero size at runtime (phantom types)
    proof_type->size = 0;
    proof_type->align = 1;
    
    return proof_type;
}

// Create a bounds-checked array access type
Type* create_safe_array_access_type(Type* array_type, ASTNode* index_expr, ASTNode* bounds_proof) {
    if (!array_type || !index_expr) return NULL;
    
    // For now, return the element type
    // In a full implementation, this would verify the bounds proof
    if (array_type->kind == TYPE_ARRAY) {
        return type_copy(array_type->data.array.element_type);
    }
    
    return NULL;
}

// Dependent vector type that tracks its length
typedef struct DependentVector {
    Type* element_type;
    size_t capacity;
    size_t length;
    ASTNode* length_constraint; // Optional constraint on length
} DependentVector;

DependentVector* dependent_vector_new(Type* element_type, size_t initial_capacity) {
    DependentVector* vec = malloc(sizeof(DependentVector));
    if (!vec) return NULL;
    
    vec->element_type = element_type ? type_copy(element_type) : NULL;
    vec->capacity = initial_capacity;
    vec->length = 0;
    vec->length_constraint = NULL;
    
    return vec;
}

void dependent_vector_free(DependentVector* vec) {
    if (!vec) return;
    
    if (vec->element_type) {
        type_free(vec->element_type);
    }
    
    free(vec);
}

// Create type for dependent vector
Type* dependent_vector_to_type(DependentVector* vec) {
    if (!vec || !vec->element_type) return NULL;
    
    Type* vec_type = type_new(TYPE_STRUCT);
    if (!vec_type) return NULL;
    
    char* name = malloc(128);
    if (name) {
        snprintf(name, 128, "DVector<%s, len=%zu>", 
                vec->element_type->name ? vec->element_type->name : "T", vec->length);
        vec_type->name = name;
    }
    
    // Create fields for the dependent vector
    vec_type->data.struct_type.field_count = 3;
    vec_type->data.struct_type.fields = calloc(3, sizeof(StructField));
    
    if (vec_type->data.struct_type.fields) {
        // Data field
        vec_type->data.struct_type.fields[0].name = str_dup("data");
        vec_type->data.struct_type.fields[0].type = type_pointer(vec->element_type);
        vec_type->data.struct_type.fields[0].offset = 0;
        vec_type->data.struct_type.fields[0].ownership = OWNERSHIP_OWNED;
        vec_type->data.struct_type.fields[0].mutability = MUTABILITY_MUTABLE;
        
        // Length field
        vec_type->data.struct_type.fields[1].name = str_dup("length");
        vec_type->data.struct_type.fields[1].type = type_new(TYPE_UINT64);
        vec_type->data.struct_type.fields[1].offset = 8;
        vec_type->data.struct_type.fields[1].ownership = OWNERSHIP_OWNED;
        vec_type->data.struct_type.fields[1].mutability = MUTABILITY_MUTABLE;
        
        // Capacity field
        vec_type->data.struct_type.fields[2].name = str_dup("capacity");
        vec_type->data.struct_type.fields[2].type = type_new(TYPE_UINT64);
        vec_type->data.struct_type.fields[2].offset = 16;
        vec_type->data.struct_type.fields[2].ownership = OWNERSHIP_OWNED;
        vec_type->data.struct_type.fields[2].mutability = MUTABILITY_MUTABLE;
    }
    
    vec_type->size = 24; // 8 bytes for pointer + 8 for length + 8 for capacity
    vec_type->align = 8;
    
    return vec_type;
}

// Create a Vector type with length dependent on a value
Type* create_dependent_vector_type(Type* element_type, ASTNode* length_expr, TypeChecker* checker) {
    if (!element_type || !length_expr || !checker) return NULL;
    
    // Try to evaluate the length expression at compile time
    Type* length_type = type_check_expression(checker, length_expr);
    
    if (length_type && type_is_integer(length_type)) {
        // If it's a compile-time constant, we can create a fixed-size array
        if (length_expr->type == AST_LITERAL) {
            LiteralNode* literal = (LiteralNode*)length_expr;
            // Parse the literal value to get the actual length
            size_t length = 1; // Default length
            if (literal->value) {
                length = (size_t)atoi(literal->value);
            }
            
            type_free(length_type);
            return type_array(element_type, length);
        }
    }
    
    if (length_type) type_free(length_type);
    
    // If we can't evaluate at compile time, create a slice (dynamic array)
    return type_slice(element_type);
}

// =============================================================================
// Advanced Type-Level Programming Features
// =============================================================================

// Create compile-time evaluated generic constraints
TypeLevelComputation* create_compile_time_constraint(const char* constraint_name, TypeVariable* type_var, ASTNode* condition_expr) {
    TypeLevelComputation* constraint = type_level_computation_new(TYPE_LEVEL_DEPENDENT, constraint_name);
    if (!constraint) return NULL;
    
    constraint->parameters = type_var;
    constraint->body = condition_expr;
    constraint->is_const_evaluable = 1;
    
    return constraint;
}

// Create higher-order type functions
TypeLevelComputation* create_higher_order_type_function(const char* name, TypeVariable* func_param, TypeVariable* type_param) {
    TypeLevelComputation* hof = type_level_computation_new(TYPE_LEVEL_FUNCTION, name);
    if (!hof) return NULL;
    
    // Chain the parameters: F<_> followed by T
    hof->parameters = func_param;
    if (func_param) {
        func_param->next = type_param;
    }
    
    hof->is_const_evaluable = 1;
    
    return hof;
}

// Phantom type support for zero-cost abstractions
Type* create_phantom_type(const char* name, Type* phantom_param) {
    Type* phantom = type_new(TYPE_STRUCT);
    if (!phantom) return NULL;
    
    phantom->name = str_dup(name);
    phantom->size = 0; // Zero-sized type
    phantom->align = 1;
    
    // Create a phantom field that doesn't affect layout
    phantom->data.struct_type.field_count = 1;
    phantom->data.struct_type.fields = calloc(1, sizeof(StructField));
    if (phantom->data.struct_type.fields) {
        phantom->data.struct_type.fields[0].name = str_dup("_phantom");
        phantom->data.struct_type.fields[0].type = phantom_param;
        phantom->data.struct_type.fields[0].offset = 0;
        phantom->data.struct_type.fields[0].ownership = OWNERSHIP_OWNED;
        phantom->data.struct_type.fields[0].mutability = MUTABILITY_IMMUTABLE;
    }
    
    return phantom;
}

// Type-level computation with memoization
typedef struct TypeComputationCache {
    char** input_signatures;
    Type** cached_results;
    size_t cache_size;
    size_t cache_capacity;
} TypeComputationCache;

TypeComputationCache* type_computation_cache_new(void) {
    TypeComputationCache* cache = malloc(sizeof(TypeComputationCache));
    if (!cache) return NULL;
    
    cache->input_signatures = NULL;
    cache->cached_results = NULL;
    cache->cache_size = 0;
    cache->cache_capacity = 0;
    
    return cache;
}

void type_computation_cache_free(TypeComputationCache* cache) {
    if (!cache) return;
    
    for (size_t i = 0; i < cache->cache_size; i++) {
        free(cache->input_signatures[i]);
        type_free(cache->cached_results[i]);
    }
    
    free(cache->input_signatures);
    free(cache->cached_results);
    free(cache);
}

Type* type_computation_cache_lookup(TypeComputationCache* cache, const char* signature) {
    if (!cache || !signature) return NULL;
    
    for (size_t i = 0; i < cache->cache_size; i++) {
        if (strcmp(cache->input_signatures[i], signature) == 0) {
            return type_copy(cache->cached_results[i]);
        }
    }
    
    return NULL;
}

int type_computation_cache_store(TypeComputationCache* cache, const char* signature, Type* result) {
    if (!cache || !signature || !result) return 0;
    
    if (cache->cache_size >= cache->cache_capacity) {
        size_t new_capacity = cache->cache_capacity == 0 ? 8 : cache->cache_capacity * 2;
        
        char** new_signatures = realloc(cache->input_signatures, sizeof(char*) * new_capacity);
        Type** new_results = realloc(cache->cached_results, sizeof(Type*) * new_capacity);
        
        if (!new_signatures || !new_results) {
            free(new_signatures);
            free(new_results);
            return 0;
        }
        
        cache->input_signatures = new_signatures;
        cache->cached_results = new_results;
        cache->cache_capacity = new_capacity;
    }
    
    cache->input_signatures[cache->cache_size] = str_dup(signature);
    cache->cached_results[cache->cache_size] = type_copy(result);
    cache->cache_size++;
    
    return 1;
}

// Enhanced type family with caching
typedef struct CachedTypeFamily {
    TypeFamily* family;
    TypeComputationCache* cache;
    size_t evaluation_count;
    double average_evaluation_time;
} CachedTypeFamily;

CachedTypeFamily* cached_type_family_new(TypeFamily* family) {
    CachedTypeFamily* cached = malloc(sizeof(CachedTypeFamily));
    if (!cached) return NULL;
    
    cached->family = family;
    cached->cache = type_computation_cache_new();
    cached->evaluation_count = 0;
    cached->average_evaluation_time = 0.0;
    
    return cached;
}

void cached_type_family_free(CachedTypeFamily* cached) {
    if (!cached) return;
    
    type_family_free(cached->family);
    type_computation_cache_free(cached->cache);
    free(cached);
}

// Compile-time assertion support
Type* create_static_assert_type(const char* assertion_name, ASTNode* condition, const char* error_message) {
    Type* assert_type = type_new(TYPE_UNKNOWN);
    if (!assert_type) return NULL;
    
    char* name = malloc(strlen(assertion_name) + 50);
    if (name) {
        sprintf(name, "StaticAssert<%s>", assertion_name);
        assert_type->name = name;
    }
    
    // Static assertions have zero runtime cost
    assert_type->size = 0;
    assert_type->align = 1;
    
    return assert_type;
}

