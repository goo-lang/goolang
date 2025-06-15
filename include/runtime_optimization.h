#ifndef RUNTIME_OPTIMIZATION_H
#define RUNTIME_OPTIMIZATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ast.h"
#include "types.h"
#include "contracts.h"
#include "proof_generation.h"
#include "memory_safety.h"

// C23 compatibility
_Static_assert(sizeof(bool) == 1, "bool should be 1 byte");

// =============================================================================
// Runtime Optimization Framework Types
// =============================================================================

// Types of runtime optimizations
typedef enum {
    OPT_BOUNDS_CHECK_ELIMINATION = 0, // Remove provably safe bounds checks
    OPT_NULL_CHECK_ELIMINATION,       // Remove provably safe null checks
    OPT_BRANCH_PREDICTION,            // Optimize branches based on proofs
    OPT_LOOP_OPTIMIZATION,            // Optimize loops with proven invariants
    OPT_MEMORY_PREFETCH,              // Hardware-assisted memory prefetching
    OPT_SPECULATIVE_EXECUTION,        // Safe speculative execution
    OPT_HARDWARE_VERIFICATION,        // Intel MPX, ARM Memory Tagging
    OPT_VECTORIZATION,                // SIMD optimization with safety proofs
    OPT_TYPE_COUNT
} OptimizationType;

// Optimization safety levels
typedef enum {
    OPT_SAFETY_AGGRESSIVE = 0,        // Maximum optimization, minimal checks
    OPT_SAFETY_BALANCED,              // Good performance with safety nets
    OPT_SAFETY_CONSERVATIVE,          // Maintain most runtime checks
    OPT_SAFETY_DEBUG                  // Full checking for debugging
} OptimizationSafetyLevel;

// Hardware capability flags
typedef enum {
    HW_CAP_NONE = 0x0,
    HW_CAP_INTEL_MPX = 0x1,           // Intel Memory Protection Extensions
    HW_CAP_ARM_MTE = 0x2,             // ARM Memory Tagging Extensions
    HW_CAP_INTEL_CET = 0x4,           // Intel Control-flow Enforcement
    HW_CAP_ARM_BTI = 0x8,             // ARM Branch Target Identification
    HW_CAP_AVX512 = 0x10,             // AVX-512 vectorization
    HW_CAP_NEON = 0x20,               // ARM NEON vectorization
    HW_CAP_PREFETCH = 0x40,           // Hardware prefetching support
    HW_CAP_SPECULATION = 0x80         // Safe speculation support
} HardwareCapabilities;

// Optimization context
typedef struct OptimizationContext {
    OptimizationSafetyLevel safety_level;
    HardwareCapabilities hw_capabilities;
    struct ProofGenerationContext* proof_ctx;          // Link to proof generation
    struct ContractContext* contract_ctx;    // Link to contract system
    
    // Statistics
    size_t bounds_checks_eliminated;
    size_t null_checks_eliminated;
    size_t branches_optimized;
    size_t loops_optimized;
    size_t speculation_hits;
    size_t speculation_misses;
    
    // Configuration
    bool enable_speculation;
    bool enable_vectorization;
    bool enable_prefetch;
    bool enable_hardware_assists;
    
    // Runtime profiling data
    struct {
        uint64_t* branch_frequencies;    // Per-branch hit counts
        uint64_t* loop_iteration_counts; // Per-loop iteration stats
        uint64_t* function_call_counts;  // Per-function call stats
        size_t profile_data_size;
    } profiling;
    
    // Error handling
    char last_error[256];
    int error_count;
} OptimizationContext;

// Bounds check elimination analysis
typedef struct BoundsCheckInfo {
    struct ASTNode* index_expr;       // The index expression
    struct ASTNode* bounds_expr;      // The bounds expression
    struct ASTNode* loop_context;     // Optional loop context
    struct ProofReport* safety_proof;        // Proof that check is safe to eliminate
    bool can_eliminate;               // Final decision
    char* elimination_reason;         // Why it can/cannot be eliminated
} BoundsCheckInfo;

// Branch prediction optimization
typedef struct BranchInfo {
    struct ASTNode* condition;        // Branch condition
    ContractExpression* proven_invariant; // Proven invariant affecting branch
    double predicted_probability;     // Predicted branch probability
    uint64_t hit_count;              // Runtime hit count
    uint64_t miss_count;             // Runtime miss count
    bool is_predictable;             // Can be statically predicted
} BranchInfo;

// Loop optimization information
typedef struct LoopOptInfo {
    struct ASTNode* loop_node;        // The loop AST node
    DependentType* induction_variable; // Loop induction variable
    ContractExpression* invariant;   // Proven loop invariant
    struct ProofReport* termination_proof;  // Termination proof
    
    // Optimization decisions
    bool can_vectorize;              // SIMD vectorization possible
    bool can_unroll;                 // Loop unrolling beneficial
    bool can_prefetch;               // Memory prefetching beneficial
    size_t unroll_factor;            // Suggested unroll factor
    size_t vector_width;             // Suggested vector width
} LoopOptInfo;

