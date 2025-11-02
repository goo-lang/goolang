// Project Health Dashboard - Comprehensive project monitoring and metrics
// Provides real-time project health insights for the Goo development environment

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
#include <dirent.h>
#include <sys/wait.h>

// Forward declarations for integration
typedef struct PerformanceMonitor PerformanceMonitor;
typedef struct ProfilingVisualization ProfilingVisualization;
typedef struct REPLContext REPLContext;

// =============================================================================
// Data Structures
// =============================================================================

// Health check severity levels
typedef enum {
    HEALTH_SEVERITY_INFO,
    HEALTH_SEVERITY_WARNING,
    HEALTH_SEVERITY_ERROR,
    HEALTH_SEVERITY_CRITICAL
} HealthSeverity;

// Health check categories
typedef enum {
    HEALTH_CATEGORY_CODE_QUALITY,
    HEALTH_CATEGORY_PERFORMANCE,
    HEALTH_CATEGORY_SECURITY,
    HEALTH_CATEGORY_TESTING,
    HEALTH_CATEGORY_DEPENDENCIES,
    HEALTH_CATEGORY_BUILD,
    HEALTH_CATEGORY_DOCUMENTATION,
    HEALTH_CATEGORY_VERSION_CONTROL,
    HEALTH_CATEGORY_COUNT
} HealthCategory;

// Health check result
typedef struct HealthCheckResult {
    char* check_id;
    char* check_name;
    char* description;
    HealthCategory category;
    HealthSeverity severity;
    bool passed;
    double score;
    char* details;
    char* recommendation;
    uint64_t timestamp_ms;
    struct HealthCheckResult* next;
} HealthCheckResult;

// Project metrics
typedef struct ProjectMetrics {
    // Code metrics
    int total_files;
    int source_files;
    int test_files;
    int lines_of_code;
    int lines_of_comments;
    int lines_of_tests;
    
    // Quality metrics
    double code_coverage;
    int static_analysis_issues;
    int security_vulnerabilities;
    int code_smells;
    int technical_debt_hours;
    
    // Performance metrics
    double build_time_seconds;
    double test_execution_time;
    double memory_usage_mb;
    double cpu_usage_percent;
    
    // Development metrics
    int commits_last_week;
    int active_branches;
    int open_issues;
    int pull_requests;
    int contributors;
    
    // Dependency metrics
    int total_dependencies;
    int outdated_dependencies;
    int vulnerable_dependencies;
    
    uint64_t last_updated_ms;
} ProjectMetrics;

// Dashboard configuration
typedef struct DashboardConfig {
    bool auto_refresh;
    int refresh_interval_ms;
    bool enable_notifications;
    bool show_detailed_metrics;
    bool export_reports;
    char* report_directory;
    HealthSeverity alert_threshold;
    bool real_time_monitoring;
} DashboardConfig;

// Project health dashboard
typedef struct ProjectHealthDashboard {
    bool is_enabled;
    bool is_running;
    char* project_root;
    
    // Health checks
    HealthCheckResult* health_checks;
    int total_checks;
    int passed_checks;
    int failed_checks;
    
    // Metrics
    ProjectMetrics current_metrics;
    ProjectMetrics* historical_metrics;
    int metrics_history_count;
    int max_history_count;
    
    // Configuration
    DashboardConfig config;
    
    // Integration
    PerformanceMonitor* performance_monitor;
    ProfilingVisualization* profiling_viz;
    REPLContext* repl;
    
    // Overall health score
    double overall_health_score;
    char* health_status;
    
} ProjectHealthDashboard;

// =============================================================================
// Color Constants
// =============================================================================

#define DASH_COLOR_RESET   "\033[0m"
#define DASH_COLOR_BOLD    "\033[1m"
#define DASH_COLOR_RED     "\033[31m"
#define DASH_COLOR_GREEN   "\033[32m"
#define DASH_COLOR_YELLOW  "\033[33m"
#define DASH_COLOR_BLUE    "\033[34m"
#define DASH_COLOR_MAGENTA "\033[35m"
#define DASH_COLOR_CYAN    "\033[36m"
#define DASH_COLOR_WHITE   "\033[37m"

// =============================================================================
// Utility Functions
// =============================================================================

