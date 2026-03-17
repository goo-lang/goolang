#include "auto_parallel.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =============================================================================
// Lifecycle
// =============================================================================

AutoParallelizer* auto_parallel_new(const TargetInfo* target) {
    AutoParallelizer* ap = calloc(1, sizeof(AutoParallelizer));
    if (!ap) return NULL;

    if (target) ap->target = *target;
    ap->min_trip_count = 64;
    ap->min_work_estimate = 10;
    ap->prefer_simd = true;
    ap->allow_speculative = false;

    return ap;
}

void auto_parallel_free(AutoParallelizer* ap) {
    if (!ap) return;

    ParallelPlan* plan = ap->plans;
    while (plan) {
        ParallelPlan* next = plan->next;
        dependency_list_free(plan->analysis.dependencies);
        free(plan);
        plan = next;
    }

    free(ap);
}

// =============================================================================
// Dependency Analysis
// =============================================================================

// Check if an AST node reads a variable
static bool reads_variable(ASTNode* node, const char* var) {
    if (!node || !var) return false;

    if (node->type == AST_IDENTIFIER) {
        IdentifierNode* ident = (IdentifierNode*)node;
        if (strcmp(ident->name, var) == 0) return true;
    }

    if (node->type == AST_BINARY_EXPR) {
        BinaryExprNode* bin = (BinaryExprNode*)node;
        return reads_variable(bin->left, var) || reads_variable(bin->right, var);
    }

    if (node->type == AST_UNARY_EXPR) {
        UnaryExprNode* unary = (UnaryExprNode*)node;
        return reads_variable(unary->operand, var);
    }

    if (node->type == AST_INDEX_EXPR) {
        IndexExprNode* idx = (IndexExprNode*)node;
        return reads_variable(idx->expr, var) || reads_variable(idx->index, var);
    }

    if (node->type == AST_CALL_EXPR) {
        CallExprNode* call = (CallExprNode*)node;
        ASTNode* arg = call->args;
        while (arg) {
            if (reads_variable(arg, var)) return true;
            arg = arg->next;
        }
    }

    return false;
}

// Check if an AST node writes to a variable
static bool writes_variable(ASTNode* node, const char* var) {
    if (!node || !var) return false;

    if (node->type == AST_VAR_DECL) {
        VarDeclNode* decl = (VarDeclNode*)node;
        for (size_t i = 0; i < decl->name_count; i++) {
            if (strcmp(decl->names[i], var) == 0) return true;
        }
    }

    // Binary assignment: var = expr or var[i] = expr
    if (node->type == AST_BINARY_EXPR) {
        BinaryExprNode* bin = (BinaryExprNode*)node;
        if (bin->operator == TOKEN_ASSIGN) {
            if (bin->left && bin->left->type == AST_IDENTIFIER) {
                IdentifierNode* ident = (IdentifierNode*)bin->left;
                if (strcmp(ident->name, var) == 0) return true;
            }
            // Array element assignment: arr[i] = ...
            if (bin->left && bin->left->type == AST_INDEX_EXPR) {
                IndexExprNode* idx = (IndexExprNode*)bin->left;
                if (idx->expr && idx->expr->type == AST_IDENTIFIER) {
                    IdentifierNode* arr_ident = (IdentifierNode*)idx->expr;
                    if (strcmp(arr_ident->name, var) == 0) return true;
                }
            }
        }
    }

    return false;
}

// Collect all variables written in a block
static void collect_written_vars(ASTNode* node, char*** vars, size_t* count, size_t* capacity) {
    if (!node) return;

    if (node->type == AST_VAR_DECL) {
        VarDeclNode* decl = (VarDeclNode*)node;
        for (size_t i = 0; i < decl->name_count; i++) {
            if (*count >= *capacity) {
                *capacity = (*capacity == 0) ? 8 : *capacity * 2;
                char** tmp = realloc(*vars, *capacity * sizeof(char*));
                if (!tmp) return;
                *vars = tmp;
            }
            (*vars)[(*count)++] = decl->names[i];
        }
    }

    if (node->type == AST_BLOCK_STMT) {
        BlockStmtNode* block = (BlockStmtNode*)node;
        ASTNode* stmt = block->statements;
        while (stmt) {
            collect_written_vars(stmt, vars, count, capacity);
            stmt = stmt->next;
        }
    }
}

