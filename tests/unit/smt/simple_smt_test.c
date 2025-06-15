#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

// Just include the SMT types we need
typedef enum {
    SMT_VAR,
    SMT_CONST,
    SMT_APP,
    SMT_QUANTIFIER
} SMTExpressionType;

typedef struct SMTExpression {
    SMTExpressionType type;
    
    union {
        struct {
            char* name;
            struct Type* var_type;
        } variable;
        
        struct {
            struct Type* const_type;
            int int_val;
            double float_val;
            bool bool_val;
            char* string_val;
        } constant;
        
        struct {
            char* function_name;
            struct SMTExpression** args;
            size_t arg_count;
        } application;
        
        struct {
            char** bound_vars;
            struct Type** var_types;
            size_t var_count;
            struct SMTExpression* body;
        } quantifier;
    };
    
    struct SMTExpression* next;
} SMTExpression;

// Copy the exact implementation from proof_generation.c
void smt_expression_free(SMTExpression* expr) {
    if (!expr) return;
    
    printf("Freeing SMT expression type %d at %p\n", expr->type, (void*)expr);
    
    switch (expr->type) {
        case SMT_VAR:
            printf("  Freeing variable name: %s\n", expr->variable.name ? expr->variable.name : "NULL");
            free(expr->variable.name);
            break;
        case SMT_CONST:
            printf("  Freeing constant\n");
            break;
        case SMT_APP:
            printf("  Freeing application function: %s\n", expr->application.function_name ? expr->application.function_name : "NULL");
            free(expr->application.function_name);
            if (expr->application.args) {
                printf("  Freeing %zu arguments\n", expr->application.arg_count);
                for (size_t i = 0; i < expr->application.arg_count; i++) {
                    printf("  Recursively freeing arg %zu: %p\n", i, (void*)expr->application.args[i]);
                    smt_expression_free(expr->application.args[i]);
                }
                free(expr->application.args);
            }
            break;
        case SMT_QUANTIFIER:
            if (expr->quantifier.bound_vars) {
                for (size_t i = 0; i < expr->quantifier.var_count; i++) {
                    free(expr->quantifier.bound_vars[i]);
                }
                free(expr->quantifier.bound_vars);
            }
            smt_expression_free(expr->quantifier.body);
            break;
        default:
            break;
    }
    
    printf("  Freeing expression itself at %p\n", (void*)expr);
    free(expr);
}

SMTExpression* smt_var(const char* name, struct Type* var_type) {
    if (!name) return NULL;
    
    SMTExpression* expr = malloc(sizeof(SMTExpression));
    if (!expr) return NULL;
    
    printf("Created variable '%s' at %p\n", name, (void*)expr);
    
    *expr = (SMTExpression) {
        .type = SMT_VAR,
        .variable = {
            .name = strdup(name),
            .var_type = var_type
        },
        .next = NULL
    };
    
    return expr;
}

SMTExpression* smt_const_int(int value) {
    SMTExpression* expr = malloc(sizeof(SMTExpression));
    if (!expr) return NULL;
    
    printf("Created constant %d at %p\n", value, (void*)expr);
    
    *expr = (SMTExpression) {
        .type = SMT_CONST,
        .constant = {
            .const_type = NULL,
            .int_val = value
        },
        .next = NULL
    };
    
    return expr;
}

SMTExpression* smt_app(const char* function_name, SMTExpression** args, size_t arg_count) {
    if (!function_name) return NULL;
    
    SMTExpression* expr = malloc(sizeof(SMTExpression));
    if (!expr) return NULL;
    
    // Copy arguments array
    SMTExpression** copied_args = NULL;
    if (args && arg_count > 0) {
        copied_args = malloc(arg_count * sizeof(SMTExpression*));
        if (!copied_args) {
            free(expr);
            return NULL;
        }
        for (size_t i = 0; i < arg_count; i++) {
            copied_args[i] = args[i];
            printf("  Copied arg %zu: %p\n", i, (void*)args[i]);
        }
    }
    
    printf("Created application '%s' at %p with %zu args\n", function_name, (void*)expr, arg_count);
    
    *expr = (SMTExpression) {
        .type = SMT_APP,
        .application = {
            .function_name = strdup(function_name),
            .args = copied_args,
            .arg_count = arg_count
        },
        .next = NULL
    };
    
    return expr;
}

int main() {
    printf("=== Simple SMT Expression Test ===\n");
    
    // Create some basic SMT expressions
    SMTExpression* var_x = smt_var("x", NULL);
    SMTExpression* const_5 = smt_const_int(5);
    
    SMTExpression* greater_than = smt_app(">", 
        (SMTExpression*[]){var_x, const_5}, 2);
    
    printf("\n=== Freeing expressions ===\n");
    // Only free the parent expression - it owns the arguments
    smt_expression_free(greater_than);
    
    printf("\n=== Test completed ===\n");
    return 0;
}
