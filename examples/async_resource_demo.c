#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/async_resource.h"

// Demo 1: Basic Resource Management with RAII
void demo_basic_raii_pattern(void) {
    printf("\n🌟 Demo 1: Basic RAII Pattern\n");
    printf("=============================\n");
    
    // Create a scoped resource management context
    WITH_ASYNC_RESOURCE_SCOPE(file_scope, NULL) {
        printf("📁 Creating file resources in scope...\n");
        
        // Create file resources that will be automatically cleaned up
        AsyncResource* input_file = async_file_resource_create("/tmp/input.txt", "w");
        AsyncResource* output_file = async_file_resource_create("/tmp/output.txt", "w");
        
        if (input_file && output_file) {
            // Add resources to scope for automatic cleanup
            async_resource_scope_add(file_scope, input_file);
            async_resource_scope_add(file_scope, output_file);
            
            // Set up deferred cleanup actions
            char* log_msg = "File processing completed";
            DEFER_ON_SUCCESS(file_scope, printf, "✅ Success: %s\n");
            DEFER_ON_ERROR(file_scope, printf, "❌ Error during file processing\n");
            
            // Use the resources
            if (async_resource_acquire(input_file, 1000).is_error ||
                async_resource_acquire(output_file, 1000).is_error) {
                printf("❌ Failed to acquire file resources\n");
            } else {
                printf("✅ File resources acquired successfully\n");
                
                // Simulate file operations
                const char* data = "Hello, async resource management!";
                size_t bytes_written;
                async_file_resource_write(input_file, data, strlen(data), &bytes_written);
                printf("📝 Wrote %zu bytes to input file\n", bytes_written);
            }
            
            // Resources will be automatically released when scope exits
            async_resource_unref(input_file);
            async_resource_unref(output_file);
        }
        
        printf("🔚 Exiting scope - automatic cleanup will occur\n");
    }
    // Scope cleanup happens here automatically
    
    printf("✅ Demo 1 completed - all resources cleaned up\n");
}

// Demo 2: Resource Dependencies and Coordination
void demo_resource_dependencies(void) {
    printf("\n🔗 Demo 2: Resource Dependencies\n");
    printf("===============================\n");
    
    // Simulate a database connection -> transaction -> query pattern
    printf("🗄️ Setting up database resource chain...\n");
    
    // Create resources with dependencies
    int db_delay = 100;    // Database connection takes 100ms
    int tx_delay = 50;     // Transaction setup takes 50ms  
    int query_delay = 25;  // Query execution takes 25ms
    
    AsyncResource* db_connection = async_resource_create("database", RESOURCE_TYPE_NETWORK,
                                                        NULL, NULL, &db_delay, sizeof(int));
    AsyncResource* transaction = async_resource_create("transaction", RESOURCE_TYPE_CUSTOM,
                                                      NULL, NULL, &tx_delay, sizeof(int));
    AsyncResource* query = async_resource_create("query", RESOURCE_TYPE_CUSTOM,
                                                NULL, NULL, &query_delay, sizeof(int));
    
    if (db_connection && transaction && query) {
        // Set up dependency chain: query -> transaction -> db_connection
        async_resource_add_dependency(transaction, db_connection);
        async_resource_add_dependency(query, transaction);
        
        printf("🔗 Dependency chain established\n");
        printf("   Query depends on Transaction\n");
        printf("   Transaction depends on Database\n");
        
        // Create scope for coordinated cleanup
        AsyncResourceScope* db_scope = async_resource_scope_create("database_scope", NULL);
        async_resource_scope_add(db_scope, db_connection);
        async_resource_scope_add(db_scope, transaction);
        async_resource_scope_add(db_scope, query);
        
        // Acquire resources in dependency order (automatic)
        printf("⏳ Acquiring resources (dependencies will be resolved automatically)...\n");
        
        // This will automatically wait for db_connection, then transaction
        Result_void_ptr query_result = async_resource_acquire(query, 5000);
        if (!query_result.is_error) {
            printf("✅ All dependent resources acquired successfully\n");
            printf("🔍 Executing database query...\n");
            usleep(100000); // Simulate query execution
            printf("📊 Query completed successfully\n");
        } else {
            printf("❌ Failed to acquire resource chain\n");
        }
        
        // Cleanup in reverse order
        async_resource_scope_cleanup(db_scope);
        async_resource_scope_destroy(db_scope);
        
        async_resource_unref(query);
        async_resource_unref(transaction);
        async_resource_unref(db_connection);
    }
    
    printf("✅ Demo 2 completed - resource dependencies handled\n");
}

