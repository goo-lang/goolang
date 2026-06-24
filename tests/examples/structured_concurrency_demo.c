#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include "include/structured_concurrency_enhanced.h"

// Demo context structures
typedef struct FileProcessingContext {
    char filename[256];
    size_t file_size;
    int processing_time_ms;
    bool success;
} FileProcessingContext;

typedef struct DataProcessingContext {
    int* data;
    size_t data_size;
    int result_sum;
    bool compute_intensive;
} DataProcessingContext;

// Demo async functions

// Simulates file processing
Result_void_ptr process_file_async(void* args, AsyncContext* async_ctx) {
    FileProcessingContext* ctx = (FileProcessingContext*)args;
    
    printf("📁 Processing file: %s (size: %zu bytes)\n", ctx->filename, ctx->file_size);
    
    // Simulate file I/O with cancellation checks
    for (int i = 0; i < ctx->processing_time_ms; i += 10) {
        if (async_ctx && atomic_load(async_ctx->is_cancelled)) {
            printf("❌ File processing cancelled: %s\n", ctx->filename);
            return ERR_PTR(error_create(ERROR_OPERATION_CANCELLED, "File processing was cancelled"));
        }
        usleep(10000); // 10ms chunks
    }
    
    // Simulate occasional failures
    if (strstr(ctx->filename, "corrupted") != NULL) {
        printf("💥 File processing failed: %s (corrupted file)\n", ctx->filename);
        return ERR_PTR(error_create(ERROR_OPERATION_FAILED, "File is corrupted"));
    }
    
    ctx->success = true;
    printf("✅ File processed successfully: %s\n", ctx->filename);
    return OK_PTR(ctx);
}

// Simulates data computation
Result_void_ptr compute_data_async(void* args, AsyncContext* async_ctx) {
    DataProcessingContext* ctx = (DataProcessingContext*)args;
    
    printf("🔢 Computing data chunk of size: %zu\n", ctx->data_size);
    
    ctx->result_sum = 0;
    
    // Process data with periodic cancellation checks
    for (size_t i = 0; i < ctx->data_size; i++) {
        if (async_ctx && atomic_load(async_ctx->is_cancelled)) {
            printf("❌ Data computation cancelled\n");
            return ERR_PTR(error_create(ERROR_OPERATION_CANCELLED, "Data computation was cancelled"));
        }
        
        // Simulate computation
        ctx->result_sum += ctx->data[i] * ctx->data[i];
        
        if (ctx->compute_intensive && i % 1000 == 0) {
            usleep(1000); // 1ms pause for intensive computation
        }
    }
    
    printf("✅ Data computation completed: sum = %d\n", ctx->result_sum);
    return OK_PTR(ctx);
}

// Simulates network request
Result_void_ptr fetch_data_async(void* args, AsyncContext* async_ctx) {
    char* url = (char*)args;
    
    printf("🌐 Fetching data from: %s\n", url);
    
    // Simulate network delay
    for (int i = 0; i < 100; i++) {
        if (async_ctx && atomic_load(async_ctx->is_cancelled)) {
            printf("❌ Network request cancelled: %s\n", url);
            return ERR_PTR(error_create(ERROR_OPERATION_CANCELLED, "Network request was cancelled"));
        }
        usleep(5000); // 5ms chunks (500ms total)
    }
    
    // Simulate occasional network failures
    if (strstr(url, "unreliable") != NULL) {
        printf("💥 Network request failed: %s (connection timeout)\n", url);
        return ERR_PTR(error_create(ERROR_OPERATION_FAILED, "Network connection timeout"));
    }
    
    printf("✅ Data fetched successfully: %s\n", url);
    return OK_PTR(strdup("response_data"));
}

