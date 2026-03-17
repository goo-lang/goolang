#ifndef GOO_OPTIMIZER_H
#define GOO_OPTIMIZER_H

#include "ast.h"
#include "types.h"
#include "comptime.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// Optimization Goals — what to optimize for
// =============================================================================

typedef enum {
    OPT_GOAL_DEFAULT     = 0,
    OPT_GOAL_THROUGHPUT  = 1 << 0,
    OPT_GOAL_LATENCY     = 1 << 1,
    OPT_GOAL_MEMORY      = 1 << 2,
    OPT_GOAL_ENERGY      = 1 << 3,
    OPT_GOAL_CODE_SIZE   = 1 << 4,
} OptimizationGoal;

// =============================================================================
// Target Hardware Capabilities
// =============================================================================

typedef struct TargetInfo {
    // CPU features
    bool has_sse;
    bool has_sse2;
    bool has_avx;
    bool has_avx2;
    bool has_avx512;
    bool has_neon;                 // ARM SIMD

    // Hardware accelerators
    bool has_gpu;
    bool has_dedicated_crypto;
    bool has_tensor_core;

    // Architecture
    const char* arch;              // "x86_64", "aarch64", etc.
    const char* os;                // "linux", "darwin", "windows"
    size_t pointer_size;           // 4 or 8
    size_t cache_line_size;        // Typically 64
    size_t num_cores;

    // LLVM target triple
    const char* target_triple;
} TargetInfo;

// =============================================================================
// Optimization Pass
// =============================================================================

typedef enum {
    OPT_PASS_CONSTANT_FOLD,        // Fold constant expressions
    OPT_PASS_CONSTANT_PROPAGATE,   // Propagate constants across boundaries
    OPT_PASS_DEAD_CODE_ELIMINATE,  // Remove unreachable code
    OPT_PASS_INLINE_SMALL,         // Inline small functions
    OPT_PASS_LOOP_UNROLL,          // Unroll short loops
    OPT_PASS_LOOP_VECTORIZE,       // Auto-vectorize loops
    OPT_PASS_TAIL_CALL,            // Tail call optimization
    OPT_PASS_ESCAPE_BASED_ALLOC,   // Stack-allocate non-escaping values
    OPT_PASS_COMPTIME_EVALUATE,    // Process comptime blocks
} OptPassKind;

typedef struct OptimizationPass {
    OptPassKind kind;
    const char* name;
    bool enabled;

    // Pass function: returns number of optimizations applied
    size_t (*run)(struct OptimizationPass* pass, ASTNode* root, void* context);

    // Configuration
    int inline_threshold;          // Max instructions for inlining
    int unroll_factor;             // Loop unroll factor
    int vectorize_width;           // SIMD vector width
} OptimizationPass;

// =============================================================================
// Optimizer — manages optimization passes
// =============================================================================

typedef struct Optimizer {
    OptimizationPass* passes;
    size_t pass_count;
    size_t pass_capacity;

    OptimizationGoal goals;
    TargetInfo target;

    // Comptime integration
    ComptimeInterpreter* comptime;

    // Statistics
    struct {
        size_t constants_folded;
        size_t constants_propagated;
        size_t dead_code_eliminated;
        size_t functions_inlined;
        size_t loops_unrolled;
        size_t loops_vectorized;
        size_t comptime_blocks_processed;
        size_t total_optimizations;
    } stats;
} Optimizer;

// =============================================================================
// Comptime Intrinsics — built-in functions for comptime blocks
// =============================================================================

// Hardware detection intrinsics for comptime
bool comptime_target_has(const TargetInfo* target, const char* feature);

// Type intrinsics
size_t comptime_sizeof_type(Type* type);
size_t comptime_alignof_type(Type* type);
bool comptime_type_is_numeric(Type* type);
bool comptime_type_is_vectorizable(Type* type);

// =============================================================================
// API
// =============================================================================

// Lifecycle
Optimizer* optimizer_new(OptimizationGoal goals);
void optimizer_free(Optimizer* optimizer);

// Target configuration
void optimizer_set_target(Optimizer* opt, const TargetInfo* target);
TargetInfo optimizer_detect_host_target(void);

// Pass management
void optimizer_add_pass(Optimizer* opt, OptPassKind kind);
void optimizer_add_default_passes(Optimizer* opt);

// Run all passes
size_t optimizer_run(Optimizer* opt, ASTNode* root);

// Individual optimization functions
size_t opt_constant_fold(ASTNode* node, ComptimeInterpreter* interp);
size_t opt_dead_code_eliminate(ASTNode* node);
size_t opt_comptime_process(ASTNode* node, ComptimeInterpreter* interp);

// Annotation parsing
bool optimizer_parse_optimize_for(const char* args, OptimizationGoal* out_goal);

#endif // GOO_OPTIMIZER_H
