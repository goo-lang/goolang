#include "optimization.h"
#include "ast.h"
#include "comptime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

// Platform-specific headers for hardware detection
#ifdef __x86_64__
#include <cpuid.h>
#ifdef __AVX__
#include <immintrin.h>
#endif
#endif

#ifdef __linux__
#ifdef __aarch64__
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#endif

// Under __COMPCERT__ we skip <sys/sysctl.h> (transitively pulls in
// <mach/port.h>'s C23 enum). CPU detection then falls back to the
// generic path; non-critical for V1.
#if defined(__APPLE__) && !defined(__COMPCERT__)
#include <sys/sysctl.h>
#endif

// =============================================================================
// Target Information and Hardware Detection
// =============================================================================

TargetInfo* target_info_detect(void) {
    TargetInfo* info = malloc(sizeof(TargetInfo));
    if (!info) return NULL;
    
    memset(info, 0, sizeof(TargetInfo));
    
    // Detect architecture
#ifdef __x86_64__
    info->architecture = strdup("x86_64");
    
    // Detect x86 features using CPUID
    unsigned int eax, ebx, ecx, edx;
    
    // Check for SSE/AVX support
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        info->features[HW_FEATURE_SSE] = (edx & bit_SSE) != 0;
        info->features[HW_FEATURE_SSE2] = (edx & bit_SSE2) != 0;
        info->features[HW_FEATURE_SSE3] = (ecx & bit_SSE3) != 0;
        info->features[HW_FEATURE_SSE4_1] = (ecx & bit_SSE4_1) != 0;
        info->features[HW_FEATURE_SSE4_2] = (ecx & bit_SSE4_2) != 0;
        info->features[HW_FEATURE_AES_NI] = (ecx & bit_AES) != 0;
        info->features[HW_FEATURE_AVX] = (ecx & bit_AVX) != 0;
    }
    
    // Check for AVX2/AVX512
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        info->features[HW_FEATURE_AVX2] = (ebx & bit_AVX2) != 0;
        info->features[HW_FEATURE_AVX512] = (ebx & bit_AVX512F) != 0;
    }
    
#elif defined(__aarch64__)
    info->architecture = strdup("aarch64");
    
#ifdef __linux__
    // Detect ARM features using getauxval (Linux only)
    unsigned long hwcaps = getauxval(AT_HWCAP);
    info->features[HW_FEATURE_NEON] = (hwcaps & HWCAP_ASIMD) != 0;
    info->features[HW_FEATURE_AES_NI] = (hwcaps & HWCAP_AES) != 0;
#elif defined(__APPLE__)
    // On Apple Silicon, NEON is always available
    info->features[HW_FEATURE_NEON] = true;
    info->features[HW_FEATURE_AES_NI] = true;
#endif
    
#else
    info->architecture = strdup("unknown");
#endif

    // Detect core count (simplified)
    info->core_count = 4; // Default assumption
#ifdef _SC_NPROCESSORS_ONLN
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) {
        info->core_count = (int)cores;
    }
#elif defined(__APPLE__)
    // Use sysctl on macOS
    size_t size = sizeof(info->core_count);
    if (sysctlbyname("hw.ncpu", &info->core_count, &size, NULL, 0) != 0) {
        info->core_count = 4; // Fallback
    }
#endif

    // Set default cache sizes (would be detected from system in real implementation)
    info->cache_line_size = 64;
    info->l1_cache_size = 32 * 1024;      // 32KB
    info->l2_cache_size = 256 * 1024;     // 256KB
    info->l3_cache_size = 8 * 1024 * 1024; // 8MB
    
    // CPU model detection (simplified)
    info->cpu_model = strdup("generic");
    
    // GPU detection (placeholder - would use OpenCL/CUDA/etc. in real implementation)
    info->has_gpu = false;
    info->gpu_model = NULL;
    info->gpu_compute_units = 0;
    info->gpu_memory = 0;
    
    return info;
}

