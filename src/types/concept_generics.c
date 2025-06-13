#include "interface_system.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =============================================================================
// 22.2: Concept-Based Generics Framework Implementation
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
// Concept Definition Management
// =============================================================================

ConceptDefinition* concept_definition_new(const char* name, Position pos) {
    ConceptDefinition* concept = malloc(sizeof(ConceptDefinition));
    if (!concept) return NULL;
    
    concept->name = str_dup(name);
    concept->type_parameters = NULL;
    concept->requirements = constraint_set_new();
    concept->required_methods = NULL;
    concept->associated_types = NULL;
    concept->associated_type_count = 0;
    concept->super_concepts = NULL;
    concept->super_concept_count = 0;
    concept->is_auto_concept = 0;
    concept->defined_pos = pos;
    concept->next = NULL;
    
    return concept;
}

void concept_definition_free(ConceptDefinition* concept) {
    if (!concept) return;
    
    free(concept->name);
    
    // Free type parameters
    TypeVariable* var = concept->type_parameters;
    while (var) {
        TypeVariable* next = var->next;
        type_variable_free(var);
        var = next;
    }
    
    // Free requirements
    constraint_set_free(concept->requirements);
    
    // Free required methods
    // Note: This assumes InterfaceMethod has a specific structure
    // In practice, you'd need proper cleanup for InterfaceMethod
    
    // Free associated types
    if (concept->associated_types) {
        for (size_t i = 0; i < concept->associated_type_count; i++) {
            if (concept->associated_types[i]) {
                type_free(concept->associated_types[i]);
            }
        }
        free(concept->associated_types);
    }
    
    // Free super concepts array (not the concepts themselves)
    free(concept->super_concepts);
    
    free(concept);
}

int concept_add_requirement(ConceptDefinition* concept, InterfaceConstraint* requirement) {
    if (!concept || !requirement) return 0;
    
    return constraint_set_add(concept->requirements, requirement);
}

int concept_add_super_concept(ConceptDefinition* concept, ConceptDefinition* super_concept) {
    if (!concept || !super_concept) return 0;
    
    // Reallocate the super concepts array
    ConceptDefinition** new_super_concepts = realloc(concept->super_concepts, 
        sizeof(ConceptDefinition*) * (concept->super_concept_count + 1));
    if (!new_super_concepts) return 0;
    
    new_super_concepts[concept->super_concept_count] = super_concept;
    concept->super_concepts = new_super_concepts;
    concept->super_concept_count++;
    
    return 1;
}

int type_satisfies_concept(Type* type, ConceptDefinition* concept, TypeChecker* checker) {
    if (!type || !concept || !checker) return 0;
    
    // Check if a type satisfies all requirements of a concept
    
    // 1. Check all constraints in the concept's requirements
    if (concept->requirements) {
        InterfaceConstraint* requirement = concept->requirements->constraints;
        while (requirement) {
            // Check if the type satisfies this specific requirement
            switch (requirement->kind) {
                case CONSTRAINT_NUMERIC:
                    if (!type_is_numeric(type)) {
                        return 0;
                    }
                    break;
                    
                case CONSTRAINT_COPY:
                    // Check if type supports copying (not pointer-like usually)
                    if (type_is_pointer_like(type)) {
                        return 0;
                    }
                    break;
                    
                case CONSTRAINT_SIZE:
                    // Check if type has known size at compile time
                    // For now, assume all non-dynamic types are sized
                    if (type->kind == TYPE_SLICE || type->kind == TYPE_MAP) {
                        return 0; // Dynamic types are not sized
                    }
                    break;
                    
                case CONSTRAINT_PARTIAL_EQ:
                    // Check if type supports equality comparison
                    // For now, most types support this except functions
                    if (type->kind == TYPE_FUNCTION) {
                        return 0;
                    }
                    break;
                    
                case CONSTRAINT_PARTIAL_ORD:
                    // Check if type supports ordering comparison
                    // Numeric types and strings typically support this
                    if (!type_is_numeric(type) && type->kind != TYPE_STRING && type->kind != TYPE_CHAR) {
                        return 0;
                    }
                    break;
                    
                case CONSTRAINT_IMPLEMENTS:
                    // Check if type implements a specific interface/protocol
                    if (requirement->protocol_name) {
                        // TODO: Look up the protocol and check conformance
                        // For now, return true as a placeholder
                    }
                    break;
                    
                default:
                    // For other constraints, assume they're satisfied for now
                    // In a full implementation, each constraint type would be checked
                    break;
            }
            
            requirement = requirement->next;
        }
    }
    
    // 2. Check if type provides all required methods
    // This would involve checking if the type (or its implementations) 
    // provides methods with compatible signatures
    
    // 3. Check if type satisfies all super concept requirements
    for (size_t i = 0; i < concept->super_concept_count; i++) {
        if (!type_satisfies_concept(type, concept->super_concepts[i], checker)) {
            return 0;
        }
    }
    
    return 1; // All requirements satisfied
}

