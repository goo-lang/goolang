#ifndef RUNTIME_H
#define RUNTIME_H

#define _XOPEN_SOURCE 700  // For ucontext functions

#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct goo_error goo_error_t;
typedef struct goo_string goo_string_t;
typedef struct goo_slice goo_slice_t;
typedef struct goo_goroutine goo_goroutine_t;
typedef struct goo_channel goo_channel_t;
typedef struct goo_scheduler goo_scheduler_t;
typedef struct goo_mutex goo_mutex_t;
typedef struct goo_cond goo_cond_t;
typedef struct goo_waitgroup goo_waitgroup_t;

// Error structure
struct goo_error {
    const char* message;
    int code;
    struct goo_error* cause;  // Optional chained error
};

// String structure (compatible with code generator)
struct goo_string {
    char* data;
    size_t length;
};

// Generic slice structure (compatible with code generator)
struct goo_slice {
    void* data;
    size_t length;
    size_t capacity;
};

// Program initialization and cleanup
void goo_init(int argc, char** argv);
void goo_exit(int code);

// Memory management
void* goo_alloc(size_t size);
void* goo_realloc(void* ptr, size_t size);
void goo_free(void* ptr);

// Error handling
void goo_panic(const char* message) __attribute__((noreturn));
goo_error_t* goo_new_error(const char* message);
goo_error_t* goo_new_error_with_code(const char* message, int code);
void goo_error_free(goo_error_t* error);

// I/O functions
void goo_print(const char* message);
void goo_println(const char* message);
void goo_print_string(goo_string_t str);
void goo_println_string(goo_string_t str);
void goo_println_int(int64_t value);
void goo_println_bool(int value);
void goo_println_float(double value);
void goo_print_int(int64_t value);
void goo_print_bool(int value);
void goo_print_float(double value);

// String operations
goo_string_t goo_string_new(const char* data);
goo_string_t goo_string_new_with_length(const char* data, size_t length);
void goo_string_free(goo_string_t str);
goo_string_t goo_string_concat(goo_string_t a, goo_string_t b);

// Stdlib package backings (used by codegen to lower fmt.*, os.*,
// strings.*, math.* calls into runtime symbols)
int goo_strings_contains(const char* haystack, const char* needle);
double goo_math_sqrt(double x);

// Minimum-viable map for `map[string]int{...}` literals + indexing
// (M8-map-literal). Linear-scan linked list — performance is not the
// point; correctness for the probe is. Generic maps and richer key
// types are future work.
struct GooMapEntrySI;
typedef struct GooMapSI {
    struct GooMapEntrySI* head;
} GooMapSI;
GooMapSI* goo_map_new_si(void);
void goo_map_set_si(GooMapSI* m, const char* k, int v);
int goo_map_get_si(GooMapSI* m, const char* k);

// Slice operations
goo_slice_t goo_slice_new(size_t element_size, size_t capacity);
void goo_slice_free(goo_slice_t slice);
void* goo_slice_get(goo_slice_t slice, size_t index, size_t element_size);
int goo_slice_append(goo_slice_t* slice, void* element, size_t element_size);

// Runtime type information (for future use)
typedef enum {
    GOO_TYPE_VOID,
    GOO_TYPE_BOOL,
    GOO_TYPE_INT8,
    GOO_TYPE_INT16,
    GOO_TYPE_INT32,
    GOO_TYPE_INT64,
    GOO_TYPE_UINT8,
    GOO_TYPE_UINT16,
    GOO_TYPE_UINT32,
    GOO_TYPE_UINT64,
    GOO_TYPE_FLOAT32,
    GOO_TYPE_FLOAT64,
    GOO_TYPE_STRING,
    GOO_TYPE_POINTER,
    GOO_TYPE_ARRAY,
    GOO_TYPE_SLICE,
    GOO_TYPE_STRUCT,
    GOO_TYPE_ERROR_UNION,
    GOO_TYPE_NULLABLE
} goo_type_kind_t;

typedef struct goo_type_info {
    goo_type_kind_t kind;
    size_t size;
    size_t align;
    const char* name;
} goo_type_info_t;

// Bounds checking (for safe mode)
void goo_bounds_check(size_t index, size_t length, const char* file, int line);