DataDependency* dependency_analyze_loop_body(ASTNode* body, const char* induction_var) {
    if (!body) return NULL;

    DataDependency* deps = NULL;

    // Collect all variables written in the loop body
    char** written_vars = NULL;
    size_t written_count = 0;
    size_t written_capacity = 0;
    collect_written_vars(body, &written_vars, &written_count, &written_capacity);

    // For each written variable, check if it's also read
    for (size_t i = 0; i < written_count; i++) {
        // Skip the induction variable
        if (induction_var && strcmp(written_vars[i], induction_var) == 0) continue;

        bool is_read = reads_variable(body, written_vars[i]);
        bool is_written = writes_variable(body, written_vars[i]);

        if (is_read && is_written) {
            // Potential loop-carried dependency
            DataDependency* dep = calloc(1, sizeof(DataDependency));
            if (!dep) continue;

            dep->kind = DEP_FLOW;
            dep->variable = written_vars[i];
            dep->is_loop_carried = true;
            dep->next = deps;
            deps = dep;
        }
    }

    free(written_vars);
    return deps;
}

void dependency_list_free(DataDependency* deps) {
    while (deps) {
        DataDependency* next = deps->next;
        free(deps);
        deps = next;
    }
}

bool auto_parallel_has_loop_carried_deps(DataDependency* deps) {
    for (DataDependency* d = deps; d; d = d->next) {
        if (d->is_loop_carried && d->kind != DEP_REDUCTION) return true;
    }
    return false;
}

// =============================================================================
// Reduction Detection
// =============================================================================

ReductionKind auto_parallel_detect_reduction(ASTNode* loop_body, const char* induction_var) {
    (void)induction_var;
    if (!loop_body) return REDUCE_NONE;

    // Look for patterns like: acc = acc + expr, acc = acc * expr, etc.
    if (loop_body->type == AST_BLOCK_STMT) {
        BlockStmtNode* block = (BlockStmtNode*)loop_body;
        ASTNode* stmt = block->statements;
        while (stmt) {
            if (stmt->type == AST_EXPR_STMT) {
                ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
                ASTNode* expr = expr_stmt->expr;

                // Check for: acc += expr pattern (binary with TOKEN_ASSIGN)
                if (expr && expr->type == AST_BINARY_EXPR) {
                    BinaryExprNode* bin = (BinaryExprNode*)expr;

                    // Look for compound operations in the RHS
                    if (bin->operator == TOKEN_ASSIGN && bin->right &&
                        bin->right->type == AST_BINARY_EXPR) {
                        BinaryExprNode* rhs = (BinaryExprNode*)bin->right;

                        // Check if LHS appears in RHS (acc = acc op something)
                        if (bin->left && bin->left->type == AST_IDENTIFIER &&
                            rhs->left && rhs->left->type == AST_IDENTIFIER) {
                            IdentifierNode* lhs = (IdentifierNode*)bin->left;
                            IdentifierNode* rhs_var = (IdentifierNode*)rhs->left;

                            if (strcmp(lhs->name, rhs_var->name) == 0) {
                                switch (rhs->operator) {
                                    case TOKEN_PLUS:     return REDUCE_SUM;
                                    case TOKEN_MULTIPLY: return REDUCE_PRODUCT;
                                    default: break;
                                }
                            }
                        }
                    }
                }
            }
            stmt = stmt->next;
        }
    }

    return REDUCE_NONE;
}

// =============================================================================
// Loop Analysis
// =============================================================================

LoopAnalysis auto_parallel_analyze_loop(AutoParallelizer* ap, ASTNode* for_stmt) {
    LoopAnalysis analysis = {0};
    analysis.parallel_kind = LOOP_SEQUENTIAL;
    analysis.reduction = REDUCE_NONE;
    analysis.trip_count = -1;

    if (!ap || !for_stmt || for_stmt->type != AST_FOR_STMT) return analysis;

    ForStmtNode* loop = (ForStmtNode*)for_stmt;
    ap->stats.loops_analyzed++;

    // Try to determine induction variable
    if (loop->init && loop->init->type == AST_VAR_DECL) {
        VarDeclNode* init_var = (VarDeclNode*)loop->init;
        if (init_var->name_count > 0) {
            analysis.induction_var = init_var->names[0];
        }
    }

    // Analyze dependencies
    analysis.dependencies = dependency_analyze_loop_body(loop->body, analysis.induction_var);
    for (DataDependency* d = analysis.dependencies; d; d = d->next) {
        analysis.dependency_count++;
    }
    ap->stats.dependencies_found += analysis.dependency_count;

    // Check for reductions
    analysis.reduction = auto_parallel_detect_reduction(loop->body, analysis.induction_var);
    if (analysis.reduction != REDUCE_NONE) {
        ap->stats.reductions_detected++;
    }

    // Determine parallelization kind
    if (analysis.dependency_count == 0) {
        analysis.parallel_kind = LOOP_PARALLEL;
    } else if (analysis.reduction != REDUCE_NONE &&
               !auto_parallel_has_loop_carried_deps(analysis.dependencies)) {
        analysis.parallel_kind = LOOP_REDUCTION;
    } else if (!auto_parallel_has_loop_carried_deps(analysis.dependencies)) {
        analysis.parallel_kind = LOOP_PARTIALLY_PARALLEL;
    }

    // Simple cost model
    analysis.estimated_work = 10; // Default estimate
    analysis.min_parallel_iters = ap->min_trip_count;
    analysis.worth_parallelizing = (analysis.parallel_kind != LOOP_SEQUENTIAL);

    // Vectorizability check
    if (analysis.parallel_kind == LOOP_PARALLEL ||
        analysis.parallel_kind == LOOP_REDUCTION) {
        analysis.is_vectorizable = true;
        analysis.vector_width = ap->target.has_avx2 ? 8 :
                                ap->target.has_sse2 ? 4 : 2;
    }

    return analysis;
}

