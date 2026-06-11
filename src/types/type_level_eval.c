#include "interface_system.h"
#include "type_level_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Task 22.4 type-level programming — evaluation context & utilities:
// computation printing/equivalence/substitution, TypeEvalContext,
// builtin family registration, full evaluation. Split from
// type_level_programming.c (refactor, no behavior change).

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
// Utility Functions
// =============================================================================

const char* type_level_computation_kind_to_string(TypeLevelComputationKind kind) {
    switch (kind) {
        case TYPE_LEVEL_CONST: return "Const";
        case TYPE_LEVEL_FUNCTION: return "Function";
        case TYPE_LEVEL_DEPENDENT: return "Dependent";
        case TYPE_LEVEL_FAMILY: return "Family";
        case TYPE_LEVEL_ASSOCIATED: return "Associated";
        default: return "Unknown";
    }
}



void print_type_level_computation(const TypeLevelComputation* computation) {
    if (!computation) {
        printf("TypeLevelComputation: null\n");
        return;
    }
    
    printf("TypeLevelComputation:\n");
    printf("  Name: %s\n", computation->name ? computation->name : "<unnamed>");
    printf("  Kind: %s\n", type_level_computation_kind_to_string(computation->kind));
    printf("  Const evaluable: %s\n", computation->is_const_evaluable ? "yes" : "no");
    
    if (computation->parameters) {
        printf("  Parameters:\n");
        TypeVariable* param = computation->parameters;
        while (param) {
            printf("    - %s (%s)\n", 
                   param->name ? param->name : "<unnamed>",
                   type_variable_kind_to_string(param->kind));
            param = param->next;
        }
    }
    
    if (computation->result_type) {
        printf("  Result type: %s\n", 
               computation->result_type->name ? computation->result_type->name : "<unnamed>");
    }
}

// Check if two type-level computations are equivalent
int type_level_computations_equivalent(const TypeLevelComputation* comp1, const TypeLevelComputation* comp2) {
    if (!comp1 || !comp2) return comp1 == comp2;
    
    if (comp1->kind != comp2->kind) return 0;
    
    if (comp1->name && comp2->name) {
        if (strcmp(comp1->name, comp2->name) != 0) return 0;
    } else if (comp1->name != comp2->name) {
        return 0;
    }
    
    // For a full implementation, this would compare the computation bodies
    // and parameter lists for structural equivalence
    
    return 1;
}

// Substitute type variables in a type-level computation
TypeLevelComputation* substitute_in_type_level_computation(TypeLevelComputation* computation,
                                                         TypeVariable* from_var, Type* to_type) {
    if (!computation || !from_var || !to_type) return NULL;
    
    // Create a copy of the computation with substitutions applied
    TypeLevelComputation* substituted = type_level_computation_new(computation->kind, computation->name);
    if (!substituted) return NULL;
    
    substituted->is_const_evaluable = computation->is_const_evaluable;
    
    // Copy parameters, applying substitution
    TypeVariable* param = computation->parameters;
    TypeVariable* prev_param = NULL;
    
    while (param) {
        if (param->name && from_var->name && strcmp(param->name, from_var->name) == 0) {
            // This parameter matches the substitution target
            // Replace it with a bound type variable
            TypeVariable* new_param = type_variable_new(param->name, param->kind, param->declared_pos);
            if (new_param) {
                new_param->bound_type = type_copy(to_type);
                new_param->is_inferred = 1;
                
                if (prev_param) {
                    prev_param->next = new_param;
                } else {
                    substituted->parameters = new_param;
                }
                prev_param = new_param;
            }
        } else {
            // Copy the parameter unchanged
            TypeVariable* new_param = type_variable_copy(param);
            if (new_param) {
                if (prev_param) {
                    prev_param->next = new_param;
                } else {
                    substituted->parameters = new_param;
                }
                prev_param = new_param;
            }
        }
        
        param = param->next;
    }
    
    // Copy body and result type (substitution in AST would be more complex)
    substituted->body = computation->body; // Shallow copy for now
    substituted->result_type = computation->result_type ? type_copy(computation->result_type) : NULL;
    
    return substituted;
}

// =============================================================================
// Compile-Time Evaluation Engine for Type Expressions
// =============================================================================

