#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "include/shared_variables.h"

// Test data structure for threaded tests
typedef struct {
    SharedVarManager* manager;
    SharedVariable* counter;
    SharedVariable* flag;
    int thread_id;
    int iterations;
} ThreadTestData;

// Test basic shared variable manager creation
void test_manager_creation() {
    printf("Testing shared variable manager creation...\n");
    
    SharedVarManager* manager = shared_var_manager_create(100);
    assert(manager != NULL);
    assert(manager->max_variables == 100);
    assert(manager->variable_count == 0);
    assert(manager->enable_global_statistics == true);
    
    shared_var_manager_destroy(manager);
    
    printf("✓ Manager creation test passed\n");
}

// Test shared variable creation with different types
void test_variable_creation() {
    printf("Testing shared variable creation...\n");
    
    SharedVarManager* manager = shared_var_manager_create(50);
    
    // Test int32 variable with atomic mode
    SharedVarConfig config = shared_var_config_atomic("test_int32", SHARED_TYPE_INT32);
    SharedVariable* int32_var = shared_var_create(manager, config);
    assert(int32_var != NULL);
    assert(int32_var->config.type == SHARED_TYPE_INT32);
    assert(int32_var->config.sync_mode == SYNC_MODE_ATOMIC);
    assert(strcmp(int32_var->name, "test_int32") == 0);
    
    // Test int64 variable with read-heavy configuration
    config = shared_var_config_read_heavy("test_int64", SHARED_TYPE_INT64);
    SharedVariable* int64_var = shared_var_create(manager, config);
    assert(int64_var != NULL);
    assert(int64_var->config.type == SHARED_TYPE_INT64);
    assert(int64_var->config.sync_mode == SYNC_MODE_RW_LOCK);
    assert(int64_var->config.access_pattern == ACCESS_PATTERN_READ_HEAVY);
    
    // Test bool variable with write-heavy configuration
    config = shared_var_config_write_heavy("test_bool", SHARED_TYPE_BOOL);
    SharedVariable* bool_var = shared_var_create(manager, config);
    assert(bool_var != NULL);
    assert(bool_var->config.type == SHARED_TYPE_BOOL);
    assert(bool_var->config.sync_mode == SYNC_MODE_MUTEX);
    
    // Test pointer variable with high contention
    config = shared_var_config_high_contention("test_ptr", SHARED_TYPE_PTR);
    SharedVariable* ptr_var = shared_var_create(manager, config);
    assert(ptr_var != NULL);
    assert(ptr_var->config.type == SHARED_TYPE_PTR);
    assert(ptr_var->config.sync_mode == SYNC_MODE_ADAPTIVE);
    
    // Verify manager statistics
    assert(manager->variable_count == 4);
    
    shared_var_manager_destroy(manager);
    
    printf("✓ Variable creation test passed\n");
}

// Test atomic operations on shared variables
void test_atomic_operations() {
    printf("Testing atomic operations...\n");
    
    SharedVarManager* manager = shared_var_manager_create(50);
    
    // Create atomic int32 variable
    SharedVarConfig config = shared_var_config_atomic("atomic_counter", SHARED_TYPE_INT32);
    SharedVariable* counter = shared_var_create(manager, config);
    
    // Test initial value (should be 0)
    Result_int32_t read_result = shared_var_get_int32(counter);
    assert(!read_result.is_error);
    assert(read_result.value == 0);
    
    // Test atomic write
    Result_void_ptr write_result = shared_var_set_int32(counter, 42);
    assert(!write_result.is_error);
    
    // Test atomic read after write
    read_result = shared_var_get_int32(counter);
    assert(!read_result.is_error);
    assert(read_result.value == 42);
    
    // Test atomic fetch_add
    Result_int32_t add_result = shared_var_fetch_add_int32(counter, 8);
    assert(!add_result.is_error);
    assert(add_result.value == 42); // Should return old value
    
    // Verify new value after add
    read_result = shared_var_get_int32(counter);
    assert(!read_result.is_error);
    assert(read_result.value == 50);
    
    // Test compare and swap - successful case
    Result_bool cas_result = shared_var_cas_int32(counter, 50, 100);
    assert(!cas_result.is_error);
    assert(cas_result.value == true);
    
    // Verify value changed
    read_result = shared_var_get_int32(counter);
    assert(!read_result.is_error);
    assert(read_result.value == 100);
    
    // Test compare and swap - failure case
    cas_result = shared_var_cas_int32(counter, 50, 200); // Expected value is wrong
    assert(!cas_result.is_error);
    assert(cas_result.value == false);
    
    // Verify value didn't change
    read_result = shared_var_get_int32(counter);
    assert(!read_result.is_error);
    assert(read_result.value == 100);
    
    shared_var_manager_destroy(manager);
    
    printf("✓ Atomic operations test passed\n");
}

