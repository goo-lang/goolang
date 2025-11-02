#ifndef PROFILING_VISUALIZATION_H
#define PROFILING_VISUALIZATION_H

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct PerformanceMonitor PerformanceMonitor;
typedef struct REPLContext REPLContext;

// =============================================================================
// Type Definitions
// =============================================================================

// Visual chart types
typedef enum {
    CHART_LINE,
    CHART_BAR,
    CHART_AREA,
    CHART_SCATTER,
    CHART_HEATMAP,
    CHART_FLAME_GRAPH
} ChartType;

// Visual metric data point
typedef struct MetricDataPoint {
    uint64_t timestamp_ms;
    double value;
    char* label;
    char* context;
    struct MetricDataPoint* next;
} MetricDataPoint;

// Visual chart configuration
typedef struct ChartConfig {
    ChartType type;
    char* title;
    char* x_label;
    char* y_label;
    char* color_scheme;
    int width;
    int height;
    double min_value;
    double max_value;
    bool auto_scale;
    bool show_grid;
    bool show_legend;
    int max_data_points;
} ChartConfig;

// Visual chart instance
typedef struct VisualizationChart {
    char* chart_id;
    ChartConfig config;
    MetricDataPoint* data_points;
    int data_count;
    uint64_t last_update_ms;
    bool is_real_time;
    struct VisualizationChart* next;
} VisualizationChart;

// Profiling visualization context
typedef struct ProfilingVisualization {
    bool is_enabled;
    bool real_time_mode;
    int refresh_interval_ms;
    char* output_directory;
    
    // Chart management
    VisualizationChart* charts;
    int chart_count;
    
    // Color configuration
    bool use_colors;
    char* color_palette[8];
    
    // Performance integration
    PerformanceMonitor* monitor;
    REPLContext* repl;
    
    // Output formats
    bool generate_html;
    bool generate_svg;
    bool generate_ascii;
    bool live_terminal_display;
    
} ProfilingVisualization;

// =============================================================================
// Core Functions
// =============================================================================

// Lifecycle management
ProfilingVisualization* profiling_visualization_new(void);
void profiling_visualization_free(ProfilingVisualization* viz);
int profiling_visualization_init(ProfilingVisualization* viz);

// Integration
int profiling_visualization_integrate_monitor(ProfilingVisualization* viz, PerformanceMonitor* monitor);
int profiling_visualization_integrate_repl(ProfilingVisualization* viz, REPLContext* repl);

// Chart management
VisualizationChart* profiling_visualization_create_chart(ProfilingVisualization* viz, 
                                                        const char* chart_id, 
                                                        ChartType type, 
                                                        const char* title);
void profiling_visualization_free_chart(VisualizationChart* chart);
VisualizationChart* profiling_visualization_get_chart(ProfilingVisualization* viz, const char* chart_id);

// Data management
int profiling_visualization_add_data_point(VisualizationChart* chart, 
                                          double value, 
                                          const char* label, 
                                          const char* context);

// Rendering functions
void profiling_visualization_render_ascii_sparkline(ProfilingVisualization* viz, VisualizationChart* chart);
void profiling_visualization_render_ascii_bar_chart(ProfilingVisualization* viz, VisualizationChart* chart);
void profiling_visualization_render_ascii_line_chart(ProfilingVisualization* viz, VisualizationChart* chart);
void profiling_visualization_render_live_dashboard(ProfilingVisualization* viz);

// Report generation
int profiling_visualization_generate_html_report(ProfilingVisualization* viz);

// Integration functions
int profiling_visualization_update_from_monitor(ProfilingVisualization* viz, PerformanceMonitor* monitor);
int profiling_visualization_handle_repl_command(ProfilingVisualization* viz, REPLContext* repl, const char* command);

// Demo and testing
void profiling_visualization_demo(ProfilingVisualization* viz);

#endif // PROFILING_VISUALIZATION_H