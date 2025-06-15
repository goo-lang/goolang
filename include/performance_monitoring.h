#ifndef PERFORMANCE_MONITORING_H
#define PERFORMANCE_MONITORING_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>

// Performance metrics collection
typedef struct PerformanceMetrics {
    // Timing metrics
    atomic_uint_fast64_t total_execution_time_ns;
    atomic_uint_fast64_t task_creation_time_ns;
    atomic_uint_fast64_t task_scheduling_time_ns;
    atomic_uint_fast64_t task_completion_time_ns;
    
    // Task metrics
    atomic_uint_fast32_t tasks_created;
    atomic_uint_fast32_t tasks_completed;
    atomic_uint_fast32_t tasks_cancelled;
    atomic_uint_fast32_t tasks_failed;
    
    // Work-stealing metrics
    atomic_uint_fast32_t steal_attempts;
    atomic_uint_fast32_t successful_steals;
    atomic_uint_fast32_t failed_steals;
    atomic_uint_fast32_t contention_events;
    
    // Load balancing metrics
    atomic_uint_fast32_t load_imbalance_events;
    atomic_uint_fast64_t idle_time_ns;
    atomic_uint_fast64_t busy_time_ns;
    
    // Memory metrics
    atomic_uint_fast64_t peak_memory_usage;
    atomic_uint_fast32_t memory_allocations;
    atomic_uint_fast32_t cache_misses;
    
    // Bottleneck detection
    atomic_uint_fast32_t synchronization_waits;
    atomic_uint_fast64_t longest_task_time_ns;
    atomic_uint_fast32_t queue_overflow_events;
} PerformanceMetrics;

// Performance monitoring configuration
typedef struct PerformanceConfig {
    bool enable_timing_metrics;
    bool enable_task_metrics;
    bool enable_memory_metrics;
    bool enable_bottleneck_detection;
    bool enable_detailed_profiling;
    
    // Sampling configuration
    uint32_t sampling_interval_ms;
    uint32_t max_samples;
    
    // Thresholds for bottleneck detection
    uint64_t slow_task_threshold_ns;
    uint32_t high_contention_threshold;
    double load_imbalance_threshold;
    
    // Callbacks
    void (*on_bottleneck_detected)(const char* bottleneck_type, const char* details);
    void (*on_performance_alert)(const char* alert_message);
} PerformanceConfig;

// Per-worker performance data
typedef struct WorkerPerformanceData {
    uint64_t worker_id;
    
    // Task execution metrics
    atomic_uint_fast32_t tasks_executed;
    atomic_uint_fast64_t execution_time_ns;
    atomic_uint_fast64_t idle_time_ns;
    
    // Work-stealing metrics
    atomic_uint_fast32_t tasks_stolen;
    atomic_uint_fast32_t tasks_stolen_from;
    atomic_uint_fast32_t steal_attempts_made;
    
    // Cache performance (estimated)
    atomic_uint_fast32_t cache_hits;
    atomic_uint_fast32_t cache_misses;
    
    // Recent task times for bottleneck detection
    uint64_t recent_task_times[16];
    atomic_uint_fast8_t recent_task_index;
} WorkerPerformanceData;

// Performance monitoring context
typedef struct PerformanceMonitor {
    PerformanceConfig config;
    PerformanceMetrics global_metrics;
    
    // Per-worker data
    WorkerPerformanceData* worker_data;
    size_t worker_count;
    
    // Sampling data
    struct timespec start_time;
    atomic_bool monitoring_active;
    
    // Bottleneck detection state
    atomic_uint_fast32_t bottleneck_detection_interval;
    atomic_uint_fast64_t last_bottleneck_check_ns;
    
    // Performance history for trend analysis
    uint64_t* execution_time_history;
    uint32_t* throughput_history;
    size_t history_size;
    atomic_size_t history_index;
} PerformanceMonitor;

// Performance monitor lifecycle
PerformanceMonitor* performance_monitor_create(PerformanceConfig config, size_t worker_count);
void performance_monitor_destroy(PerformanceMonitor* monitor);
void performance_monitor_start(PerformanceMonitor* monitor);
void performance_monitor_stop(PerformanceMonitor* monitor);
void performance_monitor_reset(PerformanceMonitor* monitor);

// Metric collection functions
void performance_record_task_start(PerformanceMonitor* monitor, uint64_t worker_id, uint64_t task_id);
void performance_record_task_completion(PerformanceMonitor* monitor, uint64_t worker_id, 
                                       uint64_t task_id, uint64_t execution_time_ns);
void performance_record_steal_attempt(PerformanceMonitor* monitor, uint64_t worker_id, bool successful);
void performance_record_memory_allocation(PerformanceMonitor* monitor, size_t size);
void performance_record_synchronization_wait(PerformanceMonitor* monitor, uint64_t worker_id, 
                                           uint64_t wait_time_ns);

// Bottleneck detection
void performance_check_bottlenecks(PerformanceMonitor* monitor);
bool performance_detect_load_imbalance(PerformanceMonitor* monitor);
bool performance_detect_contention(PerformanceMonitor* monitor);
bool performance_detect_memory_pressure(PerformanceMonitor* monitor);
bool performance_detect_slow_tasks(PerformanceMonitor* monitor);

// Analysis and reporting
void performance_generate_report(PerformanceMonitor* monitor, char* buffer, size_t buffer_size);
void performance_print_summary(PerformanceMonitor* monitor);
void performance_print_detailed_report(PerformanceMonitor* monitor);
double performance_calculate_efficiency(PerformanceMonitor* monitor);
double performance_calculate_throughput(PerformanceMonitor* monitor);

// Configuration presets
PerformanceConfig performance_config_default(void);
PerformanceConfig performance_config_detailed(void);
PerformanceConfig performance_config_minimal(void);

// Utility functions
uint64_t performance_get_timestamp_ns(void);
double performance_calculate_cpu_utilization(PerformanceMonitor* monitor, uint64_t worker_id);
void performance_update_worker_metrics(PerformanceMonitor* monitor, uint64_t worker_id);

// Integration macros for easy instrumentation
#define PERF_RECORD_TASK_START(monitor, worker_id, task_id) \
    do { if (monitor) performance_record_task_start(monitor, worker_id, task_id); } while(0)

#define PERF_RECORD_TASK_COMPLETION(monitor, worker_id, task_id, time_ns) \
    do { if (monitor) performance_record_task_completion(monitor, worker_id, task_id, time_ns); } while(0)

#define PERF_RECORD_STEAL_ATTEMPT(monitor, worker_id, success) \
    do { if (monitor) performance_record_steal_attempt(monitor, worker_id, success); } while(0)

#define PERF_CHECK_BOTTLENECKS(monitor) \
    do { if (monitor && monitor->config.enable_bottleneck_detection) performance_check_bottlenecks(monitor); } while(0)

#endif // PERFORMANCE_MONITORING_H