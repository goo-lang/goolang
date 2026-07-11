#include "hardware_aware.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#ifdef __x86_64__
// CompCert can't parse GCC's x86 intrinsic headers (__int128 in <immintrin.h>,
// multi-output asm in <cpuid.h>). The CPU-capability probe below is a runtime
// perf feature, irrelevant to the translation correctness the ccomp build
// verifies, so skip these headers (and the probe body) under CompCert.
#ifndef __COMPCERT__
#include <cpuid.h>
#include <immintrin.h>
// GCC 16 renamed bit_RDRAND to bit_RDRND in <cpuid.h>. Keep building on both.
#if !defined(bit_RDRAND) && defined(bit_RDRND)
#define bit_RDRAND bit_RDRND
#endif
#endif // __COMPCERT__
#endif

#ifdef __aarch64__
#ifdef __linux__
#include <sys/auxv.h>
#include <asm/hwcap.h>
#else
// macOS ARM64 - use sysctl instead. <sys/sysctl.h> transitively pulls
// in <mach/port.h> which uses C23 enum-with-underlying-type that
// CompCert can't parse. Under __COMPCERT__ we skip the include and
// stub sysctlbyname; this disables CPU brand detection (a non-critical
// optimization hint) but keeps the rest of hardware-aware codegen
// available to the ccomp build.
#ifndef __COMPCERT__
#include <sys/sysctl.h>
#else
static int sysctlbyname(const char* name, void* oldp, size_t* oldlenp, void* newp, size_t newlen) {
    (void)name; (void)oldp; (void)oldlenp; (void)newp; (void)newlen;
    return -1;  // signal "not available"
}
#endif
#endif
#endif

// Global hardware context (initialized once)
static HardwareAwareContext* g_hardware_context = NULL;
static pthread_once_t g_hardware_init_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_hardware_mutex = PTHREAD_MUTEX_INITIALIZER;

// Initialize global hardware context
static void initialize_global_hardware_context(void) {
    g_hardware_context = create_hardware_aware_context();
    if (g_hardware_context) {
        initialize_hardware_context(g_hardware_context);
    }
}

// CPU feature detection for x86_64
#ifdef __x86_64__
static void detect_x86_64_features(HardwareProfile* profile) {
#ifndef __COMPCERT__
    unsigned int eax, ebx, ecx, edx;
    
    // Basic CPUID
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if (ecx & bit_SSE3) profile->capabilities |= HW_CAP_SIMD_SSE;
        if (ecx & bit_AES) profile->capabilities |= HW_CAP_AES_NI;
        if (ecx & bit_RDRAND) profile->capabilities |= HW_CAP_RDRAND;
        if (ecx & bit_FMA) profile->capabilities |= HW_CAP_FMA;
    }
    
    // Extended features
    if (__get_cpuid_max(7, NULL) >= 7) {
        __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
        if (ebx & bit_AVX2) profile->capabilities |= HW_CAP_SIMD_AVX2;
        if (ebx & bit_BMI) profile->capabilities |= HW_CAP_BMI;
        if (ebx & bit_SHA) profile->capabilities |= HW_CAP_SHA_EXT;
        if (ebx & bit_AVX512F) profile->capabilities |= HW_CAP_SIMD_AVX512;
    }
    
    // Detect microarchitecture
    char vendor[13];
    __get_cpuid(0, &eax, &ebx, &ecx, &edx);
    *(unsigned int*)vendor = ebx;
    *(unsigned int*)(vendor + 4) = edx;
    *(unsigned int*)(vendor + 8) = ecx;
    vendor[12] = '\0';
    
    if (strcmp(vendor, "GenuineIntel") == 0) {
        // Simplified Intel microarch detection
        unsigned int family = (eax >> 8) & 0xF;
        unsigned int model = (eax >> 4) & 0xF;
        if (family == 6) {
            if (model >= 0x9E) profile->microarch = UARCH_INTEL_SKYLAKE;
            else if (model >= 0x7E) profile->microarch = UARCH_INTEL_ICE_LAKE;
        }
    } else if (strcmp(vendor, "AuthenticAMD") == 0) {
        profile->microarch = UARCH_AMD_ZEN3; // Simplified
    }
#else
    (void)profile; // CompCert build: no CPUID probing; default profile (no caps)
#endif
}
#endif

