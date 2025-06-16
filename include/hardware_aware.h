#ifndef HARDWARE_AWARE_H
#define HARDWARE_AWARE_H

#include "ast.h"
#include "types.h"
#include "comptime.h"
#include "advanced_optimization.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Hardware capability flags
typedef enum {
    HW_CAP_NONE = 0,
    HW_CAP_SIMD_SSE = 1 << 0,
    HW_CAP_SIMD_AVX = 1 << 1,
    HW_CAP_SIMD_AVX2 = 1 << 2,
    HW_CAP_SIMD_AVX512 = 1 << 3,
    HW_CAP_SIMD_NEON = 1 << 4,         // ARM NEON
    HW_CAP_SIMD_SVE = 1 << 5,          // ARM SVE
    HW_CAP_GPU_CUDA = 1 << 6,
    HW_CAP_GPU_OPENCL = 1 << 7,
    HW_CAP_GPU_METAL = 1 << 8,
    HW_CAP_AES_NI = 1 << 9,            // Hardware AES
    HW_CAP_SHA_EXT = 1 << 10,          // Hardware SHA
    HW_CAP_CRC32 = 1 << 11,            // Hardware CRC32
    HW_CAP_RDRAND = 1 << 12,           // Hardware random number generation
    HW_CAP_BMI = 1 << 13,              // Bit manipulation instructions
    HW_CAP_FMA = 1 << 14,              // Fused multiply-add
    HW_CAP_PREFETCH = 1 << 15,         // Software prefetch instructions
    HW_CAP_CACHE_64 = 1 << 16,         // 64-byte cache lines
    HW_CAP_CACHE_128 = 1 << 17,        // 128-byte cache lines
    HW_CAP_NUMA = 1 << 18,             // NUMA topology
    HW_CAP_HYPERTHREADING = 1 << 19,   // SMT/Hyperthreading
    HW_CAP_ATOMIC_128 = 1 << 20,       // 128-bit atomic operations
    HW_CAP_MEMORY_TAGGING = 1 << 21,   // ARM Memory Tagging Extension
    HW_CAP_BRANCH_TARGET = 1 << 22,    // Branch target identification
    HW_CAP_SPECULATION = 1 << 23,      // Speculative execution controls
} HardwareCapability;

// Architecture types
typedef enum {
    ARCH_X86_64,
    ARCH_AARCH64,
    ARCH_RISCV64,
    ARCH_WASM,
    ARCH_UNKNOWN
} TargetArchitecture;

// CPU microarchitecture families
typedef enum {
    UARCH_INTEL_SKYLAKE,
    UARCH_INTEL_ICE_LAKE,
    UARCH_INTEL_ALDERLAKE,
    UARCH_AMD_ZEN3,
    UARCH_AMD_ZEN4,
    UARCH_ARM_CORTEX_A78,
    UARCH_ARM_CORTEX_X1,
    UARCH_ARM_NEOVERSE_N1,
    UARCH_APPLE_M1,
    UARCH_APPLE_M2,
    UARCH_UNKNOWN
} CPUMicroarchitecture;

// Hardware profile
typedef struct {
    TargetArchitecture arch;
    CPUMicroarchitecture microarch;
    uint64_t capabilities;              // Bitmask of HardwareCapability
    
    // Performance characteristics
    int core_count;
    int thread_count;
    int cache_line_size;
    int l1_cache_size;
    int l2_cache_size;
    int l3_cache_size;
    int memory_bandwidth_gbps;
    int simd_width_bits;
    
    // GPU information (if available)
    bool has_gpu;
    char* gpu_name;
    int gpu_compute_units;
    int gpu_memory_mb;
    
    // NUMA topology
    bool is_numa;
    int numa_nodes;
    int cores_per_node;
    
    // Thermal and power constraints
    int thermal_design_power;           // TDP in watts
    bool has_turbo_boost;
    bool has_power_gating;
    
    // Compiler-specific features
    char* target_triple;                // LLVM target triple
    char* cpu_name;                     // CPU name for LLVM
    char* feature_string;               // LLVM feature string
} HardwareProfile;

// Hardware-aware optimization decisions
typedef struct {
    // Vectorization decisions
    bool enable_vectorization;
    int preferred_vector_width;
    bool use_gather_scatter;
    bool enable_masked_vectorization;
    
    // Parallelization decisions
    bool enable_auto_parallel;
    int max_threads;
    bool prefer_nested_parallel;
    bool use_work_stealing;
    
    // Memory optimization decisions
    bool enable_prefetching;
    bool align_to_cache_lines;
    bool use_non_temporal_stores;
    int prefetch_distance;
    
    // Algorithm selection
    char* sort_algorithm;               // "quicksort", "radix", "counting", etc.
    char* hash_algorithm;               // "xxhash", "cityhash", "aes_hash", etc.
    char* crypto_backend;               // "software", "aes_ni", "dedicated", etc.
    char* compression_algorithm;        // "lz4", "zstd", "hardware", etc.
    
    // Code generation preferences
    bool enable_branch_prediction_hints;
    bool enable_loop_unrolling;
    int max_unroll_factor;
    bool enable_function_inlining;
    bool enable_tail_call_optimization;
    
    // GPU offloading decisions
    bool can_offload_to_gpu;
    char* gpu_kernel_language;          // "cuda", "opencl", "metal", etc.
    int min_data_size_for_gpu;
    
    // Platform-specific optimizations
    bool use_platform_intrinsics;
    bool enable_security_mitigations;
    bool optimize_for_size;
    bool optimize_for_energy;
} HardwareOptimization;