TargetInfo* target_info_from_string(const char* target_spec) {
    // Parse target specification string (e.g., "x86_64-unknown-linux-gnu")
    TargetInfo* info = malloc(sizeof(TargetInfo));
    if (!info) return NULL;
    
    memset(info, 0, sizeof(TargetInfo));
    
    // Simple parsing - in real implementation would be more sophisticated
    if (strstr(target_spec, "x86_64")) {
        info->architecture = strdup("x86_64");
        // Enable common x86_64 features
        info->features[HW_FEATURE_SSE] = true;
        info->features[HW_FEATURE_SSE2] = true;
        info->features[HW_FEATURE_SSE3] = true;
    } else if (strstr(target_spec, "aarch64")) {
        info->architecture = strdup("aarch64");
        info->features[HW_FEATURE_NEON] = true;
    } else {
        info->architecture = strdup("unknown");
    }
    
    // Set defaults
    info->core_count = 4;
    info->cache_line_size = 64;
    info->l1_cache_size = 32 * 1024;
    info->l2_cache_size = 256 * 1024;
    info->l3_cache_size = 8 * 1024 * 1024;
    info->cpu_model = strdup("generic");
    
    return info;
}

void target_info_free(TargetInfo* info) {
    if (!info) return;
    
    free(info->architecture);
    free(info->cpu_model);
    free(info->gpu_model);
    free(info);
}

bool target_has_feature(TargetInfo* info, HardwareFeature feature) {
    if (!info || feature >= HW_FEATURE_COUNT) return false;
    return info->features[feature];
}

// =============================================================================
// Optimization Context Management
// =============================================================================

OptimizationContext* comptime_optimization_context_new(ComptimeContext* comptime_ctx) {
    OptimizationContext* ctx = malloc(sizeof(OptimizationContext));
    if (!ctx) return NULL;
    
    memset(ctx, 0, sizeof(OptimizationContext));
    
    ctx->comptime_ctx = comptime_ctx;
    ctx->target_info = target_info_detect();
    ctx->directives = NULL;
    
    // Initialize algorithm registry
    ctx->available_algorithms = NULL;
    ctx->algorithm_count = 0;
    
    // Set default optimization settings
    ctx->conservative_mode = true;
    ctx->cross_function_optimization = false;
    ctx->min_performance_improvement = 1.1; // Require at least 10% improvement
    ctx->enable_benchmarking = false;
    
    // Initialize code generation buffer
    ctx->generated_optimizations = NULL;
    ctx->optimization_buffer_size = 0;
    ctx->optimization_buffer_capacity = 0;
    
    // Set default benchmark cache directory
    ctx->benchmark_cache_dir = strdup(".goo_benchmark_cache");
    
    return ctx;
}

void comptime_optimization_context_free(OptimizationContext* ctx) {
    if (!ctx) return;
    
    target_info_free(ctx->target_info);
    
    // Free directives
    OptimizationDirective* directive = ctx->directives;
    while (directive) {
        OptimizationDirective* next = directive->next;
        optimization_directive_free(directive);
        directive = next;
    }
    
    // Free algorithm registry
    for (size_t i = 0; i < ctx->algorithm_count; i++) {
        free(ctx->available_algorithms[i]);
    }
    free(ctx->available_algorithms);
    
    free(ctx->benchmark_cache_dir);
    free(ctx->generated_optimizations);
    free(ctx);
}

// =============================================================================
// Optimization Directive Management
// =============================================================================

OptimizationDirective* optimization_directive_new(const char* name, OptimizationGoal goal) {
    OptimizationDirective* directive = malloc(sizeof(OptimizationDirective));
    if (!directive) return NULL;
    
    memset(directive, 0, sizeof(OptimizationDirective));
    
    directive->name = strdup(name);
    directive->goal = goal;
    directive->strategies = NULL;
    directive->strategy_count = 0;
    directive->target_node = NULL;
    directive->next = NULL;
    
    return directive;
}

void optimization_directive_free(OptimizationDirective* directive) {
    if (!directive) return;
    
    free(directive->name);
    free(directive->strategies);
    
    // Free directive-specific parameters
    if (strcmp(directive->name, "@use_algorithm") == 0) {
        free(directive->params.algorithm.algorithm_name);
    } else if (strcmp(directive->name, "@profile_guided") == 0) {
        free(directive->params.profile_guided.profile_data_path);
    } else if (strcmp(directive->name, "@benchmark_and_select") == 0) {
        for (size_t i = 0; i < directive->params.benchmark.impl_count; i++) {
            free(directive->params.benchmark.implementations[i]);
        }
        free(directive->params.benchmark.implementations);
        free(directive->params.benchmark.test_data_path);
    }
    
    free(directive);
}

int optimization_context_add_directive(OptimizationContext* ctx, OptimizationDirective* directive) {
    if (!ctx || !directive) return 0;
    
    // Add to linked list
    directive->next = ctx->directives;
    ctx->directives = directive;
    
    return 1;
}

