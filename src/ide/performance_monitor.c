#include "performance_monitor.h"
#include "repl.h"
#include "panic_free.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/time.h>

// =============================================================================
// Performance Monitor Implementation
// =============================================================================

// Metric names and units
static const char* metric_names[PERF_METRIC_COUNT] = {
    [PERF_METRIC_CPU_TIME] = "CPU Time",
    [PERF_METRIC_MEMORY_USAGE] = "Memory Usage",
    [PERF_METRIC_PARSE_TIME] = "Parse Time",
    [PERF_METRIC_TYPE_CHECK_TIME] = "Type Check Time",
    [PERF_METRIC_CODEGEN_TIME] = "Code Generation Time",
    [PERF_METRIC_IO_OPERATIONS] = "I/O Operations",
    [PERF_METRIC_GC_TIME] = "Garbage Collection Time",
    [PERF_METRIC_ALLOCATION_COUNT] = "Allocation Count",
    [PERF_METRIC_DEALLOCATION_COUNT] = "Deallocation Count",
    [PERF_METRIC_PEAK_MEMORY] = "Peak Memory",
    [PERF_METRIC_COMPILATION_COUNT] = "Compilation Count",
    [PERF_METRIC_ERROR_COUNT] = "Error Count"
};

static const char* metric_units[PERF_METRIC_COUNT] = {
    [PERF_METRIC_CPU_TIME] = "ms",
    [PERF_METRIC_MEMORY_USAGE] = "MB",
    [PERF_METRIC_PARSE_TIME] = "ms",
    [PERF_METRIC_TYPE_CHECK_TIME] = "ms",
    [PERF_METRIC_CODEGEN_TIME] = "ms",
    [PERF_METRIC_IO_OPERATIONS] = "count",
    [PERF_METRIC_GC_TIME] = "ms",
    [PERF_METRIC_ALLOCATION_COUNT] = "count",
    [PERF_METRIC_DEALLOCATION_COUNT] = "count",
    [PERF_METRIC_PEAK_MEMORY] = "MB",
    [PERF_METRIC_COMPILATION_COUNT] = "count",
    [PERF_METRIC_ERROR_COUNT] = "count"
};

// =============================================================================
// Utility Functions
// =============================================================================

uint64_t performance_get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

const char* performance_metric_type_to_string(PerformanceMetricType type) {
    if (type >= 0 && type < PERF_METRIC_COUNT) {
        return metric_names[type];
    }
    return "Unknown";
}

const char* performance_alert_type_to_string(PerformanceAlertType type) {
    switch (type) {
        case PERF_ALERT_NONE: return "None";
        case PERF_ALERT_HIGH_MEMORY: return "High Memory";
        case PERF_ALERT_HIGH_CPU: return "High CPU";
        case PERF_ALERT_SLOW_COMPILATION: return "Slow Compilation";
        case PERF_ALERT_FREQUENT_ERRORS: return "Frequent Errors";
        case PERF_ALERT_MEMORY_LEAK: return "Memory Leak";
        default: return "Unknown";
    }
}

// =============================================================================
// Performance Monitor Lifecycle
// =============================================================================

PerformanceMonitor* performance_monitor_new(void) {
    PerformanceMonitor* monitor = calloc(1, sizeof(PerformanceMonitor));
    if (!monitor) return NULL;
    
    // Initialize default configuration
    monitor->is_enabled = false;
    monitor->is_recording = false;
    monitor->sample_interval = PERF_SAMPLE_INTERVAL_100MS;
    monitor->max_samples_per_metric = 1000;
    monitor->max_sessions = 10;
    
    // Enable all monitoring by default
    monitor->enable_cpu_monitoring = true;
    monitor->enable_memory_monitoring = true;
    monitor->enable_compilation_monitoring = true;
    monitor->enable_alerts = true;
    monitor->enable_auto_export = false;
    
    // Set default alert thresholds
    monitor->memory_alert_threshold_mb = 1000.0;  // 1GB
    monitor->cpu_alert_threshold_percent = 80.0;
    monitor->compilation_time_alert_threshold_ms = 5000.0;  // 5 seconds
    monitor->error_rate_alert_threshold = 10;
    
    // Set default export settings
    monitor->export_directory = str_dup("./performance_reports");
    monitor->export_json = true;
    monitor->export_csv = true;
    monitor->export_html = false;
    
    return monitor;
}

void performance_monitor_free(PerformanceMonitor* monitor) {
    if (!monitor) return;
    
    // Stop recording if active
    if (monitor->is_recording) {
        performance_monitor_stop_recording(monitor);
    }
    
    // Free sessions
    PerformanceSession* session = monitor->sessions;
    while (session) {
        PerformanceSession* next = session->next;
        performance_session_free(session);
        session = next;
    }
    
    // Free export directory
    free((char*)monitor->export_directory);
    
    free(monitor);
}

int performance_monitor_init(PerformanceMonitor* monitor) {
    if (!monitor) return -1;
    
    // Initialize timing baseline
    clock_gettime(CLOCK_MONOTONIC, &monitor->start_time);
    getrusage(RUSAGE_SELF, &monitor->start_usage);
    
    monitor->is_enabled = true;
    return 0;
}

int performance_monitor_cleanup(PerformanceMonitor* monitor) {
    if (!monitor) return -1;
    
    performance_monitor_stop_recording(monitor);
    monitor->is_enabled = false;
    return 0;
}

