#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <glob.h>

typedef struct {
    int total_files;
    int total_lines;
    int total_functions;
    int total_tests;
    double test_coverage;
    int build_status; // 0 = failed, 1 = success
    int warnings;
    int errors;
    time_t last_commit;
    char git_branch[256];
} ProjectHealth;

typedef struct {
    char metric_name[128];
    double current_value;
    double previous_value;
    double target_value;
    int trend; // -1 = down, 0 = stable, 1 = up
} HealthMetric;

void init_project_health(ProjectHealth* health) {
    health->total_files = 0;
    health->total_lines = 0;
    health->total_functions = 0;
    health->total_tests = 0;
    health->test_coverage = 0.0;
    health->build_status = 0;
    health->warnings = 0;
    health->errors = 0;
    health->last_commit = 0;
    strcpy(health->git_branch, "unknown");
}

void scan_goo_file_metrics(const char* filepath, ProjectHealth* health) {
    FILE* file = fopen(filepath, "r");
    if (!file) return;
    
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        health->total_lines++;
        
        // Count functions
        if (strstr(line, "func ")) {
            health->total_functions++;
        }
        
        // Count test functions
        if (strstr(line, "func Test") || strstr(line, "func test")) {
            health->total_tests++;
        }
    }
    
    fclose(file);
}

void analyze_project_structure(ProjectHealth* health) {
    printf("🔍 Analyzing project structure...\n");
    
    // Scan for .goo files
    glob_t glob_result;
    int glob_status = glob("**/*.goo", GLOB_NOSORT, NULL, &glob_result);
    
    if (glob_status == 0) {
        health->total_files = glob_result.gl_pathc;
        
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
            scan_goo_file_metrics(glob_result.gl_pathv[i], health);
        }
        
        globfree(&glob_result);
    }
    
    // Check build status (simulate)
    health->build_status = (access("bin/goo", F_OK) == 0) ? 1 : 0;
    
    // Get git info
    FILE* git_branch = popen("git branch --show-current 2>/dev/null", "r");
    if (git_branch) {
        if (fgets(health->git_branch, sizeof(health->git_branch), git_branch)) {
            // Remove newline
            health->git_branch[strcspn(health->git_branch, "\n")] = '\0';
        }
        pclose(git_branch);
    }
    
    // Get last commit time
    FILE* git_log = popen("git log -1 --format=%ct 2>/dev/null", "r");
    if (git_log) {
        char timestamp[64];
        if (fgets(timestamp, sizeof(timestamp), git_log)) {
            health->last_commit = atol(timestamp);
        }
        pclose(git_log);
    }
    
    // Calculate test coverage (simplified)
    if (health->total_functions > 0) {
        health->test_coverage = ((double)health->total_tests / health->total_functions) * 100.0;
        if (health->test_coverage > 100.0) health->test_coverage = 100.0;
    }
}

void print_health_dashboard(const ProjectHealth* health) {
    printf("\\n📊 Project Health Dashboard\\n");
    printf("============================\\n\\n");
    
    // Overall health score
    int health_score = 0;
    if (health->build_status) health_score += 30;
    if (health->test_coverage > 70) health_score += 25;
    else if (health->test_coverage > 50) health_score += 15;
    else if (health->test_coverage > 30) health_score += 10;
    if (health->errors == 0) health_score += 20;
    if (health->warnings < 5) health_score += 15;
    if (health->total_tests > 0) health_score += 10;
    
    const char* health_status;
    const char* health_color;
    if (health_score >= 80) {
        health_status = "Excellent";
        health_color = "\\033[32m"; // Green
    } else if (health_score >= 60) {
        health_status = "Good";
        health_color = "\\033[33m"; // Yellow
    } else if (health_score >= 40) {
        health_status = "Fair";
        health_color = "\\033[35m"; // Magenta
    } else {
        health_status = "Poor";
        health_color = "\\033[31m"; // Red
    }
    
    printf("🎯 Overall Health: %s%s\\033[0m (%d/100)\\n\\n", health_color, health_status, health_score);
    
    // Key metrics
    printf("📈 Key Metrics:\\n");
    printf("================\\n");
    printf("📁 Total files: %d\\n", health->total_files);
    printf("📝 Lines of code: %d\\n", health->total_lines);
    printf("⚙️  Functions: %d\\n", health->total_functions);
    printf("🧪 Test functions: %d\\n", health->total_tests);
    printf("📊 Test coverage: %.1f%%\\n", health->test_coverage);
    printf("🔨 Build status: %s\\n", health->build_status ? "\\033[32m✅ Success\\033[0m" : "\\033[31m❌ Failed\\033[0m");
    printf("⚠️  Warnings: %d\\n", health->warnings);
    printf("❌ Errors: %d\\n", health->errors);
    printf("🌿 Git branch: %s\\n", health->git_branch);
    
    if (health->last_commit > 0) {
        time_t now = time(NULL);
        double hours_since_commit = difftime(now, health->last_commit) / 3600.0;
        printf("⏰ Last commit: %.1f hours ago\\n", hours_since_commit);
    }
    
    printf("\\n");
    
    // Visual health indicators
    printf("🎛️  Health Indicators:\\n");
    printf("===================\\n");
    
    // Test coverage bar
    printf("🧪 Test Coverage [");
    int coverage_bars = (int)(health->test_coverage / 10);
    for (int i = 0; i < 10; i++) {
        if (i < coverage_bars) {
            printf("\\033[32m█\\033[0m");
        } else {
            printf("░");
        }
    }
    printf("] %.1f%%\\n", health->test_coverage);
    
    // Build health bar
    printf("🔨 Build Health  [");
    for (int i = 0; i < 10; i++) {
        if (health->build_status && health->errors == 0) {
            printf("\\033[32m█\\033[0m");
        } else if (health->warnings < 5) {
            printf(i < 5 ? "\\033[33m█\\033[0m" : "░");
        } else {
            printf(i < 3 ? "\\033[31m█\\033[0m" : "░");
        }
    }
    printf("] %s\\n", health->build_status ? "Healthy" : "Issues");
    
    // Code quality bar
    int quality_score = (health->total_tests > 0 ? 3 : 0) + 
                       (health->test_coverage > 50 ? 3 : 0) + 
                       (health->errors == 0 ? 2 : 0) + 
                       (health->warnings < 5 ? 2 : 0);
    
    printf("🎯 Code Quality  [");
    for (int i = 0; i < 10; i++) {
        if (i < quality_score) {
            printf("\\033[34m█\\033[0m");
        } else {
            printf("░");
        }
    }
    printf("] %d/10\\n", quality_score);
}

