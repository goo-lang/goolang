// Project Health Dashboard Main Application
// Comprehensive project health monitoring and metrics dashboard

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include "project_health_dashboard.h"

// =============================================================================
// Global State
// =============================================================================

static ProjectHealthDashboard* g_dashboard = NULL;
static bool g_running = false;
static pthread_t g_monitor_thread;

// =============================================================================
// Signal Handler
// =============================================================================

void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n🛑 Shutting down project health dashboard...\n");
        g_running = false;
    }
}

// =============================================================================
// Monitoring Thread
// =============================================================================

void* monitoring_thread_func(void* arg) {
    ProjectHealthDashboard* dashboard = (ProjectHealthDashboard*)arg;
    
    while (g_running && dashboard->config.auto_refresh) {
        // Run health checks
        project_health_dashboard_run_all_checks(dashboard);
        
        // Update display if in real-time mode
        if (dashboard->config.real_time_monitoring) {
            project_health_dashboard_display_full_report(dashboard);
        }
        
        // Sleep for refresh interval
        usleep(dashboard->config.refresh_interval_ms * 1000);
    }
    
    return NULL;
}

// =============================================================================
// Interactive Mode
// =============================================================================

void run_interactive_mode(ProjectHealthDashboard* dashboard) {
    printf("🏥 Goo Project Health Dashboard - Interactive Mode\n");
    printf("==================================================\n");
    printf("Type 'help' for available commands, 'exit' to quit\n\n");
    
    char input[256];
    while (g_running) {
        printf("health> ");
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
            printf("   status         - Show current health status\n");
            printf("   check          - Run all health checks\n");
            printf("   metrics        - Show project metrics\n");
            printf("   report         - Display full health report\n");
            printf("   export         - Generate HTML report\n");
            printf("   monitor        - Start real-time monitoring\n");
            printf("   stop           - Stop real-time monitoring\n");
            printf("   config         - Show configuration\n");
            printf("   clear          - Clear screen\n");
            printf("   help           - Show this help\n");
            printf("   exit           - Exit application\n\n");
            continue;
        }
        
        if (strcmp(input, "clear") == 0) {
            printf("\033[2J\033[H");
            continue;
        }
        
        if (strcmp(input, "status") == 0) {
            printf("📊 Project Health Status:\n");
            printf("   Overall Score: %.1f%%\n", dashboard->overall_health_score);
            printf("   Health Status: %s\n", dashboard->health_status);
            printf("   Total Checks: %d\n", dashboard->total_checks);
            printf("   Passed: %d\n", dashboard->passed_checks);
            printf("   Failed: %d\n", dashboard->failed_checks);
            printf("   Project: %s\n\n", dashboard->project_root);
            continue;
        }
        
        if (strcmp(input, "check") == 0) {
            printf("🔍 Running health checks...\n");
            project_health_dashboard_run_all_checks(dashboard);
            printf("✅ Health checks completed!\n");
            printf("📊 Results: %d/%d checks passed (%.1f%%)\n\n",
                   dashboard->passed_checks, dashboard->total_checks,
                   dashboard->total_checks > 0 ? 
                   (100.0 * dashboard->passed_checks / dashboard->total_checks) : 0.0);
            continue;
        }
        
        if (strcmp(input, "metrics") == 0) {
            project_health_dashboard_display_metrics_summary(dashboard);
            continue;
        }
        
        if (strcmp(input, "report") == 0) {
            project_health_dashboard_display_full_report(dashboard);
            continue;
        }
        
        if (strcmp(input, "export") == 0) {
            printf("📄 Generating HTML report...\n");
            int result = project_health_dashboard_export_html_report(dashboard);
            if (result == 0) {
                printf("✅ HTML report generated successfully!\n\n");
            } else {
                printf("❌ Failed to generate HTML report\n\n");
            }
            continue;
        }
        
        if (strcmp(input, "monitor") == 0) {
            if (!dashboard->config.real_time_monitoring) {
                dashboard->config.real_time_monitoring = true;
                dashboard->config.auto_refresh = true;
                printf("🔄 Starting real-time monitoring...\n");
                pthread_create(&g_monitor_thread, NULL, monitoring_thread_func, dashboard);
            } else {
                printf("⚠️  Real-time monitoring is already running\n");
            }
            printf("\n");
            continue;
        }
        
        if (strcmp(input, "stop") == 0) {
            if (dashboard->config.real_time_monitoring) {
                dashboard->config.real_time_monitoring = false;
                dashboard->config.auto_refresh = false;
                printf("⏸️  Stopped real-time monitoring\n");
            } else {
                printf("ℹ️  Real-time monitoring is not running\n");
            }
            printf("\n");
            continue;
        }
        
        if (strcmp(input, "config") == 0) {
            printf("⚙️  Dashboard Configuration:\n");
            printf("   Auto Refresh: %s\n", dashboard->config.auto_refresh ? "ON" : "OFF");
            printf("   Refresh Interval: %d ms\n", dashboard->config.refresh_interval_ms);
            printf("   Real-time Monitoring: %s\n", dashboard->config.real_time_monitoring ? "ON" : "OFF");
            printf("   Export Reports: %s\n", dashboard->config.export_reports ? "ON" : "OFF");
            printf("   Report Directory: %s\n", dashboard->config.report_directory);
            printf("   Show Detailed Metrics: %s\n\n", dashboard->config.show_detailed_metrics ? "ON" : "OFF");
            continue;
        }
        
        printf("❌ Unknown command: %s\n", input);
        printf("💡 Type 'help' for available commands\n\n");
    }
}

