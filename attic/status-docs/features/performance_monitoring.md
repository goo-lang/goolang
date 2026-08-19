# Goo Real-time Performance Monitoring

The Goo IDE includes a comprehensive real-time performance monitoring system that tracks compilation performance, memory usage, CPU utilization, and provides live feedback during development.

## Overview

The performance monitoring system provides:

- **Real-time Metrics**: Live tracking of memory, CPU, and compilation performance
- **Smart Alerts**: Automatic detection of performance issues and bottlenecks
- **REPL Integration**: Interactive performance commands and live dashboard
- **Historical Data**: Session-based performance tracking and reporting
- **Hot Reload Integration**: Performance impact analysis of code changes

## Core Features

### 1. Performance Metrics

The system tracks multiple performance dimensions:

#### Memory Metrics
- **Current Memory Usage**: Live memory consumption in MB
- **Peak Memory Usage**: Maximum memory used during session
- **Allocation Count**: Number of memory allocations
- **Deallocation Count**: Number of memory deallocations
- **Memory Leak Detection**: Alerts for potential memory leaks

#### CPU Metrics
- **CPU Time**: Total CPU time consumed (user + system)
- **CPU Usage Percentage**: Real-time CPU utilization
- **Processing Efficiency**: CPU time per operation

#### Compilation Metrics
- **Parse Time**: Time spent parsing source files
- **Type Check Time**: Time spent in type checking phase
- **Code Generation Time**: Time spent generating LLVM IR
- **Total Compilation Time**: End-to-end compilation duration
- **Compilation Count**: Number of files compiled
- **Error Count**: Number of compilation errors encountered

#### I/O Metrics
- **File Operations**: Number of file reads/writes
- **Hot Reload Operations**: File system watching overhead

### 2. Alert System

Intelligent performance alerts with configurable thresholds:

```
Memory Alerts:
- High memory usage (default: 1GB threshold)
- Suspected memory leaks
- Rapid memory growth

CPU Alerts:
- High CPU usage (default: 80% threshold)
- CPU-intensive operations
- Performance degradation

Compilation Alerts:
- Slow compilation (default: 5s threshold)
- Frequent errors (default: 10 errors/min)
- Type checking bottlenecks
```

### 3. REPL Integration

Interactive performance monitoring through REPL commands:

#### Performance Commands

```goo
goo> :perf status
Performance Monitor Status:
  Enabled: Yes
  Recording: Yes
  Sample Interval: 100 ms
  Current Memory: 45.2 MB
  CPU Usage: 12.5%

goo> :perf metrics
Current Performance Metrics:
  Memory Usage: 45.20 MB (min: 32.10, max: 67.80, avg: 41.45)
  CPU Time: 1234.50 ms (min: 0.00, max: 1234.50, avg: 617.25)
  Parse Time: 23.40 ms (min: 12.10, max: 67.20, avg: 31.85)
  Type Check Time: 45.60 ms (min: 23.40, max: 89.70, avg: 52.30)

goo> :perf alerts
Active Performance Alerts:
  [High Memory] Memory usage (512.5 MB) exceeds threshold (500.0 MB)

goo> :perf start
Performance monitoring started.

goo> :perf stop
Performance monitoring stopped.

goo> :perf reset
Performance metrics reset.
```

### 4. Session Management

Performance data is organized into sessions:

- **Automatic Sessions**: New session starts with REPL or compilation
- **Session History**: Track performance across multiple sessions
- **Session Comparison**: Compare performance between sessions
- **Session Export**: Export data for external analysis

### 5. Hot Reload Performance Impact

Monitor the performance impact of hot reload operations:

```goo
goo> :reload
Checking for changed modules...
Hot reload completed successfully (took 234ms, memory impact: +2.3MB)

goo> :perf metrics
Hot Reload Metrics:
  Reload Count: 5
  Average Reload Time: 187ms
  Memory Impact per Reload: +1.8MB
  Files Watched: 23
```

## Configuration

### Monitoring Configuration

```c
// Enable/disable monitoring components
performance_monitor_enable_cpu_monitoring(monitor, true);
performance_monitor_enable_memory_monitoring(monitor, true);
performance_monitor_enable_compilation_monitoring(monitor, true);
performance_monitor_enable_alerts(monitor, true);

// Configure sampling
performance_monitor_set_sample_interval(monitor, PERF_SAMPLE_INTERVAL_100MS);
performance_monitor_set_max_samples(monitor, 1000);
```

### Alert Thresholds

