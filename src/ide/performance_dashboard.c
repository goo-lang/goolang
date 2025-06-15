#include "performance_dashboard.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>

// Global dashboard instance
static PerformanceDashboard* g_dashboard = NULL;
static pthread_t g_server_thread;

// Initialize the performance dashboard
bool performance_dashboard_init(int port, const char* host) {
    g_dashboard = malloc(sizeof(PerformanceDashboard));
    if (!g_dashboard) return false;
    
    memset(g_dashboard, 0, sizeof(PerformanceDashboard));
    
    g_dashboard->running = false;
    g_dashboard->port = port;
    g_dashboard->host = strdup(host ? host : "localhost");
    g_dashboard->log_file = stderr;
    
    // Initialize metrics storage
    g_dashboard->max_metrics = 100;
    g_dashboard->metrics = malloc(sizeof(PerformanceMetric) * g_dashboard->max_metrics);
    g_dashboard->metric_count = 0;
    
    // Initialize dashboards storage
    g_dashboard->max_dashboards = 10;
    g_dashboard->dashboards = malloc(sizeof(DashboardLayout) * g_dashboard->max_dashboards);
    g_dashboard->dashboard_count = 0;
    
    // Server configuration
    g_dashboard->static_path = strdup("./dashboard/static");
    g_dashboard->template_path = strdup("./dashboard/templates");
    g_dashboard->enable_websocket = true;
    g_dashboard->enable_cors = true;
    g_dashboard->max_connections = 100;
    
    // Data retention
    g_dashboard->data_retention_hours = 24;
    g_dashboard->enable_persistence = false;
    g_dashboard->data_directory = strdup("./dashboard/data");
    
    // Register default Goo metrics
    dashboard_register_goo_metrics();
    
    // Create default dashboards
    dashboard_create_system_overview();
    dashboard_create_goo_language_dashboard();
    
    return true;
}

// Add a performance metric
int dashboard_add_metric(const char* name, const char* description, const char* unit) {
    if (!g_dashboard || g_dashboard->metric_count >= g_dashboard->max_metrics) {
        return -1;
    }
    
    PerformanceMetric* metric = &g_dashboard->metrics[g_dashboard->metric_count];
    metric->name = strdup(name);
    metric->description = strdup(description ? description : "");
    metric->unit = strdup(unit ? unit : "");
    
    metric->max_data_points = 1000; // Keep last 1000 points
    metric->data = malloc(sizeof(DataPoint) * metric->max_data_points);
    metric->data_count = 0;
    
    metric->current_value = 0.0;
    metric->min_value = 0.0;
    metric->max_value = 0.0;
    metric->avg_value = 0.0;
    
    // Create default chart configuration
    metric->chart = dashboard_create_chart(name, name, CHART_LINE);
    
    g_dashboard->metric_count++;
    return g_dashboard->metric_count - 1;
}

// Update metric value
bool dashboard_update_metric(const char* name, double value) {
    return dashboard_update_metric_with_timestamp(name, value, dashboard_current_timestamp());
}

// Update metric with timestamp
bool dashboard_update_metric_with_timestamp(const char* name, double value, uint64_t timestamp) {
    PerformanceMetric* metric = dashboard_get_metric(name);
    if (!metric) return false;
    
    // Add new data point
    if (metric->data_count >= metric->max_data_points) {
        // Shift data points (remove oldest)
        memmove(metric->data, metric->data + 1, 
                sizeof(DataPoint) * (metric->max_data_points - 1));
        metric->data_count--;
    }
    
    DataPoint* point = &metric->data[metric->data_count];
    point->timestamp = timestamp;
    point->value = value;
    point->label = NULL;
    
    metric->data_count++;
    metric->current_value = value;
    
    // Update statistics
    if (metric->data_count == 1) {
        metric->min_value = value;
        metric->max_value = value;
        metric->avg_value = value;
    } else {
        if (value < metric->min_value) metric->min_value = value;
        if (value > metric->max_value) metric->max_value = value;
        metric->avg_value = dashboard_calculate_average(metric->data, metric->data_count);
    }
    
    // Broadcast update if streaming enabled
    dashboard_broadcast_update(name, value);
    
    return true;
}

