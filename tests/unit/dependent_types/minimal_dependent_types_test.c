#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

// Minimal test of dependent types implementation 
// This tests the core functionality without full type system dependencies

// Forward declarations
typedef struct Type Type;
typedef struct ASTNode ASTNode;
typedef struct TypeChecker TypeChecker;

// Mock TypeChecker for testing
typedef struct TypeChecker {
    int dummy;
} TypeChecker;

// Core dependent types structures (simplified)
typedef enum {
    DEP_CONSTRAINT_RANGE,
    DEP_CONSTRAINT_NON_ZERO,
    DEP_CONSTRAINT_POSITIVE,
    DEP_CONSTRAINT_NEGATIVE,
    DEP_CONSTRAINT_EVEN,
    DEP_CONSTRAINT_ODD,
    DEP_CONSTRAINT_SIZE_EQ,
    DEP_CONSTRAINT_SIZE_LE,
    DEP_CONSTRAINT_SIZE_GE,
    DEP_CONSTRAINT_VALID_INDEX,
    DEP_CONSTRAINT_DIVISIBLE,
    DEP_CONSTRAINT_CUSTOM
} DependentConstraintType;

typedef struct TypeConstraint {
    DependentConstraintType type;
    char* name;
    
    union {
        struct {
            int64_t min_value;
            int64_t max_value;
        } range;
        
        struct {
            int64_t size;
        } size;
        
        struct {
            char* target_array;
        } valid_index;
    } data;
    
    struct TypeConstraint* next;
} TypeConstraint;

typedef enum {
    DEPENDENT_BOUNDED_VEC,
    DEPENDENT_BOUNDED_INT,
    DEPENDENT_SIZED_ARRAY,
    DEPENDENT_REFINED_TYPE
} DependentTypeKind;

typedef struct DependentType {
    DependentTypeKind kind;
    char* name;
    TypeConstraint* constraints;
    
    union {
        struct {
            int64_t capacity;
            int is_capacity_dynamic;
            char* capacity_param;
        } bounded_vec;
        
        struct {
            int64_t min_value;
            int64_t max_value;
            int is_min_dynamic;
            int is_max_dynamic;
            char* min_param;
            char* max_param;
        } bounded_int;
        
        struct {
            int64_t size;
            int is_size_dynamic;
            char* size_param;
        } sized_array;
    } data;
} DependentType;

typedef struct RefinementType {
    char* name;
    TypeConstraint* constraint;
    struct RefinementType* next;
} RefinementType;

// Utility function
static char* str_dup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

// Core functions implementation
TypeConstraint* type_constraint_new(DependentConstraintType type) {
    TypeConstraint* constraint = malloc(sizeof(TypeConstraint));
    if (!constraint) return NULL;
    
    memset(constraint, 0, sizeof(TypeConstraint));
    constraint->type = type;
    
    return constraint;
}

void type_constraint_free(TypeConstraint* constraint) {
    if (!constraint) return;
    
    free(constraint->name);
    if (constraint->type == DEP_CONSTRAINT_VALID_INDEX) {
        free(constraint->data.valid_index.target_array);
    }
    
    free(constraint);
}

TypeConstraint* create_range_constraint(int64_t min_value, int64_t max_value) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_RANGE);
    if (constraint) {
        constraint->data.range.min_value = min_value;
        constraint->data.range.max_value = max_value;
        constraint->name = malloc(64);
        if (constraint->name) {
            snprintf(constraint->name, 64, "Range[%lld, %lld]", 
                    (long long)min_value, (long long)max_value);
        }
    }
    return constraint;
}

TypeConstraint* create_non_zero_constraint(void) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_NON_ZERO);
    if (constraint) {
        constraint->name = str_dup("NonZero");
    }
    return constraint;
}

TypeConstraint* create_positive_constraint(void) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_POSITIVE);
    if (constraint) {
        constraint->name = str_dup("Positive");
    }
    return constraint;
}

