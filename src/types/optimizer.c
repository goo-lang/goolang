#include "optimizer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =============================================================================
// Target Detection
// =============================================================================

TargetInfo optimizer_detect_host_target(void) {
    TargetInfo target = {0};

#if defined(__x86_64__) || defined(_M_X64)
    target.arch = "x86_64";
    target.pointer_size = 8;
    target.has_sse = true;
    target.has_sse2 = true;
#ifdef __AVX__
    target.has_avx = true;
#endif
#ifdef __AVX2__
    target.has_avx2 = true;
#endif
#ifdef __AVX512F__
    target.has_avx512 = true;
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    target.arch = "aarch64";
    target.pointer_size = 8;
    target.has_neon = true;
#elif defined(__i386__) || defined(_M_IX86)
    target.arch = "x86";
    target.pointer_size = 4;
#else
    target.arch = "unknown";
    target.pointer_size = sizeof(void*);
#endif

#if defined(__linux__)
    target.os = "linux";
#elif defined(__APPLE__)
    target.os = "darwin";
#elif defined(_WIN32)
    target.os = "windows";
#else
    target.os = "unknown";
#endif

    target.cache_line_size = 64;
    target.num_cores = 1; // Conservative default

    // Try to detect core count
#if defined(__linux__)
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        size_t cores = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "processor", 9) == 0) cores++;
        }
        fclose(f);
        if (cores > 0) target.num_cores = cores;
    }
#endif

    return target;
}

bool comptime_target_has(const TargetInfo* target, const char* feature) {
    if (!target || !feature) return false;

    if (strcmp(feature, "sse") == 0)       return target->has_sse;
    if (strcmp(feature, "sse2") == 0)      return target->has_sse2;
    if (strcmp(feature, "avx") == 0)       return target->has_avx;
    if (strcmp(feature, "avx2") == 0)      return target->has_avx2;
    if (strcmp(feature, "avx512") == 0)    return target->has_avx512;
    if (strcmp(feature, "neon") == 0)      return target->has_neon;
    if (strcmp(feature, "gpu") == 0)       return target->has_gpu;
    if (strcmp(feature, "crypto") == 0)    return target->has_dedicated_crypto;
    if (strcmp(feature, "tensor") == 0)    return target->has_tensor_core;

    return false;
}

// =============================================================================
// Type Intrinsics
// =============================================================================

size_t comptime_sizeof_type(Type* type) {
    return type ? type->size : 0;
}

size_t comptime_alignof_type(Type* type) {
    return type ? type->align : 0;
}

bool comptime_type_is_numeric(Type* type) {
    return type && type_is_numeric(type);
}

bool comptime_type_is_vectorizable(Type* type) {
    if (!type) return false;
    // Scalar numeric types and fixed-size arrays of numeric types are vectorizable
    if (type_is_numeric(type)) return true;
    if (type->kind == TYPE_ARRAY && type->data.array.element_type) {
        return type_is_numeric(type->data.array.element_type);
    }
    return false;
}

// =============================================================================
// Optimizer Lifecycle
// =============================================================================

Optimizer* optimizer_new(OptimizationGoal goals) {
    Optimizer* opt = calloc(1, sizeof(Optimizer));
    if (!opt) return NULL;

    opt->goals = goals;
    opt->pass_capacity = 16;
    opt->passes = calloc(opt->pass_capacity, sizeof(OptimizationPass));
    opt->comptime = comptime_interpreter_new();
    opt->target = optimizer_detect_host_target();

    if (!opt->passes || !opt->comptime) {
        optimizer_free(opt);
        return NULL;
    }

    return opt;
}

void optimizer_free(Optimizer* opt) {
    if (!opt) return;
    free(opt->passes);
    comptime_interpreter_free(opt->comptime);
    free(opt);
}

void optimizer_set_target(Optimizer* opt, const TargetInfo* target) {
    if (opt && target) opt->target = *target;
}

// =============================================================================
// Pass Management
// =============================================================================

static const char* pass_name(OptPassKind kind) {
    switch (kind) {
        case OPT_PASS_CONSTANT_FOLD:       return "constant-fold";
        case OPT_PASS_CONSTANT_PROPAGATE:  return "constant-propagate";
        case OPT_PASS_DEAD_CODE_ELIMINATE: return "dead-code-eliminate";
        case OPT_PASS_INLINE_SMALL:        return "inline-small";
        case OPT_PASS_LOOP_UNROLL:         return "loop-unroll";
        case OPT_PASS_LOOP_VECTORIZE:      return "loop-vectorize";
        case OPT_PASS_TAIL_CALL:           return "tail-call";
        case OPT_PASS_ESCAPE_BASED_ALLOC:  return "escape-based-alloc";
        case OPT_PASS_COMPTIME_EVALUATE:   return "comptime-process";
    }
    return "unknown";
}

