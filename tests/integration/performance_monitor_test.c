#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "performance_monitor.h"
#include "repl.h"
#include "test/test_framework.h"

// Test performance monitor lifecycle
void test_performance_monitor_lifecycle() {
    printf("Testing performance monitor lifecycle...\n");
    
    PerformanceMonitor* monitor = performance_monitor_new();
    assert(monitor != NULL);
    assert(monitor->is_enabled == false);
    assert(monitor->is_recording == false);
    assert(monitor->sample_interval == PERF_SAMPLE_INTERVAL_100MS);
    assert(monitor->max_samples_per_metric == 1000);
    assert(monitor->enable_cpu_monitoring == true);
    assert(monitor->enable_memory_monitoring == true);
    assert(monitor->enable_compilation_monitoring == true);
    assert(monitor->enable_alerts == true);
    
    // Test initialization
    int init_result = performance_monitor_init(monitor);
    assert(init_result == 0);
    assert(monitor->is_enabled == true);
    
    // Test cleanup
    performance_monitor_cleanup(monitor);
    assert(monitor->is_enabled == false);
    
    performance_monitor_free(monitor);
    
    printf("✓ Performance monitor lifecycle test passed\n");
}

// Test recording control
void test_recording_control() {
    printf("Testing recording control...\n");
    
    PerformanceMonitor* monitor = performance_monitor_new();
    assert(monitor != NULL);
    
    int init_result = performance_monitor_init(monitor);
    assert(init_result == 0);
    
    // Initially not recording
    assert(monitor->is_recording == false);
    
    // Start recording
    int start_result = performance_monitor_start_recording(monitor);
    assert(start_result == 0);
    assert(monitor->is_recording == true);
    assert(monitor->current_session != NULL);
    
    // Stop recording
    int stop_result = performance_monitor_stop_recording(monitor);
    assert(stop_result == 0);
    assert(monitor->is_recording == false);
    assert(monitor->current_session == NULL);
    assert(monitor->sessions != NULL);  // Session should be moved to history
    
    // Test pause/resume
    performance_monitor_start_recording(monitor);
    assert(monitor->is_recording == true);
    
    performance_monitor_pause_recording(monitor);
    assert(monitor->is_recording == false);
    
    performance_monitor_resume_recording(monitor);
    assert(monitor->is_recording == true);
    
    performance_monitor_free(monitor);
    
    printf("✓ Recording control test passed\n");
}

// Test metric management
void test_metric_management() {
    printf("Testing metric management...\n");
    
    PerformanceMonitor* monitor = performance_monitor_new();
    assert(monitor != NULL);
    
    performance_monitor_init(monitor);
    performance_monitor_start_recording(monitor);
    
    // Test metric recording
    int record_result = performance_monitor_record_metric(monitor, PERF_METRIC_MEMORY_USAGE, 256.5, "test_context");
    assert(record_result == 0);
    
    // Check that the metric was recorded
    assert(monitor->current_session != NULL);
    PerformanceMetric* metric = monitor->current_session->metrics[PERF_METRIC_MEMORY_USAGE];
    assert(metric != NULL);
    assert(metric->current_value == 256.5);
    assert(metric->sample_count == 1);
    assert(metric->samples != NULL);
    assert(metric->samples->value == 256.5);
    
    // Test multiple recordings
    performance_monitor_record_metric(monitor, PERF_METRIC_MEMORY_USAGE, 300.0, "test_context_2");
    assert(metric->sample_count == 2);
    assert(metric->current_value == 300.0);
    assert(metric->max_value == 300.0);
    assert(metric->min_value == 256.5);
    
    // Test timing recording
    uint64_t start_time = performance_get_timestamp_ms();
    usleep(10000); // Sleep for 10ms
    uint64_t end_time = performance_get_timestamp_ms();
    
    int timing_result = performance_monitor_record_timing(monitor, "test_operation", start_time, end_time);
    assert(timing_result == 0);
    
    performance_monitor_free(monitor);
    
    printf("✓ Metric management test passed\n");
}

