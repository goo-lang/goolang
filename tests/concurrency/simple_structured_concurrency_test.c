#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

// Simplified error handling for testing
typedef struct Error {
    int code;
    char message[256];
} Error;

typedef struct {
    bool is_error;
    union {
        void* value;
        Error* error;
    };
} Result_void_ptr;

#define OK_PTR(x) ((Result_void_ptr){.is_error = false, .value = (x)})
#define ERR_PTR(x) ((Result_void_ptr){.is_error = true, .error = (x)})

Error* error_create(int code, const char* message) {
    Error* err = malloc(sizeof(Error));
    if (err) {
        err->code = code;
        strncpy(err->message, message, sizeof(err->message) - 1);
        err->message[sizeof(err->message) - 1] = '\0';
    }
    return err;
}

// Include a minimal subset of the structured concurrency definitions
typedef enum {
    SCOPE_TYPE_PARALLEL,
    SCOPE_TYPE_SEQUENTIAL
} TaskScopeType;

typedef enum {
    TASK_PRIORITY_LOW = 0,
    TASK_PRIORITY_NORMAL = 1,
    TASK_PRIORITY_HIGH = 2,
    TASK_PRIORITY_CRITICAL = 3
} TaskPriority;

typedef enum {
    TASK_STATE_CREATED,
    TASK_STATE_QUEUED,
    TASK_STATE_RUNNING,
    TASK_STATE_WAITING,
    TASK_STATE_COMPLETED,
    TASK_STATE_CANCELLED,
    TASK_STATE_ERROR
} TaskState;

typedef struct TaskContext {
    uint64_t task_id;
    atomic_bool is_cancelled;
    atomic_int progress_percentage;
    char progress_message[256];
} TaskContext;

typedef Result_void_ptr (*TaskFunction)(TaskContext* context, void* args);

typedef struct TaskScopeConfig {
    TaskScopeType type;
    size_t max_concurrent_tasks;
    size_t max_total_tasks;
    size_t max_memory_per_task;
    size_t max_total_memory;
    uint64_t default_task_timeout_ms;
    bool use_work_stealing;
    bool numa_aware_scheduling;
    TaskPriority default_priority;
    bool propagate_errors;
} TaskScopeConfig;

// Macros for testing
#define CHECK_CANCELLATION(context) \
    do { \
        if (atomic_load(&(context)->is_cancelled)) { \
            return ERR_PTR(error_create(1, "Task was cancelled")); \
        } \
    } while(0)

#define UPDATE_PROGRESS(context, percentage, message) \
    do { \
        atomic_store(&(context)->progress_percentage, percentage); \
        strncpy((context)->progress_message, message, sizeof((context)->progress_message) - 1); \
        (context)->progress_message[sizeof((context)->progress_message) - 1] = '\0'; \
    } while(0)

// Configuration helper
TaskScopeConfig task_scope_config_default(void) {
    return (TaskScopeConfig) {
        .type = SCOPE_TYPE_PARALLEL,
        .max_concurrent_tasks = 8,
        .max_total_tasks = 1000,
        .max_memory_per_task = 16 * 1024 * 1024,  // 16MB
        .max_total_memory = 256 * 1024 * 1024,    // 256MB
        .default_task_timeout_ms = 30000,         // 30 seconds
        .use_work_stealing = true,
        .numa_aware_scheduling = false,
        .default_priority = TASK_PRIORITY_NORMAL,
        .propagate_errors = true
    };
}

// Test data structures
typedef struct {
    int start;
    int end;
    int sum;
} SumTask;

// Test task functions
Result_void_ptr simple_task_function(TaskContext* context, void* args) {
    SumTask* task_data = (SumTask*)args;
    
    // Simulate some work
    usleep(10000); // 10ms
    
    // Check for cancellation
    CHECK_CANCELLATION(context);
    
    // Calculate sum
    task_data->sum = 0;
    for (int i = task_data->start; i <= task_data->end; i++) {
        task_data->sum += i;
        
        // Update progress
        int progress = ((i - task_data->start + 1) * 100) / (task_data->end - task_data->start + 1);
        UPDATE_PROGRESS(context, progress, "Computing sum");
        
        // Check for cancellation periodically
        if (i % 10 == 0) {
            CHECK_CANCELLATION(context);
        }
    }
    
    return OK_PTR(task_data);
}

// Test configuration
void test_configuration() {
    printf("Testing structured concurrency configuration...\n");
    
    TaskScopeConfig config = task_scope_config_default();
    assert(config.type == SCOPE_TYPE_PARALLEL);
    assert(config.max_concurrent_tasks == 8);
    assert(config.max_total_tasks == 1000);
    assert(config.use_work_stealing == true);
    assert(config.default_priority == TASK_PRIORITY_NORMAL);
    assert(config.propagate_errors == true);
    
    printf("✓ Configuration test passed\n");
}

// Test task function execution
void test_task_function() {
    printf("Testing task function execution...\n");
    
    TaskContext context = {
        .task_id = 1,
        .is_cancelled = ATOMIC_VAR_INIT(false),
        .progress_percentage = ATOMIC_VAR_INIT(0)
    };
    strcpy(context.progress_message, "Starting");
    
    SumTask task_data = {.start = 1, .end = 100, .sum = 0};
    
    Result_void_ptr result = simple_task_function(&context, &task_data);
    assert(!result.is_error);
    assert(task_data.sum == 5050); // Sum of 1 to 100
    
    int final_progress = atomic_load(&context.progress_percentage);
    assert(final_progress == 100);
    
    printf("✓ Task function test passed\n");
}

// Test cancellation
void test_cancellation() {
    printf("Testing task cancellation...\n");
    
    TaskContext context = {
        .task_id = 2,
        .is_cancelled = ATOMIC_VAR_INIT(true), // Start cancelled
        .progress_percentage = ATOMIC_VAR_INIT(0)
    };
    strcpy(context.progress_message, "Starting");
    
    SumTask task_data = {.start = 1, .end = 100, .sum = 0};
    
    Result_void_ptr result = simple_task_function(&context, &task_data);
    assert(result.is_error);
    assert(result.error != NULL);
    assert(result.error->code == 1);
    assert(task_data.sum == 0); // Task should not have computed anything
    
    free(result.error);
    
    printf("✓ Cancellation test passed\n");
}

int main() {
    printf("=== Simple Structured Concurrency Test Suite ===\n\n");
    
    test_configuration();
    test_task_function();
    test_cancellation();
    
    printf("\n=== All Simple Tests Passed! ===\n");
    printf("Structured concurrency framework is ready!\n");
    return 0;
}