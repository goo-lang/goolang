#ifndef ADVANCED_OPTIMIZATION_H
#define ADVANCED_OPTIMIZATION_H

#include "ast.h"
#include "types.h"
#include "optimization.h"
#include "profile_guided_optimization.h"
#include <stdbool.h>

// Advanced optimization strategy types
typedef enum {
    STRATEGY_VECTORIZATION,
    STRATEGY_INLINING,
    STRATEGY_ESCAPE_ANALYSIS,
    STRATEGY_DEVIRTUALIZATION,
    STRATEGY_LOOP_UNROLLING,
    STRATEGY_PREFETCHING,
    STRATEGY_BRANCH_PREDICTION,
    STRATEGY_COUNT
} AdvancedStrategyType;

// Vectorization analysis
typedef struct {
    bool can_vectorize;
    int vector_width;
    bool requires_alignment;
    bool has_dependencies;
    char* dependency_info;
} VectorizationAnalysis;

// Inlining analysis
typedef struct {
    bool should_inline;
    int call_frequency;
    int function_size;
    int inline_cost;
    char* inline_reason;
} InliningAnalysis;

// Escape analysis result
typedef struct {
    bool escapes;
    bool stack_allocatable;
    bool needs_heap;
    char* escape_path;
} EscapeAnalysis;

// Loop optimization hints
typedef struct {
    bool can_unroll;
    int unroll_factor;
    bool vectorizable;
    bool has_invariants;
    char* optimization_notes;
} LoopOptimization;

// Advanced optimization strategy
typedef struct {
    AdvancedStrategyType type;
    char* strategy_name;
    double confidence_score;
    char* analysis_data;
    union {
        VectorizationAnalysis vectorization;
        InliningAnalysis inlining;
        EscapeAnalysis escape;
        LoopOptimization loop;
    } strategy_data;
} AdvancedStrategy;

// Strategy manager
typedef struct {
    AdvancedStrategy* strategies;
    int strategy_count;
    int capacity;
    ProfileData* pgo_data;  // Integration with PGO
} StrategyManager;

// Function declarations

// Strategy manager
StrategyManager* create_strategy_manager(void);
void destroy_strategy_manager(StrategyManager* manager);
void integrate_pgo_data(StrategyManager* manager, ProfileData* pgo_data);

// Strategy analysis
AdvancedStrategy* analyze_vectorization(ASTNode* node);
AdvancedStrategy* analyze_inlining(ASTNode* function_node, int call_frequency);
AdvancedStrategy* analyze_escape(ASTNode* allocation_node);
AdvancedStrategy* analyze_loop_optimization(ASTNode* loop_node);

// Strategy application
bool apply_vectorization_strategy(ASTNode* node, VectorizationAnalysis* analysis);
bool apply_inlining_strategy(ASTNode* function_node, InliningAnalysis* analysis);
bool apply_escape_strategy(ASTNode* allocation_node, EscapeAnalysis* analysis);
bool apply_loop_strategy(ASTNode* loop_node, LoopOptimization* optimization);

// Integration with existing systems
void integrate_with_optimization_directives(StrategyManager* manager, OptimizationContext* context);
void update_strategies_from_profile(StrategyManager* manager, ProfileData* profile);

// Strategy reporting
void print_strategy_report(StrategyManager* manager);
char* get_strategy_summary(StrategyManager* manager);

// Compile-time intrinsics for advanced optimization
ComptimeValue* comptime_get_optimization_strategy(ComptimeValue* target, ComptimeValue* strategy_type);
ComptimeValue* comptime_force_vectorization(ComptimeValue* target, ComptimeValue* vector_width);
ComptimeValue* comptime_inline_aggressively(ComptimeValue* target);
ComptimeValue* comptime_stack_allocate(ComptimeValue* target);

#endif // ADVANCED_OPTIMIZATION_H
