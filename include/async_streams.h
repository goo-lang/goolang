#ifndef GOO_ASYNC_STREAMS_H
#define GOO_ASYNC_STREAMS_H

#include "transparent_async.h"
#include "structured_concurrency_enhanced.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "ccomp_shim.h"

// Forward declarations
typedef struct AsyncIterator AsyncIterator;
typedef struct AsyncStream AsyncStream;
typedef struct StreamPipeline StreamPipeline;
typedef struct StreamBuffer StreamBuffer;
typedef struct StreamWindow StreamWindow;
typedef struct BackpressureController BackpressureController;

// Async iterator state
typedef enum {
    ASYNC_ITER_CREATED,
    ASYNC_ITER_ACTIVE,
    ASYNC_ITER_SUSPENDED,
    ASYNC_ITER_COMPLETED,
    ASYNC_ITER_ERROR,
    ASYNC_ITER_CANCELLED
} AsyncIteratorState;

// Stream operation types
typedef enum {
    STREAM_OP_MAP,
    STREAM_OP_FILTER,
    STREAM_OP_BATCH,
    STREAM_OP_WINDOW,
    STREAM_OP_REDUCE,
    STREAM_OP_COLLECT,
    STREAM_OP_PARALLEL,
    STREAM_OP_THROTTLE,
    STREAM_OP_BUFFER,
    STREAM_OP_MERGE,
    STREAM_OP_ZIP,
    STREAM_OP_FLATTEN
} StreamOperationType;

// Backpressure strategy
typedef enum {
    BACKPRESSURE_BLOCK,      // Block producer when buffer is full
    BACKPRESSURE_DROP_OLD,   // Drop oldest items when buffer is full
    BACKPRESSURE_DROP_NEW,   // Drop new items when buffer is full
    BACKPRESSURE_ERROR       // Error when buffer is full
} BackpressureStrategy;

// Stream item wrapper
typedef struct StreamItem {
    void* data;
    size_t size;
    uint64_t sequence_number;
    uint64_t timestamp_ns;
    bool is_end_marker;
    Error* error;
    
    // Reference counting for zero-copy operations
    atomic_int ref_count;
    void (*destructor)(void* data);
} StreamItem;

// Async iterator function signatures
typedef Result_void_ptr (*AsyncIteratorNext)(AsyncIterator* iter, StreamItem** item);
typedef void (*AsyncIteratorCleanup)(AsyncIterator* iter);
typedef bool (*AsyncIteratorHasNext)(AsyncIterator* iter);

// Stream processing function signatures
typedef Result_void_ptr (*StreamMapFunction)(StreamItem* input, StreamItem** output, void* context);
typedef bool (*StreamFilterFunction)(StreamItem* input, void* context);
typedef Result_void_ptr (*StreamReduceFunction)(StreamItem* accumulator, StreamItem* input, void* context);
typedef Result_void_ptr (*StreamBatchFunction)(StreamItem** batch, size_t batch_size, StreamItem** output, void* context);

// Async iterator core structure
typedef struct AsyncIterator {
    uint64_t id;
    char name[64];
    AsyncIteratorState state;
    
    // Iterator functions
    AsyncIteratorNext next_fn;
    AsyncIteratorHasNext has_next_fn;
    AsyncIteratorCleanup cleanup_fn;
    
    // Iterator context
    void* context;
    size_t context_size;
    
    // Current position and state
    uint64_t current_position;
    uint64_t total_items;
    bool is_infinite;
    
    // Async execution context
    AsyncContext* async_context;
    CancellationToken* cancel_token;
    
    // Buffer for yielded items
    StreamItem** item_buffer;
    size_t buffer_size;
    size_t buffer_capacity;
    size_t buffer_head;
    size_t buffer_tail;
    
    // Synchronization
    pthread_mutex_t iter_mutex;
    pthread_cond_t item_available;
    pthread_cond_t buffer_space;
    
    // Statistics
    uint64_t items_produced;
    uint64_t items_consumed;
    uint64_t total_yield_time_ns;
    
    // Error handling
    Error* last_error;
} AsyncIterator;