// Get metric by name
PerformanceMetric* dashboard_get_metric(const char* name) {
    if (!g_dashboard) return NULL;
    
    for (int i = 0; i < g_dashboard->metric_count; i++) {
        if (strcmp(g_dashboard->metrics[i].name, name) == 0) {
            return &g_dashboard->metrics[i];
        }
    }
    return NULL;
}

// Create chart configuration
ChartConfig* dashboard_create_chart(const char* id, const char* title, ChartType type) {
    ChartConfig* chart = malloc(sizeof(ChartConfig));
    if (!chart) return NULL;
    
    chart->id = strdup(id);
    chart->title = strdup(title);
    chart->x_axis_label = strdup("Time");
    chart->y_axis_label = strdup("Value");
    chart->type = type;
    chart->max_points = 1000;
    chart->auto_scale = true;
    chart->min_y = 0.0;
    chart->max_y = 100.0;
    chart->color = strdup("#007acc");
    chart->show_legend = true;
    chart->show_grid = true;
    chart->refresh_interval = 1000; // 1 second
    
    return chart;
}

// Generate HTML dashboard
char* dashboard_generate_html(const char* dashboard_id) {
    (void)dashboard_id; // Suppress unused parameter warning
    static char html_buffer[16384];
    
    snprintf(html_buffer, sizeof(html_buffer),
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "    <meta charset=\"UTF-8\">\n"
        "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "    <title>Goo Performance Dashboard</title>\n"
        "    <script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script>\n"
        "    <style>\n"
        "        body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background: #f5f5f5; }\n"
        "        .dashboard { display: grid; grid-template-columns: repeat(auto-fit, minmax(400px, 1fr)); gap: 20px; }\n"
        "        .widget { background: white; border-radius: 8px; padding: 20px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n"
        "        .widget h3 { margin-top: 0; color: #333; }\n"
        "        .metric-value { font-size: 2em; font-weight: bold; color: #007acc; }\n"
        "        .metric-unit { font-size: 0.8em; color: #666; }\n"
        "        .chart-container { position: relative; height: 300px; }\n"
        "        .status-indicator { display: inline-block; width: 12px; height: 12px; border-radius: 50%%; margin-right: 8px; }\n"
        "        .status-good { background: #4caf50; }\n"
        "        .status-warning { background: #ff9800; }\n"
        "        .status-error { background: #f44336; }\n"
        "        .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }\n"
        "        .header h1 { margin: 0; color: #333; }\n"
        "        .refresh-btn { padding: 10px 20px; background: #007acc; color: white; border: none; border-radius: 4px; cursor: pointer; }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"header\">\n"
        "        <h1>🚀 Goo Performance Dashboard</h1>\n"
        "        <button class=\"refresh-btn\" onclick=\"refreshDashboard()\">Refresh</button>\n"
        "    </div>\n"
        "    \n"
        "    <div class=\"dashboard\">\n"
        "        <!-- Memory Usage Widget -->\n"
        "        <div class=\"widget\">\n"
        "            <h3><span class=\"status-indicator status-good\"></span>Memory Usage</h3>\n"
        "            <div class=\"metric-value\" id=\"memory-value\">--</div>\n"
        "            <div class=\"metric-unit\">MB</div>\n"
        "            <div class=\"chart-container\">\n"
        "                <canvas id=\"memory-chart\"></canvas>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <!-- CPU Usage Widget -->\n"
        "        <div class=\"widget\">\n"
        "            <h3><span class=\"status-indicator status-good\"></span>CPU Usage</h3>\n"
        "            <div class=\"metric-value\" id=\"cpu-value\">--</div>\n"
        "            <div class=\"metric-unit\">%%</div>\n"
        "            <div class=\"chart-container\">\n"
        "                <canvas id=\"cpu-chart\"></canvas>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <!-- REPL Activity Widget -->\n"
        "        <div class=\"widget\">\n"
        "            <h3><span class=\"status-indicator status-good\"></span>REPL Commands</h3>\n"
        "            <div class=\"metric-value\" id=\"repl-value\">--</div>\n"
        "            <div class=\"metric-unit\">commands/min</div>\n"
        "            <div class=\"chart-container\">\n"
        "                <canvas id=\"repl-chart\"></canvas>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <!-- Error Rate Widget -->\n"
        "        <div class=\"widget\">\n"
        "            <h3><span class=\"status-indicator status-warning\"></span>Error Rate</h3>\n"
        "            <div class=\"metric-value\" id=\"error-value\">--</div>\n"
        "            <div class=\"metric-unit\">errors/min</div>\n"
        "            <div class=\"chart-container\">\n"
        "                <canvas id=\"error-chart\"></canvas>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <!-- Compilation Time Widget -->\n"
        "        <div class=\"widget\">\n"
        "            <h3><span class=\"status-indicator status-good\"></span>Compilation Time</h3>\n"
        "            <div class=\"metric-value\" id=\"compile-value\">--</div>\n"
        "            <div class=\"metric-unit\">ms</div>\n"
        "            <div class=\"chart-container\">\n"
        "                <canvas id=\"compile-chart\"></canvas>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <!-- Type Checking Widget -->\n"
        "        <div class=\"widget\">\n"
        "            <h3><span class=\"status-indicator status-good\"></span>Type Checking</h3>\n"
        "            <div class=\"metric-value\" id=\"typecheck-value\">--</div>\n"
        "            <div class=\"metric-unit\">ms</div>\n"
        "            <div class=\"chart-container\">\n"
        "                <canvas id=\"typecheck-chart\"></canvas>\n"
        "            </div>\n"
        "        </div>\n"
        "    </div>\n"
        "    \n"
        "    <script>\n"
        "        // Chart configurations\n"
        "        const chartConfig = {\n"
        "            type: 'line',\n"
        "            options: {\n"
        "                responsive: true,\n"
        "                maintainAspectRatio: false,\n"
        "                scales: {\n"
        "                    x: { type: 'time', time: { unit: 'minute' } },\n"
        "                    y: { beginAtZero: true }\n"
        "                },\n"
        "                plugins: { legend: { display: false } }\n"
        "            }\n"
        "        };\n"
        "        \n"
        "        // Initialize charts\n"
        "        const charts = {};\n"
        "        ['memory', 'cpu', 'repl', 'error', 'compile', 'typecheck'].forEach(metric => {\n"
        "            const ctx = document.getElementById(metric + '-chart').getContext('2d');\n"
        "            charts[metric] = new Chart(ctx, {\n"
        "                ...chartConfig,\n"
        "                data: {\n"
        "                    datasets: [{\n"
        "                        data: [],\n"
        "                        borderColor: '#007acc',\n"
        "                        backgroundColor: 'rgba(0, 122, 204, 0.1)',\n"
        "                        fill: true\n"
        "                    }]\n"
        "                }\n"
        "            });\n"
        "        });\n"
        "        \n"
        "        // Update dashboard data\n"
        "        function updateDashboard() {\n"
        "            fetch('/api/metrics')\n"
        "                .then(response => response.json())\n"
        "                .then(data => {\n"
        "                    Object.keys(data).forEach(metric => {\n"
        "                        const valueElement = document.getElementById(metric + '-value');\n"
        "                        if (valueElement) {\n"
        "                            valueElement.textContent = data[metric].current.toFixed(1);\n"
        "                        }\n"
        "                        \n"
        "                        if (charts[metric]) {\n"
        "                            charts[metric].data.datasets[0].data = data[metric].history;\n"
        "                            charts[metric].update('none');\n"
        "                        }\n"
        "                    });\n"
        "                })\n"
        "                .catch(error => console.error('Error updating dashboard:', error));\n"
        "        }\n"
        "        \n"
        "        function refreshDashboard() {\n"
        "            updateDashboard();\n"
        "        }\n"
        "        \n"
        "        // Auto-refresh every 2 seconds\n"
        "        setInterval(updateDashboard, 2000);\n"
        "        updateDashboard(); // Initial load\n"
        "    </script>\n"
        "</body>\n"
        "</html>");
    
    return html_buffer;
}