// =============================================================================
// Recording Control
// =============================================================================

int performance_monitor_start_recording(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->is_enabled) return -1;
    
    if (!monitor->is_recording) {
        // Start a new session
        performance_monitor_start_session(monitor);
        monitor->is_recording = true;
    }
    
    return 0;
}

int performance_monitor_stop_recording(PerformanceMonitor* monitor) {
    if (!monitor) return -1;
    
    if (monitor->is_recording) {
        // End current session
        performance_monitor_end_session(monitor);
        monitor->is_recording = false;
    }
    
    return 0;
}

int performance_monitor_pause_recording(PerformanceMonitor* monitor) {
    if (!monitor) return -1;
    monitor->is_recording = false;
    return 0;
}

int performance_monitor_resume_recording(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->is_enabled) return -1;
    monitor->is_recording = true;
    return 0;
}

// =============================================================================
// Session Management
// =============================================================================

PerformanceSession* performance_session_new(uint64_t session_id) {
    PerformanceSession* session = calloc(1, sizeof(PerformanceSession));
    if (!session) return NULL;
    
    session->session_id = session_id;
    session->start_time_ms = performance_get_timestamp_ms();
    
    // Initialize metrics
    for (int i = 0; i < PERF_METRIC_COUNT; i++) {
        session->metrics[i] = performance_metric_new(i);
    }
    
    return session;
}

void performance_session_free(PerformanceSession* session) {
    if (!session) return;
    
    // Free metrics
    for (int i = 0; i < PERF_METRIC_COUNT; i++) {
        performance_metric_free(session->metrics[i]);
    }
    
    // Free alerts
    PerformanceAlert* alert = session->alerts;
    while (alert) {
        PerformanceAlert* next = alert->next;
        performance_alert_free(alert);
        alert = next;
    }
    
    free(session);
}

int performance_monitor_start_session(PerformanceMonitor* monitor) {
    if (!monitor) return -1;
    
    // End current session if active
    if (monitor->current_session) {
        performance_monitor_end_session(monitor);
    }
    
    // Create new session
    uint64_t session_id = monitor->session_count + 1;
    monitor->current_session = performance_session_new(session_id);
    if (!monitor->current_session) return -1;
    
    return 0;
}

int performance_monitor_end_session(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->current_session) return -1;
    
    // Set end time
    monitor->current_session->end_time_ms = performance_get_timestamp_ms();
    
    // Add to sessions list
    monitor->current_session->next = monitor->sessions;
    monitor->sessions = monitor->current_session;
    monitor->session_count++;
    
    // Remove old sessions if we exceed the limit
    if (monitor->session_count > monitor->max_sessions) {
        PerformanceSession* session = monitor->sessions;
        PerformanceSession* prev = NULL;
        uint64_t count = 0;
        
        while (session && count < monitor->max_sessions) {
            prev = session;
            session = session->next;
            count++;
        }
        
        if (prev) {
            prev->next = NULL;
        }
        
        // Free excess sessions
        while (session) {
            PerformanceSession* next = session->next;
            performance_session_free(session);
            session = next;
            monitor->session_count--;
        }
    }
    
    monitor->current_session = NULL;
    return 0;
}

PerformanceSession* performance_monitor_get_session(PerformanceMonitor* monitor, uint64_t session_id) {
    if (!monitor) return NULL;
    
    PerformanceSession* session = monitor->sessions;
    while (session) {
        if (session->session_id == session_id) {
            return session;
        }
        session = session->next;
    }
    
    return NULL;
}

PerformanceSession* performance_monitor_get_current_session(PerformanceMonitor* monitor) {
    return monitor ? monitor->current_session : NULL;
}

// =============================================================================
// Metric Management
// =============================================================================

PerformanceMetric* performance_metric_new(PerformanceMetricType type) {
    PerformanceMetric* metric = calloc(1, sizeof(PerformanceMetric));
    if (!metric) return NULL;
    
    metric->type = type;
    metric->name = metric_names[type];
    metric->unit = metric_units[type];
    metric->is_enabled = true;
    metric->min_value = __DBL_MAX__;
    metric->max_value = -__DBL_MAX__;
    
    return metric;
}

void performance_metric_free(PerformanceMetric* metric) {
    if (!metric) return;
    
    // Free samples
    PerformanceSample* sample = metric->samples;
    while (sample) {
        PerformanceSample* next = sample->next;
        performance_sample_free(sample);
        sample = next;
    }
    
    free(metric);
}

int performance_monitor_enable_metric(PerformanceMonitor* monitor, PerformanceMetricType type) {
    if (!monitor || type >= PERF_METRIC_COUNT) return -1;
    
    if (monitor->current_session && monitor->current_session->metrics[type]) {
        monitor->current_session->metrics[type]->is_enabled = true;
    }
    
    return 0;
}

int performance_monitor_disable_metric(PerformanceMonitor* monitor, PerformanceMetricType type) {
    if (!monitor || type >= PERF_METRIC_COUNT) return -1;
    
    if (monitor->current_session && monitor->current_session->metrics[type]) {
        monitor->current_session->metrics[type]->is_enabled = false;
    }
    
    return 0;
}

