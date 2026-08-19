#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include "../../include/async_resource.h"

// Test helper functions
static void test_cleanup_action(void* data, void* context) {
    (void)context;
    int* counter = (int*)data;
    if (counter) {
        (*counter)++;
        printf("🧹 Cleanup action executed (counter: %d)\n", *counter);
    }
}

// Custom resource implementation for testing
static Result_void_ptr test_resource_acquire(void* context, AsyncWaker* waker) {
    (void)waker;
    int* delay_ms = (int*)context;
    
    if (delay_ms && *delay_ms > 0) {
        printf("⏳ Simulating resource acquisition delay: %d ms\n", *delay_ms);
        usleep(*delay_ms * 1000);
    }
    
    // Simulate resource data
    char* resource_data = malloc(64);
    if (!resource_data) {
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate test resource"));
    }
    
    strcpy(resource_data, "test_resource_data");
    printf("✅ Test resource acquired: %s\n", resource_data);
    
    return OK_PTR(resource_data);
}

static void test_resource_cleanup(void* resource_data, void* context) {
    (void)context;
    if (resource_data) {
        printf("🧹 Cleaning up test resource: %s\n", (char*)resource_data);
        free(resource_data);
    }
}

// Test basic resource creation and lifecycle
void test_basic_resource_lifecycle(void) {
    printf("\n📋 Test: Basic Resource Lifecycle\n");
    printf("==================================\n");
    
    // Initialize resource manager
    Result_void_ptr init_result = async_resource_manager_init();
    assert(!init_result.is_error);
    
    // Create a test resource
    int delay = 50; // 50ms delay
    AsyncResource* resource = async_resource_create("test_resource", RESOURCE_TYPE_CUSTOM,
                                                   test_resource_acquire, test_resource_cleanup,
                                                   &delay, sizeof(int));
    assert(resource != NULL);
    
    // Check initial state
    assert(async_resource_get_state(resource) == RESOURCE_STATE_CREATED);
    assert(!async_resource_is_acquired(resource));
    
    // Acquire the resource
    Result_void_ptr acquire_result = async_resource_acquire(resource, 1000);
    assert(!acquire_result.is_error);
    assert(async_resource_is_acquired(resource));
    
    // Release the resource
    Result_void_ptr release_result = async_resource_release(resource);
    assert(!release_result.is_error);
    assert(async_resource_get_state(resource) == RESOURCE_STATE_RELEASED);
    
    // Clean up
    async_resource_unref(resource);
    
    printf("✅ Basic resource lifecycle test passed\n");
}

// Test resource scopes and deferred actions
void test_resource_scopes_and_defer(void) {
    printf("\n🎯 Test: Resource Scopes and Deferred Actions\n");
    printf("==============================================\n");
    
    int cleanup_counter = 0;
    
    // Create a resource scope
    AsyncResourceScope* scope = async_resource_scope_create("test_scope", NULL);
    assert(scope != NULL);
    
    // Create resources and add to scope
    int delay = 10;
    AsyncResource* resource1 = async_resource_create("scoped_resource1", RESOURCE_TYPE_MEMORY,
                                                     test_resource_acquire, test_resource_cleanup,
                                                     &delay, sizeof(int));
    AsyncResource* resource2 = async_resource_create("scoped_resource2", RESOURCE_TYPE_FILE,
                                                     test_resource_acquire, test_resource_cleanup,
                                                     &delay, sizeof(int));
    
    assert(resource1 != NULL);
    assert(resource2 != NULL);
    
    // Add resources to scope
    Result_void_ptr add1_result = async_resource_scope_add(scope, resource1);
    Result_void_ptr add2_result = async_resource_scope_add(scope, resource2);
    assert(!add1_result.is_error);
    assert(!add2_result.is_error);
    
    // Create deferred actions
    DeferredAction* cleanup_action = deferred_action_create("cleanup_counter", test_cleanup_action,
                                                          &cleanup_counter, NULL, 0);
    
    assert(cleanup_action != NULL);
    
    // Configure deferred action
    cleanup_action->execute_on_success = true;
    cleanup_action->execute_on_error = false;
    cleanup_action->execute_on_cancel = false;
    
    // Add deferred action to scope
    Result_void_ptr defer_result = async_resource_scope_defer(scope, cleanup_action);
    assert(!defer_result.is_error);
    
    // Acquire resources
    async_resource_acquire(resource1, 1000);
    async_resource_acquire(resource2, 1000);
    
    // Clean up scope (should trigger success actions)
    Result_void_ptr cleanup_result = async_resource_scope_cleanup(scope);
    assert(!cleanup_result.is_error);
    
    // Check that cleanup action was executed
    assert(cleanup_counter == 1);
    
    // Clean up
    async_resource_unref(resource1);
    async_resource_unref(resource2);
    async_resource_scope_destroy(scope);
    
    printf("✅ Resource scopes and deferred actions test passed\n");
}