// CPU feature detection for ARM64
#ifdef __aarch64__
static void detect_aarch64_features(HardwareProfile* profile) {
    #ifdef __linux__
    unsigned long hwcap = getauxval(AT_HWCAP);
    unsigned long hwcap2 = getauxval(AT_HWCAP2);
    
    if (hwcap & HWCAP_ASIMD) profile->capabilities |= HW_CAP_SIMD_NEON;
    if (hwcap & HWCAP_AES) profile->capabilities |= HW_CAP_AES_NI;
    if (hwcap & HWCAP_SHA1) profile->capabilities |= HW_CAP_SHA_EXT;
    if (hwcap & HWCAP_CRC32) profile->capabilities |= HW_CAP_CRC32;
    
    #ifdef HWCAP2_SVE
    if (hwcap2 & HWCAP2_SVE) profile->capabilities |= HW_CAP_SIMD_SVE;
    #endif
    #else
    // macOS ARM64 - assume NEON is available (always present on Apple Silicon)
    profile->capabilities |= HW_CAP_SIMD_NEON;
    profile->capabilities |= HW_CAP_AES_NI;
    profile->capabilities |= HW_CAP_SHA_EXT;
    profile->capabilities |= HW_CAP_CRC32;
    #endif
    
    // Detect Apple Silicon
    #ifdef __APPLE__
    char cpu_brand[256];
    size_t size = sizeof(cpu_brand);
    if (sysctlbyname("machdep.cpu.brand_string", cpu_brand, &size, NULL, 0) == 0) {
        if (strstr(cpu_brand, "Apple M1")) {
            profile->microarch = UARCH_APPLE_M1;
        } else if (strstr(cpu_brand, "Apple M2")) {
            profile->microarch = UARCH_APPLE_M2;
        }
    }
    #endif
}
#endif

// Hardware detection implementation
HardwareProfile* detect_hardware_profile(void) {
    HardwareProfile* profile = xcalloc(1, sizeof(HardwareProfile));
    if (!profile) return NULL;
    
    // Detect architecture
    #if defined(__x86_64__)
    profile->arch = ARCH_X86_64;
    profile->simd_width_bits = 256; // Default to AVX2
    detect_x86_64_features(profile);
    #elif defined(__aarch64__)
    profile->arch = ARCH_AARCH64;
    profile->simd_width_bits = 128; // Default to NEON
    detect_aarch64_features(profile);
    #elif defined(__riscv) && (__riscv_xlen == 64)
    profile->arch = ARCH_RISCV64;
    profile->simd_width_bits = 128;
    #elif defined(__wasm__)
    profile->arch = ARCH_WASM;
    profile->simd_width_bits = 128;
    #else
    profile->arch = ARCH_UNKNOWN;
    profile->simd_width_bits = 64;
    #endif
    
    // Detect CPU topology
    #ifdef _SC_NPROCESSORS_ONLN
    profile->core_count = sysconf(_SC_NPROCESSORS_ONLN);
    profile->thread_count = profile->core_count; // Simplified
    #else
    profile->core_count = 1;
    profile->thread_count = 1;
    #endif
    
    // Set cache defaults (can be refined with /proc/cpuinfo or sysctl)
    profile->cache_line_size = 64;
    profile->l1_cache_size = 32 * 1024;
    profile->l2_cache_size = 256 * 1024;
    profile->l3_cache_size = 8 * 1024 * 1024;
    profile->memory_bandwidth_gbps = 50; // Reasonable default
    
    // GPU detection (simplified - would need platform-specific code)
    profile->has_gpu = false;
    
    // NUMA detection (simplified)
    profile->is_numa = (profile->core_count > 8);
    profile->numa_nodes = profile->is_numa ? 2 : 1;
    profile->cores_per_node = profile->core_count / profile->numa_nodes;
    
    // Power and thermal defaults
    profile->thermal_design_power = 65; // Reasonable desktop default
    profile->has_turbo_boost = true;
    profile->has_power_gating = true;
    
    // Generate LLVM target information
    profile->target_triple = strdup(
        #if defined(__x86_64__)
        "x86_64-unknown-linux-gnu"
        #elif defined(__aarch64__)
        "aarch64-unknown-linux-gnu"
        #elif defined(__riscv) && (__riscv_xlen == 64)
        "riscv64-unknown-linux-gnu"
        #else
        "unknown-unknown-unknown"
        #endif
    );
    
    profile->cpu_name = strdup("generic");
    
    // Build feature string based on detected capabilities
    char feature_buffer[1024] = {0};
    if (profile->capabilities & HW_CAP_SIMD_AVX2) {
        strncat(feature_buffer, "+avx2", sizeof(feature_buffer) - strlen(feature_buffer) - 1);
    }
    if (profile->capabilities & HW_CAP_SIMD_AVX512) {
        strncat(feature_buffer, "+avx512f", sizeof(feature_buffer) - strlen(feature_buffer) - 1);
    }
    if (profile->capabilities & HW_CAP_AES_NI) {
        strncat(feature_buffer, "+aes", sizeof(feature_buffer) - strlen(feature_buffer) - 1);
    }
    if (profile->capabilities & HW_CAP_FMA) {
        strncat(feature_buffer, "+fma", sizeof(feature_buffer) - strlen(feature_buffer) - 1);
    }
    
    profile->feature_string = strdup(feature_buffer);
    
    return profile;
}

