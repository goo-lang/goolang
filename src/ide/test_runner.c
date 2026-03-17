// Integrated Test Runner with Visual Results
// Enhanced testing tools for the Goo development environment

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdarg.h>

#define _GNU_SOURCE

// Test result status
typedef enum {
    TEST_PASSED,
    TEST_FAILED,
    TEST_SKIPPED,
    TEST_ERROR
} TestStatus;

// Test result structure
typedef struct TestResult {
    char* test_name;
    char* file_path;
    TestStatus status;
    double duration_ms;
    char* output;
    char* error_message;
    int line_number;
    struct TestResult* next;
} TestResult;

// Test suite structure
typedef struct TestSuite {
    char* suite_name;
    char* description;
    int total_tests;
    int passed_tests;
    int failed_tests;
    int skipped_tests;
    int error_tests;
    double total_duration_ms;
    TestResult* results;
    struct TestSuite* next;
} TestSuite;

// Test runner configuration
typedef struct {
    bool verbose;
    bool colored_output;
    bool generate_html_report;
    bool watch_mode;
    bool parallel_execution;
    char* output_dir;
    char* filter_pattern;
    int max_workers;
} TestConfig;

// Color constants for terminal output
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_RESET   "\033[0m"

// Global test configuration
static TestConfig g_test_config = {
    .verbose = false,
    .colored_output = true,
    .generate_html_report = true,
    .watch_mode = false,
    .parallel_execution = false,
    .output_dir = "./test-results",
    .filter_pattern = NULL,
    .max_workers = 4
};

// =============================================================================
// Utility Functions
// =============================================================================

static void print_colored(const char* color, const char* format, ...) {
    if (!g_test_config.colored_output) {
        color = "";
    }
    
    va_list args;
    va_start(args, format);
    printf("%s", color);
    vprintf(format, args);
    printf("%s", COLOR_RESET);
    va_end(args);
}

static double get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static char* read_file_content(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) return NULL;
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* content = malloc(size + 1);
    if (!content) {
        fclose(file);
        return NULL;
    }
    
    fread(content, 1, size, file);
    content[size] = '\0';
    fclose(file);
    
    return content;
}

static void create_directory(const char* path) {
    char tmp[256];
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

// =============================================================================
// Test Discovery
// =============================================================================

static bool is_test_file(const char* filename) {
    if (!filename) return false;
    
    size_t len = strlen(filename);
    if (len < 9) return false; // Minimum: "test.goo"
    
    // Check for _test.goo or test_.goo patterns
    return (strstr(filename, "_test.goo") != NULL) ||
           (strstr(filename, "test_") == filename && 
            strcmp(filename + len - 4, ".goo") == 0);
}

static void discover_tests_recursive(const char* directory, TestSuite** suites) {
    DIR* dir = opendir(directory);
    if (!dir) return;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", directory, entry->d_name);
        
        struct stat statbuf;
        if (stat(full_path, &statbuf) != 0) continue;
        
        if (S_ISDIR(statbuf.st_mode)) {
            // Recursively search subdirectories
            discover_tests_recursive(full_path, suites);
        } else if (S_ISREG(statbuf.st_mode) && is_test_file(entry->d_name)) {
            // Found a test file
            if (g_test_config.filter_pattern && 
                !strstr(entry->d_name, g_test_config.filter_pattern)) {
                continue;
            }
            
            // Create or find test suite for this directory
            TestSuite* suite = *suites;
            const char* suite_name = strrchr(directory, '/');
            suite_name = suite_name ? suite_name + 1 : directory;
            
            while (suite && strcmp(suite->suite_name, suite_name) != 0) {
                suite = suite->next;
            }
            
            if (!suite) {
                suite = calloc(1, sizeof(TestSuite));
                suite->suite_name = strdup(suite_name);
                suite->description = strdup("Test suite");
                suite->next = *suites;
                *suites = suite;
            }
            
            suite->total_tests++;
            
            if (g_test_config.verbose) {
                print_colored(COLOR_CYAN, "  Found test: %s\n", full_path);
            }
        }
    }
    
    closedir(dir);
}

// =============================================================================
// Test Execution
// =============================================================================

