#include "test/test_framework.h"
#include "errors/error.h"

// Simple test runner without LLVM dependencies
int main(int argc, char** argv) {
    TestOptions options = {
        .filter = NULL,
        .suite_filter = NULL,
        .verbose = true,
        .stop_on_failure = false,
        .list_only = false,
        .shuffle = false,
        .repeat = 1,
        .parallel = 0,
        .output_format = "text",
        .output_file = NULL
    };
    
    // Set global options
    g_test_options = options;
    
    printf("Running minimal test suite...\n");
    
    // Run all registered tests
    run_all_tests(&options);
    
    return 0;
}