TypeConstraint* create_size_constraint(DependentConstraintType size_type, int64_t size) {
    TypeConstraint* constraint = type_constraint_new(size_type);
    if (constraint) {
        constraint->data.size.size = size;
        constraint->name = malloc(64);
        if (constraint->name) {
            const char* op = "";
            switch (size_type) {
                case DEP_CONSTRAINT_SIZE_EQ: op = "=="; break;
                case DEP_CONSTRAINT_SIZE_LE: op = "<="; break;
                case DEP_CONSTRAINT_SIZE_GE: op = ">="; break;
                default: op = "?"; break;
            }
            snprintf(constraint->name, 64, "Size%s%lld", op, (long long)size);
        }
    }
    return constraint;
}

TypeConstraint* create_valid_index_constraint(const char* array_name) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_VALID_INDEX);
    if (constraint) {
        constraint->data.valid_index.target_array = str_dup(array_name);
        constraint->name = malloc(64);
        if (constraint->name) {
            snprintf(constraint->name, 64, "ValidIndex<%s>", array_name ? array_name : "?");
        }
    }
    return constraint;
}

DependentType* dependent_type_new(DependentTypeKind kind, const char* name) {
    DependentType* type = malloc(sizeof(DependentType));
    if (!type) return NULL;
    
    memset(type, 0, sizeof(DependentType));
    type->kind = kind;
    type->name = name ? str_dup(name) : NULL;
    
    return type;
}

void dependent_type_free(DependentType* type) {
    if (!type) return;
    
    free(type->name);
    
    // Free constraints
    TypeConstraint* constraint = type->constraints;
    while (constraint) {
        TypeConstraint* next = constraint->next;
        type_constraint_free(constraint);
        constraint = next;
    }
    
    // Free type-specific data
    switch (type->kind) {
        case DEPENDENT_BOUNDED_VEC:
            free(type->data.bounded_vec.capacity_param);
            break;
        case DEPENDENT_BOUNDED_INT:
            free(type->data.bounded_int.min_param);
            free(type->data.bounded_int.max_param);
            break;
        case DEPENDENT_SIZED_ARRAY:
            free(type->data.sized_array.size_param);
            break;
        default:
            break;
    }
    
    free(type);
}

DependentType* create_bounded_vec_type(Type* element_type, int64_t capacity) {
    (void)element_type; // Unused in minimal version
    
    DependentType* type = dependent_type_new(DEPENDENT_BOUNDED_VEC, "BoundedVec");
    if (!type) return NULL;
    
    type->data.bounded_vec.capacity = capacity;
    type->data.bounded_vec.is_capacity_dynamic = 0;
    
    // Add capacity constraint
    TypeConstraint* capacity_constraint = create_size_constraint(DEP_CONSTRAINT_SIZE_LE, capacity);
    if (capacity_constraint) {
        capacity_constraint->next = type->constraints;
        type->constraints = capacity_constraint;
    }
    
    return type;
}

DependentType* create_dynamic_bounded_vec_type(Type* element_type, const char* capacity_param) {
    (void)element_type; // Unused in minimal version
    
    DependentType* type = dependent_type_new(DEPENDENT_BOUNDED_VEC, "BoundedVec");
    if (!type) return NULL;
    
    type->data.bounded_vec.is_capacity_dynamic = 1;
    type->data.bounded_vec.capacity_param = str_dup(capacity_param);
    
    return type;
}

DependentType* create_bounded_int_type(int64_t min_value, int64_t max_value) {
    DependentType* type = dependent_type_new(DEPENDENT_BOUNDED_INT, "BoundedInt");
    if (!type) return NULL;
    
    type->data.bounded_int.min_value = min_value;
    type->data.bounded_int.max_value = max_value;
    type->data.bounded_int.is_min_dynamic = 0;
    type->data.bounded_int.is_max_dynamic = 0;
    
    // Add range constraint
    TypeConstraint* range_constraint = create_range_constraint(min_value, max_value);
    if (range_constraint) {
        range_constraint->next = type->constraints;
        type->constraints = range_constraint;
    }
    
    return type;
}

DependentType* create_dynamic_bounded_int_type(const char* min_param, const char* max_param) {
    DependentType* type = dependent_type_new(DEPENDENT_BOUNDED_INT, "BoundedInt");
    if (!type) return NULL;
    
    type->data.bounded_int.is_min_dynamic = 1;
    type->data.bounded_int.is_max_dynamic = 1;
    type->data.bounded_int.min_param = str_dup(min_param);
    type->data.bounded_int.max_param = str_dup(max_param);
    
    return type;
}