// =============================================================================
// Monitor Mode
// =============================================================================

void run_monitor_mode(ProjectHealthDashboard* dashboard) {
    printf("🔄 Starting project health monitoring...\n");
    printf("Press Ctrl+C to exit\n\n");
    
    dashboard->config.real_time_monitoring = true;
    dashboard->config.auto_refresh = true;
    
    // Start monitoring thread
    pthread_create(&g_monitor_thread, NULL, monitoring_thread_func, dashboard);
    
    // Main loop
    while (g_running) {
        sleep(1);
    }
    
    // Wait for monitoring thread to finish
    dashboard->config.auto_refresh = false;
    pthread_join(g_monitor_thread, NULL);
}

// =============================================================================
// Report Mode
// =============================================================================

void run_report_mode(ProjectHealthDashboard* dashboard) {
    printf("📊 Generating project health report...\n");
    
    // Run all health checks
    printf("🔍 Running health checks...\n");
    project_health_dashboard_run_all_checks(dashboard);
    
    // Display full report
    project_health_dashboard_display_full_report(dashboard);
    
    // Generate HTML report
    printf("\n📄 Generating HTML report...\n");
    int result = project_health_dashboard_export_html_report(dashboard);
    
    if (result == 0) {
        printf("✅ HTML report generated successfully!\n");
        printf("📂 Location: %s/health-report.html\n", dashboard->config.report_directory);
        printf("🌐 Open in browser to view detailed report\n");
    } else {
        printf("❌ Failed to generate HTML report\n");
    }
}

// =============================================================================
// Quick Check Mode
// =============================================================================

void run_quick_check_mode(ProjectHealthDashboard* dashboard) {
    printf("⚡ Running quick health check...\n");
    
    // Run health checks
    project_health_dashboard_run_all_checks(dashboard);
    
    // Display summary
    project_health_dashboard_display_header(dashboard);
    
    // Show quick summary
    printf("📈 Quick Summary:\n");
    printf("   ✅ Passed: %d\n", dashboard->passed_checks);
    printf("   ❌ Failed: %d\n", dashboard->failed_checks);
    printf("   📊 Score: %.1f%%\n", dashboard->overall_health_score);
    
    // Show critical issues only
    bool has_critical = false;
    HealthCheckResult* check = dashboard->health_checks;
    while (check) {
        if (!check->passed && 
            (check->severity == HEALTH_SEVERITY_ERROR || check->severity == HEALTH_SEVERITY_CRITICAL)) {
            if (!has_critical) {
                printf("\n🚨 Critical Issues:\n");
                has_critical = true;
            }
            printf("   ❌ %s: %s\n", check->check_name, check->description);
            if (check->recommendation) {
                printf("      💡 %s\n", check->recommendation);
            }
        }
        check = check->next;
    }
    
    if (!has_critical) {
        printf("\n🎉 No critical issues found!\n");
    }
}

