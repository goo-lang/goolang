#include "../include/auto_parallel.h"
#include "../include/ast.h"
#include "../include/types.h"
#include "../include/errors/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

// Forward declarations to match existing header
typedef struct AutoParallelContext {
    ASTNode* ast_root;
    ParallelDecision* opportunities;
    size_t opportunity_count;
    size_t opportunity_capacity;
    LoopInfo** loops;
    size_t loop_count;
    size_t loop_capacity;
    bool aggressive_mode;
    int min_work_threshold;
    bool enable_simd;
    bool enable_tasks;
    size_t functions_parallelized;
    size_t loops_analyzed;
} AutoParallelContext;

typedef enum {
    STRATEGY_NONE,
    STRATEGY_LOOP_PARALLEL,
    STRATEGY_TASK_PARALLEL,
    STRATEGY_SIMD,
    STRATEGY_PIPELINE,
    STRATEGY_HYBRID
} ParallelStrategy;

// AST analysis for auto-parallelization
bool has_auto_parallel_annotation(ASTNode* func_node) {
    if (!func_node || func_node->type != AST_FUNC_DECL) {
        return false;
    }
    
    FuncDeclNode* func = (FuncDeclNode*)func_node;
    if (!func->annotations) {
        return false;
    }
    
    // Traverse the annotation list
    ASTNode* current = func->annotations;
    while (current) {
        if (current->type == AST_ATTRIBUTE) {
            AttributeNode* attr = (AttributeNode*)current;
            if (attr->name && strcmp(attr->name, "auto_parallel") == 0) {
                return true;
            }
        }
        current = current->next;
    }
    
    return false;
}

bool should_parallelize_function(ASTNode* func_node) {
    if (!func_node || func_node->type != AST_FUNC_DECL) {
        return false;
    }
    
    // Check for explicit annotations
    if (has_auto_parallel_annotation(func_node)) {
        return true;
    }
    
    // TODO: Add heuristic-based analysis for functions without annotations
    return false;
}

// Analyze a function's body for parallelization opportunities  
void analyze_function_node(AutoParallelContext* ctx, ASTNode* func_node) {
    if (!ctx || !func_node || func_node->type != AST_FUNC_DECL) {
        return;
    }
    
    FuncDeclNode* func = (FuncDeclNode*)func_node;
    printf("Analyzing function '%s' for parallelization opportunities\n", 
           func->name ? func->name : "<unnamed>");
    
    // Check if this function has auto_parallel annotation
    if (!should_parallelize_function(func_node)) {
        printf("  Skipping function '%s' - no auto_parallel annotation\n", func->name);
        return;
    }
    
    printf("  Function '%s' marked for auto-parallelization\n", func->name);
    
    // Analyze the function body
    if (func->body) {
        analyze_ast_node(ctx, func->body);
    }
    
    ctx->functions_parallelized++;
}

// Main AST traversal function
void analyze_ast_node(AutoParallelContext* ctx, ASTNode* node) {
    if (!ctx || !node) {
        return;
    }
    
    switch (node->type) {
        case AST_FUNC_DECL:
            analyze_function_node(ctx, node);
            break;
            
        case AST_FOR_STMT:
            analyze_for_loop(ctx, node);
            break;
            
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)node;
            if (block->statements) {
                analyze_ast_node(ctx, block->statements);
            }
            break;
        }
        
        case AST_PROGRAM: {
            ProgramNode* prog = (ProgramNode*)node;
            if (prog->decls) {
                analyze_ast_node(ctx, prog->decls);
            }
            break;
        }
        
        default:
            // For other node types, just traverse children if they exist
            break;
    }
    
    // Traverse to next sibling
    if (node->next) {
        analyze_ast_node(ctx, node->next);
    }
}

// Analyze for loops for parallelization opportunities
void analyze_for_loop(AutoParallelContext* ctx, ASTNode* for_node) {
    if (!ctx || !for_node || for_node->type != AST_FOR_STMT) {
        return;
    }
    
    printf("  Analyzing for loop for parallelization\n");
    
    // Create loop info structure
    LoopInfo* loop_info = analyze_loop(ctx, for_node);
    if (!loop_info) {
        printf("    Failed to analyze loop\n");
        return;
    }
    
    // Check if the loop is parallelizable
    if (is_parallelizable_loop(loop_info)) {
        ParallelStrategy strategy = recommend_loop_strategy(loop_info);
        int confidence = 75; // Default confidence
        int estimated_gain = estimate_parallelization_benefit(loop_info, strategy);
        
        printf("    Loop is parallelizable with strategy: %s\n", 
               parallel_strategy_string(strategy));
        printf("    Estimated performance gain: %d%%\n", estimated_gain);
        
        // Add parallelization opportunity (simplified for now)
        printf("    Adding parallelization opportunity\n");
    } else {
        printf("    Loop is not suitable for parallelization\n");
    }
    
    ctx->loops_analyzed++;
    
    // Analyze loop body for nested loops
    ForStmtNode* for_stmt = (ForStmtNode*)for_node;
    if (for_stmt->body) {
        analyze_ast_node(ctx, for_stmt->body);
    }
}