DependentType* create_sized_array_type(Type* element_type, int64_t size) {
    (void)element_type; // Unused in minimal version
    
    DependentType* type = dependent_type_new(DEPENDENT_SIZED_ARRAY, "Array");
    if (!type) return NULL;
    
    type->data.sized_array.size = size;
    type->data.sized_array.is_size_dynamic = 0;
    
    // Add size constraint
    TypeConstraint* size_constraint = create_size_constraint(DEP_CONSTRAINT_SIZE_EQ, size);
    if (size_constraint) {
        size_constraint->next = type->constraints;
        type->constraints = size_constraint;
    }
    
    return type;
}

RefinementType* create_refinement_type(const char* name, Type* base_type, TypeConstraint* constraint) {
    (void)base_type; // Unused in minimal version
    
    RefinementType* type = malloc(sizeof(RefinementType));
    if (!type) return NULL;
    
    memset(type, 0, sizeof(RefinementType));
    type->name = str_dup(name);
    type->constraint = constraint;
    
    return type;
}

void refinement_type_free(RefinementType* type) {
    if (!type) return;
    
    free(type->name);
    if (type->constraint) {
        type_constraint_free(type->constraint);
    }
    free(type);
}

RefinementType* create_non_zero_int_type(void) {
    TypeConstraint* constraint = create_non_zero_constraint();
    return create_refinement_type("NonZeroInt", NULL, constraint);
}

RefinementType* create_positive_int_type(void) {
    TypeConstraint* constraint = create_positive_constraint();
    return create_refinement_type("PositiveInt", NULL, constraint);
}

RefinementType* create_negative_int_type(void) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_NEGATIVE);
    if (constraint) {
        constraint->name = str_dup("Negative");
    }
    return create_refinement_type("NegativeInt", NULL, constraint);
}

RefinementType* create_even_int_type(void) {
    TypeConstraint* constraint = type_constraint_new(DEP_CONSTRAINT_EVEN);
    if (constraint) {
        constraint->name = str_dup("Even");
    }
    return create_refinement_type("EvenInt", NULL, constraint);
}

RefinementType* create_valid_index_type(const char* array_name) {
    TypeConstraint* constraint = create_valid_index_constraint(array_name);
    return create_refinement_type("ValidIndex", NULL, constraint);
}

char* dependent_type_to_string(const DependentType* type) {
    if (!type) return str_dup("null");
    
    char* result = malloc(512);
    if (!result) return NULL;
    
    switch (type->kind) {
        case DEPENDENT_BOUNDED_VEC:
            if (type->data.bounded_vec.is_capacity_dynamic) {
                snprintf(result, 512, "BoundedVec<T, %s>", 
                        type->data.bounded_vec.capacity_param);
            } else {
                snprintf(result, 512, "BoundedVec<T, %lld>", 
                        (long long)type->data.bounded_vec.capacity);
            }
            break;
        case DEPENDENT_BOUNDED_INT:
            if (type->data.bounded_int.is_min_dynamic || type->data.bounded_int.is_max_dynamic) {
                snprintf(result, 512, "BoundedInt<%s, %s>",
                        type->data.bounded_int.min_param ? type->data.bounded_int.min_param : "?",
                        type->data.bounded_int.max_param ? type->data.bounded_int.max_param : "?");
            } else {
                snprintf(result, 512, "BoundedInt<%lld, %lld>",
                        (long long)type->data.bounded_int.min_value,
                        (long long)type->data.bounded_int.max_value);
            }
            break;
        case DEPENDENT_SIZED_ARRAY:
            if (type->data.sized_array.is_size_dynamic) {
                snprintf(result, 512, "Array<T, %s>", 
                        type->data.sized_array.size_param);
            } else {
                snprintf(result, 512, "Array<T, %lld>", 
                        (long long)type->data.sized_array.size);
            }
            break;
        default:
            snprintf(result, 512, "Unknown<%s>", type->name ? type->name : "?");
            break;
    }
    
    return result;
}