```c
// Memory alert threshold (in MB)
performance_monitor_set_alert_threshold(monitor, PERF_METRIC_MEMORY_USAGE, 1024.0);

// CPU usage alert threshold (in percent)
performance_monitor_set_alert_threshold(monitor, PERF_METRIC_CPU_TIME, 80.0);

// Compilation time alert threshold (in ms)
performance_monitor_set_alert_threshold(monitor, PERF_METRIC_PARSE_TIME, 5000.0);
```

### Sample Intervals

Choose monitoring granularity:

- `PERF_SAMPLE_INTERVAL_1MS` - High precision (1ms intervals)
- `PERF_SAMPLE_INTERVAL_10MS` - Detailed monitoring (10ms intervals)
- `PERF_SAMPLE_INTERVAL_100MS` - Standard monitoring (100ms intervals) - **Default**
- `PERF_SAMPLE_INTERVAL_1S` - Basic monitoring (1 second intervals)
- `PERF_SAMPLE_INTERVAL_5S` - Minimal monitoring (5 second intervals)

## API Reference

### Core Monitor Operations

```c
// Lifecycle
PerformanceMonitor* performance_monitor_new(void);
void performance_monitor_free(PerformanceMonitor* monitor);
int performance_monitor_init(PerformanceMonitor* monitor);

// Recording control
int performance_monitor_start_recording(PerformanceMonitor* monitor);
int performance_monitor_stop_recording(PerformanceMonitor* monitor);
int performance_monitor_pause_recording(PerformanceMonitor* monitor);
int performance_monitor_resume_recording(PerformanceMonitor* monitor);
```

### Metric Recording

```c
// Record custom metrics
int performance_monitor_record_metric(PerformanceMonitor* monitor, 
                                     PerformanceMetricType type, 
                                     double value, 
                                     const char* context);

// Record timing information
int performance_monitor_record_timing(PerformanceMonitor* monitor, 
                                     const char* operation_name,
                                     uint64_t start_time_ms, 
                                     uint64_t end_time_ms);
```

### Memory Tracking

```c
// Track memory usage
int performance_monitor_track_memory_usage(PerformanceMonitor* monitor);
int performance_monitor_track_allocation(PerformanceMonitor* monitor, 
                                        size_t size, 
                                        const char* location);
double performance_monitor_get_current_memory_mb(PerformanceMonitor* monitor);
double performance_monitor_get_peak_memory_mb(PerformanceMonitor* monitor);
```

### CPU Monitoring

```c
// Track CPU usage
int performance_monitor_track_cpu_usage(PerformanceMonitor* monitor);
double performance_monitor_get_cpu_usage_percent(PerformanceMonitor* monitor);
uint64_t performance_monitor_get_cpu_time_ms(PerformanceMonitor* monitor);
```

### Compilation Monitoring

```c
// Track compilation phases
int performance_monitor_track_parse_time(PerformanceMonitor* monitor, 
                                        const char* file_path, 
                                        uint64_t time_ms);
int performance_monitor_track_type_check_time(PerformanceMonitor* monitor, 
                                             const char* file_path, 
                                             uint64_t time_ms);
int performance_monitor_track_codegen_time(PerformanceMonitor* monitor, 
                                          const char* file_path, 
                                          uint64_t time_ms);
```

## Usage Examples

### Basic Performance Monitoring

```c
// Create and initialize monitor
PerformanceMonitor* monitor = performance_monitor_new();
performance_monitor_init(monitor);

// Start monitoring
performance_monitor_start_recording(monitor);

// Your code here
compile_file("example.goo");

// Check current metrics
double memory_mb = performance_monitor_get_current_memory_mb(monitor);
double cpu_percent = performance_monitor_get_cpu_usage_percent(monitor);

printf("Memory: %.1f MB, CPU: %.1f%%\n", memory_mb, cpu_percent);

// Stop monitoring
performance_monitor_stop_recording(monitor);
performance_monitor_free(monitor);
```

### Performance Profiling

```c
// Using automatic profiling macro
{
    PERF_PROFILE(monitor, "type_checking");
    
    // Type checking code here
    type_check_file(file);
    
} // Profiler automatically records timing when scope exits
```

### Custom Metric Recording

```c
// Record custom metrics with context
performance_monitor_record_metric(monitor, 
                                 PERF_METRIC_COMPILATION_COUNT, 
                                 1.0, 
                                 "main.goo");

// Record operation timing
uint64_t start = performance_get_timestamp_ms();
parse_expression(expr);
uint64_t end = performance_get_timestamp_ms();

performance_monitor_record_timing(monitor, "parse_expression", start, end);
```

### Alert Handling

