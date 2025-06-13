#include "interface_system.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =============================================================================
// 22.3: Higher-Kinded Type Support Implementation
// =============================================================================

// Helper function for string duplication
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
// Higher-Kinded Type Management
// =============================================================================

HigherKindedType* higher_kinded_type_new(HigherKindedTypeKind kind, Type* type_constructor) {
    HigherKindedType* hkt = malloc(sizeof(HigherKindedType));
    if (!hkt) return NULL;
    
    hkt->kind = kind;
    hkt->type_constructor = type_constructor;
    hkt->type_arguments = NULL;
    hkt->arity = 0;
    hkt->kind_signature = NULL;
    
    // Set up default arity and kind signature based on the kind
    switch (kind) {
        case HKT_TYPE:
            hkt->arity = 0;
            hkt->kind_signature = str_dup("*");
            break;
            
        case HKT_TYPE_TO_TYPE:
            hkt->arity = 1;
            hkt->kind_signature = str_dup("* -> *");
            break;
            
        case HKT_TYPE_TO_TYPE_TO_TYPE:
            hkt->arity = 2;
            hkt->kind_signature = str_dup("* -> * -> *");
            break;
            
        case HKT_CONSTRAINT:
            hkt->arity = 1;
            hkt->kind_signature = str_dup("Constraint");
            break;
            
        case HKT_ROW:
            hkt->arity = 0;
            hkt->kind_signature = str_dup("Row");
            break;
            
        case HKT_EFFECT:
            hkt->arity = 1;
            hkt->kind_signature = str_dup("Effect");
            break;
    }
    
    // Allocate type arguments array if needed
    if (hkt->arity > 0) {
        hkt->type_arguments = malloc(sizeof(Type*) * hkt->arity);
        if (hkt->type_arguments) {
            memset(hkt->type_arguments, 0, sizeof(Type*) * hkt->arity);
        }
    }
    
    return hkt;
}

void higher_kinded_type_free(HigherKindedType* hkt) {
    if (!hkt) return;
    
    if (hkt->type_constructor) {
        type_free(hkt->type_constructor);
    }
    
    if (hkt->type_arguments) {
        for (size_t i = 0; i < hkt->arity; i++) {
            if (hkt->type_arguments[i]) {
                type_free(hkt->type_arguments[i]);
            }
        }
        free(hkt->type_arguments);
    }
    
    free(hkt->kind_signature);
    free(hkt);
}

int higher_kinded_type_apply(HigherKindedType* hkt, Type* argument) {
    if (!hkt || !argument) return 0;
    
    // Find the first empty slot to apply the argument
    for (size_t i = 0; i < hkt->arity; i++) {
        if (!hkt->type_arguments[i]) {
            hkt->type_arguments[i] = type_copy(argument);
            return 1;
        }
    }
    
    return 0; // No more slots available
}

