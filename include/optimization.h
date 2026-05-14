#ifndef OPTIMIZATION_H
#define OPTIMIZATION_H

#include "comptime.h"
#include "ast.h"
#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct OptimizationContext OptimizationContext;
typedef struct OptimizationDirective OptimizationDirective;
typedef struct TargetInfo TargetInfo;

// =============================================================================
// Optimization Types and Strategies
// =============================================================================

// Primary optimization goals
typedef enum {
    OPT_GOAL_THROUGHPUT,    // Maximize data processing throughput
    OPT_GOAL_LATENCY,       // Minimize response time
    OPT_GOAL_MEMORY,        // Minimize memory usage
    OPT_GOAL_ENERGY,        // Minimize power consumption
    OPT_GOAL_SIZE,          // Minimize code size
    OPT_GOAL_BALANCED       // Balanced approach
} OptimizationGoal;

// Specific optimization strategies
typedef enum {
    OPT_STRATEGY_SIMD,              // SIMD vectorization
    OPT_STRATEGY_PARALLEL,          // Multi-threading
    OPT_STRATEGY_CACHE_FRIENDLY,    // Cache-aware optimizations
    OPT_STRATEGY_UNROLL,            // Loop unrolling
    OPT_STRATEGY_INLINE,            // Function inlining
    OPT_STRATEGY_PREFETCH,          // Memory prefetching
    OPT_STRATEGY_BRANCH_PREDICT,    // Branch prediction hints
    OPT_STRATEGY_CONSTANT_PROP,     // Constant propagation
    OPT_STRATEGY_DEAD_CODE_ELIM,    // Dead code elimination
    OPT_STRATEGY_TAIL_CALL          // Tail call optimization
} OptimizationStrategy;

// Hardware feature detection
typedef enum {
    HW_FEATURE_SSE,
    HW_FEATURE_SSE2,
    HW_FEATURE_SSE3,
    HW_FEATURE_SSE4_1,
    HW_FEATURE_SSE4_2,
    HW_FEATURE_AVX,
    HW_FEATURE_AVX2,
    HW_FEATURE_AVX512,
    HW_FEATURE_NEON,
    HW_FEATURE_AES_NI,
    HW_FEATURE_GPU,
    HW_FEATURE_DEDICATED_CRYPTO,
    HW_FEATURE_COUNT
} HardwareFeature;

// =============================================================================
// Target Information and Capabilities
// =============================================================================

typedef struct TargetInfo {
    char* architecture;         // x86_64, aarch64, etc.
    char* cpu_model;           // Specific CPU model
    int core_count;            // Number of CPU cores
    size_t cache_line_size;    // L1 cache line size
    size_t l1_cache_size;      // L1 cache size
    size_t l2_cache_size;      // L2 cache size
    size_t l3_cache_size;      // L3 cache size
    bool features[HW_FEATURE_COUNT]; // Available hardware features
    
    // GPU information (if available)
    bool has_gpu;
    char* gpu_model;
    int gpu_compute_units;
    size_t gpu_memory;
} TargetInfo;

// =============================================================================
// Optimization Directives
// =============================================================================

typedef struct OptimizationDirective {
    char* name;                          // Directive name (@optimize_for, @use_algorithm, etc.)
    OptimizationGoal goal;               // Primary optimization goal
    OptimizationStrategy* strategies;    // List of strategies to apply
    size_t strategy_count;               // Number of strategies
    
    // Directive-specific parameters
    union {
        struct {
            char* algorithm_name;        // For @use_algorithm
            double performance_threshold; // Minimum expected improvement
        } algorithm;
        
        struct {
            bool enable_vectorization;   // For @optimize_for
            bool enable_parallelization;
            bool enable_prefetching;
            int unroll_factor;
        } general;
        
        struct {
            char* profile_data_path;     // For @profile_guided
            double branch_threshold;     // Branch prediction confidence threshold
        } profile_guided;
        
        struct {
            char** implementations;      // For @benchmark_and_select
            size_t impl_count;
            char* test_data_path;
            int benchmark_iterations;
        } benchmark;
    } params;
    
    // Applied function/block information
    ASTNode* target_node;               // Function or block this applies to
    Position source_position;          // Where directive was defined
    
    struct OptimizationDirective* next; // For linked lists
} OptimizationDirective;

// =============================================================================
// Optimization Context and State
// =============================================================================

typedef struct OptimizationContext {
    TargetInfo* target_info;            // Target hardware information
    ComptimeContext* comptime_ctx;      // Compile-time execution context
    
    // Registered optimization directives
    OptimizationDirective* directives;
    
    // Algorithm registry
    char** available_algorithms;
    size_t algorithm_count;
    
    // Performance data and benchmarks
    char* benchmark_cache_dir;
    bool enable_benchmarking;
    
    // Optimization settings
    bool conservative_mode;             // Only apply safe optimizations
    bool cross_function_optimization;   // Enable inter-procedural optimization
    double min_performance_improvement; // Minimum required speedup
    
    // Generated optimization code
    char* generated_optimizations;
    size_t optimization_buffer_size;
    size_t optimization_buffer_capacity;
} OptimizationContext;

// =============================================================================
// Core Optimization Functions
// =============================================================================

// Context management
OptimizationContext* comptime_optimization_context_new(ComptimeContext* comptime_ctx);
void comptime_optimization_context_free(OptimizationContext* ctx);

// Target information
TargetInfo* target_info_detect(void);
TargetInfo* target_info_from_string(const char* target_spec);
void target_info_free(TargetInfo* info);
bool target_has_feature(TargetInfo* info, HardwareFeature feature);

