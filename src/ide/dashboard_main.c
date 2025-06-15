#include "performance_dashboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

// Global flag for graceful shutdown
static volatile bool g_running = true;

// Signal handler for graceful shutdown
void signal_handler(int signal) {
    (void)signal;
    printf("\nShutting down dashboard server...\n");
    g_running = false;
}

// Simple HTTP server implementation for dashboard
void* http_server_thread(void* arg) {
    (void)arg;
    
    // In a full implementation, this would be a proper HTTP server
    // For this demo, we'll simulate server activity
    while (g_running) {
        // Simulate dashboard updates
        dashboard_update_from_monitor();
        sleep(2);
    }
    
    return NULL;
}

// Demo mode - continuously update metrics
void run_demo_mode(void) {
    printf("Starting demo mode with simulated metrics...\n");
    
    int counter = 0;
    while (g_running) {
        counter++;
        
        // Simulate various metric updates
        dashboard_update_metric("memory_usage", 120.0 + (counter % 100));
        dashboard_update_metric("cpu_usage", 15.0 + (counter % 50));
        dashboard_update_metric("compilation_time", 80.0 + (counter % 40));
        dashboard_update_metric("type_check_time", 30.0 + (counter % 20));
        dashboard_update_metric("repl_commands", (counter % 10) + 1);
        dashboard_update_metric("error_rate", (counter % 20) < 2 ? 1.0 : 0.0);
        
        // Show terminal dashboard every 10 updates
        if (counter % 10 == 0) {
            dashboard_show_terminal_view();
        }
        
        sleep(1);
    }
}

// Interactive terminal mode
void run_interactive_mode(void) {
    char command[256];
    
    printf("\n🚀 Goo Performance Dashboard - Interactive Mode\n");
    printf("Commands:\n");
    printf("  chart <metric>     - Show chart for metric\n");
    printf("  gauge <metric>     - Show gauge for metric\n");
    printf("  list               - List all metrics\n");
    printf("  dashboard          - Show full dashboard\n");
    printf("  update <metric> <value> - Update metric value\n");
    printf("  web                - Start web server\n");
    printf("  demo               - Run demo mode\n");
    printf("  quit               - Exit\n\n");
    
    while (g_running) {
        printf("dashboard> ");
        fflush(stdout);
        
        if (!fgets(command, sizeof(command), stdin)) {
            break;
        }
        
        // Remove newline
        command[strcspn(command, "\n")] = 0;
        
        if (strlen(command) == 0) {
            continue;
        }
        
        char* cmd = strtok(command, " ");
        char* arg1 = strtok(NULL, " ");
        char* arg2 = strtok(NULL, " ");
        
        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        } else if (strcmp(cmd, "list") == 0) {
            printf("\nRegistered Metrics:\n");
            for (int i = 0; i < 10; i++) { // Assuming we have metrics
                const char* metrics[] = {
                    "memory_usage", "cpu_usage", "compilation_time", 
                    "type_check_time", "repl_commands", "error_rate",
                    "gc_collections", "channel_operations", 
                    "error_unions_created", "ownership_transfers"
                };
                if (i < 10) {
                    printf("  %d. %s\n", i + 1, metrics[i]);
                }
            }
            printf("\n");
        } else if (strcmp(cmd, "chart") == 0 && arg1) {
            dashboard_terminal_chart(arg1, 60, 10);
        } else if (strcmp(cmd, "gauge") == 0 && arg1) {
            if (strcmp(arg1, "cpu_usage") == 0) {
                dashboard_terminal_gauge(arg1, 0, 100);
            } else if (strcmp(arg1, "memory_usage") == 0) {
                dashboard_terminal_gauge(arg1, 0, 500);
            } else {
                dashboard_terminal_gauge(arg1, 0, 100);
            }
        } else if (strcmp(cmd, "dashboard") == 0) {
            dashboard_show_terminal_view();
        } else if (strcmp(cmd, "update") == 0 && arg1 && arg2) {
            double value = atof(arg2);
            if (dashboard_update_metric(arg1, value)) {
                printf("Updated %s to %.2f\n", arg1, value);
            } else {
                printf("Failed to update metric %s\n", arg1);
            }
        } else if (strcmp(cmd, "web") == 0) {
            printf("Starting web server on http://localhost:8080\n");
            printf("Note: In this demo, the web server is simulated\n");
            printf("HTML dashboard:\n%s\n", dashboard_generate_html("system"));
        } else if (strcmp(cmd, "demo") == 0) {
            run_demo_mode();
        } else {
            printf("Unknown command: %s\n", cmd);
            printf("Type 'quit' to exit or use the commands listed above.\n");
        }
    }
}

