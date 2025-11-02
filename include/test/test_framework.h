
#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// Test status enum
typedef enum {
    TEST_PASS,
    TEST_FAIL,
    TEST_SKIP,
    TEST_ERROR
} TestStatus;

// Forward declarations
typedef struct TestCase TestCase;
typedef struct TestResult TestResult;
typedef struct TestSuite TestSuite;
typedef struct TestFixture TestFixture;

// Test function prototypes
typedef TestStatus (*TestFunc)(void*);
typedef void (*SetupFunc)(void*);
typedef void (*TeardownFunc)(void*);

// Test case structure
struct TestCase {
    const char* name;
    const char* suite;
    TestFunc test_func;
    SetupFunc setup;
    TeardownFunc teardown;
    const char* file;
    int line;
    TestCase* next;
};

// Test result structure
struct TestResult {
    TestCase* test;
    TestStatus status;
    char* failure_message;
    const char* failure_file;
    int failure_line;
    double duration_ms;
    TestResult* next;
};

// Test suite structure
struct TestSuite {
    const char* name;
    TestCase* tests;
    TestResult* results;
    size_t total_tests;
    size_t passed;
    size_t failed;
    size_t skipped;
    size_t errors;
    double total_duration_ms;
};

// Test options structure
typedef struct {
    const char* filter;
    const char* suite_filter;
    bool verbose;
    bool stop_on_failure;
    bool list_only;
    bool shuffle;
    int repeat;
    int parallel;
    const char* output_format;
    const char* output_file;
} TestOptions;

// Test fixture structure
struct TestFixture {
    const char* name;
    SetupFunc setup;
    TeardownFunc teardown;
    void* data;
};

// Global test variables
extern TestCase* g_test_registry;
extern TestOptions g_test_options;

// Test framework functions
void register_test(const char* suite, const char* name, TestFunc func, const char* file, int line);
void register_test_with_fixture(const char* suite, const char* name, TestFunc func, SetupFunc setup, TeardownFunc teardown, const char* file, int line);
TestSuite* discover_tests(const char* pattern);
void free_test_suite(TestSuite* suite);
double test_get_time_ms(void);
TestResult* run_test(TestCase* test);
TestSuite* run_test_suite(TestSuite* suite, const TestOptions* options);
void run_all_tests(const TestOptions* options);
void print_test_results(const TestSuite* suite);
void print_tap_results(const TestSuite* suite, FILE* out);
void print_junit_results(const TestSuite* suite, FILE* out);

// Assertion implementations
bool assert_true_impl(bool condition, const char* expr, const char* file, int line);
bool assert_false_impl(bool condition, const char* expr, const char* file, int line);
bool assert_eq_int_impl(int expected, int actual, const char* file, int line);
bool assert_eq_uint_impl(unsigned int expected, unsigned int actual, const char* file, int line);
bool assert_eq_ptr_impl(const void* expected, const void* actual, const char* file, int line);
bool assert_eq_str_impl(const char* expected, const char* actual, const char* file, int line);
bool assert_eq_mem_impl(const void* expected, const void* actual, size_t size, const char* file, int line);
bool assert_null_impl(const void* ptr, const char* expr, const char* file, int line);
bool assert_not_null_impl(const void* ptr, const char* expr, const char* file, int line);
bool assert_near_double_impl(double expected, double actual, double epsilon, const char* file, int line);

// Test failure and skip functions
void test_fail_impl(const char* message, const char* file, int line);
void test_skip_impl(const char* reason, const char* file, int line);

// Logging functions
void test_log(const char* format, ...);
void test_log_verbose(const char* format, ...);

// Assertion macros
#define ASSERT_TRUE(cond) assert_true_impl(cond, #cond, __FILE__, __LINE__)
#define ASSERT_FALSE(cond) assert_false_impl(cond, #cond, __FILE__, __LINE__)
#define ASSERT_EQ_INT(exp, act) assert_eq_int_impl(exp, act, __FILE__, __LINE__)
#define ASSERT_EQ_UINT(exp, act) assert_eq_uint_impl(exp, act, __FILE__, __LINE__)
#define ASSERT_EQ_PTR(exp, act) assert_eq_ptr_impl(exp, act, __FILE__, __LINE__)
#define ASSERT_EQ_STR(exp, act) assert_eq_str_impl(exp, act, __FILE__, __LINE__)
#define ASSERT_EQ_MEM(exp, act, size) assert_eq_mem_impl(exp, act, size, __FILE__, __LINE__)
#define ASSERT_NULL(ptr) assert_null_impl(ptr, #ptr, __FILE__, __LINE__)
#define ASSERT_NOT_NULL(ptr) assert_not_null_impl(ptr, #ptr, __FILE__, __LINE__)
#define ASSERT_NEAR_DOUBLE(exp, act, eps) assert_near_double_impl(exp, act, eps, __FILE__, __LINE__)

#define TEST_FAIL(msg) test_fail_impl(msg, __FILE__, __LINE__)
#define TEST_SKIP(reason) test_skip_impl(reason, __FILE__, __LINE__)

#endif // TEST_FRAMEWORK_H