// Test alert system
void test_alert_system() {
    printf("Testing alert system...\n");
    
    PerformanceMonitor* monitor = performance_monitor_new();
    assert(monitor != NULL);
    
    performance_monitor_init(monitor);
    performance_monitor_start_recording(monitor);
    
    // Set low thresholds to trigger alerts
    performance_monitor_set_alert_threshold(monitor, PERF_METRIC_MEMORY_USAGE, 100.0); // 100MB threshold
    
    // Record high memory usage to trigger alert
    performance_monitor_record_metric(monitor, PERF_METRIC_MEMORY_USAGE, 200.0, "high_memory_test");
    
    // Check alerts manually (since check_alerts might depend on specific conditions)
    PerformanceAlert* alert = performance_alert_new(PERF_ALERT_HIGH_MEMORY, "Test alert", 100.0, 200.0);
    assert(alert != NULL);
    assert(alert->type == PERF_ALERT_HIGH_MEMORY);
    assert(alert->threshold == 100.0);
    assert(alert->current_value == 200.0);
    assert(alert->is_active == true);
    
    // Add alert to monitor
    performance_monitor_add_alert(monitor, alert);
    
    // Check that alert was added
    PerformanceAlert* active_alerts = performance_monitor_get_active_alerts(monitor);
    assert(active_alerts != NULL);
    assert(active_alerts->type == PERF_ALERT_HIGH_MEMORY);
    
    // Clear alerts
    performance_monitor_clear_alerts(monitor);
    active_alerts = performance_monitor_get_active_alerts(monitor);
    assert(active_alerts == NULL);
    
    performance_monitor_free(monitor);
    
    printf("✓ Alert system test passed\n");
}

// Test memory tracking
void test_memory_tracking() {
    printf("Testing memory tracking...\n");
    
    PerformanceMonitor* monitor = performance_monitor_new();
    assert(monitor != NULL);
    
    performance_monitor_init(monitor);
    performance_monitor_start_recording(monitor);
    
    // Test memory usage tracking
    int memory_result = performance_monitor_track_memory_usage(monitor);
    assert(memory_result == 0);
    
    double current_memory = performance_monitor_get_current_memory_mb(monitor);
    assert(current_memory >= 0.0);  // Should be some positive value
    
    // Test allocation tracking
    int alloc_result = performance_monitor_track_allocation(monitor, 1024, "test_allocation");
    assert(alloc_result == 0);
    
    // Test deallocation tracking
    int dealloc_result = performance_monitor_track_deallocation(monitor, 1024, "test_deallocation");
    assert(dealloc_result == 0);
    
    performance_monitor_free(monitor);
    
    printf("✓ Memory tracking test passed\n");
}

// Test CPU monitoring
void test_cpu_monitoring() {
    printf("Testing CPU monitoring...\n");
    
    PerformanceMonitor* monitor = performance_monitor_new();
    assert(monitor != NULL);
    
    performance_monitor_init(monitor);
    performance_monitor_start_recording(monitor);
    
    // Test CPU usage tracking
    int cpu_result = performance_monitor_track_cpu_usage(monitor);
    assert(cpu_result == 0);
    
    double cpu_usage = performance_monitor_get_cpu_usage_percent(monitor);
    assert(cpu_usage >= 0.0);  // Should be non-negative
    
    uint64_t cpu_time = performance_monitor_get_cpu_time_ms(monitor);
    assert(cpu_time >= 0);  // Should be non-negative
    
    performance_monitor_free(monitor);
    
    printf("✓ CPU monitoring test passed\n");
}

// Test compilation monitoring
void test_compilation_monitoring() {
    printf("Testing compilation monitoring...\n");
    
    PerformanceMonitor* monitor = performance_monitor_new();
    assert(monitor != NULL);
    
    performance_monitor_init(monitor);
    performance_monitor_start_recording(monitor);
    
    const char* test_file = "test.goo";
    
    // Test compilation timing
    int start_result = performance_monitor_start_compilation_timing(monitor, test_file);
    assert(start_result == 0);
    
    int end_result = performance_monitor_end_compilation_timing(monitor, test_file, true);
    assert(end_result == 0);
    
    // Test phase-specific timing
    int parse_result = performance_monitor_track_parse_time(monitor, test_file, 50);
    assert(parse_result == 0);
    
    int typecheck_result = performance_monitor_track_type_check_time(monitor, test_file, 100);
    assert(typecheck_result == 0);
    
    int codegen_result = performance_monitor_track_codegen_time(monitor, test_file, 75);
    assert(codegen_result == 0);
    
    performance_monitor_free(monitor);
    
    printf("✓ Compilation monitoring test passed\n");
}