```c
// Check for alerts
performance_monitor_check_alerts(monitor);

// Get active alerts
PerformanceAlert* alerts = performance_monitor_get_active_alerts(monitor);
while (alerts) {
    printf("Alert: %s - %s\n", 
           performance_alert_type_to_string(alerts->type),
           alerts->message);
    alerts = alerts->next;
}
```

## Integration with IDE Components

### Type Checker Integration

```c
// Integrate with type checker for automatic timing
performance_monitor_integrate_type_checker(monitor, type_checker);

// Type checking operations are automatically timed
```

### Hot Reload Integration

```c
// Monitor hot reload performance
performance_monitor_integrate_hot_reload(monitor, hot_reload_context);

// File changes and reloads are automatically tracked
```

### REPL Integration

```c
// Full REPL integration
REPLContext* repl = repl_context_new();
repl_init(repl); // Automatically creates and integrates performance monitor

// Use :perf commands in REPL
```

## Data Export and Reporting

### Report Generation

```c
// Generate summary report
char* summary = performance_monitor_generate_summary_report(monitor);
printf("%s", summary);
free(summary);

// Generate detailed session report
PerformanceSession* session = performance_monitor_get_current_session(monitor);
char* detailed = performance_monitor_generate_detailed_report(monitor, session);
printf("%s", detailed);
free(detailed);
```

### Data Export

```c
// Export session data
performance_monitor_export_session(monitor, session, "performance_data.txt");

// Export in different formats
performance_monitor_export_metrics_json(monitor, "metrics.json");
performance_monitor_export_metrics_csv(monitor, "metrics.csv");
performance_monitor_export_metrics_html(monitor, "dashboard.html");
```

## Performance Best Practices

### 1. Monitoring Overhead

- Use appropriate sample intervals for your use case
- Disable unused monitoring components in production
- Configure reasonable history limits

### 2. Alert Configuration

- Set realistic thresholds based on your system
- Enable alerts only for actionable conditions
- Review and adjust thresholds based on usage patterns

### 3. Memory Management

- Monitor memory usage during development
- Use allocation tracking to identify leaks
- Set memory alerts for early warning

### 4. Compilation Performance

- Track compilation phases separately
- Monitor the impact of type system features
- Use performance data to guide optimization

### 5. Development Workflow

- Use REPL performance commands during development
- Monitor hot reload impact on productivity
- Export performance data for analysis

## Troubleshooting

### Common Issues

1. **High Memory Usage**
   - Check for memory leaks in allocations
   - Review data structure sizes
   - Monitor peak memory during compilation

2. **Slow Compilation**
   - Identify bottleneck phases (parse, typecheck, codegen)
   - Check for complex type inference scenarios
   - Monitor file size vs. compilation time correlation

3. **CPU Performance**
   - Profile CPU-intensive operations
   - Check for infinite loops or recursive algorithms
   - Monitor CPU usage patterns

4. **Missing Metrics**
   - Ensure monitoring is enabled and recording
   - Check that integration components are properly connected
   - Verify sample intervals are appropriate

### Performance Monitoring Self-Diagnostics

```goo
goo> :perf status
Performance Monitor Status:
  Enabled: Yes
  Recording: Yes
  Sample Interval: 100 ms
  Current Memory: 45.2 MB
  CPU Usage: 12.5%
  Active Session: 3
  Total Sessions: 5
  Alerts Active: 0

goo> :perf config
Performance Monitor Configuration:
  CPU Monitoring: Enabled
  Memory Monitoring: Enabled
  Compilation Monitoring: Enabled
  Alerts: Enabled
  Max Samples: 1000
  Memory Alert Threshold: 1024.0 MB
  CPU Alert Threshold: 80.0%
  Compilation Alert Threshold: 5000.0 ms
```

## Architecture

The performance monitoring system consists of:

- **PerformanceMonitor**: Main monitoring controller
- **PerformanceSession**: Session-based data organization
- **PerformanceMetric**: Individual metric tracking
- **PerformanceSample**: Time-series data points
- **PerformanceAlert**: Alert management system
- **PerformanceProfiler**: Automatic profiling utilities

The system integrates seamlessly with:

- Type Checker for compilation performance
- Hot Reload system for development impact
- REPL for interactive monitoring
- File system for data export

## Future Enhancements

- **Visual Dashboard**: Web-based real-time dashboard
- **Performance Regression Detection**: Automatic detection of performance regressions
- **Comparative Analysis**: Compare performance across code changes
- **Advanced Profiling**: Function-level and line-level profiling
- **Performance Suggestions**: Automated optimization recommendations
- **Distributed Monitoring**: Multi-process performance tracking

The performance monitoring system provides comprehensive insights into your Goo development workflow, helping you identify bottlenecks, optimize compilation performance, and maintain productive development velocity.