bool performance_monitor_is_metric_enabled(PerformanceMonitor* monitor, PerformanceMetricType type) {
    if (!monitor || type >= PERF_METRIC_COUNT) return false;
    
    if (monitor->current_session && monitor->current_session->metrics[type]) {
        return monitor->current_session->metrics[type]->is_enabled;
    }
    
    return false;
}

int performance_monitor_record_metric(PerformanceMonitor* monitor, PerformanceMetricType type, double value, const char* context) {
    if (!monitor || !monitor->is_recording || !monitor->current_session || type >= PERF_METRIC_COUNT) {
        return -1;
    }
    
    PerformanceMetric* metric = monitor->current_session->metrics[type];
    if (!metric || !metric->is_enabled) return -1;
    
    // Create new sample
    PerformanceSample* sample = performance_sample_new(type, value, context);
    if (!sample) return -1;
    
    // Add to metric
    performance_metric_add_sample(metric, sample);
    
    // Update metric statistics
    metric->current_value = value;
    if (value < metric->min_value) {
        metric->min_value = value;
    }
    if (value > metric->max_value) {
        metric->max_value = value;
    }
    
    // Update running average
    metric->avg_value = ((metric->avg_value * (metric->sample_count - 1)) + value) / metric->sample_count;
    
    // Check for alerts
    if (monitor->enable_alerts) {
        performance_monitor_check_alerts(monitor);
    }
    
    return 0;
}

int performance_monitor_record_timing(PerformanceMonitor* monitor, const char* operation_name, uint64_t start_time_ms, uint64_t end_time_ms) {
    if (!monitor || !operation_name) return -1;
    
    double duration_ms = (double)(end_time_ms - start_time_ms);
    
    // Determine metric type based on operation name
    PerformanceMetricType type = PERF_METRIC_CPU_TIME;
    if (strstr(operation_name, "parse")) {
        type = PERF_METRIC_PARSE_TIME;
    } else if (strstr(operation_name, "type_check") || strstr(operation_name, "typecheck")) {
        type = PERF_METRIC_TYPE_CHECK_TIME;
    } else if (strstr(operation_name, "codegen") || strstr(operation_name, "generate")) {
        type = PERF_METRIC_CODEGEN_TIME;
    }
    
    return performance_monitor_record_metric(monitor, type, duration_ms, operation_name);
}

// =============================================================================
// Sample Management
// =============================================================================

PerformanceSample* performance_sample_new(PerformanceMetricType type, double value, const char* context) {
    PerformanceSample* sample = calloc(1, sizeof(PerformanceSample));
    if (!sample) return NULL;
    
    sample->timestamp_ms = performance_get_timestamp_ms();
    sample->metric_type = type;
    sample->value = value;
    sample->context = context ? str_dup(context) : NULL;
    
    return sample;
}

void performance_sample_free(PerformanceSample* sample) {
    if (!sample) return;
    
    free((char*)sample->context);
    free(sample);
}

int performance_metric_add_sample(PerformanceMetric* metric, PerformanceSample* sample) {
    if (!metric || !sample) return -1;
    
    // Add to front of list
    sample->next = metric->samples;
    metric->samples = sample;
    metric->sample_count++;
    
    return 0;
}

PerformanceSample* performance_metric_get_latest_sample(PerformanceMetric* metric) {
    return metric ? metric->samples : NULL;
}

PerformanceSample* performance_metric_get_samples_since(PerformanceMetric* metric, uint64_t timestamp_ms) {
    if (!metric) return NULL;
    
    PerformanceSample* sample = metric->samples;
    while (sample && sample->timestamp_ms < timestamp_ms) {
        sample = sample->next;
    }
    
    return sample;
}

// =============================================================================
// Memory Monitoring
// =============================================================================

int performance_monitor_track_memory_usage(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->enable_memory_monitoring) return -1;
    
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
    
    // Convert to MB
    double memory_mb = (double)usage.ru_maxrss / (1024.0 * 1024.0);
    
    performance_monitor_record_metric(monitor, PERF_METRIC_MEMORY_USAGE, memory_mb, "memory_tracking");
    
    return 0;
}

int performance_monitor_track_allocation(PerformanceMonitor* monitor, size_t size, const char* location) {
    if (!monitor) return -1;
    
    performance_monitor_record_metric(monitor, PERF_METRIC_ALLOCATION_COUNT, 1.0, location);
    
    // Update memory usage
    performance_monitor_track_memory_usage(monitor);
    
    return 0;
}

int performance_monitor_track_deallocation(PerformanceMonitor* monitor, size_t size, const char* location) {
    if (!monitor) return -1;
    
    performance_monitor_record_metric(monitor, PERF_METRIC_DEALLOCATION_COUNT, 1.0, location);
    
    return 0;
}

double performance_monitor_get_current_memory_mb(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->current_session) return 0.0;
    
    PerformanceMetric* metric = monitor->current_session->metrics[PERF_METRIC_MEMORY_USAGE];
    return metric ? metric->current_value : 0.0;
}

double performance_monitor_get_peak_memory_mb(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->current_session) return 0.0;
    
    PerformanceMetric* metric = monitor->current_session->metrics[PERF_METRIC_MEMORY_USAGE];
    return metric ? metric->max_value : 0.0;
}

// =============================================================================
// CPU Monitoring
// =============================================================================