Type* higher_kinded_type_instantiate(HigherKindedType* hkt, Type** arguments, size_t arg_count) {
    if (!hkt || !arguments || arg_count != hkt->arity) return NULL;
    
    // Create a concrete type by applying all arguments to the type constructor
    switch (hkt->kind) {
        case HKT_TYPE:
            // Already a concrete type, just return a copy
            return type_copy(hkt->type_constructor);
            
        case HKT_TYPE_TO_TYPE: {
            // Apply one type argument to create a concrete type
            if (arg_count != 1) return NULL;
            Type* arg = arguments[0];
            
            // For common type constructors, create the appropriate concrete type
            if (hkt->type_constructor && hkt->type_constructor->name) {
                if (strcmp(hkt->type_constructor->name, "Vec") == 0 || 
                    strcmp(hkt->type_constructor->name, "Array") == 0) {
                    // Create array type: Vec<T> -> [T]
                    return type_array(type_copy(arg), 1); // Default size 1, should be parameterized
                }
                
                if (strcmp(hkt->type_constructor->name, "Slice") == 0) {
                    // Create slice type: Slice<T> -> []T
                    return type_slice(type_copy(arg));
                }
                
                if (strcmp(hkt->type_constructor->name, "Option") == 0) {
                    // Create nullable type: Option<T> -> ?T
                    return type_nullable(type_copy(arg));
                }
                
                if (strcmp(hkt->type_constructor->name, "Result") == 0) {
                    // Create error union type: Result<T> -> !T
                    return type_error_union(type_copy(arg), NULL);
                }
                
                if (strcmp(hkt->type_constructor->name, "Ptr") == 0) {
                    // Create pointer type: Ptr<T> -> *T
                    return type_pointer(type_copy(arg));
                }
                
                if (strcmp(hkt->type_constructor->name, "Chan") == 0) {
                    // Create channel type: Chan<T> -> chan T
                    return type_channel(type_copy(arg), CHAN_PATTERN_BASIC);
                }
            }
            
            // For unknown type constructors, create a generic instantiation
            // This would need to be extended with proper type constructor resolution
            return type_copy(arg);
        }
        
        case HKT_TYPE_TO_TYPE_TO_TYPE: {
            // Apply two type arguments to create a concrete type
            if (arg_count != 2) return NULL;
            Type* arg1 = arguments[0];
            Type* arg2 = arguments[1];
            
            // For common binary type constructors
            if (hkt->type_constructor && hkt->type_constructor->name) {
                if (strcmp(hkt->type_constructor->name, "Map") == 0) {
                    // Create map type: Map<K, V> -> map[K]V
                    return type_map(type_copy(arg1), type_copy(arg2));
                }
                
                if (strcmp(hkt->type_constructor->name, "Function") == 0 ||
                    strcmp(hkt->type_constructor->name, "Fn") == 0) {
                    // Create function type: Fn<T, U> -> T -> U
                    Type** param_types = malloc(sizeof(Type*));
                    if (param_types) {
                        param_types[0] = type_copy(arg1);
                        return type_function(param_types, 1, type_copy(arg2));
                    }
                }
            }
            
            // Default: return the second argument (return type)
            return type_copy(arg2);
        }
        
        case HKT_CONSTRAINT:
        case HKT_ROW:
        case HKT_EFFECT:
            // These kinds need special handling based on the constraint/effect system
            // For now, return a copy of the first argument
            if (arg_count > 0) {
                return type_copy(arguments[0]);
            }
            return type_copy(hkt->type_constructor);
            
        default:
            return NULL;
    }
}

// =============================================================================
// Kind Inference and Checking
// =============================================================================

// Infer the kind of a type
HigherKindedTypeKind infer_type_kind(Type* type) {
    if (!type) return HKT_TYPE;
    
    switch (type->kind) {
        case TYPE_VOID:
        case TYPE_BOOL:
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT32:
        case TYPE_INT64:
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64:
        case TYPE_FLOAT32:
        case TYPE_FLOAT64:
        case TYPE_STRING:
        case TYPE_CHAR:
            return HKT_TYPE; // Concrete types have kind *
            
        case TYPE_ARRAY:
        case TYPE_SLICE:
        case TYPE_POINTER:
        case TYPE_NULLABLE:
        case TYPE_ERROR_UNION:
            return HKT_TYPE_TO_TYPE; // Type constructors: * -> *
            
        case TYPE_MAP:
        case TYPE_FUNCTION:
            return HKT_TYPE_TO_TYPE_TO_TYPE; // Binary type constructors: * -> * -> *
            
        case TYPE_INTERFACE:
            return HKT_CONSTRAINT; // Interfaces represent constraints
            
        default:
            return HKT_TYPE;
    }
}

// Check if two kinds are compatible
int kinds_compatible(HigherKindedTypeKind kind1, HigherKindedTypeKind kind2) {
    return kind1 == kind2;
}

// Create a higher-kinded type from a regular type
HigherKindedType* type_to_higher_kinded(Type* type) {
    if (!type) return NULL;
    
    HigherKindedTypeKind kind = infer_type_kind(type);
    return higher_kinded_type_new(kind, type_copy(type));
}

// =============================================================================
// Common Higher-Kinded Type Constructors
// =============================================================================

// Create common HKT constructors
HigherKindedType* create_option_hkt(void) {
    Type* option_constructor = type_new(TYPE_UNKNOWN);
    if (option_constructor) {
        option_constructor->name = str_dup("Option");
    }
    
    return higher_kinded_type_new(HKT_TYPE_TO_TYPE, option_constructor);
}

HigherKindedType* create_result_hkt(void) {
    Type* result_constructor = type_new(TYPE_UNKNOWN);
    if (result_constructor) {
        result_constructor->name = str_dup("Result");
    }
    
    return higher_kinded_type_new(HKT_TYPE_TO_TYPE, result_constructor);
}