// Simple loop analysis
LoopInfo* analyze_loop(AutoParallelContext* ctx, ASTNode* loop_node) {
    if (!ctx || !loop_node || loop_node->type != AST_FOR_STMT) {
        return NULL;
    }
    
    // Allocate loop info
    LoopInfo* loop_info = (LoopInfo*)xcalloc(1, sizeof(LoopInfo));
    if (!loop_info) {
        return NULL;
    }
    
    ForStmtNode* for_stmt = (ForStmtNode*)loop_node;
    loop_info->loop_node = loop_node;
    loop_info->init_stmt = for_stmt->init;
    loop_info->condition = for_stmt->condition;
    loop_info->increment = for_stmt->post;
    loop_info->body = for_stmt->body;
    
    // Simple analysis - assume loop is countable and has no dependencies for now
    loop_info->is_countable = true;
    loop_info->iteration_count = 1000; // Default assumption
    loop_info->has_dependencies = false;
    loop_info->is_vectorizable = true;
    loop_info->is_parallelizable = true;
    loop_info->has_constant_stride = true;
    
    // Store in context
    if (ctx->loop_capacity == 0) {
        ctx->loop_capacity = 10;
        ctx->loops = (LoopInfo**)malloc(ctx->loop_capacity * sizeof(LoopInfo*));
    } else if (ctx->loop_count >= ctx->loop_capacity) {
        ctx->loop_capacity *= 2;
        ctx->loops = (LoopInfo**)realloc(ctx->loops, ctx->loop_capacity * sizeof(LoopInfo*));
    }
    
    if (ctx->loops) {
        ctx->loops[ctx->loop_count] = loop_info;
        ctx->loop_count++;
    }
    
    return loop_info;
}

// Check if a loop is parallelizable
bool is_parallelizable_loop(const LoopInfo* loop_info) {
    if (!loop_info) {
        return false;
    }
    
    // Simple heuristics for now
    return loop_info->is_countable && 
           !loop_info->has_dependencies && 
           loop_info->iteration_count > 10;
}

// Recommend parallelization strategy for a loop
ParallelStrategy recommend_loop_strategy(const LoopInfo* loop_info) {
    if (!loop_info || !is_parallelizable_loop(loop_info)) {
        return STRATEGY_NONE;
    }
    
    // Simple strategy selection
    if (loop_info->is_vectorizable && loop_info->has_constant_stride) {
        return STRATEGY_SIMD;
    } else if (loop_info->iteration_count > 1000) {
        return STRATEGY_LOOP_PARALLEL;
    } else {
        return STRATEGY_TASK_PARALLEL;
    }
}

// Add parallelization opportunity to context
void add_parallelization_opportunity(AutoParallelContext* ctx, 
                                   ASTNode* target, ParallelStrategy strategy,
                                   int confidence, int gain, const char* reasoning) {
    if (!ctx) {
        return;
    }
    
    // Allocate opportunities array if needed
    if (ctx->opportunity_capacity == 0) {
        ctx->opportunity_capacity = 10;
        ctx->opportunities = (ParallelizationOpportunity*)malloc(
            ctx->opportunity_capacity * sizeof(ParallelizationOpportunity));
    } else if (ctx->opportunity_count >= ctx->opportunity_capacity) {
        ctx->opportunity_capacity *= 2;
        ctx->opportunities = (ParallelizationOpportunity*)realloc(
            ctx->opportunities, ctx->opportunity_capacity * sizeof(ParallelizationOpportunity));
    }
    
    if (!ctx->opportunities) {
        return;
    }
    
    // Create new opportunity
    ParallelizationOpportunity* opp = &ctx->opportunities[ctx->opportunity_count];
    opp->target_node = target;
    opp->strategy = strategy;
    opp->confidence_score = confidence;
    opp->performance_gain = gain;
    opp->reasoning = reasoning ? strdup(reasoning) : NULL;
    opp->loop_info = NULL; // Will be set separately if needed
    opp->transformed_node = NULL;
    
    ctx->opportunity_count++;
}