bool update_hardware_profile(HardwareProfile* profile) {
    if (!profile) return false;
    
    // Re-detect dynamic capabilities (thermal state, current frequency, etc.)
    // This is a simplified implementation
    return true;
}

void free_hardware_profile(HardwareProfile* profile) {
    if (!profile) return;
    
    free(profile->gpu_name);
    free(profile->target_triple);
    free(profile->cpu_name);
    free(profile->feature_string);
    free(profile);
}

bool has_hardware_capability(const HardwareProfile* profile, HardwareCapability cap) {
    if (!profile) return false;
    return (profile->capabilities & cap) != 0;
}

bool check_simd_support(const HardwareProfile* profile, int required_width) {
    if (!profile) return false;
    
    return profile->simd_width_bits >= required_width;
}

bool check_gpu_capability(const HardwareProfile* profile, const char* api) {
    if (!profile || !api) return false;
    
    if (!profile->has_gpu) return false;
    
    // Simplified GPU API checking
    if (strcmp(api, "cuda") == 0) {
        return (profile->capabilities & HW_CAP_GPU_CUDA) != 0;
    } else if (strcmp(api, "opencl") == 0) {
        return (profile->capabilities & HW_CAP_GPU_OPENCL) != 0;
    } else if (strcmp(api, "metal") == 0) {
        return (profile->capabilities & HW_CAP_GPU_METAL) != 0;
    }
    
    return false;
}

int get_optimal_thread_count(const HardwareProfile* profile) {
    if (!profile) return 1;
    
    // For CPU-bound work, use physical cores
    // For I/O-bound work, could use more threads
    int optimal = profile->core_count;
    
    // Account for hyperthreading
    if (profile->capabilities & HW_CAP_HYPERTHREADING) {
        optimal = profile->thread_count;
    }
    
    // Cap at reasonable maximum
    return optimal > 64 ? 64 : optimal;
}

