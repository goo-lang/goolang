#include "../../include/performance_monitoring.h"
#include "../../include/errors/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// Get high-precision timestamp
uint64_t performance_get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Create performance monitor
PerformanceMonitor* performance_monitor_create(PerformanceConfig config, size_t worker_count) {
    PerformanceMonitor* monitor = calloc(1, sizeof(PerformanceMonitor));
    if (!monitor) return NULL;
    
    monitor->config = config;
    monitor->worker_count = worker_count;
    
    // Initialize global metrics
    atomic_init(&monitor->global_metrics.total_execution_time_ns, 0);
    atomic_init(&monitor->global_metrics.task_creation_time_ns, 0);
    atomic_init(&monitor->global_metrics.task_scheduling_time_ns, 0);
    atomic_init(&monitor->global_metrics.task_completion_time_ns, 0);
    atomic_init(&monitor->global_metrics.tasks_created, 0);
    atomic_init(&monitor->global_metrics.tasks_completed, 0);
    atomic_init(&monitor->global_metrics.tasks_cancelled, 0);
    atomic_init(&monitor->global_metrics.tasks_failed, 0);
    atomic_init(&monitor->global_metrics.steal_attempts, 0);
    atomic_init(&monitor->global_metrics.successful_steals, 0);
    atomic_init(&monitor->global_metrics.failed_steals, 0);
    atomic_init(&monitor->global_metrics.contention_events, 0);
    atomic_init(&monitor->global_metrics.load_imbalance_events, 0);
    atomic_init(&monitor->global_metrics.idle_time_ns, 0);
    atomic_init(&monitor->global_metrics.busy_time_ns, 0);
    atomic_init(&monitor->global_metrics.peak_memory_usage, 0);
    atomic_init(&monitor->global_metrics.memory_allocations, 0);
    atomic_init(&monitor->global_metrics.cache_misses, 0);
    atomic_init(&monitor->global_metrics.synchronization_waits, 0);
    atomic_init(&monitor->global_metrics.longest_task_time_ns, 0);
    atomic_init(&monitor->global_metrics.queue_overflow_events, 0);
    
    // Allocate per-worker data
    monitor->worker_data = calloc(worker_count, sizeof(WorkerPerformanceData));
    if (!monitor->worker_data) {
        free(monitor);
        return NULL;
    }
    
    // Initialize per-worker metrics
    for (size_t i = 0; i < worker_count; i++) {
        WorkerPerformanceData* worker = &monitor->worker_data[i];
        worker->worker_id = i;
        atomic_init(&worker->tasks_executed, 0);
        atomic_init(&worker->execution_time_ns, 0);
        atomic_init(&worker->idle_time_ns, 0);
        atomic_init(&worker->tasks_stolen, 0);
        atomic_init(&worker->tasks_stolen_from, 0);
        atomic_init(&worker->steal_attempts_made, 0);
        atomic_init(&worker->cache_hits, 0);
        atomic_init(&worker->cache_misses, 0);
        atomic_init(&worker->recent_task_index, 0);
        memset(worker->recent_task_times, 0, sizeof(worker->recent_task_times));
    }
    
    // Allocate performance history
    monitor->history_size = config.max_samples > 0 ? config.max_samples : 1000;
    monitor->execution_time_history = calloc(monitor->history_size, sizeof(uint64_t));
    monitor->throughput_history = calloc(monitor->history_size, sizeof(uint32_t));
    atomic_init(&monitor->history_index, 0);
    
    if (!monitor->execution_time_history || !monitor->throughput_history) {
        performance_monitor_destroy(monitor);
        return NULL;
    }
    
    atomic_init(&monitor->monitoring_active, false);
    atomic_init(&monitor->bottleneck_detection_interval, 1000); // 1 second
    atomic_init(&monitor->last_bottleneck_check_ns, 0);
    
    return monitor;
}

// Destroy performance monitor
void performance_monitor_destroy(PerformanceMonitor* monitor) {
    if (!monitor) return;
    
    free(monitor->worker_data);
    free(monitor->execution_time_history);
    free(monitor->throughput_history);
    free(monitor);
}