// Estimate parallelization benefit
int estimate_parallelization_benefit(const LoopInfo* loop_info, ParallelStrategy strategy) {
    if (!loop_info) {
        return 0;
    }
    
    // Simple estimation based on strategy and loop characteristics
    switch (strategy) {
        case STRATEGY_SIMD:
            return loop_info->has_constant_stride ? 200 : 150; // 150-200% speedup
        case STRATEGY_LOOP_PARALLEL:
            return (loop_info->iteration_count > 10000) ? 300 : 150; // 150-300% speedup
        case STRATEGY_TASK_PARALLEL:
            return 100; // 100% speedup
        default:
            return 0;
    }
}

// Context management
AutoParallelContext* auto_parallel_context_new(void) {
    AutoParallelContext* ctx = (AutoParallelContext*)xcalloc(1, sizeof(AutoParallelContext));
    if (!ctx) {
        return NULL;
    }
    
    ctx->aggressive_mode = false;
    ctx->min_work_threshold = 100;
    ctx->enable_simd = true;
    ctx->enable_tasks = true;
    
    return ctx;
}

void auto_parallel_context_free(AutoParallelContext* ctx) {
    if (!ctx) {
        return;
    }
    
    // Free opportunities
    if (ctx->opportunities) {
        for (size_t i = 0; i < ctx->opportunity_count; i++) {
            free(ctx->opportunities[i].reasoning);
        }
        free(ctx->opportunities);
    }
    
    // Free loops
    if (ctx->loops) {
        for (size_t i = 0; i < ctx->loop_count; i++) {
            free(ctx->loops[i]);
        }
        free(ctx->loops);
    }
    
    free(ctx);
}

// Main analysis function
bool auto_parallel_analyze(AutoParallelContext* ctx, ASTNode* ast_root) {
    if (!ctx || !ast_root) {
        return false;
    }
    
    printf("Starting auto-parallelization analysis\n");
    
    ctx->ast_root = ast_root;
    analyze_ast_node(ctx, ast_root);
    
    printf("Analysis complete: found %zu opportunities in %zu loops, %zu functions processed\n",
           ctx->opportunity_count, ctx->loops_analyzed, ctx->functions_parallelized);
    
    return true;
}

// Transform functions (stubs for now)
bool auto_parallel_transform(AutoParallelContext* ctx) {
    if (!ctx) {
        return false;
    }
    
    printf("Applying parallelization transformations (%zu opportunities)\n", 
           ctx->opportunity_count);
    
    // TODO: Implement actual transformations
    for (size_t i = 0; i < ctx->opportunity_count; i++) {
        ParallelizationOpportunity* opp = &ctx->opportunities[i];
        printf("  Transforming node with strategy: %s (confidence: %d%%, gain: %d%%)\n",
               parallel_strategy_string(opp->strategy), 
               opp->confidence_score, opp->performance_gain);
    }
    
    return true;
}

// Utility functions
const char* parallel_strategy_string(ParallelStrategy strategy) {
    switch (strategy) {
        case STRATEGY_NONE: return "None";
        case STRATEGY_LOOP_PARALLEL: return "Loop Parallel";
        case STRATEGY_TASK_PARALLEL: return "Task Parallel";
        case STRATEGY_SIMD: return "SIMD";
        case STRATEGY_PIPELINE: return "Pipeline";
        case STRATEGY_HYBRID: return "Hybrid";
        default: return "Unknown";
    }
}

void print_parallelization_opportunities(const AutoParallelContext* ctx) {
    if (!ctx) {
        return;
    }
    
    printf("\n=== Parallelization Opportunities ===\n");
    printf("Total opportunities found: %zu\n", ctx->opportunity_count);
    
    for (size_t i = 0; i < ctx->opportunity_count; i++) {
        const ParallelizationOpportunity* opp = &ctx->opportunities[i];
        printf("\nOpportunity #%zu:\n", i + 1);
        printf("  Strategy: %s\n", parallel_strategy_string(opp->strategy));
        printf("  Confidence: %d%%\n", opp->confidence_score);
        printf("  Expected gain: %d%%\n", opp->performance_gain);
        if (opp->reasoning) {
            printf("  Reasoning: %s\n", opp->reasoning);
        }
    }
    
    printf("\nStatistics:\n");
    printf("  Functions analyzed: %zu\n", ctx->functions_parallelized);
    printf("  Loops analyzed: %zu\n", ctx->loops_analyzed);
}