// =============================================================================
// Usage Information
// =============================================================================

void print_usage(const char* program_name) {
    printf("Goo Project Health Dashboard\n");
    printf("============================\n\n");
    printf("Usage: %s [OPTIONS] [MODE] [PROJECT_PATH]\n\n", program_name);
    printf("Modes:\n");
    printf("  interactive    Interactive command-line mode (default)\n");
    printf("  monitor        Real-time monitoring mode\n");
    printf("  report         Generate comprehensive health report\n");
    printf("  quick          Quick health check summary\n\n");
    printf("Options:\n");
    printf("  -h, --help              Show this help\n");
    printf("  -o, --output <dir>      Output directory for reports (default: ./health-reports)\n");
    printf("  -r, --refresh <ms>      Refresh interval in milliseconds (default: 5000)\n");
    printf("  --no-export             Disable HTML report generation\n");
    printf("  --no-auto-refresh       Disable automatic refresh\n\n");
    printf("Examples:\n");
    printf("  %s                              # Interactive mode in current directory\n", program_name);
    printf("  %s monitor /path/to/project     # Monitor specific project\n", program_name);
    printf("  %s report -o ./reports          # Generate report with custom output\n", program_name);
    printf("  %s quick                        # Quick health check\n", program_name);
}

// =============================================================================
// Main Function
// =============================================================================

int main(int argc, char* argv[]) {
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    g_running = true;
    
    // Parse command line arguments
    char* mode = "interactive";
    char* project_root = ".";
    char* output_dir = NULL;
    int refresh_interval = 5000;
    bool enable_export = true;
    bool enable_auto_refresh = true;
    
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
        } else if (strcmp(argv[i], "--no-export") == 0) {
            enable_export = false;
        } else if (strcmp(argv[i], "--no-auto-refresh") == 0) {
            enable_auto_refresh = false;
        } else if (argv[i][0] != '-') {
            // First non-option argument is mode, second is project path
            static bool mode_set = false;
            if (!mode_set) {
                mode = argv[i];
                mode_set = true;
            } else {
                project_root = argv[i];
            }
        }
    }
    
    // Initialize dashboard
    g_dashboard = project_health_dashboard_new(project_root);
    if (!g_dashboard) {
        fprintf(stderr, "Failed to initialize project health dashboard\n");
        return 1;
    }
    
    // Configure based on command line arguments
    if (output_dir) {
        free(g_dashboard->config.report_directory);
        g_dashboard->config.report_directory = strdup(output_dir);
    }
    
    g_dashboard->config.refresh_interval_ms = refresh_interval;
    g_dashboard->config.export_reports = enable_export;
    g_dashboard->config.auto_refresh = enable_auto_refresh;
    
    // Initialize
    if (project_health_dashboard_init(g_dashboard) != 0) {
        fprintf(stderr, "Failed to initialize project health dashboard\n");
        project_health_dashboard_free(g_dashboard);
        return 1;
    }
    
    // Run based on mode
    if (strcmp(mode, "interactive") == 0) {
        run_interactive_mode(g_dashboard);
    } else if (strcmp(mode, "monitor") == 0) {
        run_monitor_mode(g_dashboard);
    } else if (strcmp(mode, "report") == 0) {
        run_report_mode(g_dashboard);
    } else if (strcmp(mode, "quick") == 0) {
        run_quick_check_mode(g_dashboard);
    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        print_usage(argv[0]);
        project_health_dashboard_free(g_dashboard);
        return 1;
    }
    
    // Cleanup
    printf("\n🧹 Cleaning up...\n");
    project_health_dashboard_free(g_dashboard);
    
    printf("👋 Goodbye!\n");
    return 0;
}