// Generate JSON data for metrics
char* dashboard_generate_json_data(const char* metric_name) {
    static char json_buffer[8192];
    
    if (metric_name) {
        // Single metric
        PerformanceMetric* metric = dashboard_get_metric(metric_name);
        if (!metric) {
            strcpy(json_buffer, "{}");
            return json_buffer;
        }
        
        snprintf(json_buffer, sizeof(json_buffer),
            "{"
                "\"name\":\"%s\","
                "\"current\":%.2f,"
                "\"min\":%.2f,"
                "\"max\":%.2f,"
                "\"avg\":%.2f,"
                "\"unit\":\"%s\","
                "\"history\":["
            "}",
            metric->name, metric->current_value, metric->min_value,
            metric->max_value, metric->avg_value, metric->unit);
        
        // Add history data points
        for (int i = 0; i < metric->data_count; i++) {
            char point_json[128];
            snprintf(point_json, sizeof(point_json),
                "%s{\"x\":%llu,\"y\":%.2f}",
                (i > 0) ? "," : "",
                (unsigned long long)metric->data[i].timestamp,
                metric->data[i].value);
            strcat(json_buffer, point_json);
        }
        
        strcat(json_buffer, "]}");
    } else {
        // All metrics
        strcpy(json_buffer, "{");
        
        for (int i = 0; i < g_dashboard->metric_count; i++) {
            PerformanceMetric* metric = &g_dashboard->metrics[i];
            char metric_json[1024];
            
            snprintf(metric_json, sizeof(metric_json),
                "%s\"%s\":{"
                    "\"current\":%.2f,"
                    "\"min\":%.2f,"
                    "\"max\":%.2f,"
                    "\"avg\":%.2f,"
                    "\"unit\":\"%s\","
                    "\"history\":["
                "}",
                (i > 0) ? "," : "",
                metric->name, metric->current_value, metric->min_value,
                metric->max_value, metric->avg_value, metric->unit);
            
            strcat(json_buffer, metric_json);
            
            // Add limited history for all metrics overview
            int start = (metric->data_count > 20) ? metric->data_count - 20 : 0;
            for (int j = start; j < metric->data_count; j++) {
                char point_json[64];
                snprintf(point_json, sizeof(point_json),
                    "%s{\"x\":%llu,\"y\":%.2f}",
                    (j > start) ? "," : "",
                    (unsigned long long)metric->data[j].timestamp,
                    metric->data[j].value);
                strcat(json_buffer, point_json);
            }
            
            strcat(json_buffer, "]}");
        }
        
        strcat(json_buffer, "}");
    }
    
    return json_buffer;
}