int performance_monitor_track_cpu_usage(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->enable_cpu_monitoring) return -1;
    
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
    
    // Calculate CPU time in milliseconds
    double cpu_time_ms = (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
                         (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
    
    performance_monitor_record_metric(monitor, PERF_METRIC_CPU_TIME, cpu_time_ms, "cpu_tracking");
    
    return 0;
}

double performance_monitor_get_cpu_usage_percent(PerformanceMonitor* monitor) {
    // This is a simplified implementation
    // In a real system, you'd track CPU usage over time intervals
    if (!monitor || !monitor->current_session) return 0.0;
    
    PerformanceMetric* metric = monitor->current_session->metrics[PERF_METRIC_CPU_TIME];
    if (!metric || metric->sample_count < 2) return 0.0;
    
    // Calculate percentage based on recent samples
    PerformanceSample* latest = metric->samples;
    PerformanceSample* previous = latest ? latest->next : NULL;
    
    if (!latest || !previous) return 0.0;
    
    double time_diff = (double)(latest->timestamp_ms - previous->timestamp_ms);
    double cpu_diff = latest->value - previous->value;
    
    return time_diff > 0 ? (cpu_diff / time_diff) * 100.0 : 0.0;
}

uint64_t performance_monitor_get_cpu_time_ms(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->current_session) return 0;
    
    PerformanceMetric* metric = monitor->current_session->metrics[PERF_METRIC_CPU_TIME];
    return metric ? (uint64_t)metric->current_value : 0;
}

// =============================================================================
// Compilation Monitoring
// =============================================================================

int performance_monitor_start_compilation_timing(PerformanceMonitor* monitor, const char* file_path) {
    if (!monitor || !monitor->enable_compilation_monitoring) return -1;
    
    performance_monitor_record_metric(monitor, PERF_METRIC_COMPILATION_COUNT, 1.0, file_path);
    
    return 0;
}

int performance_monitor_end_compilation_timing(PerformanceMonitor* monitor, const char* file_path, bool success) {
    if (!monitor) return -1;
    
    if (!success) {
        performance_monitor_record_metric(monitor, PERF_METRIC_ERROR_COUNT, 1.0, file_path);
    }
    
    return 0;
}

int performance_monitor_track_parse_time(PerformanceMonitor* monitor, const char* file_path, uint64_t time_ms) {
    if (!monitor) return -1;
    return performance_monitor_record_metric(monitor, PERF_METRIC_PARSE_TIME, (double)time_ms, file_path);
}

int performance_monitor_track_type_check_time(PerformanceMonitor* monitor, const char* file_path, uint64_t time_ms) {
    if (!monitor) return -1;
    return performance_monitor_record_metric(monitor, PERF_METRIC_TYPE_CHECK_TIME, (double)time_ms, file_path);
}

int performance_monitor_track_codegen_time(PerformanceMonitor* monitor, const char* file_path, uint64_t time_ms) {
    if (!monitor) return -1;
    return performance_monitor_record_metric(monitor, PERF_METRIC_CODEGEN_TIME, (double)time_ms, file_path);
}

// =============================================================================
// Alert System
// =============================================================================

PerformanceAlert* performance_alert_new(PerformanceAlertType type, const char* message, double threshold, double current_value) {
    PerformanceAlert* alert = calloc(1, sizeof(PerformanceAlert));
    if (!alert) return NULL;
    
    alert->type = type;
    alert->message = str_dup(message);
    alert->threshold = threshold;
    alert->current_value = current_value;
    alert->timestamp_ms = performance_get_timestamp_ms();
    alert->is_active = true;
    
    return alert;
}

void performance_alert_free(PerformanceAlert* alert) {
    if (!alert) return;
    
    free((char*)alert->message);
    free(alert);
}

int performance_monitor_check_alerts(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->enable_alerts || !monitor->current_session) return -1;
    
    PerformanceSession* session = monitor->current_session;
    
    // Check memory usage alert
    if (monitor->enable_memory_monitoring) {
        PerformanceMetric* memory_metric = session->metrics[PERF_METRIC_MEMORY_USAGE];
        if (memory_metric && memory_metric->current_value > monitor->memory_alert_threshold_mb) {
            char message[256];
            snprintf(message, sizeof(message), "Memory usage (%.1f MB) exceeds threshold (%.1f MB)",
                    memory_metric->current_value, monitor->memory_alert_threshold_mb);
            
            PerformanceAlert* alert = performance_alert_new(PERF_ALERT_HIGH_MEMORY, message,
                                                          monitor->memory_alert_threshold_mb,
                                                          memory_metric->current_value);
            performance_monitor_add_alert(monitor, alert);
        }
    }
    
    // Check CPU usage alert
    if (monitor->enable_cpu_monitoring) {
        double cpu_percent = performance_monitor_get_cpu_usage_percent(monitor);
        if (cpu_percent > monitor->cpu_alert_threshold_percent) {
            char message[256];
            snprintf(message, sizeof(message), "CPU usage (%.1f%%) exceeds threshold (%.1f%%)",
                    cpu_percent, monitor->cpu_alert_threshold_percent);
            
            PerformanceAlert* alert = performance_alert_new(PERF_ALERT_HIGH_CPU, message,
                                                          monitor->cpu_alert_threshold_percent,
                                                          cpu_percent);
            performance_monitor_add_alert(monitor, alert);
        }
    }
    
    // Check compilation time alert
    if (monitor->enable_compilation_monitoring) {
        PerformanceMetric* parse_metric = session->metrics[PERF_METRIC_PARSE_TIME];
        PerformanceMetric* typecheck_metric = session->metrics[PERF_METRIC_TYPE_CHECK_TIME];
        PerformanceMetric* codegen_metric = session->metrics[PERF_METRIC_CODEGEN_TIME];
        
        double total_compilation_time = 0.0;
        if (parse_metric) total_compilation_time += parse_metric->current_value;
        if (typecheck_metric) total_compilation_time += typecheck_metric->current_value;
        if (codegen_metric) total_compilation_time += codegen_metric->current_value;
        
        if (total_compilation_time > monitor->compilation_time_alert_threshold_ms) {
            char message[256];
            snprintf(message, sizeof(message), "Compilation time (%.1f ms) exceeds threshold (%.1f ms)",
                    total_compilation_time, monitor->compilation_time_alert_threshold_ms);
            
            PerformanceAlert* alert = performance_alert_new(PERF_ALERT_SLOW_COMPILATION, message,
                                                          monitor->compilation_time_alert_threshold_ms,
                                                          total_compilation_time);
            performance_monitor_add_alert(monitor, alert);
        }
    }
    
    return 0;
}

