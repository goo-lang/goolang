#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "runtime_optimization.h"

// Define ANSI color codes
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

// =============================================================================
// Demo Helper Functions
// =============================================================================

void print_section(const char* title) {
    printf("\n" ANSI_COLOR_CYAN "===== %s =====" ANSI_COLOR_RESET "\n", title);
}

void print_optimization_stats(OptimizationContext* ctx) {
    printf("\n" ANSI_COLOR_GREEN "Optimization Statistics:" ANSI_COLOR_RESET "\n");
    printf("  Bounds checks eliminated: %zu\n", ctx->bounds_checks_eliminated);
    printf("  Null checks eliminated: %zu\n", ctx->null_checks_eliminated);
    printf("  Branches optimized: %zu\n", ctx->branches_optimized);
    printf("  Loops optimized: %zu\n", ctx->loops_optimized);
    printf("  Speculation hits: %zu\n", ctx->speculation_hits);
    printf("  Speculation misses: %zu\n", ctx->speculation_misses);
}

void print_hardware_capabilities(HardwareCapabilities caps) {
    printf("\n" ANSI_COLOR_BLUE "Hardware Capabilities:" ANSI_COLOR_RESET "\n");
    
    if (caps & HW_CAP_INTEL_MPX) {
        printf("  ✓ Intel Memory Protection Extensions\n");
    }
    if (caps & HW_CAP_ARM_MTE) {
        printf("  ✓ ARM Memory Tagging Extensions\n");
    }
    if (caps & HW_CAP_INTEL_CET) {
        printf("  ✓ Intel Control-flow Enforcement\n");
    }
    if (caps & HW_CAP_ARM_BTI) {
        printf("  ✓ ARM Branch Target Identification\n");
    }
    if (caps & HW_CAP_AVX512) {
        printf("  ✓ AVX-512 Vectorization\n");
    }
    if (caps & HW_CAP_NEON) {
        printf("  ✓ ARM NEON Vectorization\n");
    }
    if (caps & HW_CAP_PREFETCH) {
        printf("  ✓ Hardware Prefetch\n");
    }
    if (caps & HW_CAP_SPECULATION) {
        printf("  ✓ Speculative Execution\n");
    }
    
    if (caps == HW_CAP_NONE) {
        printf("  No advanced hardware capabilities detected\n");
    }
}

// =============================================================================
// Performance Simulation Functions
// =============================================================================

// Simulate an optimization scenario
void demo_basic_optimization(OptimizationContext* ctx) {
    print_section("Basic Optimization Demo");
    
    printf("Simulating optimization scenarios...\n");
    
    // Just increment the counters to simulate successful optimizations
    ctx->bounds_checks_eliminated += 10;
    ctx->null_checks_eliminated += 5;
    ctx->branches_optimized += 3;
    ctx->loops_optimized += 2;
    
    printf("Simulated 20 optimization opportunities:\n");
    printf("  - 10 bounds checks eliminated\n");
    printf("  - 5 null checks eliminated\n");
    printf("  - 3 branches optimized\n");
    printf("  - 2 loops optimized\n");
}

// Simulate speculative execution
void demo_speculative_execution(OptimizationContext* ctx) {
    print_section("Speculative Execution Demo");
    
    printf("Creating speculative execution context...\n");
    
    // Create a speculation context (we pass NULL since we don't have a real AST node)
    SpeculationContext* spec_ctx = speculation_context_new(NULL);
    if (!spec_ctx) {
        printf("Failed to create speculation context\n");
        return;
    }
    
    printf("Beginning speculation...\n");
    int result = begin_speculation(ctx, spec_ctx);
    
    if (result == 0) {
        ctx->speculation_hits++;
        printf("✓ Speculation began successfully\n");
        
        // Commit the speculation
        if (commit_speculation(ctx, spec_ctx) == 0) {
            printf("✓ Speculation committed\n");
        } else {
            printf("✗ Failed to commit speculation\n");
        }
    } else {
        ctx->speculation_misses++;
        printf("✗ Speculation failed to begin\n");
    }
    
    speculation_context_free(spec_ctx);
}

