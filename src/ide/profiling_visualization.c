// Profiling Visualization - Real-time performance visualization in the editor
// Provides visual profiling data integration with the Goo development environment

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/stat.h>

// Forward declarations for integration
typedef struct PerformanceMonitor PerformanceMonitor;
typedef struct REPLContext REPLContext;

// =============================================================================
// Data Structures
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
// Color Schemes and Terminal Graphics
// =============================================================================

// ANSI color codes for terminal output
#define VIZ_COLOR_RESET   "\033[0m"
#define VIZ_COLOR_BOLD    "\033[1m"
#define VIZ_COLOR_RED     "\033[31m"
#define VIZ_COLOR_GREEN   "\033[32m"
#define VIZ_COLOR_YELLOW  "\033[33m"
#define VIZ_COLOR_BLUE    "\033[34m"
#define VIZ_COLOR_MAGENTA "\033[35m"
#define VIZ_COLOR_CYAN    "\033[36m"
#define VIZ_COLOR_WHITE   "\033[37m"

// Chart symbols for ASCII visualization
static const char* bar_chars[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
static const char* spark_chars[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
static const char* dot_chars[] = {"⋅", "•", "●", "◉"};

// Forward declarations for internal functions
void profiling_visualization_free_chart(VisualizationChart* chart);

// =============================================================================
// Utility Functions
// =============================================================================

static void create_directory_recursive(const char* path) {
    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
    }
    mkdir(tmp, S_IRWXU);
}

static char* duplicate_string(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

static uint64_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void print_colored_viz(ProfilingVisualization* viz, const char* color, const char* format, ...) {
    if (viz->use_colors) {
        printf("%s", color);
    }
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    if (viz->use_colors) {
        printf("%s", VIZ_COLOR_RESET);
    }
}

// =============================================================================
// Profiling Visualization Lifecycle
// =============================================================================

ProfilingVisualization* profiling_visualization_new(void) {
    ProfilingVisualization* viz = calloc(1, sizeof(ProfilingVisualization));
    if (!viz) return NULL;
    
    // Initialize default configuration
    viz->is_enabled = true;
    viz->real_time_mode = false;
    viz->refresh_interval_ms = 1000; // 1 second
    viz->output_directory = duplicate_string("./profiling_visualizations");
    
    // Default output formats
    viz->generate_html = true;
    viz->generate_svg = false;
    viz->generate_ascii = true;
    viz->live_terminal_display = true;
    viz->use_colors = true;
    
    // Initialize color palette
    viz->color_palette[0] = duplicate_string(VIZ_COLOR_RED);
    viz->color_palette[1] = duplicate_string(VIZ_COLOR_GREEN);
    viz->color_palette[2] = duplicate_string(VIZ_COLOR_BLUE);
    viz->color_palette[3] = duplicate_string(VIZ_COLOR_YELLOW);
    viz->color_palette[4] = duplicate_string(VIZ_COLOR_MAGENTA);
    viz->color_palette[5] = duplicate_string(VIZ_COLOR_CYAN);
    viz->color_palette[6] = duplicate_string(VIZ_COLOR_WHITE);
    viz->color_palette[7] = duplicate_string(VIZ_COLOR_BOLD);
    
    return viz;
}

void profiling_visualization_free(ProfilingVisualization* viz) {
    if (!viz) return;
    
    // Free charts
    VisualizationChart* chart = viz->charts;
    while (chart) {
        VisualizationChart* next = chart->next;
        profiling_visualization_free_chart(chart);
        chart = next;
    }
    
    // Free strings
    free(viz->output_directory);
    for (int i = 0; i < 8; i++) {
        free(viz->color_palette[i]);
    }
    
    free(viz);
}

int profiling_visualization_init(ProfilingVisualization* viz) {
    if (!viz) return -1;
    
    // Create output directory
    if (viz->output_directory) {
        create_directory_recursive(viz->output_directory);
    }
    
    viz->is_enabled = true;
    return 0;
}

int profiling_visualization_integrate_monitor(ProfilingVisualization* viz, PerformanceMonitor* monitor) {
    if (!viz) return -1;
    viz->monitor = monitor;
    return 0;
}

int profiling_visualization_integrate_repl(ProfilingVisualization* viz, REPLContext* repl) {
    if (!viz) return -1;
    viz->repl = repl;
    return 0;
}

// =============================================================================
// Chart Management
// =============================================================================

VisualizationChart* profiling_visualization_create_chart(ProfilingVisualization* viz, 
                                                        const char* chart_id, 
                                                        ChartType type, 
                                                        const char* title) {
    if (!viz || !chart_id) return NULL;
    
    VisualizationChart* chart = calloc(1, sizeof(VisualizationChart));
    if (!chart) return NULL;
    
    chart->chart_id = duplicate_string(chart_id);
    chart->config.type = type;
    chart->config.title = duplicate_string(title);
    chart->config.x_label = duplicate_string("Time");
    chart->config.y_label = duplicate_string("Value");
    chart->config.color_scheme = duplicate_string("default");
    chart->config.width = 80;
    chart->config.height = 20;
    chart->config.auto_scale = true;
    chart->config.show_grid = true;
    chart->config.show_legend = true;
    chart->config.max_data_points = 100;
    chart->is_real_time = true;
    
    // Add to visualization
    chart->next = viz->charts;
    viz->charts = chart;
    viz->chart_count++;
    
    return chart;
}

void profiling_visualization_free_chart(VisualizationChart* chart) {
    if (!chart) return;
    
    // Free data points
    MetricDataPoint* point = chart->data_points;
    while (point) {
        MetricDataPoint* next = point->next;
        free(point->label);
        free(point->context);
        free(point);
        point = next;
    }
    
    // Free chart config
    free(chart->chart_id);
    free(chart->config.title);
    free(chart->config.x_label);
    free(chart->config.y_label);
    free(chart->config.color_scheme);
    
    free(chart);
}

VisualizationChart* profiling_visualization_get_chart(ProfilingVisualization* viz, const char* chart_id) {
    if (!viz || !chart_id) return NULL;
    
    VisualizationChart* chart = viz->charts;
    while (chart) {
        if (strcmp(chart->chart_id, chart_id) == 0) {
            return chart;
        }
        chart = chart->next;
    }
    
    return NULL;
}

// =============================================================================
// Data Point Management
// =============================================================================

int profiling_visualization_add_data_point(VisualizationChart* chart, 
                                          double value, 
                                          const char* label, 
                                          const char* context) {
    if (!chart) return -1;
    
    MetricDataPoint* point = calloc(1, sizeof(MetricDataPoint));
    if (!point) return -1;
    
    point->timestamp_ms = get_timestamp_ms();
    point->value = value;
    point->label = label ? duplicate_string(label) : NULL;
    point->context = context ? duplicate_string(context) : NULL;
    
    // Add to front of list
    point->next = chart->data_points;
    chart->data_points = point;
    chart->data_count++;
    
    // Limit data points
    if (chart->data_count > chart->config.max_data_points) {
        MetricDataPoint* current = chart->data_points;
        for (int i = 0; i < chart->config.max_data_points - 1 && current; i++) {
            current = current->next;
        }
        
        if (current && current->next) {
            MetricDataPoint* to_remove = current->next;
            current->next = NULL;
            
            // Free removed points
            while (to_remove) {
                MetricDataPoint* next = to_remove->next;
                free(to_remove->label);
                free(to_remove->context);
                free(to_remove);
                to_remove = next;
                chart->data_count--;
            }
        }
    }
    
    chart->last_update_ms = point->timestamp_ms;
    return 0;
}

// =============================================================================
// ASCII Chart Rendering
// =============================================================================

void profiling_visualization_render_ascii_sparkline(ProfilingVisualization* viz, 
                                                   VisualizationChart* chart) {
    if (!viz || !chart) return;
    
    print_colored_viz(viz, VIZ_COLOR_BOLD, "📊 %s ", chart->config.title);
    
    if (chart->data_count == 0) {
        printf("(no data)\n");
        return;
    }
    
    // Calculate value range
    double min_val = chart->data_points->value;
    double max_val = chart->data_points->value;
    
    MetricDataPoint* point = chart->data_points;
    while (point) {
        if (point->value < min_val) min_val = point->value;
        if (point->value > max_val) max_val = point->value;
        point = point->next;
    }
    
    if (max_val == min_val) max_val = min_val + 1; // Avoid division by zero
    
    // Render sparkline (reversed order to show chronologically)
    MetricDataPoint* points[100]; // Temporary array for ordering
    int count = 0;
    point = chart->data_points;
    while (point && count < 100) {
        points[count++] = point;
        point = point->next;
    }
    
    // Print in reverse order (oldest to newest)
    for (int i = count - 1; i >= 0; i--) {
        double normalized = (points[i]->value - min_val) / (max_val - min_val);
        int index = (int)(normalized * 7);
        if (index > 7) index = 7;
        printf("%s", spark_chars[index]);
    }
    
    printf(" ");
    print_colored_viz(viz, VIZ_COLOR_GREEN, "(%.2f)", points[0]->value);
    if (chart->config.y_label) {
        print_colored_viz(viz, VIZ_COLOR_CYAN, " %s", chart->config.y_label);
    }
    printf("\n");
}

void profiling_visualization_render_ascii_bar_chart(ProfilingVisualization* viz, 
                                                   VisualizationChart* chart) {
    if (!viz || !chart || chart->data_count == 0) return;
    
    print_colored_viz(viz, VIZ_COLOR_BOLD VIZ_COLOR_CYAN, "\n📊 %s\n", chart->config.title);
    print_colored_viz(viz, VIZ_COLOR_CYAN, "═");
    for (int i = 0; i < strlen(chart->config.title) + 2; i++) {
        printf("═");
    }
    printf("\n");
    
    // Calculate range
    double min_val = chart->data_points->value;
    double max_val = chart->data_points->value;
    
    MetricDataPoint* point = chart->data_points;
    while (point) {
        if (point->value < min_val) min_val = point->value;
        if (point->value > max_val) max_val = point->value;
        point = point->next;
    }
    
    if (max_val == min_val) max_val = min_val + 1;
    
    // Show recent data points (limit to screen width)
    int max_bars = 20;
    MetricDataPoint* points[20];
    int count = 0;
    point = chart->data_points;
    while (point && count < max_bars) {
        points[count++] = point;
        point = point->next;
    }
    
    // Render bars
    for (int i = count - 1; i >= 0; i--) {
        double normalized = (points[i]->value - min_val) / (max_val - min_val);
        int bar_height = (int)(normalized * chart->config.height);
        
        printf("│");
        for (int h = 0; h < bar_height; h++) {
            print_colored_viz(viz, viz->color_palette[i % 8], "█");
        }
        
        printf(" ");
        print_colored_viz(viz, VIZ_COLOR_WHITE, "%.1f", points[i]->value);
        if (points[i]->label) {
            print_colored_viz(viz, VIZ_COLOR_YELLOW, " (%s)", points[i]->label);
        }
        printf("\n");
    }
    
    printf("└");
    for (int i = 0; i < chart->config.width; i++) {
        printf("─");
    }
    printf("\n");
    
    print_colored_viz(viz, VIZ_COLOR_CYAN, " Range: %.2f - %.2f", min_val, max_val);
    if (chart->config.y_label) {
        printf(" %s", chart->config.y_label);
    }
    printf("\n\n");
}

void profiling_visualization_render_ascii_line_chart(ProfilingVisualization* viz, 
                                                    VisualizationChart* chart) {
    if (!viz || !chart || chart->data_count == 0) return;
    
    print_colored_viz(viz, VIZ_COLOR_BOLD VIZ_COLOR_MAGENTA, "\n📈 %s\n", chart->config.title);
    
    // Simplified line chart using ASCII characters
    const int height = 15;
    const int width = 60;
    char grid[height][width + 1];
    
    // Initialize grid
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            grid[y][x] = ' ';
        }
        grid[y][width] = '\0';
    }
    
    // Calculate range
    double min_val = chart->data_points->value;
    double max_val = chart->data_points->value;
    
    MetricDataPoint* point = chart->data_points;
    while (point) {
        if (point->value < min_val) min_val = point->value;
        if (point->value > max_val) max_val = point->value;
        point = point->next;
    }
    
    if (max_val == min_val) max_val = min_val + 1;
    
    // Plot points
    MetricDataPoint* points[60];
    int count = 0;
    point = chart->data_points;
    while (point && count < width) {
        points[count++] = point;
        point = point->next;
    }
    
    // Draw line
    for (int i = count - 1; i >= 0; i--) {
        int x = width - count + i;
        double normalized = (points[i]->value - min_val) / (max_val - min_val);
        int y = height - 1 - (int)(normalized * (height - 1));
        
        if (x >= 0 && x < width && y >= 0 && y < height) {
            grid[y][x] = '*';
        }
    }
    
    // Add grid lines if enabled
    if (chart->config.show_grid) {
        for (int y = 0; y < height; y += 5) {
            for (int x = 0; x < width; x += 10) {
                if (grid[y][x] == ' ') {
                    grid[y][x] = '.';
                }
            }
        }
    }
    
    // Render grid
    print_colored_viz(viz, VIZ_COLOR_CYAN, "┌");
    for (int x = 0; x < width; x++) {
        printf("─");
    }
    printf("┐\n");
    
    for (int y = 0; y < height; y++) {
        print_colored_viz(viz, VIZ_COLOR_CYAN, "│");
        for (int x = 0; x < width; x++) {
            if (grid[y][x] == '*') {
                print_colored_viz(viz, VIZ_COLOR_GREEN, "*");
            } else if (grid[y][x] == '.') {
                print_colored_viz(viz, VIZ_COLOR_BLUE, ".");
            } else {
                printf(" ");
            }
        }
        print_colored_viz(viz, VIZ_COLOR_CYAN, "│");
        
        // Y-axis labels
        if (y == 0) {
            print_colored_viz(viz, VIZ_COLOR_YELLOW, " %.2f", max_val);
        } else if (y == height - 1) {
            print_colored_viz(viz, VIZ_COLOR_YELLOW, " %.2f", min_val);
        }
        printf("\n");
    }
    
    print_colored_viz(viz, VIZ_COLOR_CYAN, "└");
    for (int x = 0; x < width; x++) {
        printf("─");
    }
    printf("┘\n");
    
    // X-axis label
    printf("  ");
    print_colored_viz(viz, VIZ_COLOR_YELLOW, "%s", 
                     chart->config.x_label ? chart->config.x_label : "Time");
    printf("\n\n");
}