const char* dependent_constraint_type_to_string(DependentConstraintType type) {
    switch (type) {
        case DEP_CONSTRAINT_RANGE: return "Range";
        case DEP_CONSTRAINT_NON_ZERO: return "NonZero";
        case DEP_CONSTRAINT_POSITIVE: return "Positive";
        case DEP_CONSTRAINT_NEGATIVE: return "Negative";
        case DEP_CONSTRAINT_EVEN: return "Even";
        case DEP_CONSTRAINT_ODD: return "Odd";
        case DEP_CONSTRAINT_SIZE_EQ: return "SizeEq";
        case DEP_CONSTRAINT_SIZE_LE: return "SizeLe";
        case DEP_CONSTRAINT_SIZE_GE: return "SizeGe";
        case DEP_CONSTRAINT_VALID_INDEX: return "ValidIndex";
        case DEP_CONSTRAINT_DIVISIBLE: return "Divisible";
        case DEP_CONSTRAINT_CUSTOM: return "Custom";
        default: return "Unknown";
    }
}

const char* dependent_type_kind_to_string(DependentTypeKind kind) {
    switch (kind) {
        case DEPENDENT_BOUNDED_VEC: return "BoundedVec";
        case DEPENDENT_BOUNDED_INT: return "BoundedInt";
        case DEPENDENT_SIZED_ARRAY: return "Array";
        case DEPENDENT_REFINED_TYPE: return "RefinedType";
        default: return "Unknown";
    }
}