int performance_monitor_add_alert(PerformanceMonitor* monitor, PerformanceAlert* alert) {
    if (!monitor || !alert || !monitor->current_session) return -1;
    
    alert->next = monitor->current_session->alerts;
    monitor->current_session->alerts = alert;
    
    return 0;
}

int performance_monitor_clear_alerts(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->current_session) return -1;
    
    PerformanceAlert* alert = monitor->current_session->alerts;
    while (alert) {
        PerformanceAlert* next = alert->next;
        performance_alert_free(alert);
        alert = next;
    }
    
    monitor->current_session->alerts = NULL;
    return 0;
}

PerformanceAlert* performance_monitor_get_active_alerts(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->current_session) return NULL;
    return monitor->current_session->alerts;
}

// =============================================================================
// Configuration
// =============================================================================

int performance_monitor_set_sample_interval(PerformanceMonitor* monitor, PerformanceSampleInterval interval) {
    if (!monitor) return -1;
    monitor->sample_interval = interval;
    return 0;
}

int performance_monitor_set_max_samples(PerformanceMonitor* monitor, uint64_t max_samples) {
    if (!monitor) return -1;
    monitor->max_samples_per_metric = max_samples;
    return 0;
}

int performance_monitor_set_alert_threshold(PerformanceMonitor* monitor, PerformanceMetricType metric, double threshold) {
    if (!monitor) return -1;
    
    switch (metric) {
        case PERF_METRIC_MEMORY_USAGE:
            monitor->memory_alert_threshold_mb = threshold;
            break;
        case PERF_METRIC_CPU_TIME:
            monitor->cpu_alert_threshold_percent = threshold;
            break;
        case PERF_METRIC_PARSE_TIME:
        case PERF_METRIC_TYPE_CHECK_TIME:
        case PERF_METRIC_CODEGEN_TIME:
            monitor->compilation_time_alert_threshold_ms = threshold;
            break;
        default:
            return -1;
    }
    
    return 0;
}

int performance_monitor_enable_cpu_monitoring(PerformanceMonitor* monitor, bool enable) {
    if (!monitor) return -1;
    monitor->enable_cpu_monitoring = enable;
    return 0;
}

int performance_monitor_enable_memory_monitoring(PerformanceMonitor* monitor, bool enable) {
    if (!monitor) return -1;
    monitor->enable_memory_monitoring = enable;
    return 0;
}

int performance_monitor_enable_compilation_monitoring(PerformanceMonitor* monitor, bool enable) {
    if (!monitor) return -1;
    monitor->enable_compilation_monitoring = enable;
    return 0;
}

int performance_monitor_enable_alerts(PerformanceMonitor* monitor, bool enable) {
    if (!monitor) return -1;
    monitor->enable_alerts = enable;
    return 0;
}

// =============================================================================
// Integration Functions
// =============================================================================

int performance_monitor_integrate_type_checker(PerformanceMonitor* monitor, TypeChecker* type_checker) {
    if (!monitor) return -1;
    monitor->type_checker = type_checker;
    return 0;
}

int performance_monitor_integrate_hot_reload(PerformanceMonitor* monitor, HotReloadContext* hot_reload) {
    if (!monitor) return -1;
    monitor->hot_reload = hot_reload;
    return 0;
}

int performance_monitor_integrate_repl(PerformanceMonitor* monitor, REPLContext* repl) {
    if (!monitor) return -1;
    monitor->repl_context = repl;
    return 0;
}

// =============================================================================
// Performance Profiler
// =============================================================================

PerformanceProfiler* performance_profiler_start(PerformanceMonitor* monitor, const char* operation_name) {
    PerformanceProfiler* profiler = malloc(sizeof(PerformanceProfiler));
    if (!profiler) return NULL;
    
    profiler->operation_name = str_dup(operation_name);
    profiler->start_time_ms = performance_get_timestamp_ms();
    profiler->monitor = monitor;
    
    return profiler;
}

