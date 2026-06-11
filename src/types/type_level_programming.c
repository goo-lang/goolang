#include "interface_system.h"
#include "type_level_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// =============================================================================
// 22.4: Type-Level Programming Capabilities Implementation
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
// Type-Level Computation Management
// =============================================================================

TypeLevelComputation* type_level_computation_new(TypeLevelComputationKind kind, const char* name) {
    TypeLevelComputation* computation = malloc(sizeof(TypeLevelComputation));
    if (!computation) return NULL;
    
    computation->kind = kind;
    computation->name = str_dup(name);
    computation->parameters = NULL;
    computation->body = NULL;
    computation->result_type = NULL;
    computation->is_const_evaluable = 1; // Default to const-evaluable
    
    return computation;
}

void type_level_computation_free(TypeLevelComputation* computation) {
    if (!computation) return;
    
    free(computation->name);
    
    // Free type parameters
    TypeVariable* param = computation->parameters;
    while (param) {
        TypeVariable* next = param->next;
        type_variable_free(param);
        param = next;
    }
    
    // Note: AST body is not freed here as it's managed by the AST system
    
    if (computation->result_type) {
        type_free(computation->result_type);
    }
    
    free(computation);
}

Type* evaluate_type_level_computation(TypeLevelComputation* computation, TypeChecker* checker) {
    if (!computation || !checker) return NULL;
    
    switch (computation->kind) {
        case TYPE_LEVEL_CONST: {
            // Evaluate a compile-time constant
            if (computation->body && computation->body->type == AST_LITERAL) {
                LiteralNode* literal = (LiteralNode*)computation->body;
                
                switch (literal->literal_type) {
                    case TOKEN_INT: {
                        // Create an integer type based on the literal value
                        // For now, default to int32
                        return type_checker_get_builtin(checker, TYPE_INT32);
                    }
                    case TOKEN_STRING: {
                        return type_checker_get_builtin(checker, TYPE_STRING);
                    }
                    case TOKEN_TRUE:
                    case TOKEN_FALSE: {
                        return type_checker_get_builtin(checker, TYPE_BOOL);
                    }
                    default:
                        return NULL;
                }
            }
            
            // If we have a cached result type, return it
            if (computation->result_type) {
                return type_copy(computation->result_type);
            }
            
            return NULL;
        }
        
        case TYPE_LEVEL_FUNCTION: {
            // Evaluate a type-level function
            // This would involve substituting parameters and evaluating the body
            // For now, return the cached result if available
            if (computation->result_type) {
                return type_copy(computation->result_type);
            }
            
            // Try to evaluate the function body
            if (computation->body) {
                Type* body_type = type_check_expression(checker, computation->body);
                if (body_type) {
                    // Cache the result
                    computation->result_type = type_copy(body_type);
                    return body_type;
                }
            }
            
            return NULL;
        }
        
        case TYPE_LEVEL_DEPENDENT: {
            // Evaluate a dependent type
            // This is more complex as it depends on runtime values
            // For now, return the result type if available
            if (computation->result_type) {
                return type_copy(computation->result_type);
            }
            
            return NULL;
        }
        
        case TYPE_LEVEL_FAMILY: {
            // Evaluate a type family
            // This would involve pattern matching and case analysis
            if (computation->result_type) {
                return type_copy(computation->result_type);
            }
            
            return NULL;
        }
        
        case TYPE_LEVEL_ASSOCIATED: {
            // Evaluate an associated type projection
            // This would involve looking up the associated type in a protocol/trait
            if (computation->result_type) {
                return type_copy(computation->result_type);
            }
            
            return NULL;
        }
        
        default:
            return NULL;
    }
}

int type_level_computation_is_const_evaluable(TypeLevelComputation* computation) {
    if (!computation) return 0;
    
    // Check if the computation can be evaluated at compile time
    switch (computation->kind) {
        case TYPE_LEVEL_CONST:
            return 1; // Constants are always const-evaluable
            
        case TYPE_LEVEL_FUNCTION:
            // Type-level functions are const-evaluable if all their inputs are const
            return computation->is_const_evaluable;
            
        case TYPE_LEVEL_DEPENDENT:
            return 0; // Dependent types generally require runtime information
            
        case TYPE_LEVEL_FAMILY:
        case TYPE_LEVEL_ASSOCIATED:
            // These can be const-evaluable depending on their definition
            return computation->is_const_evaluable;
            
        default:
            return 0;
    }
}