// Evaluation context for type expressions
typedef struct TypeEvalContext {
    TypeChecker* type_checker;         // Associated type checker
    PatternEnv* variable_bindings;     // Variable bindings from pattern matching
    TypeFamily** type_families;       // Available type families
    size_t family_count;              // Number of type families
    TypeLevelNat** nat_constants;     // Cache of natural number constants
    size_t nat_cache_size;            // Size of natural number cache
    int evaluation_depth;             // Current evaluation depth
    int max_evaluation_depth;         // Maximum allowed depth
} TypeEvalContext;

TypeEvalContext* type_eval_context_new(TypeChecker* checker) {
    TypeEvalContext* ctx = malloc(sizeof(TypeEvalContext));
    if (!ctx) return NULL;
    
    ctx->type_checker = checker;
    ctx->variable_bindings = pattern_env_new();
    ctx->type_families = NULL;
    ctx->family_count = 0;
    ctx->nat_constants = NULL;
    ctx->nat_cache_size = 0;
    ctx->evaluation_depth = 0;
    ctx->max_evaluation_depth = 100; // Prevent infinite recursion
    
    return ctx;
}

void type_eval_context_free(TypeEvalContext* ctx) {
    if (!ctx) return;
    
    pattern_env_free(ctx->variable_bindings);
    
    if (ctx->type_families) {
        for (size_t i = 0; i < ctx->family_count; i++) {
            type_family_free(ctx->type_families[i]);
        }
        free(ctx->type_families);
    }
    
    if (ctx->nat_constants) {
        for (size_t i = 0; i < ctx->nat_cache_size; i++) {
            type_level_nat_free(ctx->nat_constants[i]);
        }
        free(ctx->nat_constants);
    }
    
    free(ctx);
}

int type_eval_context_add_family(TypeEvalContext* ctx, TypeFamily* family) {
    if (!ctx || !family) return 0;
    
    TypeFamily** new_families = realloc(ctx->type_families, 
                                       sizeof(TypeFamily*) * (ctx->family_count + 1));
    if (!new_families) return 0;
    
    ctx->type_families = new_families;
    ctx->type_families[ctx->family_count] = family;
    ctx->family_count++;
    
    return 1;
}

// Get or create a natural number constant
TypeLevelNat* type_eval_context_get_nat(TypeEvalContext* ctx, size_t value) {
    if (!ctx) return NULL;
    
    // Check if we already have this constant cached
    for (size_t i = 0; i < ctx->nat_cache_size; i++) {
        if (ctx->nat_constants[i]->value == value) {
            return ctx->nat_constants[i];
        }
    }
    
    // Create new constant
    TypeLevelNat* nat;
    if (value == 0) {
        nat = type_level_nat_zero();
    } else {
        // Build up Succ chain
        nat = type_level_nat_zero();
        for (size_t i = 0; i < value; i++) {
            TypeLevelNat* old_nat = nat;
            nat = type_level_nat_succ(old_nat);
            if (!nat) {
                type_level_nat_free(old_nat);
                return NULL;
            }
        }
    }
    
    if (!nat) return NULL;
    
    // Add to cache
    TypeLevelNat** new_constants = realloc(ctx->nat_constants, 
                                          sizeof(TypeLevelNat*) * (ctx->nat_cache_size + 1));
    if (new_constants) {
        ctx->nat_constants = new_constants;
        ctx->nat_constants[ctx->nat_cache_size] = nat;
        ctx->nat_cache_size++;
    }
    
    return nat;
}