// Hardware detection and profiling
typedef struct {
    HardwareProfile* profile;
    HardwareOptimization* optimization;
    StrategyManager* strategy_manager;
    
    // Benchmarking results
    double* benchmark_results;
    size_t benchmark_count;
    
    // Runtime adaptation
    bool enable_runtime_adaptation;
    double adaptation_threshold;
    time_t last_profile_update;
} HardwareAwareContext;

// Function declarations

// Hardware detection
HardwareProfile* detect_hardware_profile(void);
bool update_hardware_profile(HardwareProfile* profile);
void free_hardware_profile(HardwareProfile* profile);

// Capability checking
bool has_hardware_capability(const HardwareProfile* profile, HardwareCapability cap);
bool check_simd_support(const HardwareProfile* profile, int required_width);
bool check_gpu_capability(const HardwareProfile* profile, const char* api);
int get_optimal_thread_count(const HardwareProfile* profile);

// Optimization decision making
HardwareOptimization* create_hardware_optimization(const HardwareProfile* profile);
void update_optimization_for_workload(HardwareOptimization* opt, const char* workload_type);
void free_hardware_optimization(HardwareOptimization* opt);

// Algorithm selection
const char* select_optimal_algorithm(const HardwareProfile* profile, 
                                    const char* algorithm_class,
                                    size_t data_size);
int select_optimal_vector_width(const HardwareProfile* profile, const char* operation);
bool should_use_gpu_acceleration(const HardwareProfile* profile, 
                                size_t data_size, 
                                const char* operation);

// Context management
HardwareAwareContext* create_hardware_aware_context(void);
bool initialize_hardware_context(HardwareAwareContext* context);
void update_hardware_context(HardwareAwareContext* context);
void free_hardware_aware_context(HardwareAwareContext* context);

// Compile-time integration
bool apply_hardware_optimizations(ASTNode* node, HardwareAwareContext* context);
void generate_hardware_specific_code(ASTNode* node, HardwareAwareContext* context);
void insert_hardware_intrinsics(ASTNode* node, HardwareAwareContext* context);

// Runtime adaptation
bool adapt_to_runtime_conditions(HardwareAwareContext* context);
void update_performance_counters(HardwareAwareContext* context);
bool should_recompile_for_hardware(HardwareAwareContext* context);

// Benchmarking and profiling
bool run_hardware_benchmarks(HardwareAwareContext* context);
double benchmark_algorithm_variants(const HardwareProfile* profile,
                                   const char* algorithm_class,
                                   void* test_data,
                                   size_t data_size);
void profile_memory_access_patterns(HardwareAwareContext* context, ASTNode* node);

// Target-specific code generation
void generate_x86_64_optimized_code(ASTNode* node, HardwareAwareContext* context);
void generate_aarch64_optimized_code(ASTNode* node, HardwareAwareContext* context);
void generate_gpu_kernel_code(ASTNode* node, HardwareAwareContext* context);
void generate_wasm_optimized_code(ASTNode* node, HardwareAwareContext* context);

// Compile-time intrinsics for hardware awareness
ComptimeValue* comptime_target_has_capability(ComptimeValue* capability);
ComptimeValue* comptime_get_optimal_vector_width(ComptimeValue* operation);
ComptimeValue* comptime_select_algorithm(ComptimeValue* algorithm_class, ComptimeValue* data_size);
ComptimeValue* comptime_enable_gpu_offload(ComptimeValue* min_size);
ComptimeValue* comptime_get_cache_line_size(void);
ComptimeValue* comptime_get_core_count(void);
ComptimeValue* comptime_optimize_for_target(ComptimeValue* optimization_goal);

// Debug and reporting
void print_hardware_profile(const HardwareProfile* profile);
void print_hardware_optimization_report(const HardwareOptimization* opt);
char* get_hardware_summary(const HardwareAwareContext* context);
void export_hardware_profile_json(const HardwareProfile* profile, const char* filename);

// Configuration and tuning
typedef struct {
    bool enable_aggressive_vectorization;
    bool enable_gpu_offloading;
    bool enable_runtime_adaptation;
    bool enable_branch_prediction;
    bool prefer_throughput_over_latency;
    bool optimize_for_mobile;
    double energy_efficiency_weight;
    int max_compile_time_seconds;
} HardwareAwareConfig;

HardwareAwareConfig* create_hardware_config(void);
void load_hardware_config(HardwareAwareConfig* config, const char* filename);
void save_hardware_config(const HardwareAwareConfig* config, const char* filename);
void free_hardware_config(HardwareAwareConfig* config);

#endif // HARDWARE_AWARE_H