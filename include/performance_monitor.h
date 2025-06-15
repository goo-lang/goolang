#ifndef PERFORMANCE_MONITOR_H
#define PERFORMANCE_MONITOR_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/resource.h>

// =============================================================================
// Performance Monitoring System for Goo Compiler IDE
// =============================================================================

// Forward declarations
typedef struct TypeChecker TypeChecker;
typedef struct HotReloadContext HotReloadContext;
typedef struct REPLContext REPLContext;

// =============================================================================
// Performance Metrics Types
// =============================================================================

typedef enum {
    PERF_METRIC_CPU_TIME,
    PERF_METRIC_MEMORY_USAGE,
    PERF_METRIC_PARSE_TIME,
    PERF_METRIC_TYPE_CHECK_TIME,
    PERF_METRIC_CODEGEN_TIME,
    PERF_METRIC_IO_OPERATIONS,
    PERF_METRIC_GC_TIME,
    PERF_METRIC_ALLOCATION_COUNT,
    PERF_METRIC_DEALLOCATION_COUNT,
    PERF_METRIC_PEAK_MEMORY,
    PERF_METRIC_COMPILATION_COUNT,
    PERF_METRIC_ERROR_COUNT,
    PERF_METRIC_COUNT
} PerformanceMetricType;

typedef enum {
    PERF_SAMPLE_INTERVAL_1MS = 1,
    PERF_SAMPLE_INTERVAL_10MS = 10,
    PERF_SAMPLE_INTERVAL_100MS = 100,
    PERF_SAMPLE_INTERVAL_1S = 1000,
    PERF_SAMPLE_INTERVAL_5S = 5000
} PerformanceSampleInterval;

typedef enum {
    PERF_ALERT_NONE,
    PERF_ALERT_HIGH_MEMORY,
    PERF_ALERT_HIGH_CPU,
    PERF_ALERT_SLOW_COMPILATION,
    PERF_ALERT_FREQUENT_ERRORS,
    PERF_ALERT_MEMORY_LEAK
} PerformanceAlertType;

// =============================================================================
// Core Data Structures
// =============================================================================

typedef struct PerformanceSample {
    uint64_t timestamp_ms;
    PerformanceMetricType metric_type;
    double value;
    const char* context;  // e.g., "parsing main.goo", "type checking"
    struct PerformanceSample* next;
} PerformanceSample;

typedef struct PerformanceMetric {
    PerformanceMetricType type;
    const char* name;
    const char* unit;
    double current_value;
    double min_value;
    double max_value;
    double avg_value;
    uint64_t sample_count;
    PerformanceSample* samples;
    bool is_enabled;
} PerformanceMetric;

typedef struct PerformanceAlert {
    PerformanceAlertType type;
    const char* message;
    double threshold;
    double current_value;
    uint64_t timestamp_ms;
    bool is_active;
    struct PerformanceAlert* next;
} PerformanceAlert;

typedef struct PerformanceSession {
    uint64_t session_id;
    uint64_t start_time_ms;
    uint64_t end_time_ms;
    uint64_t total_samples;
    PerformanceMetric* metrics[PERF_METRIC_COUNT];
    PerformanceAlert* alerts;
    struct PerformanceSession* next;
} PerformanceSession;

typedef struct PerformanceMonitor {
    // Core state
    bool is_enabled;
    bool is_recording;
    PerformanceSampleInterval sample_interval;
    uint64_t max_samples_per_metric;
    uint64_t max_sessions;
    
    // Current session
    PerformanceSession* current_session;
    PerformanceSession* sessions;
    uint64_t session_count;
    
    // Monitoring configuration
    bool enable_cpu_monitoring;
    bool enable_memory_monitoring;
    bool enable_compilation_monitoring;
    bool enable_alerts;
    bool enable_auto_export;
    
    // Alert thresholds
    double memory_alert_threshold_mb;
    double cpu_alert_threshold_percent;
    double compilation_time_alert_threshold_ms;
    uint32_t error_rate_alert_threshold;
    
    // Integration points
    TypeChecker* type_checker;
    HotReloadContext* hot_reload;
    REPLContext* repl_context;
    
    // Timing helpers
    struct timespec start_time;
    struct rusage start_usage;
    
    // Export settings
    const char* export_directory;
    bool export_json;
    bool export_csv;
    bool export_html;
} PerformanceMonitor;

