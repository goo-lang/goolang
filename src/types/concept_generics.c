#include "interface_system.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =============================================================================
// Forward declarations and stub implementations for missing functions
// =============================================================================

// Simplified stub implementations for missing functions
// interface_method_copy is now implemented in protocol_oriented_programming.c

Type* type_interface_enhanced(InterfaceMethod* methods, size_t method_count, ConstraintSet* constraints) {
    (void)constraints; // Unused for now
    Type* interface_type = type_new(TYPE_INTERFACE);
    if (interface_type && methods && method_count > 0) {
        interface_type->data.interface.methods = methods;
        interface_type->data.interface.method_count = method_count;
        interface_type->data.interface.name = strdup("SynthesizedInterface");
        interface_type->data.interface.is_synthesized = 0;
        interface_type->data.interface.source_concept = NULL;
    }
    return interface_type;
}

int constraint_inference_add_constraint(ConstraintInferenceEngine* engine, InterfaceConstraint* constraint) {
    // Simplified implementation
    if (!engine || !constraint) return 0;
    return constraint_set_add(engine->active_constraints, constraint);
}

int concept_registry_register(ConceptRegistry* registry, ConceptDefinition* concept) {
    // Simplified implementation
    if (!registry || !concept) return 0;
    // In practice, would add to registry data structure
    return 1;
}

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
    if (!name) return NULL;
    
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
                    // Check if type supports copying (not pointer-like or functions)
                    if (type_is_pointer_like(type) || type->kind == TYPE_FUNCTION) {
                        return 0;
                    }
                    break;
                    
                case CONSTRAINT_SIZE:
                    // Check if type has known size at compile time
                    // For now, assume all non-dynamic types are sized
                    // Note: Slices have runtime size, so they're considered sized for containers
                    if (type->kind == TYPE_MAP) {
                        return 0; // Maps are truly dynamic and not sized
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
                    
                case CONSTRAINT_INDEX:
                    // Check if type supports indexing (arrays, slices, etc.)
                    if (type->kind != TYPE_ARRAY && type->kind != TYPE_SLICE && 
                        type->kind != TYPE_STRING && type->kind != TYPE_MAP) {
                        return 0; // Only certain types support indexing
                    }
                    break;
                    
                case CONSTRAINT_ITERATOR:
                    // Check if type is iterable (arrays, slices, strings, etc.)
                    if (type->kind != TYPE_ARRAY && type->kind != TYPE_SLICE && 
                        type->kind != TYPE_STRING && type->kind != TYPE_MAP) {
                        return 0; // Only certain types are iterable
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
// Enhanced Concept Definition and Requirements System
// =============================================================================

// Add a method requirement to a concept
int concept_add_method_requirement(ConceptDefinition* concept, const char* method_name, 
                                  Type* method_signature, Position pos) {
    if (!concept || !method_name || !method_signature) return 0;
    
    // Create a new interface method for the requirement
    InterfaceMethod* method = malloc(sizeof(InterfaceMethod));
    if (!method) return 0;
    
    method->name = str_dup(method_name);
    method->type = type_copy(method_signature);
    
    // Add to the linked list of required methods
    // Note: This assumes InterfaceMethod has a 'next' field for linked list
    // In practice, you might need to adjust this based on the actual structure
    method->next = concept->required_methods;
    concept->required_methods = method;
    
    return 1;
}

// Add an associated type to a concept
int concept_add_associated_type(ConceptDefinition* concept, Type* associated_type) {
    if (!concept || !associated_type) return 0;
    
    // Reallocate the associated types array
    Type** new_types = realloc(concept->associated_types, 
        sizeof(Type*) * (concept->associated_type_count + 1));
    if (!new_types) return 0;
    
    new_types[concept->associated_type_count] = type_copy(associated_type);
    concept->associated_types = new_types;
    concept->associated_type_count++;
    
    return 1;
}

// Add a type parameter to a concept
int concept_add_type_parameter(ConceptDefinition* concept, const char* param_name, 
                              TypeVariableKind kind, Position pos) {
    if (!concept || !param_name) return 0;
    
    TypeVariable* param = type_variable_new(param_name, kind, pos);
    if (!param) return 0;
    
    // Add to the linked list of type parameters
    param->next = concept->type_parameters;
    concept->type_parameters = param;
    
    return 1;
}

// Check if a concept is well-formed (no circular dependencies, etc.)
int concept_is_well_formed(ConceptDefinition* concept) {
    if (!concept) return 0;
    
    // Check for circular dependencies in super concepts
    return concept_check_circular_dependencies(concept, NULL, 0);
}

// Helper function to check circular dependencies
int concept_check_circular_dependencies(ConceptDefinition* concept, 
                                       ConceptDefinition** visited, size_t visited_count) {
    if (!concept) return 1;
    
    // Check if this concept is already in the visited list
    for (size_t i = 0; i < visited_count; i++) {
        if (visited[i] == concept) {
            return 0; // Circular dependency detected
        }
    }
    
    // Add this concept to the visited list
    ConceptDefinition** new_visited = malloc(sizeof(ConceptDefinition*) * (visited_count + 1));
    if (!new_visited) return 0;
    
    if (visited) {
        memcpy(new_visited, visited, sizeof(ConceptDefinition*) * visited_count);
    }
    new_visited[visited_count] = concept;
    
    // Recursively check super concepts
    int result = 1;
    for (size_t i = 0; i < concept->super_concept_count && result; i++) {
        result = concept_check_circular_dependencies(concept->super_concepts[i], 
                                                   new_visited, visited_count + 1);
    }
    
    free(new_visited);
    return result;
}

// Enhanced concept satisfaction checking with method requirements
int type_satisfies_concept_enhanced(Type* type, ConceptDefinition* concept, TypeChecker* checker) {
    if (!type || !concept || !checker) return 0;
    
    // First check basic constraint requirements
    if (!type_satisfies_concept(type, concept, checker)) {
        return 0;
    }
    
    // Check method requirements
    InterfaceMethod* required_method = concept->required_methods;
    while (required_method) {
        if (!type_has_method(type, required_method->name, required_method->type, checker)) {
            return 0; // Required method not found or incompatible
        }
        required_method = required_method->next;
    }
    
    // Check associated type requirements
    for (size_t i = 0; i < concept->associated_type_count; i++) {
        Type* assoc_type = concept->associated_types[i];
        if (!type_has_associated_type(type, assoc_type, checker)) {
            return 0; // Required associated type not found
        }
    }
    
    return 1;
}

// type_has_method is implemented in protocol_oriented_programming.c

// Check if a type has a specific associated type
int type_has_associated_type(Type* type, Type* associated_type, TypeChecker* checker) {
    if (!type || !associated_type || !checker) return 0;
    
    // This is a placeholder for associated type checking
    // In practice, this would involve complex type analysis
    return 1;
}

// =============================================================================
// Concept Composition and Refinement
// =============================================================================

// Create a concept that combines multiple concepts
ConceptDefinition* create_concept_composition(const char* name, 
                                            ConceptDefinition** base_concepts, 
                                            size_t base_count, Position pos) {
    if (!name || !base_concepts || base_count == 0) return NULL;
    
    ConceptDefinition* composite = concept_definition_new(name, pos);
    if (!composite) return NULL;
    
    // Add all base concepts as super concepts
    for (size_t i = 0; i < base_count; i++) {
        if (!concept_add_super_concept(composite, base_concepts[i])) {
            concept_definition_free(composite);
            return NULL;
        }
    }
    
    // Merge requirements from all base concepts
    for (size_t i = 0; i < base_count; i++) {
        ConceptDefinition* base = base_concepts[i];
        
        // Merge constraints
        if (base->requirements) {
            constraint_set_merge(composite->requirements, base->requirements);
        }
        
        // Copy method requirements
        InterfaceMethod* method = base->required_methods;
        while (method) {
            concept_add_method_requirement(composite, method->name, method->type, pos);
            method = method->next;
        }
        
        // Copy associated types
        for (size_t j = 0; j < base->associated_type_count; j++) {
            concept_add_associated_type(composite, base->associated_types[j]);
        }
    }
    
    return composite;
}

// Create a concept that refines another concept with additional requirements
ConceptDefinition* create_concept_refinement(const char* name, ConceptDefinition* base_concept,
                                           InterfaceConstraint** additional_constraints,
                                           size_t constraint_count, Position pos) {
    if (!name || !base_concept) return NULL;
    
    ConceptDefinition* refined = concept_definition_new(name, pos);
    if (!refined) return NULL;
    
    // Add base concept as super concept
    if (!concept_add_super_concept(refined, base_concept)) {
        concept_definition_free(refined);
        return NULL;
    }
    
    // Add additional constraints
    for (size_t i = 0; i < constraint_count; i++) {
        if (additional_constraints[i]) {
            concept_add_requirement(refined, additional_constraints[i]);
        }
    }
    
    return refined;
}

// =============================================================================
// Advanced Concept Library
// =============================================================================

// Create a Functor concept (higher-kinded type that supports map)
ConceptDefinition* create_functor_concept(Position pos) {
    ConceptDefinition* concept = concept_definition_new("Functor", pos);
    if (!concept) return NULL;
    
    // Add type parameter for the functor itself
    concept_add_type_parameter(concept, "F", TYPE_VAR_HIGHER_KINDED, pos);
    concept_add_type_parameter(concept, "A", TYPE_VAR_GENERIC, pos);
    concept_add_type_parameter(concept, "B", TYPE_VAR_GENERIC, pos);
    
    // Functor requires a map method: (A -> B) -> F<A> -> F<B>
    Type** map_params = malloc(sizeof(Type*) * 2);
    map_params[0] = type_function(NULL, 0, NULL); // (A -> B) function
    map_params[1] = type_new(TYPE_UNKNOWN); // F<A>
    Type* map_return = type_new(TYPE_UNKNOWN); // F<B>
    Type* map_signature = type_function(map_params, 2, map_return);
    
    concept_add_method_requirement(concept, "map", map_signature, pos);
    
    return concept;
}

// Create a Monad concept (Functor + additional operations)
ConceptDefinition* create_monad_concept(Position pos) {
    ConceptDefinition* monad = concept_definition_new("Monad", pos);
    if (!monad) return NULL;
    
    // Monad extends Functor
    ConceptDefinition* functor = create_functor_concept(pos);
    if (functor) {
        concept_add_super_concept(monad, functor);
    }
    
    // Add type parameters
    concept_add_type_parameter(monad, "M", TYPE_VAR_HIGHER_KINDED, pos);
    concept_add_type_parameter(monad, "A", TYPE_VAR_GENERIC, pos);
    concept_add_type_parameter(monad, "B", TYPE_VAR_GENERIC, pos);
    
    // Monad requires return/pure method: A -> M<A>
    Type** return_params = malloc(sizeof(Type*) * 1);
    return_params[0] = type_new(TYPE_UNKNOWN); // A
    Type* return_result = type_new(TYPE_UNKNOWN); // M<A>
    Type* return_signature = type_function(return_params, 1, return_result);
    
    concept_add_method_requirement(monad, "return", return_signature, pos);
    
    // Monad requires bind/flatMap method: M<A> -> (A -> M<B>) -> M<B>
    Type** bind_params = malloc(sizeof(Type*) * 2);
    bind_params[0] = type_new(TYPE_UNKNOWN); // M<A>
    bind_params[1] = type_function(NULL, 0, NULL); // (A -> M<B>)
    Type* bind_result = type_new(TYPE_UNKNOWN); // M<B>
    Type* bind_signature = type_function(bind_params, 2, bind_result);
    
    concept_add_method_requirement(monad, "bind", bind_signature, pos);
    
    return monad;
}

// Create a Serializable concept with associated types
ConceptDefinition* create_serializable_concept(Position pos) {
    ConceptDefinition* concept = concept_definition_new("Serializable", pos);
    if (!concept) return NULL;
    
    // Add serializable constraint
    InterfaceConstraint* serializable_constraint = interface_constraint_new(CONSTRAINT_SERIALIZABLE, NULL, pos);
    if (serializable_constraint) {
        concept_add_requirement(concept, serializable_constraint);
    }
    
    // Add associated type for serialization format
    Type* format_type = type_new(TYPE_STRING); // Default to string format
    format_type->name = str_dup("SerializationFormat");
    concept_add_associated_type(concept, format_type);
    
    // Require serialize method: Self -> SerializationFormat
    Type** serialize_params = malloc(sizeof(Type*) * 1);
    serialize_params[0] = type_new(TYPE_UNKNOWN); // Self
    Type* serialize_return = type_copy(format_type);
    Type* serialize_signature = type_function(serialize_params, 1, serialize_return);
    
    concept_add_method_requirement(concept, "serialize", serialize_signature, pos);
    
    // Require deserialize method: SerializationFormat -> Self
    Type** deserialize_params = malloc(sizeof(Type*) * 1);
    deserialize_params[0] = type_copy(format_type);
    Type* deserialize_return = type_new(TYPE_UNKNOWN); // Self
    Type* deserialize_signature = type_function(deserialize_params, 1, deserialize_return);
    
    concept_add_method_requirement(concept, "deserialize", deserialize_signature, pos);
    
    return concept;
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
                   "generic"); // Simplified for now
            param = param->next;
        }
    }
    
    if (concept->requirements) {
        printf("  Requirements:\n");
        // Simplified constraint printing
        printf("    - %zu constraints\n", concept->requirements->count);
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

// =============================================================================
// Requires Block Functionality (Task 22.7.1)
// =============================================================================

// Extract required operations from a concept's AST requirements block
int extract_concept_requirements(ConceptDefinition* concept, ASTNode* requirements_ast, TypeChecker* checker) {
    if (!concept || !requirements_ast) return 0;
    
    ASTNode* current = requirements_ast;
    while (current) {
        switch (current->type) {
            case AST_IDENTIFIER: {
                // Simple constraint requirement like "Addable"
                IdentifierNode* ident = (IdentifierNode*)current;
                InterfaceConstraint* constraint = interface_constraint_from_name(ident->name, current->pos);
                if (constraint) {
                    concept_add_requirement(concept, constraint);
                }
                break;
            }
            case AST_FUNC_DECL: {
                // Method requirement - extract and add to concept
                FuncDeclNode* sig = (FuncDeclNode*)current;
                Type* method_type = type_from_func_signature(sig, checker);
                if (method_type) {
                    concept_add_method_requirement(concept, sig->name, method_type, current->pos);
                }
                break;
            }
            case AST_TYPE_DECL: {
                // Associated type requirement
                TypeDeclNode* type_decl = (TypeDeclNode*)current;
                Type* assoc_type = type_from_ast_node(type_decl->type, checker);
                if (assoc_type) {
                    assoc_type->name = strdup(type_decl->name);
                    concept_add_associated_type(concept, assoc_type);
                }
                break;
            }
            default:
                // Handle other requirement types as needed
                break;
        }
        current = current->next;
    }
    
    return 1;
}

// Helper function to create constraint from name
InterfaceConstraint* interface_constraint_from_name(const char* name, Position pos) {
    if (!name) return NULL;
    
    // Map common constraint names to constraint types
    if (strcmp(name, "Numeric") == 0) {
        return interface_constraint_new(CONSTRAINT_NUMERIC, NULL, pos);
    } else if (strcmp(name, "Addable") == 0) {
        return interface_constraint_new(CONSTRAINT_ARITHMETIC, NULL, pos);
    } else if (strcmp(name, "Copyable") == 0) {
        return interface_constraint_new(CONSTRAINT_COPY, NULL, pos);
    } else if (strcmp(name, "Comparable") == 0) {
        return interface_constraint_new(CONSTRAINT_PARTIAL_EQ, NULL, pos);
    } else if (strcmp(name, "Displayable") == 0) {
        return interface_constraint_new(CONSTRAINT_DISPLAY, NULL, pos);
    } else if (strcmp(name, "Iterator") == 0) {
        return interface_constraint_new(CONSTRAINT_ITERATOR, NULL, pos);
    } else if (strcmp(name, "Container") == 0) {
        return interface_constraint_new(CONSTRAINT_INDEX, NULL, pos);
    }
    
    // Default to a simple constraint
    return interface_constraint_new(CONSTRAINT_COPY, NULL, pos);
}

// =============================================================================
// Automatic Interface Synthesis (Task 22.7.2)
// =============================================================================

// Synthesize an interface from concept requirements
Type* synthesize_interface_from_concept(ConceptDefinition* concept, TypeChecker* checker) {
    if (!concept || !checker) return NULL;
    
    // Count required methods to determine interface size
    size_t method_count = 0;
    InterfaceMethod* method = concept->required_methods;
    while (method) {
        method_count++;
        method = method->next;
    }
    
    if (method_count == 0) return NULL;
    
    // Create array of interface methods
    InterfaceMethod* methods = malloc(sizeof(InterfaceMethod) * method_count);
    if (!methods) return NULL;
    
    // Copy methods from concept requirements
    method = concept->required_methods;
    for (size_t i = 0; i < method_count; i++) {
        methods[i] = *interface_method_copy(method);
        method = method->next;
    }
    
    // Create interface type with synthesized methods
    Type* interface_type = type_interface_enhanced(methods, method_count, concept->requirements);
    
    // Set metadata indicating this was synthesized from a concept
    if (interface_type) {
        interface_type->data.interface.is_synthesized = 1;
        interface_type->data.interface.source_concept = concept;
    }
    
    return interface_type;
}

// Generate common operations for concept-conforming types
int generate_common_operations(ConceptDefinition* concept, Type* target_type, TypeChecker* checker) {
    if (!concept || !target_type || !checker) return 0;
    
    // Generate zero() operation for numeric concepts
    if (concept_has_constraint(concept, CONSTRAINT_NUMERIC)) {
        if (!add_zero_operation(target_type, checker)) {
            return 0;
        }
    }
    
    // Generate copy() operation for copyable concepts  
    if (concept_has_constraint(concept, CONSTRAINT_COPY)) {
        if (!add_copy_operation(target_type, checker)) {
            return 0;
        }
    }
    
    // Generate display() operation for displayable concepts
    if (concept_has_constraint(concept, CONSTRAINT_DISPLAY)) {
        if (!add_display_operation(target_type, checker)) {
            return 0;
        }
    }
    
    // Generate comparison operations for comparable concepts
    if (concept_has_constraint(concept, CONSTRAINT_PARTIAL_EQ)) {
        if (!add_comparison_operations(target_type, checker)) {
            return 0;
        }
    }
    
    return 1;
}

// Helper to check if concept has a specific constraint
int concept_has_constraint(ConceptDefinition* concept, ConstraintKind kind) {
    if (!concept || !concept->requirements) return 0;
    
    InterfaceConstraint* constraint = concept->requirements->constraints;
    while (constraint) {
        if (constraint->kind == kind) {
            return 1;
        }
        constraint = constraint->next;
    }
    
    return 0;
}

// Add zero() operation for numeric types
int add_zero_operation(Type* type, TypeChecker* checker) {
    if (!type || !checker) return 0;
    
    // Create zero() method signature: () -> Self
    Type* zero_return = type_copy(type);
    Type* zero_signature = type_function(NULL, 0, zero_return);
    
    // Add to type's method table
    return type_add_method(type, "zero", zero_signature, checker);
}

// Add copy() operation for copyable types
int add_copy_operation(Type* type, TypeChecker* checker) {
    if (!type || !checker) return 0;
    
    // Create copy() method signature: (self: &Self) -> Self
    Type* self_ref = type_reference_simple(type_copy(type));
    Type* copy_params[] = {self_ref};
    Type* copy_return = type_copy(type);
    Type* copy_signature = type_function(copy_params, 1, copy_return);
    
    return type_add_method(type, "copy", copy_signature, checker);
}

// Add display() operation for displayable types  
int add_display_operation(Type* type, TypeChecker* checker) {
    if (!type || !checker) return 0;
    
    // Create display() method signature: (self: &Self) -> string
    Type* self_ref = type_reference_simple(type_copy(type));
    Type* display_params[] = {self_ref};
    Type* string_return = type_string_type();
    Type* display_signature = type_function(display_params, 1, string_return);
    
    return type_add_method(type, "display", display_signature, checker);
}

// Add comparison operations for comparable types
int add_comparison_operations(Type* type, TypeChecker* checker) {
    if (!type || !checker) return 0;
    
    // Create eq() method signature: (self: &Self, other: &Self) -> bool
    Type* self_ref1 = type_reference_simple(type_copy(type));
    Type* self_ref2 = type_reference_simple(type_copy(type));
    Type* eq_params[] = {self_ref1, self_ref2};
    Type* bool_return = type_bool();
    Type* eq_signature = type_function(eq_params, 2, bool_return);
    
    if (!type_add_method(type, "eq", eq_signature, checker)) {
        return 0;
    }
    
    // Create ne() method signature (same as eq)
    Type* ne_signature = type_copy(eq_signature);
    if (!type_add_method(type, "ne", ne_signature, checker)) {
        return 0;
    }
    
    return 1;
}

// =============================================================================
// Concept Constraints in Generic Functions (Task 22.7.5)
// =============================================================================

// Create a function type with concept constraints
Type* create_concept_constrained_function_enhanced(const char* func_name,
                                                  ConceptDefinition** concept_constraints,
                                                  size_t constraint_count,
                                                  Type** param_types, 
                                                  size_t param_count,
                                                  Type* return_type,
                                                  Position pos) {
    if (!func_name || !param_types || !return_type) return NULL;
    
    // Create basic function type
    Type* func_type = type_function(param_types, param_count, return_type);
    if (!func_type) return NULL;
    
    // Attach concept constraints to function metadata
    if (concept_constraints && constraint_count > 0) {
        func_type->data.function.concept_constraints = malloc(sizeof(ConceptDefinition*) * constraint_count);
        if (func_type->data.function.concept_constraints) {
            memcpy(func_type->data.function.concept_constraints, concept_constraints, 
                   sizeof(ConceptDefinition*) * constraint_count);
            func_type->data.function.concept_constraint_count = constraint_count;
        }
    }
    
    func_type->name = strdup(func_name);
    return func_type;
}

// Validate concept constraints during function instantiation
int validate_concept_constraints_on_instantiation(Type* func_type, Type** arg_types, 
                                                 size_t arg_count, TypeChecker* checker) {
    if (!func_type || !arg_types || !checker) return 0;
    
    if (func_type->kind != TYPE_FUNCTION) return 0;
    
    // Check concept constraints if present
    if (func_type->data.function.concept_constraints) {
        for (size_t i = 0; i < arg_count && i < func_type->data.function.param_count; i++) {
            Type* arg_type = arg_types[i];
            
            // Check each concept constraint for this parameter
            for (size_t j = 0; j < func_type->data.function.concept_constraint_count; j++) {
                ConceptDefinition* constraint = func_type->data.function.concept_constraints[j];
                if (!type_satisfies_concept_enhanced(arg_type, constraint, checker)) {
                    return 0; // Constraint not satisfied
                }
            }
        }
    }
    
    return 1;
}

// =============================================================================
// Integration with Constraint Inference System (Task 22.7.6)
// =============================================================================

// Integrate concept constraints with automatic constraint inference
int integrate_concepts_with_constraint_inference(ConceptDefinition* concept, 
                                               ConstraintInferenceEngine* engine,
                                               TypeChecker* checker) {
    if (!concept || !engine || !checker) return 0;
    
    // Simple integration - just mark as successful for now
    // A full implementation would integrate concept requirements with constraint inference
    engine->constraints_inferred++;
    
    return 1;
    
    return 1;
}

// Register concept for automatic inference
int register_concept_for_inference(ConstraintInferenceEngine* engine, ConceptDefinition* concept) {
    if (!engine || !concept) return 0;
    
    // Simple implementation - just mark as successful
    // A full implementation would register the concept for automatic inference  
    return 1;
}

// Infer concept constraints from type usage patterns
ConceptDefinition** infer_concept_constraints_from_usage(Type* type, ASTNode* usage_context,
                                                        TypeChecker* checker, size_t* concept_count) {
    if (!type || !usage_context || !checker || !concept_count) return NULL;
    
    *concept_count = 0;
    
    // Analyze how the type is used in the context
    size_t capacity = 4;
    ConceptDefinition** inferred_concepts = malloc(sizeof(ConceptDefinition*) * capacity);
    if (!inferred_concepts) return NULL;
    
    Position pos = usage_context->pos;
    
    // Check for arithmetic operations usage
    if (ast_uses_arithmetic_operations(usage_context, type)) {
        if (*concept_count >= capacity) {
            capacity *= 2;
            ConceptDefinition** new_concepts = realloc(inferred_concepts, sizeof(ConceptDefinition*) * capacity);
            if (!new_concepts) {
                free(inferred_concepts);
                return NULL;
            }
            inferred_concepts = new_concepts;
        }
        inferred_concepts[(*concept_count)++] = create_numeric_concept(pos);
    }
    
    // Check for comparison operations usage
    if (ast_uses_comparison_operations(usage_context, type)) {
        if (*concept_count >= capacity) {
            capacity *= 2;
            ConceptDefinition** new_concepts = realloc(inferred_concepts, sizeof(ConceptDefinition*) * capacity);
            if (!new_concepts) {
                for (size_t i = 0; i < *concept_count; i++) {
                    concept_definition_free(inferred_concepts[i]);
                }
                free(inferred_concepts);
                return NULL;
            }
            inferred_concepts = new_concepts;
        }
        inferred_concepts[(*concept_count)++] = create_comparable_concept(pos);
    }
    
    // Check for copy operations usage
    if (ast_uses_copy_operations(usage_context, type)) {
        if (*concept_count >= capacity) {
            capacity *= 2;
            ConceptDefinition** new_concepts = realloc(inferred_concepts, sizeof(ConceptDefinition*) * capacity);
            if (!new_concepts) {
                for (size_t i = 0; i < *concept_count; i++) {
                    concept_definition_free(inferred_concepts[i]);
                }
                free(inferred_concepts);
                return NULL;
            }
            inferred_concepts = new_concepts;
        }
        inferred_concepts[(*concept_count)++] = create_copyable_concept(pos);
    }
    
    return inferred_concepts;
}

// Helper functions for AST usage analysis
int ast_uses_arithmetic_operations(ASTNode* node, Type* target_type) {
    if (!node || !target_type) return 0;
    
    // Simplified check - in practice would need full AST traversal
    return (node->type == AST_BINARY_EXPR && 
            (((BinaryExprNode*)node)->operator == TOKEN_PLUS ||
             ((BinaryExprNode*)node)->operator == TOKEN_MINUS ||
             ((BinaryExprNode*)node)->operator == TOKEN_MULTIPLY ||
             ((BinaryExprNode*)node)->operator == TOKEN_DIVIDE));
}

int ast_uses_comparison_operations(ASTNode* node, Type* target_type) {
    if (!node || !target_type) return 0;
    
    return (node->type == AST_BINARY_EXPR && 
            (((BinaryExprNode*)node)->operator == TOKEN_EQ ||
             ((BinaryExprNode*)node)->operator == TOKEN_NE ||
             ((BinaryExprNode*)node)->operator == TOKEN_LT ||
             ((BinaryExprNode*)node)->operator == TOKEN_GT ||
             ((BinaryExprNode*)node)->operator == TOKEN_LE ||
             ((BinaryExprNode*)node)->operator == TOKEN_GE));
}

int ast_uses_copy_operations(ASTNode* node, Type* target_type) {
    if (!node || !target_type) return 0;
    
    // Check for variable declarations that would require copying
    return (node->type == AST_VAR_DECL);
}

// Helper functions for type system integration

// Helper function to create function signature from AST
Type* type_from_func_signature(FuncDeclNode* sig, TypeChecker* checker) {
    if (!sig || !checker) return NULL;
    
    // Convert parameter types
    Type** param_types = NULL;
    size_t param_count = 0;
    
    if (sig->params) {
        // Count parameters
        ASTNode* param = sig->params;
        while (param) {
            param_count++;
            param = param->next;
        }
        
        // Convert parameter types
        param_types = malloc(sizeof(Type*) * param_count);
        if (param_types) {
            param = sig->params;
            for (size_t i = 0; i < param_count; i++) {
                param_types[i] = type_from_ast_node(param, checker);
                param = param->next;
            }
        }
    }
    
    // Convert return type
    Type* return_type = type_from_ast_node(sig->return_type, checker);
    if (!return_type) {
        return_type = type_void();
    }
    
    return type_function(param_types, param_count, return_type);
}

// Helper function to convert AST node to type
Type* type_from_ast_node(ASTNode* node, TypeChecker* checker) {
    if (!node || !checker) return NULL;
    
    switch (node->type) {
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)node;
            return type_lookup_by_name(ident->name, checker);
        }
        case AST_TYPE_DECL: {
            TypeDeclNode* type_decl = (TypeDeclNode*)node;
            return type_from_ast_node(type_decl->type, checker);
        }
        default:
            return type_unknown();
    }
}