// =============================================================================
// Const Generics Support
// =============================================================================

// Create a const generic parameter
TypeVariable* create_const_generic_parameter(const char* name, Type* const_type, Position pos) {
    TypeVariable* param = type_variable_new(name, TYPE_VAR_CONST, pos);
    if (!param) return NULL;
    
    param->bound_type = const_type ? type_copy(const_type) : NULL;
    
    // Add a constraint that this must be const-evaluable
    InterfaceConstraint* const_constraint = interface_constraint_new(CONSTRAINT_CONST_EVAL, NULL, pos);
    if (const_constraint) {
        type_variable_add_constraint(param, const_constraint);
    }
    
    return param;
}

// Evaluate a const generic expression
Type* evaluate_const_generic(ASTNode* expr, TypeChecker* checker) {
    if (!expr || !checker) return NULL;
    
    switch (expr->type) {
        case AST_LITERAL: {
            LiteralNode* literal = (LiteralNode*)expr;
            
            switch (literal->literal_type) {
                case TOKEN_INT: {
                    // Create a type representing the integer constant
                    Type* const_type = type_new(TYPE_UNKNOWN);
                    if (const_type) {
                        const_type->name = malloc(64);
                        if (const_type->name) {
                            snprintf(const_type->name, 64, "const(%s)", literal->value);
                        }
                    }
                    return const_type;
                }
                
                case TOKEN_STRING: {
                    // Create a type representing the string constant
                    Type* const_type = type_new(TYPE_UNKNOWN);
                    if (const_type) {
                        const_type->name = malloc(strlen(literal->value) + 20);
                        if (const_type->name) {
                            sprintf(const_type->name, "const(\"%s\")", literal->value);
                        }
                    }
                    return const_type;
                }
                
                default:
                    return NULL;
            }
        }
        
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expr;
            
            // Evaluate both operands as const generics
            Type* left_type = evaluate_const_generic(binary->left, checker);
            Type* right_type = evaluate_const_generic(binary->right, checker);
            
            if (left_type && right_type) {
                // Create a type representing the computed result
                Type* result_type = type_new(TYPE_UNKNOWN);
                if (result_type) {
                    // For now, create a simple representation
                    result_type->name = str_dup("const(computed)");
                }
                
                type_free(left_type);
                type_free(right_type);
                return result_type;
            }
            
            if (left_type) type_free(left_type);
            if (right_type) type_free(right_type);
            return NULL;
        }
        
        case AST_IDENTIFIER: {
            // Look up the const generic parameter
            IdentifierNode* ident = (IdentifierNode*)expr;
            Variable* var = type_checker_lookup_variable(checker, ident->name);
            
            if (var && var->type) {
                return type_copy(var->type);
            }
            
            return NULL;
        }
        
        default:
            return NULL;
    }
}

// =============================================================================
// Type Families and Pattern Matching
// =============================================================================

// Type-level natural number representation lives in
// type_level_internal.h (shared with the dependent/eval units).

// Pattern kinds for type family matching are now defined in interface_system.h

// Pattern for type family case matching
// struct body moved to type_level_internal.h (shared with eval unit)

// Define a type family case
// struct body moved to type_level_internal.h (shared with eval unit)

// Define a type family
// struct body moved to type_level_internal.h (shared with eval unit)

TypeFamily* type_family_new(const char* name) {
    TypeFamily* family = malloc(sizeof(TypeFamily));
    if (!family) return NULL;
    
    family->name = str_dup(name);
    family->parameters = NULL;
    family->cases = NULL;
    family->default_case = NULL;
    
    return family;
}

void type_family_free(TypeFamily* family) {
    if (!family) return;
    
    free(family->name);
    
    // Free parameters
    TypeVariable* param = family->parameters;
    while (param) {
        TypeVariable* next = param->next;
        type_variable_free(param);
        param = next;
    }
    
    // Free cases
    TypeFamilyCase* case_item = family->cases;
    while (case_item) {
        TypeFamilyCase* next = case_item->next;
        type_level_computation_free(case_item->result);
        free(case_item);
        case_item = next;
    }
    
    type_level_computation_free(family->default_case);
    free(family);
}

// =============================================================================
// Type-Level Natural Numbers
// =============================================================================

TypeLevelNat* type_level_nat_zero(void) {
    TypeLevelNat* nat = malloc(sizeof(TypeLevelNat));
    if (!nat) return NULL;
    
    nat->kind = NAT_ZERO;
    nat->predecessor = NULL;
    nat->value = 0;
    
    return nat;
}