// Demo 1: Basic concurrent file processing
// Returns Result_void_ptr because the CONCURRENT_* macros early-return on error.
Result_void_ptr demo_basic_concurrent_processing(void) {
    printf("\n🚀 Demo 1: Basic Concurrent File Processing\n");
    printf("===========================================\n");
    
    // Initialize scheduler
    StructuredScheduler* scheduler = structured_scheduler_create(NULL);
    structured_scheduler_start(scheduler);
    structured_set_global_scheduler(scheduler);
    
    // Create concurrent block
    ConcurrentBlockConfig config = concurrent_block_config_io_intensive();
    config.max_concurrent_tasks = 3;
    
    CONCURRENT_BLOCK_WITH_CONFIG(file_processing_block, config);
    
    // Create file processing contexts
    FileProcessingContext files[] = {
        {"document1.pdf", 1024*1024, 100, false},
        {"image2.jpg", 2048*1024, 150, false},
        {"data3.csv", 512*1024, 80, false},
        {"report4.docx", 1536*1024, 120, false}
    };
    
    // Add file processing expressions
    for (int i = 0; i < 4; i++) {
        CONCURRENT_EXPR(file_processing_block, file_processing, process_file_async, &files[i]);
    }
    
    // Execute and wait
    CONCURRENT_TRY(file_processing_block);
    CONCURRENT_BLOCK_END(file_processing_block);
    
    // Check results
    int successful_files = 0;
    for (int i = 0; i < 4; i++) {
        if (files[i].success) successful_files++;
    }
    
    printf("📊 Processing completed: %d/%d files successful\n", successful_files, 4);

    // Cleanup
    structured_scheduler_shutdown(scheduler, 5000);
    structured_scheduler_destroy(scheduler);
    return OK_PTR(NULL);
}

// Demo 2: Fail-fast error handling
Result_void_ptr demo_fail_fast_error_handling(void) {
    printf("\n💥 Demo 2: Fail-Fast Error Handling\n");
    printf("===================================\n");
    
    // Initialize scheduler
    StructuredScheduler* scheduler = structured_scheduler_create(NULL);
    structured_scheduler_start(scheduler);
    structured_set_global_scheduler(scheduler);
    
    // Create fail-fast concurrent block
    ConcurrentBlockConfig config = concurrent_block_config_fail_fast();
    
    CONCURRENT_BLOCK_WITH_CONFIG(fail_fast_block, config);
    
    // Create file processing contexts (one will fail)
    FileProcessingContext files[] = {
        {"good_file1.txt", 1024, 50, false},
        {"corrupted_file.txt", 2048, 100, false}, // This will fail
        {"good_file2.txt", 1024, 200, false},     // Should be cancelled
        {"good_file3.txt", 1024, 150, false}      // Should be cancelled
    };
    
    // Add expressions
    for (int i = 0; i < 4; i++) {
        CONCURRENT_EXPR(fail_fast_block, file_processing, process_file_async, &files[i]);
    }
    
    // Execute - expect failure due to corrupted file
    Result_void_ptr exec_result = concurrent_block_execute(fail_fast_block);
    if (!exec_result.is_error) {
        Result_void_ptr wait_result = concurrent_block_wait(fail_fast_block, 10000);
        if (wait_result.is_error) {
            printf("🎯 Fail-fast correctly triggered: %s\n", wait_result.error->message);
        }
    }
    
    concurrent_block_destroy(fail_fast_block);

    // Cleanup
    structured_scheduler_shutdown(scheduler, 5000);
    structured_scheduler_destroy(scheduler);
    return OK_PTR(NULL);
}

// Demo 3: Timeout and retry decorators (no early-returning macros; stays void)
void demo_timeout_and_retry(void) {
    printf("\n⏰ Demo 3: Timeout and Retry Decorators\n");
    printf("======================================\n");
    
    // Initialize transparent execution
    transparent_execution_init();
    
    // Register functions
    FunctionRegistry* registry = function_registry_global();
    function_registry_register(registry, "fetch_data", NULL, fetch_data_async, FUNC_TYPE_ASYNC_NATIVE);
    
    TransparentFunction* fetch_func = function_registry_find(registry, "fetch_data");
    
    // Demo timeout decorator
    printf("\n🕐 Testing Timeout Decorator:\n");
    TimeoutDecorator* timeout_dec = timeout_decorator_create(200, fetch_func); // 200ms timeout
    
    char* fast_url = "https://api.fast-service.com/data";
    char* slow_url = "https://api.unreliable-service.com/data"; // Will fail
    
    printf("Fast request (should succeed):\n");
    Result_void_ptr fast_result = timeout_decorator_execute(timeout_dec, fast_url, strlen(fast_url) + 1);
    printf("Result: %s\n", fast_result.is_error ? "TIMEOUT/ERROR" : "SUCCESS");
    
    printf("\nSlow request (should timeout):\n");
    Result_void_ptr slow_result = timeout_decorator_execute(timeout_dec, slow_url, strlen(slow_url) + 1);
    printf("Result: %s\n", slow_result.is_error ? "TIMEOUT/ERROR" : "SUCCESS");
    
    timeout_decorator_destroy(timeout_dec);
    
    // Demo retry decorator
    printf("\n🔄 Testing Retry Decorator:\n");
    RetryDecorator* retry_dec = retry_decorator_create(3, 50, fetch_func); // 3 attempts, 50ms delay
    
    printf("Reliable request (should succeed on first try):\n");
    char* reliable_url = "https://api.reliable-service.com/data";
    Result_void_ptr reliable_result = retry_decorator_execute(retry_dec, reliable_url, strlen(reliable_url) + 1);
    printf("Result: %s (retries: %llu)\n", reliable_result.is_error ? "FAILED" : "SUCCESS", retry_dec->retry_count);
    
    printf("\nUnreliable request (should fail after retries):\n");
    Result_void_ptr unreliable_result = retry_decorator_execute(retry_dec, slow_url, strlen(slow_url) + 1);
    printf("Result: %s (retries: %llu)\n", unreliable_result.is_error ? "FAILED" : "SUCCESS", retry_dec->retry_count);
    
    retry_decorator_destroy(retry_dec);
    
    // Cleanup
    transparent_execution_shutdown();
}