// Terminal-based chart display
void dashboard_terminal_chart(const char* metric_name, int width, int height) {
    PerformanceMetric* metric = dashboard_get_metric(metric_name);
    if (!metric || metric->data_count == 0) {
        printf("No data available for metric: %s\n", metric_name);
        return;
    }
    
    printf("\n📊 %s (%s)\n", metric->name, metric->unit);
    printf("Current: %.2f | Min: %.2f | Max: %.2f | Avg: %.2f\n\n",
           metric->current_value, metric->min_value, metric->max_value, metric->avg_value);
    
    // Simple ASCII chart
    double range = metric->max_value - metric->min_value;
    if (range == 0) range = 1.0;
    
    // Use last 'width' data points
    int start = (metric->data_count > width) ? metric->data_count - width : 0;
    int data_width = metric->data_count - start;
    
    for (int row = height - 1; row >= 0; row--) {
        double threshold = metric->min_value + (range * row / (height - 1));
        
        printf("%6.1f │", threshold);
        
        for (int col = 0; col < data_width; col++) {
            double value = metric->data[start + col].value;
            if (value >= threshold) {
                printf("█");
            } else {
                printf(" ");
            }
        }
        printf("\n");
    }
    
    printf("       └");
    for (int i = 0; i < data_width; i++) printf("─");
    printf("\n");
    
    printf("        ");
    for (int i = 0; i < data_width; i += 10) {
        printf("%-10d", i);
    }
    printf("\n\n");
}