static uint64_t dashboard_get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static char* dashboard_duplicate_string(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

static void dashboard_print_colored(const char* color, const char* format, ...) {
    printf("%s", color);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("%s", DASH_COLOR_RESET);
}

static const char* health_severity_to_string(HealthSeverity severity) {
    switch (severity) {
        case HEALTH_SEVERITY_INFO: return "INFO";
        case HEALTH_SEVERITY_WARNING: return "WARNING";
        case HEALTH_SEVERITY_ERROR: return "ERROR";
        case HEALTH_SEVERITY_CRITICAL: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

static const char* health_severity_to_color(HealthSeverity severity) {
    switch (severity) {
        case HEALTH_SEVERITY_INFO: return DASH_COLOR_BLUE;
        case HEALTH_SEVERITY_WARNING: return DASH_COLOR_YELLOW;
        case HEALTH_SEVERITY_ERROR: return DASH_COLOR_RED;
        case HEALTH_SEVERITY_CRITICAL: return DASH_COLOR_BOLD DASH_COLOR_RED;
        default: return DASH_COLOR_WHITE;
    }
}

static const char* health_category_to_string(HealthCategory category) {
    switch (category) {
        case HEALTH_CATEGORY_CODE_QUALITY: return "Code Quality";
        case HEALTH_CATEGORY_PERFORMANCE: return "Performance";
        case HEALTH_CATEGORY_SECURITY: return "Security";
        case HEALTH_CATEGORY_TESTING: return "Testing";
        case HEALTH_CATEGORY_DEPENDENCIES: return "Dependencies";
        case HEALTH_CATEGORY_BUILD: return "Build";
        case HEALTH_CATEGORY_DOCUMENTATION: return "Documentation";
        case HEALTH_CATEGORY_VERSION_CONTROL: return "Version Control";
        default: return "Unknown";
    }
}

// =============================================================================
// Dashboard Lifecycle
// =============================================================================

ProjectHealthDashboard* project_health_dashboard_new(const char* project_root) {
    ProjectHealthDashboard* dashboard = calloc(1, sizeof(ProjectHealthDashboard));
    if (!dashboard) return NULL;
    
    dashboard->project_root = dashboard_duplicate_string(project_root ? project_root : ".");
    dashboard->is_enabled = true;
    dashboard->is_running = false;
    dashboard->max_history_count = 100;
    
    // Initialize configuration
    dashboard->config.auto_refresh = true;
    dashboard->config.refresh_interval_ms = 5000; // 5 seconds
    dashboard->config.enable_notifications = true;
    dashboard->config.show_detailed_metrics = true;
    dashboard->config.export_reports = true;
    dashboard->config.report_directory = dashboard_duplicate_string("./health-reports");
    dashboard->config.alert_threshold = HEALTH_SEVERITY_WARNING;
    dashboard->config.real_time_monitoring = false;
    
    // Initialize metrics
    dashboard->current_metrics.last_updated_ms = dashboard_get_timestamp_ms();
    dashboard->overall_health_score = 0.0;
    dashboard->health_status = dashboard_duplicate_string("Unknown");
    
    return dashboard;
}

void project_health_dashboard_free(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return;
    
    // Free health checks
    HealthCheckResult* check = dashboard->health_checks;
    while (check) {
        HealthCheckResult* next = check->next;
        free(check->check_id);
        free(check->check_name);
        free(check->description);
        free(check->details);
        free(check->recommendation);
        free(check);
        check = next;
    }
    
    // Free historical metrics
    free(dashboard->historical_metrics);
    
    // Free strings
    free(dashboard->project_root);
    free(dashboard->config.report_directory);
    free(dashboard->health_status);
    
    free(dashboard);
}

int project_health_dashboard_init(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return -1;
    
    // Create report directory
    if (dashboard->config.report_directory) {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "mkdir -p %s", dashboard->config.report_directory);
        system(tmp);
    }
    
    dashboard->is_enabled = true;
    return 0;
}

// =============================================================================
// Health Check System
// =============================================================================

HealthCheckResult* project_health_dashboard_create_check(const char* check_id,
                                                        const char* check_name,
                                                        HealthCategory category,
                                                        HealthSeverity severity) {
    HealthCheckResult* check = calloc(1, sizeof(HealthCheckResult));
    if (!check) return NULL;
    
    check->check_id = dashboard_duplicate_string(check_id);
    check->check_name = dashboard_duplicate_string(check_name);
    check->category = category;
    check->severity = severity;
    check->timestamp_ms = dashboard_get_timestamp_ms();
    check->passed = false;
    check->score = 0.0;
    
    return check;
}

int project_health_dashboard_add_check(ProjectHealthDashboard* dashboard, HealthCheckResult* check) {
    if (!dashboard || !check) return -1;
    
    check->next = dashboard->health_checks;
    dashboard->health_checks = check;
    dashboard->total_checks++;
    
    if (check->passed) {
        dashboard->passed_checks++;
    } else {
        dashboard->failed_checks++;
    }
    
    return 0;
}

// =============================================================================
// Metrics Collection
// =============================================================================

int project_health_dashboard_collect_code_metrics(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return -1;
    
    ProjectMetrics* metrics = &dashboard->current_metrics;
    
    // Count files and lines
    char command[512];
    FILE* pipe;
    
    // Count source files
    snprintf(command, sizeof(command), 
             "find %s -name '*.goo' -o -name '*.c' -o -name '*.h' | wc -l", 
             dashboard->project_root);
    pipe = popen(command, "r");
    if (pipe) {
        fscanf(pipe, "%d", &metrics->source_files);
        pclose(pipe);
    }
    
    // Count test files
    snprintf(command, sizeof(command),
             "find %s -name '*test*.goo' -o -name '*test*.c' | wc -l",
             dashboard->project_root);
    pipe = popen(command, "r");
    if (pipe) {
        fscanf(pipe, "%d", &metrics->test_files);
        pclose(pipe);
    }
    
    // Count lines of code
    snprintf(command, sizeof(command),
             "find %s -name '*.goo' -o -name '*.c' -o -name '*.h' | xargs wc -l | tail -1 | awk '{print $1}'",
             dashboard->project_root);
    pipe = popen(command, "r");
    if (pipe) {
        fscanf(pipe, "%d", &metrics->lines_of_code);
        pclose(pipe);
    }
    
    metrics->total_files = metrics->source_files + metrics->test_files;
    metrics->last_updated_ms = dashboard_get_timestamp_ms();
    
    return 0;
}

int project_health_dashboard_collect_git_metrics(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return -1;
    
    ProjectMetrics* metrics = &dashboard->current_metrics;
    char command[512];
    FILE* pipe;
    
    // Count commits in last week
    snprintf(command, sizeof(command),
             "cd %s && git rev-list --count --since='1 week ago' HEAD 2>/dev/null || echo 0",
             dashboard->project_root);
    pipe = popen(command, "r");
    if (pipe) {
        fscanf(pipe, "%d", &metrics->commits_last_week);
        pclose(pipe);
    }
    
    // Count active branches
    snprintf(command, sizeof(command),
             "cd %s && git branch -a 2>/dev/null | wc -l || echo 0",
             dashboard->project_root);
    pipe = popen(command, "r");
    if (pipe) {
        fscanf(pipe, "%d", &metrics->active_branches);
        pclose(pipe);
    }
    
    // Count contributors
    snprintf(command, sizeof(command),
             "cd %s && git shortlog -sn 2>/dev/null | wc -l || echo 0",
             dashboard->project_root);
    pipe = popen(command, "r");
    if (pipe) {
        fscanf(pipe, "%d", &metrics->contributors);
        pclose(pipe);
    }
    
    return 0;
}

int project_health_dashboard_collect_build_metrics(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return -1;
    
    ProjectMetrics* metrics = &dashboard->current_metrics;
    
    // Measure build time
    uint64_t start_time = dashboard_get_timestamp_ms();
    
    char command[256];
    snprintf(command, sizeof(command), "cd %s && make clean >/dev/null 2>&1 && make >/dev/null 2>&1", 
             dashboard->project_root);
    
    int build_result = system(command);
    uint64_t end_time = dashboard_get_timestamp_ms();
    
    metrics->build_time_seconds = (double)(end_time - start_time) / 1000.0;
    
    // Simple build success check
    if (build_result == 0) {
        // Build succeeded
        HealthCheckResult* check = project_health_dashboard_create_check(
            "build_success", "Build Success", HEALTH_CATEGORY_BUILD, HEALTH_SEVERITY_INFO);
        check->passed = true;
        check->score = 100.0;
        check->description = dashboard_duplicate_string("Project builds successfully");
        check->details = dashboard_duplicate_string("No build errors detected");
        project_health_dashboard_add_check(dashboard, check);
    } else {
        // Build failed
        HealthCheckResult* check = project_health_dashboard_create_check(
            "build_failure", "Build Failure", HEALTH_CATEGORY_BUILD, HEALTH_SEVERITY_ERROR);
        check->passed = false;
        check->score = 0.0;
        check->description = dashboard_duplicate_string("Project build failed");
        check->details = dashboard_duplicate_string("Build errors detected - check compilation logs");
        check->recommendation = dashboard_duplicate_string("Fix compilation errors and ensure all dependencies are available");
        project_health_dashboard_add_check(dashboard, check);
    }
    
    return 0;
}

// =============================================================================
// Health Check Implementations
// =============================================================================

int project_health_dashboard_check_code_quality(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return -1;
    
    ProjectMetrics* metrics = &dashboard->current_metrics;
    
    // Check code-to-comment ratio
    double comment_ratio = metrics->lines_of_code > 0 ? 
        (double)metrics->lines_of_comments / metrics->lines_of_code : 0.0;
    
    if (comment_ratio >= 0.2) {
        HealthCheckResult* check = project_health_dashboard_create_check(
            "comment_ratio", "Comment Coverage", HEALTH_CATEGORY_CODE_QUALITY, HEALTH_SEVERITY_INFO);
        check->passed = true;
        check->score = comment_ratio * 100.0;
        check->description = dashboard_duplicate_string("Good comment coverage");
        char details[256];
        snprintf(details, sizeof(details), "Comment ratio: %.1f%% (target: 20%%)", comment_ratio * 100);
        check->details = dashboard_duplicate_string(details);
        project_health_dashboard_add_check(dashboard, check);
    } else {
        HealthCheckResult* check = project_health_dashboard_create_check(
            "comment_ratio", "Comment Coverage", HEALTH_CATEGORY_CODE_QUALITY, HEALTH_SEVERITY_WARNING);
        check->passed = false;
        check->score = comment_ratio * 100.0;
        check->description = dashboard_duplicate_string("Low comment coverage");
        char details[256];
        snprintf(details, sizeof(details), "Comment ratio: %.1f%% (target: 20%%)", comment_ratio * 100);
        check->details = dashboard_duplicate_string(details);
        check->recommendation = dashboard_duplicate_string("Add more comments to explain complex code sections");
        project_health_dashboard_add_check(dashboard, check);
    }
    
    // Check file organization
    if (metrics->source_files > 0) {
        double files_per_directory = (double)metrics->source_files / 10.0; // Assume ~10 directories
        
        HealthCheckResult* check = project_health_dashboard_create_check(
            "file_organization", "File Organization", HEALTH_CATEGORY_CODE_QUALITY, HEALTH_SEVERITY_INFO);
        check->passed = files_per_directory <= 20.0;
        check->score = check->passed ? 100.0 : 70.0;
        check->description = dashboard_duplicate_string("File organization analysis");
        char details[256];
        snprintf(details, sizeof(details), "Total source files: %d", metrics->source_files);
        check->details = dashboard_duplicate_string(details);
        if (!check->passed) {
            check->recommendation = dashboard_duplicate_string("Consider organizing files into more directories for better structure");
        }
        project_health_dashboard_add_check(dashboard, check);
    }
    
    return 0;
}

int project_health_dashboard_check_testing(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return -1;
    
    ProjectMetrics* metrics = &dashboard->current_metrics;
    
    // Check test-to-source ratio
    double test_ratio = metrics->source_files > 0 ? 
        (double)metrics->test_files / metrics->source_files : 0.0;
    
    if (test_ratio >= 0.5) {
        HealthCheckResult* check = project_health_dashboard_create_check(
            "test_coverage", "Test Coverage", HEALTH_CATEGORY_TESTING, HEALTH_SEVERITY_INFO);
        check->passed = true;
        check->score = test_ratio * 100.0;
        check->description = dashboard_duplicate_string("Good test coverage");
        char details[256];
        snprintf(details, sizeof(details), "Test files: %d, Source files: %d (ratio: %.1f%%)", 
                metrics->test_files, metrics->source_files, test_ratio * 100);
        check->details = dashboard_duplicate_string(details);
        project_health_dashboard_add_check(dashboard, check);
    } else {
        HealthCheckResult* check = project_health_dashboard_create_check(
            "test_coverage", "Test Coverage", HEALTH_CATEGORY_TESTING, HEALTH_SEVERITY_WARNING);
        check->passed = false;
        check->score = test_ratio * 100.0;
        check->description = dashboard_duplicate_string("Insufficient test coverage");
        char details[256];
        snprintf(details, sizeof(details), "Test files: %d, Source files: %d (ratio: %.1f%%, target: 50%%)", 
                metrics->test_files, metrics->source_files, test_ratio * 100);
        check->details = dashboard_duplicate_string(details);
        check->recommendation = dashboard_duplicate_string("Add more test files to improve test coverage");
        project_health_dashboard_add_check(dashboard, check);
    }
    
    return 0;
}

int project_health_dashboard_check_performance(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return -1;
    
    ProjectMetrics* metrics = &dashboard->current_metrics;
    
    // Check build time
    if (metrics->build_time_seconds > 0) {
        bool fast_build = metrics->build_time_seconds <= 30.0; // 30 seconds threshold
        
        HealthCheckResult* check = project_health_dashboard_create_check(
            "build_time", "Build Performance", HEALTH_CATEGORY_PERFORMANCE, 
            fast_build ? HEALTH_SEVERITY_INFO : HEALTH_SEVERITY_WARNING);
        check->passed = fast_build;
        check->score = fast_build ? 100.0 : 60.0;
        check->description = dashboard_duplicate_string("Build time analysis");
        char details[256];
        snprintf(details, sizeof(details), "Build time: %.2f seconds", metrics->build_time_seconds);
        check->details = dashboard_duplicate_string(details);
        if (!fast_build) {
            check->recommendation = dashboard_duplicate_string("Consider optimizing build process or using incremental builds");
        }
        project_health_dashboard_add_check(dashboard, check);
    }
    
    return 0;
}

// =============================================================================
// Dashboard Display
// =============================================================================

void project_health_dashboard_display_header(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return;
    
    dashboard_print_colored(DASH_COLOR_BOLD DASH_COLOR_CYAN, 
                           "🏥 Goo Project Health Dashboard\n");
    dashboard_print_colored(DASH_COLOR_CYAN, 
                           "===============================\n");
    
    printf("📂 Project: %s\n", dashboard->project_root);
    printf("🕐 Last Update: ");
    
    time_t now = time(NULL);
    char* time_str = ctime(&now);
    time_str[strlen(time_str) - 1] = '\0';
    printf("%s\n", time_str);
    
    // Overall health score
    const char* health_color = DASH_COLOR_GREEN;
    const char* health_icon = "✅";
    
    if (dashboard->overall_health_score < 60.0) {
        health_color = DASH_COLOR_RED;
        health_icon = "❌";
    } else if (dashboard->overall_health_score < 80.0) {
        health_color = DASH_COLOR_YELLOW;
        health_icon = "⚠️";
    }
    
    printf("🎯 Overall Health: ");
    dashboard_print_colored(health_color, "%s %.1f%%\n", health_icon, dashboard->overall_health_score);
    printf("\n");
}

void project_health_dashboard_display_metrics_summary(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return;
    
    ProjectMetrics* metrics = &dashboard->current_metrics;
    
    dashboard_print_colored(DASH_COLOR_BOLD DASH_COLOR_BLUE, "📊 Project Metrics Summary\n");
    dashboard_print_colored(DASH_COLOR_BLUE, "==========================\n");
    
    printf("📄 Source Files:     %d\n", metrics->source_files);
    printf("🧪 Test Files:       %d\n", metrics->test_files);
    printf("📝 Lines of Code:    %d\n", metrics->lines_of_code);
    printf("⏱️  Build Time:       %.2f seconds\n", metrics->build_time_seconds);
    printf("🔀 Commits (7 days): %d\n", metrics->commits_last_week);
    printf("🌿 Active Branches:  %d\n", metrics->active_branches);
    printf("👥 Contributors:     %d\n", metrics->contributors);
    printf("\n");
}

void project_health_dashboard_display_health_checks(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return;
    
    dashboard_print_colored(DASH_COLOR_BOLD DASH_COLOR_MAGENTA, "🔍 Health Check Results\n");
    dashboard_print_colored(DASH_COLOR_MAGENTA, "=======================\n");
    
    // Group checks by category
    for (int cat = 0; cat < HEALTH_CATEGORY_COUNT; cat++) {
        bool category_has_checks = false;
        
        // Check if this category has any checks
        HealthCheckResult* check = dashboard->health_checks;
        while (check) {
            if (check->category == cat) {
                category_has_checks = true;
                break;
            }
            check = check->next;
        }
        
        if (!category_has_checks) continue;
        
        printf("\n");
        dashboard_print_colored(DASH_COLOR_BOLD, "📋 %s\n", health_category_to_string(cat));
        printf("   ────────────────\n");
        
        check = dashboard->health_checks;
        while (check) {
            if (check->category == cat) {
                const char* status_icon = check->passed ? "✅" : "❌";
                const char* severity_color = health_severity_to_color(check->severity);
                
                printf("   %s ", status_icon);
                dashboard_print_colored(severity_color, "%s", check->check_name);
                printf(" (%.1f%%)\n", check->score);
                
                if (check->details) {
                    printf("      📝 %s\n", check->details);
                }
                
                if (!check->passed && check->recommendation) {
                    dashboard_print_colored(DASH_COLOR_YELLOW, "      💡 %s\n", check->recommendation);
                }
            }
            check = check->next;
        }
    }
    
    printf("\n");
    printf("📈 Summary: %d/%d checks passed (%.1f%%)\n", 
           dashboard->passed_checks, dashboard->total_checks,
           dashboard->total_checks > 0 ? (100.0 * dashboard->passed_checks / dashboard->total_checks) : 0.0);
    printf("\n");
}

void project_health_dashboard_display_recommendations(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return;
    
    dashboard_print_colored(DASH_COLOR_BOLD DASH_COLOR_YELLOW, "💡 Recommendations\n");
    dashboard_print_colored(DASH_COLOR_YELLOW, "==================\n");
    
    int recommendation_count = 0;
    HealthCheckResult* check = dashboard->health_checks;
    
    while (check) {
        if (!check->passed && check->recommendation) {
            recommendation_count++;
            printf("%d. ", recommendation_count);
            dashboard_print_colored(health_severity_to_color(check->severity), 
                                   "[%s] ", health_severity_to_string(check->severity));
            printf("%s\n", check->recommendation);
            printf("   → Related to: %s\n\n", check->check_name);
        }
        check = check->next;
    }
    
    if (recommendation_count == 0) {
        dashboard_print_colored(DASH_COLOR_GREEN, "🎉 Great job! No recommendations at this time.\n\n");
    }
}

// =============================================================================
// Main Dashboard Operations
// =============================================================================

int project_health_dashboard_run_all_checks(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return -1;
    
    // Clear previous checks
    HealthCheckResult* check = dashboard->health_checks;
    while (check) {
        HealthCheckResult* next = check->next;
        free(check->check_id);
        free(check->check_name);
        free(check->description);
        free(check->details);
        free(check->recommendation);
        free(check);
        check = next;
    }
    dashboard->health_checks = NULL;
    dashboard->total_checks = 0;
    dashboard->passed_checks = 0;
    dashboard->failed_checks = 0;
    
    // Collect metrics
    project_health_dashboard_collect_code_metrics(dashboard);
    project_health_dashboard_collect_git_metrics(dashboard);
    project_health_dashboard_collect_build_metrics(dashboard);
    
    // Run health checks
    project_health_dashboard_check_code_quality(dashboard);
    project_health_dashboard_check_testing(dashboard);
    project_health_dashboard_check_performance(dashboard);
    
    // Calculate overall health score
    if (dashboard->total_checks > 0) {
        dashboard->overall_health_score = (100.0 * dashboard->passed_checks) / dashboard->total_checks;
    } else {
        dashboard->overall_health_score = 0.0;
    }
    
    // Update health status
    free(dashboard->health_status);
    if (dashboard->overall_health_score >= 90.0) {
        dashboard->health_status = dashboard_duplicate_string("Excellent");
    } else if (dashboard->overall_health_score >= 75.0) {
        dashboard->health_status = dashboard_duplicate_string("Good");
    } else if (dashboard->overall_health_score >= 60.0) {
        dashboard->health_status = dashboard_duplicate_string("Fair");
    } else {
        dashboard->health_status = dashboard_duplicate_string("Poor");
    }
    
    return 0;
}

void project_health_dashboard_display_full_report(ProjectHealthDashboard* dashboard) {
    if (!dashboard) return;
    
    // Clear screen
    printf("\033[2J\033[H");
    
    project_health_dashboard_display_header(dashboard);
    project_health_dashboard_display_metrics_summary(dashboard);
    project_health_dashboard_display_health_checks(dashboard);
    project_health_dashboard_display_recommendations(dashboard);
    
    dashboard_print_colored(DASH_COLOR_CYAN, "────────────────────────────────────────\n");
    dashboard_print_colored(DASH_COLOR_CYAN, "🔄 Auto-refresh: %s | ⚡ Interval: %ds\n", 
                           dashboard->config.auto_refresh ? "ON" : "OFF",
                           dashboard->config.refresh_interval_ms / 1000);
    dashboard_print_colored(DASH_COLOR_CYAN, "📊 Use 'dashboard interactive' for controls\n");
}

// =============================================================================
// Integration Functions
// =============================================================================

int project_health_dashboard_integrate_performance_monitor(ProjectHealthDashboard* dashboard, 
                                                          PerformanceMonitor* monitor) {
    if (!dashboard) return -1;
    dashboard->performance_monitor = monitor;
    return 0;
}

int project_health_dashboard_integrate_profiling_viz(ProjectHealthDashboard* dashboard,
                                                    ProfilingVisualization* viz) {
    if (!dashboard) return -1;
    dashboard->profiling_viz = viz;
    return 0;
}

int project_health_dashboard_integrate_repl(ProjectHealthDashboard* dashboard, REPLContext* repl) {
    if (!dashboard) return -1;
    dashboard->repl = repl;
    return 0;
}

// =============================================================================
// Export and Reporting
// =============================================================================

int project_health_dashboard_export_html_report(ProjectHealthDashboard* dashboard) {
    if (!dashboard || !dashboard->config.export_reports) return -1;
    
    char report_path[512];
    snprintf(report_path, sizeof(report_path), "%s/health-report.html", dashboard->config.report_directory);
    
    FILE* html = fopen(report_path, "w");
    if (!html) return -1;
    
    // HTML header
    fprintf(html, "<!DOCTYPE html>\n");
    fprintf(html, "<html lang=\"en\">\n");
    fprintf(html, "<head>\n");
    fprintf(html, "    <meta charset=\"UTF-8\">\n");
    fprintf(html, "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
    fprintf(html, "    <title>Project Health Dashboard - %s</title>\n", dashboard->project_root);
    fprintf(html, "    <style>\n");
    fprintf(html, "        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif; margin: 0; padding: 20px; background: #f8f9fa; }\n");
    fprintf(html, "        .container { max-width: 1200px; margin: 0 auto; }\n");
    fprintf(html, "        .header { background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); color: white; padding: 30px; border-radius: 12px; margin-bottom: 30px; }\n");
    fprintf(html, "        .metrics-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 20px; margin-bottom: 30px; }\n");
    fprintf(html, "        .metric-card { background: white; border-radius: 8px; padding: 20px; text-align: center; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }\n");
    fprintf(html, "        .metric-value { font-size: 2em; font-weight: bold; color: #667eea; }\n");
    fprintf(html, "        .metric-label { color: #666; margin-top: 10px; }\n");
    fprintf(html, "        .health-section { background: white; border-radius: 8px; padding: 20px; margin-bottom: 20px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }\n");
    fprintf(html, "        .check-item { padding: 10px; border-left: 4px solid #e9ecef; margin-bottom: 10px; }\n");
    fprintf(html, "        .check-passed { border-left-color: #28a745; background: #f8fff9; }\n");
    fprintf(html, "        .check-failed { border-left-color: #dc3545; background: #fff8f8; }\n");
    fprintf(html, "        .health-score { font-size: 3em; font-weight: bold; text-align: center; margin: 20px 0; }\n");
    fprintf(html, "        .score-excellent { color: #28a745; }\n");
    fprintf(html, "        .score-good { color: #ffc107; }\n");
    fprintf(html, "        .score-poor { color: #dc3545; }\n");
    fprintf(html, "    </style>\n");
    fprintf(html, "</head>\n");
    fprintf(html, "<body>\n");
    fprintf(html, "    <div class=\"container\">\n");
    
    // Header
    time_t now = time(NULL);
    char* time_str = ctime(&now);
    time_str[strlen(time_str) - 1] = '\0';
    
    fprintf(html, "        <div class=\"header\">\n");
    fprintf(html, "            <h1>🏥 Project Health Dashboard</h1>\n");
    fprintf(html, "            <p>Project: %s</p>\n", dashboard->project_root);
    fprintf(html, "            <p>Generated on %s</p>\n", time_str);
    fprintf(html, "        </div>\n");
    
    // Overall health score
    const char* score_class = "score-good";
    if (dashboard->overall_health_score >= 90.0) score_class = "score-excellent";
    else if (dashboard->overall_health_score < 60.0) score_class = "score-poor";
    
    fprintf(html, "        <div class=\"health-section\">\n");
    fprintf(html, "            <div class=\"health-score %s\">%.1f%%</div>\n", score_class, dashboard->overall_health_score);
    fprintf(html, "            <p style=\"text-align: center;\">Overall Health Score (%s)</p>\n", dashboard->health_status);
    fprintf(html, "        </div>\n");
    
    // Metrics
    ProjectMetrics* metrics = &dashboard->current_metrics;
    fprintf(html, "        <div class=\"metrics-grid\">\n");
    fprintf(html, "            <div class=\"metric-card\">\n");
    fprintf(html, "                <div class=\"metric-value\">%d</div>\n", metrics->source_files);
    fprintf(html, "                <div class=\"metric-label\">Source Files</div>\n");
    fprintf(html, "            </div>\n");
    fprintf(html, "            <div class=\"metric-card\">\n");
    fprintf(html, "                <div class=\"metric-value\">%d</div>\n", metrics->test_files);
    fprintf(html, "                <div class=\"metric-label\">Test Files</div>\n");
    fprintf(html, "            </div>\n");
    fprintf(html, "            <div class=\"metric-card\">\n");
    fprintf(html, "                <div class=\"metric-value\">%d</div>\n", metrics->lines_of_code);
    fprintf(html, "                <div class=\"metric-label\">Lines of Code</div>\n");
    fprintf(html, "            </div>\n");
    fprintf(html, "            <div class=\"metric-card\">\n");
    fprintf(html, "                <div class=\"metric-value\">%.1fs</div>\n", metrics->build_time_seconds);
    fprintf(html, "                <div class=\"metric-label\">Build Time</div>\n");
    fprintf(html, "            </div>\n");
    fprintf(html, "        </div>\n");
    
    // Health checks
    fprintf(html, "        <div class=\"health-section\">\n");
    fprintf(html, "            <h3>Health Check Results</h3>\n");
    
    HealthCheckResult* check = dashboard->health_checks;
    while (check) {
        const char* check_class = check->passed ? "check-passed" : "check-failed";
        const char* status_icon = check->passed ? "✅" : "❌";
        
        fprintf(html, "            <div class=\"check-item %s\">\n", check_class);
        fprintf(html, "                <strong>%s %s</strong> (%.1f%%)<br>\n", status_icon, check->check_name, check->score);
        if (check->details) {
            fprintf(html, "                <small>%s</small><br>\n", check->details);
        }
        if (!check->passed && check->recommendation) {
            fprintf(html, "                <em>💡 %s</em>\n", check->recommendation);
        }
        fprintf(html, "            </div>\n");
        check = check->next;
    }
    
    fprintf(html, "        </div>\n");
    fprintf(html, "    </div>\n");
    fprintf(html, "</body>\n");
    fprintf(html, "</html>\n");
    
    fclose(html);
    
    printf("📄 Health report generated: %s\n", report_path);
    return 0;
}