void print_recommendations(const ProjectHealth* health) {
    printf("\\n💡 Recommendations:\\n");
    printf("===================\\n");
    
    if (!health->build_status) {
        printf("🔨 Fix build issues - project doesn't compile\\n");
    }
    
    if (health->test_coverage < 50) {
        printf("🧪 Increase test coverage (currently %.1f%%, target: 70%%)\\n", health->test_coverage);
    }
    
    if (health->total_tests == 0) {
        printf("📝 Add unit tests to improve code reliability\\n");
    }
    
    if (health->errors > 0) {
        printf("❌ Fix %d compilation errors\\n", health->errors);
    }
    
    if (health->warnings > 10) {
        printf("⚠️  Reduce warnings count (currently %d)\\n", health->warnings);
    }
    
    if (health->total_functions > 0 && health->total_tests == 0) {
        printf("🎯 Add at least one test per function\\n");
    }
    
    if (health->last_commit > 0) {
        time_t now = time(NULL);
        double days_since_commit = difftime(now, health->last_commit) / 86400.0;
        if (days_since_commit > 7) {
            printf("📅 Consider more frequent commits (%.0f days since last commit)\\n", days_since_commit);
        }
    }
    
    printf("\\n🚀 Next Actions:\\n");
    printf("  1. Run 'make test' to execute test suite\\n");
    printf("  2. Run 'make coverage' for detailed coverage report\\n");
    printf("  3. Use 'goo vet' to check for code issues\\n");
    printf("  4. Review and fix any compiler warnings\\n");
}