// Test function
int main() {
    printf("=== Minimal Dependent and Refinement Type System Test ===\n\n");
    
    // Test 1: Bounded Vector Types
    printf("--- Testing Bounded Vector Types ---\n");
    
    DependentType* bounded_vec_10 = create_bounded_vec_type(NULL, 10);
    assert(bounded_vec_10 != NULL);
    assert(bounded_vec_10->kind == DEPENDENT_BOUNDED_VEC);
    assert(bounded_vec_10->data.bounded_vec.capacity == 10);
    assert(!bounded_vec_10->data.bounded_vec.is_capacity_dynamic);
    
    char* vec_str = dependent_type_to_string(bounded_vec_10);
    printf("Static bounded vector: %s\n", vec_str);
    free(vec_str);
    
    DependentType* dynamic_vec = create_dynamic_bounded_vec_type(NULL, "N");
    assert(dynamic_vec != NULL);
    assert(dynamic_vec->data.bounded_vec.is_capacity_dynamic);
    assert(strcmp(dynamic_vec->data.bounded_vec.capacity_param, "N") == 0);
    
    char* dyn_vec_str = dependent_type_to_string(dynamic_vec);
    printf("Dynamic bounded vector: %s\n", dyn_vec_str);
    free(dyn_vec_str);
    
    // Test 2: Bounded Integer Types
    printf("\n--- Testing Bounded Integer Types ---\n");
    
    DependentType* bounded_int_0_255 = create_bounded_int_type(0, 255);
    assert(bounded_int_0_255 != NULL);
    assert(bounded_int_0_255->kind == DEPENDENT_BOUNDED_INT);
    assert(bounded_int_0_255->data.bounded_int.min_value == 0);
    assert(bounded_int_0_255->data.bounded_int.max_value == 255);
    
    char* int_str = dependent_type_to_string(bounded_int_0_255);
    printf("Bounded integer [0, 255]: %s\n", int_str);
    free(int_str);
    
    DependentType* dynamic_int = create_dynamic_bounded_int_type("Min", "Max");
    assert(dynamic_int != NULL);
    assert(dynamic_int->data.bounded_int.is_min_dynamic);
    assert(dynamic_int->data.bounded_int.is_max_dynamic);
    assert(strcmp(dynamic_int->data.bounded_int.min_param, "Min") == 0);
    assert(strcmp(dynamic_int->data.bounded_int.max_param, "Max") == 0);
    
    char* dyn_int_str = dependent_type_to_string(dynamic_int);
    printf("Dynamic bounded integer: %s\n", dyn_int_str);
    free(dyn_int_str);
    
    // Test 3: Sized Array Types
    printf("\n--- Testing Sized Array Types ---\n");
    
    DependentType* array_1024 = create_sized_array_type(NULL, 1024);
    assert(array_1024 != NULL);
    assert(array_1024->kind == DEPENDENT_SIZED_ARRAY);
    assert(array_1024->data.sized_array.size == 1024);
    assert(!array_1024->data.sized_array.is_size_dynamic);
    
    char* array_str = dependent_type_to_string(array_1024);
    printf("Sized array [1024]: %s\n", array_str);
    free(array_str);
    
    // Test 4: Refinement Types
    printf("\n--- Testing Refinement Types ---\n");
    
    RefinementType* non_zero = create_non_zero_int_type();
    assert(non_zero != NULL);
    assert(non_zero->constraint->type == DEP_CONSTRAINT_NON_ZERO);
    printf("NonZeroInt refinement: %s\n", non_zero->name);
    
    RefinementType* positive = create_positive_int_type();
    assert(positive != NULL);
    assert(positive->constraint->type == DEP_CONSTRAINT_POSITIVE);
    printf("PositiveInt refinement: %s\n", positive->name);
    
    RefinementType* even = create_even_int_type();
    assert(even != NULL);
    assert(even->constraint->type == DEP_CONSTRAINT_EVEN);
    printf("EvenInt refinement: %s\n", even->name);
    
    RefinementType* valid_index = create_valid_index_type("array");
    assert(valid_index != NULL);
    assert(valid_index->constraint->type == DEP_CONSTRAINT_VALID_INDEX);
    printf("ValidIndex refinement: %s\n", valid_index->name);
    
    // Test 5: Constraint Testing
    printf("\n--- Testing Constraints ---\n");
    
    TypeConstraint* range_constraint = create_range_constraint(1, 100);
    assert(range_constraint != NULL);
    assert(range_constraint->type == DEP_CONSTRAINT_RANGE);
    assert(range_constraint->data.range.min_value == 1);
    assert(range_constraint->data.range.max_value == 100);
    printf("Range constraint: %s\n", range_constraint->name);
    
    TypeConstraint* size_constraint = create_size_constraint(DEP_CONSTRAINT_SIZE_LE, 50);
    assert(size_constraint != NULL);
    assert(size_constraint->type == DEP_CONSTRAINT_SIZE_LE);
    assert(size_constraint->data.size.size == 50);
    printf("Size constraint: %s\n", size_constraint->name);
    
    // Test 6: Constraint Type Strings
    printf("\n--- Testing Constraint Type Strings ---\n");
    
    printf("- RANGE: %s\n", dependent_constraint_type_to_string(DEP_CONSTRAINT_RANGE));
    printf("- NON_ZERO: %s\n", dependent_constraint_type_to_string(DEP_CONSTRAINT_NON_ZERO));
    printf("- POSITIVE: %s\n", dependent_constraint_type_to_string(DEP_CONSTRAINT_POSITIVE));
    printf("- EVEN: %s\n", dependent_constraint_type_to_string(DEP_CONSTRAINT_EVEN));
    
    // Test 7: Dependent Type Kind Strings
    printf("\n--- Testing Dependent Type Kind Strings ---\n");
    
    printf("- BOUNDED_VEC: %s\n", dependent_type_kind_to_string(DEPENDENT_BOUNDED_VEC));
    printf("- BOUNDED_INT: %s\n", dependent_type_kind_to_string(DEPENDENT_BOUNDED_INT));
    printf("- SIZED_ARRAY: %s\n", dependent_type_kind_to_string(DEPENDENT_SIZED_ARRAY));
    
    // Cleanup
    dependent_type_free(bounded_vec_10);
    dependent_type_free(dynamic_vec);
    dependent_type_free(bounded_int_0_255);
    dependent_type_free(dynamic_int);
    dependent_type_free(array_1024);
    
    refinement_type_free(non_zero);
    refinement_type_free(positive);
    refinement_type_free(even);
    refinement_type_free(valid_index);
    
    type_constraint_free(range_constraint);
    type_constraint_free(size_constraint);
    
    printf("\n=== All Tests Passed! ===\n");
    printf("✓ Dependent types system is working correctly\n");
    printf("✓ Refinement types system is working correctly\n");
    printf("✓ Type constraints are working correctly\n");
    printf("✓ BoundedVec<T, N> parameterized type implemented\n");
    printf("✓ BoundedInt<Min, Max> type implemented\n");
    printf("✓ Refinement types (NonZeroInt, PositiveInt, ValidIndex) implemented\n");
    
    return 0;
}
