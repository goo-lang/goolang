#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>

// Define the test function pointer type.
// A test function should return 0 on success and a non-zero value on failure.
typedef int (*test_func_t)(void);

/**
 * @brief Registers a test case with the framework.
 *
 * @param name The name of the test.
 * @param func A pointer to the test function.
 * @return 1 on success, 0 on failure (e.g., test limit reached).
 */
int test_framework_register_test(const char* name, test_func_t func);

/**
 * @brief Runs all registered tests.
 *
 * @return The number of failed tests.
 */
int test_framework_run_all_tests(void);


#endif // TEST_FRAMEWORK_H