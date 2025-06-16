#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "hardware_aware.h"

// Test hardware profile detection
void test_hardware_profile_detection(void) {
    printf("Testing hardware profile detection...\n");
    
    HardwareProfile* profile = detect_hardware_profile();
    assert(profile != NULL);
    
    // Verify basic profile information
    assert(profile->arch != ARCH_UNKNOWN);
    assert(profile->core_count > 0);
    assert(profile->thread_count > 0);
    assert(profile->simd_width_bits >= 64);
    assert(profile->cache_line_size > 0);
    
    // Verify strings are allocated
    assert(profile->target_triple != NULL);
    assert(profile->cpu_name != NULL);
    assert(profile->feature_string != NULL);
    
    printf("  Architecture: %d\n", profile->arch);
    printf("  Cores: %d\n", profile->core_count);
    printf("  SIMD width: %d bits\n", profile->simd_width_bits);
    printf("  Target triple: %s\n", profile->target_triple);
    
    free_hardware_profile(profile);
    printf("Hardware profile detection test passed!\n\n");
}

// Test capability checking
void test_capability_checking(void) {
    printf("Testing capability checking...\n");
    
    HardwareProfile* profile = detect_hardware_profile();
    assert(profile != NULL);
    
    // Test SIMD support checking
    bool simd_64 = check_simd_support(profile, 64);
    bool simd_1024 = check_simd_support(profile, 1024);
    
    assert(simd_64 == true);  // Should always support 64-bit SIMD
    // simd_1024 may or may not be supported depending on hardware
    
    // Test capability flags
    bool has_some_simd = has_hardware_capability(profile, HW_CAP_SIMD_SSE) ||
                        has_hardware_capability(profile, HW_CAP_SIMD_NEON);
    
    printf("  64-bit SIMD: %s\n", simd_64 ? "supported" : "not supported");
    printf("  1024-bit SIMD: %s\n", simd_1024 ? "supported" : "not supported");
    printf("  Has SIMD: %s\n", has_some_simd ? "yes" : "no");
    
    free_hardware_profile(profile);
    printf("Capability checking test passed!\n\n");
}

// Test optimization creation
void test_optimization_creation(void) {
    printf("Testing optimization creation...\n");
    
    HardwareProfile* profile = detect_hardware_profile();
    assert(profile != NULL);
    
    HardwareOptimization* opt = create_hardware_optimization(profile);
    assert(opt != NULL);
    
    // Verify optimization settings are reasonable
    assert(opt->preferred_vector_width >= 64);
    assert(opt->max_threads > 0);
    assert(opt->max_unroll_factor > 0);
    assert(opt->sort_algorithm != NULL);
    assert(opt->hash_algorithm != NULL);
    assert(opt->crypto_backend != NULL);
    
    printf("  Vectorization: %s (width: %d)\n", 
           opt->enable_vectorization ? "enabled" : "disabled",
           opt->preferred_vector_width);
    printf("  Max threads: %d\n", opt->max_threads);
    printf("  Sort algorithm: %s\n", opt->sort_algorithm);
    printf("  Hash algorithm: %s\n", opt->hash_algorithm);
    
    // Test workload-specific optimization
    update_optimization_for_workload(opt, "compute_intensive");
    assert(opt->enable_vectorization == true);
    assert(opt->enable_auto_parallel == true);
    
    update_optimization_for_workload(opt, "energy_efficient");
    assert(opt->optimize_for_energy == true);
    
    free_hardware_optimization(opt);
    free_hardware_profile(profile);
    printf("Optimization creation test passed!\n\n");
}

// Test algorithm selection
void test_algorithm_selection(void) {
    printf("Testing algorithm selection...\n");
    
    HardwareProfile* profile = detect_hardware_profile();
    assert(profile != NULL);
    
    // Test sort algorithm selection
    const char* small_sort = select_optimal_algorithm(profile, "sort", 100);
    const char* large_sort = select_optimal_algorithm(profile, "sort", 1000000);
    
    assert(small_sort != NULL);
    assert(large_sort != NULL);
    
    printf("  Small data sort: %s\n", small_sort);
    printf("  Large data sort: %s\n", large_sort);
    
    // Test hash algorithm selection
    const char* hash_algo = select_optimal_algorithm(profile, "hash", 1024);
    assert(hash_algo != NULL);
    printf("  Hash algorithm: %s\n", hash_algo);
    
    // Test vector width selection
    int fp32_width = select_optimal_vector_width(profile, "fp32_math");
    int fp64_width = select_optimal_vector_width(profile, "fp64_math");
    
    assert(fp32_width >= 64);
    assert(fp64_width >= 64);
    
    printf("  FP32 vector width: %d\n", fp32_width);
    printf("  FP64 vector width: %d\n", fp64_width);
    
    free_hardware_profile(profile);
    printf("Algorithm selection test passed!\n\n");
}

