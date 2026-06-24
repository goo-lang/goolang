/*
 * test/test_framework.h — public interface for the xUnit-style C test
 * framework implemented in tests/framework/test_framework.c.
 *
 * The header was never committed alongside the implementation, which left
 * `make test` (and every target linking $(OBJS)) failing to compile on a
 * missing include. It is reconstructed here from the framework source and
 * its call sites under tests/unit and tests/integration.
 *
 * Model: each TEST()/TEST_F() expands to a function plus a
 * __attribute__((constructor)) that self-registers the test into the global
 * g_test_registry before main() runs. Fixtures store their state in a fixed
 * 1 KB blob (g_current_fixture->data) that GET_FIXTURE() casts to the
 * fixture struct — fixtures must stay POD and well under 1 KB.
 */
#ifndef GOO_TEST_FRAMEWORK_H
#define GOO_TEST_FRAMEWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>  /* tests routinely malloc/free; mirror the original header */
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Outcome of a single test. Test functions return one of these; the runner
 * also flips a test to TEST_FAIL when an assertion records a failure. */
typedef enum {
    TEST_PASS = 0,
    TEST_FAIL,
    TEST_SKIP,
    TEST_ERROR
} TestStatus;

/* A test body has signature `TestStatus(void)`; setup/teardown are void. */
typedef TestStatus (*TestFunc)(void);
typedef void (*SetupFunc)(void);
typedef void (*TeardownFunc)(void);

/* A registered test. Linked into g_test_registry at load time. */
typedef struct TestCase {
    const char* name;
    const char* suite;
    TestFunc test_func;
    SetupFunc setup;
    TeardownFunc teardown;
    const char* file;
    int line;
    struct TestCase* next;
} TestCase;

/* Outcome record for one executed test. */
typedef struct TestResult {
    TestCase* test;
    TestStatus status;
    double duration_ms;
    char* failure_message;
    const char* failure_file;
    int failure_line;
    struct TestResult* next;
} TestResult;

/* Active fixture for a TEST_F test. `data` is a 1 KB blob the framework
 * allocates around setup/teardown; GET_FIXTURE() casts it to the fixture
 * struct declared via TEST_FIXTURE(). */
typedef struct TestFixture {
    const char* name;
    SetupFunc setup;
    TeardownFunc teardown;
    void* data;
} TestFixture;

/* A collection of matched tests plus its aggregate results/counters. */
typedef struct TestSuite {
    const char* name;
    TestCase* tests;
    TestResult* results;
    size_t total_tests;
    size_t passed;
    size_t failed;
    size_t skipped;
    size_t errors;
    double total_duration_ms;
} TestSuite;

