#include "interface_system.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

// =============================================================================
// 22.1: Automatic Constraint Inference System Implementation
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
// Constraint Inference Engine
// =============================================================================

ConstraintInferenceEngine* constraint_inference_engine_new(TypeChecker* type_checker) {
    ConstraintInferenceEngine* engine = malloc(sizeof(ConstraintInferenceEngine));
    if (!engine) return NULL;
    
    engine->type_checker = type_checker;
    engine->type_variables = NULL;
    engine->active_constraints = constraint_set_new();
    engine->inference_depth = 0;
    engine->max_inference_depth = 50; // Prevent infinite recursion
    
    // Initialize statistics
    engine->constraints_inferred = 0;
    engine->type_variables_inferred = 0;
    engine->substitutions_made = 0;
    
    // Initialize cache
    engine->cache_capacity = 64;
    engine->cache_size = 0;
    engine->inferred_types_cache = malloc(sizeof(Type*) * engine->cache_capacity);
    if (engine->inferred_types_cache) {
        memset(engine->inferred_types_cache, 0, sizeof(Type*) * engine->cache_capacity);
    }
    
    return engine;
}

void constraint_inference_engine_free(ConstraintInferenceEngine* engine) {
    if (!engine) return;
    
    // Free type variables
    TypeVariable* var = engine->type_variables;
    while (var) {
        TypeVariable* next = var->next;
        type_variable_free(var);
        var = next;
    }
    
    // Free constraints
    constraint_set_free(engine->active_constraints);
    
    // Free cache
    if (engine->inferred_types_cache) {
        for (size_t i = 0; i < engine->cache_size; i++) {
            if (engine->inferred_types_cache[i]) {
                type_free(engine->inferred_types_cache[i]);
            }
        }
        free(engine->inferred_types_cache);
    }
    
    free(engine);
}

// =============================================================================
// Type Variable Management
// =============================================================================

TypeVariable* type_variable_new(const char* name, TypeVariableKind kind, Position pos) {
    TypeVariable* var = malloc(sizeof(TypeVariable));
    if (!var) return NULL;
    
    var->name = str_dup(name);
    var->kind = kind;
    var->bound_type = NULL;
    var->constraints = constraint_set_new();
    var->is_inferred = 0;
    var->declared_pos = pos;
    var->next = NULL;
    
    return var;
}

void type_variable_free(TypeVariable* var) {
    if (!var) return;
    
    free(var->name);
    if (var->bound_type) {
        type_free(var->bound_type);
    }
    constraint_set_free(var->constraints);
    free(var);
}

TypeVariable* type_variable_copy(const TypeVariable* var) {
    if (!var) return NULL;
    
    TypeVariable* copy = type_variable_new(var->name, var->kind, var->declared_pos);
    if (!copy) return NULL;
    
    copy->bound_type = var->bound_type ? type_copy(var->bound_type) : NULL;
    copy->is_inferred = var->is_inferred;
    
    // Copy constraints
    if (var->constraints && var->constraints->constraints) {
        InterfaceConstraint* constraint = var->constraints->constraints;
        while (constraint) {
            InterfaceConstraint* copy_constraint = interface_constraint_copy(constraint);
            if (copy_constraint) {
                constraint_set_add(copy->constraints, copy_constraint);
            }
            constraint = constraint->next;
        }
    }
    
    return copy;
}

int type_variable_add_constraint(TypeVariable* var, InterfaceConstraint* constraint) {
    if (!var || !constraint) return 0;
    return constraint_set_add(var->constraints, constraint);
}

TypeVariable* constraint_inference_engine_add_type_variable(ConstraintInferenceEngine* engine, 
                                                           const char* name, TypeVariableKind kind, Position pos) {
    if (!engine || !name) return NULL;
    
    TypeVariable* var = type_variable_new(name, kind, pos);
    if (!var) return NULL;
    
    // Add to engine's type variable list
    var->next = engine->type_variables;
    engine->type_variables = var;
    engine->type_variables_inferred++;
    
    return var;
}

// =============================================================================
// Constraint Management
// =============================================================================