// Demo 3: Error Handling and Recovery
void demo_error_handling(void) {
    printf("\n🚨 Demo 3: Error Handling and Recovery\n");
    printf("=====================================\n");
    
    // Simulate resource acquisition that might fail
    printf("🎯 Demonstrating error handling patterns...\n");
    
    WITH_ASYNC_RESOURCE_SCOPE(error_scope, NULL) {
        // Set up error recovery actions
        char* recovery_msg = "Initiating error recovery procedures";
        DEFER_ON_ERROR(error_scope, printf, "🔧 %s\n");
        
        // Create a resource that might fail
        AsyncResource* unreliable_resource = async_resource_create("unreliable", RESOURCE_TYPE_NETWORK,
                                                                  NULL, NULL, NULL, 0);
        
        if (unreliable_resource) {
            async_resource_scope_add(error_scope, unreliable_resource);
            
            // Try to acquire with timeout
            printf("⏳ Attempting to acquire unreliable resource...\n");
            Result_void_ptr acquire_result = async_resource_acquire(unreliable_resource, 2000);
            
            if (acquire_result.is_error) {
                printf("❌ Resource acquisition failed: %s\n", 
                       acquire_result.error ? acquire_result.error->message : "Unknown error");
                
                // Demonstrate error recovery
                printf("🔄 Attempting recovery...\n");
                
                // Try alternative resource or fallback
                AsyncResource* fallback_resource = async_resource_create("fallback", RESOURCE_TYPE_MEMORY,
                                                                        NULL, NULL, NULL, 0);
                if (fallback_resource) {
                    async_resource_scope_add(error_scope, fallback_resource);
                    
                    Result_void_ptr fallback_result = async_resource_acquire(fallback_resource, 1000);
                    if (!fallback_result.is_error) {
                        printf("✅ Fallback resource acquired successfully\n");
                        printf("🔄 Continuing with degraded functionality\n");
                    }
                    
                    async_resource_unref(fallback_resource);
                }
            } else {
                printf("✅ Resource acquired successfully (unexpected!)\n");
            }
            
            async_resource_unref(unreliable_resource);
        }
        
        printf("🔚 Exiting error handling scope\n");
    }
    
    printf("✅ Demo 3 completed - error handling demonstrated\n");
}

// Demo 4: Memory Resource Management
void demo_memory_management(void) {
    printf("\n💾 Demo 4: Memory Resource Management\n");
    printf("====================================\n");
    
    printf("🔧 Demonstrating automatic memory management...\n");
    
    WITH_ASYNC_RESOURCE_SCOPE(memory_scope, NULL) {
        // Create various memory resources
        AsyncResource* small_buffer = async_memory_resource_create("small_buffer", 1024, 0);
        AsyncResource* large_buffer = async_memory_resource_create("large_buffer", 64 * 1024, 0);
        AsyncResource* working_set = async_memory_resource_create("working_set", 4096, 16);
        
        if (small_buffer && large_buffer && working_set) {
            // Add to scope for automatic cleanup
            async_resource_scope_add(memory_scope, small_buffer);
            async_resource_scope_add(memory_scope, large_buffer);
            async_resource_scope_add(memory_scope, working_set);
            
            // Set up memory cleanup logging
            DEFER(memory_scope, printf, "🧹 All memory resources have been freed\n");
            
            // Acquire memory resources
            printf("📦 Acquiring memory resources...\n");
            async_resource_acquire(small_buffer, 1000);
            async_resource_acquire(large_buffer, 1000);
            async_resource_acquire(working_set, 1000);
            
            // Use the memory
            if (async_resource_is_acquired(small_buffer)) {
                void* buffer = small_buffer->resource_data;
                memset(buffer, 0x42, 1024);
                printf("✏️ Initialized small buffer with test pattern\n");
            }
            
            if (async_resource_is_acquired(large_buffer)) {
                printf("📊 Large buffer ready for bulk operations\n");
                
                // Demonstrate memory resizing
                Result_void_ptr resize_result = async_memory_resource_resize(large_buffer, 128 * 1024);
                if (!resize_result.is_error) {
                    printf("↗️ Large buffer resized to 128KB\n");
                }
            }
            
            if (async_resource_is_acquired(working_set)) {
                printf("⚙️ Working set ready for processing\n");
            }
            
            // Simulate some work
            printf("🔄 Performing memory-intensive operations...\n");
            usleep(100000); // 100ms of "work"
            
            async_resource_unref(small_buffer);
            async_resource_unref(large_buffer);
            async_resource_unref(working_set);
        }
        
        printf("🔚 Exiting memory scope - automatic cleanup will occur\n");
    }
    
    printf("✅ Demo 4 completed - memory automatically managed\n");
}