// Test GPU capability checking
void test_gpu_capability(void) {
    printf("Testing GPU capability...\n");
    
    HardwareProfile* profile = detect_hardware_profile();
    assert(profile != NULL);
    
    // Test GPU acceleration decision
    bool small_gpu = should_use_gpu_acceleration(profile, 1024, "matrix_multiply");
    bool large_gpu = should_use_gpu_acceleration(profile, 10*1024*1024, "matrix_multiply");
    bool seq_gpu = should_use_gpu_acceleration(profile, 10*1024*1024, "sequential_search");
    
    printf("  GPU for small matrix: %s\n", small_gpu ? "yes" : "no");
    printf("  GPU for large matrix: %s\n", large_gpu ? "yes" : "no");
    printf("  GPU for sequential search: %s\n", seq_gpu ? "yes" : "no");
    
    // Small data should not use GPU
    assert(small_gpu == false);
    
    free_hardware_profile(profile);
    printf("GPU capability test passed!\n\n");
}

// Test hardware context
void test_hardware_context(void) {
    printf("Testing hardware context...\n");
    
    HardwareAwareContext* context = create_hardware_aware_context();
    assert(context != NULL);
    
    bool init_success = initialize_hardware_context(context);
    assert(init_success == true);
    
    assert(context->profile != NULL);
    assert(context->optimization != NULL);
    assert(context->strategy_manager != NULL);
    
    printf("  Context initialized successfully\n");
    printf("  Profile arch: %d\n", context->profile->arch);
    printf("  Optimization vectorization: %s\n", 
           context->optimization->enable_vectorization ? "enabled" : "disabled");
    
    // Test context update
    update_hardware_context(context);
    printf("  Context updated successfully\n");
    
    free_hardware_aware_context(context);
    printf("Hardware context test passed!\n\n");
}

// Test compile-time intrinsics
void test_comptime_intrinsics(void) {
    printf("Testing compile-time intrinsics...\n");
    
    // Create test values
    ComptimeValue* avx2_cap = malloc(sizeof(ComptimeValue));
    avx2_cap->type = COMPTIME_VALUE_STRING;
    avx2_cap->string_value = "avx2";
    
    ComptimeValue* fp32_op = malloc(sizeof(ComptimeValue));
    fp32_op->type = COMPTIME_VALUE_STRING;
    fp32_op->string_value = "fp32_math";
    
    // Test capability checking
    ComptimeValue* has_avx2 = comptime_target_has_capability(avx2_cap);
    if (has_avx2) {
        assert(has_avx2->type == COMPTIME_VALUE_BOOL);
        printf("  AVX2 capability: %s\n", has_avx2->bool_value ? "yes" : "no");
        free(has_avx2);
    }
    
    // Test vector width query
    ComptimeValue* vector_width = comptime_get_optimal_vector_width(fp32_op);
    if (vector_width) {
        assert(vector_width->type == COMPTIME_VALUE_INT);
        assert(vector_width->int_value >= 64);
        printf("  Optimal FP32 vector width: %d\n", (int)vector_width->int_value);
        free(vector_width);
    }
    
    // Test core count query
    ComptimeValue* core_count = comptime_get_core_count();
    if (core_count) {
        assert(core_count->type == COMPTIME_VALUE_INT);
        assert(core_count->int_value > 0);
        printf("  Core count: %d\n", (int)core_count->int_value);
        free(core_count);
    }
    
    free(avx2_cap);
    free(fp32_op);
    printf("Compile-time intrinsics test passed!\n\n");
}

// Test hardware reporting
void test_hardware_reporting(void) {
    printf("Testing hardware reporting...\n");
    
    HardwareProfile* profile = detect_hardware_profile();
    assert(profile != NULL);
    
    HardwareOptimization* opt = create_hardware_optimization(profile);
    assert(opt != NULL);
    
    printf("--- Hardware Profile Report ---\n");
    print_hardware_profile(profile);
    
    printf("--- Hardware Optimization Report ---\n");
    print_hardware_optimization_report(opt);
    
    free_hardware_optimization(opt);
    free_hardware_profile(profile);
    printf("Hardware reporting test passed!\n\n");
}

int main(void) {
    printf("=== Hardware-Aware Compilation Test Suite ===\n\n");
    
    test_hardware_profile_detection();
    test_capability_checking();
    test_optimization_creation();
    test_algorithm_selection();
    test_gpu_capability();
    test_hardware_context();
    test_comptime_intrinsics();
    test_hardware_reporting();
    
    printf("=== All Hardware-Aware Tests Passed! ===\n");
    return 0;
}