// Backpressure controller
typedef struct BackpressureController {
    BackpressureStrategy strategy;
    size_t max_buffer_size;
    size_t current_buffer_size;
    atomic_size_t pending_items;
    
    // Adaptive backpressure
    double target_latency_ms;
    double current_latency_ms;
    double throughput_items_per_sec;
    
    // Throttling
    uint64_t max_items_per_second;
    uint64_t items_this_second;
    uint64_t last_second_timestamp;
    
    // Statistics
    uint64_t total_blocked_time_ns;
    uint64_t total_dropped_items;
    uint64_t backpressure_events;
    
    pthread_mutex_t backpressure_mutex;
} BackpressureController;

// Stream buffer for intermediate storage
typedef struct StreamBuffer {
    StreamItem** items;
    size_t capacity;
    size_t size;
    size_t head;
    size_t tail;
    
    BackpressureController* backpressure;
    
    // Buffer state
    bool is_closed;
    atomic_bool has_error;
    Error* buffer_error;
    
    // Synchronization
    pthread_mutex_t buffer_mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    
    // Statistics
    uint64_t total_items_added;
    uint64_t total_items_removed;
    size_t peak_size;
} StreamBuffer;

// Stream window for windowing operations
typedef struct StreamWindow {
    uint64_t window_id;
    
    // Window configuration
    enum {
        WINDOW_TIME_BASED,
        WINDOW_COUNT_BASED,
        WINDOW_SESSION_BASED
    } window_type;
    
    union {
        struct {
            uint64_t window_size_ms;
            uint64_t slide_interval_ms;
        } time_window;
        
        struct {
            size_t window_size;
            size_t slide_size;
        } count_window;
        
        struct {
            uint64_t session_timeout_ms;
            bool (*session_key_fn)(StreamItem* item, void* context);
            void* key_context;
        } session_window;
    } config;
    
    // Window state
    StreamItem** items;
    size_t item_count;
    size_t item_capacity;
    uint64_t window_start_time;
    uint64_t window_end_time;
    
    // Window management
    bool is_complete;
    bool is_triggered;
    
    struct StreamWindow* next;
} StreamWindow;

// Stream operation descriptor
typedef struct StreamOperation {
    uint64_t id;
    StreamOperationType type;
    char name[64];
    
    // Operation function pointers
    union {
        struct {
            StreamMapFunction map_fn;
            void* context;
            size_t context_size;
        } map_op;
        
        struct {
            StreamFilterFunction filter_fn;
            void* context;
            size_t context_size;
        } filter_op;
        
        struct {
            size_t batch_size;
            uint64_t batch_timeout_ms;
            StreamBatchFunction batch_fn;
            void* context;
        } batch_op;
        
        struct {
            StreamWindow* window_template;
            StreamReduceFunction reduce_fn;
            void* context;
        } window_op;
        
        struct {
            size_t parallel_degree;
            ConcurrentBlockConfig concurrent_config;
        } parallel_op;
        
        struct {
            size_t buffer_size;
            BackpressureStrategy strategy;
        } buffer_op;
        
        struct {
            uint64_t max_items_per_second;
            uint64_t burst_size;
        } throttle_op;
    } config;
    
    // Operation state
    StreamBuffer* input_buffer;
    StreamBuffer* output_buffer;
    ConcurrentBlock* execution_block;
    
    // Statistics
    uint64_t items_processed;
    uint64_t processing_time_ns;
    uint64_t error_count;
    
    struct StreamOperation* next;
} StreamOperation;

// Stream pipeline for chaining operations
typedef struct StreamPipeline {
    uint64_t id;
    char name[64];
    
    // Pipeline structure
    AsyncIterator* source_iterator;
    StreamOperation* first_operation;
    StreamOperation* last_operation;
    size_t operation_count;
    
    // Pipeline configuration
    BackpressureController* global_backpressure;
    size_t default_buffer_size;
    bool auto_start;
    
    // Execution state
    enum {
        PIPELINE_CREATED,
        PIPELINE_STARTING,
        PIPELINE_RUNNING,
        PIPELINE_PAUSED,
        PIPELINE_STOPPING,
        PIPELINE_STOPPED,
        PIPELINE_ERROR
    } state;
    
    // Execution context
    StructuredScheduler* scheduler;
    ConcurrentBlock* pipeline_block;
    
    // Output handling
    StreamBuffer* output_buffer;
    AsyncIterator* output_iterator;
    
    // Error handling
    Error* pipeline_error;
    bool stop_on_error;
    
    // Resource management
    pthread_t* worker_threads;
    size_t worker_count;
    
    // Synchronization
    pthread_mutex_t pipeline_mutex;
    pthread_cond_t state_changed;
    
    // Statistics
    uint64_t total_items_processed;
    uint64_t pipeline_start_time;
    uint64_t pipeline_end_time;
    double throughput_items_per_sec;
    
    // Monitoring
    uint64_t last_stats_time;
    size_t items_since_last_stats;
} StreamPipeline;