TypeLevelNat* type_level_nat_succ(TypeLevelNat* n) {
    if (!n) return NULL;
    
    TypeLevelNat* nat = malloc(sizeof(TypeLevelNat));
    if (!nat) return NULL;
    
    nat->kind = NAT_SUCC;
    nat->predecessor = n;
    nat->value = n->value + 1;
    
    return nat;
}

void type_level_nat_free(TypeLevelNat* nat) {
    if (!nat) return;
    
    if (nat->predecessor) {
        type_level_nat_free(nat->predecessor);
    }
    
    free(nat);
}

TypeLevelNat* type_level_nat_add(TypeLevelNat* a, TypeLevelNat* b) {
    if (!a || !b) return NULL;
    
    if (a->kind == NAT_ZERO) {
        // Add(Zero, B) = B
        TypeLevelNat* result = malloc(sizeof(TypeLevelNat));
        if (result) {
            *result = *b; // Copy b
            result->predecessor = b->predecessor; // Share reference
        }
        return result;
    }
    
    if (a->kind == NAT_SUCC) {
        // Add(Succ(A), B) = Succ(Add(A, B))
        TypeLevelNat* inner_add = type_level_nat_add(a->predecessor, b);
        if (inner_add) {
            return type_level_nat_succ(inner_add);
        }
    }
    
    return NULL;
}

// =============================================================================
// Pattern Matching
// =============================================================================

TypePattern* type_pattern_new(TypePatternKind kind, const char* name) {
    TypePattern* pattern = malloc(sizeof(TypePattern));
    if (!pattern) return NULL;
    
    pattern->kind = kind;
    pattern->name = name ? str_dup(name) : NULL;
    pattern->subpatterns = NULL;
    pattern->subpattern_count = 0;
    pattern->literal_type = NULL;
    pattern->next = NULL;
    
    return pattern;
}

void type_pattern_free(TypePattern* pattern) {
    if (!pattern) return;
    
    free(pattern->name);
    
    if (pattern->subpatterns) {
        for (size_t i = 0; i < pattern->subpattern_count; i++) {
            type_pattern_free(pattern->subpatterns[i]);
        }
        free(pattern->subpatterns);
    }
    
    if (pattern->literal_type) {
        type_free(pattern->literal_type);
    }
    
    free(pattern);
}

int type_pattern_add_subpattern(TypePattern* pattern, TypePattern* subpattern) {
    if (!pattern || !subpattern) return 0;
    
    TypePattern** new_subpatterns = realloc(pattern->subpatterns, 
                                          sizeof(TypePattern*) * (pattern->subpattern_count + 1));
    if (!new_subpatterns) return 0;
    
    pattern->subpatterns = new_subpatterns;
    pattern->subpatterns[pattern->subpattern_count] = subpattern;
    pattern->subpattern_count++;
    
    return 1;
}

// Pattern matching environment for variable bindings
// struct body moved to type_level_internal.h (shared with eval unit)

PatternEnv* pattern_env_new(void) {
    PatternEnv* env = malloc(sizeof(PatternEnv));
    if (!env) return NULL;
    
    env->var_names = NULL;
    env->var_types = NULL;
    env->binding_count = 0;
    env->capacity = 0;
    
    return env;
}

void pattern_env_free(PatternEnv* env) {
    if (!env) return;
    
    for (size_t i = 0; i < env->binding_count; i++) {
        free(env->var_names[i]);
        type_free(env->var_types[i]);
    }
    
    free(env->var_names);
    free(env->var_types);
    free(env);
}

int pattern_env_bind(PatternEnv* env, const char* name, Type* type) {
    if (!env || !name || !type) return 0;
    
    if (env->binding_count >= env->capacity) {
        size_t new_capacity = env->capacity == 0 ? 8 : env->capacity * 2;
        
        char** new_names = realloc(env->var_names, sizeof(char*) * new_capacity);
        Type** new_types = realloc(env->var_types, sizeof(Type*) * new_capacity);
        
        if (!new_names || !new_types) {
            free(new_names);
            free(new_types);
            return 0;
        }
        
        env->var_names = new_names;
        env->var_types = new_types;
        env->capacity = new_capacity;
    }
    
    env->var_names[env->binding_count] = str_dup(name);
    env->var_types[env->binding_count] = type_copy(type);
    env->binding_count++;
    
    return 1;
}