InterfaceConstraint* interface_constraint_new(ConstraintKind kind, Type* constrained_type, Position pos) {
    InterfaceConstraint* constraint = malloc(sizeof(InterfaceConstraint));
    if (!constraint) return NULL;
    
    constraint->kind = kind;
    constraint->constrained_type = constrained_type;
    constraint->target_type = NULL;
    constraint->protocol_name = NULL;
    constraint->associated_type_name = NULL;
    constraint->constraint_data = NULL;
    constraint->is_auto_inferred = 0;
    constraint->source_pos = pos;
    constraint->next = NULL;
    
    return constraint;
}

void interface_constraint_free(InterfaceConstraint* constraint) {
    if (!constraint) return;
    
    if (constraint->target_type) {
        type_free(constraint->target_type);
    }
    free(constraint->protocol_name);
    free(constraint->associated_type_name);
    free(constraint->constraint_data);
    free(constraint);
}

InterfaceConstraint* interface_constraint_copy(const InterfaceConstraint* constraint) {
    if (!constraint) return NULL;
    
    InterfaceConstraint* copy = malloc(sizeof(InterfaceConstraint));
    if (!copy) return NULL;
    
    *copy = *constraint; // Copy all fields
    
    // Deep copy string fields
    if (constraint->protocol_name) {
        copy->protocol_name = str_dup(constraint->protocol_name);
    }
    if (constraint->associated_type_name) {
        copy->associated_type_name = str_dup(constraint->associated_type_name);
    }
    if (constraint->constraint_data) {
        copy->constraint_data = str_dup((const char*)constraint->constraint_data);
    }
    
    copy->next = NULL; // Don't copy the next pointer
    
    return copy;
}

ConstraintSet* constraint_set_new(void) {
    ConstraintSet* set = malloc(sizeof(ConstraintSet));
    if (!set) return NULL;
    
    set->constraints = NULL;
    set->count = 0;
    set->is_satisfied = 0;
    set->error_message = NULL;
    
    return set;
}

void constraint_set_free(ConstraintSet* set) {
    if (!set) return;
    
    InterfaceConstraint* constraint = set->constraints;
    while (constraint) {
        InterfaceConstraint* next = constraint->next;
        interface_constraint_free(constraint);
        constraint = next;
    }
    
    free(set->error_message);
    free(set);
}

int constraint_set_add(ConstraintSet* set, InterfaceConstraint* constraint) {
    if (!set || !constraint) return 0;
    
    constraint->next = set->constraints;
    set->constraints = constraint;
    set->count++;
    
    return 1;
}

int constraint_set_merge(ConstraintSet* dest, const ConstraintSet* src) {
    if (!dest || !src) return 0;
    
    InterfaceConstraint* constraint = src->constraints;
    while (constraint) {
        InterfaceConstraint* copy = interface_constraint_copy(constraint);
        if (copy) {
            constraint_set_add(dest, copy);
        }
        constraint = constraint->next;
    }
    
    return 1;
}

int constraint_set_is_satisfied(const ConstraintSet* set, TypeChecker* checker) {
    if (!set || !checker) return 0;
    
    // Check each constraint to see if it's satisfied
    InterfaceConstraint* constraint = set->constraints;
    while (constraint) {
        // For now, we'll implement basic satisfaction checking
        // This would be expanded with full constraint solving logic
        
        switch (constraint->kind) {
            case CONSTRAINT_IMPLEMENTS:
                // Check if constrained_type implements the specified protocol/interface
                // TODO: Implement full interface conformance checking
                break;
                
            case CONSTRAINT_EQUALITY:
                // Check if constrained_type equals target_type
                if (constraint->constrained_type && constraint->target_type) {
                    if (!type_equals(constraint->constrained_type, constraint->target_type)) {
                        return 0;
                    }
                }
                break;
                
            case CONSTRAINT_SUBTYPE:
                // Check if constrained_type is a subtype of target_type
                if (constraint->constrained_type && constraint->target_type) {
                    if (!type_compatible(constraint->constrained_type, constraint->target_type)) {
                        return 0;
                    }
                }
                break;
                
            case CONSTRAINT_NUMERIC:
                // Check if constrained_type is numeric
                if (constraint->constrained_type) {
                    if (!type_is_numeric(constraint->constrained_type)) {
                        return 0;
                    }
                }
                break;
                
            case CONSTRAINT_COPY:
                // Check if constrained_type can be copied (not pointer-like usually)
                if (constraint->constrained_type) {
                    if (type_is_pointer_like(constraint->constrained_type)) {
                        return 0; // Pointers usually can't be simply copied
                    }
                }
                break;
                
            default:
                // For other constraints, assume satisfied for now
                // TODO: Implement full constraint checking
                break;
        }
        
        constraint = constraint->next;
    }
    
    return 1; // All constraints satisfied
}