// Async stream - high-level stream abstraction
typedef struct AsyncStream {
    uint64_t id;
    char name[64];
    
    // Underlying pipeline
    StreamPipeline* pipeline;
    AsyncIterator* iterator;
    
    // Stream configuration
    size_t default_buffer_size;
    BackpressureStrategy default_backpressure;
    bool auto_close_on_error;
    
    // Stream state
    bool is_started;
    bool is_closed;
    atomic_size_t active_consumers;
    
    // Type information
    size_t item_size;
    const char* item_type_name;
    
    // Error handling
    Error* stream_error;
    void (*error_handler)(AsyncStream* stream, Error* error, void* context);
    void* error_context;
} AsyncStream;

// Core async iterator operations
AsyncIterator* async_iterator_create(const char* name, 
    AsyncIteratorNext next_fn, AsyncIteratorHasNext has_next_fn, 
    AsyncIteratorCleanup cleanup_fn, void* context, size_t context_size);
void async_iterator_destroy(AsyncIterator* iter);

Result_void_ptr async_iterator_start(AsyncIterator* iter);
Result_void_ptr async_iterator_next(AsyncIterator* iter, StreamItem** item);
bool async_iterator_has_next(AsyncIterator* iter);
Result_void_ptr async_iterator_cancel(AsyncIterator* iter);

// Stream item operations
StreamItem* stream_item_create(void* data, size_t size);
StreamItem* stream_item_create_with_destructor(void* data, size_t size, void (*destructor)(void*));
StreamItem* stream_item_ref(StreamItem* item);
void stream_item_unref(StreamItem* item);
StreamItem* stream_item_end_marker(void);
StreamItem* stream_item_error(Error* error);

// Stream buffer operations
StreamBuffer* stream_buffer_create(size_t capacity, BackpressureStrategy strategy);
void stream_buffer_destroy(StreamBuffer* buffer);
Result_void_ptr stream_buffer_put(StreamBuffer* buffer, StreamItem* item, uint64_t timeout_ms);
Result_void_ptr stream_buffer_get(StreamBuffer* buffer, StreamItem** item, uint64_t timeout_ms);
Result_void_ptr stream_buffer_close(StreamBuffer* buffer);
bool stream_buffer_is_empty(StreamBuffer* buffer);
bool stream_buffer_is_full(StreamBuffer* buffer);

// Backpressure controller operations
BackpressureController* backpressure_controller_create(BackpressureStrategy strategy, size_t max_buffer_size);
void backpressure_controller_destroy(BackpressureController* controller);
Result_void_ptr backpressure_controller_check(BackpressureController* controller, size_t pending_items);
void backpressure_controller_update_metrics(BackpressureController* controller, 
    double latency_ms, double throughput);

// Stream pipeline operations
StreamPipeline* stream_pipeline_create(const char* name, AsyncIterator* source);
void stream_pipeline_destroy(StreamPipeline* pipeline);

Result_void_ptr stream_pipeline_start(StreamPipeline* pipeline);
Result_void_ptr stream_pipeline_stop(StreamPipeline* pipeline, uint64_t timeout_ms);
Result_void_ptr stream_pipeline_pause(StreamPipeline* pipeline);
Result_void_ptr stream_pipeline_resume(StreamPipeline* pipeline);

// Pipeline operation builders
Result_void_ptr stream_pipeline_map(StreamPipeline* pipeline, const char* name,
    StreamMapFunction map_fn, void* context, size_t context_size);
Result_void_ptr stream_pipeline_filter(StreamPipeline* pipeline, const char* name,
    StreamFilterFunction filter_fn, void* context, size_t context_size);
