#ifndef GOO_AUTO_PARALLEL_H
#define GOO_AUTO_PARALLEL_H

#include "ast.h"
#include "types.h"
#include "optimizer.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// Data Dependency Analysis
// =============================================================================

typedef enum {
    DEP_NONE,                      // No dependency
    DEP_FLOW,                      // Read-after-write (true dependency)
    DEP_ANTI,                      // Write-after-read
    DEP_OUTPUT,                    // Write-after-write
    DEP_REDUCTION,                 // Associative reduction (parallelizable)
} DependencyKind;

typedef struct DataDependency {
    DependencyKind kind;
    const char* variable;          // Variable involved
    int source_iteration;          // Source iteration (-1 = unknown)
    int target_iteration;          // Target iteration
    bool is_loop_carried;          // Dependency spans loop iterations
    struct DataDependency* next;
} DataDependency;

// =============================================================================
// Loop Analysis
// =============================================================================

typedef enum {
    LOOP_SEQUENTIAL,               // Cannot be parallelized
    LOOP_PARALLEL,                 // Fully parallelizable (no dependencies)
    LOOP_REDUCTION,                // Parallelizable with reduction
    LOOP_PARTIALLY_PARALLEL,       // Some iterations are independent
    LOOP_VECTORIZABLE,             // Can use SIMD
} LoopParallelKind;

typedef enum {
    REDUCE_SUM,
    REDUCE_PRODUCT,
    REDUCE_MIN,
    REDUCE_MAX,
    REDUCE_AND,
    REDUCE_OR,
    REDUCE_NONE,
} ReductionKind;

typedef struct LoopAnalysis {
    LoopParallelKind parallel_kind;
    ReductionKind reduction;
    const char* reduction_var;     // Variable being reduced
    const char* induction_var;     // Loop induction variable

    // Iteration space
    int64_t trip_count;            // Number of iterations (-1 = unknown)
    bool has_constant_bounds;

    // Dependencies
    DataDependency* dependencies;
    size_t dependency_count;

    // Cost model
    size_t estimated_work;         // Estimated work per iteration
    size_t min_parallel_iters;     // Minimum iterations for parallel benefit
    bool worth_parallelizing;

    // SIMD info
    bool is_vectorizable;
    int vector_width;              // Recommended SIMD width
} LoopAnalysis;

// =============================================================================
// Parallelization Plan
// =============================================================================

typedef enum {
    PAR_STRATEGY_NONE,
    PAR_STRATEGY_PARALLEL_FOR,     // OpenMP-style parallel for
    PAR_STRATEGY_SIMD,             // SIMD vectorization
    PAR_STRATEGY_TASK_SPAWN,       // Task-based parallelism
    PAR_STRATEGY_REDUCTION,        // Parallel reduction
    PAR_STRATEGY_PIPELINE,         // Pipeline parallelism
} ParallelStrategy;

typedef struct ParallelPlan {
    ParallelStrategy strategy;
    ASTNode* target_node;          // AST node to parallelize
    LoopAnalysis analysis;

    // Configuration
    size_t num_threads;            // Recommended thread count
    size_t chunk_size;             // Work chunk size

    struct ParallelPlan* next;
} ParallelPlan;

// =============================================================================
// Auto-Parallelizer
// =============================================================================

typedef struct AutoParallelizer {
    TargetInfo target;

    // Analysis results
    ParallelPlan* plans;
    size_t plan_count;

    // Configuration
    size_t min_trip_count;         // Minimum iterations to consider
    size_t min_work_estimate;      // Minimum work per iteration
    bool prefer_simd;              // Prefer SIMD over threading
    bool allow_speculative;        // Allow speculative parallelization

    // Statistics
    struct {
        size_t loops_analyzed;
        size_t loops_parallelized;
        size_t loops_vectorized;
        size_t reductions_detected;
        size_t dependencies_found;
    } stats;
} AutoParallelizer;

// =============================================================================
// API
// =============================================================================

// Lifecycle
AutoParallelizer* auto_parallel_new(const TargetInfo* target);
void auto_parallel_free(AutoParallelizer* ap);

// Analysis
LoopAnalysis auto_parallel_analyze_loop(AutoParallelizer* ap, ASTNode* for_stmt);
bool auto_parallel_has_loop_carried_deps(DataDependency* deps);
ReductionKind auto_parallel_detect_reduction(ASTNode* loop_body, const char* induction_var);

// Planning
ParallelStrategy auto_parallel_choose_strategy(const LoopAnalysis* analysis,
                                                const TargetInfo* target);
ParallelPlan* auto_parallel_plan_function(AutoParallelizer* ap, ASTNode* func);

// Dependency analysis
DataDependency* dependency_analyze_loop_body(ASTNode* body, const char* induction_var);
void dependency_list_free(DataDependency* deps);

// Annotation parsing
bool auto_parallel_parse_annotation(const char* args, size_t* min_iters, size_t* chunk_size);

#endif // GOO_AUTO_PARALLEL_H
