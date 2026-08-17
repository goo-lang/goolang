#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "runtime_optimization.h"
#include "proof_generation.h"
#include "contracts.h"

// =============================================================================
// Demo: Runtime Optimization Framework
// =============================================================================

// Example function with bounds checks that can be eliminated
void process_array_safe(int* array, size_t size) {
    // The compiler can prove these bounds are safe
    for (size_t i = 0; i < size; i++) {
        array[i] *= 2; // Bounds check can be eliminated
    }
}

// Example function with predictable branches
int find_max_with_nullcheck(int* array, size_t size) {
    if (!array) { // This check is predictable (usually false)
        return -1;
    }
    
    int max = array[0];
    for (size_t i = 1; i < size; i++) {
        if (array[i] > max) { // Branch prediction can optimize this
            max = array[i];
        }
    }
    return max;
}

// Example function that can be vectorized
void vector_add(float* a, float* b, float* result, size_t size) {
    // This loop can be vectorized with SIMD instructions
    for (size_t i = 0; i < size; i++) {
        result[i] = a[i] + b[i];
    }
}

// Simulate AST creation for demo
ASTNode* create_demo_array_access(const char* array_name, int index) {
    // Create array[index] AST
    IdentifierNode* array_id = malloc(sizeof(IdentifierNode));
    array_id->base.type = AST_IDENTIFIER;
    array_id->name = strdup(array_name);
    
    LiteralNode* index_lit = malloc(sizeof(LiteralNode));
    index_lit->base.type = AST_LITERAL;
    index_lit->literal_type = TOKEN_INT;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", index);
    index_lit->value = strdup(buf);
    
    IndexExprNode* index_expr = malloc(sizeof(IndexExprNode));
    index_expr->base.type = AST_INDEX_EXPR;
    index_expr->expr = (ASTNode*)array_id;
    index_expr->index = (ASTNode*)index_lit;
    
    return (ASTNode*)index_expr;
}

void free_demo_ast(ASTNode* node) {
    if (!node) return;
    
    switch (node->type) {
        case AST_IDENTIFIER: {
            IdentifierNode* id = (IdentifierNode*)node;
            free(id->name);
            break;
        }
        case AST_LITERAL: {
            LiteralNode* lit = (LiteralNode*)node;
            free(lit->value);
            break;
        }
        case AST_INDEX_EXPR: {
            IndexExprNode* idx = (IndexExprNode*)node;
            free_demo_ast(idx->expr);
            free_demo_ast(idx->index);
            break;
        }
        default:
            break;
    }
    free(node);
}

void demonstrate_bounds_check_elimination() {
    printf("\n=== Bounds Check Elimination Demo ===\n");
    
    // Create optimization context
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    if (!ctx) {
        printf("Failed to create optimization context\n");
        return;
    }
    
    // Simulate different array access patterns
    int test_indices[] = {0, 5, 10, -1, 100};
    const char* descriptions[] = {
        "First element (safe)",
        "Middle element (depends on array size)",
        "Larger index (may be unsafe)",
        "Negative index (always unsafe)",
        "Large index (likely unsafe)"
    };
    
    for (int i = 0; i < 5; i++) {
        printf("\nTesting array[%d] - %s:\n", test_indices[i], descriptions[i]);
        
        ASTNode* access = create_demo_array_access("data", test_indices[i]);
        BoundsCheckInfo* info = analyze_bounds_check(ctx, access);
        
        if (info) {
            printf("  Can eliminate: %s\n", info->can_eliminate ? "YES" : "NO");
            printf("  Reason: %s\n", info->elimination_reason);
            
            if (can_eliminate_runtime_bounds_check(ctx, info)) {
                eliminate_bounds_check(ctx, info);
                printf("  ✓ Bounds check eliminated!\n");
            } else {
                printf("  ✗ Bounds check required\n");
            }
            
            bounds_check_info_free(info);
        }
        
        free_demo_ast(access);
    }
    
    printf("\nTotal bounds checks eliminated: %zu\n", ctx->bounds_checks_eliminated);
    
    optimization_context_free(ctx);
}