// Speculative execution context
typedef struct SpeculationContext {
    struct ASTNode* speculation_point; // Where speculation begins
    ContractExpression* speculation_guard; // Condition that must hold
    struct ProofReport* rollback_safety;    // Proof that rollback is safe
    
    // Speculation state
    bool is_speculating;             // Currently in speculative mode
    void* checkpoint_state;          // Saved state for rollback
    size_t checkpoint_size;          // Size of saved state
    
    // Statistics
    uint64_t speculation_attempts;
    uint64_t speculation_successes;
    uint64_t rollback_count;
} SpeculationContext;

// Hardware-assisted verification
typedef struct HardwareVerifier {
    HardwareCapabilities available_features;
    
    // Intel MPX support
    struct {
        bool enabled;
        void* (*setup_bounds)(void* ptr, size_t size);
        bool (*check_bounds)(void* ptr, void* bounds);
        void (*clear_bounds)(void* bounds);
    } mpx;
    
    // ARM Memory Tagging support
    struct {
        bool enabled;
        uint8_t (*tag_memory)(void* ptr, size_t size);
        bool (*check_tag)(void* ptr, uint8_t expected_tag);
        void (*clear_tag)(void* ptr, size_t size);
    } mte;
    
    // Control-flow integrity
    struct {
        bool enabled;
        void (*enable_cfi)(void);
        void (*disable_cfi)(void);
        bool (*verify_target)(void* target);
    } cfi;
} HardwareVerifier;

// =============================================================================
// Core Functions
// =============================================================================

// Context management
OptimizationContext* optimization_context_new(OptimizationSafetyLevel safety_level);
void optimization_context_free(OptimizationContext* ctx);
int optimization_context_set_proofs(OptimizationContext* ctx, struct ProofGenerationContext* proof_ctx);
int optimization_context_set_contracts(OptimizationContext* ctx, struct ContractContext* contract_ctx);

// Hardware capability detection
HardwareCapabilities detect_hardware_capabilities(void);
int enable_hardware_verification(OptimizationContext* ctx, HardwareCapabilities caps);
HardwareVerifier* hardware_verifier_new(HardwareCapabilities caps);
void hardware_verifier_free(HardwareVerifier* verifier);

// Bounds check elimination
BoundsCheckInfo* analyze_bounds_check(OptimizationContext* ctx, struct ASTNode* index_access);
// Bounds check elimination - renamed to avoid conflict
bool can_eliminate_runtime_bounds_check(OptimizationContext* ctx, BoundsCheckInfo* info);
int eliminate_bounds_check(OptimizationContext* ctx, BoundsCheckInfo* info);
void bounds_check_info_free(BoundsCheckInfo* info);

// Null check elimination
bool can_eliminate_null_check(OptimizationContext* ctx, struct ASTNode* pointer_access);
int eliminate_null_check(OptimizationContext* ctx, struct ASTNode* pointer_access);

// Branch prediction optimization
BranchInfo* analyze_branch(OptimizationContext* ctx, struct ASTNode* branch_node);
int optimize_branch_prediction(OptimizationContext* ctx, BranchInfo* info);
void branch_info_free(BranchInfo* info);

// Loop optimization
LoopOptInfo* analyze_loop(OptimizationContext* ctx, struct ASTNode* loop_node);
int optimize_loop(OptimizationContext* ctx, LoopOptInfo* info);
void loop_opt_info_free(LoopOptInfo* info);

// Speculative execution
SpeculationContext* speculation_context_new(struct ASTNode* speculation_point);
void speculation_context_free(SpeculationContext* spec_ctx);
int begin_speculation(OptimizationContext* ctx, SpeculationContext* spec_ctx);
int commit_speculation(OptimizationContext* ctx, SpeculationContext* spec_ctx);
int rollback_speculation(OptimizationContext* ctx, SpeculationContext* spec_ctx);

// Vectorization with safety proofs
bool can_vectorize_loop(OptimizationContext* ctx, struct ASTNode* loop_node);
int vectorize_loop(OptimizationContext* ctx, struct ASTNode* loop_node, size_t vector_width);

// Memory prefetching
int enable_memory_prefetch(OptimizationContext* ctx, struct ASTNode* memory_access);
int predict_memory_pattern(OptimizationContext* ctx, struct ASTNode* access_pattern);

// =============================================================================
// Integration with Existing Systems
// =============================================================================

// Integration with proof generation
int link_optimization_to_proofs(OptimizationContext* opt_ctx, struct ProofGenerationContext* proof_ctx);
struct ProofReport* generate_optimization_safety_proof(OptimizationContext* ctx, 
                                               OptimizationType opt_type,
                                               struct ASTNode* target);

// Integration with contracts
int validate_optimization_contracts(OptimizationContext* ctx, struct ASTNode* node);
ContractExpression* create_optimization_contract(OptimizationContext* ctx,
                                                OptimizationType opt_type,
                                                struct ASTNode* target);

// Integration with memory safety
int verify_optimization_memory_safety(OptimizationContext* ctx, struct ASTNode* node);