void generate_html_dashboard(const ProjectHealth* health, const char* filename) {
    FILE* file = fopen(filename, "w");
    if (!file) {
        printf("⚠️  Could not generate HTML dashboard: %s\\n", filename);
        return;
    }
    
    // Calculate health score
    int health_score = 0;
    if (health->build_status) health_score += 30;
    if (health->test_coverage > 70) health_score += 25;
    if (health->errors == 0) health_score += 20;
    if (health->warnings < 5) health_score += 15;
    if (health->total_tests > 0) health_score += 10;
    
    fprintf(file, "<!DOCTYPE html>\\n<html>\\n<head>\\n");
    fprintf(file, "<title>Project Health Dashboard</title>\\n");
    fprintf(file, "<style>\\n");
    fprintf(file, "body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; margin: 40px; background: #f7fafc; }\\n");
    fprintf(file, ".dashboard { background: white; padding: 30px; border-radius: 12px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }\\n");
    fprintf(file, ".metric { padding: 20px; margin: 15px 0; border-radius: 8px; border-left: 4px solid #3182ce; background: #f7fafc; }\\n");
    fprintf(file, ".health-score { font-size: 48px; font-weight: bold; text-align: center; margin: 20px 0; }\\n");
    fprintf(file, ".excellent { color: #38a169; }\\n");
    fprintf(file, ".good { color: #d69e2e; }\\n");
    fprintf(file, ".fair { color: #805ad5; }\\n");
    fprintf(file, ".poor { color: #e53e3e; }\\n");
    fprintf(file, ".progress-bar { width: 100%%; height: 24px; background: #e2e8f0; border-radius: 12px; overflow: hidden; margin: 10px 0; }\\n");
    fprintf(file, ".progress-fill { height: 100%%; background: linear-gradient(90deg, #4299e1, #3182ce); transition: width 0.3s ease; }\\n");
    fprintf(file, "</style>\\n");
    fprintf(file, "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>\\n");
    fprintf(file, "</head>\\n<body>\\n");
    
    fprintf(file, "<div class='dashboard'>\\n");
    fprintf(file, "<h1>📊 Project Health Dashboard</h1>\\n");
    
    // Health score
    const char* score_class = health_score >= 80 ? "excellent" : 
                             health_score >= 60 ? "good" : 
                             health_score >= 40 ? "fair" : "poor";
    
    fprintf(file, "<div class='health-score %s'>%d/100</div>\\n", score_class, health_score);
    
    // Metrics
    fprintf(file, "<div class='metric'>\\n");
    fprintf(file, "<h3>📁 Project Size</h3>\\n");
    fprintf(file, "<p>Files: %d | Lines: %d | Functions: %d</p>\\n", 
            health->total_files, health->total_lines, health->total_functions);
    fprintf(file, "</div>\\n");
    
    fprintf(file, "<div class='metric'>\\n");
    fprintf(file, "<h3>🧪 Testing</h3>\\n");
    fprintf(file, "<p>Test Functions: %d | Coverage: %.1f%%</p>\\n", 
            health->total_tests, health->test_coverage);
    fprintf(file, "<div class='progress-bar'>\\n");
    fprintf(file, "<div class='progress-fill' style='width: %.1f%%;'></div>\\n", health->test_coverage);
    fprintf(file, "</div>\\n");
    fprintf(file, "</div>\\n");
    
    fprintf(file, "<div class='metric'>\\n");
    fprintf(file, "<h3>🔨 Build Quality</h3>\\n");
    fprintf(file, "<p>Status: %s | Errors: %d | Warnings: %d</p>\\n", 
            health->build_status ? "✅ Success" : "❌ Failed", health->errors, health->warnings);
    fprintf(file, "</div>\\n");
    
    fprintf(file, "<div class='metric'>\\n");
    fprintf(file, "<h3>📊 Repository Info</h3>\\n");
    fprintf(file, "<p>Branch: %s</p>\\n", health->git_branch);
    if (health->last_commit > 0) {
        time_t now = time(NULL);
        double hours_ago = difftime(now, health->last_commit) / 3600.0;
        fprintf(file, "<p>Last Commit: %.1f hours ago</p>\\n", hours_ago);
    }
    fprintf(file, "</div>\\n");
    
    // Chart
    fprintf(file, "<canvas id='healthChart' width='400' height='200'></canvas>\\n");
    
    fprintf(file, "</div>\\n");
    
    // JavaScript for chart
    fprintf(file, "<script>\\n");
    fprintf(file, "const ctx = document.getElementById('healthChart').getContext('2d');\\n");
    fprintf(file, "new Chart(ctx, {\\n");
    fprintf(file, "  type: 'doughnut',\\n");
    fprintf(file, "  data: {\\n");
    fprintf(file, "    labels: ['Passed', 'Remaining'],\\n");
    fprintf(file, "    datasets: [{\\n");
    fprintf(file, "      data: [%d, %d],\\n", health_score, 100 - health_score);
    fprintf(file, "      backgroundColor: ['#38a169', '#e2e8f0']\\n");
    fprintf(file, "    }]\\n");
    fprintf(file, "  },\\n");
    fprintf(file, "  options: { responsive: true, plugins: { legend: { display: false } } }\\n");
    fprintf(file, "});\\n");
    fprintf(file, "</script>\\n");
    
    fprintf(file, "</body>\\n</html>\\n");
    fclose(file);
    
    printf("📄 HTML dashboard generated: %s\\n", filename);
}

int main(int argc, char* argv[]) {
    printf("📊 Goo Project Health Dashboard\\n");
    printf("===============================\\n\\n");
    
    const char* html_output = "project_health.html";
    int continuous_mode = 0;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--continuous") == 0) {
            continuous_mode = 1;
        } else if (strncmp(argv[i], "--output=", 9) == 0) {
            html_output = argv[i] + 9;
        }
    }
    
    ProjectHealth health;
    
    do {
        init_project_health(&health);
        analyze_project_structure(&health);
        print_health_dashboard(&health);
        print_recommendations(&health);
        generate_html_dashboard(&health, html_output);
        
        if (continuous_mode) {
            printf("\\n🔄 Monitoring mode - press Ctrl+C to exit\\n");
            printf("📊 Dashboard will refresh every 30 seconds\\n\\n");
            sleep(30);
            printf("\\033[2J\\033[H"); // Clear screen
        }
    } while (continuous_mode);
    
    printf("\\n🎉 Health analysis complete!\\n");
    printf("🌐 View detailed dashboard: %s\\n", html_output);
    
    return 0;
}