// Test different synchronization modes
void test_sync_modes() {
    printf("Testing different synchronization modes...\n");
    
    SharedVarManager* manager = shared_var_manager_create(50);
    
    // Test mutex mode
    SharedVarConfig config = shared_var_config_write_heavy("mutex_var", SHARED_TYPE_INT64);
    SharedVariable* mutex_var = shared_var_create(manager, config);
    
    shared_var_set_int64(mutex_var, 1000);
    Result_int64_t result = shared_var_get_int64(mutex_var);
    assert(!result.is_error);
    assert(result.value == 1000);
    
    // Test read-write lock mode
    config = shared_var_config_read_heavy("rwlock_var", SHARED_TYPE_UINT32);
    SharedVariable* rwlock_var = shared_var_create(manager, config);
    
    shared_var_set_uint32(rwlock_var, 2000);
    Result_uint32_t result_u32 = shared_var_get_uint32(rwlock_var);
    assert(!result_u32.is_error);
    assert(result_u32.value == 2000);
    
    // Test adaptive mode
    config = shared_var_config_high_contention("adaptive_var", SHARED_TYPE_BOOL);
    SharedVariable* adaptive_var = shared_var_create(manager, config);
    
    shared_var_set_bool(adaptive_var, true);
    Result_bool result_bool = shared_var_get_bool(adaptive_var);
    assert(!result_bool.is_error);
    assert(result_bool.value == true);
    
    shared_var_manager_destroy(manager);
    
    printf("✓ Synchronization modes test passed\n");
}

// Test variable finding by name and ID
void test_variable_finding() {
    printf("Testing variable finding...\n");
    
    SharedVarManager* manager = shared_var_manager_create(50);
    
    // Create some variables
    SharedVarConfig config = shared_var_config_default("findable_var1", SHARED_TYPE_INT32);
    SharedVariable* var1 = shared_var_create(manager, config);
    uint64_t var1_id = var1->id;
    
    config = shared_var_config_default("findable_var2", SHARED_TYPE_INT64);
    SharedVariable* var2 = shared_var_create(manager, config);
    uint64_t var2_id = var2->id;
    
    // Test finding by name
    SharedVariable* found_var = shared_var_find_by_name(manager, "findable_var1");
    assert(found_var != NULL);
    assert(found_var->id == var1_id);
    assert(strcmp(found_var->name, "findable_var1") == 0);
    
    // Test finding by ID
    found_var = shared_var_find_by_id(manager, var2_id);
    assert(found_var != NULL);
    assert(found_var->id == var2_id);
    assert(strcmp(found_var->name, "findable_var2") == 0);
    
    // Test not finding non-existent variable
    found_var = shared_var_find_by_name(manager, "nonexistent");
    assert(found_var == NULL);
    
    found_var = shared_var_find_by_id(manager, 99999);
    assert(found_var == NULL);
    
    shared_var_manager_destroy(manager);
    
    printf("✓ Variable finding test passed\n");
}

// Thread function for testing concurrent access
void* thread_increment_counter(void* arg) {
    ThreadTestData* data = (ThreadTestData*)arg;
    
    for (int i = 0; i < data->iterations; i++) {
        // Increment the shared counter
        shared_var_fetch_add_int32(data->counter, 1);
        
        // Small delay to increase chance of contention
        usleep(1);
    }
    
    return NULL;
}