// =============================================================================
// Performance Monitor Lifecycle
// =============================================================================

PerformanceMonitor* performance_monitor_new(void);
void performance_monitor_free(PerformanceMonitor* monitor);

int performance_monitor_init(PerformanceMonitor* monitor);
int performance_monitor_cleanup(PerformanceMonitor* monitor);

int performance_monitor_start_recording(PerformanceMonitor* monitor);
int performance_monitor_stop_recording(PerformanceMonitor* monitor);
int performance_monitor_pause_recording(PerformanceMonitor* monitor);
int performance_monitor_resume_recording(PerformanceMonitor* monitor);

// =============================================================================
// Session Management
// =============================================================================

PerformanceSession* performance_session_new(uint64_t session_id);
void performance_session_free(PerformanceSession* session);

int performance_monitor_start_session(PerformanceMonitor* monitor);
int performance_monitor_end_session(PerformanceMonitor* monitor);
PerformanceSession* performance_monitor_get_session(PerformanceMonitor* monitor, uint64_t session_id);
PerformanceSession* performance_monitor_get_current_session(PerformanceMonitor* monitor);

// =============================================================================
// Metric Management
// =============================================================================

PerformanceMetric* performance_metric_new(PerformanceMetricType type);
void performance_metric_free(PerformanceMetric* metric);

int performance_monitor_enable_metric(PerformanceMonitor* monitor, PerformanceMetricType type);
int performance_monitor_disable_metric(PerformanceMonitor* monitor, PerformanceMetricType type);
bool performance_monitor_is_metric_enabled(PerformanceMonitor* monitor, PerformanceMetricType type);

int performance_monitor_record_metric(PerformanceMonitor* monitor, PerformanceMetricType type, double value, const char* context);
int performance_monitor_record_timing(PerformanceMonitor* monitor, const char* operation_name, uint64_t start_time_ms, uint64_t end_time_ms);

// =============================================================================
// Sample Management
// =============================================================================

PerformanceSample* performance_sample_new(PerformanceMetricType type, double value, const char* context);
void performance_sample_free(PerformanceSample* sample);

int performance_metric_add_sample(PerformanceMetric* metric, PerformanceSample* sample);
PerformanceSample* performance_metric_get_latest_sample(PerformanceMetric* metric);
PerformanceSample* performance_metric_get_samples_since(PerformanceMetric* metric, uint64_t timestamp_ms);

// =============================================================================
// Memory Monitoring
// =============================================================================

int performance_monitor_track_memory_usage(PerformanceMonitor* monitor);
int performance_monitor_track_allocation(PerformanceMonitor* monitor, size_t size, const char* location);
int performance_monitor_track_deallocation(PerformanceMonitor* monitor, size_t size, const char* location);
double performance_monitor_get_current_memory_mb(PerformanceMonitor* monitor);
double performance_monitor_get_peak_memory_mb(PerformanceMonitor* monitor);

// =============================================================================
// CPU Monitoring
// =============================================================================

int performance_monitor_track_cpu_usage(PerformanceMonitor* monitor);
double performance_monitor_get_cpu_usage_percent(PerformanceMonitor* monitor);
uint64_t performance_monitor_get_cpu_time_ms(PerformanceMonitor* monitor);

// =============================================================================
// Compilation Monitoring
// =============================================================================

int performance_monitor_start_compilation_timing(PerformanceMonitor* monitor, const char* file_path);
int performance_monitor_end_compilation_timing(PerformanceMonitor* monitor, const char* file_path, bool success);
int performance_monitor_track_parse_time(PerformanceMonitor* monitor, const char* file_path, uint64_t time_ms);
int performance_monitor_track_type_check_time(PerformanceMonitor* monitor, const char* file_path, uint64_t time_ms);
int performance_monitor_track_codegen_time(PerformanceMonitor* monitor, const char* file_path, uint64_t time_ms);

// =============================================================================
// Alert System
// =============================================================================

PerformanceAlert* performance_alert_new(PerformanceAlertType type, const char* message, double threshold, double current_value);
void performance_alert_free(PerformanceAlert* alert);