// Null checking (for nullable types)
void goo_null_check(void* ptr, const char* file, int line);

// Debug macros
#ifdef GOO_DEBUG
#define GOO_BOUNDS_CHECK(index, length) goo_bounds_check(index, length, __FILE__, __LINE__)
#define GOO_NULL_CHECK(ptr) goo_null_check(ptr, __FILE__, __LINE__)
#else
#define GOO_BOUNDS_CHECK(index, length) ((void)0)
#define GOO_NULL_CHECK(ptr) ((void)0)
#endif

// Concurrency support

// Goroutine function type
typedef void (*goo_goroutine_func_t)(void* arg);

// Goroutine creation and management
void goo_scheduler_init(int num_threads);
void goo_scheduler_shutdown(void);
goo_goroutine_t* goo_go(goo_goroutine_func_t func, void* arg);
void goo_yield(void);
void goo_goroutine_exit(void);

// Channel operations
goo_channel_t* goo_make_chan(size_t elem_size, size_t buffer_size);
void goo_chan_close(goo_channel_t* ch);
void goo_chan_free(goo_channel_t* ch);

// Channel send/receive
int goo_chan_send(goo_channel_t* ch, void* data);
int goo_chan_recv(goo_channel_t* ch, void* data);
int goo_chan_send_timeout(goo_channel_t* ch, void* data, uint64_t timeout_ns);
int goo_chan_recv_timeout(goo_channel_t* ch, void* data, uint64_t timeout_ns);

// Non-blocking operations
int goo_chan_try_send(goo_channel_t* ch, void* data);
int goo_chan_try_recv(goo_channel_t* ch, void* data);

// Channel state queries
int goo_chan_is_closed(goo_channel_t* ch);
size_t goo_chan_len(goo_channel_t* ch);
size_t goo_chan_cap(goo_channel_t* ch);

// Select operation support
typedef struct goo_select_case {
    goo_channel_t* channel;
    void* data;
    int is_send;  // 1 for send, 0 for receive
    int ready;    // Set by select operation
} goo_select_case_t;

int goo_select(goo_select_case_t* cases, size_t num_cases, int64_t timeout_ns);

// Deadlock detection
typedef struct goo_deadlock_detector {
    int enabled;
    uint64_t last_check_time;
    uint64_t check_interval_ns;
    int detected_deadlock;
} goo_deadlock_detector_t;

// Deadlock detection functions
int goo_deadlock_init(void);
void goo_deadlock_shutdown(void);
int goo_deadlock_check(void);
void goo_deadlock_enable(int enable);
int goo_deadlock_detected(void);

// Channel pattern operations
int goo_chan_subscribe(goo_channel_t* publisher, goo_channel_t* subscriber);
int goo_chan_unsubscribe(goo_channel_t* publisher, goo_channel_t* subscriber);
int goo_chan_pair_req_rep(goo_channel_t* req_chan, goo_channel_t* rep_chan);
int goo_chan_add_worker(goo_channel_t* push_chan, goo_channel_t* pull_chan);

// Goroutine states
typedef enum {
    GOO_GOROUTINE_READY,
    GOO_GOROUTINE_RUNNING,
    GOO_GOROUTINE_BLOCKED,
    GOO_GOROUTINE_DONE
} goo_goroutine_state_t;

// Channel patterns (from AST)
typedef enum {
    GOO_CHANNEL_BASIC,
    GOO_CHANNEL_PUB,
    GOO_CHANNEL_SUB,
    GOO_CHANNEL_REQ,
    GOO_CHANNEL_REP,
    GOO_CHANNEL_PUSH,
    GOO_CHANNEL_PULL
} goo_channel_pattern_t;

// Pattern channel function (defined after enum)
goo_channel_t* goo_make_pattern_chan(goo_channel_pattern_t pattern, size_t elem_size, const char* endpoint);

// Platform-specific includes for structure definitions
#if defined(__APPLE__) && defined(__MACH__)
    #define GOO_PLATFORM_MACOS 1
    #define GOO_PLATFORM_UNIX 1
#elif defined(__linux__)
    #define GOO_PLATFORM_LINUX 1
    #define GOO_PLATFORM_UNIX 1