// Test memory resource implementation
void test_memory_resource(void) {
    printf("\n💾 Test: Memory Resource Implementation\n");
    printf("======================================\n");
    
    // Create memory resource
    size_t size = 1024;
    AsyncResource* memory_resource = async_memory_resource_create("test_memory", size, 0);
    assert(memory_resource != NULL);
    assert(memory_resource->type == RESOURCE_TYPE_MEMORY);
    
    // Acquire memory
    Result_void_ptr acquire_result = async_resource_acquire(memory_resource, 1000);
    assert(!acquire_result.is_error);
    assert(async_resource_is_acquired(memory_resource));
    
    // Test memory operations - get the memory from the result
    void* memory = acquire_result.value;
    assert(memory != NULL);
    
    // Write some data
    memset(memory, 0xAA, size);
    printf("📝 Wrote test pattern to memory\n");
    
    // Resize memory 
    size_t new_size = 2048;
    Result_void_ptr resize_result = async_memory_resource_resize(memory_resource, new_size);
    assert(!resize_result.is_error);
    printf("✅ Memory resize completed successfully\n");
    
    // Release memory
    async_resource_release(memory_resource);
    async_resource_unref(memory_resource);
    
    printf("✅ Memory resource test passed\n");
}

// Test global statistics
void test_global_statistics(void) {
    printf("\n📊 Test: Global Statistics\n");
    printf("=========================\n");
    
    // Reset statistics
    async_resource_reset_global_stats();
    
    // Create and use some resources
    int delay = 10;
    AsyncResource* resource1 = async_resource_create("stats_test1", RESOURCE_TYPE_MEMORY,
                                                     test_resource_acquire, test_resource_cleanup,
                                                     &delay, sizeof(int));
    AsyncResource* resource2 = async_resource_create("stats_test2", RESOURCE_TYPE_FILE,
                                                     test_resource_acquire, test_resource_cleanup,
                                                     &delay, sizeof(int));
    
    // Acquire and release
    async_resource_acquire(resource1, 1000);
    async_resource_acquire(resource2, 1000);
    async_resource_release(resource1);
    async_resource_release(resource2);
    
    // Get statistics
    AsyncResourceStats stats = async_resource_get_global_stats();
    
    printf("📊 Global Statistics:\n");
    printf("   Total created: %llu\n", stats.total_resources_created);
    printf("   Currently active: %llu\n", stats.currently_active_resources);
    printf("   Total acquisitions: %llu\n", stats.total_acquisitions);
    printf("   Successful acquisitions: %llu\n", stats.successful_acquisitions);
    printf("   Total releases: %llu\n", stats.total_releases);
    
    // Verify some basic statistics
    assert(stats.total_resources_created >= 2);
    assert(stats.total_acquisitions >= 2);
    assert(stats.successful_acquisitions >= 2);
    assert(stats.total_releases >= 2);
    
    // Clean up
    async_resource_unref(resource1);
    async_resource_unref(resource2);
    
    printf("✅ Global statistics test passed\n");
}

int main(void) {
    printf("🧪 Async Resource Management Testing\n");
    printf("====================================\n");
    
    // Run tests
    test_basic_resource_lifecycle();
    test_resource_scopes_and_defer();
    test_memory_resource();
    test_global_statistics();
    
    // Print final global statistics
    printf("\n");
    async_resource_manager_print_global_stats();
    
    // Shutdown resource manager
    async_resource_manager_shutdown();
    
    printf("\n🎉 All async resource management tests passed!\n");
    return 0;
}