// =============================================================================
// Concept-Based Generic Functions
// =============================================================================

// Create a generic function type with concept constraints
Type* create_concept_constrained_function(ConceptDefinition* concepts[], size_t concept_count,
                                         Type** param_types, size_t param_count,
                                         Type* return_type) {
    if (!concepts || !param_types || !return_type) return NULL;
    
    // Create a function type
    Type* func_type = type_function(param_types, param_count, return_type);
    if (!func_type) return NULL;
    
    // TODO: Attach concept constraints to the function type
    // This would require extending the Type structure to include constraint information
    // For now, we just return the basic function type
    
    return func_type;
}

// Check if a function call satisfies concept constraints
int check_concept_constrained_call(Type* function_type, Type** arg_types, size_t arg_count,
                                  ConceptDefinition* concepts[], size_t concept_count,
                                  TypeChecker* checker) {
    if (!function_type || !arg_types || !concepts || !checker) return 0;
    
    if (function_type->kind != TYPE_FUNCTION) return 0;
    
    // Check if argument types satisfy the concept constraints
    for (size_t i = 0; i < arg_count && i < function_type->data.function.param_count; i++) {
        Type* arg_type = arg_types[i];
        
        // Check against all applicable concepts
        for (size_t j = 0; j < concept_count; j++) {
            if (!type_satisfies_concept(arg_type, concepts[j], checker)) {
                return 0; // Concept not satisfied
            }
        }
    }
    
    return 1; // All concept constraints satisfied
}

// =============================================================================
// Common Concepts Library
// =============================================================================

// Create standard concepts that are commonly used
ConceptDefinition* create_numeric_concept(Position pos) {
    ConceptDefinition* concept = concept_definition_new("Numeric", pos);
    if (!concept) return NULL;
    
    // Add numeric constraint
    InterfaceConstraint* numeric_constraint = interface_constraint_new(CONSTRAINT_NUMERIC, NULL, pos);
    if (numeric_constraint) {
        concept_add_requirement(concept, numeric_constraint);
    }
    
    // Add arithmetic constraint
    InterfaceConstraint* arithmetic_constraint = interface_constraint_new(CONSTRAINT_ARITHMETIC, NULL, pos);
    if (arithmetic_constraint) {
        concept_add_requirement(concept, arithmetic_constraint);
    }
    
    concept->is_auto_concept = 1; // This is automatically implemented for numeric types
    
    return concept;
}

ConceptDefinition* create_comparable_concept(Position pos) {
    ConceptDefinition* concept = concept_definition_new("Comparable", pos);
    if (!concept) return NULL;
    
    // Add partial equality constraint
    InterfaceConstraint* eq_constraint = interface_constraint_new(CONSTRAINT_PARTIAL_EQ, NULL, pos);
    if (eq_constraint) {
        concept_add_requirement(concept, eq_constraint);
    }
    
    // Add partial ordering constraint
    InterfaceConstraint* ord_constraint = interface_constraint_new(CONSTRAINT_PARTIAL_ORD, NULL, pos);
    if (ord_constraint) {
        concept_add_requirement(concept, ord_constraint);
    }
    
    return concept;
}

ConceptDefinition* create_copyable_concept(Position pos) {
    ConceptDefinition* concept = concept_definition_new("Copyable", pos);
    if (!concept) return NULL;
    
    // Add copy constraint
    InterfaceConstraint* copy_constraint = interface_constraint_new(CONSTRAINT_COPY, NULL, pos);
    if (copy_constraint) {
        concept_add_requirement(concept, copy_constraint);
    }
    
    // Add size constraint (copied types must be sized)
    InterfaceConstraint* size_constraint = interface_constraint_new(CONSTRAINT_SIZE, NULL, pos);
    if (size_constraint) {
        concept_add_requirement(concept, size_constraint);
    }
    
    return concept;
}