// Directive management
OptimizationDirective* optimization_directive_new(const char* name, OptimizationGoal goal);
void optimization_directive_free(OptimizationDirective* directive);
int optimization_context_add_directive(OptimizationContext* ctx, OptimizationDirective* directive);
OptimizationDirective* optimization_context_find_directive(OptimizationContext* ctx, const char* name);

// Directive parsing and processing
OptimizationDirective* parse_optimization_directive(ASTNode* directive_node);
int apply_optimization_directive(OptimizationContext* ctx, OptimizationDirective* directive, ASTNode* target);

// =============================================================================
// Specific Optimization Directive Implementations
// =============================================================================

// @optimize_for directive
typedef struct {
    OptimizationGoal goal;
    bool enable_auto_vectorization;
    bool enable_auto_parallelization;
    bool enable_cache_optimization;
    bool enable_branch_optimization;
} OptimizeForParams;

ComptimeResult* comptime_directive_optimize_for(OptimizationContext* ctx, OptimizeForParams* params, ASTNode* target);

// @use_algorithm directive
typedef struct {
    char* algorithm_name;
    double min_speedup_required;
    bool fallback_to_default;
} UseAlgorithmParams;

ComptimeResult* comptime_directive_use_algorithm(OptimizationContext* ctx, UseAlgorithmParams* params, ASTNode* target);

// @target_has directive (compile-time hardware detection)
typedef struct {
    HardwareFeature feature;
} TargetHasParams;

ComptimeResult* comptime_directive_target_has(OptimizationContext* ctx, TargetHasParams* params);

// @profile_guided directive
typedef struct {
    char* profile_data_file;
    bool enable_speculative_optimization;
    double confidence_threshold;
} ProfileGuidedParams;

ComptimeResult* comptime_directive_profile_guided(OptimizationContext* ctx, ProfileGuidedParams* params, ASTNode* target);

// @benchmark_and_select directive
typedef struct {
    char** implementation_names;
    size_t implementation_count;
    char* test_data_spec;
    int benchmark_iterations;
    OptimizationGoal selection_criteria;
} BenchmarkSelectParams;

ComptimeResult* comptime_directive_benchmark_and_select(OptimizationContext* ctx, BenchmarkSelectParams* params);

// =============================================================================
// Algorithm and Implementation Registry
// =============================================================================

typedef struct AlgorithmImplementation {
    char* name;                     // Algorithm name
    char* description;              // Human-readable description
    OptimizationGoal* strengths;    // What this implementation optimizes for
    size_t strength_count;
    HardwareFeature* requirements;  // Required hardware features
    size_t requirement_count;
    
    // Code generation function
    ComptimeResult* (*generate_code)(OptimizationContext* ctx, ASTNode* target, void* params);
    
    struct AlgorithmImplementation* next;
} AlgorithmImplementation;

// Algorithm registry management
int optimization_register_algorithm(OptimizationContext* ctx, AlgorithmImplementation* impl);
AlgorithmImplementation* optimization_find_algorithm(OptimizationContext* ctx, const char* name);
AlgorithmImplementation* optimization_select_best_algorithm(OptimizationContext* ctx, 
    const char** candidates, size_t candidate_count, OptimizationGoal goal);

// =============================================================================
// Built-in Algorithm Implementations
// =============================================================================

// Sort algorithms
ComptimeResult* algorithm_quicksort_generate(OptimizationContext* ctx, ASTNode* target, void* params);
ComptimeResult* algorithm_mergesort_generate(OptimizationContext* ctx, ASTNode* target, void* params);
ComptimeResult* algorithm_radixsort_generate(OptimizationContext* ctx, ASTNode* target, void* params);
ComptimeResult* algorithm_simd_sort_generate(OptimizationContext* ctx, ASTNode* target, void* params);

// Search algorithms
ComptimeResult* algorithm_linear_search_generate(OptimizationContext* ctx, ASTNode* target, void* params);
ComptimeResult* algorithm_binary_search_generate(OptimizationContext* ctx, ASTNode* target, void* params);
ComptimeResult* algorithm_simd_search_generate(OptimizationContext* ctx, ASTNode* target, void* params);

// Matrix operations
ComptimeResult* algorithm_matrix_multiply_naive_generate(OptimizationContext* ctx, ASTNode* target, void* params);
ComptimeResult* algorithm_matrix_multiply_blocked_generate(OptimizationContext* ctx, ASTNode* target, void* params);
ComptimeResult* algorithm_matrix_multiply_simd_generate(OptimizationContext* ctx, ASTNode* target, void* params);

// =============================================================================
// Utility Functions
// =============================================================================

// Hardware detection utilities
const char* hardware_feature_name(HardwareFeature feature);
bool detect_hardware_feature(HardwareFeature feature);
TargetInfo* get_current_target_info(void);

// Optimization goal utilities
const char* optimization_goal_name(OptimizationGoal goal);
const char* optimization_strategy_name(OptimizationStrategy strategy);

// Performance estimation
double estimate_performance_improvement(OptimizationContext* ctx, OptimizationDirective* directive, ASTNode* target);
bool is_optimization_beneficial(OptimizationContext* ctx, OptimizationDirective* directive, ASTNode* target);

// Code generation utilities
int generate_optimized_code(OptimizationContext* ctx, ASTNode* target, char** output_code);
int apply_all_optimizations(OptimizationContext* ctx, ASTNode* program);

// Built-in directive registration
void register_builtin_optimization_directives(OptimizationContext* ctx);
void register_builtin_algorithms(OptimizationContext* ctx);

#endif // OPTIMIZATION_H