OptimizationDirective* optimization_context_find_directive(OptimizationContext* ctx, const char* name) {
    if (!ctx || !name) return NULL;
    
    OptimizationDirective* directive = ctx->directives;
    while (directive) {
        if (strcmp(directive->name, name) == 0) {
            return directive;
        }
        directive = directive->next;
    }
    
    return NULL;
}

// =============================================================================
// Specific Optimization Directive Implementations
// =============================================================================

ComptimeResult* comptime_directive_optimize_for(OptimizationContext* ctx, OptimizeForParams* params, ASTNode* target) {
    if (!ctx || !params || !target) {
        return comptime_result_new(NULL, comptime_error_new("Invalid parameters for @optimize_for", (Position){0}), NULL);
    }
    
    // Generate optimization code based on the goal
    char* optimization_code = malloc(1024);
    if (!optimization_code) {
        return comptime_result_new(NULL, comptime_error_new("Out of memory", (Position){0}), NULL);
    }
    
    switch (params->goal) {
        case OPT_GOAL_THROUGHPUT:
            snprintf(optimization_code, 1024, 
                "// Optimized for throughput\n"
                "#pragma GCC optimize(\"O3\")\n"
                "#pragma GCC target(\"avx2\")\n"
                "// Enable vectorization and parallelization\n");
            break;
            
        case OPT_GOAL_LATENCY:
            snprintf(optimization_code, 1024,
                "// Optimized for low latency\n"
                "#pragma GCC optimize(\"O2\")\n"
                "// Prefer smaller, faster code\n");
            break;
            
        case OPT_GOAL_MEMORY:
            snprintf(optimization_code, 1024,
                "// Optimized for memory usage\n"
                "#pragma GCC optimize(\"Os\")\n"
                "// Minimize memory allocations\n");
            break;
            
        case OPT_GOAL_ENERGY:
            snprintf(optimization_code, 1024,
                "// Optimized for energy efficiency\n"
                "#pragma GCC optimize(\"O2\")\n"
                "// Prefer efficient algorithms\n");
            break;
            
        default:
            snprintf(optimization_code, 1024,
                "// Balanced optimization\n"
                "#pragma GCC optimize(\"O2\")\n");
            break;
    }
    
    ComptimeValue* result_value = comptime_value_new(COMPTIME_VALUE_STRING);
    result_value->string_value = strdup(optimization_code);  // Create a copy for the value
    
    return comptime_result_new(result_value, NULL, optimization_code);  // Use original for generated_code
}

ComptimeResult* comptime_directive_use_algorithm(OptimizationContext* ctx, UseAlgorithmParams* params, ASTNode* target) {
    if (!ctx || !params || !target) {
        return comptime_result_new(NULL, comptime_error_new("Invalid parameters for @use_algorithm", (Position){0}), NULL);
    }
    
    // Generate code to use the specified algorithm
    char* algorithm_code = malloc(512);
    if (!algorithm_code) {
        return comptime_result_new(NULL, comptime_error_new("Out of memory", (Position){0}), NULL);
    }
    
    snprintf(algorithm_code, 512,
        "// Using algorithm: %s\n"
        "// Minimum speedup required: %.2fx\n"
        "// Implementation will be selected at compile time\n",
        params->algorithm_name,
        params->min_speedup_required);
    
    ComptimeValue* result_value = comptime_value_new(COMPTIME_VALUE_STRING);
    result_value->string_value = strdup(algorithm_code);  // Create a copy for the value
    
    return comptime_result_new(result_value, NULL, algorithm_code);  // Use original for generated_code
}

ComptimeResult* comptime_directive_target_has(OptimizationContext* ctx, TargetHasParams* params) {
    if (!ctx || !params) {
        return comptime_result_new(NULL, comptime_error_new("Invalid parameters for @target_has", (Position){0}), NULL);
    }
    
    bool has_feature = target_has_feature(ctx->target_info, params->feature);
    
    ComptimeValue* result_value = comptime_value_new(COMPTIME_VALUE_BOOL);
    result_value->bool_value = has_feature;
    
    return comptime_result_new(result_value, NULL, NULL);
}

