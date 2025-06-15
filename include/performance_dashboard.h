#ifndef PERFORMANCE_DASHBOARD_H
#define PERFORMANCE_DASHBOARD_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Performance visualization dashboard for real-time monitoring
// Provides web-based and terminal-based visualization interfaces

// Chart types for performance data visualization
typedef enum {
    CHART_LINE,           // Time-series line charts
    CHART_BAR,            // Bar charts for comparisons
    CHART_AREA,           // Filled area charts
    CHART_SCATTER,        // Scatter plots for correlations
    CHART_HISTOGRAM,      // Distribution histograms
    CHART_HEATMAP,        // Heat maps for 2D data
    CHART_GAUGE,          // Gauge charts for current values
    CHART_DONUT           // Donut charts for proportions
} ChartType;

// Data point for time-series visualization
typedef struct {
    uint64_t timestamp;   // Unix timestamp in milliseconds
    double value;         // Metric value
    char* label;          // Optional label
} DataPoint;

// Chart configuration
typedef struct {
    char* id;             // Unique chart identifier
    char* title;          // Chart title
    char* x_axis_label;   // X-axis label
    char* y_axis_label;   // Y-axis label
    ChartType type;       // Chart type
    int max_points;       // Maximum data points to keep
    bool auto_scale;      // Auto-scale axes
    double min_y;         // Manual Y-axis minimum
    double max_y;         // Manual Y-axis maximum
    char* color;          // Primary color
    bool show_legend;     // Show legend
    bool show_grid;       // Show grid lines
    int refresh_interval; // Refresh interval in ms
} ChartConfig;

// Performance metric for dashboard
typedef struct {
    char* name;           // Metric name
    char* description;    // Metric description
    char* unit;           // Unit of measurement
    DataPoint* data;      // Time-series data points
    int data_count;       // Number of data points
    int max_data_points;  // Maximum data points to keep
    double current_value; // Current/latest value
    double min_value;     // Historical minimum
    double max_value;     // Historical maximum
    double avg_value;     // Historical average
    ChartConfig* chart;   // Associated chart configuration
} PerformanceMetric;

// Dashboard widget configuration
typedef struct {
    char* id;             // Widget ID
    char* title;          // Widget title
    char* type;           // Widget type (chart, gauge, table, etc.)
    int width;            // Widget width (grid units)
    int height;           // Widget height (grid units)
    int x_position;       // X position on dashboard
    int y_position;       // Y position on dashboard
    char* metric_ids[10]; // Associated metric IDs
    int metric_count;     // Number of associated metrics
    char* config_json;    // Widget-specific configuration
} DashboardWidget;

// Dashboard layout configuration
typedef struct {
    char* id;             // Dashboard ID
    char* title;          // Dashboard title
    char* description;    // Dashboard description
    int grid_columns;     // Grid columns for layout
    int grid_rows;        // Grid rows for layout
    DashboardWidget* widgets; // Dashboard widgets
    int widget_count;     // Number of widgets
    int max_widgets;      // Maximum widgets allowed
    int refresh_interval; // Global refresh interval in ms
    bool auto_refresh;    // Auto-refresh enabled
} DashboardLayout;

// Performance dashboard server
typedef struct {
    bool running;         // Server running state
    int port;             // HTTP server port
    char* host;           // Server host
    FILE* log_file;       // Log file handle
    
    // Metrics storage
    PerformanceMetric* metrics;
    int metric_count;
    int max_metrics;
    
    // Dashboard layouts
    DashboardLayout* dashboards;
    int dashboard_count;
    int max_dashboards;
    
    // Server configuration
    char* static_path;    // Static files path
    char* template_path;  // Template files path
    bool enable_websocket; // WebSocket support
    bool enable_cors;     // CORS support
    int max_connections;  // Maximum concurrent connections
    
    // Data retention
    int data_retention_hours; // Hours to keep data
    bool enable_persistence;  // Persist data to disk
    char* data_directory;     // Data storage directory
} PerformanceDashboard;

// Core dashboard functions
bool performance_dashboard_init(int port, const char* host);
void performance_dashboard_run(void);
void performance_dashboard_shutdown(void);

