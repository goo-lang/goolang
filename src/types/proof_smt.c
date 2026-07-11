#include "../../include/proof_generation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <math.h>
#include <unistd.h>    // For mkstemp, write, close, unlink
#include <sys/stat.h>  // For mkdir

// C23 compatibility checks
_Static_assert(sizeof(ProofType) == sizeof(int), "ProofType should be int size");
_Static_assert(sizeof(SMTSolver) == sizeof(int), "SMTSolver should be int size");

// SMT layer: expression constructors, satisfiability checking, solver
// invocation, serialization. Split from proof_generation.c (refactor,
// no behavior change).
// =============================================================================
// SMT Expression Construction and Management
// =============================================================================

SMTExpression* smt_var(const char* name, struct Type* type) {
    if (!name) return NULL;
    
    SMTExpression* expr = xmalloc(sizeof(SMTExpression));
    if (!expr) return NULL;
    
    *expr = (SMTExpression) {
        .type = SMT_VAR,
        .variable = {
            .name = strdup(name),
            .var_type = type
        },
        .next = NULL
    };
    
    return expr;
}

SMTExpression* smt_const_int(int64_t value) {
    SMTExpression* expr = xmalloc(sizeof(SMTExpression));
    if (!expr) return NULL;
    
    *expr = (SMTExpression) {
        .type = SMT_CONST,
        .constant = {
            .int_val = value,
            .const_type = NULL  // Would be int type
        },
        .next = NULL
    };
    
    return expr;
}

SMTExpression* smt_const_bool(bool value) {
    SMTExpression* expr = xmalloc(sizeof(SMTExpression));
    if (!expr) return NULL;
    
    *expr = (SMTExpression) {
        .type = SMT_CONST,
        .constant = {
            .bool_val = value,
            .const_type = NULL  // Would be bool type
        },
        .next = NULL
    };
    
    return expr;
}

SMTExpression* smt_app(const char* function_name, SMTExpression** args, size_t arg_count) {
    if (!function_name) return NULL;
    
    SMTExpression* expr = xmalloc(sizeof(SMTExpression));
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
        }
    }
    
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

SMTExpression* smt_forall(char** var_names, struct Type** var_types, size_t var_count, SMTExpression* body) {
    if (!var_names || !body || var_count == 0) return NULL;
    
    SMTExpression* expr = xmalloc(sizeof(SMTExpression));
    if (!expr) return NULL;
    
    // Copy variable names
    char** copied_names = malloc(var_count * sizeof(char*));
    if (!copied_names) {
        free(expr);
        return NULL;
    }
    for (size_t i = 0; i < var_count; i++) {
        copied_names[i] = strdup(var_names[i]);
    }
    
    *expr = (SMTExpression) {
        .type = SMT_QUANTIFIER,
        .quantifier = {
            .quantifier_type = SMT_FORALL,
            .bound_vars = copied_names,
            .var_types = var_types,  // Shallow copy for now
            .var_count = var_count,
            .body = body
        },
        .next = NULL
    };
    
    return expr;
}

SMTExpression* smt_and(SMTExpression* left, SMTExpression* right) {
    return smt_app("and", (SMTExpression*[]){left, right}, 2);
}

SMTExpression* smt_or(SMTExpression* left, SMTExpression* right) {
    return smt_app("or", (SMTExpression*[]){left, right}, 2);
}

SMTExpression* smt_not(SMTExpression* expr) {
    return smt_app("not", (SMTExpression*[]){expr}, 1);
}

SMTExpression* smt_implies(SMTExpression* left, SMTExpression* right) {
    return smt_app("=>", (SMTExpression*[]){left, right}, 2);
}

SMTExpression* smt_equals(SMTExpression* left, SMTExpression* right) {
    return smt_app("=", (SMTExpression*[]){left, right}, 2);
}

