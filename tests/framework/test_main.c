#include "test/test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

// External test declarations
extern void register_error_tests(void);
extern void register_lexer_tests(void);
extern void register_parser_tests(void);
extern void register_type_tests(void);
extern void register_codegen_tests(void);

// Print usage information
static void print_usage(const char* program) {
    printf("Usage: %s [options]\n", program);
    printf("\nOptions:\n");
    printf("  -f, --filter <pattern>      Run only tests matching pattern\n");
    printf("  -s, --suite <pattern>       Run only suites matching pattern\n");
    printf("  -l, --list                  List all tests without running\n");
    printf("  -v, --verbose               Enable verbose output\n");
    printf("  -x, --stop-on-failure       Stop on first test failure\n");
    printf("  -r, --repeat <count>        Repeat tests N times\n");
    printf("  -o, --output <format>       Output format (text, tap, junit)\n");
    printf("  -O, --output-file <file>    Write output to file\n");
    printf("  -h, --help                  Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s                          Run all tests\n", program);
    printf("  %s -f parser                Run tests with 'parser' in name\n", program);
    printf("  %s -s lexer -v              Run all lexer suite tests verbosely\n", program);
    printf("  %s -o tap > results.tap     Output TAP format\n", program);
}

// Parse command line options
static TestOptions parse_options(int argc, char** argv) {
    TestOptions options = {
        .filter = NULL,
        .suite_filter = NULL,
        .verbose = false,
        .stop_on_failure = false,
        .list_only = false,
        .shuffle = false,
        .repeat = 1,
        .parallel = 0,
        .output_format = "text",
        .output_file = NULL
    };
    
    static struct option long_options[] = {
        {"filter",          required_argument, 0, 'f'},
        {"suite",           required_argument, 0, 's'},
        {"list",            no_argument,       0, 'l'},
        {"verbose",         no_argument,       0, 'v'},
        {"stop-on-failure", no_argument,       0, 'x'},
        {"repeat",          required_argument, 0, 'r'},
        {"output",          required_argument, 0, 'o'},
        {"output-file",     required_argument, 0, 'O'},
        {"help",            no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "f:s:lvxr:o:O:h", 
                             long_options, &option_index)) != -1) {
        switch (opt) {
            case 'f':
                options.filter = optarg;
                break;
            case 's':
                options.suite_filter = optarg;
                break;
            case 'l':
                options.list_only = true;
                break;
            case 'v':
                options.verbose = true;
                break;
            case 'x':
                options.stop_on_failure = true;
                break;
            case 'r':
                options.repeat = atoi(optarg);
                if (options.repeat < 1) options.repeat = 1;
                break;
            case 'o':
                options.output_format = optarg;
                break;
            case 'O':
                options.output_file = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                print_usage(argv[0]);
                exit(1);
        }
    }
    
    return options;
}

// List all tests
static void list_tests(void) {
    printf("Available tests:\n");
    
    const char* current_suite = "";
    for (TestCase* test = g_test_registry; test; test = test->next) {
        if (strcmp(test->suite, current_suite) != 0) {
            current_suite = test->suite;
            printf("\n%s:\n", current_suite);
        }
        printf("  - %s\n", test->name);
    }
}

// Register all test suites
static void register_all_tests(void) {
    // Register test suites
    // Note: These functions would be implemented in respective test files
    // For now, we'll just have placeholders
    
    // TODO: Uncomment these as test files are created
    register_error_tests();
    // register_lexer_tests();
    // register_parser_tests();
    // register_type_tests();
    // register_codegen_tests();
}