// =============================================================================
// Real-time Monitoring Dashboard
// =============================================================================

void profiling_visualization_render_live_dashboard(ProfilingVisualization* viz) {
    if (!viz || !viz->live_terminal_display) return;
    
    // Clear screen for live updates
    printf("\033[2J\033[H");
    
    print_colored_viz(viz, VIZ_COLOR_BOLD VIZ_COLOR_CYAN, 
                     "🎯 Goo Profiling Visualization Dashboard\n");
    print_colored_viz(viz, VIZ_COLOR_CYAN, 
                     "═══════════════════════════════════════\n");
    
    uint64_t current_time = get_timestamp_ms();
    printf("📅 Last Update: %lu ms\n", current_time);
    printf("📊 Active Charts: %d\n", viz->chart_count);
    printf("🔄 Refresh Rate: %d ms\n\n", viz->refresh_interval_ms);
    
    // Render all charts
    VisualizationChart* chart = viz->charts;
    while (chart) {
        switch (chart->config.type) {
            case CHART_LINE:
                profiling_visualization_render_ascii_line_chart(viz, chart);
                break;
            case CHART_BAR:
                profiling_visualization_render_ascii_bar_chart(viz, chart);
                break;
            case CHART_AREA:
            case CHART_SCATTER:
                // Fallback to sparkline for unsupported types
                profiling_visualization_render_ascii_sparkline(viz, chart);
                break;
            default:
                profiling_visualization_render_ascii_sparkline(viz, chart);
                break;
        }
        chart = chart->next;
    }
    
    print_colored_viz(viz, VIZ_COLOR_BOLD VIZ_COLOR_GREEN, 
                     "💡 Use ':viz stop' to exit live mode\n");
}