HigherKindedType* create_vec_hkt(void) {
    Type* vec_constructor = type_new(TYPE_UNKNOWN);
    if (vec_constructor) {
        vec_constructor->name = str_dup("Vec");
    }
    
    return higher_kinded_type_new(HKT_TYPE_TO_TYPE, vec_constructor);
}

HigherKindedType* create_map_hkt(void) {
    Type* map_constructor = type_new(TYPE_UNKNOWN);
    if (map_constructor) {
        map_constructor->name = str_dup("Map");
    }
    
    return higher_kinded_type_new(HKT_TYPE_TO_TYPE_TO_TYPE, map_constructor);
}

HigherKindedType* create_function_hkt(void) {
    Type* func_constructor = type_new(TYPE_UNKNOWN);
    if (func_constructor) {
        func_constructor->name = str_dup("Fn");
    }
    
    return higher_kinded_type_new(HKT_TYPE_TO_TYPE_TO_TYPE, func_constructor);
}

// =============================================================================
// Higher-Kinded Type Application and Composition
// =============================================================================

// Partial application of a higher-kinded type
HigherKindedType* partial_apply_hkt(HigherKindedType* hkt, Type* argument) {
    if (!hkt || !argument) return NULL;
    
    // Create a new HKT with reduced arity
    HigherKindedTypeKind new_kind;
    switch (hkt->kind) {
        case HKT_TYPE_TO_TYPE:
            new_kind = HKT_TYPE;
            break;
        case HKT_TYPE_TO_TYPE_TO_TYPE:
            new_kind = HKT_TYPE_TO_TYPE;
            break;
        default:
            return NULL; // Can't partial apply
    }
    
    HigherKindedType* new_hkt = higher_kinded_type_new(new_kind, type_copy(hkt->type_constructor));
    if (!new_hkt) return NULL;
    
    // Apply the argument to the first position
    if (new_hkt->type_arguments && hkt->type_arguments) {
        new_hkt->type_arguments[0] = type_copy(argument);
        
        // Copy remaining arguments if any
        for (size_t i = 1; i < hkt->arity && i - 1 < new_hkt->arity; i++) {
            if (hkt->type_arguments[i]) {
                new_hkt->type_arguments[i - 1] = type_copy(hkt->type_arguments[i]);
            }
        }
    }
    
    return new_hkt;
}

// Compose two higher-kinded types
HigherKindedType* compose_hkt(HigherKindedType* outer, HigherKindedType* inner) {
    if (!outer || !inner) return NULL;
    
    // For now, implement simple composition for common cases
    // This would be extended with full kind checking and composition rules
    
    if (outer->kind == HKT_TYPE_TO_TYPE && inner->kind == HKT_TYPE_TO_TYPE) {
        // Compose F<G<T>> style
        Type* composed_constructor = type_new(TYPE_UNKNOWN);
        if (composed_constructor) {
            // Create a name representing the composition
            if (outer->type_constructor && outer->type_constructor->name &&
                inner->type_constructor && inner->type_constructor->name) {
                char* composed_name = malloc(strlen(outer->type_constructor->name) + 
                                           strlen(inner->type_constructor->name) + 10);
                if (composed_name) {
                    sprintf(composed_name, "%s<%s>", outer->type_constructor->name, 
                           inner->type_constructor->name);
                    composed_constructor->name = composed_name;
                }
            }
        }
        
        return higher_kinded_type_new(HKT_TYPE_TO_TYPE, composed_constructor);
    }
    
    return NULL; // Composition not supported for these kinds
}

// =============================================================================
// Kind Polymorphism and Generic HKTs
// =============================================================================

// Create a kind-polymorphic higher-kinded type
typedef struct {
    char* name;
    char* kind_variable; // e.g., "k" in "forall k. k -> *"
    HigherKindedType* body;
} KindPolymorphicType;

KindPolymorphicType* create_kind_polymorphic_type(const char* name, const char* kind_var, 
                                                 HigherKindedType* body) {
    KindPolymorphicType* kpt = malloc(sizeof(KindPolymorphicType));
    if (!kpt) return NULL;
    
    kpt->name = str_dup(name);
    kpt->kind_variable = str_dup(kind_var);
    kpt->body = body;
    
    return kpt;
}

void free_kind_polymorphic_type(KindPolymorphicType* kpt) {
    if (!kpt) return;
    
    free(kpt->name);
    free(kpt->kind_variable);
    higher_kinded_type_free(kpt->body);
    free(kpt);
}