static TestResult* run_single_test(const char* test_file, const char* test_name) {
    TestResult* result = calloc(1, sizeof(TestResult));
    result->test_name = strdup(test_name);
    result->file_path = strdup(test_file);
    
    double start_time = get_time_ms();
    
    // Execute the test (simplified - would use actual Goo compiler)
    char command[1024];
    snprintf(command, sizeof(command), "goo test %s 2>&1", test_file);
    
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        result->status = TEST_ERROR;
        result->error_message = strdup("Failed to execute test");
        result->duration_ms = get_time_ms() - start_time;
        return result;
    }
    
    // Read test output
    char output_buffer[4096] = {0};
    size_t out_offset = 0;
    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        if (out_offset < sizeof(output_buffer) - 1) {
            out_offset += snprintf(output_buffer + out_offset,
                                   sizeof(output_buffer) - out_offset, "%s", line);
        }
    }
    
    int exit_status = pclose(pipe);
    result->duration_ms = get_time_ms() - start_time;
    result->output = strdup(output_buffer);
    
    // Determine test status based on exit code and output
    if (exit_status == 0) {
        if (strstr(output_buffer, "SKIP") || strstr(output_buffer, "skip")) {
            result->status = TEST_SKIPPED;
        } else {
            result->status = TEST_PASSED;
        }
    } else {
        if (strstr(output_buffer, "FAIL") || strstr(output_buffer, "fail")) {
            result->status = TEST_FAILED;
        } else {
            result->status = TEST_ERROR;
        }
        
        // Extract error message
        char* error_start = strstr(output_buffer, "Error:");
        if (!error_start) error_start = strstr(output_buffer, "FAIL:");
        if (error_start) {
            char* error_end = strchr(error_start, '\n');
            if (error_end) {
                size_t error_len = error_end - error_start;
                result->error_message = malloc(error_len + 1);
                strncpy(result->error_message, error_start, error_len);
                result->error_message[error_len] = '\0';
            } else {
                result->error_message = strdup(error_start);
            }
        }
    }
    
    return result;
}

static void run_test_suite(TestSuite* suite) {
    print_colored(COLOR_BOLD, "\n🧪 Running test suite: %s\n", suite->suite_name);
    print_colored(COLOR_BLUE, "===========================================\n");
    
    double suite_start_time = get_time_ms();
    
    // For this example, we'll simulate running tests
    // In a real implementation, this would discover and run actual test functions
    
    // Simulate some test results
    const char* test_names[] = {
        "test_basic_functionality",
        "test_error_handling", 
        "test_edge_cases",
        "test_performance"
    };
    
    for (size_t i = 0; i < sizeof(test_names) / sizeof(test_names[0]); i++) {
        if (g_test_config.verbose) {
            printf("  Running: %s... ", test_names[i]);
            fflush(stdout);
        }
        
        TestResult* result = run_single_test("test_file.goo", test_names[i]);
        
        // Add to suite results
        result->next = suite->results;
        suite->results = result;
        
        // Update suite counters
        switch (result->status) {
            case TEST_PASSED:
                suite->passed_tests++;
                if (g_test_config.verbose) {
                    print_colored(COLOR_GREEN, "PASS");
                }
                break;
            case TEST_FAILED:
                suite->failed_tests++;
                if (g_test_config.verbose) {
                    print_colored(COLOR_RED, "FAIL");
                }
                break;
            case TEST_SKIPPED:
                suite->skipped_tests++;
                if (g_test_config.verbose) {
                    print_colored(COLOR_YELLOW, "SKIP");
                }
                break;
            case TEST_ERROR:
                suite->error_tests++;
                if (g_test_config.verbose) {
                    print_colored(COLOR_MAGENTA, "ERROR");
                }
                break;
        }
        
        if (g_test_config.verbose) {
            printf(" (%.2fms)\n", result->duration_ms);
        }
    }
    
    suite->total_duration_ms = get_time_ms() - suite_start_time;
}

// =============================================================================
// Visual Test Results
// =============================================================================