Result_void_ptr stream_pipeline_batch(StreamPipeline* pipeline, const char* name,
    size_t batch_size, uint64_t timeout_ms, StreamBatchFunction batch_fn, void* context);
Result_void_ptr stream_pipeline_parallel(StreamPipeline* pipeline, const char* name,
    size_t parallel_degree, ConcurrentBlockConfig config);
Result_void_ptr stream_pipeline_buffer(StreamPipeline* pipeline, const char* name,
    size_t buffer_size, BackpressureStrategy strategy);
Result_void_ptr stream_pipeline_throttle(StreamPipeline* pipeline, const char* name,
    uint64_t max_items_per_second, uint64_t burst_size);

// Stream window operations
StreamWindow* stream_window_time_based(uint64_t window_size_ms, uint64_t slide_interval_ms);
StreamWindow* stream_window_count_based(size_t window_size, size_t slide_size);
StreamWindow* stream_window_session_based(uint64_t session_timeout_ms, 
    bool (*session_key_fn)(StreamItem*, void*), void* context);
void stream_window_destroy(StreamWindow* window);

Result_void_ptr stream_pipeline_window(StreamPipeline* pipeline, const char* name,
    StreamWindow* window, StreamReduceFunction reduce_fn, void* context);

// High-level async stream operations
AsyncStream* async_stream_create(const char* name, AsyncIterator* source, size_t item_size);
AsyncStream* async_stream_from_array(const char* name, void** items, size_t item_count, size_t item_size);
AsyncStream* async_stream_from_range(const char* name, int start, int end, int step);
AsyncStream* async_stream_from_generator(const char* name, 
    Result_void_ptr (*generator)(uint64_t index, void** item, void* context), void* context);
void async_stream_destroy(AsyncStream* stream);

// Stream transformation operations
AsyncStream* async_stream_map(AsyncStream* stream, const char* name,
    StreamMapFunction map_fn, void* context);
AsyncStream* async_stream_filter(AsyncStream* stream, const char* name,
    StreamFilterFunction filter_fn, void* context);
AsyncStream* async_stream_batch(AsyncStream* stream, const char* name,
    size_t batch_size, uint64_t timeout_ms);
AsyncStream* async_stream_parallel(AsyncStream* stream, const char* name,
    size_t parallel_degree);
AsyncStream* async_stream_buffer(AsyncStream* stream, const char* name,
    size_t buffer_size, BackpressureStrategy strategy);
AsyncStream* async_stream_throttle(AsyncStream* stream, const char* name,
    uint64_t max_items_per_second);

// Stream combination operations
AsyncStream* async_stream_merge(AsyncStream** streams, size_t stream_count, const char* name);
AsyncStream* async_stream_zip(AsyncStream* stream1, AsyncStream* stream2, const char* name,
    Result_void_ptr (*zip_fn)(StreamItem* item1, StreamItem* item2, StreamItem** output, void* context),
    void* context);
AsyncStream* async_stream_flatten(AsyncStream* stream, const char* name);

// Stream terminal operations
typedef struct StreamCollector {
    Result_void_ptr (*collect_fn)(StreamItem** items, size_t item_count, void** result, void* context);
    void* context;
    size_t max_items;
} StreamCollector;

Result_void_ptr async_stream_collect(AsyncStream* stream, StreamCollector* collector, void** result);
Result_void_ptr async_stream_reduce(AsyncStream* stream, StreamItem* initial_value,
    StreamReduceFunction reduce_fn, void* context, StreamItem** result);
Result_void_ptr async_stream_for_each(AsyncStream* stream,
    Result_void_ptr (*for_each_fn)(StreamItem* item, void* context), void* context);
Result_void_ptr async_stream_count(AsyncStream* stream, uint64_t* count);
Result_void_ptr async_stream_first(AsyncStream* stream, StreamItem** first_item);
Result_void_ptr async_stream_last(AsyncStream* stream, StreamItem** last_item);

// Stream iteration
typedef struct AsyncStreamIterator {
    AsyncStream* stream;
    AsyncIterator* iterator;
    bool is_started;
} AsyncStreamIterator;

AsyncStreamIterator* async_stream_iterator(AsyncStream* stream);
Result_void_ptr async_stream_iterator_next(AsyncStreamIterator* iter, StreamItem** item);
bool async_stream_iterator_has_next(AsyncStreamIterator* iter);
void async_stream_iterator_destroy(AsyncStreamIterator* iter);