// =============================================================================
// HKT-Based Generic Programming Patterns
// =============================================================================

// Functor pattern: F<A> -> (A -> B) -> F<B>
Type* functor_map(HigherKindedType* functor_type, Type* input_element, Type* output_element) {
    if (!functor_type || !input_element || !output_element) return NULL;
    
    // Create the mapping function type A -> B
    Type** param_types = malloc(sizeof(Type*));
    if (!param_types) return NULL;
    param_types[0] = type_copy(input_element);
    Type* map_func_type = type_function(param_types, 1, type_copy(output_element));
    
    if (!map_func_type) {
        free(param_types);
        return NULL;
    }
    
    // Apply the functor to the output element type
    Type* result_args[] = { output_element };
    Type* result = higher_kinded_type_instantiate(functor_type, result_args, 1);
    
    type_free(map_func_type);
    return result;
}

// Monad pattern: M<A> -> (A -> M<B>) -> M<B>
Type* monad_bind(HigherKindedType* monad_type, Type* input_element, Type* output_element) {
    if (!monad_type || !input_element || !output_element) return NULL;
    
    // Create M<B>
    Type* output_args[] = { output_element };
    Type* monad_output = higher_kinded_type_instantiate(monad_type, output_args, 1);
    if (!monad_output) return NULL;
    
    // Create the binding function type A -> M<B>
    Type** param_types = malloc(sizeof(Type*));
    if (!param_types) {
        type_free(monad_output);
        return NULL;
    }
    param_types[0] = type_copy(input_element);
    Type* bind_func_type = type_function(param_types, 1, monad_output);
    
    if (!bind_func_type) {
        free(param_types);
        return NULL;
    }
    
    // Result is also M<B>
    Type* result_args[] = { output_element };
    Type* result = higher_kinded_type_instantiate(monad_type, result_args, 1);
    
    type_free(bind_func_type);
    return result;
}

// =============================================================================
// Utility Functions
// =============================================================================

const char* higher_kinded_type_kind_to_string(HigherKindedTypeKind kind) {
    switch (kind) {
        case HKT_TYPE: return "*";
        case HKT_TYPE_TO_TYPE: return "* -> *";
        case HKT_TYPE_TO_TYPE_TO_TYPE: return "* -> * -> *";
        case HKT_CONSTRAINT: return "Constraint";
        case HKT_ROW: return "Row";
        case HKT_EFFECT: return "Effect";
        default: return "Unknown";
    }
}

void print_higher_kinded_type(const HigherKindedType* hkt) {
    if (!hkt) {
        printf("HigherKindedType: null\n");
        return;
    }
    
    printf("HigherKindedType:\n");
    printf("  Kind: %s\n", higher_kinded_type_kind_to_string(hkt->kind));
    printf("  Arity: %zu\n", hkt->arity);
    
    if (hkt->kind_signature) {
        printf("  Kind Signature: %s\n", hkt->kind_signature);
    }
    
    if (hkt->type_constructor && hkt->type_constructor->name) {
        printf("  Type Constructor: %s\n", hkt->type_constructor->name);
    }
    
    if (hkt->type_arguments) {
        printf("  Type Arguments:\n");
        for (size_t i = 0; i < hkt->arity; i++) {
            if (hkt->type_arguments[i]) {
                printf("    [%zu]: %s\n", i, 
                       hkt->type_arguments[i]->name ? hkt->type_arguments[i]->name : "<unnamed>");
            } else {
                printf("    [%zu]: <unbound>\n", i);
            }
        }
    }
}

// Check if a higher-kinded type is fully applied
int hkt_is_fully_applied(const HigherKindedType* hkt) {
    if (!hkt) return 0;
    
    if (hkt->arity == 0) return 1; // Nullary types are always fully applied
    
    if (!hkt->type_arguments) return 0;
    
    for (size_t i = 0; i < hkt->arity; i++) {
        if (!hkt->type_arguments[i]) {
            return 0; // Found an unbound argument
        }
    }
    
    return 1; // All arguments are bound
}

// Get the number of unbound type arguments
size_t hkt_unbound_count(const HigherKindedType* hkt) {
    if (!hkt || !hkt->type_arguments) return hkt ? hkt->arity : 0;
    
    size_t unbound = 0;
    for (size_t i = 0; i < hkt->arity; i++) {
        if (!hkt->type_arguments[i]) {
            unbound++;
        }
    }
    
    return unbound;
}