// =============================================================================
// Automatic Constraint Inference
// =============================================================================

int infer_constraints_from_expression(ConstraintInferenceEngine* engine, ASTNode* expr) {
    if (!engine || !expr) return 0;
    
    // Prevent infinite recursion
    if (engine->inference_depth >= engine->max_inference_depth) {
        return 0;
    }
    
    engine->inference_depth++;
    
    int result = 1;
    
    switch (expr->type) {
        case AST_BINARY_EXPR:
            result = infer_constraints_from_binary_operation(engine, expr);
            break;
            
        case AST_CALL_EXPR:
            result = infer_constraints_from_function_call(engine, expr);
            break;
            
        case AST_INDEX_EXPR:
            // For array[index] operations, we can infer indexing constraints
            // TODO: Implement infer_constraints_from_index_operation
            break;
            
        case AST_SELECTOR_EXPR:
            // For obj.field operations, we can infer field access constraints
            // TODO: Implement infer_constraints_from_field_access
            break;
            
        case AST_FOR_STMT:
            // For 'for item in collection' loops, we can infer iterator constraints
            // TODO: Implement infer_constraints_from_for_loop
            break;
            
        case AST_SLICE_EXPR:
            // For array[start:end] operations, we can infer slicing constraints
            // TODO: Implement infer_constraints_from_slice_operation
            break;
            
        case AST_IDENTIFIER: {
            // For identifiers, we might infer constraints based on usage
            IdentifierNode* ident = (IdentifierNode*)expr;
            Variable* var = type_checker_lookup_variable(engine->type_checker, ident->name);
            
            if (var && var->type) {
                // If the type is unknown, we might create a type variable
                if (var->type->kind == TYPE_UNKNOWN) {
                    TypeVariable* type_var = constraint_inference_engine_add_type_variable(
                        engine, ident->name, TYPE_VAR_GENERIC, expr->pos);
                    if (type_var) {
                        type_var->is_inferred = 1;
                        engine->constraints_inferred++;
                    }
                }
            }
            break;
        }
        
        
        case AST_LITERAL: {
            // Literals can help infer numeric constraints
            LiteralNode* lit = (LiteralNode*)expr;
            switch (lit->literal_type) {
                case TOKEN_INT:
                    // If this literal is used in a context requiring a type variable,
                    // we can infer it should be integral
                    break;
                case TOKEN_FLOAT:
                    // Similarly for floating point
                    break;
                default:
                    break;
            }
            break;
        }
        
        default:
            // For other expression types, recursively process child nodes
            // This is a simplified implementation
            break;
    }
    
    engine->inference_depth--;
    return result;
}

int infer_constraints_from_function_call(ConstraintInferenceEngine* engine, ASTNode* call_expr) {
    if (!engine || !call_expr || call_expr->type != AST_CALL_EXPR) return 0;
    
    CallExprNode* call = (CallExprNode*)call_expr;
    
    // Get the function type to understand what constraints might be needed
    Type* func_type = type_check_expression(engine->type_checker, call->function);
    
    if (func_type && func_type->kind == TYPE_FUNCTION) {
        // Check each argument against parameter types
        ASTNode* arg = call->args;
        size_t param_index = 0;
        
        while (arg && param_index < func_type->data.function.param_count) {
            Type* param_type = func_type->data.function.param_types[param_index];
            Type* arg_type = type_check_expression(engine->type_checker, arg);
            
            if (param_type && arg_type) {
                // If types don't match directly, see if we can infer constraints
                if (!type_compatible(arg_type, param_type)) {
                    // Create a constraint that the argument type must be compatible
                    InterfaceConstraint* constraint = interface_constraint_new(
                        CONSTRAINT_SUBTYPE, arg_type, call_expr->pos);
                    if (constraint) {
                        constraint->target_type = type_copy(param_type);
                        constraint->is_auto_inferred = 1;
                        constraint_set_add(engine->active_constraints, constraint);
                        engine->constraints_inferred++;
                    }
                }
            }
            
            // Recursively infer from argument expressions
            infer_constraints_from_expression(engine, arg);
            
            arg = arg->next;
            param_index++;
        }
    }
    
    return 1;
}