HardwareOptimization* create_hardware_optimization(const HardwareProfile* profile) {
    if (!profile) return NULL;
    
    HardwareOptimization* opt = xcalloc(1, sizeof(HardwareOptimization));
    if (!opt) return NULL;
    
    // Vectorization decisions
    opt->enable_vectorization = (profile->capabilities & (HW_CAP_SIMD_SSE | HW_CAP_SIMD_NEON)) != 0;
    if (profile->capabilities & HW_CAP_SIMD_AVX512) {
        opt->preferred_vector_width = 512;
        opt->use_gather_scatter = true;
        opt->enable_masked_vectorization = true;
    } else if (profile->capabilities & HW_CAP_SIMD_AVX2) {
        opt->preferred_vector_width = 256;
        opt->use_gather_scatter = false;
        opt->enable_masked_vectorization = false;
    } else if (profile->capabilities & HW_CAP_SIMD_NEON) {
        opt->preferred_vector_width = 128;
        opt->use_gather_scatter = false;
        opt->enable_masked_vectorization = false;
    } else {
        opt->preferred_vector_width = 64;
    }
    
    // Parallelization decisions
    opt->enable_auto_parallel = (profile->core_count > 1);
    opt->max_threads = get_optimal_thread_count(profile);
    opt->prefer_nested_parallel = (profile->core_count >= 8);
    opt->use_work_stealing = (profile->core_count >= 4);
    
    // Memory optimization decisions
    opt->enable_prefetching = (profile->capabilities & HW_CAP_PREFETCH) != 0;
    opt->align_to_cache_lines = true;
    opt->use_non_temporal_stores = (profile->l3_cache_size > 0);
    opt->prefetch_distance = profile->cache_line_size * 8; // 8 cache lines ahead
    
    // Algorithm selection based on hardware
    if (profile->capabilities & HW_CAP_SIMD_AVX512) {
        opt->sort_algorithm = strdup("radix_avx512");
    } else if (profile->capabilities & HW_CAP_SIMD_AVX2) {
        opt->sort_algorithm = strdup("quicksort_avx2");
    } else {
        opt->sort_algorithm = strdup("quicksort");
    }
    
    if (profile->capabilities & HW_CAP_AES_NI) {
        opt->hash_algorithm = strdup("aes_hash");
        opt->crypto_backend = strdup("aes_ni");
    } else {
        opt->hash_algorithm = strdup("xxhash");
        opt->crypto_backend = strdup("software");
    }
    
    opt->compression_algorithm = strdup("lz4"); // Fast, good for most cases
    
    // Code generation preferences
    opt->enable_branch_prediction_hints = true;
    opt->enable_loop_unrolling = true;
    opt->max_unroll_factor = profile->arch == ARCH_X86_64 ? 8 : 4;
    opt->enable_function_inlining = true;
    opt->enable_tail_call_optimization = true;
    
    // GPU offloading decisions
    opt->can_offload_to_gpu = profile->has_gpu;
    if (profile->capabilities & HW_CAP_GPU_CUDA) {
        opt->gpu_kernel_language = strdup("cuda");
    } else if (profile->capabilities & HW_CAP_GPU_OPENCL) {
        opt->gpu_kernel_language = strdup("opencl");
    } else if (profile->capabilities & HW_CAP_GPU_METAL) {
        opt->gpu_kernel_language = strdup("metal");
    }
    opt->min_data_size_for_gpu = 1024 * 1024; // 1MB minimum
    
    // Platform-specific optimizations
    opt->use_platform_intrinsics = true;
    opt->enable_security_mitigations = true;
    opt->optimize_for_size = false; // Default to speed
    opt->optimize_for_energy = (profile->thermal_design_power < 15); // Mobile/embedded
    
    return opt;
}

void update_optimization_for_workload(HardwareOptimization* opt, const char* workload_type) {
    if (!opt || !workload_type) return;
    
    if (strcmp(workload_type, "compute_intensive") == 0) {
        opt->enable_vectorization = true;
        opt->enable_auto_parallel = true;
        opt->can_offload_to_gpu = true;
        opt->enable_loop_unrolling = true;
        opt->max_unroll_factor *= 2;
    } else if (strcmp(workload_type, "memory_intensive") == 0) {
        opt->enable_prefetching = true;
        opt->use_non_temporal_stores = true;
        opt->align_to_cache_lines = true;
        opt->prefetch_distance *= 2;
    } else if (strcmp(workload_type, "latency_sensitive") == 0) {
        opt->enable_branch_prediction_hints = true;
        opt->enable_function_inlining = true;
        opt->max_threads = 1; // Avoid context switching overhead
        opt->optimize_for_size = false;
    } else if (strcmp(workload_type, "energy_efficient") == 0) {
        opt->optimize_for_energy = true;
        opt->max_threads /= 2; // Use fewer cores
        opt->enable_vectorization = false; // Vectorization can use more energy
        opt->can_offload_to_gpu = false;
    }
}

void free_hardware_optimization(HardwareOptimization* opt) {
    if (!opt) return;
    
    free(opt->sort_algorithm);
    free(opt->hash_algorithm);
    free(opt->crypto_backend);
    free(opt->compression_algorithm);
    free(opt->gpu_kernel_language);
    free(opt);
}

const char* select_optimal_algorithm(const HardwareProfile* profile, 
                                    const char* algorithm_class,
                                    size_t data_size) {
    if (!profile || !algorithm_class) return "generic";
    
    if (strcmp(algorithm_class, "sort") == 0) {
        if (data_size < 1000) {
            return "insertion_sort";
        } else if (profile->capabilities & HW_CAP_SIMD_AVX512) {
            return "radix_sort_avx512";
        } else if (profile->capabilities & HW_CAP_SIMD_AVX2) {
            return "quicksort_avx2";  
        } else if (data_size > 1000000) {
            return "merge_sort_parallel";
        } else {
            return "quicksort";
        }
    } else if (strcmp(algorithm_class, "hash") == 0) {
        if (profile->capabilities & HW_CAP_AES_NI) {
            return "aes_hash";
        } else if (profile->capabilities & HW_CAP_CRC32) {
            return "crc32_hash";
        } else {
            return "xxhash";
        }
    } else if (strcmp(algorithm_class, "compression") == 0) {
        if (data_size < 1024) {
            return "none";
        } else if (profile->thermal_design_power < 15) {
            return "lz4";
        } else {
            return "zstd";
        }
    }
    
    return "generic";
}