// Start monitoring
void performance_monitor_start(PerformanceMonitor* monitor) {
    if (!monitor) return;
    
    clock_gettime(CLOCK_MONOTONIC, &monitor->start_time);
    atomic_store(&monitor->monitoring_active, true);
    
    if (monitor->config.on_performance_alert) {
        monitor->config.on_performance_alert("Performance monitoring started");
    }
}

// Stop monitoring
void performance_monitor_stop(PerformanceMonitor* monitor) {
    if (!monitor) return;
    
    atomic_store(&monitor->monitoring_active, false);
    
    if (monitor->config.on_performance_alert) {
        monitor->config.on_performance_alert("Performance monitoring stopped");
    }
}

// Reset metrics
void performance_monitor_reset(PerformanceMonitor* monitor) {
    if (!monitor) return;
    
    // Reset global metrics
    atomic_store(&monitor->global_metrics.total_execution_time_ns, 0);
    atomic_store(&monitor->global_metrics.task_creation_time_ns, 0);
    atomic_store(&monitor->global_metrics.task_scheduling_time_ns, 0);
    atomic_store(&monitor->global_metrics.task_completion_time_ns, 0);
    atomic_store(&monitor->global_metrics.tasks_created, 0);
    atomic_store(&monitor->global_metrics.tasks_completed, 0);
    atomic_store(&monitor->global_metrics.tasks_cancelled, 0);
    atomic_store(&monitor->global_metrics.tasks_failed, 0);
    atomic_store(&monitor->global_metrics.steal_attempts, 0);
    atomic_store(&monitor->global_metrics.successful_steals, 0);
    atomic_store(&monitor->global_metrics.failed_steals, 0);
    atomic_store(&monitor->global_metrics.contention_events, 0);
    atomic_store(&monitor->global_metrics.load_imbalance_events, 0);
    atomic_store(&monitor->global_metrics.idle_time_ns, 0);
    atomic_store(&monitor->global_metrics.busy_time_ns, 0);
    atomic_store(&monitor->global_metrics.peak_memory_usage, 0);
    atomic_store(&monitor->global_metrics.memory_allocations, 0);
    atomic_store(&monitor->global_metrics.cache_misses, 0);
    atomic_store(&monitor->global_metrics.synchronization_waits, 0);
    atomic_store(&monitor->global_metrics.longest_task_time_ns, 0);
    atomic_store(&monitor->global_metrics.queue_overflow_events, 0);
    
    // Reset per-worker metrics
    for (size_t i = 0; i < monitor->worker_count; i++) {
        WorkerPerformanceData* worker = &monitor->worker_data[i];
        atomic_store(&worker->tasks_executed, 0);
        atomic_store(&worker->execution_time_ns, 0);
        atomic_store(&worker->idle_time_ns, 0);
        atomic_store(&worker->tasks_stolen, 0);
        atomic_store(&worker->tasks_stolen_from, 0);
        atomic_store(&worker->steal_attempts_made, 0);
        atomic_store(&worker->cache_hits, 0);
        atomic_store(&worker->cache_misses, 0);
        atomic_store(&worker->recent_task_index, 0);
        memset(worker->recent_task_times, 0, sizeof(worker->recent_task_times));
    }
    
    // Reset history
    memset(monitor->execution_time_history, 0, monitor->history_size * sizeof(uint64_t));
    memset(monitor->throughput_history, 0, monitor->history_size * sizeof(uint32_t));
    atomic_store(&monitor->history_index, 0);
}

// Record task start
void performance_record_task_start(PerformanceMonitor* monitor, uint64_t worker_id, uint64_t task_id) {
    if (!monitor || !atomic_load(&monitor->monitoring_active)) return;
    if (worker_id >= monitor->worker_count) return;
    
    (void)task_id; // Task ID tracking could be added later
    
    atomic_fetch_add(&monitor->global_metrics.tasks_created, 1);
    
    if (monitor->config.enable_timing_metrics) {
        uint64_t now = performance_get_timestamp_ns();
        atomic_fetch_add(&monitor->global_metrics.task_creation_time_ns, now);
    }
}

