#ifndef PROJECT_HEALTH_DASHBOARD_H
#define PROJECT_HEALTH_DASHBOARD_H

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct PerformanceMonitor PerformanceMonitor;
typedef struct ProfilingVisualization ProfilingVisualization;
typedef struct REPLContext REPLContext;

// =============================================================================
// Type Definitions
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
// Core Functions
// =============================================================================

// Lifecycle management
ProjectHealthDashboard* project_health_dashboard_new(const char* project_root);
void project_health_dashboard_free(ProjectHealthDashboard* dashboard);
int project_health_dashboard_init(ProjectHealthDashboard* dashboard);

// Health check system
HealthCheckResult* project_health_dashboard_create_check(const char* check_id,
                                                        const char* check_name,
                                                        HealthCategory category,
                                                        HealthSeverity severity);
int project_health_dashboard_add_check(ProjectHealthDashboard* dashboard, HealthCheckResult* check);

// Metrics collection
int project_health_dashboard_collect_code_metrics(ProjectHealthDashboard* dashboard);
int project_health_dashboard_collect_git_metrics(ProjectHealthDashboard* dashboard);
int project_health_dashboard_collect_build_metrics(ProjectHealthDashboard* dashboard);

// Health checks
int project_health_dashboard_check_code_quality(ProjectHealthDashboard* dashboard);
int project_health_dashboard_check_testing(ProjectHealthDashboard* dashboard);
int project_health_dashboard_check_performance(ProjectHealthDashboard* dashboard);

// Dashboard operations
int project_health_dashboard_run_all_checks(ProjectHealthDashboard* dashboard);
void project_health_dashboard_display_full_report(ProjectHealthDashboard* dashboard);

// Display functions
void project_health_dashboard_display_header(ProjectHealthDashboard* dashboard);
void project_health_dashboard_display_metrics_summary(ProjectHealthDashboard* dashboard);
void project_health_dashboard_display_health_checks(ProjectHealthDashboard* dashboard);
void project_health_dashboard_display_recommendations(ProjectHealthDashboard* dashboard);

// Integration functions
int project_health_dashboard_integrate_performance_monitor(ProjectHealthDashboard* dashboard, 
                                                          PerformanceMonitor* monitor);
int project_health_dashboard_integrate_profiling_viz(ProjectHealthDashboard* dashboard,
                                                    ProfilingVisualization* viz);
int project_health_dashboard_integrate_repl(ProjectHealthDashboard* dashboard, REPLContext* repl);

// Export and reporting
int project_health_dashboard_export_html_report(ProjectHealthDashboard* dashboard);

#endif // PROJECT_HEALTH_DASHBOARD_H