int select_optimal_vector_width(const HardwareProfile* profile, const char* operation) {
    if (!profile || !operation) return 64;
    
    // Check specific operation requirements
    if (strcmp(operation, "fp64_math") == 0) {
        // Double precision operations
        if (profile->capabilities & HW_CAP_SIMD_AVX512) {
            return 512; // 8 doubles
        } else if (profile->capabilities & HW_CAP_SIMD_AVX2) {
            return 256; // 4 doubles
        } else {
            return 128; // 2 doubles
        }
    } else if (strcmp(operation, "fp32_math") == 0) {
        // Single precision operations
        if (profile->capabilities & HW_CAP_SIMD_AVX512) {
            return 512; // 16 floats
        } else if (profile->capabilities & HW_CAP_SIMD_AVX2) {
            return 256; // 8 floats
        } else {
            return 128; // 4 floats
        }
    } else if (strcmp(operation, "integer") == 0) {
        // Integer operations
        return profile->simd_width_bits;
    }
    
    // Default to maximum available width
    return profile->simd_width_bits;
}

bool should_use_gpu_acceleration(const HardwareProfile* profile, 
                                size_t data_size, 
                                const char* operation) {
    if (!profile || !operation || !profile->has_gpu) {
        return false;
    }
    
    // Minimum data size threshold
    if (data_size < 1024 * 1024) { // 1MB
        return false;
    }
    
    // Check if operation is GPU-friendly
    if (strcmp(operation, "matrix_multiply") == 0 ||
        strcmp(operation, "vector_math") == 0 ||
        strcmp(operation, "image_processing") == 0 ||
        strcmp(operation, "parallel_reduction") == 0) {
        return true;
    }
    
    return false;
}

HardwareAwareContext* create_hardware_aware_context(void) {
    HardwareAwareContext* context = xcalloc(1, sizeof(HardwareAwareContext));
    if (!context) return NULL;
    
    context->enable_runtime_adaptation = true;
    context->adaptation_threshold = 0.1; // 10% performance change threshold
    context->last_profile_update = time(NULL);
    
    return context;
}

bool initialize_hardware_context(HardwareAwareContext* context) {
    if (!context) return false;
    
    // Detect hardware profile
    context->profile = detect_hardware_profile();
    if (!context->profile) return false;
    
    // Create optimization settings
    context->optimization = create_hardware_optimization(context->profile);
    if (!context->optimization) return false;
    
    // Initialize strategy manager
    context->strategy_manager = create_strategy_manager();
    if (!context->strategy_manager) return false;
    
    // Run initial benchmarks
    run_hardware_benchmarks(context);
    
    return true;
}

void update_hardware_context(HardwareAwareContext* context) {
    if (!context) return;
    
    time_t now = time(NULL);
    
    // Update profile periodically (e.g., thermal state changes)
    if (now - context->last_profile_update > 60) { // 1 minute
        update_hardware_profile(context->profile);
        context->last_profile_update = now;
    }
    
    // Runtime adaptation based on performance counters
    if (context->enable_runtime_adaptation) {
        adapt_to_runtime_conditions(context);
    }
}

void free_hardware_aware_context(HardwareAwareContext* context) {
    if (!context) return;
    
    free_hardware_profile(context->profile);
    free_hardware_optimization(context->optimization);
    destroy_strategy_manager(context->strategy_manager);
    free(context->benchmark_results);
    free(context);
}

// Get the global hardware context (thread-safe)
HardwareAwareContext* get_global_hardware_context(void) {
    pthread_once(&g_hardware_init_once, initialize_global_hardware_context);
    return g_hardware_context;
}

