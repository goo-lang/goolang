#include "memory_safety.h"
#include "types.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Simple test for resource manager using only public API
int main() {
    printf("=== Simple Automatic Resource Management Test ===\n\n");
    
    // Mock type checker for testing
    TypeChecker* tc = malloc(sizeof(TypeChecker));
    if (tc) {
        memset(tc, 0, sizeof(TypeChecker));
    }
    
    // Test resource manager creation
    printf("Testing resource manager creation...\n");
    ResourceManager* rm = resource_manager_new(tc);
    assert(rm != NULL);
    printf("✓ Resource manager created successfully\n");
    
    // Test resource type detection
    printf("Testing resource type detection...\n");
    assert(get_resource_type_for_function("open") == RESOURCE_TYPE_FILE);
    assert(get_resource_type_for_function("fopen") == RESOURCE_TYPE_FILE);
    assert(get_resource_type_for_function("socket") == RESOURCE_TYPE_NETWORK);
    assert(get_resource_type_for_function("malloc") == RESOURCE_TYPE_MEMORY);
    assert(get_resource_type_for_function("mutex_new") == RESOURCE_TYPE_MUTEX);
    assert(get_resource_type_for_function("thread_create") == RESOURCE_TYPE_THREAD);
    assert(get_resource_type_for_function("gpu_alloc") == RESOURCE_TYPE_GPU_BUFFER);
    assert(get_resource_type_for_function("unknown_func") == RESOURCE_TYPE_UNKNOWN);
    printf("✓ Resource type detection working correctly\n");
    
    // Test resource tracking
    printf("Testing resource tracking...\n");
    Position pos = {1, 1, 0, "test.goo"};
    
    int result = resource_manager_track_resource(rm, "file_handle", RESOURCE_TYPE_FILE, NULL, pos);
    assert(result == 1);
    printf("✓ File resource tracked successfully\n");
    
    result = resource_manager_track_resource(rm, "buffer", RESOURCE_TYPE_MEMORY, NULL, pos);
    assert(result == 1);
    printf("✓ Memory resource tracked successfully\n");
    
    // Test resource lookup
    printf("Testing resource lookup...\n");
    ResourceInfo* res = resource_manager_find_resource(rm, "file_handle");
    assert(res != NULL);
    printf("✓ File resource found\n");
    
    ResourceInfo* mem_res = resource_manager_find_resource(rm, "buffer");
    assert(mem_res != NULL);
    printf("✓ Memory resource found\n");
    
    ResourceInfo* not_found = resource_manager_find_resource(rm, "nonexistent");
    assert(not_found == NULL);
    printf("✓ Non-existent resource correctly not found\n");
    
    // Test resource state changes
    printf("Testing resource state changes...\n");
    result = resource_manager_mark_resource_borrowed(rm, "file_handle");
    assert(result == 1);
    printf("✓ Resource marked as borrowed\n");
    
    result = resource_manager_mark_resource_moved(rm, "buffer");
    assert(result == 1);
    printf("✓ Resource marked as moved\n");
    
    // Test utility functions
    printf("Testing utility functions...\n");
    assert(strcmp(resource_type_to_string(RESOURCE_TYPE_FILE), "file") == 0);
    assert(strcmp(resource_type_to_string(RESOURCE_TYPE_MEMORY), "memory") == 0);
    assert(strcmp(resource_type_to_string(RESOURCE_TYPE_UNKNOWN), "unknown") == 0);
    
    assert(strcmp(resource_context_to_string(RESOURCE_CONTEXT_DIRECT), "direct") == 0);
    assert(strcmp(resource_context_to_string(RESOURCE_CONTEXT_FUNCTION_CALL), "function_call") == 0);
    
    assert(strcmp(cleanup_method_to_string(CLEANUP_METHOD_FUNCTION_CALL), "function_call") == 0);
    assert(strcmp(cleanup_method_to_string(CLEANUP_METHOD_RAII), "raii") == 0);
    assert(strcmp(cleanup_method_to_string(CLEANUP_METHOD_DEFER), "defer") == 0);
    printf("✓ Utility functions working correctly\n");
    
    // Test statistics reporting (just make sure it doesn't crash)
    printf("Testing statistics reporting...\n");
    resource_manager_print_statistics(rm);
    resource_manager_print_resource_info(rm, "file_handle");
    printf("✓ Statistics reporting working\n");
    
    // Cleanup
    resource_manager_free(rm);
    free(tc);
    
    printf("\n=== All Tests Passed! ===\n");
    printf("✓ Resource manager creation and cleanup\n");
    printf("✓ Resource type detection\n");
    printf("✓ Resource tracking and lookup\n");
    printf("✓ Resource state management\n");
    printf("✓ Utility functions\n");
    printf("✓ Statistics reporting\n");
    
    return 0;
}