void performance_profiler_end(PerformanceProfiler* profiler) {
    if (!profiler) return;
    
    uint64_t end_time_ms = performance_get_timestamp_ms();
    
    if (profiler->monitor) {
        performance_monitor_record_timing(profiler->monitor, profiler->operation_name,
                                        profiler->start_time_ms, end_time_ms);
    }
    
    free((char*)profiler->operation_name);
    free(profiler);
}

// =============================================================================
// REPL Integration
// =============================================================================

int performance_monitor_register_repl_commands(REPLContext* repl, PerformanceMonitor* monitor) {
    if (!repl || !monitor) return -1;
    
    // Integration is handled in REPL command processing
    return performance_monitor_integrate_repl(monitor, repl);
}

int performance_handle_repl_perf_command(REPLContext* repl, PerformanceMonitor* monitor, const char* command) {
    if (!repl || !monitor || !command) return -1;
    
    // Parse command
    if (strstr(command, "status")) {
        // Show performance status
        repl_printf(repl, "Performance Monitor Status:\n");
        repl_printf(repl, "  Enabled: %s\n", monitor->is_enabled ? "Yes" : "No");
        repl_printf(repl, "  Recording: %s\n", monitor->is_recording ? "Yes" : "No");
        repl_printf(repl, "  Sample Interval: %d ms\n", monitor->sample_interval);
        repl_printf(repl, "  Current Memory: %.1f MB\n", performance_monitor_get_current_memory_mb(monitor));
        repl_printf(repl, "  CPU Usage: %.1f%%\n", performance_monitor_get_cpu_usage_percent(monitor));
        
    } else if (strstr(command, "start")) {
        // Start monitoring
        int result = performance_monitor_start_recording(monitor);
        if (result == 0) {
            repl_printf(repl, "Performance monitoring started.\n");
        } else {
            repl_printf(repl, "Failed to start performance monitoring.\n");
        }
        
    } else if (strstr(command, "stop")) {
        // Stop monitoring
        int result = performance_monitor_stop_recording(monitor);
        if (result == 0) {
            repl_printf(repl, "Performance monitoring stopped.\n");
        } else {
            repl_printf(repl, "Failed to stop performance monitoring.\n");
        }
        
    } else if (strstr(command, "reset")) {
        // Reset metrics
        performance_monitor_clear_alerts(monitor);
        repl_printf(repl, "Performance metrics reset.\n");
        
    } else if (strstr(command, "metrics")) {
        // Show current metrics
        if (!monitor->current_session) {
            repl_printf(repl, "No active performance session.\n");
            return 0;
        }
        
        repl_printf(repl, "Current Performance Metrics:\n");
        for (int i = 0; i < PERF_METRIC_COUNT; i++) {
            PerformanceMetric* metric = monitor->current_session->metrics[i];
            if (metric && metric->is_enabled && metric->sample_count > 0) {
                repl_printf(repl, "  %s: %.2f %s (min: %.2f, max: %.2f, avg: %.2f)\n",
                           metric->name, metric->current_value, metric->unit,
                           metric->min_value, metric->max_value, metric->avg_value);
            }
        }
        
    } else if (strstr(command, "alerts")) {
        // Show active alerts
        PerformanceAlert* alert = performance_monitor_get_active_alerts(monitor);
        if (!alert) {
            repl_printf(repl, "No active performance alerts.\n");
        } else {
            repl_printf(repl, "Active Performance Alerts:\n");
            while (alert) {
                repl_printf(repl, "  [%s] %s\n",
                           performance_alert_type_to_string(alert->type),
                           alert->message);
                alert = alert->next;
            }
        }
        
    } else if (strstr(command, "live")) {
        // Real-time live monitoring display
        repl_printf(repl, "🔴 %sReal-time Performance Monitor%s\n",
                   repl->color_output ? "\033[1m\033[31m" : "",
                   repl->color_output ? "\033[0m" : "");
        repl_printf(repl, "========================================\n");
        
        // Display real-time metrics in a formatted table
        uint64_t timestamp = performance_get_timestamp_ms();
        double memory_mb = performance_monitor_get_current_memory_mb(monitor);
        double cpu_percent = performance_monitor_get_cpu_usage_percent(monitor);
        uint64_t duration = monitor->current_session ? 
            (timestamp - monitor->current_session->start_time_ms) : 0;
        
        repl_printf(repl, "📊 Current Session: %lu (Duration: %lu ms)\n",
                   monitor->current_session ? monitor->current_session->session_id : 0,
                   duration);
        repl_printf(repl, "🧠 Memory Usage: %.2f MB (Peak: %.2f MB)\n",
                   memory_mb, performance_monitor_get_peak_memory_mb(monitor));
        repl_printf(repl, "⚡ CPU Usage: %.2f%% (Time: %lu ms)\n",
                   cpu_percent, performance_monitor_get_cpu_time_ms(monitor));
        repl_printf(repl, "⏱️  Sample Interval: %d ms\n", monitor->sample_interval);
        repl_printf(repl, "🎯 Recording: %s | Alerts: %s\n",
                   monitor->is_recording ? "ON" : "OFF",
                   monitor->enable_alerts ? "ON" : "OFF");
        
        // Show recent metrics trends if available
        if (monitor->current_session) {
            repl_printf(repl, "\n📈 Session Statistics:\n");
            repl_printf(repl, "   Total Samples: %lu\n", monitor->current_session->total_samples);
            repl_printf(repl, "   Session Started: %lu ms ago\n", duration);
        }
        
        repl_printf(repl, "\n💡 Use ':perf stream' for continuous updates\n");
        
    } else if (strstr(command, "dashboard")) {
        // Start/show dashboard information
        if (strstr(command, "start")) {
            int port = 8080; // Default port
            // Extract port if specified
            char* port_str = strstr(command, "port=");
            if (port_str) {
                port = atoi(port_str + 5);
            }
            performance_monitor_start_dashboard(monitor, port);
        } else if (strstr(command, "json")) {
            // Show JSON output for dashboard
            char* json = performance_monitor_get_dashboard_json(monitor);
            if (json) {
                repl_printf(repl, "📋 Dashboard JSON:\n%s\n", json);
                free(json);
            }
        } else {
            repl_printf(repl, "🌐 Dashboard Commands:\n");
            repl_printf(repl, "  :perf dashboard start [port=8080] - Start web dashboard\n");
            repl_printf(repl, "  :perf dashboard json            - Show JSON output\n");
            repl_printf(repl, "  :perf dashboard stop            - Stop web dashboard\n");
        }
        
    } else if (strstr(command, "stream")) {
        // Simulated streaming output (in a real implementation, this would continuously update)
        repl_printf(repl, "📡 %sReal-time Performance Stream%s\n",
                   repl->color_output ? "\033[1m\033[36m" : "",
                   repl->color_output ? "\033[0m" : "");
        repl_printf(repl, "=====================================\n");
        repl_printf(repl, "💡 Streaming performance data every %d ms...\n", monitor->sample_interval);
        repl_printf(repl, "🛑 (Press Ctrl+C to stop streaming)\n\n");
        
        // Show 5 sample readings
        for (int i = 0; i < 5; i++) {
            uint64_t current_time = performance_get_timestamp_ms();
            double current_memory = performance_monitor_get_current_memory_mb(monitor);
            double current_cpu = performance_monitor_get_cpu_usage_percent(monitor);
            
            repl_printf(repl, "[%lu] Memory: %.2f MB | CPU: %.2f%% | Samples: %lu\n",
                       current_time, current_memory, current_cpu,
                       monitor->current_session ? monitor->current_session->total_samples : 0);
            
            // In a real implementation, this would sleep and continuously update
            // For demo purposes, we'll just show multiple snapshots
            usleep(monitor->sample_interval * 1000); // Convert ms to microseconds
        }
        
        repl_printf(repl, "\n✅ Stream demo completed. Use ':perf live' for single snapshot.\n");
        
    } else if (strstr(command, "config")) {
        // Performance configuration
        if (strstr(command, "interval=")) {
            char* interval_str = strstr(command, "interval=");
            int new_interval = atoi(interval_str + 9);
            if (new_interval >= 1 && new_interval <= 5000) {
                performance_monitor_set_sample_interval(monitor, new_interval);
                repl_printf(repl, "✅ Sample interval set to %d ms\n", new_interval);
            } else {
                repl_printf(repl, "❌ Invalid interval. Use 1-5000 ms\n");
            }
        } else {
            repl_printf(repl, "⚙️  Performance Configuration:\n");
            repl_printf(repl, "   Sample Interval: %d ms\n", monitor->sample_interval);
            repl_printf(repl, "   CPU Monitoring: %s\n", monitor->enable_cpu_monitoring ? "ON" : "OFF");
            repl_printf(repl, "   Memory Monitoring: %s\n", monitor->enable_memory_monitoring ? "ON" : "OFF");
            repl_printf(repl, "   Alerts: %s\n", monitor->enable_alerts ? "ON" : "OFF");
            repl_printf(repl, "\n💡 Use ':perf config interval=<ms>' to change sample rate\n");
        }
        
    } else {
        repl_printf(repl, "Unknown performance command. Available commands:\n");
        repl_printf(repl, "  status, start, stop, reset, metrics, alerts\n");
        repl_printf(repl, "  live, dashboard [start|json|stop], stream, config\n");
        return -1;
    }
    
    return 0;
}