// Evaluate a type expression at compile time
Type* evaluate_type_expression(TypeEvalContext* ctx, ASTNode* expr) {
    if (!ctx || !expr) return NULL;
    
    // Prevent infinite recursion
    if (ctx->evaluation_depth >= ctx->max_evaluation_depth) {
        return NULL;
    }
    
    ctx->evaluation_depth++;
    Type* result = NULL;
    
    switch (expr->type) {
        case AST_LITERAL: {
            LiteralNode* literal = (LiteralNode*)expr;
            
            if (literal->literal_type == TOKEN_INT) {
                // Create a type-level natural number
                size_t value = (size_t)atoi(literal->value);
                TypeLevelNat* nat = type_eval_context_get_nat(ctx, value);
                
                if (nat) {
                    Type* nat_type = type_new(TYPE_UNKNOWN);
                    if (nat_type) {
                        char* name = malloc(64);
                        if (name) {
                            if (value == 0) {
                                strcpy(name, "Zero");
                            } else {
                                snprintf(name, 64, "Nat<%zu>", value);
                            }
                            nat_type->name = name;
                        }
                        result = nat_type;
                    }
                }
            }
            break;
        }
        
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)expr;
            
            // Look up in variable bindings first
            if (ctx->variable_bindings) {
                for (size_t i = 0; i < ctx->variable_bindings->binding_count; i++) {
                    if (strcmp(ctx->variable_bindings->var_names[i], ident->name) == 0) {
                        result = type_copy(ctx->variable_bindings->var_types[i]);
                        break;
                    }
                }
            }
            
            // If not found, check if it's a type constructor
            if (!result) {
                if (strcmp(ident->name, "Zero") == 0) {
                    TypeLevelNat* zero = type_level_nat_zero();
                    if (zero) {
                        Type* zero_type = type_new(TYPE_UNKNOWN);
                        if (zero_type) {
                            zero_type->name = str_dup("Zero");
                            result = zero_type;
                        }
                    }
                }
            }
            break;
        }
        
        // Skip function calls for now - they need proper AST integration
        // case AST_FUNCTION_CALL: 
        //     break;
        
        // Skip binary expressions for now - they need proper AST integration  
        // case AST_BINARY_EXPR: {
        //     break;
        
        default:
            // For unsupported expressions, try regular type checking
            result = type_check_expression(ctx->type_checker, expr);
            break;
    }
    
    ctx->evaluation_depth--;
    return result;
}

// Initialize built-in type families
int type_eval_context_init_builtins(TypeEvalContext* ctx) {
    if (!ctx) return 0;
    
    // Add built-in type families
    TypeFamily* add_family = create_add_type_family();
    TypeFamily* mul_family = create_mul_type_family();
    TypeFamily* equal_family = create_equal_type_family();
    
    int success = 1;
    
    if (add_family) {
        success &= type_eval_context_add_family(ctx, add_family);
    }
    
    if (mul_family) {
        success &= type_eval_context_add_family(ctx, mul_family);
    }
    
    if (equal_family) {
        success &= type_eval_context_add_family(ctx, equal_family);
    }
    
    return success;
}

// Evaluate type-level computation with full context
Type* evaluate_type_level_computation_full(TypeLevelComputation* computation, TypeEvalContext* ctx) {
    if (!computation || !ctx) return NULL;
    
    switch (computation->kind) {
        case TYPE_LEVEL_CONST:
            // Evaluate constant computation
            if (computation->body) {
                return evaluate_type_expression(ctx, computation->body);
            }
            if (computation->result_type) {
                return type_copy(computation->result_type);
            }
            return NULL;
            
        case TYPE_LEVEL_FUNCTION:
            // Evaluate function computation
            if (computation->body) {
                return evaluate_type_expression(ctx, computation->body);
            }
            return NULL;
            
        case TYPE_LEVEL_FAMILY:
            // Type families are evaluated through the family evaluation system
            return NULL;
            
        case TYPE_LEVEL_DEPENDENT:
            // Dependent types require special handling
            if (computation->body) {
                return evaluate_type_expression(ctx, computation->body);
            }
            return NULL;
            
        case TYPE_LEVEL_ASSOCIATED:
            // Associated types require trait/protocol resolution
            return NULL;
            
        default:
            return NULL;
    }
}

// Check if a type expression can be evaluated at compile time
int is_compile_time_evaluable(TypeEvalContext* ctx, ASTNode* expr) {
    if (!ctx || !expr) return 0;
    
    switch (expr->type) {
        case AST_LITERAL:
            return 1; // Literals are always compile-time
            
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)expr;
            
            // Check if it's a compile-time constant
            if (strcmp(ident->name, "Zero") == 0) return 1;
            
            // Check variable bindings
            if (ctx->variable_bindings) {
                for (size_t i = 0; i < ctx->variable_bindings->binding_count; i++) {
                    if (strcmp(ctx->variable_bindings->var_names[i], ident->name) == 0) {
                        return 1; // Bound variables are compile-time
                    }
                }
            }
            
            return 0;
        }
        
        // Skip AST function calls and binary expressions for now
        // case AST_FUNCTION_CALL:
        //     return 0;
        // case AST_BINARY_EXPR:
        //     return 0;
        
        default:
            return 0;
    }
}