// Demo 4: CPU-intensive parallel processing
Result_void_ptr demo_cpu_intensive_processing(void) {
    printf("\n🔥 Demo 4: CPU-Intensive Parallel Processing\n");
    printf("==========================================\n");
    
    // Initialize scheduler
    StructuredScheduler* scheduler = structured_scheduler_create(NULL);
    structured_scheduler_start(scheduler);
    structured_set_global_scheduler(scheduler);
    
    // Create CPU-intensive concurrent block
    ConcurrentBlockConfig config = concurrent_block_config_cpu_intensive();
    config.max_concurrent_tasks = 4; // Use 4 cores
    
    CONCURRENT_BLOCK_WITH_CONFIG(cpu_block, config);
    
    // Create data chunks for parallel processing
    const size_t chunk_size = 10000;
    const int num_chunks = 4;
    
    DataProcessingContext chunks[num_chunks];
    
    for (int i = 0; i < num_chunks; i++) {
        chunks[i].data = malloc(chunk_size * sizeof(int));
        chunks[i].data_size = chunk_size;
        chunks[i].compute_intensive = true;
        chunks[i].result_sum = 0;
        
        // Initialize with random data
        for (size_t j = 0; j < chunk_size; j++) {
            chunks[i].data[j] = (int)(j + i * chunk_size) % 100;
        }
        
        CONCURRENT_EXPR(cpu_block, compute_chunk, compute_data_async, &chunks[i]);
    }
    
    uint64_t start_time = async_get_timestamp_ns();
    
    // Execute parallel computation
    CONCURRENT_TRY(cpu_block);
    CONCURRENT_BLOCK_END(cpu_block);
    
    uint64_t end_time = async_get_timestamp_ns();
    double execution_time_ms = (end_time - start_time) / 1000000.0;
    
    // Calculate total result
    long long total_sum = 0;
    for (int i = 0; i < num_chunks; i++) {
        total_sum += chunks[i].result_sum;
        free(chunks[i].data);
    }
    
    printf("📊 Parallel computation completed in %.2f ms\n", execution_time_ms);
    printf("📊 Total sum across all chunks: %lld\n", total_sum);
    printf("📊 Processed %d chunks of %zu elements each\n", num_chunks, chunk_size);
    
    // Cleanup
    structured_scheduler_shutdown(scheduler, 5000);
    structured_scheduler_destroy(scheduler);
    return OK_PTR(NULL);
}

// Demo 5 helpers. C has no nested functions (a GCC-only extension clang rejects),
// so the connection type and its open/close routines live at file scope.
typedef struct DatabaseConnection {
    int connection_id;
    bool is_open;
    char connection_string[256];
} DatabaseConnection;

static DatabaseConnection* create_db_connection(const char* conn_str) {
    DatabaseConnection* conn = malloc(sizeof(DatabaseConnection));
    conn->connection_id = rand() % 10000;
    conn->is_open = true;
    strncpy(conn->connection_string, conn_str, sizeof(conn->connection_string) - 1);
    conn->connection_string[sizeof(conn->connection_string) - 1] = '\0';

    printf("🔌 Database connection opened: ID=%d, %s\n", conn->connection_id, conn_str);
    return conn;
}

static void close_db_connection(void* resource) {
    DatabaseConnection* conn = (DatabaseConnection*)resource;
    if (conn && conn->is_open) {
        printf("🔌 Database connection closed: ID=%d\n", conn->connection_id);
        conn->is_open = false;
        free(conn);
    }
}