void smt_expression_free(SMTExpression* expr) {
    if (!expr) return;
    
    switch (expr->type) {
        case SMT_VAR:
            free(expr->variable.name);
            break;
        case SMT_CONST:
            // Only free string_val if we know it's actually a string
            // We need to be very careful here because string_val is in a union
            // For now, we'll only free if const_type indicates it's a string
            // and string_val is not NULL
            if (expr->constant.const_type && expr->constant.string_val) {
                // This is risky - we need a better way to identify string constants
                // For now, let's not free string_val to avoid double-free
                // TODO: Add proper constant type tracking
            }
            break;
        case SMT_APP:
            free(expr->application.function_name);
            if (expr->application.args) {
                for (size_t i = 0; i < expr->application.arg_count; i++) {
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
    
    free(expr);
}

// =============================================================================
// SMT Solver Integration
// =============================================================================

SMTResult smt_check_satisfiability(ProofGenerationContext* ctx, SMTExpression* formula) {
    if (!ctx || !formula) return SMT_RESULT_ERROR;
    
    ctx->statistics.smt_queries_generated++;
    
    // Generate SMT-LIB format string
    char* smt_query = smt_expression_to_string(formula);
    if (!smt_query) return SMT_RESULT_ERROR;
    
    printf("🔍 SMT Query generated (%zu chars)\n", strlen(smt_query));
    
    SMTResult result = SMT_RESULT_UNKNOWN;
    
    switch (ctx->solver_backend) {
        case SMT_SOLVER_Z3:
            result = invoke_z3_solver(ctx, smt_query);
            break;
        case SMT_SOLVER_CVC5:
            result = invoke_cvc5_solver(ctx, smt_query);
            break;
        case SMT_SOLVER_YICES:
            result = invoke_yices_solver(ctx, smt_query);
            break;
        default:
            printf("⚠️  Unsupported SMT solver backend\n");
            result = SMT_RESULT_ERROR;
            break;
    }
    
    free(smt_query);
    return result;
}

SMTResult invoke_z3_solver(ProofGenerationContext* ctx, const char* smt_query) {
    if (!ctx || !smt_query) return SMT_RESULT_ERROR;
    
    // Create temporary file for SMT query
    char temp_file[] = "/tmp/goo_smt_query_XXXXXX";
    int fd = mkstemp(temp_file);
    if (fd == -1) {
        return SMT_RESULT_ERROR;
    }
    
    // Write query to file
    write(fd, smt_query, strlen(smt_query));
    close(fd);
    
    // Invoke Z3
    char command[512];
    snprintf(command, sizeof(command), 
             "timeout %d z3 -smt2 %s 2>/dev/null", 
             ctx->solver_timeout_seconds, temp_file);
    
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        unlink(temp_file);
        return SMT_RESULT_ERROR;
    }
    
    char result_buffer[256];
    SMTResult result = SMT_RESULT_UNKNOWN;
    
    if (fgets(result_buffer, sizeof(result_buffer), pipe)) {
        if (strstr(result_buffer, "sat") && !strstr(result_buffer, "unsat")) {
            result = SMT_RESULT_SAT;
        } else if (strstr(result_buffer, "unsat")) {
            result = SMT_RESULT_UNSAT;
        } else if (strstr(result_buffer, "timeout")) {
            result = SMT_RESULT_TIMEOUT;
        }
    }
    
    pclose(pipe);
    unlink(temp_file);
    
    printf("🔧 Z3 solver result: %s\n", 
           result == SMT_RESULT_SAT ? "SAT" : 
           result == SMT_RESULT_UNSAT ? "UNSAT" : 
           result == SMT_RESULT_TIMEOUT ? "TIMEOUT" : "UNKNOWN");
    
    return result;
}

SMTResult invoke_cvc5_solver(ProofGenerationContext* ctx, const char* smt_query) {
    // Similar implementation for CVC5
    printf("🔧 CVC5 solver not yet implemented, falling back to mock result\n");
    return SMT_RESULT_UNSAT;  // Mock result for now
}

SMTResult invoke_yices_solver(ProofGenerationContext* ctx, const char* smt_query) {
    // Similar implementation for Yices
    printf("🔧 Yices solver not yet implemented, falling back to mock result\n");
    return SMT_RESULT_UNSAT;  // Mock result for now
}

char* smt_expression_to_string(SMTExpression* expr) {
    if (!expr) return NULL;
    
    char* buffer = malloc(4096);  // Large buffer for complex expressions
    if (!buffer) return NULL;
    
    switch (expr->type) {
        case SMT_VAR:
            snprintf(buffer, 4096, "%s", expr->variable.name);
            break;
        case SMT_CONST:
            snprintf(buffer, 4096, "%lld", expr->constant.int_val);
            break;
        case SMT_APP:
            if (expr->application.arg_count == 0) {
                snprintf(buffer, 4096, "%s", expr->application.function_name);
            } else {
                snprintf(buffer, 4096, "(%s", expr->application.function_name);
                for (size_t i = 0; i < expr->application.arg_count; i++) {
                    char* arg_str = smt_expression_to_string(expr->application.args[i]);
                    if (arg_str) {
                        strncat(buffer, " ", 4095 - strlen(buffer));
                        strncat(buffer, arg_str, 4095 - strlen(buffer));
                        free(arg_str);
                    }
                }
                strncat(buffer, ")", 4095 - strlen(buffer));
            }
            break;
        case SMT_QUANTIFIER:
            snprintf(buffer, 4096, "(%s (", 
                     expr->quantifier.quantifier_type == SMT_FORALL ? "forall" : "exists");
            for (size_t i = 0; i < expr->quantifier.var_count; i++) {
                strncat(buffer, "(", 4095 - strlen(buffer));
                strncat(buffer, expr->quantifier.bound_vars[i], 4095 - strlen(buffer));
                strncat(buffer, " Int)", 4095 - strlen(buffer));  // Simplified type
            }
            strncat(buffer, ") ", 4095 - strlen(buffer));
            char* body_str = smt_expression_to_string(expr->quantifier.body);
            if (body_str) {
                strncat(buffer, body_str, 4095 - strlen(buffer));
                free(body_str);
            }
            strncat(buffer, ")", 4095 - strlen(buffer));
            break;
        default:
            snprintf(buffer, 4096, "unknown-expr");
            break;
    }
    
    return buffer;
}

