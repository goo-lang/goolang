// Profiling Visualization Main Application
// Integration with the Goo IDE and REPL for real-time profiling visualization

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include "profiling_visualization.h"

// =============================================================================
// Global State
// =============================================================================

static ProfilingVisualization* g_viz = NULL;
static bool g_running = false;
static pthread_t g_update_thread;

// =============================================================================
// Signal Handler
// =============================================================================

void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n🛑 Shutting down profiling visualization...\n");
        g_running = false;
    }
}

// =============================================================================
// Real-time Update Thread
// =============================================================================

void* update_thread_func(void* arg) {
    ProfilingVisualization* viz = (ProfilingVisualization*)arg;
    
    while (g_running && viz->real_time_mode) {
        // Simulate real-time data updates
        VisualizationChart* chart = viz->charts;
        while (chart) {
            double value = 50 + 20 * (rand() % 100) / 100.0;
            char label[32];
            snprintf(label, sizeof(label), "Live_%ld", time(NULL) % 1000);
            
            profiling_visualization_add_data_point(chart, value, label, "live_update");
            chart = chart->next;
        }
        
        if (viz->live_terminal_display) {
            profiling_visualization_render_live_dashboard(viz);
        }
        
        usleep(viz->refresh_interval_ms * 1000);
    }
    
    return NULL;
}

// =============================================================================
// Interactive REPL Mode
// =============================================================================

void run_interactive_mode(ProfilingVisualization* viz) {
    printf("🎯 Goo Profiling Visualization - Interactive Mode\n");
    printf("=================================================\n");
    printf("Type 'help' for available commands, 'exit' to quit\n\n");
    
    char input[256];
    while (g_running) {
        printf("viz> ");
        fflush(stdout);
        
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        
        input[strcspn(input, "\n")] = 0;
        
        if (strlen(input) == 0) {
            continue;
        }
        
        if (strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0) {
            break;
        }
        
        if (strcmp(input, "help") == 0) {
            printf("📊 Available Commands:\n");
            printf("   status         - Show visualization status\n");
            printf("   start          - Start real-time visualization\n");
            printf("   stop           - Stop real-time mode\n");
            printf("   dashboard      - Show live dashboard\n");
            printf("   report         - Generate HTML report\n");
            printf("   demo           - Run demonstration\n");
            printf("   chart <params> - Create new chart\n");
            printf("   config         - Show/modify configuration\n");
            printf("   clear          - Clear screen\n");
            printf("   help           - Show this help\n");
            printf("   exit           - Exit application\n\n");
            continue;
        }
        
        if (strcmp(input, "clear") == 0) {
            printf("\033[2J\033[H");
            continue;
        }
        
        if (strcmp(input, "demo") == 0) {
            profiling_visualization_demo(viz);
            continue;
        }
        
        profiling_visualization_handle_repl_command(viz, NULL, input);
        printf("\n");
    }
}

// =============================================================================
// Dashboard Mode
// =============================================================================

void run_dashboard_mode(ProfilingVisualization* viz) {
    printf("🌐 Starting live dashboard mode...\n");
    printf("Press Ctrl+C to exit\n\n");
    
    profiling_visualization_create_chart(viz, "memory", CHART_LINE, "Memory Usage (MB)");
    profiling_visualization_create_chart(viz, "cpu", CHART_BAR, "CPU Usage (%)");
    profiling_visualization_create_chart(viz, "compile", CHART_LINE, "Compilation Time (ms)");
    
    viz->real_time_mode = true;
    viz->live_terminal_display = true;
    
    pthread_create(&g_update_thread, NULL, update_thread_func, viz);
    
    while (g_running) {
        sleep(1);
    }
    
    pthread_join(g_update_thread, NULL);
}

// =============================================================================
// Report Generation Mode
// =============================================================================