// Test concurrent access with multiple threads
void test_concurrent_access() {
    printf("Testing concurrent access...\n");
    
    SharedVarManager* manager = shared_var_manager_create(50);
    
    // Create atomic counter
    SharedVarConfig config = shared_var_config_atomic("concurrent_counter", SHARED_TYPE_INT32);
    SharedVariable* counter = shared_var_create(manager, config);
    
    const int num_threads = 4;
    const int iterations_per_thread = 1000;
    pthread_t threads[num_threads];
    ThreadTestData thread_data[num_threads];
    
    // Initialize thread data
    for (int i = 0; i < num_threads; i++) {
        thread_data[i].manager = manager;
        thread_data[i].counter = counter;
        thread_data[i].thread_id = i;
        thread_data[i].iterations = iterations_per_thread;
    }
    
    // Start threads
    for (int i = 0; i < num_threads; i++) {
        int result = pthread_create(&threads[i], NULL, thread_increment_counter, &thread_data[i]);
        assert(result == 0);
    }
    
    // Wait for threads to complete
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // Verify final counter value
    Result_int32_t final_result = shared_var_get_int32(counter);
    assert(!final_result.is_error);
    int expected_value = num_threads * iterations_per_thread;
    assert(final_result.value == expected_value);
    
    printf("  Final counter value: %d (expected: %d)\n", final_result.value, expected_value);
    
    // Check statistics
    assert(counter->stats.total_writes > 0);
    assert(counter->stats.total_reads >= 1); // At least the final read
    
    shared_var_manager_destroy(manager);
    
    printf("✓ Concurrent access test passed\n");
}

// Test error handling
void test_error_handling() {
    printf("Testing error handling...\n");
    
    SharedVarManager* manager = shared_var_manager_create(50);
    
    // Create int32 variable
    SharedVarConfig config = shared_var_config_atomic("test_var", SHARED_TYPE_INT32);
    SharedVariable* var = shared_var_create(manager, config);
    
    // Test type mismatch errors
    Result_int64_t bad_read = shared_var_get_int64(var); // Wrong type
    assert(bad_read.is_error);
    
    Result_void_ptr bad_write = shared_var_set_int64(var, 123); // Wrong type
    assert(bad_write.is_error);
    
    // Test NULL parameter handling
    Result_int32_t null_read = shared_var_get_int32(NULL);
    assert(null_read.is_error);
    
    Result_void_ptr null_write = shared_var_set_int32(NULL, 123);
    assert(null_write.is_error);
    
    shared_var_manager_destroy(manager);
    
    printf("✓ Error handling test passed\n");
}

// Test statistics collection
void test_statistics() {
    printf("Testing statistics collection...\n");
    
    SharedVarManager* manager = shared_var_manager_create(50);
    
    // Create variable with statistics enabled
    SharedVarConfig config = shared_var_config_atomic("stats_var", SHARED_TYPE_INT32);
    config.enable_statistics = true;
    SharedVariable* var = shared_var_create(manager, config);
    
    // Perform some operations
    shared_var_set_int32(var, 10);
    shared_var_get_int32(var);
    shared_var_get_int32(var);
    shared_var_fetch_add_int32(var, 5);
    shared_var_cas_int32(var, 15, 20);
    
    // Check variable statistics
    assert(var->stats.total_reads >= 2);
    assert(var->stats.total_writes >= 3); // set, add, cas
    assert(var->stats.total_cas_attempts >= 1);
    assert(var->stats.successful_cas >= 1);
    
    // Check manager statistics
    uint64_t total_vars, total_ops, success_rate;
    shared_var_manager_get_stats(manager);
    
    printf("  Variable stats - Reads: %llu, Writes: %llu, CAS attempts: %llu, Successful CAS: %llu\n",
           var->stats.total_reads, var->stats.total_writes, 
           var->stats.total_cas_attempts, var->stats.successful_cas);
    
    shared_var_manager_destroy(manager);
    
    printf("✓ Statistics test passed\n");
}

// Performance test with many operations
void test_performance() {
    printf("Testing performance...\n");
    
    SharedVarManager* manager = shared_var_manager_create(10);
    
    // Create atomic counter for performance test
    SharedVarConfig config = shared_var_config_atomic("perf_counter", SHARED_TYPE_INT64);
    SharedVariable* counter = shared_var_create(manager, config);
    
    const int num_operations = 100000;
    clock_t start_time = clock();
    
    // Perform many atomic increments
    for (int i = 0; i < num_operations; i++) {
        shared_var_fetch_add_int64(counter, 1);
    }
    
    clock_t end_time = clock();
    double elapsed = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    double ops_per_sec = num_operations / elapsed;
    
    // Verify final value
    Result_int64_t final_result = shared_var_get_int64(counter);
    assert(!final_result.is_error);
    assert(final_result.value == num_operations);
    
    printf("  Performed %d atomic operations in %.3f seconds (%.0f ops/sec)\n", 
           num_operations, elapsed, ops_per_sec);
    
    shared_var_manager_destroy(manager);
    
    printf("✓ Performance test passed\n");
}