// Record task completion
void performance_record_task_completion(PerformanceMonitor* monitor, uint64_t worker_id, 
                                       uint64_t task_id, uint64_t execution_time_ns) {
    if (!monitor || !atomic_load(&monitor->monitoring_active)) return;
    if (worker_id >= monitor->worker_count) return;
    
    (void)task_id;
    
    atomic_fetch_add(&monitor->global_metrics.tasks_completed, 1);
    atomic_fetch_add(&monitor->global_metrics.task_completion_time_ns, execution_time_ns);
    
    // Update worker metrics
    WorkerPerformanceData* worker = &monitor->worker_data[worker_id];
    atomic_fetch_add(&worker->tasks_executed, 1);
    atomic_fetch_add(&worker->execution_time_ns, execution_time_ns);
    
    // Track recent task times for bottleneck detection
    if (monitor->config.enable_bottleneck_detection) {
        uint8_t index = atomic_fetch_add(&worker->recent_task_index, 1) % 16;
        worker->recent_task_times[index] = execution_time_ns;
        
        // Update longest task time
        uint64_t longest = atomic_load(&monitor->global_metrics.longest_task_time_ns);
        while (execution_time_ns > longest) {
            if (atomic_compare_exchange_weak(&monitor->global_metrics.longest_task_time_ns, 
                                           &longest, execution_time_ns)) {
                break;
            }
        }
        
        // Check for slow task threshold
        if (execution_time_ns > monitor->config.slow_task_threshold_ns) {
            if (monitor->config.on_bottleneck_detected) {
                char details[256];
                snprintf(details, sizeof(details), 
                        "Slow task detected: worker %llu, execution time %llu ns", 
                        worker_id, execution_time_ns);
                monitor->config.on_bottleneck_detected("slow_task", details);
            }
        }
    }
}

// Record steal attempt
void performance_record_steal_attempt(PerformanceMonitor* monitor, uint64_t worker_id, bool successful) {
    if (!monitor || !atomic_load(&monitor->monitoring_active)) return;
    if (worker_id >= monitor->worker_count) return;
    
    atomic_fetch_add(&monitor->global_metrics.steal_attempts, 1);
    
    if (successful) {
        atomic_fetch_add(&monitor->global_metrics.successful_steals, 1);
        atomic_fetch_add(&monitor->worker_data[worker_id].tasks_stolen, 1);
    } else {
        atomic_fetch_add(&monitor->global_metrics.failed_steals, 1);
    }
    
    atomic_fetch_add(&monitor->worker_data[worker_id].steal_attempts_made, 1);
}

// Record memory allocation
void performance_record_memory_allocation(PerformanceMonitor* monitor, size_t size) {
    if (!monitor || !atomic_load(&monitor->monitoring_active)) return;
    if (!monitor->config.enable_memory_metrics) return;
    
    atomic_fetch_add(&monitor->global_metrics.memory_allocations, 1);
    
    // Update peak memory usage (simplified tracking)
    uint64_t current_peak = atomic_load(&monitor->global_metrics.peak_memory_usage);
    uint64_t new_usage = current_peak + size;
    
    while (new_usage > current_peak) {
        if (atomic_compare_exchange_weak(&monitor->global_metrics.peak_memory_usage, 
                                       &current_peak, new_usage)) {
            break;
        }
        new_usage = current_peak + size;
    }
}

// Record synchronization wait
void performance_record_synchronization_wait(PerformanceMonitor* monitor, uint64_t worker_id, 
                                           uint64_t wait_time_ns) {
    if (!monitor || !atomic_load(&monitor->monitoring_active)) return;
    if (worker_id >= monitor->worker_count) return;
    
    atomic_fetch_add(&monitor->global_metrics.synchronization_waits, 1);
    atomic_fetch_add(&monitor->worker_data[worker_id].idle_time_ns, wait_time_ns);
    
    // Check for contention
    if (monitor->config.enable_bottleneck_detection) {
        if (wait_time_ns > monitor->config.slow_task_threshold_ns / 10) { // 10% of slow task threshold
            atomic_fetch_add(&monitor->global_metrics.contention_events, 1);
        }
    }
}