// Match a type against a pattern
int type_matches_pattern(Type* type, TypePattern* pattern, PatternEnv* env) {
    if (!type || !pattern) return 0;
    
    switch (pattern->kind) {
        case TYPE_PATTERN_WILDCARD:
            return 1; // Wildcard matches anything
            
        case TYPE_PATTERN_VARIABLE:
            // Variable pattern binds the type to the variable name
            if (env && pattern->name) {
                return pattern_env_bind(env, pattern->name, type);
            }
            return 1;
            
        case TYPE_PATTERN_CONSTRUCTOR:
            // Constructor pattern matches specific type constructors
            if (pattern->name && type->name) {
                if (strcmp(pattern->name, "Zero") == 0) {
                    // Match Zero constructor for natural numbers
                    return type->kind == TYPE_UNKNOWN && type->name && 
                           strcmp(type->name, "Zero") == 0;
                }
                
                if (strcmp(pattern->name, "Succ") == 0) {
                    // Match Succ constructor - should have one subpattern
                    if (pattern->subpattern_count == 1 && type->name &&
                        strcmp(type->name, "Succ") == 0) {
                        // Recursively match the predecessor
                        return type_matches_pattern(type, pattern->subpatterns[0], env);
                    }
                }
                
                // Generic constructor matching
                return strcmp(pattern->name, type->name) == 0;
            }
            return 0;
            
        case TYPE_PATTERN_LITERAL:
            // Literal pattern matches exact type
            if (pattern->literal_type) {
                return type_equals(type, pattern->literal_type);
            }
            return 0;
            
        case TYPE_PATTERN_APPLICATION:
            // Application pattern matches type applications like Add<A, B>
            if (pattern->name && type->name) {
                return strcmp(pattern->name, type->name) == 0;
            }
            return 0;
            
        default:
            return 0;
    }
}

// Add a case to a type family
int type_family_add_case(TypeFamily* family, TypePattern* pattern, TypeLevelComputation* result) {
    if (!family || !pattern || !result) return 0;
    
    TypeFamilyCase* new_case = malloc(sizeof(TypeFamilyCase));
    if (!new_case) return 0;
    
    new_case->pattern = pattern;
    new_case->result = result;
    new_case->next = family->cases;
    family->cases = new_case;
    
    return 1;
}

// Evaluate a type family with given arguments
Type* type_family_evaluate(TypeFamily* family, Type** arguments, size_t arg_count, TypeChecker* checker) {
    if (!family || !arguments || !checker) return NULL;
    
    // Verify argument count matches parameter count
    if (arg_count != family->parameter_count) {
        return NULL;
    }
    
    // Try to match each case pattern
    TypeFamilyCase* case_item = family->cases;
    while (case_item) {
        PatternEnv* env = pattern_env_new();
        if (!env) continue;
        
        // Try to match all arguments against the pattern
        int matches = 1;
        
        if (family->parameter_count == 1) {
            // Single parameter family
            matches = type_matches_pattern(arguments[0], case_item->pattern, env);
        } else if (family->parameter_count == 2) {
            // Binary family (like Add<A, B>)
            // For binary families, the pattern should match the family application
            // This is a simplified implementation
            matches = type_matches_pattern(arguments[0], case_item->pattern, env);
        }
        
        if (matches) {
            // Pattern matched, evaluate the result with variable substitutions
            Type* result = evaluate_type_level_computation(case_item->result, checker);
            pattern_env_free(env);
            if (result) {
                return result;
            }
        }
        
        pattern_env_free(env);
        case_item = case_item->next;
    }
    
    // If no case matches, try the default case
    if (family->default_case) {
        return evaluate_type_level_computation(family->default_case, checker);
    }
    
    return NULL;
}

// Evaluate type computation with pattern environment
Type* evaluate_type_level_computation_with_env(TypeLevelComputation* computation, TypeChecker* checker, PatternEnv* env) {
    if (!computation || !checker) return NULL;
    
    // If no environment, use regular evaluation
    if (!env) {
        return evaluate_type_level_computation(computation, checker);
    }
    
    // For now, use the regular evaluation
    // In a full implementation, this would substitute pattern variables
    return evaluate_type_level_computation(computation, checker);
}

// =============================================================================
// Built-in Type Families
// =============================================================================

