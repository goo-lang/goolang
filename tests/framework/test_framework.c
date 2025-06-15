#include "test/test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>

// ANSI color codes
#define COLOR_GREEN  "\033[32m"
#define COLOR_RED    "\033[31m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_BLUE   "\033[34m"
#define COLOR_RESET  "\033[0m"

// Global test registry
TestCase* g_test_registry = NULL;
TestOptions g_test_options = {0};
TestFixture* g_current_fixture = NULL;

// Current test context
static TestResult* g_current_result = NULL;
static bool g_test_failed = false;

// Register a test
void register_test(const char* suite, const char* name, TestFunc func,
                  const char* file, int line) {
    register_test_with_fixture(suite, name, func, NULL, NULL, file, line);
}

// Register a test with fixture
void register_test_with_fixture(const char* suite, const char* name,
                               TestFunc func, SetupFunc setup,
                               TeardownFunc teardown,
                               const char* file, int line) {
    TestCase* test = (TestCase*)calloc(1, sizeof(TestCase));
    if (!test) return;
    
    test->name = name;
    test->suite = suite;
    test->test_func = func;
    test->setup = setup;
    test->teardown = teardown;
    test->file = file;
    test->line = line;
    
    // Add to registry
    test->next = g_test_registry;
    g_test_registry = test;
}

// Discover tests matching pattern
TestSuite* discover_tests(const char* pattern) {
    TestSuite* suite = (TestSuite*)calloc(1, sizeof(TestSuite));
    if (!suite) return NULL;
    
    suite->name = pattern ? pattern : "All Tests";
    
    // Count and collect matching tests
    for (TestCase* test = g_test_registry; test; test = test->next) {
        bool matches = true;
        
        // Apply filters
        if (g_test_options.filter && 
            !strstr(test->name, g_test_options.filter)) {
            matches = false;
        }
        if (g_test_options.suite_filter && 
            !strstr(test->suite, g_test_options.suite_filter)) {
            matches = false;
        }
        if (pattern && !strstr(test->name, pattern) && 
            !strstr(test->suite, pattern)) {
            matches = false;
        }
        
        if (matches) {
            suite->total_tests++;
            // Create a copy for the suite so we don't corrupt the global registry
            TestCase* test_copy = (TestCase*)calloc(1, sizeof(TestCase));
            if (test_copy) {
                *test_copy = *test; // Copy all fields
                test_copy->next = suite->tests; // Link to suite's test list
                suite->tests = test_copy;
            }
        }
    }
    
    return suite;
}

// Free test suite
void free_test_suite(TestSuite* suite) {
    if (!suite) return;
    
    // Free test copies
    TestCase* test = suite->tests;
    while (test) {
        TestCase* next = test->next;
        free(test);
        test = next;
    }
    
    // Free results
    TestResult* result = suite->results;
    while (result) {
        TestResult* next = result->next;
        free(result->failure_message);
        free(result);
        result = next;
    }
    
    free(suite);
}

// Get current time in milliseconds
double test_get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

// Run a single test
TestResult* run_test(TestCase* test) {
    TestResult* result = (TestResult*)calloc(1, sizeof(TestResult));
    if (!result) return NULL;
    
    result->test = test;
    g_current_result = result;
    g_test_failed = false;
    
    // Print test start
    if (g_test_options.verbose) {
        printf("[ RUN      ] %s.%s\n", test->suite, test->name);
    }
    
    double start_time = test_get_time_ms();
    
    // Setup fixture if needed
    if (test->setup) {
        // Create fixture for fixture-based tests
        g_current_fixture = (TestFixture*)calloc(1, sizeof(TestFixture));
        if (g_current_fixture) {
            g_current_fixture->name = test->suite;
            g_current_fixture->setup = test->setup;
            g_current_fixture->teardown = test->teardown;
            // Allocate fixture data - we need to infer the size
            // For now, allocate a reasonable amount for test fixtures
            g_current_fixture->data = calloc(1, 1024); // 1KB should be enough for most test fixtures
        }
        test->setup();
    }
    
    // Run the test
    TestStatus status = TEST_PASS;
    if (test->test_func) {
        status = test->test_func();
    }
    
    // Teardown fixture if needed
    if (test->teardown) {
        test->teardown();
    }
    
    // Clean up fixture
    if (g_current_fixture) {
        free(g_current_fixture->data);
        free(g_current_fixture);
        g_current_fixture = NULL;
    }
    
    double end_time = test_get_time_ms();
    result->duration_ms = end_time - start_time;
    
    // Set final status
    if (g_test_failed || status == TEST_FAIL) {
        result->status = TEST_FAIL;
    } else {
        result->status = status;
    }
    
    // Print test result
    if (g_test_options.verbose || result->status != TEST_PASS) {
        const char* status_str;
        const char* color;
        
        switch (result->status) {
            case TEST_PASS:
                status_str = "       OK";
                color = COLOR_GREEN;
                break;
            case TEST_FAIL:
                status_str = "  FAILED";
                color = COLOR_RED;
                break;
            case TEST_SKIP:
                status_str = " SKIPPED";
                color = COLOR_YELLOW;
                break;
            case TEST_ERROR:
                status_str = "   ERROR";
                color = COLOR_RED;
                break;
            default:
                status_str = " UNKNOWN";
                color = COLOR_RED;
        }
        
        printf("[%s%s%s ] %s.%s (%.2f ms)\n",
               color, status_str, COLOR_RESET,
               test->suite, test->name, result->duration_ms);
        
        // Print failure details
        if (result->status == TEST_FAIL && result->failure_message) {
            printf("%s:%d: %s\n", 
                   result->failure_file ? result->failure_file : test->file,
                   result->failure_line > 0 ? result->failure_line : test->line,
                   result->failure_message);
        }
    }
    
    g_current_result = NULL;
    return result;
}