void optimizer_add_pass(Optimizer* opt, OptPassKind kind) {
    if (!opt) return;

    if (opt->pass_count >= opt->pass_capacity) {
        size_t new_cap = opt->pass_capacity * 2;
        OptimizationPass* tmp = realloc(opt->passes, new_cap * sizeof(OptimizationPass));
        if (!tmp) return;
        opt->passes = tmp;
        opt->pass_capacity = new_cap;
    }

    OptimizationPass* pass = &opt->passes[opt->pass_count++];
    memset(pass, 0, sizeof(OptimizationPass));
    pass->kind = kind;
    pass->name = pass_name(kind);
    pass->enabled = true;

    // Set defaults based on kind
    switch (kind) {
        case OPT_PASS_INLINE_SMALL:
            pass->inline_threshold = 20;
            break;
        case OPT_PASS_LOOP_UNROLL:
            pass->unroll_factor = 4;
            break;
        case OPT_PASS_LOOP_VECTORIZE:
            pass->vectorize_width = opt->target.has_avx2 ? 8 :
                                    opt->target.has_sse2 ? 4 : 2;
            break;
        default:
            break;
    }
}

void optimizer_add_default_passes(Optimizer* opt) {
    if (!opt) return;

    // Always run these
    optimizer_add_pass(opt, OPT_PASS_COMPTIME_EVALUATE);
    optimizer_add_pass(opt, OPT_PASS_CONSTANT_FOLD);
    optimizer_add_pass(opt, OPT_PASS_CONSTANT_PROPAGATE);
    optimizer_add_pass(opt, OPT_PASS_DEAD_CODE_ELIMINATE);

    // Goal-dependent passes
    if (opt->goals & OPT_GOAL_THROUGHPUT) {
        optimizer_add_pass(opt, OPT_PASS_INLINE_SMALL);
        optimizer_add_pass(opt, OPT_PASS_LOOP_UNROLL);
        optimizer_add_pass(opt, OPT_PASS_LOOP_VECTORIZE);
    }

    if (opt->goals & OPT_GOAL_MEMORY) {
        optimizer_add_pass(opt, OPT_PASS_ESCAPE_BASED_ALLOC);
    }

    if (opt->goals & OPT_GOAL_LATENCY) {
        optimizer_add_pass(opt, OPT_PASS_TAIL_CALL);
        optimizer_add_pass(opt, OPT_PASS_INLINE_SMALL);
    }
}

// =============================================================================
// Constant Folding — replace constant expressions with their values
// =============================================================================

size_t opt_constant_fold(ASTNode* node, ComptimeInterpreter* interp) {
    if (!node || !interp) return 0;
    size_t count = 0;

    if (node->type == AST_BINARY_EXPR) {
        BinaryExprNode* bin = (BinaryExprNode*)node;

        // Recurse first
        count += opt_constant_fold(bin->left, interp);
        count += opt_constant_fold(bin->right, interp);

        // If both operands are literals, fold
        if (bin->left && bin->left->type == AST_LITERAL &&
            bin->right && bin->right->type == AST_LITERAL) {
            ComptimeValue result = comptime_eval_expression(interp, node);
            if (!comptime_has_error(interp) && result.kind != COMPTIME_ERROR) {
                // Replace the binary expression with a literal
                // (In a full implementation, this would modify the AST in-place)
                count++;
            }
            comptime_value_free(&result);
        }
    }

    // Recurse into child nodes
    if (node->next) {
        count += opt_constant_fold(node->next, interp);
    }

    return count;
}

// =============================================================================
// Dead Code Elimination — remove unreachable code
// =============================================================================