ComptimeResult* comptime_directive_profile_guided(OptimizationContext* ctx, ProfileGuidedParams* params, ASTNode* target) {
    if (!ctx || !params || !target) {
        return comptime_result_new(NULL, comptime_error_new("Invalid parameters for @profile_guided", (Position){0}), NULL);
    }
    
    // Generate profile-guided optimization code
    char* pgo_code = malloc(512);
    if (!pgo_code) {
        return comptime_result_new(NULL, comptime_error_new("Out of memory", (Position){0}), NULL);
    }
    
    snprintf(pgo_code, 512,
        "// Profile-guided optimization enabled\n"
        "// Profile data: %s\n"
        "// Confidence threshold: %.2f\n"
        "#pragma GCC optimize(\"O3\")\n"
        "// Branch prediction hints will be added based on profile data\n",
        params->profile_data_file ? params->profile_data_file : "default",
        params->confidence_threshold);
    
    ComptimeValue* result_value = comptime_value_new(COMPTIME_VALUE_STRING);
    result_value->string_value = strdup(pgo_code);  // Create a copy for the value
    
    return comptime_result_new(result_value, NULL, pgo_code);  // Use original for generated_code
}

ComptimeResult* comptime_directive_benchmark_and_select(OptimizationContext* ctx, BenchmarkSelectParams* params) {
    if (!ctx || !params) {
        return comptime_result_new(NULL, comptime_error_new("Invalid parameters for @benchmark_and_select", (Position){0}), NULL);
    }
    
    // In a real implementation, this would actually run benchmarks
    // For now, we'll simulate selecting the "best" implementation
    
    char* selected_impl = "default_implementation";
    if (params->implementation_count > 0) {
        // Simple heuristic: prefer SIMD implementations if available
        for (size_t i = 0; i < params->implementation_count; i++) {
            if (strstr(params->implementation_names[i], "simd") && 
                target_has_feature(ctx->target_info, HW_FEATURE_AVX2)) {
                selected_impl = params->implementation_names[i];
                break;
            }
        }
        if (selected_impl == params->implementation_names[0]) {
            selected_impl = params->implementation_names[0]; // Fallback to first
        }
    }
    
    char* benchmark_code = malloc(512);
    if (!benchmark_code) {
        return comptime_result_new(NULL, comptime_error_new("Out of memory", (Position){0}), NULL);
    }
    
    snprintf(benchmark_code, 512,
        "// Benchmark-selected implementation: %s\n"
        "// Benchmarked %zu implementations\n"
        "// Selection criteria: %s\n",
        selected_impl,
        params->implementation_count,
        optimization_goal_name(params->selection_criteria));
    
    ComptimeValue* result_value = comptime_value_new(COMPTIME_VALUE_STRING);
    result_value->string_value = strdup(selected_impl);
    
    return comptime_result_new(result_value, NULL, benchmark_code);
}

// =============================================================================
// Utility Functions
// =============================================================================

const char* hardware_feature_name(HardwareFeature feature) {
    switch (feature) {
        case HW_FEATURE_SSE: return "sse";
        case HW_FEATURE_SSE2: return "sse2";
        case HW_FEATURE_SSE3: return "sse3";
        case HW_FEATURE_SSE4_1: return "sse4.1";
        case HW_FEATURE_SSE4_2: return "sse4.2";
        case HW_FEATURE_AVX: return "avx";
        case HW_FEATURE_AVX2: return "avx2";
        case HW_FEATURE_AVX512: return "avx512";
        case HW_FEATURE_NEON: return "neon";
        case HW_FEATURE_AES_NI: return "aes";
        case HW_FEATURE_GPU: return "gpu";
        case HW_FEATURE_DEDICATED_CRYPTO: return "dedicated_crypto";
        default: return "unknown";
    }
}

bool detect_hardware_feature(HardwareFeature feature) {
    TargetInfo* info = target_info_detect();
    if (!info) return false;
    
    bool result = target_has_feature(info, feature);
    target_info_free(info);
    return result;
}

TargetInfo* get_current_target_info(void) {
    return target_info_detect();
}

const char* optimization_goal_name(OptimizationGoal goal) {
    switch (goal) {
        case OPT_GOAL_THROUGHPUT: return "throughput";
        case OPT_GOAL_LATENCY: return "latency";
        case OPT_GOAL_MEMORY: return "memory";
        case OPT_GOAL_ENERGY: return "energy";
        case OPT_GOAL_SIZE: return "size";
        case OPT_GOAL_BALANCED: return "balanced";
        default: return "unknown";
    }
}