// Utility macros for async stream processing

// Create and start a stream from an array
#define ASYNC_STREAM_FROM_ARRAY(name, array, count, type) \
    async_stream_from_array(name, (void**)array, count, sizeof(type))

// Create a stream from a range of integers
#define ASYNC_STREAM_RANGE(name, start, end) \
    async_stream_from_range(name, start, end, 1)

// For-each async iteration with automatic cleanup
#define ASYNC_STREAM_FOR_EACH(stream, item_var, block) \
    do { \
        AsyncStreamIterator* __iter = async_stream_iterator(stream); \
        if (__iter) { \
            StreamItem* item_var; \
            while (async_stream_iterator_has_next(__iter) && \
                   !async_stream_iterator_next(__iter, &item_var).is_error) { \
                if (item_var && !item_var->is_end_marker && !item_var->error) { \
                    block \
                } \
                if (item_var) stream_item_unref(item_var); \
                if (item_var && item_var->is_end_marker) break; \
            } \
            async_stream_iterator_destroy(__iter); \
        } \
    } while(0)

// Pipeline builder pattern
#define STREAM_PIPELINE_BEGIN(name, source) \
    StreamPipeline* name = stream_pipeline_create(#name, source); \
    if (!name) { \
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create stream pipeline")); \
    }

#define STREAM_PIPELINE_MAP(pipeline, operation_name, map_fn, ctx) \
    do { \
        Result_void_ptr __result = stream_pipeline_map(pipeline, operation_name, map_fn, ctx, sizeof(*ctx)); \
        if (__result.is_error) { \
            stream_pipeline_destroy(pipeline); \
            return __result; \
        } \
    } while(0)

#define STREAM_PIPELINE_FILTER(pipeline, operation_name, filter_fn, ctx) \
    do { \
        Result_void_ptr __result = stream_pipeline_filter(pipeline, operation_name, filter_fn, ctx, sizeof(*ctx)); \
        if (__result.is_error) { \
            stream_pipeline_destroy(pipeline); \
            return __result; \
        } \
    } while(0)

#define STREAM_PIPELINE_BATCH(pipeline, operation_name, batch_sz, timeout, batch_fn, ctx) \
    do { \
        Result_void_ptr __result = stream_pipeline_batch(pipeline, operation_name, batch_sz, timeout, batch_fn, ctx); \
        if (__result.is_error) { \
            stream_pipeline_destroy(pipeline); \
            return __result; \
        } \
    } while(0)

#define STREAM_PIPELINE_PARALLEL(pipeline, operation_name, degree) \
    do { \
        ConcurrentBlockConfig __config = concurrent_block_config_default(); \
        __config.max_concurrent_tasks = degree; \
        Result_void_ptr __result = stream_pipeline_parallel(pipeline, operation_name, degree, __config); \
        if (__result.is_error) { \
            stream_pipeline_destroy(pipeline); \
            return __result; \
        } \
    } while(0)

#define STREAM_PIPELINE_END(pipeline) \
    do { \
        Result_void_ptr __start_result = stream_pipeline_start(pipeline); \
        if (__start_result.is_error) { \
            stream_pipeline_destroy(pipeline); \
            return __start_result; \
        } \
    } while(0)

// Statistics and monitoring
typedef struct AsyncStreamStats {
    uint64_t total_items_processed;
    uint64_t items_per_second;
    uint64_t total_processing_time_ns;
    double average_latency_ms;
    
    size_t active_streams;
    size_t active_pipelines;
    size_t total_buffer_size;
    size_t peak_buffer_size;
    
    uint64_t backpressure_events;
    uint64_t items_dropped;
    uint64_t error_count;
    
    double memory_usage_mb;
    double cpu_usage_percent;
} AsyncStreamStats;

AsyncStreamStats async_stream_get_global_stats(void);
void async_stream_reset_global_stats(void);

// Stream monitoring and debugging
void async_stream_enable_monitoring(AsyncStream* stream, uint64_t report_interval_ms);
void async_stream_disable_monitoring(AsyncStream* stream);
void async_stream_print_stats(AsyncStream* stream);

#endif // GOO_ASYNC_STREAMS_H