// Check for bottlenecks
void performance_check_bottlenecks(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->config.enable_bottleneck_detection) return;
    
    uint64_t now = performance_get_timestamp_ns();
    uint64_t last_check = atomic_load(&monitor->last_bottleneck_check_ns);
    uint64_t interval = atomic_load(&monitor->bottleneck_detection_interval) * 1000000; // Convert ms to ns
    
    if (now - last_check < interval) return;
    
    atomic_store(&monitor->last_bottleneck_check_ns, now);
    
    // Check various bottleneck conditions
    performance_detect_load_imbalance(monitor);
    performance_detect_contention(monitor);
    performance_detect_memory_pressure(monitor);
    performance_detect_slow_tasks(monitor);
}

// Detect load imbalance
bool performance_detect_load_imbalance(PerformanceMonitor* monitor) {
    if (!monitor || monitor->worker_count < 2) return false;
    
    // Calculate load distribution across workers
    uint64_t max_tasks = 0, min_tasks = UINT64_MAX;
    uint64_t total_tasks = 0;
    
    for (size_t i = 0; i < monitor->worker_count; i++) {
        uint64_t tasks = atomic_load(&monitor->worker_data[i].tasks_executed);
        total_tasks += tasks;
        if (tasks > max_tasks) max_tasks = tasks;
        if (tasks < min_tasks) min_tasks = tasks;
    }
    
    if (total_tasks == 0) return false;
    
    double imbalance_ratio = (double)(max_tasks - min_tasks) / (total_tasks / monitor->worker_count);
    
    if (imbalance_ratio > monitor->config.load_imbalance_threshold) {
        atomic_fetch_add(&monitor->global_metrics.load_imbalance_events, 1);
        
        if (monitor->config.on_bottleneck_detected) {
            char details[256];
            snprintf(details, sizeof(details), 
                    "Load imbalance detected: ratio %.2f, max tasks %llu, min tasks %llu", 
                    imbalance_ratio, max_tasks, min_tasks);
            monitor->config.on_bottleneck_detected("load_imbalance", details);
        }
        return true;
    }
    
    return false;
}

// Detect contention
bool performance_detect_contention(PerformanceMonitor* monitor) {
    uint32_t contention_events = atomic_load(&monitor->global_metrics.contention_events);
    uint32_t total_attempts = atomic_load(&monitor->global_metrics.steal_attempts);
    
    if (total_attempts == 0) return false;
    
    double contention_rate = (double)contention_events / total_attempts;
    
    if (contention_rate > (monitor->config.high_contention_threshold / 100.0)) {
        if (monitor->config.on_bottleneck_detected) {
            char details[256];
            snprintf(details, sizeof(details), 
                    "High contention detected: %.1f%% contention rate", 
                    contention_rate * 100.0);
            monitor->config.on_bottleneck_detected("contention", details);
        }
        return true;
    }
    
    return false;
}

// Detect memory pressure
bool performance_detect_memory_pressure(PerformanceMonitor* monitor) {
    uint64_t peak_memory = atomic_load(&monitor->global_metrics.peak_memory_usage);
    uint32_t allocations = atomic_load(&monitor->global_metrics.memory_allocations);
    
    // Simple heuristic: high allocation rate might indicate memory pressure
    if (allocations > 10000) { // Threshold for high allocation rate
        if (monitor->config.on_bottleneck_detected) {
            char details[256];
            snprintf(details, sizeof(details), 
                    "Memory pressure detected: %u allocations, %llu bytes peak usage", 
                    allocations, peak_memory);
            monitor->config.on_bottleneck_detected("memory_pressure", details);
        }
        return true;
    }
    
    return false;
}

// Detect slow tasks
bool performance_detect_slow_tasks(PerformanceMonitor* monitor) {
    uint64_t longest_task = atomic_load(&monitor->global_metrics.longest_task_time_ns);
    
    if (longest_task > monitor->config.slow_task_threshold_ns * 2) { // 2x threshold
        if (monitor->config.on_bottleneck_detected) {
            char details[256];
            snprintf(details, sizeof(details), 
                    "Extremely slow task detected: %llu ns (threshold: %llu ns)", 
                    longest_task, monitor->config.slow_task_threshold_ns);
            monitor->config.on_bottleneck_detected("slow_task_extreme", details);
        }
        return true;
    }
    
    return false;
}