// Print usage information
void print_usage(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -p, --port PORT     Set HTTP server port (default: 8080)\n");
    printf("  -h, --host HOST     Set server host (default: localhost)\n");
    printf("  -d, --demo          Run in demo mode with simulated data\n");
    printf("  -i, --interactive   Run in interactive terminal mode\n");
    printf("  -w, --web           Start web server only\n");
    printf("  --help              Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s --demo                    # Run demo with terminal dashboard\n", program_name);
    printf("  %s --interactive             # Interactive command mode\n", program_name);
    printf("  %s --web --port 3000         # Web server on port 3000\n", program_name);
}

int main(int argc, char* argv[]) {
    int port = 8080;
    char* host = "localhost";
    bool demo_mode = false;
    bool interactive_mode = false;
    bool web_only = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                port = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--host") == 0) {
            if (i + 1 < argc) {
                host = argv[++i];
            }
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--demo") == 0) {
            demo_mode = true;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0) {
            interactive_mode = true;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--web") == 0) {
            web_only = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize dashboard
    printf("Initializing Goo Performance Dashboard...\n");
    if (!performance_dashboard_init(port, host)) {
        fprintf(stderr, "Failed to initialize dashboard\n");
        return 1;
    }
    
    printf("Dashboard initialized successfully\n");
    printf("Port: %d, Host: %s\n", port, host);
    
    // Add some initial data points for demonstration
    dashboard_update_metric("memory_usage", 125.5);
    dashboard_update_metric("cpu_usage", 23.4);
    dashboard_update_metric("compilation_time", 89.2);
    dashboard_update_metric("type_check_time", 34.1);
    dashboard_update_metric("repl_commands", 5.0);
    dashboard_update_metric("error_rate", 0.5);
    
    // Run in appropriate mode
    if (demo_mode) {
        run_demo_mode();
    } else if (interactive_mode) {
        run_interactive_mode();
    } else if (web_only) {
        printf("Starting web server on http://%s:%d\n", host, port);
        printf("Dashboard HTML generated. In a full implementation,\n");
        printf("this would serve the dashboard via HTTP.\n\n");
        
        // Show sample HTML output
        printf("Sample HTML dashboard output:\n");
        printf("=====================================\n");
        char* html = dashboard_generate_html("system");
        printf("%.500s...\n", html); // Show first 500 chars
        printf("=====================================\n");
        
        // Keep server running
        while (g_running) {
            dashboard_update_from_monitor();
            sleep(5);
        }
    } else {
        // Default: show terminal dashboard once and exit
        printf("\n📊 Goo Performance Dashboard\n");
        printf("Generating sample dashboard...\n\n");
        
        dashboard_show_terminal_view();
        
        printf("\nSample JSON data:\n");
        printf("%s\n", dashboard_generate_json_data(NULL));
        
        printf("\nTo run interactively: %s --interactive\n", argv[0]);
        printf("To run demo mode: %s --demo\n", argv[0]);
        printf("To start web server: %s --web\n", argv[0]);
    }
    
    // Cleanup
    performance_dashboard_shutdown();
    printf("Dashboard shutdown complete\n");
    
    return 0;
}