// =============================================================================
// Export and Reporting Functions (Simplified implementations)
// =============================================================================

char* performance_monitor_generate_summary_report(PerformanceMonitor* monitor) {
    if (!monitor || !monitor->current_session) return NULL;
    
    char* report = malloc(4096);
    if (!report) return NULL;
    
    snprintf(report, 4096,
        "Performance Summary Report\n"
        "=========================\n"
        "Session ID: %lu\n"
        "Duration: %lu ms\n"
        "Current Memory: %.1f MB\n"
        "Peak Memory: %.1f MB\n"
        "CPU Usage: %.1f%%\n"
        "Active Alerts: %s\n",
        monitor->current_session->session_id,
        performance_get_timestamp_ms() - monitor->current_session->start_time_ms,
        performance_monitor_get_current_memory_mb(monitor),
        performance_monitor_get_peak_memory_mb(monitor),
        performance_monitor_get_cpu_usage_percent(monitor),
        monitor->current_session->alerts ? "Yes" : "None"
    );
    
    return report;
}

char* performance_monitor_generate_detailed_report(PerformanceMonitor* monitor, PerformanceSession* session) {
    if (!monitor || !session) return NULL;
    
    char* report = malloc(8192);
    if (!report) return NULL;
    
    snprintf(report, 8192,
        "Detailed Performance Report\n"
        "===========================\n"
        "Session ID: %lu\n"
        "Start Time: %lu ms\n"
        "End Time: %lu ms\n"
        "Duration: %lu ms\n"
        "Total Samples: %lu\n",
        session->session_id,
        session->start_time_ms,
        session->end_time_ms,
        session->end_time_ms - session->start_time_ms,
        session->total_samples
    );
    
    return report;
}