void run_report_mode(ProfilingVisualization* viz) {
    printf("📊 Generating profiling visualization report...\n");
    
    VisualizationChart* memory_chart = profiling_visualization_create_chart(viz, "memory_usage", CHART_LINE, "Memory Usage Over Time");
    VisualizationChart* cpu_chart = profiling_visualization_create_chart(viz, "cpu_usage", CHART_BAR, "CPU Usage Distribution");
    VisualizationChart* parse_chart = profiling_visualization_create_chart(viz, "parse_time", CHART_LINE, "Parse Time Performance");
    VisualizationChart* typecheck_chart = profiling_visualization_create_chart(viz, "typecheck_time", CHART_AREA, "Type Check Performance");
    
    printf("📈 Generating sample performance data...\n");
    
    for (int i = 0; i < 30; i++) {
        double memory_val = 45 + i * 0.8 + (rand() % 15);
        double cpu_val = 25 + 30 * sin(i * 0.3) + (rand() % 20);
        double parse_val = 50 + (rand() % 30) + (i % 10 == 0 ? 100 : 0);
        double typecheck_val = 80 + i * 2 + (rand() % 40);
        
        char label[32];
        snprintf(label, sizeof(label), "Sample_%d", i + 1);
        
        profiling_visualization_add_data_point(memory_chart, memory_val, label, "sample_data");
        profiling_visualization_add_data_point(cpu_chart, cpu_val, label, "sample_data");
        profiling_visualization_add_data_point(parse_chart, parse_val, label, "sample_data");
        profiling_visualization_add_data_point(typecheck_chart, typecheck_val, label, "sample_data");
        
        printf(".");
        fflush(stdout);
        usleep(50000);
    }
    
    printf("\n");
    
    printf("📄 Generating HTML report...\n");
    int result = profiling_visualization_generate_html_report(viz);
    
    if (result == 0) {
        printf("✅ Report generated successfully!\n");
        printf("📂 Location: %s/profiling_report.html\n", viz->output_directory);
        printf("🌐 Open in browser to view interactive charts\n");
    } else {
        printf("❌ Failed to generate report\n");
    }
    
    printf("\n📊 ASCII Preview:\n");
    printf("================\n");
    profiling_visualization_render_ascii_line_chart(viz, memory_chart);
    profiling_visualization_render_ascii_bar_chart(viz, cpu_chart);
    profiling_visualization_render_ascii_sparkline(viz, parse_chart);
    profiling_visualization_render_ascii_sparkline(viz, typecheck_chart);
}

// =============================================================================
// Usage Information
// =============================================================================

void print_usage(const char* program_name) {
    printf("Goo Profiling Visualization Tool\n");
    printf("===============================\n\n");
    printf("Usage: %s [OPTIONS] [MODE]\n\n", program_name);
    printf("Modes:\n");
    printf("  interactive    Interactive command-line mode (default)\n");
    printf("  dashboard      Real-time dashboard mode\n");
    printf("  report         Generate HTML report and exit\n");
    printf("  demo           Run demonstration and exit\n\n");
    printf("Options:\n");
    printf("  -h, --help              Show this help\n");
    printf("  -o, --output <dir>      Output directory (default: ./profiling_visualizations)\n");
    printf("  -r, --refresh <ms>      Refresh interval in milliseconds (default: 1000)\n");
    printf("  --no-color              Disable colored output\n");
    printf("  --no-html               Disable HTML report generation\n");
    printf("  --ascii-only            Use ASCII charts only\n\n");
    printf("Examples:\n");
    printf("  %s                          # Interactive mode\n", program_name);
    printf("  %s dashboard               # Live dashboard\n", program_name);
    printf("  %s report -o ./reports     # Generate report\n", program_name);
    printf("  %s demo --no-color         # Demo without colors\n", program_name);
}

// =============================================================================
// Main Function
// =============================================================================

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    g_running = true;
    
    char* mode = "interactive";
    char* output_dir = NULL;
    int refresh_interval = 1000;
    bool use_colors = true;
    bool generate_html = true;
    bool ascii_only = false;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 < argc) {
                output_dir = argv[++i];
            }
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--refresh") == 0) {
            if (i + 1 < argc) {
                refresh_interval = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "--no-color") == 0) {
            use_colors = false;
        } else if (strcmp(argv[i], "--no-html") == 0) {
            generate_html = false;
        } else if (strcmp(argv[i], "--ascii-only") == 0) {
            ascii_only = true;
        } else if (argv[i][0] != '-') {
            mode = argv[i];
        }
    }
    
    g_viz = profiling_visualization_new();
    if (!g_viz) {
        fprintf(stderr, "Failed to initialize profiling visualization\n");
        return 1;
    }
    
    if (output_dir) {
        free(g_viz->output_directory);
        g_viz->output_directory = strdup(output_dir);
    }
    
    g_viz->refresh_interval_ms = refresh_interval;
    g_viz->use_colors = use_colors;
    g_viz->generate_html = generate_html;
    g_viz->generate_ascii = true;
    
    if (ascii_only) {
        g_viz->generate_html = false;
        g_viz->generate_svg = false;
    }
    
    if (profiling_visualization_init(g_viz) != 0) {
        fprintf(stderr, "Failed to initialize profiling visualization\n");
        profiling_visualization_free(g_viz);
        return 1;
    }
    
    if (strcmp(mode, "interactive") == 0) {
        run_interactive_mode(g_viz);
    } else if (strcmp(mode, "dashboard") == 0) {
        run_dashboard_mode(g_viz);
    } else if (strcmp(mode, "report") == 0) {
        run_report_mode(g_viz);
    } else if (strcmp(mode, "demo") == 0) {
        profiling_visualization_demo(g_viz);
    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        print_usage(argv[0]);
        profiling_visualization_free(g_viz);
        return 1;
    }
    
    printf("\n🧹 Cleaning up...\n");
    profiling_visualization_free(g_viz);
    
    printf("👋 Goodbye!\n");
    return 0;
}