void demonstrate_hardware_capabilities() {
    printf("\n=== Hardware Capability Detection ===\n");
    
    HardwareCapabilities caps = detect_hardware_capabilities();
    
    printf("Detected hardware features:\n");
    if (caps & HW_CAP_INTEL_MPX) printf("  ✓ Intel MPX (Memory Protection Extensions)\n");
    if (caps & HW_CAP_ARM_MTE) printf("  ✓ ARM MTE (Memory Tagging Extensions)\n");
    if (caps & HW_CAP_INTEL_CET) printf("  ✓ Intel CET (Control-flow Enforcement)\n");
    if (caps & HW_CAP_ARM_BTI) printf("  ✓ ARM BTI (Branch Target Identification)\n");
    if (caps & HW_CAP_AVX512) printf("  ✓ AVX-512 (Advanced Vector Extensions)\n");
    if (caps & HW_CAP_NEON) printf("  ✓ ARM NEON (SIMD)\n");
    if (caps & HW_CAP_PREFETCH) printf("  ✓ Hardware Prefetching\n");
    if (caps & HW_CAP_SPECULATION) printf("  ✓ Speculative Execution Support\n");
    
    if (caps == HW_CAP_NONE) {
        printf("  No special hardware features detected\n");
    }
    
    printf("\nHardware capabilities mask: 0x%x\n", caps);
}

void demonstrate_optimization_levels() {
    printf("\n=== Optimization Safety Levels ===\n");
    
    OptimizationSafetyLevel levels[] = {
        OPT_SAFETY_DEBUG,
        OPT_SAFETY_CONSERVATIVE,
        OPT_SAFETY_BALANCED,
        OPT_SAFETY_AGGRESSIVE
    };
    
    const char* level_names[] = {
        "DEBUG", "CONSERVATIVE", "BALANCED", "AGGRESSIVE"
    };
    
    for (int i = 0; i < 4; i++) {
        OptimizationContext* ctx = optimization_context_new(levels[i]);
        if (!ctx) continue;
        
        printf("\n%s mode:\n", level_names[i]);
        printf("  Speculation: %s\n", ctx->enable_speculation ? "ENABLED" : "DISABLED");
        printf("  Vectorization: %s\n", ctx->enable_vectorization ? "ENABLED" : "DISABLED");
        printf("  Prefetching: %s\n", ctx->enable_prefetch ? "ENABLED" : "DISABLED");
        printf("  Hardware assists: %s\n", ctx->enable_hardware_assists ? "ENABLED" : "DISABLED");
        
        optimization_context_free(ctx);
    }
}

void demonstrate_performance_impact() {
    printf("\n=== Performance Impact Demo ===\n");
    
    const size_t ARRAY_SIZE = 10000000; // 10 million elements
    const int ITERATIONS = 10;
    
    // Allocate test arrays
    int* test_array = malloc(sizeof(int) * ARRAY_SIZE);
    if (!test_array) {
        printf("Failed to allocate test array\n");
        return;
    }
    
    // Initialize array
    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        test_array[i] = rand() % 1000;
    }
    
    // Test with bounds checking
    clock_t start = clock();
    for (int iter = 0; iter < ITERATIONS; iter++) {
        int sum = 0;
        for (size_t i = 0; i < ARRAY_SIZE; i++) {
            // Simulate bounds check
            if (i < ARRAY_SIZE) {
                sum += test_array[i];
            }
        }
    }
    clock_t end = clock();
    double with_checks = ((double)(end - start)) / CLOCKS_PER_SEC;
    
    // Test without bounds checking (simulated)
    start = clock();
    for (int iter = 0; iter < ITERATIONS; iter++) {
        int sum = 0;
        for (size_t i = 0; i < ARRAY_SIZE; i++) {
            // No bounds check
            sum += test_array[i];
        }
    }
    end = clock();
    double without_checks = ((double)(end - start)) / CLOCKS_PER_SEC;
    
    printf("Array size: %zu elements\n", ARRAY_SIZE);
    printf("Iterations: %d\n", ITERATIONS);
    printf("\nExecution time:\n");
    printf("  With bounds checks: %.3f seconds\n", with_checks);
    printf("  Without bounds checks: %.3f seconds\n", without_checks);
    printf("  Speedup: %.2fx\n", with_checks / without_checks);
    printf("  Time saved: %.3f seconds (%.1f%%)\n", 
           with_checks - without_checks,
           ((with_checks - without_checks) / with_checks) * 100);
    
    free(test_array);
}

int main() {
    printf("Runtime Optimization Framework Demo\n");
    printf("===================================\n");
    
    // Run demonstrations
    demonstrate_hardware_capabilities();
    demonstrate_optimization_levels();
    demonstrate_bounds_check_elimination();
    demonstrate_performance_impact();
    
    printf("\n\nDemo completed successfully!\n");
    
    return 0;
}