#elif defined(_WIN32) || defined(_WIN64)
    #define GOO_PLATFORM_WINDOWS 1
#else
    #define GOO_PLATFORM_UNKNOWN 1
    #define GOO_PLATFORM_UNIX 1  // Assume Unix-like
#endif

#ifdef GOO_PLATFORM_UNIX
    #include <pthread.h>
    #include <ucontext.h>
#endif

// Mutex structure
struct goo_mutex {
#ifdef GOO_PLATFORM_UNIX
    pthread_mutex_t mutex;
#endif
    int locked;
};

// Condition variable structure
struct goo_cond {
#ifdef GOO_PLATFORM_UNIX
    pthread_cond_t cond;
#endif
    int dummy;  // Prevent empty struct
};

// Wait group structure
struct goo_waitgroup {
    goo_mutex_t* mutex;
    goo_cond_t* cond;
    int counter;
};

// Goroutine structure (simplified for header)
// Note: Uses ucontext on Unix systems (deprecated on macOS but functional)
struct goo_goroutine {
    goo_goroutine_state_t state;
    void (*function)(void*);
    void* arg;
    void* stack;
    size_t stack_size;
    
#ifdef GOO_PLATFORM_UNIX
    ucontext_t context;
#endif
    
    struct goo_goroutine* next;
    uint64_t id;
    uint64_t creation_time;
    
    // Deadlock detection - what this goroutine is waiting for
    goo_channel_t* waiting_on_channel;
    int waiting_for_send;  // 1 for send, 0 for receive
};

// Subscriber node for pub/sub pattern
struct goo_subscriber {
    goo_channel_t* channel;
    struct goo_subscriber* next;
    int active;
};

// Channel structure
struct goo_channel {
    void* buffer;
    size_t elem_size;
    size_t capacity;
    size_t length;
    size_t head;
    size_t tail;
    
    goo_channel_pattern_t pattern;
    char* endpoint;
    
    goo_mutex_t* mutex;
    goo_cond_t* not_empty;
    goo_cond_t* not_full;
    
    int closed;
    uint64_t id;
    
    struct goo_goroutine* send_waiters;
    struct goo_goroutine* recv_waiters;
    
    // Pattern-specific data
    union {
        struct {
            struct goo_subscriber* subscribers;
            size_t subscriber_count;
        } pub_data;
        
        struct {
            goo_channel_t* publisher;
            struct goo_subscriber* sub_node;
        } sub_data;
        
        struct {
            goo_channel_t* paired_channel;
            uint64_t request_id_counter;
        } req_rep_data;
        
        struct {
            goo_channel_t** workers;
            size_t worker_count;
            size_t next_worker_index;
        } push_pull_data;
    } pattern_data;
};

// Runtime statistics structure
typedef struct {
    size_t num_goroutines;
    size_t num_channels;
    size_t scheduler_cycles;
    size_t context_switches;
} goo_runtime_stats_t;

// Scheduler structure (simplified for header)
struct goo_scheduler {
    goo_goroutine_t* ready_queue;
    goo_goroutine_t* current_goroutine;
    
#ifdef GOO_PLATFORM_UNIX
    pthread_t* os_threads;
    ucontext_t main_context;
#endif
    
    int num_threads;
    int running;
    
    goo_mutex_t* scheduler_mutex;
    uint64_t next_goroutine_id;
    uint64_t next_channel_id;
    
    goo_runtime_stats_t stats;
    goo_deadlock_detector_t deadlock_detector;
};

// Synchronization primitives (now just function declarations)
goo_mutex_t* goo_mutex_new(void);
void goo_mutex_free(goo_mutex_t* mutex);
void goo_mutex_lock(goo_mutex_t* mutex);
void goo_mutex_unlock(goo_mutex_t* mutex);
int goo_mutex_try_lock(goo_mutex_t* mutex);

goo_waitgroup_t* goo_waitgroup_new(void);
void goo_waitgroup_free(goo_waitgroup_t* wg);
void goo_waitgroup_add(goo_waitgroup_t* wg, int delta);
void goo_waitgroup_done(goo_waitgroup_t* wg);
void goo_waitgroup_wait(goo_waitgroup_t* wg);

goo_runtime_stats_t goo_get_runtime_stats(void);

#endif // RUNTIME_H