// Test string operations
void test_string_operations() {
    printf("Testing string operations...\n");
    
    SharedVarManager* manager = shared_var_manager_create(50);
    
    // Create string variable with mutex mode for safety
    SharedVarConfig config = shared_var_config_write_heavy("test_string", SHARED_TYPE_STRING);
    SharedVariable* string_var = shared_var_create(manager, config);
    
    // Test setting and getting string values
    Result_void_ptr set_result = shared_var_set_string(string_var, "Hello, World!");
    assert(!set_result.is_error);
    
    char buffer[256];
    Result_void_ptr get_result = shared_var_get_string(string_var, buffer, sizeof(buffer));
    assert(!get_result.is_error);
    assert(strcmp(buffer, "Hello, World!") == 0);
    
    // Test updating string
    set_result = shared_var_set_string(string_var, "Updated string value");
    assert(!set_result.is_error);
    
    get_result = shared_var_get_string(string_var, buffer, sizeof(buffer));
    assert(!get_result.is_error);
    assert(strcmp(buffer, "Updated string value") == 0);
    
    // Test setting NULL string
    set_result = shared_var_set_string(string_var, NULL);
    assert(!set_result.is_error);
    
    get_result = shared_var_get_string(string_var, buffer, sizeof(buffer));
    assert(!get_result.is_error);
    assert(strlen(buffer) == 0);
    
    shared_var_manager_destroy(manager);
    
    printf("✓ String operations test passed\n");
}

// Custom type example for testing
typedef struct {
    int x, y;
    char name[32];
} CustomPoint;

// Custom synchronization operations for CustomPoint
bool custom_point_try_lock(void* sync_state) {
    pthread_mutex_t* mutex = (pthread_mutex_t*)sync_state;
    return pthread_mutex_trylock(mutex) == 0;
}

void custom_point_lock(void* sync_state) {
    pthread_mutex_t* mutex = (pthread_mutex_t*)sync_state;
    pthread_mutex_lock(mutex);
}

void custom_point_unlock(void* sync_state) {
    pthread_mutex_t* mutex = (pthread_mutex_t*)sync_state;
    pthread_mutex_unlock(mutex);
}

void custom_point_serialize(const void* value, void* buffer, size_t* size) {
    const CustomPoint* point = (const CustomPoint*)value;
    if (*size >= sizeof(CustomPoint)) {
        memcpy(buffer, point, sizeof(CustomPoint));
        *size = sizeof(CustomPoint);
    }
}

void custom_point_deserialize(void* value, const void* buffer, size_t size) {
    CustomPoint* point = (CustomPoint*)value;
    if (size >= sizeof(CustomPoint)) {
        memcpy(point, buffer, sizeof(CustomPoint));
    }
}

void custom_point_destroy(void* sync_state) {
    pthread_mutex_t* mutex = (pthread_mutex_t*)sync_state;
    pthread_mutex_destroy(mutex);
    free(sync_state);
}

// Test custom type operations
void test_custom_type_operations() {
    printf("Testing custom type operations...\n");
    
    SharedVarManager* manager = shared_var_manager_create(50);
    
    // Set up custom synchronization
    pthread_mutex_t* custom_mutex = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(custom_mutex, NULL);
    
    CustomSyncOps custom_ops = {
        .try_lock = custom_point_try_lock,
        .lock = custom_point_lock,
        .unlock = custom_point_unlock,
        .serialize = custom_point_serialize,
        .deserialize = custom_point_deserialize,
        .destroy = custom_point_destroy
    };
    
    // Create custom type variable
    SharedVarConfig config = shared_var_config_default("custom_point", SHARED_TYPE_CUSTOM);
    config.custom_ops = &custom_ops;
    config.custom_sync_state = custom_mutex;
    
    SharedVariable* custom_var = shared_var_create(manager, config);
    assert(custom_var != NULL);
    
    // Create test data
    CustomPoint original_point = {.x = 10, .y = 20};
    strcpy(original_point.name, "TestPoint");
    
    // Test setting custom value
    Result_void_ptr set_result = shared_var_set_custom(custom_var, &original_point, sizeof(CustomPoint));
    assert(!set_result.is_error);
    
    // Test getting custom value
    CustomPoint retrieved_point;
    Result_void_ptr get_result = shared_var_get_custom(custom_var, &retrieved_point, sizeof(CustomPoint));
    assert(!get_result.is_error);
    assert(retrieved_point.x == 10);
    assert(retrieved_point.y == 20);
    assert(strcmp(retrieved_point.name, "TestPoint") == 0);
    
    // Test custom compare-and-swap
    CustomPoint new_point = {.x = 30, .y = 40};
    strcpy(new_point.name, "NewPoint");
    
    Result_bool cas_result = shared_var_cas_custom(custom_var, &original_point, &new_point, sizeof(CustomPoint));
    assert(!cas_result.is_error);
    assert(cas_result.value == true);
    
    // Verify the change
    get_result = shared_var_get_custom(custom_var, &retrieved_point, sizeof(CustomPoint));
    assert(!get_result.is_error);
    assert(retrieved_point.x == 30);
    assert(retrieved_point.y == 40);
    assert(strcmp(retrieved_point.name, "NewPoint") == 0);
    
    shared_var_manager_destroy(manager);
    
    printf("✓ Custom type operations test passed\n");
}