// Metric management
int dashboard_add_metric(const char* name, const char* description, const char* unit);
bool dashboard_update_metric(const char* name, double value);
bool dashboard_update_metric_with_timestamp(const char* name, double value, uint64_t timestamp);
PerformanceMetric* dashboard_get_metric(const char* name);
void dashboard_remove_metric(const char* name);

// Chart configuration
ChartConfig* dashboard_create_chart(const char* id, const char* title, ChartType type);
bool dashboard_configure_chart(const char* chart_id, const ChartConfig* config);
void dashboard_destroy_chart(ChartConfig* chart);

// Widget management
int dashboard_add_widget(const char* dashboard_id, const char* widget_id, 
                        const char* title, const char* type);
bool dashboard_configure_widget(const char* dashboard_id, const char* widget_id, 
                               const DashboardWidget* config);
void dashboard_remove_widget(const char* dashboard_id, const char* widget_id);

// Dashboard layout management
int dashboard_create_layout(const char* id, const char* title, int columns, int rows);
bool dashboard_set_layout_config(const char* dashboard_id, const DashboardLayout* config);
DashboardLayout* dashboard_get_layout(const char* dashboard_id);
void dashboard_remove_layout(const char* dashboard_id);

// Data export and import
bool dashboard_export_data(const char* filename, const char* format);
bool dashboard_import_data(const char* filename);
bool dashboard_export_layout(const char* dashboard_id, const char* filename);
bool dashboard_import_layout(const char* filename);

// Real-time data streaming
bool dashboard_enable_streaming(const char* metric_name);
void dashboard_disable_streaming(const char* metric_name);
void dashboard_broadcast_update(const char* metric_name, double value);

// Terminal-based visualization
void dashboard_show_terminal_view(void);
void dashboard_terminal_chart(const char* metric_name, int width, int height);
void dashboard_terminal_gauge(const char* metric_name, double min_val, double max_val);
void dashboard_terminal_table(const char** metric_names, int count);

// Web interface generation
char* dashboard_generate_html(const char* dashboard_id);
char* dashboard_generate_json_data(const char* metric_name);
char* dashboard_generate_chart_config(const char* chart_id);

// Performance monitoring integration
void dashboard_integrate_performance_monitor(void);
void dashboard_register_goo_metrics(void);
void dashboard_update_from_monitor(void);

// Alerting and notifications
typedef struct {
    char* metric_name;
    double threshold;
    bool above_threshold; // true for above, false for below
    char* message;
    bool enabled;
    uint64_t last_triggered;
    int cooldown_seconds;
} PerformanceAlert;

int dashboard_add_alert(const char* metric_name, double threshold, 
                       bool above, const char* message);
void dashboard_check_alerts(void);
void dashboard_trigger_alert(PerformanceAlert* alert, double current_value);

// Utility functions
uint64_t dashboard_current_timestamp(void);
double dashboard_calculate_average(DataPoint* points, int count);
double dashboard_calculate_percentile(DataPoint* points, int count, double percentile);
void dashboard_cleanup_old_data(void);

// Predefined dashboard templates
void dashboard_create_system_overview(void);
void dashboard_create_memory_dashboard(void);
void dashboard_create_cpu_dashboard(void);
void dashboard_create_goo_language_dashboard(void);
void dashboard_create_repl_dashboard(void);

// HTTP server utilities
typedef struct {
    char* method;
    char* path;
    char* query_string;
    char* body;
    char* headers[32];
    int header_count;
} HTTPRequest;

typedef struct {
    int status_code;
    char* content_type;
    char* body;
    char* headers[32];
    int header_count;
} HTTPResponse;

// HTTP request handlers
void dashboard_handle_index(HTTPRequest* req, HTTPResponse* resp);
void dashboard_handle_api_metrics(HTTPRequest* req, HTTPResponse* resp);
void dashboard_handle_api_dashboard(HTTPRequest* req, HTTPResponse* resp);
void dashboard_handle_websocket(HTTPRequest* req, HTTPResponse* resp);
void dashboard_handle_static(HTTPRequest* req, HTTPResponse* resp);

// WebSocket support
void dashboard_websocket_broadcast(const char* message);
void dashboard_websocket_send_metric_update(const char* metric_name, double value);

#endif // PERFORMANCE_DASHBOARD_H