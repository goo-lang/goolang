#include "interface_system.h"
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

// Type-level natural number representation
typedef struct TypeLevelNat {
    enum { NAT_ZERO, NAT_SUCC } kind;
    struct TypeLevelNat* predecessor; // For Succ(n), this is n
    size_t value; // Cached numeric value
} TypeLevelNat;

// Pattern kinds for type family matching are now defined in interface_system.h

// Pattern for type family case matching
typedef struct TypePattern {
    TypePatternKind kind;
    char* name;                    // Variable or constructor name
    struct TypePattern** subpatterns; // Subpatterns for constructors
    size_t subpattern_count;
    Type* literal_type;            // For literal patterns
    struct TypePattern* next;
} TypePattern;

// Define a type family case
typedef struct TypeFamilyCase {
    TypePattern* pattern;          // Pattern to match against
    TypeLevelComputation* result;  // Result computation
    struct TypeFamilyCase* next;   // Next case
} TypeFamilyCase;

// Define a type family
typedef struct TypeFamily {
    char* name;                    // Family name
    TypeVariable* parameters;     // Type parameters
    TypeFamilyCase* cases;        // List of cases
    TypeLevelComputation* default_case; // Default case
    size_t parameter_count;       // Number of parameters
} TypeFamily;

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
typedef struct PatternEnv {
    char** var_names;
    Type** var_types;
    size_t binding_count;
    size_t capacity;
} PatternEnv;

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
    matrix_type->data.struct_type.fields = malloc(sizeof(StructField));
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
    matrix_type->data.struct_type.fields = malloc(sizeof(StructField));
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
    vec_type->data.struct_type.fields = malloc(sizeof(StructField) * 3);
    
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
    phantom->data.struct_type.fields = malloc(sizeof(StructField));
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