// Test Software Transactional Memory (STM)
void test_stm_operations() {
    printf("Testing Software Transactional Memory (STM)...\n");
    
    SharedVarManager* manager = shared_var_manager_create(50);
    
    // Create variables for STM testing
    SharedVarConfig config1 = shared_var_config_atomic("stm_var1", SHARED_TYPE_INT32);
    SharedVariable* var1 = shared_var_create(manager, config1);
    shared_var_set_int32(var1, 100);
    
    SharedVarConfig config2 = shared_var_config_atomic("stm_var2", SHARED_TYPE_INT32);
    SharedVariable* var2 = shared_var_create(manager, config2);
    shared_var_set_int32(var2, 200);
    
    // Test successful transaction
    STMTransaction* tx = stm_begin_transaction();
    assert(tx != NULL);
    
    // Read both variables in transaction
    int32_t value1, value2;
    Result_void_ptr read_result1 = stm_read(tx, var1, &value1, sizeof(int32_t));
    assert(!read_result1.is_error);
    assert(value1 == 100);
    
    Result_void_ptr read_result2 = stm_read(tx, var2, &value2, sizeof(int32_t));
    assert(!read_result2.is_error);
    assert(value2 == 200);
    
    // Write new values in transaction
    int32_t new_value1 = 150;
    int32_t new_value2 = 250;
    Result_void_ptr write_result1 = stm_write(tx, var1, &new_value1, sizeof(int32_t));
    assert(!write_result1.is_error);
    
    Result_void_ptr write_result2 = stm_write(tx, var2, &new_value2, sizeof(int32_t));
    assert(!write_result2.is_error);
    
    // Commit transaction
    Result_void_ptr commit_result = stm_commit(tx);
    assert(!commit_result.is_error);
    
    // Verify changes were applied
    Result_int32_t final_result1 = shared_var_get_int32(var1);
    assert(!final_result1.is_error);
    assert(final_result1.value == 150);
    
    Result_int32_t final_result2 = shared_var_get_int32(var2);
    assert(!final_result2.is_error);
    assert(final_result2.value == 250);
    
    // Clean up transaction
    stm_destroy_transaction(tx);
    
    printf("  STM transaction completed successfully\n");
    
    // Test conflict detection
    tx = stm_begin_transaction();
    
    // Read variable
    Result_void_ptr read_result = stm_read(tx, var1, &value1, sizeof(int32_t));
    assert(!read_result.is_error);
    
    // Modify variable outside transaction to create conflict
    shared_var_set_int32(var1, 999);
    
    // Try to commit - should fail due to conflict
    commit_result = stm_commit(tx);
    assert(commit_result.is_error);
    
    printf("  STM conflict detection working correctly\n");
    
    stm_destroy_transaction(tx);
    shared_var_manager_destroy(manager);
    
    printf("✓ STM operations test passed\n");
}

int main() {
    printf("=== Shared Variables with Automatic Synchronization Tests ===\n\n");
    
    test_manager_creation();
    test_variable_creation();
    test_atomic_operations();
    test_sync_modes();
    test_variable_finding();
    test_concurrent_access();
    test_error_handling();
    test_statistics();
    test_performance();
    test_string_operations();
    test_custom_type_operations();
    test_stm_operations();
    
    printf("\n=== All Shared Variables Tests Passed! ===\n");
    return 0;
}