// Demo 5: Resource management
Result_void_ptr demo_resource_management(void) {
    printf("\n🛡️  Demo 5: Async Resource Management\n");
    printf("===================================\n");

    // Use resource with automatic cleanup
    {
        DatabaseConnection* conn = create_db_connection("postgresql://localhost:5432/testdb");
        CONCURRENT_RESOURCE(db_resource, conn, close_db_connection);
        
        // Simulate some work with the connection
        printf("💼 Working with database connection...\n");
        usleep(100000); // 100ms of work
        printf("💼 Database work completed\n");
        
        CONCURRENT_RESOURCE_END(db_resource);
        // Connection is automatically closed here
    }
    
    printf("✅ Resource management demo completed\n");
    return OK_PTR(NULL);
}

// Demo 6 per-item processor (file scope: C has no nested functions).
static Result_void_ptr process_item(void* item, size_t index, void* context) {
    int* value = (int*)item;
    int* counter = (int*)context;

    printf("  🔄 Processing item %zu: %d\n", index, *value);

    // Simulate processing time
    usleep(50000 + (rand() % 50000)); // 50-100ms

    *value *= *value; // Square the value
    (*counter)++;

    printf("  ✅ Item %zu processed: %d\n", index, *value);
    return OK_PTR(item);
}

// Demo 6: For-each parallel processing (no early-returning macros; stays void)
void demo_for_each_parallel(void) {
    printf("\n🔄 Demo 6: For-Each Parallel Processing\n");
    printf("======================================\n");
    
    // Initialize scheduler
    StructuredScheduler* scheduler = structured_scheduler_create(NULL);
    structured_scheduler_start(scheduler);
    structured_set_global_scheduler(scheduler);
    
    // Create dataset
    const int dataset_size = 10;
    int dataset[dataset_size];
    for (int i = 0; i < dataset_size; i++) {
        dataset[i] = i + 1;
    }
    
    void* items[dataset_size];
    for (int i = 0; i < dataset_size; i++) {
        items[i] = &dataset[i];
    }
    
    int processed_count = 0;

    // Execute for-each parallel processing (process_item is at file scope)
    ConcurrentBlockConfig config = concurrent_block_config_default();
    config.max_concurrent_tasks = 4;
    
    uint64_t start_time = async_get_timestamp_ns();
    
    Result_void_ptr result = concurrent_for_each(items, dataset_size, sizeof(int), 
                                               process_item, &processed_count, config);
    
    uint64_t end_time = async_get_timestamp_ns();
    double execution_time_ms = (end_time - start_time) / 1000000.0;
    
    if (!result.is_error) {
        printf("📊 For-each processing completed in %.2f ms\n", execution_time_ms);
        printf("📊 Processed %d/%d items\n", processed_count, dataset_size);
        
        printf("📊 Final values: ");
        for (int i = 0; i < dataset_size; i++) {
            printf("%d ", dataset[i]);
        }
        printf("\n");
    } else {
        printf("❌ For-each processing failed: %s\n", result.error->message);
    }
    
    // Cleanup
    structured_scheduler_shutdown(scheduler, 5000);
    structured_scheduler_destroy(scheduler);
}

int main(void) {
    printf("🎯 Enhanced Structured Concurrency Demo\n");
    printf("======================================\n");
    
    // Seed random number generator
    srand((unsigned int)time(NULL));
    
    // Run all demos
    demo_basic_concurrent_processing();
    demo_fail_fast_error_handling();
    demo_timeout_and_retry();
    demo_cpu_intensive_processing();
    demo_resource_management();
    demo_for_each_parallel();
    
    // Show final statistics
    printf("\n📈 Final Statistics\n");
    printf("==================\n");
    
    StructuredScheduler* scheduler = structured_get_global_scheduler();
    if (scheduler) {
        StructuredConcurrencyStats stats = structured_concurrency_get_stats(scheduler);
        printf("📊 Total blocks created: %llu\n", stats.total_blocks_created);
        printf("📊 Total expressions executed: %llu\n", stats.total_expressions_executed);
        printf("📊 Expression success rate: %.1f%%\n", stats.expression_success_rate * 100);
        printf("📊 Timeout events: %llu\n", stats.timeout_events);
        printf("📊 Retry events: %llu\n", stats.retry_events);
    }
    
    printf("\n🎉 All structured concurrency demos completed successfully!\n");
    return 0;
}