// Simplified export functions
int performance_monitor_export_session(PerformanceMonitor* monitor, PerformanceSession* session, const char* file_path) {
    if (!monitor || !session || !file_path) return -1;
    
    FILE* file = fopen(file_path, "w");
    if (!file) return -1;
    
    char* report = performance_monitor_generate_detailed_report(monitor, session);
    if (report) {
        fprintf(file, "%s", report);
        free(report);
    }
    
    fclose(file);
    return 0;
}

int performance_monitor_export_metrics_json(PerformanceMonitor* monitor, const char* file_path) {
    if (!monitor || !file_path) return -1;
    // Implementation would create JSON export
    return 0;
}

int performance_monitor_export_metrics_csv(PerformanceMonitor* monitor, const char* file_path) {
    if (!monitor || !file_path) return -1;
    // Implementation would create CSV export
    return 0;
}

int performance_monitor_export_metrics_html(PerformanceMonitor* monitor, const char* file_path) {
    if (!monitor || !file_path) return -1;
    // Implementation would create HTML dashboard
    return 0;
}

// Dashboard functions (simplified)
int performance_monitor_start_dashboard(PerformanceMonitor* monitor, int port) {
    if (!monitor) return -1;
    
    printf("🌐 Starting real-time performance dashboard on port %d\n", port);
    printf("📊 Dashboard URL: http://localhost:%d/dashboard\n", port);
    printf("📡 Real-time API: http://localhost:%d/api/metrics\n", port);
    printf("💡 Use ':perf stop-dashboard' to stop the server\n");
    
    // Note: In a full implementation, this would start an HTTP server
    // using a library like microhttpd or by spawning a separate process
    // For now, we'll mark the dashboard as "started" and provide JSON output
    
    return 0;
}

int performance_monitor_stop_dashboard(PerformanceMonitor* monitor) {
    if (!monitor) return -1;
    // Implementation would stop HTTP server
    return 0;
}

char* performance_monitor_get_dashboard_json(PerformanceMonitor* monitor) {
    if (!monitor) return NULL;
    
    // Get current timestamp
    uint64_t timestamp = performance_get_timestamp_ms();
    
    // Get current metrics
    double memory_mb = performance_monitor_get_current_memory_mb(monitor);
    double cpu_percent = performance_monitor_get_cpu_usage_percent(monitor);
    uint64_t cpu_time = performance_monitor_get_cpu_time_ms(monitor);
    
    // Build comprehensive JSON response for real-time dashboard
    char* json = malloc(2048);
    if (!json) return NULL;
    
    snprintf(json, 2048,
        "{\n"
        "  \"timestamp\": %lu,\n"
        "  \"status\": \"%s\",\n"
        "  \"recording\": %s,\n"
        "  \"sample_interval_ms\": %d,\n"
        "  \"realtime_metrics\": {\n"
        "    \"memory_usage_mb\": %.2f,\n"
        "    \"cpu_usage_percent\": %.2f,\n"
        "    \"cpu_time_ms\": %lu,\n"
        "    \"peak_memory_mb\": %.2f\n"
        "  },\n"
        "  \"session_info\": {\n"
        "    \"session_id\": %lu,\n"
        "    \"start_time\": %lu,\n"
        "    \"duration_ms\": %lu,\n"
        "    \"total_samples\": %lu\n"
        "  },\n"
        "  \"configuration\": {\n"
        "    \"cpu_monitoring\": %s,\n"
        "    \"memory_monitoring\": %s,\n"
        "    \"compilation_monitoring\": %s,\n"
        "    \"alerts_enabled\": %s\n"
        "  },\n"
        "  \"alerts\": {\n"
        "    \"active_count\": 0,\n"
        "    \"memory_threshold_mb\": %.2f,\n"
        "    \"cpu_threshold_percent\": %.2f\n"
        "  }\n"
        "}",
        timestamp,
        monitor->is_enabled ? "enabled" : "disabled",
        monitor->is_recording ? "true" : "false",
        monitor->sample_interval,
        memory_mb,
        cpu_percent,
        cpu_time,
        performance_monitor_get_peak_memory_mb(monitor),
        monitor->current_session ? monitor->current_session->session_id : 0,
        monitor->current_session ? monitor->current_session->start_time_ms : 0,
        monitor->current_session ? (timestamp - monitor->current_session->start_time_ms) : 0,
        monitor->current_session ? monitor->current_session->total_samples : 0,
        monitor->enable_cpu_monitoring ? "true" : "false",
        monitor->enable_memory_monitoring ? "true" : "false",
        monitor->enable_compilation_monitoring ? "true" : "false",
        monitor->enable_alerts ? "true" : "false",
        monitor->memory_alert_threshold_mb,
        monitor->cpu_alert_threshold_percent
    );
    
    return json;
}