/* Runner configuration, populated from CLI flags in test_main.c. */
typedef struct TestOptions {
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

/* Global state owned by the framework implementation. */
extern TestCase* g_test_registry;
extern TestOptions g_test_options;
extern TestFixture* g_current_fixture;

/* Registration (normally reached through the TEST/TEST_F macros). */
void register_test(const char* suite, const char* name, TestFunc func,
                   const char* file, int line);
void register_test_with_fixture(const char* suite, const char* name,
                                TestFunc func, SetupFunc setup,
                                TeardownFunc teardown,
                                const char* file, int line);

/* Discovery and execution. */
TestSuite* discover_tests(const char* pattern);
void free_test_suite(TestSuite* suite);
double test_get_time_ms(void);
TestResult* run_test(TestCase* test);
TestSuite* run_test_suite(TestSuite* suite, const TestOptions* options);
void run_all_tests(const TestOptions* options);

/* Reporting. */
void print_test_results(const TestSuite* suite);
void print_tap_results(const TestSuite* suite, FILE* out);
void print_junit_results(const TestSuite* suite, FILE* out);

/* Assertion backends — return true on success, false (and record the
 * failure on the active result) otherwise. */
bool assert_true_impl(bool condition, const char* expr,
                      const char* file, int line);
bool assert_false_impl(bool condition, const char* expr,
                       const char* file, int line);
bool assert_eq_int_impl(int expected, int actual,
                        const char* file, int line);
bool assert_eq_uint_impl(unsigned int expected, unsigned int actual,
                         const char* file, int line);
bool assert_eq_ptr_impl(const void* expected, const void* actual,
                        const char* file, int line);
bool assert_eq_str_impl(const char* expected, const char* actual,
                        const char* file, int line);
bool assert_eq_mem_impl(const void* expected, const void* actual,
                        size_t size, const char* file, int line);
bool assert_null_impl(const void* ptr, const char* expr,
                      const char* file, int line);
bool assert_not_null_impl(const void* ptr, const char* expr,
                          const char* file, int line);
bool assert_near_double_impl(double expected, double actual, double epsilon,
                             const char* file, int line);

/* Explicit failure / skip / logging. */
void test_fail_impl(const char* message, const char* file, int line);
void test_skip_impl(const char* reason, const char* file, int line);
void test_log(const char* format, ...);
void test_log_verbose(const char* format, ...);

/*
 * Test definition macros.
 *
 * TEST(suite, name) { ... }            — a standalone test.
 * TEST_F(Fixture, name) { ... }        — a test using a fixture.
 *
 * Each expands to a function definition preceded by a constructor that
 * registers it, so the trailing `{ ... }` in the source becomes the body.
 */
#define TEST(suite, name)                                                   \
    static TestStatus suite##_##name(void);                                 \
    __attribute__((constructor)) static void register_##suite##_##name(void)\
    {                                                                       \
        register_test(#suite, #name, suite##_##name, __FILE__, __LINE__);   \
    }                                                                       \
    static TestStatus suite##_##name(void)

#define TEST_F(fixture, name)                                               \
    static TestStatus fixture##_##name(void);                               \
    __attribute__((constructor)) static void register_##fixture##_##name(void)\
    {                                                                       \
        register_test_with_fixture(#fixture, #name, fixture##_##name,       \
                                   fixture##_setup, fixture##_teardown,      \
                                   __FILE__, __LINE__);                      \
    }                                                                       \
    static TestStatus fixture##_##name(void)

/*
 * Fixture macros.
 *
 * TEST_FIXTURE(Name) { fields };   — declares `struct Name`.
 * SETUP_FIXTURE(Name) { ... }      — defines Name's setup function.
 * TEARDOWN_FIXTURE(Name) { ... }   — defines Name's teardown function.
 * GET_FIXTURE(Name)                — Name* view over the active fixture blob.
 */
#define TEST_FIXTURE(name)                                                  \
    typedef struct name name;                                               \
    struct name

#define SETUP_FIXTURE(name) static void name##_setup(void)
#define TEARDOWN_FIXTURE(name) static void name##_teardown(void)
#define GET_FIXTURE(name)                                                   \
    ((name*)(g_current_fixture ? g_current_fixture->data : NULL))

/*
 * Assertion macros. ASSERT_* abort the current test on failure (return
 * TEST_FAIL); EXPECT_* record the failure but let the test continue.
 */
#define ASSERT_TRUE(cond)                                                   \
    do { if (!assert_true_impl((cond), #cond, __FILE__, __LINE__))          \
         return TEST_FAIL; } while (0)
#define ASSERT_FALSE(cond)                                                  \
    do { if (!assert_false_impl((cond), #cond, __FILE__, __LINE__))         \
         return TEST_FAIL; } while (0)
#define ASSERT_EQ(expected, actual)                                         \
    do { if (!assert_eq_int_impl((int)(expected), (int)(actual),            \
                                 __FILE__, __LINE__))                        \
         return TEST_FAIL; } while (0)
#define ASSERT_EQ_UINT(expected, actual)                                    \
    do { if (!assert_eq_uint_impl((unsigned int)(expected),                 \
                                  (unsigned int)(actual), __FILE__, __LINE__))\
         return TEST_FAIL; } while (0)
#define ASSERT_EQ_PTR(expected, actual)                                     \
    do { if (!assert_eq_ptr_impl((const void*)(expected),                   \
                                 (const void*)(actual), __FILE__, __LINE__)) \
         return TEST_FAIL; } while (0)
#define ASSERT_EQ_STR(expected, actual)                                     \
    do { if (!assert_eq_str_impl((expected), (actual), __FILE__, __LINE__)) \
         return TEST_FAIL; } while (0)
#define ASSERT_EQ_MEM(expected, actual, size)                               \
    do { if (!assert_eq_mem_impl((expected), (actual), (size),              \
                                 __FILE__, __LINE__))                        \
         return TEST_FAIL; } while (0)
#define ASSERT_NULL(ptr)                                                    \
    do { if (!assert_null_impl((ptr), #ptr, __FILE__, __LINE__))            \
         return TEST_FAIL; } while (0)
#define ASSERT_NOT_NULL(ptr)                                                \
    do { if (!assert_not_null_impl((ptr), #ptr, __FILE__, __LINE__))        \
         return TEST_FAIL; } while (0)
#define ASSERT_NEAR(expected, actual, epsilon)                              \
    do { if (!assert_near_double_impl((expected), (actual), (epsilon),      \
                                      __FILE__, __LINE__))                   \
         return TEST_FAIL; } while (0)

#define EXPECT_TRUE(cond)                                                   \
    ((void)assert_true_impl((cond), #cond, __FILE__, __LINE__))
#define EXPECT_FALSE(cond)                                                  \
    ((void)assert_false_impl((cond), #cond, __FILE__, __LINE__))
#define EXPECT_EQ(expected, actual)                                         \
    ((void)assert_eq_int_impl((int)(expected), (int)(actual),               \
                              __FILE__, __LINE__))

/* Explicit failure/skip from inside a test body. */
#define FAIL_TEST(msg)                                                      \
    do { test_fail_impl((msg), __FILE__, __LINE__); return TEST_FAIL; }     \
    while (0)
#define SKIP_TEST(reason)                                                   \
    do { test_skip_impl((reason), __FILE__, __LINE__); return TEST_SKIP; }  \
    while (0)

#ifdef __cplusplus
}
#endif

#endif /* GOO_TEST_FRAMEWORK_H */