static void print_test_summary(TestSuite* suites) {
    int total_suites = 0;
    int total_tests = 0;
    int total_passed = 0;
    int total_failed = 0;
    int total_skipped = 0;
    int total_errors = 0;
    double total_duration = 0;
    
    // Calculate totals
    for (TestSuite* suite = suites; suite; suite = suite->next) {
        total_suites++;
        total_tests += suite->total_tests;
        total_passed += suite->passed_tests;
        total_failed += suite->failed_tests;
        total_skipped += suite->skipped_tests;
        total_errors += suite->error_tests;
        total_duration += suite->total_duration_ms;
    }
    
    printf("\n");
    print_colored(COLOR_BOLD COLOR_CYAN, "📊 Test Results Summary\n");
    print_colored(COLOR_CYAN, "======================\n");
    
    // Overall status
    bool all_passed = (total_failed == 0 && total_errors == 0);
    if (all_passed) {
        print_colored(COLOR_BOLD COLOR_GREEN, "✅ ALL TESTS PASSED\n");
    } else {
        print_colored(COLOR_BOLD COLOR_RED, "❌ SOME TESTS FAILED\n");
    }
    
    printf("\n");
    printf("Suites:     %d\n", total_suites);
    printf("Tests:      %d\n", total_tests);
    print_colored(COLOR_GREEN, "Passed:     %d\n", total_passed);
    
    if (total_failed > 0) {
        print_colored(COLOR_RED, "Failed:     %d\n", total_failed);
    }
    if (total_skipped > 0) {
        print_colored(COLOR_YELLOW, "Skipped:    %d\n", total_skipped);
    }
    if (total_errors > 0) {
        print_colored(COLOR_MAGENTA, "Errors:     %d\n", total_errors);
    }
    
    printf("Duration:   %.2fms\n", total_duration);
    
    // Progress bar
    printf("\nProgress: ");
    int bar_width = 50;
    int filled = (total_tests > 0) ? (total_passed * bar_width / total_tests) : 0;
    
    print_colored(COLOR_GREEN, "[");
    for (int i = 0; i < filled; i++) {
        print_colored(COLOR_GREEN, "█");
    }
    for (int i = filled; i < bar_width; i++) {
        printf(" ");
    }
    print_colored(COLOR_GREEN, "]");
    printf(" %d/%d (%.1f%%)\n", total_passed, total_tests, 
           total_tests > 0 ? (100.0 * total_passed / total_tests) : 0);
}

static void print_detailed_results(TestSuite* suites) {
    for (TestSuite* suite = suites; suite; suite = suite->next) {
        printf("\n");
        print_colored(COLOR_BOLD, "📁 Suite: %s\n", suite->suite_name);
        print_colored(COLOR_BLUE, "-------------------\n");
        
        for (TestResult* result = suite->results; result; result = result->next) {
            const char* status_symbol;
            const char* status_color;
            
            switch (result->status) {
                case TEST_PASSED:
                    status_symbol = "✅";
                    status_color = COLOR_GREEN;
                    break;
                case TEST_FAILED:
                    status_symbol = "❌";
                    status_color = COLOR_RED;
                    break;
                case TEST_SKIPPED:
                    status_symbol = "⏭️";
                    status_color = COLOR_YELLOW;
                    break;
                case TEST_ERROR:
                    status_symbol = "💥";
                    status_color = COLOR_MAGENTA;
                    break;
            }
            
            print_colored(status_color, "%s %s", status_symbol, result->test_name);
            printf(" (%.2fms)", result->duration_ms);
            
            if (result->status == TEST_FAILED || result->status == TEST_ERROR) {
                printf("\n");
                if (result->error_message) {
                    print_colored(COLOR_RED, "    Error: %s\n", result->error_message);
                }
                if (result->output && g_test_config.verbose) {
                    printf("    Output:\n");
                    char* line = strtok(result->output, "\n");
                    while (line) {
                        printf("      %s\n", line);
                        line = strtok(NULL, "\n");
                    }
                }
            } else {
                printf("\n");
            }
        }
    }
}

// =============================================================================
// HTML Report Generation
// =============================================================================