// Calculate efficiency
double performance_calculate_efficiency(PerformanceMonitor* monitor) {
    if (!monitor) return 0.0;
    
    uint64_t total_busy_time = 0;
    uint64_t total_idle_time = 0;
    
    for (size_t i = 0; i < monitor->worker_count; i++) {
        total_busy_time += atomic_load(&monitor->worker_data[i].execution_time_ns);
        total_idle_time += atomic_load(&monitor->worker_data[i].idle_time_ns);
    }
    
    uint64_t total_time = total_busy_time + total_idle_time;
    return total_time > 0 ? (double)total_busy_time / total_time : 0.0;
}

// Calculate throughput
double performance_calculate_throughput(PerformanceMonitor* monitor) {
    if (!monitor) return 0.0;
    
    uint32_t completed_tasks = atomic_load(&monitor->global_metrics.tasks_completed);
    uint64_t total_time_ns = atomic_load(&monitor->global_metrics.total_execution_time_ns);
    
    if (total_time_ns == 0) return 0.0;
    
    // Tasks per second
    return (double)completed_tasks / (total_time_ns / 1e9);
}

// Print summary
void performance_print_summary(PerformanceMonitor* monitor) {
    if (!monitor) return;
    
    printf("\n=== Performance Monitoring Summary ===\n");
    printf("Tasks: Created=%u, Completed=%u, Failed=%u\n",
           atomic_load(&monitor->global_metrics.tasks_created),
           atomic_load(&monitor->global_metrics.tasks_completed),
           atomic_load(&monitor->global_metrics.tasks_failed));
    
    printf("Work-Stealing: Attempts=%u, Successful=%u, Failed=%u\n",
           atomic_load(&monitor->global_metrics.steal_attempts),
           atomic_load(&monitor->global_metrics.successful_steals),
           atomic_load(&monitor->global_metrics.failed_steals));
    
    printf("Efficiency: %.1f%%, Throughput: %.1f tasks/sec\n",
           performance_calculate_efficiency(monitor) * 100.0,
           performance_calculate_throughput(monitor));
    
    printf("Bottlenecks: Load imbalance=%u, Contention=%u, Sync waits=%u\n",
           atomic_load(&monitor->global_metrics.load_imbalance_events),
           atomic_load(&monitor->global_metrics.contention_events),
           atomic_load(&monitor->global_metrics.synchronization_waits));
}

// Configuration presets
PerformanceConfig performance_config_default(void) {
    return (PerformanceConfig) {
        .enable_timing_metrics = true,
        .enable_task_metrics = true,
        .enable_memory_metrics = true,
        .enable_bottleneck_detection = true,
        .enable_detailed_profiling = false,
        .sampling_interval_ms = 100,
        .max_samples = 1000,
        .slow_task_threshold_ns = 10000000, // 10ms
        .high_contention_threshold = 20, // 20%
        .load_imbalance_threshold = 0.3, // 30%
        .on_bottleneck_detected = NULL,
        .on_performance_alert = NULL
    };
}

PerformanceConfig performance_config_detailed(void) {
    PerformanceConfig config = performance_config_default();
    config.enable_detailed_profiling = true;
    config.sampling_interval_ms = 50; // More frequent sampling
    config.max_samples = 5000;
    config.slow_task_threshold_ns = 5000000; // 5ms - more sensitive
    config.high_contention_threshold = 15; // 15%
    config.load_imbalance_threshold = 0.2; // 20%
    return config;
}

PerformanceConfig performance_config_minimal(void) {
    return (PerformanceConfig) {
        .enable_timing_metrics = false,
        .enable_task_metrics = true,
        .enable_memory_metrics = false,
        .enable_bottleneck_detection = false,
        .enable_detailed_profiling = false,
        .sampling_interval_ms = 1000, // Less frequent
        .max_samples = 100,
        .slow_task_threshold_ns = 50000000, // 50ms
        .high_contention_threshold = 50, // 50%
        .load_imbalance_threshold = 0.5, // 50%
        .on_bottleneck_detected = NULL,
        .on_performance_alert = NULL
    };
}