bool apply_hardware_optimizations(ASTNode* node, HardwareAwareContext* context) {
    if (!node || !context) return false;
    
    pthread_mutex_lock(&g_hardware_mutex);
    
    // Apply different optimizations based on node type
    switch (node->type) {
        case AST_FUNC_DECL:
            // Function-level optimizations
            if (context->optimization->enable_function_inlining) {
                // Mark small functions for inlining
                // This would be implemented in the code generator
            }
            break;
            
        case AST_FOR_STMT:
            // Loop optimizations
            if (context->optimization->enable_vectorization) {
                // Mark loops for vectorization
                // This would be implemented in the code generator
            }
            if (context->optimization->enable_loop_unrolling) {
                // Apply loop unrolling hints
            }
            break;
            
        case AST_BINARY_EXPR:
            // Memory access optimizations (placeholder)
            if (context->optimization->enable_prefetching) {
                // Insert prefetch instructions
            }
            break;
            
        default:
            break;
    }
    
    pthread_mutex_unlock(&g_hardware_mutex);
    return true;
}

void generate_hardware_specific_code(ASTNode* node, HardwareAwareContext* context) {
    if (!node || !context) return;
    
    switch (context->profile->arch) {
        case ARCH_X86_64:
            generate_x86_64_optimized_code(node, context);
            break;
        case ARCH_AARCH64:
            generate_aarch64_optimized_code(node, context);
            break;
        case ARCH_WASM:
            generate_wasm_optimized_code(node, context);
            break;
        default:
            // Generate generic code
            break;
    }
}

void insert_hardware_intrinsics(ASTNode* node, HardwareAwareContext* context) {
    if (!node || !context || !context->optimization->use_platform_intrinsics) {
        return;
    }
    
    // This is a simplified example - in practice, this would be much more complex
    // and integrated with the code generator
    
    if (node->type == AST_FOR_STMT && context->optimization->enable_vectorization) {
        // Insert SIMD intrinsics for vectorizable loops
        if (context->profile->capabilities & HW_CAP_SIMD_AVX2) {
            // Insert AVX2 intrinsics
        } else if (context->profile->capabilities & HW_CAP_SIMD_NEON) {
            // Insert NEON intrinsics
        }
    }
}

bool run_hardware_benchmarks(HardwareAwareContext* context) {
    if (!context) return false;
    
    // Simplified benchmarking - in practice, this would run micro-benchmarks
    // to determine optimal parameters for the specific hardware
    
    context->benchmark_count = 5;
    context->benchmark_results = calloc(context->benchmark_count, sizeof(double));
    if (!context->benchmark_results) return false;
    
    // Simulate benchmark results
    context->benchmark_results[0] = 1.0; // Memory bandwidth score
    context->benchmark_results[1] = 1.2; // SIMD performance score  
    context->benchmark_results[2] = 0.9; // Branch prediction score
    context->benchmark_results[3] = 1.1; // Cache performance score
    context->benchmark_results[4] = 1.3; // Parallel performance score
    
    return true;
}

bool adapt_to_runtime_conditions(HardwareAwareContext* context) {
    if (!context) return false;
    
    // This would monitor performance counters and adapt optimization strategies
    // For now, it's a placeholder
    return true;
}

// Compile-time intrinsics implementation
ComptimeValue* comptime_target_has_capability(ComptimeValue* capability) {
    if (!capability || capability->type != COMPTIME_VALUE_STRING) {
        return NULL;
    }
    
    HardwareAwareContext* context = get_global_hardware_context();
    if (!context || !context->profile) {
        return NULL;
    }
    
    const char* cap_name = capability->string_value;
    bool has_cap = false;
    
    if (strcmp(cap_name, "avx2") == 0) {
        has_cap = has_hardware_capability(context->profile, HW_CAP_SIMD_AVX2);
    } else if (strcmp(cap_name, "avx512") == 0) {
        has_cap = has_hardware_capability(context->profile, HW_CAP_SIMD_AVX512);
    } else if (strcmp(cap_name, "neon") == 0) {
        has_cap = has_hardware_capability(context->profile, HW_CAP_SIMD_NEON);
    } else if (strcmp(cap_name, "aes_ni") == 0) {
        has_cap = has_hardware_capability(context->profile, HW_CAP_AES_NI);
    } else if (strcmp(cap_name, "gpu") == 0) {
        has_cap = context->profile->has_gpu;
    }
    
    ComptimeValue* result = xmalloc(sizeof(ComptimeValue));
    if (!result) return NULL;
    
    result->type = COMPTIME_VALUE_BOOL;
    result->bool_value = has_cap;
    return result;
}