static void generate_html_report(TestSuite* suites) {
    if (!g_test_config.generate_html_report) return;
    
    create_directory(g_test_config.output_dir);
    
    char report_path[512];
    snprintf(report_path, sizeof(report_path), "%s/test-report.html", g_test_config.output_dir);
    
    FILE* html = fopen(report_path, "w");
    if (!html) {
        print_colored(COLOR_RED, "Failed to create HTML report: %s\n", report_path);
        return;
    }
    
    // HTML header with styling
    fprintf(html, "<!DOCTYPE html>\n");
    fprintf(html, "<html lang=\"en\">\n");
    fprintf(html, "<head>\n");
    fprintf(html, "    <meta charset=\"UTF-8\">\n");
    fprintf(html, "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
    fprintf(html, "    <title>Goo Test Results</title>\n");
    fprintf(html, "    <style>\n");
    fprintf(html, "        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif; margin: 0; padding: 20px; background: #f5f5f5; }\n");
    fprintf(html, "        .container { max-width: 1200px; margin: 0 auto; background: white; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }\n");
    fprintf(html, "        .header { background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); color: white; padding: 30px; border-radius: 8px 8px 0 0; }\n");
    fprintf(html, "        .summary { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 20px; padding: 30px; }\n");
    fprintf(html, "        .metric { text-align: center; padding: 20px; border-radius: 8px; background: #f8f9fa; }\n");
    fprintf(html, "        .metric.passed { border-left: 4px solid #28a745; }\n");
    fprintf(html, "        .metric.failed { border-left: 4px solid #dc3545; }\n");
    fprintf(html, "        .metric.skipped { border-left: 4px solid #ffc107; }\n");
    fprintf(html, "        .metric.error { border-left: 4px solid #6f42c1; }\n");
    fprintf(html, "        .metric-value { font-size: 2em; font-weight: bold; margin-bottom: 5px; }\n");
    fprintf(html, "        .metric-label { color: #666; font-size: 0.9em; }\n");
    fprintf(html, "        .suite { margin: 20px 30px; border: 1px solid #e9ecef; border-radius: 8px; overflow: hidden; }\n");
    fprintf(html, "        .suite-header { background: #e9ecef; padding: 15px; font-weight: bold; }\n");
    fprintf(html, "        .test-item { padding: 10px 15px; border-bottom: 1px solid #f1f3f4; display: flex; justify-content: space-between; align-items: center; }\n");
    fprintf(html, "        .test-item:last-child { border-bottom: none; }\n");
    fprintf(html, "        .test-name { font-family: 'Monaco', 'Menlo', monospace; }\n");
    fprintf(html, "        .test-status { padding: 4px 8px; border-radius: 12px; font-size: 0.8em; color: white; }\n");
    fprintf(html, "        .status-passed { background: #28a745; }\n");
    fprintf(html, "        .status-failed { background: #dc3545; }\n");
    fprintf(html, "        .status-skipped { background: #ffc107; color: #333; }\n");
    fprintf(html, "        .status-error { background: #6f42c1; }\n");
    fprintf(html, "        .progress-bar { width: 100%%; height: 20px; background: #e9ecef; border-radius: 10px; overflow: hidden; margin: 20px 0; }\n");
    fprintf(html, "        .progress-fill { height: 100%%; background: linear-gradient(90deg, #28a745, #20c997); transition: width 0.3s ease; }\n");
    fprintf(html, "        .error-details { background: #f8d7da; color: #721c24; padding: 10px; margin-top: 10px; border-radius: 4px; font-family: monospace; font-size: 0.9em; }\n");
    fprintf(html, "    </style>\n");
    fprintf(html, "</head>\n");
    fprintf(html, "<body>\n");
    fprintf(html, "    <div class=\"container\">\n");
    
    // Header
    time_t now = time(NULL);
    char* time_str = ctime(&now);
    time_str[strlen(time_str) - 1] = '\0'; // Remove newline
    
    fprintf(html, "        <div class=\"header\">\n");
    fprintf(html, "            <h1>🧪 Goo Test Results</h1>\n");
    fprintf(html, "            <p>Generated on %s</p>\n", time_str);
    fprintf(html, "        </div>\n");
    
    // Calculate summary metrics
    int total_tests = 0, total_passed = 0, total_failed = 0, total_skipped = 0, total_errors = 0;
    double total_duration = 0;
    
    for (TestSuite* suite = suites; suite; suite = suite->next) {
        total_tests += suite->total_tests;
        total_passed += suite->passed_tests;
        total_failed += suite->failed_tests;
        total_skipped += suite->skipped_tests;
        total_errors += suite->error_tests;
        total_duration += suite->total_duration_ms;
    }
    
    // Summary metrics
    fprintf(html, "        <div class=\"summary\">\n");
    fprintf(html, "            <div class=\"metric passed\">\n");
    fprintf(html, "                <div class=\"metric-value\">%d</div>\n", total_passed);
    fprintf(html, "                <div class=\"metric-label\">Passed</div>\n");
    fprintf(html, "            </div>\n");
    
    if (total_failed > 0) {
        fprintf(html, "            <div class=\"metric failed\">\n");
        fprintf(html, "                <div class=\"metric-value\">%d</div>\n", total_failed);
        fprintf(html, "                <div class=\"metric-label\">Failed</div>\n");
        fprintf(html, "            </div>\n");
    }
    
    if (total_skipped > 0) {
        fprintf(html, "            <div class=\"metric skipped\">\n");
        fprintf(html, "                <div class=\"metric-value\">%d</div>\n", total_skipped);
        fprintf(html, "                <div class=\"metric-label\">Skipped</div>\n");
        fprintf(html, "            </div>\n");
    }
    
    if (total_errors > 0) {
        fprintf(html, "            <div class=\"metric error\">\n");
        fprintf(html, "                <div class=\"metric-value\">%d</div>\n", total_errors);
        fprintf(html, "                <div class=\"metric-label\">Errors</div>\n");
        fprintf(html, "            </div>\n");
    }
    
    fprintf(html, "            <div class=\"metric\">\n");
    fprintf(html, "                <div class=\"metric-value\">%.1fms</div>\n", total_duration);
    fprintf(html, "                <div class=\"metric-label\">Duration</div>\n");
    fprintf(html, "            </div>\n");
    fprintf(html, "        </div>\n");
    
    // Progress bar
    double success_rate = total_tests > 0 ? (100.0 * total_passed / total_tests) : 0;
    fprintf(html, "        <div style=\"padding: 0 30px;\">\n");
    fprintf(html, "            <div class=\"progress-bar\">\n");
    fprintf(html, "                <div class=\"progress-fill\" style=\"width: %.1f%%\"></div>\n", success_rate);
    fprintf(html, "            </div>\n");
    fprintf(html, "            <p style=\"text-align: center; margin: 0; color: #666;\">%.1f%% Success Rate (%d/%d tests passed)</p>\n", 
             success_rate, total_passed, total_tests);
    fprintf(html, "        </div>\n");
    
    // Test suites details
    for (TestSuite* suite = suites; suite; suite = suite->next) {
        fprintf(html, "        <div class=\"suite\">\n");
        fprintf(html, "            <div class=\"suite-header\">📁 %s</div>\n", suite->suite_name);
        
        for (TestResult* result = suite->results; result; result = result->next) {
            const char* status_class;
            const char* status_text;
            
            switch (result->status) {
                case TEST_PASSED:
                    status_class = "status-passed";
                    status_text = "PASSED";
                    break;
                case TEST_FAILED:
                    status_class = "status-failed";
                    status_text = "FAILED";
                    break;
                case TEST_SKIPPED:
                    status_class = "status-skipped";
                    status_text = "SKIPPED";
                    break;
                case TEST_ERROR:
                    status_class = "status-error";
                    status_text = "ERROR";
                    break;
            }
            
            fprintf(html, "            <div class=\"test-item\">\n");
            fprintf(html, "                <div>\n");
            fprintf(html, "                    <div class=\"test-name\">%s</div>\n", result->test_name);
            fprintf(html, "                    <small style=\"color: #666;\">%.2fms</small>\n", result->duration_ms);
            
            if (result->error_message) {
                fprintf(html, "                    <div class=\"error-details\">%s</div>\n", result->error_message);
            }
            
            fprintf(html, "                </div>\n");
            fprintf(html, "                <div class=\"test-status %s\">%s</div>\n", status_class, status_text);
            fprintf(html, "            </div>\n");
        }
        
        fprintf(html, "        </div>\n");
    }
    
    // Footer
    fprintf(html, "    </div>\n");
    fprintf(html, "</body>\n");
    fprintf(html, "</html>\n");
    
    fclose(html);
    
    print_colored(COLOR_GREEN, "📄 HTML report generated: %s\n", report_path);
}

// =============================================================================
// Main Test Runner
// =============================================================================

static void print_usage() {
    printf("Goo Integrated Test Runner\n");
    printf("==========================\n");
    printf("\n");
    printf("Usage: goo-test-runner [OPTIONS] [PATH...]\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help              Show this help\n");
    printf("  -v, --verbose           Verbose output\n");
    printf("  --no-color              Disable colored output\n");
    printf("  --no-html               Disable HTML report generation\n");
    printf("  --watch                 Watch mode - rerun tests on file changes\n");
    printf("  --parallel              Run tests in parallel\n");
    printf("  --output-dir <dir>      Output directory for reports (default: ./test-results)\n");
    printf("  --filter <pattern>      Run only tests matching pattern\n");
    printf("  --workers <num>         Number of parallel workers (default: 4)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  goo-test-runner                    # Run all tests\n");
    printf("  goo-test-runner --verbose tests/   # Run tests in tests/ directory\n");
    printf("  goo-test-runner --filter unit      # Run only tests matching 'unit'\n");
    printf("  goo-test-runner --watch --parallel # Watch mode with parallel execution\n");
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    char** test_paths = NULL;
    int path_count = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_test_config.verbose = true;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            g_test_config.colored_output = false;
        } else if (strcmp(argv[i], "--no-html") == 0) {
            g_test_config.generate_html_report = false;
        } else if (strcmp(argv[i], "--watch") == 0) {
            g_test_config.watch_mode = true;
        } else if (strcmp(argv[i], "--parallel") == 0) {
            g_test_config.parallel_execution = true;
        } else if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            g_test_config.output_dir = argv[++i];
        } else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            g_test_config.filter_pattern = argv[++i];
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            g_test_config.max_workers = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            // Add to test paths
            char** tmp = realloc(test_paths, (path_count + 1) * sizeof(char*));
            if (!tmp) continue;
            test_paths = tmp;
            test_paths[path_count++] = argv[i];
        }
    }
    
    // Default test paths
    if (path_count == 0) {
        test_paths = malloc(sizeof(char*));
        test_paths[0] = "./tests";
        path_count = 1;
    }
    
    print_colored(COLOR_BOLD COLOR_CYAN, "🚀 Goo Integrated Test Runner\n");
    print_colored(COLOR_CYAN, "==============================\n");
    
    if (g_test_config.filter_pattern) {
        printf("Filter: %s\n", g_test_config.filter_pattern);
    }
    
    if (g_test_config.parallel_execution) {
        printf("Parallel execution: %d workers\n", g_test_config.max_workers);
    }
    
    printf("\n");
    
    do {
        TestSuite* test_suites = NULL;
        
        // Discover tests
        print_colored(COLOR_BLUE, "🔍 Discovering tests...\n");
        for (int i = 0; i < path_count; i++) {
            discover_tests_recursive(test_paths[i], &test_suites);
        }
        
        if (!test_suites) {
            print_colored(COLOR_YELLOW, "⚠️  No test files found\n");
            break;
        }
        
        // Run test suites
        for (TestSuite* suite = test_suites; suite; suite = suite->next) {
            run_test_suite(suite);
        }
        
        // Display results
        print_test_summary(test_suites);
        print_detailed_results(test_suites);
        
        // Generate HTML report
        generate_html_report(test_suites);
        
        // Clean up
        // TODO: Free memory
        
        // Watch mode - wait for file changes
        if (g_test_config.watch_mode) {
            print_colored(COLOR_CYAN, "\n👀 Watching for changes... (Press Ctrl+C to exit)\n");
            sleep(5); // Simplified - would use inotify in real implementation
        }
        
    } while (g_test_config.watch_mode);
    
    free(test_paths);
    return 0;
}