// Run test suite
TestSuite* run_test_suite(TestSuite* suite, const TestOptions* options) {
    if (!suite) return NULL;
    
    printf("Running %zu test%s from %s\n",
           suite->total_tests,
           suite->total_tests == 1 ? "" : "s",
           suite->name);
    
    double start_time = test_get_time_ms();
    
    // Run each test
    for (TestCase* test = suite->tests; test; test = test->next) {
        TestResult* result = run_test(test);
        if (!result) continue;
        
        // Update counters
        switch (result->status) {
            case TEST_PASS:
                suite->passed++;
                break;
            case TEST_FAIL:
                suite->failed++;
                break;
            case TEST_SKIP:
                suite->skipped++;
                break;
            case TEST_ERROR:
                suite->errors++;
                break;
        }
        
        // Add to results list
        result->next = suite->results;
        suite->results = result;
        
        // Stop on failure if requested
        if (options->stop_on_failure && result->status == TEST_FAIL) {
            break;
        }
    }
    
    double end_time = test_get_time_ms();
    suite->total_duration_ms = end_time - start_time;
    
    return suite;
}

// Run all tests
void run_all_tests(const TestOptions* options) {
    if (options) {
        g_test_options = *options;
    }
    
    TestSuite* suite = discover_tests(NULL);
    if (!suite) return;
    
    run_test_suite(suite, &g_test_options);
    print_test_results(suite);
    
    // Exit with appropriate code
    int exit_code = (suite->failed > 0 || suite->errors > 0) ? 1 : 0;
    free_test_suite(suite);
    exit(exit_code);
}

// Print test results
void print_test_results(const TestSuite* suite) {
    if (!suite) return;
    
    printf("\n");
    printf("Test suite completed in %.2f ms\n", suite->total_duration_ms);
    printf("Tests run: %zu\n", suite->total_tests);
    
    if (suite->passed > 0) {
        printf(COLOR_GREEN "Passed: %zu" COLOR_RESET "\n", suite->passed);
    }
    if (suite->failed > 0) {
        printf(COLOR_RED "Failed: %zu" COLOR_RESET "\n", suite->failed);
    }
    if (suite->skipped > 0) {
        printf(COLOR_YELLOW "Skipped: %zu" COLOR_RESET "\n", suite->skipped);
    }
    if (suite->errors > 0) {
        printf(COLOR_RED "Errors: %zu" COLOR_RESET "\n", suite->errors);
    }
    
    if (suite->failed == 0 && suite->errors == 0) {
        printf(COLOR_GREEN "\nAll tests passed!" COLOR_RESET "\n");
    } else {
        printf(COLOR_RED "\nSome tests failed!" COLOR_RESET "\n");
    }
}

// Assertion implementations
bool assert_true_impl(bool condition, const char* expr,
                     const char* file, int line) {
    if (!condition) {
        if (g_current_result) {
            g_current_result->failure_file = file;
            g_current_result->failure_line = line;
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                    "Expected true: %s", expr);
            g_current_result->failure_message = strdup(buffer);
        }
        g_test_failed = true;
        return false;
    }
    return true;
}

bool assert_false_impl(bool condition, const char* expr,
                      const char* file, int line) {
    if (condition) {
        if (g_current_result) {
            g_current_result->failure_file = file;
            g_current_result->failure_line = line;
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                    "Expected false: %s", expr);
            g_current_result->failure_message = strdup(buffer);
        }
        g_test_failed = true;
        return false;
    }
    return true;
}