ConceptDefinition* create_displayable_concept(Position pos) {
    ConceptDefinition* concept = concept_definition_new("Displayable", pos);
    if (!concept) return NULL;
    
    // Add display constraint
    InterfaceConstraint* display_constraint = interface_constraint_new(CONSTRAINT_DISPLAY, NULL, pos);
    if (display_constraint) {
        concept_add_requirement(concept, display_constraint);
    }
    
    return concept;
}

ConceptDefinition* create_iterator_concept(Position pos) {
    ConceptDefinition* concept = concept_definition_new("Iterator", pos);
    if (!concept) return NULL;
    
    // Add iterator constraint
    InterfaceConstraint* iter_constraint = interface_constraint_new(CONSTRAINT_ITERATOR, NULL, pos);
    if (iter_constraint) {
        concept_add_requirement(concept, iter_constraint);
    }
    
    // TODO: Add required methods like next(), has_next(), etc.
    // This would require extending InterfaceMethod creation
    
    return concept;
}

ConceptDefinition* create_container_concept(Position pos) {
    ConceptDefinition* concept = concept_definition_new("Container", pos);
    if (!concept) return NULL;
    
    // Add indexing constraint
    InterfaceConstraint* index_constraint = interface_constraint_new(CONSTRAINT_INDEX, NULL, pos);
    if (index_constraint) {
        concept_add_requirement(concept, index_constraint);
    }
    
    // Add size constraint
    InterfaceConstraint* size_constraint = interface_constraint_new(CONSTRAINT_SIZE, NULL, pos);
    if (size_constraint) {
        concept_add_requirement(concept, size_constraint);
    }
    
    // Container should also be iterable
    ConceptDefinition* iter_concept = create_iterator_concept(pos);
    if (iter_concept) {
        concept_add_super_concept(concept, iter_concept);
    }
    
    return concept;
}

// =============================================================================
// Concept Inference and Auto-Implementation
// =============================================================================

// Automatically infer what concepts a type satisfies
ConceptDefinition** infer_type_concepts(Type* type, TypeChecker* checker, size_t* concept_count) {
    if (!type || !checker || !concept_count) return NULL;
    
    *concept_count = 0;
    
    // Start with a reasonable initial capacity
    size_t capacity = 8;
    ConceptDefinition** concepts = malloc(sizeof(ConceptDefinition*) * capacity);
    if (!concepts) return NULL;
    
    Position pos = {0, 0, 0, "auto-inferred"};
    
    // Check for numeric concept
    if (type_is_numeric(type)) {
        if (*concept_count >= capacity) {
            capacity *= 2;
            ConceptDefinition** new_concepts = realloc(concepts, sizeof(ConceptDefinition*) * capacity);
            if (!new_concepts) {
                free(concepts);
                return NULL;
            }
            concepts = new_concepts;
        }
        concepts[(*concept_count)++] = create_numeric_concept(pos);
    }
    
    // Check for comparable concept (most types support comparison)
    if (type->kind != TYPE_FUNCTION && type->kind != TYPE_VOID) {
        if (*concept_count >= capacity) {
            capacity *= 2;
            ConceptDefinition** new_concepts = realloc(concepts, sizeof(ConceptDefinition*) * capacity);
            if (!new_concepts) {
                for (size_t i = 0; i < *concept_count; i++) {
                    concept_definition_free(concepts[i]);
                }
                free(concepts);
                return NULL;
            }
            concepts = new_concepts;
        }
        concepts[(*concept_count)++] = create_comparable_concept(pos);
    }
    
    // Check for copyable concept (non-pointer types are typically copyable)
    if (!type_is_pointer_like(type) && type->kind != TYPE_FUNCTION) {
        if (*concept_count >= capacity) {
            capacity *= 2;
            ConceptDefinition** new_concepts = realloc(concepts, sizeof(ConceptDefinition*) * capacity);
            if (!new_concepts) {
                for (size_t i = 0; i < *concept_count; i++) {
                    concept_definition_free(concepts[i]);
                }
                free(concepts);
                return NULL;
            }
            concepts = new_concepts;
        }
        concepts[(*concept_count)++] = create_copyable_concept(pos);
    }
    
    // Check for container concept
    if (type->kind == TYPE_ARRAY || type->kind == TYPE_SLICE) {
        if (*concept_count >= capacity) {
            capacity *= 2;
            ConceptDefinition** new_concepts = realloc(concepts, sizeof(ConceptDefinition*) * capacity);
            if (!new_concepts) {
                for (size_t i = 0; i < *concept_count; i++) {
                    concept_definition_free(concepts[i]);
                }
                free(concepts);
                return NULL;
            }
            concepts = new_concepts;
        }
        concepts[(*concept_count)++] = create_container_concept(pos);
    }
    
    // Check for displayable concept (most types can be displayed)
    if (type->kind != TYPE_FUNCTION && type->kind != TYPE_VOID) {
        if (*concept_count >= capacity) {
            capacity *= 2;
            ConceptDefinition** new_concepts = realloc(concepts, sizeof(ConceptDefinition*) * capacity);
            if (!new_concepts) {
                for (size_t i = 0; i < *concept_count; i++) {
                    concept_definition_free(concepts[i]);
                }
                free(concepts);
                return NULL;
            }
            concepts = new_concepts;
        }
        concepts[(*concept_count)++] = create_displayable_concept(pos);
    }
    
    return concepts;
}