// Helper function to add method to type
int type_add_method(Type* type, const char* method_name, Type* method_signature, TypeChecker* checker) {
    if (!type || !method_name || !method_signature || !checker) return 0;
    
    // For now, this is a simplified implementation
    // In practice, would need to handle method tables properly
    return 1;
}



// Helper function to create reference type
Type* type_reference_simple(Type* base_type) {
    if (!base_type) return NULL;
    
    Type* ref_type = type_new(TYPE_REFERENCE);
    if (ref_type) {
        ref_type->data.reference.referenced_type = base_type;
        ref_type->data.reference.is_mutable = 0;
    }
    return ref_type;
}

// Helper function to lookup type by name
Type* type_lookup_by_name(const char* name, TypeChecker* checker) {
    if (!name || !checker) return NULL;
    
    // Basic type mapping
    if (strcmp(name, "int") == 0) return type_int(32, 1);
    if (strcmp(name, "float") == 0) return type_float(64);
    if (strcmp(name, "bool") == 0) return type_bool();
    if (strcmp(name, "string") == 0) return type_string_type();
    if (strcmp(name, "void") == 0) return type_void();
    
    // Default to unknown type
    return type_unknown();
}

// Helper function to create unknown type
Type* type_unknown(void) {
    return type_new(TYPE_UNKNOWN);
}