bool assert_eq_int_impl(int expected, int actual,
                       const char* file, int line) {
    if (expected != actual) {
        if (g_current_result) {
            g_current_result->failure_file = file;
            g_current_result->failure_line = line;
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                    "Expected: %d, Actual: %d", expected, actual);
            g_current_result->failure_message = strdup(buffer);
        }
        g_test_failed = true;
        return false;
    }
    return true;
}

bool assert_eq_uint_impl(unsigned int expected, unsigned int actual,
                        const char* file, int line) {
    if (expected != actual) {
        if (g_current_result) {
            g_current_result->failure_file = file;
            g_current_result->failure_line = line;
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                    "Expected: %u, Actual: %u", expected, actual);
            g_current_result->failure_message = strdup(buffer);
        }
        g_test_failed = true;
        return false;
    }
    return true;
}

bool assert_eq_ptr_impl(const void* expected, const void* actual,
                       const char* file, int line) {
    if (expected != actual) {
        if (g_current_result) {
            g_current_result->failure_file = file;
            g_current_result->failure_line = line;
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                    "Expected: %p, Actual: %p", expected, actual);
            g_current_result->failure_message = strdup(buffer);
        }
        g_test_failed = true;
        return false;
    }
    return true;
}

bool assert_eq_str_impl(const char* expected, const char* actual,
                       const char* file, int line) {
    if (expected == NULL && actual == NULL) {
        return true;
    }
    if (expected == NULL || actual == NULL || strcmp(expected, actual) != 0) {
        if (g_current_result) {
            g_current_result->failure_file = file;
            g_current_result->failure_line = line;
            char buffer[512];
            snprintf(buffer, sizeof(buffer), 
                    "Expected: \"%s\", Actual: \"%s\"", 
                    expected ? expected : "(null)",
                    actual ? actual : "(null)");
            g_current_result->failure_message = strdup(buffer);
        }
        g_test_failed = true;
        return false;
    }
    return true;
}

bool assert_eq_mem_impl(const void* expected, const void* actual,
                       size_t size, const char* file, int line) {
    if (memcmp(expected, actual, size) != 0) {
        if (g_current_result) {
            g_current_result->failure_file = file;
            g_current_result->failure_line = line;
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                    "Memory comparison failed (%zu bytes)", size);
            g_current_result->failure_message = strdup(buffer);
        }
        g_test_failed = true;
        return false;
    }
    return true;
}

bool assert_null_impl(const void* ptr, const char* expr,
                     const char* file, int line) {
    if (ptr != NULL) {
        if (g_current_result) {
            g_current_result->failure_file = file;
            g_current_result->failure_line = line;
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                    "Expected NULL: %s = %p", expr, ptr);
            g_current_result->failure_message = strdup(buffer);
        }
        g_test_failed = true;
        return false;
    }
    return true;
}

bool assert_not_null_impl(const void* ptr, const char* expr,
                         const char* file, int line) {
    if (ptr == NULL) {
        if (g_current_result) {
            g_current_result->failure_file = file;
            g_current_result->failure_line = line;
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                    "Expected non-NULL: %s", expr);
            g_current_result->failure_message = strdup(buffer);
        }
        g_test_failed = true;
        return false;
    }
    return true;
}

bool assert_near_double_impl(double expected, double actual, double epsilon,
                            const char* file, int line) {
    double diff = expected - actual;
    if (diff < 0) diff = -diff;
    
    if (diff > epsilon) {
        if (g_current_result) {
            g_current_result->failure_file = file;
            g_current_result->failure_line = line;
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                    "Expected: %f ± %f, Actual: %f", 
                    expected, epsilon, actual);
            g_current_result->failure_message = strdup(buffer);
        }
        g_test_failed = true;
        return false;
    }
    return true;
}

// Test failure reporting
void test_fail_impl(const char* message, const char* file, int line) {
    if (g_current_result) {
        g_current_result->failure_file = file;
        g_current_result->failure_line = line;
        g_current_result->failure_message = strdup(message);
    }
    g_test_failed = true;
}

// Test skip
void test_skip_impl(const char* reason, const char* file, int line) {
    if (g_current_result) {
        g_current_result->failure_file = file;
        g_current_result->failure_line = line;
        g_current_result->failure_message = reason ? strdup(reason) : NULL;
        g_current_result->status = TEST_SKIP;
    }
}

// Test logging
void test_log(const char* format, ...) {
    printf("    ");
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

void test_log_verbose(const char* format, ...) {
    if (g_test_options.verbose) {
        printf("    ");
        va_list args;
        va_start(args, format);
        vprintf(format, args);
        va_end(args);
        printf("\n");
    }
}