// =============================================================================
// Runtime Profiling and Feedback
// =============================================================================

// Profile-guided optimization
typedef struct ProfileData {
    char* function_name;
    uint64_t call_count;
    uint64_t total_cycles;
    
    struct {
        size_t branch_count;
        struct {
            uint64_t taken_count;
            uint64_t not_taken_count;
            double prediction_accuracy;
        }* branches;
    } branch_profile;
    
    struct {
        size_t loop_count;
        struct {
            uint64_t iteration_count;
            uint64_t total_iterations;
            double avg_iterations;
        }* loops;
    } loop_profile;
} ProfileData;

ProfileData* profile_data_new(const char* function_name);
void profile_data_free(ProfileData* data);
int collect_runtime_profile(OptimizationContext* ctx, const char* function_name);
int apply_profile_guided_optimization(OptimizationContext* ctx, ProfileData* data);

// Adaptive optimization
typedef struct AdaptiveOptimizer {
    OptimizationContext* ctx;
    ProfileData** profiles;
    size_t profile_count;
    
    // Adaptation parameters
    double adaptation_threshold;      // When to trigger re-optimization
    uint64_t sample_interval;        // Profiling sample frequency
    size_t optimization_history_size; // Keep track of past optimizations
    
    // State
    bool is_collecting;              // Currently collecting profile data
    uint64_t last_optimization_time; // When we last re-optimized
} AdaptiveOptimizer;

AdaptiveOptimizer* adaptive_optimizer_new(OptimizationContext* ctx);
void adaptive_optimizer_free(AdaptiveOptimizer* optimizer);
int adaptive_optimizer_update(AdaptiveOptimizer* optimizer);
int adaptive_optimizer_trigger_reoptimization(AdaptiveOptimizer* optimizer);

// =============================================================================
// Code Generation Integration
// =============================================================================

// Forward declaration for code generator
struct CodeGenerator;

// Code generation optimization hooks
int optimize_code_generation(struct CodeGenerator* codegen, OptimizationContext* ctx);
int emit_optimized_bounds_check(struct CodeGenerator* codegen, 
                               OptimizationContext* ctx,
                               BoundsCheckInfo* info);
int emit_optimized_branch(struct CodeGenerator* codegen,
                         OptimizationContext* ctx, 
                         BranchInfo* info);
int emit_vectorized_loop(struct CodeGenerator* codegen,
                        OptimizationContext* ctx,
                        LoopOptInfo* info);

// =============================================================================
// Testing and Validation
// =============================================================================

// Test optimization safety
bool test_optimization_safety(OptimizationContext* ctx, struct ASTNode* original,
                             struct ASTNode* optimized);
int validate_optimization_correctness(OptimizationContext* ctx, 
                                     OptimizationType opt_type,
                                     struct ASTNode* target);

// Benchmark optimization impact
typedef struct OptimizationBenchmark {
    char* name;
    double baseline_time;            // Time without optimization
    double optimized_time;           // Time with optimization
    double speedup_factor;           // optimized_time / baseline_time
    size_t memory_usage_baseline;
    size_t memory_usage_optimized;
    bool correctness_verified;       // Correctness check passed
} OptimizationBenchmark;

OptimizationBenchmark* benchmark_optimization(OptimizationContext* ctx,
                                             OptimizationType opt_type,
                                             struct ASTNode* target);
void optimization_benchmark_free(OptimizationBenchmark* benchmark);

// =============================================================================
// Error Handling and Diagnostics
// =============================================================================

// Optimization error types
typedef enum {
    OPT_ERROR_NONE = 0,
    OPT_ERROR_PROOF_FAILED,          // Required proof could not be generated
    OPT_ERROR_CONTRACT_VIOLATION,    // Optimization violates contract
    OPT_ERROR_HARDWARE_UNSUPPORTED,  // Hardware feature not available
    OPT_ERROR_SAFETY_VIOLATION,      // Optimization not safe at current level
    OPT_ERROR_PROFILE_INSUFFICIENT,  // Not enough profile data
    OPT_ERROR_SPECULATION_FAILED,    // Speculation rollback occurred
    OPT_ERROR_VERIFICATION_FAILED    // Hardware verification failed
} OptimizationError;

const char* optimization_error_string(OptimizationError error);
OptimizationError optimization_get_last_error(OptimizationContext* ctx);
void optimization_clear_error(OptimizationContext* ctx);

// Diagnostic information
typedef struct OptimizationDiagnostic {
    OptimizationType opt_type;
    struct ASTNode* target_node;
    OptimizationError error;
    char* message;
    char* suggestion;
    bool is_warning;                 // Warning vs error
} OptimizationDiagnostic;

OptimizationDiagnostic* optimization_diagnostic_new(OptimizationType opt_type,
                                                   struct ASTNode* target,
                                                   OptimizationError error,
                                                   const char* message);
void optimization_diagnostic_free(OptimizationDiagnostic* diag);
int emit_optimization_diagnostic(OptimizationContext* ctx, OptimizationDiagnostic* diag);

#endif // RUNTIME_OPTIMIZATION_H