ComptimeValue* comptime_get_optimal_vector_width(ComptimeValue* operation) {
    if (!operation || operation->type != COMPTIME_VALUE_STRING) {
        return NULL;
    }
    
    HardwareAwareContext* context = get_global_hardware_context();
    if (!context || !context->profile) {
        return NULL;
    }
    
    int width = select_optimal_vector_width(context->profile, operation->string_value);
    
    ComptimeValue* result = xmalloc(sizeof(ComptimeValue));
    if (!result) return NULL;
    
    result->type = COMPTIME_VALUE_INT;
    result->int_value = width;
    return result;
}

ComptimeValue* comptime_get_core_count(void) {
    HardwareAwareContext* context = get_global_hardware_context();
    if (!context || !context->profile) {
        return NULL;
    }
    
    ComptimeValue* result = xmalloc(sizeof(ComptimeValue));
    if (!result) return NULL;
    
    result->type = COMPTIME_VALUE_INT;
    result->int_value = context->profile->core_count;
    return result;
}

// Debug and reporting functions
void print_hardware_profile(const HardwareProfile* profile) {
    if (!profile) return;
    
    printf("Hardware Profile:\n");
    printf("  Architecture: %s\n", 
           profile->arch == ARCH_X86_64 ? "x86_64" :
           profile->arch == ARCH_AARCH64 ? "aarch64" :
           profile->arch == ARCH_RISCV64 ? "riscv64" :
           profile->arch == ARCH_WASM ? "wasm" : "unknown");
    
    printf("  CPU Cores: %d\n", profile->core_count);
    printf("  CPU Threads: %d\n", profile->thread_count);
    printf("  SIMD Width: %d bits\n", profile->simd_width_bits);
    printf("  Cache Line Size: %d bytes\n", profile->cache_line_size);
    printf("  L3 Cache Size: %d KB\n", profile->l3_cache_size / 1024);
    
    printf("  Capabilities:\n");
    if (profile->capabilities & HW_CAP_SIMD_AVX2) printf("    AVX2\n");
    if (profile->capabilities & HW_CAP_SIMD_AVX512) printf("    AVX512\n");
    if (profile->capabilities & HW_CAP_SIMD_NEON) printf("    NEON\n");
    if (profile->capabilities & HW_CAP_AES_NI) printf("    AES-NI\n");
    if (profile->capabilities & HW_CAP_GPU_CUDA) printf("    CUDA GPU\n");
    
    printf("  Target Triple: %s\n", profile->target_triple ? profile->target_triple : "unknown");
    printf("  Feature String: %s\n", profile->feature_string ? profile->feature_string : "");
}

void print_hardware_optimization_report(const HardwareOptimization* opt) {
    if (!opt) return;
    
    printf("Hardware Optimization Settings:\n");
    printf("  Vectorization: %s (width: %d)\n", 
           opt->enable_vectorization ? "enabled" : "disabled",
           opt->preferred_vector_width);
    printf("  Auto Parallelization: %s (max threads: %d)\n",
           opt->enable_auto_parallel ? "enabled" : "disabled",
           opt->max_threads);
    printf("  Memory Prefetching: %s\n",
           opt->enable_prefetching ? "enabled" : "disabled");
    printf("  GPU Offloading: %s\n",
           opt->can_offload_to_gpu ? "enabled" : "disabled");
    printf("  Sort Algorithm: %s\n", opt->sort_algorithm ? opt->sort_algorithm : "generic");
    printf("  Hash Algorithm: %s\n", opt->hash_algorithm ? opt->hash_algorithm : "generic");
    printf("  Crypto Backend: %s\n", opt->crypto_backend ? opt->crypto_backend : "software");
}

// Placeholder implementations for target-specific code generation
void generate_x86_64_optimized_code(ASTNode* node __attribute__((unused)), 
                                   HardwareAwareContext* context __attribute__((unused))) {
    // This would generate x86_64-specific optimized code
}

void generate_aarch64_optimized_code(ASTNode* node __attribute__((unused)), 
                                   HardwareAwareContext* context __attribute__((unused))) {
    // This would generate AArch64-specific optimized code  
}

void generate_gpu_kernel_code(ASTNode* node __attribute__((unused)), 
                            HardwareAwareContext* context __attribute__((unused))) {
    // This would generate GPU kernel code
}

void generate_wasm_optimized_code(ASTNode* node __attribute__((unused)), 
                                HardwareAwareContext* context __attribute__((unused))) {
    // This would generate WebAssembly-optimized code
}