// Main test runner
int main(int argc, char** argv) {
    // Parse command line options
    TestOptions options = parse_options(argc, argv);
    g_test_options = options;
    
    // Register all tests
    register_all_tests();
    
    // List tests if requested
    if (options.list_only) {
        list_tests();
        return 0;
    }
    
    // Discover tests based on filters
    TestSuite* suite = discover_tests(NULL);
    if (!suite) {
        fprintf(stderr, "Failed to create test suite\n");
        return 1;
    }
    
    // Check if any tests were found
    if (suite->total_tests == 0) {
        printf("No tests found matching filters\n");
        free_test_suite(suite);
        return 0;
    }
    
    // Open output file if specified
    FILE* output = stdout;
    if (options.output_file) {
        output = fopen(options.output_file, "w");
        if (!output) {
            fprintf(stderr, "Failed to open output file: %s\n", 
                    options.output_file);
            free_test_suite(suite);
            return 1;
        }
    }
    
    // Run tests multiple times if requested
    for (int i = 0; i < options.repeat; i++) {
        if (options.repeat > 1) {
            printf("\n=== Test run %d of %d ===\n", i + 1, options.repeat);
        }
        
        // Run the test suite
        run_test_suite(suite, &options);
    }
    
    // Output results in requested format
    if (strcmp(options.output_format, "tap") == 0) {
        print_tap_results(suite, output);
    } else if (strcmp(options.output_format, "junit") == 0) {
        print_junit_results(suite, output);
    } else {
        print_test_results(suite);
    }
    
    // Close output file if needed
    if (output != stdout) {
        fclose(output);
    }
    
    // Determine exit code
    int exit_code = (suite->failed > 0 || suite->errors > 0) ? 1 : 0;
    
    // Clean up
    free_test_suite(suite);
    
    return exit_code;
}

// TAP output format
void print_tap_results(const TestSuite* suite, FILE* out) {
    if (!suite || !out) return;
    
    fprintf(out, "TAP version 13\n");
    fprintf(out, "1..%zu\n", suite->total_tests);
    
    int test_number = 1;
    for (TestResult* result = suite->results; result; result = result->next) {
        const char* status = (result->status == TEST_PASS) ? "ok" : "not ok";
        fprintf(out, "%s %d - %s.%s\n", 
                status, test_number++,
                result->test->suite, result->test->name);
        
        if (result->status == TEST_SKIP && result->failure_message) {
            fprintf(out, "  # SKIP %s\n", result->failure_message);
        } else if (result->status == TEST_FAIL && result->failure_message) {
            fprintf(out, "  ---\n");
            fprintf(out, "  message: %s\n", result->failure_message);
            fprintf(out, "  severity: fail\n");
            fprintf(out, "  data:\n");
            fprintf(out, "    file: %s\n", result->failure_file);
            fprintf(out, "    line: %d\n", result->failure_line);
            fprintf(out, "  ...\n");
        }
    }
}

// JUnit XML output format
void print_junit_results(const TestSuite* suite, FILE* out) {
    if (!suite || !out) return;
    
    fprintf(out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(out, "<testsuites tests=\"%zu\" failures=\"%zu\" errors=\"%zu\" "
            "skipped=\"%zu\" time=\"%.3f\">\n",
            suite->total_tests, suite->failed, suite->errors,
            suite->skipped, suite->total_duration_ms / 1000.0);
    
    fprintf(out, "  <testsuite name=\"%s\" tests=\"%zu\" failures=\"%zu\" "
            "errors=\"%zu\" skipped=\"%zu\" time=\"%.3f\">\n",
            suite->name, suite->total_tests, suite->failed,
            suite->errors, suite->skipped, suite->total_duration_ms / 1000.0);
    
    for (TestResult* result = suite->results; result; result = result->next) {
        fprintf(out, "    <testcase name=\"%s\" classname=\"%s\" "
                "time=\"%.3f\">\n",
                result->test->name, result->test->suite,
                result->duration_ms / 1000.0);
        
        if (result->status == TEST_FAIL) {
            fprintf(out, "      <failure message=\"%s\" type=\"AssertionError\">\n",
                    result->failure_message ? result->failure_message : "Test failed");
            fprintf(out, "        %s:%d\n", result->failure_file, result->failure_line);
            fprintf(out, "      </failure>\n");
        } else if (result->status == TEST_ERROR) {
            fprintf(out, "      <error message=\"%s\" type=\"Error\">\n",
                    result->failure_message ? result->failure_message : "Test error");
            fprintf(out, "      </error>\n");
        } else if (result->status == TEST_SKIP) {
            fprintf(out, "      <skipped message=\"%s\"/>\n",
                    result->failure_message ? result->failure_message : "Test skipped");
        }
        
        fprintf(out, "    </testcase>\n");
    }
    
    fprintf(out, "  </testsuite>\n");
    fprintf(out, "</testsuites>\n");
}