// Simulate adaptive optimization
void demo_adaptive_optimization(OptimizationContext* ctx) {
    print_section("Adaptive Optimization Demo");
    
    printf("Creating adaptive optimizer...\n");
    
    AdaptiveOptimizer* optimizer = adaptive_optimizer_new(ctx);
    if (!optimizer) {
        printf("Failed to create adaptive optimizer\n");
        return;
    }
    
    // Simulate multiple optimization cycles
    for (int cycle = 0; cycle < 5; cycle++) {
        printf("Optimization cycle %d: ", cycle + 1);
        
        int result = adaptive_optimizer_update(optimizer);
        
        if (result == 0) {
            printf("SUCCESS - Optimizer updated\n");
            ctx->loops_optimized++;
        } else {
            printf("SKIPPED - No update needed\n");
        }
    }
    
    adaptive_optimizer_free(optimizer);
}

// Simulate profile-guided optimization
void demo_profile_guided_optimization(OptimizationContext* ctx) {
    print_section("Profile-Guided Optimization Demo");
    
    printf("Collecting runtime profiles...\n");
    
    // Simulate profile data collection for multiple functions
    const char* function_names[] = {
        "main_loop", "process_data", "validate_input", "optimize_query", "render_frame"
    };
    
    int collected = 0;
    for (size_t i = 0; i < sizeof(function_names) / sizeof(function_names[0]); i++) {
        if (collect_runtime_profile(ctx, function_names[i]) == 0) {
            collected++;
        }
    }
    
    printf("Collected profiles for %d out of %zu functions\n", 
           collected, sizeof(function_names) / sizeof(function_names[0]));
    
    // Create and apply profile data
    ProfileData* profile = profile_data_new("hot_function");
    if (profile) {
        int result = apply_profile_guided_optimization(ctx, profile);
        if (result == 0) {
            printf("✓ Applied profile-guided optimization\n");
        } else {
            printf("✗ Failed to apply profile-guided optimization\n");
        }
        profile_data_free(profile);
    }
}

// =============================================================================
// Main Demo Function
// =============================================================================

int main() {
    printf(ANSI_COLOR_CYAN "Runtime Optimization Framework Demo" ANSI_COLOR_RESET "\n");
    printf("=========================================\n\n");
    
    // Test different safety levels
    OptimizationSafetyLevel safety_levels[] = {
        OPT_SAFETY_CONSERVATIVE,
        OPT_SAFETY_BALANCED,
        OPT_SAFETY_AGGRESSIVE
    };
    
    const char* safety_names[] = {
        "Conservative",
        "Balanced", 
        "Aggressive"
    };
    
    for (int level = 0; level < 3; level++) {
        printf("\n" ANSI_COLOR_YELLOW "Testing %s Safety Level" ANSI_COLOR_RESET "\n", 
               safety_names[level]);
        printf("==============================\n");
        
        // Create optimization context
        OptimizationContext* ctx = optimization_context_new(safety_levels[level]);
        if (!ctx) {
            printf("Failed to create optimization context\n");
            continue;
        }
        
        // Display hardware capabilities
        print_hardware_capabilities(ctx->hw_capabilities);
        
        // Run optimization demos
        demo_basic_optimization(ctx);
        demo_speculative_execution(ctx);
        demo_adaptive_optimization(ctx);
        demo_profile_guided_optimization(ctx);
        
        // Show final statistics
        print_optimization_stats(ctx);
        
        // Test diagnostics
        print_section("Diagnostics Test");
        
        // Test error handling by causing an error
        snprintf(ctx->last_error, sizeof(ctx->last_error), "Demo diagnostic message");
        ctx->error_count++;
        
        printf("Recorded diagnostic message: %s\n", ctx->last_error);
        printf("Total error count: %d\n", ctx->error_count);
        
        // Cleanup
        optimization_context_free(ctx);
        
        if (level < 2) {
            printf("\nPress Enter to continue to next safety level...");
            getchar();
        }
    }
    
    printf("\n" ANSI_COLOR_GREEN "Demo completed successfully!" ANSI_COLOR_RESET "\n");
    return 0;
}