// Terminal gauge display
void dashboard_terminal_gauge(const char* metric_name, double min_val, double max_val) {
    PerformanceMetric* metric = dashboard_get_metric(metric_name);
    if (!metric) {
        printf("Metric not found: %s\n", metric_name);
        return;
    }
    
    double value = metric->current_value;
    double percentage = (value - min_val) / (max_val - min_val) * 100.0;
    if (percentage < 0) percentage = 0;
    if (percentage > 100) percentage = 100;
    
    int filled = (int)(percentage / 5); // 20 segments
    
    printf("\n🎯 %s: %.2f %s\n", metric->name, value, metric->unit);
    printf("┌");
    for (int i = 0; i < 20; i++) printf("─");
    printf("┐\n│");
    
    for (int i = 0; i < 20; i++) {
        if (i < filled) {
            if (percentage > 80) printf("█");      // Red zone
            else if (percentage > 60) printf("▓"); // Yellow zone  
            else printf("▒");                      // Green zone
        } else {
            printf(" ");
        }
    }
    
    printf("│ %.1f%%\n└", percentage);
    for (int i = 0; i < 20; i++) printf("─");
    printf("┘\n");
    printf("%.1f", min_val);
    for (int i = 0; i < 16; i++) printf(" ");
    printf("%.1f\n\n", max_val);
}

// Register Goo-specific metrics
void dashboard_register_goo_metrics(void) {
    dashboard_add_metric("memory_usage", "Current memory usage", "MB");
    dashboard_add_metric("cpu_usage", "CPU utilization", "%");
    dashboard_add_metric("repl_commands", "REPL commands per minute", "cmd/min");
    dashboard_add_metric("error_rate", "Errors per minute", "errors/min");
    dashboard_add_metric("compilation_time", "Average compilation time", "ms");
    dashboard_add_metric("type_check_time", "Type checking time", "ms");
    dashboard_add_metric("gc_collections", "Garbage collections", "count");
    dashboard_add_metric("channel_operations", "Channel operations per second", "ops/sec");
    dashboard_add_metric("error_unions_created", "Error unions created", "count");
    dashboard_add_metric("ownership_transfers", "Ownership transfers", "count");
}

// Create system overview dashboard
void dashboard_create_system_overview(void) {
    dashboard_create_layout("system", "System Overview", 3, 2);
    
    dashboard_add_widget("system", "memory", "Memory Usage", "chart");
    dashboard_add_widget("system", "cpu", "CPU Usage", "gauge");
    dashboard_add_widget("system", "errors", "Error Rate", "chart");
}

// Create Goo language specific dashboard
void dashboard_create_goo_language_dashboard(void) {
    dashboard_create_layout("goo", "Goo Language Metrics", 2, 3);
    
    dashboard_add_widget("goo", "repl", "REPL Activity", "chart");
    dashboard_add_widget("goo", "compilation", "Compilation Performance", "chart");
    dashboard_add_widget("goo", "types", "Type System", "chart");
    dashboard_add_widget("goo", "channels", "Channel Operations", "chart");
    dashboard_add_widget("goo", "ownership", "Ownership Tracking", "gauge");
    dashboard_add_widget("goo", "errors", "Error Unions", "chart");
}

// Update from performance monitor
void dashboard_update_from_monitor(void) {
    // Integrate with existing performance monitor
    // Update metrics from current performance data
    
    // Simulate metric updates (in real implementation, get from performance_monitor.h)
    static int update_counter = 0;
    update_counter++;
    
    // Update memory usage
    dashboard_update_metric("memory_usage", 150.0 + (update_counter % 50));
    
    // Update CPU usage
    dashboard_update_metric("cpu_usage", 25.0 + (update_counter % 30));
    
    // Update compilation time
    dashboard_update_metric("compilation_time", 100.0 + (update_counter % 20));
    
    // Update type checking time
    dashboard_update_metric("type_check_time", 50.0 + (update_counter % 15));
    
    // Update error rate
    dashboard_update_metric("error_rate", (update_counter % 10) < 2 ? 1.0 : 0.0);
    
    // Update REPL activity
    dashboard_update_metric("repl_commands", 5.0 + (update_counter % 8));
}

// Calculate average of data points
double dashboard_calculate_average(DataPoint* points, int count) {
    if (count == 0) return 0.0;
    
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        sum += points[i].value;
    }
    return sum / count;
}

// Get current timestamp in milliseconds
uint64_t dashboard_current_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Broadcast metric update (placeholder for WebSocket)
void dashboard_broadcast_update(const char* metric_name, double value) {
    // In a full implementation, this would broadcast via WebSocket
    // For now, just log the update
    (void)metric_name;
    (void)value;
}