// Demo 5: Nested Resource Scopes
void demo_nested_scopes(void) {
    printf("\n🪆 Demo 5: Nested Resource Scopes\n");
    printf("=================================\n");
    
    printf("🎯 Demonstrating nested resource management...\n");
    
    // Outer scope for application-level resources
    WITH_ASYNC_RESOURCE_SCOPE(app_scope, NULL) {
        printf("🏢 Application scope created\n");
        
        // Create application-level resources
        AsyncResource* config_file = async_file_resource_create("/tmp/app.conf", "w");
        AsyncResource* log_file = async_file_resource_create("/tmp/app.log", "w");
        
        if (config_file && log_file) {
            async_resource_scope_add(app_scope, config_file);
            async_resource_scope_add(app_scope, log_file);
            
            DEFER(app_scope, printf, "🏢 Application resources cleaned up\n");
            
            async_resource_acquire(config_file, 1000);
            async_resource_acquire(log_file, 1000);
            
            printf("📋 Application infrastructure ready\n");
            
            // Inner scope for request-specific resources  
            WITH_ASYNC_RESOURCE_SCOPE(request_scope, NULL) {
                printf("  📝 Request scope created\n");
                
                // Create request-specific resources
                AsyncResource* temp_file = async_file_resource_create("/tmp/request.tmp", "w");
                AsyncResource* response_buffer = async_memory_resource_create("response", 8192, 0);
                
                if (temp_file && response_buffer) {
                    async_resource_scope_add(request_scope, temp_file);
                    async_resource_scope_add(request_scope, response_buffer);
                    
                    DEFER(request_scope, printf, "  📝 Request resources cleaned up\n");
                    
                    async_resource_acquire(temp_file, 1000);
                    async_resource_acquire(response_buffer, 1000);
                    
                    printf("  🔄 Processing request...\n");
                    usleep(50000); // 50ms of request processing
                    
                    // Even more nested scope for temporary operations
                    WITH_ASYNC_RESOURCE_SCOPE(temp_scope, NULL) {
                        printf("    🔧 Temporary operation scope created\n");
                        
                        AsyncResource* scratch_memory = async_memory_resource_create("scratch", 1024, 0);
                        if (scratch_memory) {
                            async_resource_scope_add(temp_scope, scratch_memory);
                            async_resource_acquire(scratch_memory, 1000);
                            
                            printf("    ⚡ Performing temporary computation...\n");
                            usleep(25000); // 25ms of computation
                            
                            async_resource_unref(scratch_memory);
                        }
                        printf("    🔧 Temporary scope cleanup (automatic)\n");
                    }
                    
                    printf("  ✅ Request completed\n");
                    async_resource_unref(temp_file);
                    async_resource_unref(response_buffer);
                }
                printf("  📝 Request scope cleanup (automatic)\n");
            }
            
            async_resource_unref(config_file);
            async_resource_unref(log_file);
        }
        printf("🏢 Application scope cleanup (automatic)\n");
    }
    
    printf("✅ Demo 5 completed - nested scopes handled correctly\n");
}

