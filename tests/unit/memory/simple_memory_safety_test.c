#include "memory_safety.h"
#include "types.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int main() {
    printf("=== Simple Memory Safety Integration Test ===\n\n");
    
    // Create type checker
    TypeChecker* tc = type_checker_new();
    assert(tc != NULL);
    printf("✓ Type checker created\n");
    
    // Initialize memory safety integration
    int result = integrate_memory_safety_with_type_checker(tc);
    assert(result == 1);
    printf("✓ Memory safety integration initialized\n");
    
    // Check that context was created
    MemorySafetyContext* ctx = get_memory_safety_context();
    assert(ctx != NULL);
    printf("✓ Memory safety context available\n");
    
    // Test feature configuration
    assert(memory_safety_is_feature_enabled("null_safety") == 1);
    assert(memory_safety_is_feature_enabled("ownership_tracking") == 1);
    assert(memory_safety_is_feature_enabled("resource_management") == 1);
    printf("✓ All safety features enabled by default\n");
    
    // Test disabling and enabling a feature
    memory_safety_enable_feature("null_safety", 0);
    assert(memory_safety_is_feature_enabled("null_safety") == 0);
    memory_safety_enable_feature("null_safety", 1);
    assert(memory_safety_is_feature_enabled("null_safety") == 1);
    printf("✓ Feature configuration working\n");
    
    // Test statistics (should not crash)
    printf("\n--- Statistics Output ---\n");
    memory_safety_print_statistics();
    printf("--- End Statistics ---\n\n");
    
    // Cleanup
    cleanup_memory_safety_integration();
    type_checker_free(tc);
    
    printf("✓ Memory safety integration test passed!\n");
    printf("✓ All components cleaned up successfully\n");
    
    return 0;
}