int performance_monitor_check_alerts(PerformanceMonitor* monitor);
int performance_monitor_add_alert(PerformanceMonitor* monitor, PerformanceAlert* alert);
int performance_monitor_clear_alerts(PerformanceMonitor* monitor);
PerformanceAlert* performance_monitor_get_active_alerts(PerformanceMonitor* monitor);

// =============================================================================
// Configuration
// =============================================================================

int performance_monitor_set_sample_interval(PerformanceMonitor* monitor, PerformanceSampleInterval interval);
int performance_monitor_set_max_samples(PerformanceMonitor* monitor, uint64_t max_samples);
int performance_monitor_set_alert_threshold(PerformanceMonitor* monitor, PerformanceMetricType metric, double threshold);

int performance_monitor_enable_cpu_monitoring(PerformanceMonitor* monitor, bool enable);
int performance_monitor_enable_memory_monitoring(PerformanceMonitor* monitor, bool enable);
int performance_monitor_enable_compilation_monitoring(PerformanceMonitor* monitor, bool enable);
int performance_monitor_enable_alerts(PerformanceMonitor* monitor, bool enable);

// =============================================================================
// Integration with IDE Components
// =============================================================================

int performance_monitor_integrate_type_checker(PerformanceMonitor* monitor, TypeChecker* type_checker);
int performance_monitor_integrate_hot_reload(PerformanceMonitor* monitor, HotReloadContext* hot_reload);
int performance_monitor_integrate_repl(PerformanceMonitor* monitor, REPLContext* repl);

// =============================================================================
// Data Export and Reporting
// =============================================================================

int performance_monitor_export_session(PerformanceMonitor* monitor, PerformanceSession* session, const char* file_path);
int performance_monitor_export_metrics_json(PerformanceMonitor* monitor, const char* file_path);
int performance_monitor_export_metrics_csv(PerformanceMonitor* monitor, const char* file_path);
int performance_monitor_export_metrics_html(PerformanceMonitor* monitor, const char* file_path);

char* performance_monitor_generate_summary_report(PerformanceMonitor* monitor);
char* performance_monitor_generate_detailed_report(PerformanceMonitor* monitor, PerformanceSession* session);

// =============================================================================
// Real-time Dashboard
// =============================================================================

int performance_monitor_start_dashboard(PerformanceMonitor* monitor, int port);
int performance_monitor_stop_dashboard(PerformanceMonitor* monitor);
char* performance_monitor_get_dashboard_json(PerformanceMonitor* monitor);

// =============================================================================
// Utility Functions
// =============================================================================

uint64_t performance_get_timestamp_ms(void);
const char* performance_metric_type_to_string(PerformanceMetricType type);
const char* performance_alert_type_to_string(PerformanceAlertType type);

// =============================================================================
// Performance Profiling Helpers
// =============================================================================

typedef struct PerformanceProfiler {
    const char* operation_name;
    uint64_t start_time_ms;
    PerformanceMonitor* monitor;
} PerformanceProfiler;

PerformanceProfiler* performance_profiler_start(PerformanceMonitor* monitor, const char* operation_name);
void performance_profiler_end(PerformanceProfiler* profiler);

// Macro for automatic profiling
#define PERF_PROFILE(monitor, operation) \
    PerformanceProfiler* __profiler = performance_profiler_start(monitor, operation); \
    __attribute__((cleanup(performance_profiler_end))) PerformanceProfiler* __profiler_cleanup = __profiler

// =============================================================================
// REPL Integration Commands
// =============================================================================

int performance_monitor_register_repl_commands(REPLContext* repl, PerformanceMonitor* monitor);
int performance_handle_repl_perf_command(REPLContext* repl, PerformanceMonitor* monitor, const char* command);

// Commands:
// :perf status - Show current performance status
// :perf start - Start performance monitoring
// :perf stop - Stop performance monitoring
// :perf reset - Reset all metrics
// :perf metrics - Show all current metrics
// :perf alerts - Show active alerts
// :perf export [file] - Export current session data
// :perf config [setting] [value] - Configure monitoring settings

#endif // PERFORMANCE_MONITOR_H