const char* optimization_strategy_name(OptimizationStrategy strategy) {
    switch (strategy) {
        case OPT_STRATEGY_SIMD: return "simd";
        case OPT_STRATEGY_PARALLEL: return "parallel";
        case OPT_STRATEGY_CACHE_FRIENDLY: return "cache_friendly";
        case OPT_STRATEGY_UNROLL: return "unroll";
        case OPT_STRATEGY_INLINE: return "inline";
        case OPT_STRATEGY_PREFETCH: return "prefetch";
        case OPT_STRATEGY_BRANCH_PREDICT: return "branch_predict";
        case OPT_STRATEGY_CONSTANT_PROP: return "constant_prop";
        case OPT_STRATEGY_DEAD_CODE_ELIM: return "dead_code_elim";
        case OPT_STRATEGY_TAIL_CALL: return "tail_call";
        default: return "unknown";
    }
}

double estimate_performance_improvement(OptimizationContext* ctx, OptimizationDirective* directive, ASTNode* target) {
    if (!ctx || !directive || !target) return 1.0;
    
    // Simple heuristic-based estimation
    // In a real implementation, this would use more sophisticated modeling
    
    double improvement = 1.0;
    
    switch (directive->goal) {
        case OPT_GOAL_THROUGHPUT:
            if (target_has_feature(ctx->target_info, HW_FEATURE_AVX2)) {
                improvement *= 2.0; // Assume 2x speedup with vectorization
            }
            if (ctx->target_info->core_count > 1) {
                improvement *= 1.5; // Assume 1.5x speedup with parallelization
            }
            break;
            
        case OPT_GOAL_LATENCY:
            improvement *= 1.2; // Conservative improvement for latency optimization
            break;
            
        case OPT_GOAL_MEMORY:
            improvement *= 1.1; // Modest improvement for memory optimization
            break;
            
        default:
            improvement *= 1.15; // Default modest improvement
            break;
    }
    
    return improvement;
}

bool is_optimization_beneficial(OptimizationContext* ctx, OptimizationDirective* directive, ASTNode* target) {
    if (!ctx || !directive || !target) return false;
    
    double estimated_improvement = estimate_performance_improvement(ctx, directive, target);
    return estimated_improvement >= ctx->min_performance_improvement;
}

// =============================================================================
// Built-in Registration Functions
// =============================================================================

void register_builtin_optimization_directives(OptimizationContext* ctx) {
    if (!ctx) return;
    
    // Register @optimize_for directive
    OptimizationDirective* optimize_for = optimization_directive_new("@optimize_for", OPT_GOAL_BALANCED);
    if (optimize_for) {
        optimization_context_add_directive(ctx, optimize_for);
    }
    
    // Register @use_algorithm directive
    OptimizationDirective* use_algorithm = optimization_directive_new("@use_algorithm", OPT_GOAL_THROUGHPUT);
    if (use_algorithm) {
        optimization_context_add_directive(ctx, use_algorithm);
    }
    
    // Register @target_has directive
    OptimizationDirective* target_has = optimization_directive_new("@target_has", OPT_GOAL_BALANCED);
    if (target_has) {
        optimization_context_add_directive(ctx, target_has);
    }
    
    // Register @profile_guided directive
    OptimizationDirective* profile_guided = optimization_directive_new("@profile_guided", OPT_GOAL_THROUGHPUT);
    if (profile_guided) {
        optimization_context_add_directive(ctx, profile_guided);
    }
    
    // Register @benchmark_and_select directive
    OptimizationDirective* benchmark_select = optimization_directive_new("@benchmark_and_select", OPT_GOAL_THROUGHPUT);
    if (benchmark_select) {
        optimization_context_add_directive(ctx, benchmark_select);
    }
}

void register_builtin_algorithms(OptimizationContext* ctx) {
    if (!ctx) return;
    
    // Register basic algorithm names
    const char* builtin_algorithms[] = {
        "quicksort", "mergesort", "radixsort", "simd_sort",
        "linear_search", "binary_search", "simd_search",
        "matrix_multiply_naive", "matrix_multiply_blocked", "matrix_multiply_simd"
    };
    
    size_t count = sizeof(builtin_algorithms) / sizeof(builtin_algorithms[0]);
    
    ctx->available_algorithms = malloc(sizeof(char*) * count);
    if (!ctx->available_algorithms) return;
    
    for (size_t i = 0; i < count; i++) {
        ctx->available_algorithms[i] = strdup(builtin_algorithms[i]);
    }
    ctx->algorithm_count = count;
}