// Check if a generic function can be instantiated with given types
int can_instantiate_generic_function(Type* generic_func_type, Type** type_arguments, size_t arg_count,
                                    ConceptDefinition** constraints, size_t constraint_count,
                                    TypeChecker* checker) {
    if (!generic_func_type || !type_arguments || !checker) return 0;
    
    // Check each type argument against the concept constraints
    for (size_t i = 0; i < arg_count && i < constraint_count; i++) {
        Type* arg_type = type_arguments[i];
        ConceptDefinition* constraint = constraints[i];
        
        if (!type_satisfies_concept(arg_type, constraint, checker)) {
            return 0; // Type doesn't satisfy the concept constraint
        }
    }
    
    return 1; // All constraints satisfied
}

// Generate specialized function instance
Type* instantiate_generic_function(Type* generic_func_type, Type** type_arguments, size_t arg_count) {
    if (!generic_func_type || !type_arguments || generic_func_type->kind != TYPE_FUNCTION) return NULL;
    
    // Create a new function type with substituted type arguments
    // This is a simplified implementation - in practice, this would involve
    // complex type substitution throughout the function signature
    
    Type** new_param_types = NULL;
    size_t param_count = generic_func_type->data.function.param_count;
    
    if (param_count > 0) {
        new_param_types = malloc(sizeof(Type*) * param_count);
        if (!new_param_types) return NULL;
        
        for (size_t i = 0; i < param_count; i++) {
            // For now, just copy the original parameter types
            // In practice, this would substitute type variables with type arguments
            new_param_types[i] = type_copy(generic_func_type->data.function.param_types[i]);
        }
    }
    
    Type* new_return_type = type_copy(generic_func_type->data.function.return_type);
    
    return type_function(new_param_types, param_count, new_return_type);
}

// =============================================================================
// Utility Functions for Concepts
// =============================================================================

void print_concept_definition(const ConceptDefinition* concept) {
    if (!concept) {
        printf("ConceptDefinition: null\n");
        return;
    }
    
    printf("Concept: %s\n", concept->name ? concept->name : "<unnamed>");
    
    if (concept->type_parameters) {
        printf("  Type Parameters:\n");
        TypeVariable* param = concept->type_parameters;
        while (param) {
            printf("    - %s (%s)\n", 
                   param->name ? param->name : "<unnamed>",
                   type_variable_kind_to_string(param->kind));
            param = param->next;
        }
    }
    
    if (concept->requirements) {
        printf("  Requirements:\n");
        print_constraint_set(concept->requirements);
    }
    
    if (concept->super_concept_count > 0) {
        printf("  Super Concepts: %zu\n", concept->super_concept_count);
        for (size_t i = 0; i < concept->super_concept_count; i++) {
            if (concept->super_concepts[i]) {
                printf("    - %s\n", concept->super_concepts[i]->name ? concept->super_concepts[i]->name : "<unnamed>");
            }
        }
    }
    
    if (concept->is_auto_concept) {
        printf("  [auto-concept]\n");
    }
}

int concept_is_subtype_of(ConceptDefinition* sub_concept, ConceptDefinition* super_concept) {
    if (!sub_concept || !super_concept) return 0;
    
    // Check if sub_concept is a subtype of super_concept
    // This happens if sub_concept extends super_concept (directly or indirectly)
    
    for (size_t i = 0; i < sub_concept->super_concept_count; i++) {
        ConceptDefinition* super = sub_concept->super_concepts[i];
        if (super == super_concept) {
            return 1; // Direct subtype
        }
        
        // Check transitively
        if (concept_is_subtype_of(super, super_concept)) {
            return 1; // Indirect subtype
        }
    }
    
    return 0; // Not a subtype
}