// Demo 6: Resource Monitoring and Statistics
void demo_monitoring_and_stats(void) {
    printf("\n📊 Demo 6: Resource Monitoring and Statistics\n");
    printf("============================================\n");
    
    printf("📈 Demonstrating resource monitoring capabilities...\n");
    
    // Reset statistics for clean demo
    async_resource_reset_global_stats();
    
    // Enable comprehensive monitoring
    async_resource_manager_enable_tracking(true);
    async_resource_manager_enable_leak_detection(true);
    async_resource_manager_set_default_timeout(3000);
    
    printf("🔧 Resource monitoring enabled\n");
    
    WITH_ASYNC_RESOURCE_SCOPE(monitor_scope, NULL) {
        // Create multiple resources of different types
        AsyncResource* resources[5];
        const char* names[] = {"mem1", "mem2", "file1", "net1", "custom1"};
        AsyncResourceType types[] = {
            RESOURCE_TYPE_MEMORY, RESOURCE_TYPE_MEMORY, RESOURCE_TYPE_FILE,
            RESOURCE_TYPE_NETWORK, RESOURCE_TYPE_CUSTOM
        };
        
        printf("🏗️ Creating %d resources for monitoring...\n", 5);
        
        for (int i = 0; i < 5; i++) {
            if (types[i] == RESOURCE_TYPE_MEMORY) {
                resources[i] = async_memory_resource_create(names[i], (i + 1) * 1024, 0);
            } else {
                int delay = (i + 1) * 10;
                resources[i] = async_resource_create(names[i], types[i], NULL, NULL, 
                                                   &delay, sizeof(int));
            }
            
            if (resources[i]) {
                async_resource_scope_add(monitor_scope, resources[i]);
                
                // Stagger acquisitions to show timing
                printf("⏳ Acquiring %s...\n", names[i]);
                async_resource_acquire(resources[i], 2000);
                usleep(20000); // 20ms between acquisitions
            }
        }
        
        // Show individual resource statistics
        printf("\n📊 Individual Resource Statistics:\n");
        for (int i = 0; i < 5; i++) {
            if (resources[i]) {
                async_resource_print_stats(resources[i]);
            }
        }
        
        // Show scope statistics
        printf("\n🎯 Scope Statistics:\n");
        async_resource_scope_print_stats(monitor_scope);
        
        // Simulate some resource usage
        printf("\n🔄 Simulating resource usage...\n");
        for (int i = 0; i < 10; i++) {
            usleep(10000); // 10ms intervals
            if (i % 3 == 0) {
                printf("💼 Resource activity checkpoint %d\n", i + 1);
            }
        }
        
        // Check for leaks (shouldn't find any in this demo)
        printf("\n🔍 Checking for resource leaks...\n");
        size_t leak_count;
        ResourceLeak* leaks = async_resource_detect_leaks(&leak_count);
        
        if (leak_count > 0) {
            printf("⚠️ Found %zu potential leaks (expected in this demo)\n", leak_count);
            async_resource_free_leak_report(leaks, leak_count);
        } else {
            printf("✅ No resource leaks detected\n");
        }
        
        // Cleanup resources
        for (int i = 0; i < 5; i++) {
            if (resources[i]) {
                async_resource_unref(resources[i]);
            }
        }
        
        printf("\n🔚 Exiting monitoring scope\n");
    }
    
    // Show final global statistics
    printf("\n📈 Final Global Statistics:\n");
    async_resource_manager_print_global_stats();
    
    printf("✅ Demo 6 completed - monitoring data collected\n");
}

int main(void) {
    printf("🌟 Async Resource Management Demonstration\n");
    printf("==========================================\n");
    printf("This demo showcases various patterns for managing resources\n");
    printf("in async contexts with automatic cleanup and error handling.\n");
    
    // Initialize the resource management system
    Result_void_ptr init_result = async_resource_manager_init();
    if (init_result.is_error) {
        printf("❌ Failed to initialize resource manager\n");
        return 1;
    }
    
    printf("✅ Resource manager initialized\n\n");
    
    // Run all demos
    demo_basic_raii_pattern();
    demo_resource_dependencies();
    demo_error_handling();
    demo_memory_management();
    demo_nested_scopes();
    demo_monitoring_and_stats();
    
    // Final cleanup
    printf("\n🔚 Shutting down resource manager...\n");
    async_resource_manager_shutdown();
    
    printf("\n🎉 All demos completed successfully!\n");
    printf("💡 Key takeaways:\n");
    printf("   • Resources are automatically cleaned up when scopes exit\n");
    printf("   • Dependencies are resolved automatically during acquisition\n");
    printf("   • Error handling and recovery can be structured with deferred actions\n");
    printf("   • Memory management follows RAII principles\n");
    printf("   • Nested scopes provide hierarchical resource management\n");
    printf("   • Comprehensive monitoring and leak detection available\n");
    
    return 0;
}