// =============================================================================
// HTML Report Generation
// =============================================================================

int profiling_visualization_generate_html_report(ProfilingVisualization* viz) {
    if (!viz || !viz->generate_html) return -1;
    
    char report_path[512];
    snprintf(report_path, sizeof(report_path), "%s/profiling_report.html", viz->output_directory);
    
    FILE* html = fopen(report_path, "w");
    if (!html) return -1;
    
    // HTML header with modern styling and Chart.js
    fprintf(html, "<!DOCTYPE html>\n");
    fprintf(html, "<html lang=\"en\">\n");
    fprintf(html, "<head>\n");
    fprintf(html, "    <meta charset=\"UTF-8\">\n");
    fprintf(html, "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
    fprintf(html, "    <title>Goo Profiling Visualization Report</title>\n");
    fprintf(html, "    <script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script>\n");
    fprintf(html, "    <style>\n");
    fprintf(html, "        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif; margin: 0; padding: 20px; background: #f8f9fa; }\n");
    fprintf(html, "        .container { max-width: 1400px; margin: 0 auto; }\n");
    fprintf(html, "        .header { background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); color: white; padding: 30px; border-radius: 12px; margin-bottom: 30px; }\n");
    fprintf(html, "        .chart-container { background: white; border-radius: 8px; padding: 20px; margin-bottom: 20px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }\n");
    fprintf(html, "        .chart-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(400px, 1fr)); gap: 20px; }\n");
    fprintf(html, "        .metric-card { background: white; border-radius: 8px; padding: 20px; text-align: center; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }\n");
    fprintf(html, "        .metric-value { font-size: 2em; font-weight: bold; color: #667eea; }\n");
    fprintf(html, "        .metric-label { color: #666; margin-top: 10px; }\n");
    fprintf(html, "        .status-indicator { display: inline-block; width: 12px; height: 12px; border-radius: 50%%; margin-right: 8px; }\n");
    fprintf(html, "        .status-active { background: #28a745; }\n");
    fprintf(html, "        .status-inactive { background: #dc3545; }\n");
    fprintf(html, "    </style>\n");
    fprintf(html, "</head>\n");
    fprintf(html, "<body>\n");
    fprintf(html, "    <div class=\"container\">\n");
    
    // Header
    time_t now = time(NULL);
    char* time_str = ctime(&now);
    time_str[strlen(time_str) - 1] = '\0';
    
    fprintf(html, "        <div class=\"header\">\n");
    fprintf(html, "            <h1>📊 Goo Profiling Visualization Report</h1>\n");
    fprintf(html, "            <p>Generated on %s</p>\n", time_str);
    fprintf(html, "            <p><span class=\"status-indicator %s\"></span>Visualization Status: %s</p>\n",
            viz->is_enabled ? "status-active" : "status-inactive",
            viz->is_enabled ? "Active" : "Inactive");
    fprintf(html, "        </div>\n");
    
    // Metrics overview
    fprintf(html, "        <div class=\"chart-grid\">\n");
    fprintf(html, "            <div class=\"metric-card\">\n");
    fprintf(html, "                <div class=\"metric-value\">%d</div>\n", viz->chart_count);
    fprintf(html, "                <div class=\"metric-label\">Active Charts</div>\n");
    fprintf(html, "            </div>\n");
    fprintf(html, "            <div class=\"metric-card\">\n");
    fprintf(html, "                <div class=\"metric-value\">%d ms</div>\n", viz->refresh_interval_ms);
    fprintf(html, "                <div class=\"metric-label\">Refresh Interval</div>\n");
    fprintf(html, "            </div>\n");
    fprintf(html, "            <div class=\"metric-card\">\n");
    fprintf(html, "                <div class=\"metric-value\">%s</div>\n", viz->real_time_mode ? "ON" : "OFF");
    fprintf(html, "                <div class=\"metric-label\">Real-time Mode</div>\n");
    fprintf(html, "            </div>\n");
    fprintf(html, "        </div>\n");
    
    // Chart containers
    VisualizationChart* chart = viz->charts;
    while (chart) {
        fprintf(html, "        <div class=\"chart-container\">\n");
        fprintf(html, "            <h3>%s</h3>\n", chart->config.title);
        fprintf(html, "            <canvas id=\"chart_%s\" width=\"400\" height=\"200\"></canvas>\n", chart->chart_id);
        
        // Generate Chart.js data
        fprintf(html, "            <script>\n");
        fprintf(html, "                const ctx_%s = document.getElementById('chart_%s').getContext('2d');\n", 
                chart->chart_id, chart->chart_id);
        fprintf(html, "                new Chart(ctx_%s, {\n", chart->chart_id);
        fprintf(html, "                    type: '%s',\n", 
                chart->config.type == CHART_LINE ? "line" : 
                chart->config.type == CHART_BAR ? "bar" : "line");
        fprintf(html, "                    data: {\n");
        fprintf(html, "                        labels: [");
        
        // Generate labels and data
        MetricDataPoint* point = chart->data_points;
        int count = 0;
        while (point && count < 20) {
            if (count > 0) fprintf(html, ", ");
            fprintf(html, "'%s'", point->label ? point->label : "Sample");
            point = point->next;
            count++;
        }
        
        fprintf(html, "],\n");
        fprintf(html, "                        datasets: [{\n");
        fprintf(html, "                            label: '%s',\n", chart->config.y_label);
        fprintf(html, "                            data: [");
        
        point = chart->data_points;
        count = 0;
        while (point && count < 20) {
            if (count > 0) fprintf(html, ", ");
            fprintf(html, "%.2f", point->value);
            point = point->next;
            count++;
        }
        
        fprintf(html, "],\n");
        fprintf(html, "                            borderColor: 'rgb(102, 126, 234)',\n");
        fprintf(html, "                            backgroundColor: 'rgba(102, 126, 234, 0.2)',\n");
        fprintf(html, "                            tension: 0.3\n");
        fprintf(html, "                        }]\n");
        fprintf(html, "                    },\n");
        fprintf(html, "                    options: {\n");
        fprintf(html, "                        responsive: true,\n");
        fprintf(html, "                        plugins: {\n");
        fprintf(html, "                            title: {\n");
        fprintf(html, "                                display: true,\n");
        fprintf(html, "                                text: '%s'\n", chart->config.title);
        fprintf(html, "                            }\n");
        fprintf(html, "                        },\n");
        fprintf(html, "                        scales: {\n");
        fprintf(html, "                            y: {\n");
        fprintf(html, "                                beginAtZero: true,\n");
        fprintf(html, "                                title: {\n");
        fprintf(html, "                                    display: true,\n");
        fprintf(html, "                                    text: '%s'\n", chart->config.y_label);
        fprintf(html, "                                }\n");
        fprintf(html, "                            },\n");
        fprintf(html, "                            x: {\n");
        fprintf(html, "                                title: {\n");
        fprintf(html, "                                    display: true,\n");
        fprintf(html, "                                    text: '%s'\n", chart->config.x_label);
        fprintf(html, "                                }\n");
        fprintf(html, "                            }\n");
        fprintf(html, "                        }\n");
        fprintf(html, "                    }\n");
        fprintf(html, "                });\n");
        fprintf(html, "            </script>\n");
        fprintf(html, "        </div>\n");
        
        chart = chart->next;
    }
    
    // Auto-refresh script
    fprintf(html, "        <script>\n");
    fprintf(html, "            // Auto-refresh page every %d seconds in real-time mode\n", viz->refresh_interval_ms / 1000);
    if (viz->real_time_mode) {
        fprintf(html, "            setTimeout(() => { location.reload(); }, %d);\n", viz->refresh_interval_ms);
    }
    fprintf(html, "        </script>\n");
    
    fprintf(html, "    </div>\n");
    fprintf(html, "</body>\n");
    fprintf(html, "</html>\n");
    
    fclose(html);
    
    printf("📄 Profiling visualization report generated: %s\n", report_path);
    return 0;
}

// =============================================================================
// Integration with Performance Monitor
// =============================================================================

int profiling_visualization_update_from_monitor(ProfilingVisualization* viz, PerformanceMonitor* monitor) {
    if (!viz || !monitor) return -1;
    
    // This is a simplified integration that would read from the performance monitor
    // and update visualization charts. In a full implementation, this would:
    // 1. Query the monitor for recent metrics
    // 2. Update existing charts with new data points
    // 3. Create new charts for new metric types
    // 4. Trigger re-rendering if in real-time mode
    
    return 0;
}

// =============================================================================
// REPL Integration Commands
// =============================================================================

int profiling_visualization_handle_repl_command(ProfilingVisualization* viz, REPLContext* repl, const char* command) {
    if (!viz || !repl || !command) return -1;
    
    // Parse command
    if (strstr(command, "status")) {
        printf("📊 Profiling Visualization Status:\n");
        printf("   Enabled: %s\n", viz->is_enabled ? "Yes" : "No");
        printf("   Real-time Mode: %s\n", viz->real_time_mode ? "Yes" : "No");
        printf("   Refresh Interval: %d ms\n", viz->refresh_interval_ms);
        printf("   Active Charts: %d\n", viz->chart_count);
        printf("   Output Directory: %s\n", viz->output_directory);
        
    } else if (strstr(command, "start")) {
        viz->is_enabled = true;
        viz->real_time_mode = true;
        printf("✅ Profiling visualization started in real-time mode\n");
        
    } else if (strstr(command, "stop")) {
        viz->real_time_mode = false;
        printf("⏸️  Real-time visualization stopped\n");
        
    } else if (strstr(command, "dashboard") || strstr(command, "live")) {
        if (viz->live_terminal_display) {
            profiling_visualization_render_live_dashboard(viz);
        } else {
            printf("❌ Live terminal display is disabled\n");
        }
        
    } else if (strstr(command, "report")) {
        int result = profiling_visualization_generate_html_report(viz);
        if (result == 0) {
            printf("✅ HTML report generated successfully\n");
        } else {
            printf("❌ Failed to generate HTML report\n");
        }
        
    } else if (strstr(command, "chart")) {
        // Create new chart command
        char* chart_type = strstr(command, "type=");
        char* chart_title = strstr(command, "title=");
        
        if (chart_type && chart_title) {
            char type_str[32];
            char title_str[128];
            sscanf(chart_type + 5, "%31s", type_str);
            sscanf(chart_title + 6, "%127s", title_str);
            
            ChartType type = CHART_LINE;
            if (strcmp(type_str, "bar") == 0) type = CHART_BAR;
            else if (strcmp(type_str, "area") == 0) type = CHART_AREA;
            
            char chart_id[64];
            snprintf(chart_id, sizeof(chart_id), "chart_%d", viz->chart_count + 1);
            
            VisualizationChart* chart = profiling_visualization_create_chart(viz, chart_id, type, title_str);
            if (chart) {
                printf("✅ Created new chart: %s (type: %s)\n", title_str, type_str);
            } else {
                printf("❌ Failed to create chart\n");
            }
        } else {
            printf("Usage: :viz chart type=<line|bar|area> title=<title>\n");
        }
        
    } else if (strstr(command, "config")) {
        if (strstr(command, "interval=")) {
            char* interval_str = strstr(command, "interval=");
            int new_interval = atoi(interval_str + 9);
            if (new_interval >= 100 && new_interval <= 10000) {
                viz->refresh_interval_ms = new_interval;
                printf("✅ Refresh interval set to %d ms\n", new_interval);
            } else {
                printf("❌ Invalid interval. Use 100-10000 ms\n");
            }
        } else {
            printf("⚙️  Visualization Configuration:\n");
            printf("   Refresh Interval: %d ms\n", viz->refresh_interval_ms);
            printf("   Generate HTML: %s\n", viz->generate_html ? "Yes" : "No");
            printf("   ASCII Charts: %s\n", viz->generate_ascii ? "Yes" : "No");
            printf("   Terminal Colors: %s\n", viz->use_colors ? "Yes" : "No");
        }
        
    } else {
        printf("📊 Profiling Visualization Commands:\n");
        printf("   :viz status                    - Show status\n");
        printf("   :viz start                     - Start real-time visualization\n");
        printf("   :viz stop                      - Stop real-time mode\n");
        printf("   :viz dashboard                 - Show live dashboard\n");
        printf("   :viz report                    - Generate HTML report\n");
        printf("   :viz chart type=<type> title=<title> - Create new chart\n");
        printf("   :viz config [interval=<ms>]    - Configure settings\n");
        return -1;
    }
    
    return 0;
}

// =============================================================================
// Demo and Testing Functions
// =============================================================================

void profiling_visualization_demo(ProfilingVisualization* viz) {
    if (!viz) return;
    
    print_colored_viz(viz, VIZ_COLOR_BOLD VIZ_COLOR_MAGENTA, 
                     "🚀 Profiling Visualization Demo\n");
    print_colored_viz(viz, VIZ_COLOR_MAGENTA, 
                     "================================\n\n");
    
    // Create demo charts
    VisualizationChart* memory_chart = profiling_visualization_create_chart(viz, "memory_usage", CHART_LINE, "Memory Usage");
    VisualizationChart* cpu_chart = profiling_visualization_create_chart(viz, "cpu_usage", CHART_BAR, "CPU Usage");
    VisualizationChart* compile_chart = profiling_visualization_create_chart(viz, "compile_time", CHART_LINE, "Compilation Time");
    
    // Add demo data
    for (int i = 0; i < 20; i++) {
        double memory_val = 50 + 20 * sin(i * 0.3) + (rand() % 10);
        double cpu_val = 30 + 15 * cos(i * 0.4) + (rand() % 20);
        double compile_val = 100 + 50 * sin(i * 0.2) + (rand() % 30);
        
        char label[32];
        snprintf(label, sizeof(label), "T%d", i);
        
        profiling_visualization_add_data_point(memory_chart, memory_val, label, "memory_tracking");
        profiling_visualization_add_data_point(cpu_chart, cpu_val, label, "cpu_tracking");
        profiling_visualization_add_data_point(compile_chart, compile_val, label, "compile_tracking");
        
        usleep(50000); // 50ms delay for demo
    }
    
    // Render charts
    profiling_visualization_render_ascii_line_chart(viz, memory_chart);
    profiling_visualization_render_ascii_bar_chart(viz, cpu_chart);
    profiling_visualization_render_ascii_sparkline(viz, compile_chart);
    
    // Generate HTML report
    profiling_visualization_generate_html_report(viz);
    
    print_colored_viz(viz, VIZ_COLOR_BOLD VIZ_COLOR_GREEN, 
                     "✅ Demo completed! Check the HTML report for interactive charts.\n");
}