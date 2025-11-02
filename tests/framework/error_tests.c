
#include "test/test_framework.h"

// Example test case
TestStatus example_error_test(void* data) {
    (void)data; // Unused
    ASSERT_TRUE(1 == 1);
    return TEST_PASS;
}

// Register all error tests
void register_error_tests(void) {
    register_test("errors", "example_error_test", example_error_test, __FILE__, __LINE__);
}