// Create the Add type family for type-level arithmetic
TypeFamily* create_add_type_family(void) {
    TypeFamily* add_family = type_family_new("Add");
    if (!add_family) return NULL;
    
    add_family->parameter_count = 2;
    
    // Add<Zero, B> = B
    TypePattern* zero_pattern = type_pattern_new(TYPE_PATTERN_CONSTRUCTOR, "Zero");
    TypePattern* b_pattern = type_pattern_new(TYPE_PATTERN_VARIABLE, "B");
    TypeLevelComputation* result_b = type_level_computation_new(TYPE_LEVEL_CONST, "B");
    
    if (zero_pattern && b_pattern && result_b) {
        type_family_add_case(add_family, zero_pattern, result_b);
    }
    
    // Add<Succ<A>, B> = Succ<Add<A, B>>
    TypePattern* succ_pattern = type_pattern_new(TYPE_PATTERN_CONSTRUCTOR, "Succ");
    TypePattern* a_pattern = type_pattern_new(TYPE_PATTERN_VARIABLE, "A");
    type_pattern_add_subpattern(succ_pattern, a_pattern);
    
    TypeLevelComputation* recursive_add = type_level_computation_new(TYPE_LEVEL_FUNCTION, "Add");
    TypeLevelComputation* succ_result = type_level_computation_new(TYPE_LEVEL_FUNCTION, "Succ");
    
    if (succ_pattern && recursive_add && succ_result) {
        type_family_add_case(add_family, succ_pattern, succ_result);
    }
    
    return add_family;
}

// Create the Mul type family for type-level multiplication
TypeFamily* create_mul_type_family(void) {
    TypeFamily* mul_family = type_family_new("Mul");
    if (!mul_family) return NULL;
    
    mul_family->parameter_count = 2;
    
    // Mul<Zero, B> = Zero
    TypePattern* zero_pattern = type_pattern_new(TYPE_PATTERN_CONSTRUCTOR, "Zero");
    TypeLevelComputation* result_zero = type_level_computation_new(TYPE_LEVEL_CONST, "Zero");
    
    if (zero_pattern && result_zero) {
        type_family_add_case(mul_family, zero_pattern, result_zero);
    }
    
    // Mul<Succ<A>, B> = Add<B, Mul<A, B>>
    TypePattern* succ_pattern = type_pattern_new(TYPE_PATTERN_CONSTRUCTOR, "Succ");
    TypePattern* a_pattern = type_pattern_new(TYPE_PATTERN_VARIABLE, "A");
    type_pattern_add_subpattern(succ_pattern, a_pattern);
    
    TypeLevelComputation* add_result = type_level_computation_new(TYPE_LEVEL_FUNCTION, "Add");
    
    if (succ_pattern && add_result) {
        type_family_add_case(mul_family, succ_pattern, add_result);
    }
    
    return mul_family;
}

// Create the Equal type family for type-level equality
TypeFamily* create_equal_type_family(void) {
    TypeFamily* equal_family = type_family_new("Equal");
    if (!equal_family) return NULL;
    
    equal_family->parameter_count = 2;
    
    // Equal<Zero, Zero> = true
    TypePattern* zero_zero_pattern = type_pattern_new(TYPE_PATTERN_CONSTRUCTOR, "Zero");
    TypeLevelComputation* result_true = type_level_computation_new(TYPE_LEVEL_CONST, "true");
    
    if (zero_zero_pattern && result_true) {
        type_family_add_case(equal_family, zero_zero_pattern, result_true);
    }
    
    // Equal<Succ<A>, Succ<B>> = Equal<A, B>
    TypePattern* succ_succ_pattern = type_pattern_new(TYPE_PATTERN_CONSTRUCTOR, "Succ");
    TypePattern* a_pattern = type_pattern_new(TYPE_PATTERN_VARIABLE, "A");
    TypePattern* b_pattern = type_pattern_new(TYPE_PATTERN_VARIABLE, "B");
    type_pattern_add_subpattern(succ_succ_pattern, a_pattern);
    
    TypeLevelComputation* recursive_equal = type_level_computation_new(TYPE_LEVEL_FUNCTION, "Equal");
    
    if (succ_succ_pattern && recursive_equal) {
        type_family_add_case(equal_family, succ_succ_pattern, recursive_equal);
    }
    
    // Default cases: Equal<Zero, Succ<B>> = false, Equal<Succ<A>, Zero> = false
    TypeLevelComputation* result_false = type_level_computation_new(TYPE_LEVEL_CONST, "false");
    equal_family->default_case = result_false;
    
    return equal_family;
}