int infer_constraints_from_binary_operation(ConstraintInferenceEngine* engine, ASTNode* binary_expr) {
    if (!engine || !binary_expr || binary_expr->type != AST_BINARY_EXPR) return 0;
    
    BinaryExprNode* binary = (BinaryExprNode*)binary_expr;
    
    // Infer constraints based on the operation type
    switch (binary->operator) {
        case TOKEN_PLUS:
        case TOKEN_MINUS:
        case TOKEN_MULTIPLY:
        case TOKEN_DIVIDE:
        case TOKEN_MODULO: {
            // Arithmetic operations require numeric types
            Type* left_type = type_check_expression(engine->type_checker, binary->left);
            Type* right_type = type_check_expression(engine->type_checker, binary->right);
            
            // Create numeric constraints for both operands
            if (left_type) {
                InterfaceConstraint* constraint = interface_constraint_new(
                    CONSTRAINT_NUMERIC, left_type, binary_expr->pos);
                if (constraint) {
                    constraint->is_auto_inferred = 1;
                    constraint_set_add(engine->active_constraints, constraint);
                    engine->constraints_inferred++;
                }
            }
            
            if (right_type) {
                InterfaceConstraint* constraint = interface_constraint_new(
                    CONSTRAINT_NUMERIC, right_type, binary_expr->pos);
                if (constraint) {
                    constraint->is_auto_inferred = 1;
                    constraint_set_add(engine->active_constraints, constraint);
                    engine->constraints_inferred++;
                }
            }
            break;
        }
        
        case TOKEN_EQ:
        case TOKEN_NE: {
            // Equality operations require PartialEq
            Type* left_type = type_check_expression(engine->type_checker, binary->left);
            
            if (left_type) {
                InterfaceConstraint* constraint = interface_constraint_new(
                    CONSTRAINT_PARTIAL_EQ, left_type, binary_expr->pos);
                if (constraint) {
                    constraint->is_auto_inferred = 1;
                    constraint_set_add(engine->active_constraints, constraint);
                    engine->constraints_inferred++;
                }
            }
            break;
        }
        
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_GT:
        case TOKEN_GE: {
            // Comparison operations require PartialOrd
            Type* left_type = type_check_expression(engine->type_checker, binary->left);
            
            if (left_type) {
                InterfaceConstraint* constraint = interface_constraint_new(
                    CONSTRAINT_PARTIAL_ORD, left_type, binary_expr->pos);
                if (constraint) {
                    constraint->is_auto_inferred = 1;
                    constraint_set_add(engine->active_constraints, constraint);
                    engine->constraints_inferred++;
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    // Recursively infer from operand expressions
    infer_constraints_from_expression(engine, binary->left);
    infer_constraints_from_expression(engine, binary->right);
    
    return 1;
}

int infer_constraints_from_assignment(ConstraintInferenceEngine* engine, ASTNode* assignment) {
    if (!engine || !assignment) return 0;
    
    // For assignments, the RHS must be compatible with LHS type
    // This would be implemented based on the actual assignment AST structure
    // For now, this is a placeholder
    
    return 1;
}

int infer_constraints_from_return_statement(ConstraintInferenceEngine* engine, ASTNode* return_stmt) {
    if (!engine || !return_stmt) return 0;
    
    // Return statements create constraints between the returned expression
    // and the function's return type
    // This would be implemented based on the actual return statement AST structure
    
    return 1;
}

// =============================================================================
// Constraint Solving
// =============================================================================

int solve_constraints(ConstraintInferenceEngine* engine) {
    if (!engine || !engine->active_constraints) return 0;
    
    // Simple constraint solver - this would be much more sophisticated in practice
    // For now, we just check if constraints are satisfiable
    
    int satisfied = constraint_set_is_satisfied(engine->active_constraints, engine->type_checker);
    
    if (satisfied) {
        engine->active_constraints->is_satisfied = 1;
        free(engine->active_constraints->error_message);
        engine->active_constraints->error_message = NULL;
    } else {
        engine->active_constraints->is_satisfied = 0;
        engine->active_constraints->error_message = str_dup("Constraint solving failed: some constraints could not be satisfied");
    }
    
    return satisfied;
}

int unify_types_with_constraints(ConstraintInferenceEngine* engine, Type* type1, Type* type2) {
    if (!engine || !type1 || !type2) return 0;
    
    // Type unification with constraint propagation
    // This is a simplified implementation
    
    if (type_equals(type1, type2)) {
        return 1; // Already unified
    }
    
    // If one type is a type variable, try to bind it
    if (type1->kind == TYPE_UNKNOWN || type2->kind == TYPE_UNKNOWN) {
        // Create an equality constraint
        InterfaceConstraint* constraint = interface_constraint_new(
            CONSTRAINT_EQUALITY, type1, (Position){0, 0, 0, "unification"});
        if (constraint) {
            constraint->target_type = type_copy(type2);
            constraint->is_auto_inferred = 1;
            constraint_set_add(engine->active_constraints, constraint);
            engine->constraints_inferred++;
            engine->substitutions_made++;
        }
        return 1;
    }
    
    // For other types, check if they're compatible
    return type_compatible(type1, type2);
}

Type* substitute_type_variables(ConstraintInferenceEngine* engine, Type* type) {
    if (!engine || !type) return NULL;
    
    // Substitute any type variables with their bound types
    // This would walk through the type structure and replace type variables
    // For now, just return the original type
    
    return type_copy(type);
}

// =============================================================================
// Test Utility Functions
// =============================================================================

const char* constraint_kind_to_string(ConstraintKind kind) {
    switch (kind) {
        case CONSTRAINT_IMPLEMENTS: return "implements";
        case CONSTRAINT_SUBTYPE: return "subtype";
        case CONSTRAINT_EQUALITY: return "equality";
        case CONSTRAINT_SIZE: return "sized";
        case CONSTRAINT_COPY: return "copy";
        case CONSTRAINT_CLONE: return "clone";
        case CONSTRAINT_SEND: return "send";
        case CONSTRAINT_SYNC: return "sync";
        case CONSTRAINT_DEFAULT: return "default";
        case CONSTRAINT_PARTIAL_EQ: return "partial_eq";
        case CONSTRAINT_PARTIAL_ORD: return "partial_ord";
        case CONSTRAINT_HASH: return "hash";
        case CONSTRAINT_DEBUG: return "debug";
        case CONSTRAINT_DISPLAY: return "display";
        case CONSTRAINT_SERIALIZABLE: return "serializable";
        case CONSTRAINT_ITERATOR: return "iterator";
        case CONSTRAINT_INTO: return "into";
        case CONSTRAINT_FROM: return "from";
        case CONSTRAINT_NUMERIC: return "numeric";
        case CONSTRAINT_INTEGRAL: return "integral";
        case CONSTRAINT_FLOATING: return "floating";
        case CONSTRAINT_HIGHER_KINDED: return "higher_kinded";
        case CONSTRAINT_PROTOCOL: return "protocol";
        default: return "unknown";
    }
}

// Convert constraint kind to proper trait name (for trait bounds)
static const char* constraint_kind_to_trait_name(ConstraintKind kind) {
    switch (kind) {
        case CONSTRAINT_IMPLEMENTS: return "Implements";
        case CONSTRAINT_SUBTYPE: return "Subtype";
        case CONSTRAINT_EQUALITY: return "Equality";
        case CONSTRAINT_SIZE: return "Sized";
        case CONSTRAINT_COPY: return "Copy";
        case CONSTRAINT_CLONE: return "Clone";
        case CONSTRAINT_SEND: return "Send";
        case CONSTRAINT_SYNC: return "Sync";
        case CONSTRAINT_DEFAULT: return "Default";
        case CONSTRAINT_PARTIAL_EQ: return "PartialEq";
        case CONSTRAINT_PARTIAL_ORD: return "PartialOrd";
        case CONSTRAINT_HASH: return "Hash";
        case CONSTRAINT_DEBUG: return "Debug";
        case CONSTRAINT_DISPLAY: return "Display";
        case CONSTRAINT_SERIALIZABLE: return "Serializable";
        case CONSTRAINT_ITERATOR: return "Iterator";
        case CONSTRAINT_INTO: return "Into";
        case CONSTRAINT_FROM: return "From";
        case CONSTRAINT_NUMERIC: return "Numeric";
        case CONSTRAINT_INTEGRAL: return "Integral";
        case CONSTRAINT_FLOATING: return "Floating";
        case CONSTRAINT_HIGHER_KINDED: return "HigherKinded";
        case CONSTRAINT_PROTOCOL: return "Protocol";
        default: return "Unknown";
    }
}

TraitBoundSet* generate_trait_bounds_from_constraints(ConstraintInferenceEngine* engine, TypeVariable* var) {
    if (!engine || !var) return NULL;
    
    TraitBoundSet* bounds = malloc(sizeof(TraitBoundSet));
    if (!bounds) return NULL;
    
    bounds->bounds = NULL;
    bounds->count = 0;
    bounds->is_optimized = 0;
    bounds->generated_where_clause = NULL;
    
    // Generate trait bounds from constraints on the type variable
    ConstraintSet* constraints = var->constraints;
    if (constraints) {
        InterfaceConstraint* current = constraints->constraints;
        while (current) {
            TraitBound* bound = malloc(sizeof(TraitBound));
            if (bound) {
                bound->kind = TRAIT_BOUND_SIMPLE;
                bound->type_param_name = str_dup(var->name);
                bound->trait_name = str_dup(constraint_kind_to_trait_name(current->kind));
                bound->type_parameter = NULL;
                bound->associated_type_name = NULL;
                bound->associated_type = NULL;
                bound->is_auto_generated = 1;
                bound->confidence_score = 0.8f;
                bound->source_pos = current->source_pos;
                bound->next = bounds->bounds;
                
                bounds->bounds = bound;
                bounds->count++;
            }
            current = current->next;
        }
    }
    
    return bounds;
}

char* generate_where_clause_from_bounds(TraitBoundSet* bounds) {
    if (!bounds || bounds->count == 0) return NULL;
    
    size_t buffer_size = 1024;
    char* where_clause = malloc(buffer_size);
    if (!where_clause) return NULL;
    
    strcpy(where_clause, "where ");
    
    TraitBound* current = bounds->bounds;
    bool first = true;
    while (current) {
        if (!first) strcat(where_clause, ", ");
        strcat(where_clause, current->type_param_name ? current->type_param_name : "T");
        strcat(where_clause, ": ");
        strcat(where_clause, current->trait_name ? current->trait_name : "Unknown");
        first = false;
        current = current->next;
    }
    
    return where_clause;
}

void optimize_trait_bounds(TraitBoundSet* bounds) {
    if (!bounds) return;
    
    // Simple optimization: remove duplicate bounds
    TraitBound* current = bounds->bounds;
    while (current) {
        TraitBound* next = current->next;
        TraitBound* search = next;
        TraitBound* prev = current;
        
        while (search) {
            if (current->trait_name && search->trait_name &&
                strcmp(current->trait_name, search->trait_name) == 0) {
                // Remove duplicate
                prev->next = search->next;
                free(search->trait_name);
                free(search->type_param_name);
                free(search);
                bounds->count--;
                search = prev->next;
            } else {
                prev = search;
                search = search->next;
            }
        }
        current = next;
    }
    
    // Mark as optimized
    bounds->is_optimized = 1;
}

bool validate_generated_trait_bounds(TraitBoundSet* bounds) {
    if (!bounds) return false;
    
    // Validate that all bounds have valid trait names
    TraitBound* current = bounds->bounds;
    while (current) {
        if (!current->trait_name || strlen(current->trait_name) == 0) {
            return false;
        }
        current = current->next;
    }
    
    return true;
}

// =============================================================================
// Missing Constraint Inference Functions
// =============================================================================

int infer_constraints_from_arithmetic_context(ConstraintInferenceEngine* engine, Type* type, Position pos) {
    if (!engine || !type) return 0;
    
    InterfaceConstraint* constraint = malloc(sizeof(InterfaceConstraint));
    if (!constraint) return 0;
    
    constraint->kind = CONSTRAINT_NUMERIC;
    constraint->constrained_type = type;
    constraint->target_type = NULL;
    constraint->protocol_name = NULL;
    constraint->associated_type_name = NULL;
    constraint->constraint_data = str_dup("arithmetic");
    constraint->is_auto_inferred = 1;
    constraint->is_resolved = 0;
    constraint->source_pos = pos;
    constraint->next = NULL;
    
    // Add to the engine's active constraints
    if (engine->active_constraints) {
        constraint->next = engine->active_constraints->constraints;
        engine->active_constraints->constraints = constraint;
        engine->active_constraints->count++;
    }
    
    // Update inference statistics
    engine->constraints_inferred++;
    
    return 1;
}

int infer_constraints_from_comparison_context(ConstraintInferenceEngine* engine, Type* type, Position pos) {
    if (!engine || !type) return 0;
    
    InterfaceConstraint* constraint = malloc(sizeof(InterfaceConstraint));
    if (!constraint) return 0;
    
    constraint->kind = CONSTRAINT_PARTIAL_ORD;
    constraint->constrained_type = type;
    constraint->target_type = NULL;
    constraint->protocol_name = NULL;
    constraint->associated_type_name = NULL;
    constraint->constraint_data = str_dup("comparison");
    constraint->is_auto_inferred = 1;
    constraint->is_resolved = 0;
    constraint->source_pos = pos;
    constraint->next = NULL;
    
    // Add to the engine's active constraints
    if (engine->active_constraints) {
        constraint->next = engine->active_constraints->constraints;
        engine->active_constraints->constraints = constraint;
        engine->active_constraints->count++;
    }
    
    // Update inference statistics
    engine->constraints_inferred++;
    
    return 1;
}

int infer_constraints_from_usage_pattern(ConstraintInferenceEngine* engine, Type* type, const char* usage_pattern, Position pos) {
    if (!engine || !type) return 0;
    
    // Count how many constraints we add
    int constraints_added = 0;
    
    // Determine constraint kinds based on usage pattern
    ConstraintKind kinds[4] = {0}; // Support multiple constraints from one pattern
    int kind_count = 0;
    
    if (strstr(usage_pattern, "copy")) {
        kinds[kind_count++] = CONSTRAINT_COPY;
    }
    if (strstr(usage_pattern, "clone")) {
        kinds[kind_count++] = CONSTRAINT_CLONE;
    }
    if (strstr(usage_pattern, "debug")) {
        kinds[kind_count++] = CONSTRAINT_DEBUG;
    }
    if (strstr(usage_pattern, "display")) {
        kinds[kind_count++] = CONSTRAINT_DISPLAY;
    }
    if (strstr(usage_pattern, "send")) {
        kinds[kind_count++] = CONSTRAINT_SEND;
    }
    if (strstr(usage_pattern, "sync")) {
        kinds[kind_count++] = CONSTRAINT_SYNC;
    }
    if (strstr(usage_pattern, "comparison")) {
        kinds[kind_count++] = CONSTRAINT_PARTIAL_ORD;
    }
    
    // If no specific pattern found, add a generic implements constraint
    if (kind_count == 0) {
        kinds[0] = CONSTRAINT_IMPLEMENTS;
        kind_count = 1;
    }
    
    // Add constraints for each identified kind
    for (int i = 0; i < kind_count; i++) {
        InterfaceConstraint* constraint = malloc(sizeof(InterfaceConstraint));
        if (!constraint) continue;
        
        constraint->kind = kinds[i];
        constraint->constrained_type = type;
        constraint->target_type = NULL;
        constraint->protocol_name = NULL;
        constraint->associated_type_name = NULL;
        constraint->constraint_data = str_dup(usage_pattern);
        constraint->is_auto_inferred = 1;
        constraint->is_resolved = 0;
        constraint->source_pos = pos;
        constraint->next = NULL;
        
        // Add to the engine's active constraints
        if (engine->active_constraints) {
            constraint->next = engine->active_constraints->constraints;
            engine->active_constraints->constraints = constraint;
            engine->active_constraints->count++;
            constraints_added++;
        }
    }
    
    // Update inference statistics
    engine->constraints_inferred += constraints_added;
    
    return constraints_added;
}

int propagate_constraints(ConstraintInferenceEngine* engine) {
    if (!engine) return 0;
    
    // Simple constraint propagation: 
    // Look for patterns that can infer additional constraints
    int changes = 0;
    
    InterfaceConstraint* current = engine->active_constraints->constraints;
    while (current) {
        // If we have a comparison constraint, also infer partial equality
        if (current->kind == CONSTRAINT_PARTIAL_ORD) {
            InterfaceConstraint* eq_constraint = malloc(sizeof(InterfaceConstraint));
            if (eq_constraint) {
                eq_constraint->kind = CONSTRAINT_PARTIAL_EQ;
                eq_constraint->constrained_type = current->constrained_type;
                eq_constraint->target_type = NULL;
                eq_constraint->protocol_name = NULL;
                eq_constraint->associated_type_name = NULL;
                eq_constraint->constraint_data = str_dup("propagated_from_ord");
                eq_constraint->is_auto_inferred = 1;
                eq_constraint->is_resolved = 0;
                eq_constraint->source_pos = current->source_pos;
                eq_constraint->next = engine->active_constraints->constraints;
                
                engine->active_constraints->constraints = eq_constraint;
                engine->active_constraints->count++;
                engine->constraints_inferred++;
                changes++;
            }
        }
        current = current->next;
    }
    
    return changes;
}

// =============================================================================
// Missing Trait Bound Functions
// =============================================================================

TraitBound* trait_bound_new(const char* type_param_name, Position source_pos) {
    TraitBound* bound = malloc(sizeof(TraitBound));
    if (!bound) return NULL;
    
    bound->kind = TRAIT_BOUND_SIMPLE;
    bound->type_param_name = str_dup(type_param_name);
    bound->trait_name = NULL;
    bound->type_parameter = NULL;
    bound->associated_type_name = NULL;
    bound->associated_type = NULL;
    bound->is_auto_generated = 0;
    bound->confidence_score = 1.0f;
    bound->source_pos = source_pos;
    bound->next = NULL;
    
    return bound;
}

TraitBoundSet* trait_bound_set_new(void) {
    TraitBoundSet* set = malloc(sizeof(TraitBoundSet));
    if (!set) return NULL;
    
    set->bounds = NULL;
    set->count = 0;
    set->is_optimized = 0;
    set->generated_where_clause = NULL;
    
    return set;
}

void trait_bound_set_free(TraitBoundSet* set) {
    if (!set) return;
    
    TraitBound* current = set->bounds;
    while (current) {
        TraitBound* next = current->next;
        free(current->type_param_name);
        free(current->trait_name);
        free(current->associated_type_name);
        free(current);
        current = next;
    }
    
    free(set->generated_where_clause);
    free(set);
}

int trait_bound_set_add(TraitBoundSet* set, TraitBound* bound) {
    if (!set || !bound) return 0;
    
    bound->next = set->bounds;
    set->bounds = bound;
    set->count++;
    
    return 1;
}

const char* type_variable_kind_to_string(TypeVariableKind kind) {
    switch (kind) {
        case TYPE_VAR_GENERIC: return "generic";
        case TYPE_VAR_CONST: return "const";
        case TYPE_VAR_LIFETIME: return "lifetime";
        case TYPE_VAR_HIGHER_KINDED: return "higher_kinded";
        case TYPE_VAR_ASSOCIATED: return "associated";
        default: return "unknown";
    }
}