// Create dashboard layout
int dashboard_create_layout(const char* id, const char* title, int columns, int rows) {
    if (!g_dashboard || g_dashboard->dashboard_count >= g_dashboard->max_dashboards) {
        return -1;
    }
    
    DashboardLayout* layout = &g_dashboard->dashboards[g_dashboard->dashboard_count];
    layout->id = strdup(id);
    layout->title = strdup(title);
    layout->description = strdup("");
    layout->grid_columns = columns;
    layout->grid_rows = rows;
    layout->widget_count = 0;
    layout->max_widgets = 20;
    layout->widgets = malloc(sizeof(DashboardWidget) * layout->max_widgets);
    layout->refresh_interval = 2000;
    layout->auto_refresh = true;
    
    g_dashboard->dashboard_count++;
    return g_dashboard->dashboard_count - 1;
}

// Add widget to dashboard
int dashboard_add_widget(const char* dashboard_id, const char* widget_id, 
                        const char* title, const char* type) {
    DashboardLayout* layout = dashboard_get_layout(dashboard_id);
    if (!layout || layout->widget_count >= layout->max_widgets) {
        return -1;
    }
    
    DashboardWidget* widget = &layout->widgets[layout->widget_count];
    widget->id = strdup(widget_id);
    widget->title = strdup(title);
    widget->type = strdup(type);
    widget->width = 1;
    widget->height = 1;
    widget->x_position = layout->widget_count % layout->grid_columns;
    widget->y_position = layout->widget_count / layout->grid_columns;
    widget->metric_count = 0;
    widget->config_json = strdup("{}");
    
    layout->widget_count++;
    return layout->widget_count - 1;
}

// Get dashboard layout
DashboardLayout* dashboard_get_layout(const char* dashboard_id) {
    if (!g_dashboard) return NULL;
    
    for (int i = 0; i < g_dashboard->dashboard_count; i++) {
        if (strcmp(g_dashboard->dashboards[i].id, dashboard_id) == 0) {
            return &g_dashboard->dashboards[i];
        }
    }
    return NULL;
}

// Show terminal view
void dashboard_show_terminal_view(void) {
    system("clear"); // Clear terminal
    
    printf("╔══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                        🚀 GOO PERFORMANCE DASHBOARD                        ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════════╝\n\n");
    
    // Show gauges for key metrics
    dashboard_terminal_gauge("memory_usage", 0, 500);
    dashboard_terminal_gauge("cpu_usage", 0, 100);
    
    // Show charts for trending metrics
    dashboard_terminal_chart("compilation_time", 60, 8);
    dashboard_terminal_chart("error_rate", 60, 6);
    
    printf("Press 'q' to quit, 'r' to refresh, or any other key to continue...\n");
}

// Shutdown dashboard
void performance_dashboard_shutdown(void) {
    if (!g_dashboard) return;
    
    g_dashboard->running = false;
    
    // Cleanup metrics
    for (int i = 0; i < g_dashboard->metric_count; i++) {
        free(g_dashboard->metrics[i].name);
        free(g_dashboard->metrics[i].description);
        free(g_dashboard->metrics[i].unit);
        free(g_dashboard->metrics[i].data);
        if (g_dashboard->metrics[i].chart) {
            dashboard_destroy_chart(g_dashboard->metrics[i].chart);
        }
    }
    free(g_dashboard->metrics);
    
    // Cleanup dashboards
    for (int i = 0; i < g_dashboard->dashboard_count; i++) {
        DashboardLayout* layout = &g_dashboard->dashboards[i];
        free(layout->id);
        free(layout->title);
        free(layout->description);
        
        for (int j = 0; j < layout->widget_count; j++) {
            free(layout->widgets[j].id);
            free(layout->widgets[j].title);
            free(layout->widgets[j].type);
            free(layout->widgets[j].config_json);
        }
        free(layout->widgets);
    }
    free(g_dashboard->dashboards);
    
    free(g_dashboard->host);
    free(g_dashboard->static_path);
    free(g_dashboard->template_path);
    free(g_dashboard->data_directory);
    
    free(g_dashboard);
    g_dashboard = NULL;
}

// Destroy chart configuration
void dashboard_destroy_chart(ChartConfig* chart) {
    if (!chart) return;
    
    free(chart->id);
    free(chart->title);
    free(chart->x_axis_label);
    free(chart->y_axis_label);
    free(chart->color);
    free(chart);
}