size_t opt_dead_code_eliminate(ASTNode* node) {
    if (!node) return 0;
    size_t count = 0;

    if (node->type == AST_IF_STMT) {
        IfStmtNode* if_stmt = (IfStmtNode*)node;

        // If condition is a known-true literal, eliminate else branch
        if (if_stmt->condition && if_stmt->condition->type == AST_LITERAL) {
            LiteralNode* lit = (LiteralNode*)if_stmt->condition;
            if (lit->literal_type == TOKEN_TRUE && if_stmt->else_stmt) {
                ast_node_free(if_stmt->else_stmt);
                if_stmt->else_stmt = NULL;
                count++;
            } else if (lit->literal_type == TOKEN_FALSE && if_stmt->then_stmt) {
                // Replace then with else (or eliminate if no else)
                if (if_stmt->else_stmt) {
                    ast_node_free(if_stmt->then_stmt);
                    if_stmt->then_stmt = if_stmt->else_stmt;
                    if_stmt->else_stmt = NULL;
                    count++;
                }
            }
        }
    }

    if (node->type == AST_BLOCK_STMT) {
        BlockStmtNode* block = (BlockStmtNode*)node;
        ASTNode* stmt = block->statements;
        while (stmt) {
            count += opt_dead_code_eliminate(stmt);
            stmt = stmt->next;
        }
    }

    return count;
}

// =============================================================================
// Comptime Block Processing
// =============================================================================

size_t opt_comptime_process(ASTNode* node, ComptimeInterpreter* interp) {
    if (!node || !interp) return 0;
    size_t count = 0;

    if (node->type == AST_COMPTIME_BLOCK) {
        ComptimeBlockNode* block = (ComptimeBlockNode*)node;
        if (comptime_process_block(interp, block)) {
            count++;
        }
    }

    // Recurse into blocks
    if (node->type == AST_BLOCK_STMT) {
        BlockStmtNode* block = (BlockStmtNode*)node;
        ASTNode* stmt = block->statements;
        while (stmt) {
            count += opt_comptime_process(stmt, interp);
            stmt = stmt->next;
        }
    }

    // Recurse into function bodies
    if (node->type == AST_FUNC_DECL) {
        FuncDeclNode* func = (FuncDeclNode*)node;
        if (func->is_comptime) {
            // Register comptime function for later use
            comptime_register_function(interp, func->name, func->params, func->body);
            count++;
        } else if (func->body) {
            count += opt_comptime_process(func->body, interp);
        }
    }

    if (node->next) {
        count += opt_comptime_process(node->next, interp);
    }

    return count;
}

// =============================================================================
// Run All Passes
// =============================================================================

size_t optimizer_run(Optimizer* opt, ASTNode* root) {
    if (!opt || !root) return 0;
    size_t total = 0;

    for (size_t i = 0; i < opt->pass_count; i++) {
        OptimizationPass* pass = &opt->passes[i];
        if (!pass->enabled) continue;

        size_t applied = 0;

        switch (pass->kind) {
            case OPT_PASS_COMPTIME_EVALUATE:
                applied = opt_comptime_process(root, opt->comptime);
                opt->stats.comptime_blocks_processed += applied;
                break;
            case OPT_PASS_CONSTANT_FOLD:
                applied = opt_constant_fold(root, opt->comptime);
                opt->stats.constants_folded += applied;
                break;
            case OPT_PASS_DEAD_CODE_ELIMINATE:
                applied = opt_dead_code_eliminate(root);
                opt->stats.dead_code_eliminated += applied;
                break;
            case OPT_PASS_CONSTANT_PROPAGATE:
            case OPT_PASS_INLINE_SMALL:
            case OPT_PASS_LOOP_UNROLL:
            case OPT_PASS_LOOP_VECTORIZE:
            case OPT_PASS_TAIL_CALL:
            case OPT_PASS_ESCAPE_BASED_ALLOC:
                // These delegate to LLVM passes during codegen
                break;
        }

        total += applied;
    }

    opt->stats.total_optimizations = total;
    return total;
}

// =============================================================================
// Annotation Parsing
// =============================================================================

bool optimizer_parse_optimize_for(const char* args, OptimizationGoal* out_goal) {
    if (!args || !out_goal) return false;

    *out_goal = OPT_GOAL_DEFAULT;

    if (strcmp(args, "throughput") == 0) {
        *out_goal = OPT_GOAL_THROUGHPUT;
    } else if (strcmp(args, "latency") == 0) {
        *out_goal = OPT_GOAL_LATENCY;
    } else if (strcmp(args, "memory") == 0) {
        *out_goal = OPT_GOAL_MEMORY;
    } else if (strcmp(args, "energy") == 0) {
        *out_goal = OPT_GOAL_ENERGY;
    } else if (strcmp(args, "size") == 0 || strcmp(args, "code_size") == 0) {
        *out_goal = OPT_GOAL_CODE_SIZE;
    } else {
        return false;
    }

    return true;
}