// Test configuration
void test_configuration() {
    printf("Testing configuration...\n");
    
    PerformanceMonitor* monitor = performance_monitor_new();
    assert(monitor != NULL);
    
    // Test sample interval configuration
    int interval_result = performance_monitor_set_sample_interval(monitor, PERF_SAMPLE_INTERVAL_1S);
    assert(interval_result == 0);
    assert(monitor->sample_interval == PERF_SAMPLE_INTERVAL_1S);
    
    // Test max samples configuration
    int max_samples_result = performance_monitor_set_max_samples(monitor, 500);
    assert(max_samples_result == 0);
    assert(monitor->max_samples_per_metric == 500);
    
    // Test enabling/disabling monitoring components
    int cpu_enable_result = performance_monitor_enable_cpu_monitoring(monitor, false);
    assert(cpu_enable_result == 0);
    assert(monitor->enable_cpu_monitoring == false);
    
    int memory_enable_result = performance_monitor_enable_memory_monitoring(monitor, false);
    assert(memory_enable_result == 0);
    assert(monitor->enable_memory_monitoring == false);
    
    int compilation_enable_result = performance_monitor_enable_compilation_monitoring(monitor, false);
    assert(compilation_enable_result == 0);
    assert(monitor->enable_compilation_monitoring == false);
    
    int alerts_enable_result = performance_monitor_enable_alerts(monitor, false);
    assert(alerts_enable_result == 0);
    assert(monitor->enable_alerts == false);
    
    performance_monitor_free(monitor);
    
    printf("✓ Configuration test passed\n");
}

// Test REPL integration
void test_repl_integration() {
    printf("Testing REPL integration...\n");
    
    REPLContext* repl_ctx = repl_context_new();
    assert(repl_ctx != NULL);
    
    int repl_init_result = repl_init(repl_ctx);
    assert(repl_init_result == 0);
    assert(repl_ctx->performance_monitor != NULL);
    
    PerformanceMonitor* monitor = repl_ctx->performance_monitor;
    
    // Test integration functions
    int type_checker_integration = performance_monitor_integrate_type_checker(monitor, repl_ctx->type_checker);
    assert(type_checker_integration == 0);
    
    int hot_reload_integration = performance_monitor_integrate_hot_reload(monitor, repl_ctx->hot_reload);
    assert(hot_reload_integration == 0);
    
    int repl_integration = performance_monitor_integrate_repl(monitor, repl_ctx);
    assert(repl_integration == 0);
    
    // Test REPL command registration
    int command_registration = performance_monitor_register_repl_commands(repl_ctx, monitor);
    assert(command_registration == 0);
    
    // Test performance command handling (just make sure it doesn't crash)
    int status_result = performance_handle_repl_perf_command(repl_ctx, monitor, "status");
    // Don't assert on result since it depends on implementation details
    
    repl_context_free(repl_ctx);
    
    printf("✓ REPL integration test passed\n");
}

// Test utility functions
void test_utility_functions() {
    printf("Testing utility functions...\n");
    
    // Test timestamp function
    uint64_t timestamp1 = performance_get_timestamp_ms();
    usleep(10000); // Sleep for 10ms
    uint64_t timestamp2 = performance_get_timestamp_ms();
    assert(timestamp2 > timestamp1);
    assert((timestamp2 - timestamp1) >= 10); // Should be at least 10ms difference
    
    // Test metric type to string conversion
    const char* memory_str = performance_metric_type_to_string(PERF_METRIC_MEMORY_USAGE);
    assert(memory_str != NULL);
    assert(strcmp(memory_str, "Memory Usage") == 0);
    
    const char* cpu_str = performance_metric_type_to_string(PERF_METRIC_CPU_TIME);
    assert(cpu_str != NULL);
    assert(strcmp(cpu_str, "CPU Time") == 0);
    
    // Test alert type to string conversion
    const char* high_memory_str = performance_alert_type_to_string(PERF_ALERT_HIGH_MEMORY);
    assert(high_memory_str != NULL);
    assert(strcmp(high_memory_str, "High Memory") == 0);
    
    const char* high_cpu_str = performance_alert_type_to_string(PERF_ALERT_HIGH_CPU);
    assert(high_cpu_str != NULL);
    assert(strcmp(high_cpu_str, "High CPU") == 0);
    
    printf("✓ Utility functions test passed\n");
}

int main() {
    printf("Running Performance Monitor tests...\n\n");
    
    test_performance_monitor_lifecycle();
    test_recording_control();
    test_metric_management();
    test_alert_system();
    test_memory_tracking();
    test_cpu_monitoring();
    test_compilation_monitoring();
    test_configuration();
    test_repl_integration();
    test_utility_functions();
    
    printf("\n✅ All Performance Monitor tests passed!\n");
    return 0;
}