// =============================================================================
// Strategy Selection
// =============================================================================

ParallelStrategy auto_parallel_choose_strategy(const LoopAnalysis* analysis,
                                                const TargetInfo* target) {
    if (!analysis || analysis->parallel_kind == LOOP_SEQUENTIAL) {
        return PAR_STRATEGY_NONE;
    }

    // Prefer SIMD for small, vectorizable loops
    if (analysis->is_vectorizable && target &&
        (target->has_avx2 || target->has_sse2 || target->has_neon)) {
        if (analysis->trip_count > 0 && analysis->trip_count <= 1024) {
            return PAR_STRATEGY_SIMD;
        }
    }

    // Use reduction strategy for reduction loops
    if (analysis->parallel_kind == LOOP_REDUCTION) {
        return PAR_STRATEGY_REDUCTION;
    }

    // Use parallel for for large parallel loops
    if (analysis->parallel_kind == LOOP_PARALLEL) {
        return PAR_STRATEGY_PARALLEL_FOR;
    }

    // Task spawn for partially parallel work
    if (analysis->parallel_kind == LOOP_PARTIALLY_PARALLEL) {
        return PAR_STRATEGY_TASK_SPAWN;
    }

    return PAR_STRATEGY_NONE;
}

// =============================================================================
// Function-Level Planning
// =============================================================================

ParallelPlan* auto_parallel_plan_function(AutoParallelizer* ap, ASTNode* func) {
    if (!ap || !func || func->type != AST_FUNC_DECL) return NULL;

    FuncDeclNode* func_decl = (FuncDeclNode*)func;
    if (!func_decl->body) return NULL;

    ParallelPlan* plans = NULL;

    // Walk the function body looking for parallelizable loops
    ASTNode* stmt = NULL;
    if (func_decl->body->type == AST_BLOCK_STMT) {
        BlockStmtNode* block = (BlockStmtNode*)func_decl->body;
        stmt = block->statements;
    }

    while (stmt) {
        if (stmt->type == AST_FOR_STMT) {
            LoopAnalysis analysis = auto_parallel_analyze_loop(ap, stmt);

            if (analysis.worth_parallelizing) {
                ParallelPlan* plan = calloc(1, sizeof(ParallelPlan));
                if (plan) {
                    plan->strategy = auto_parallel_choose_strategy(&analysis, &ap->target);
                    plan->target_node = stmt;
                    plan->analysis = analysis;
                    plan->num_threads = ap->target.num_cores;
                    plan->chunk_size = 64;
                    plan->next = plans;
                    plans = plan;
                    ap->plan_count++;

                    if (plan->strategy == PAR_STRATEGY_SIMD) {
                        ap->stats.loops_vectorized++;
                    } else if (plan->strategy != PAR_STRATEGY_NONE) {
                        ap->stats.loops_parallelized++;
                    }
                }
            } else {
                dependency_list_free(analysis.dependencies);
            }
        }
        stmt = stmt->next;
    }

    ap->plans = plans;
    return plans;
}

// =============================================================================
// Annotation Parsing
// =============================================================================

bool auto_parallel_parse_annotation(const char* args, size_t* min_iters, size_t* chunk_size) {
    if (!min_iters || !chunk_size) return false;

    *min_iters = 64;     // Default
    *chunk_size = 0;     // Auto

    if (!args || strlen(args) == 0) return true;

    // Parse "min_iters=N,chunk_size=M" format
    const char* p = args;
    while (*p) {
        if (strncmp(p, "min_iters=", 10) == 0) {
            *min_iters = (size_t)atoll(p + 10);
        } else if (strncmp(p, "chunk_size=", 11) == 0) {
            *chunk_size = (size_t)atoll(p + 11);
        }

        // Skip to next parameter
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }

    return true;
}
