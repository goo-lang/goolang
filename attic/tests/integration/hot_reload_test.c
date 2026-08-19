#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "hot_reload.h"
#include "ast.h"
#include "types.h"

// ANSI color codes for output
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

// Test result tracking
typedef struct {
    int total;
    int passed;
    int failed;
} TestResults;

static TestResults results = {0, 0, 0};

// Test macros
#define TEST(name) \
    printf("\n" ANSI_COLOR_CYAN "Testing: %s" ANSI_COLOR_RESET "\n", #name); \
    results.total++; \
    if (test_##name())

#define PASS() \
    do { \
        printf(ANSI_COLOR_GREEN "✓ PASSED" ANSI_COLOR_RESET "\n"); \
        results.passed++; \
        return 1; \
    } while(0)

#define FAIL(msg) \
    do { \
        printf(ANSI_COLOR_RED "✗ FAILED: %s" ANSI_COLOR_RESET "\n", msg); \
        results.failed++; \
        return 0; \
    } while(0)

// Mock AST node for testing
static FuncDeclNode* create_mock_function(const char* name) {
    FuncDeclNode* func = calloc(1, sizeof(FuncDeclNode));
    if (!func) return NULL;
    
    func->base.type = AST_FUNC_DECL;
    func->name = strdup(name);
    
    return func;
}

static void free_mock_function(FuncDeclNode* func) {
    if (!func) return;
    if (func->name) {
        free(func->name);
    }
    free(func);
}

// Test: Context creation and destruction
static int test_context_lifecycle() {
    HotReloadContext* ctx = hot_reload_context_new();
    if (!ctx) FAIL("Failed to create context");
    
    // Enable hot reload
    if (hot_reload_enable(ctx, HOT_RELOAD_CAP_FUNCTION | HOT_RELOAD_CAP_TYPE) != 0) {
        hot_reload_context_free(ctx);
        FAIL("Failed to enable hot reload");
    }
    
    // Disable hot reload
    if (hot_reload_disable(ctx) != 0) {
        hot_reload_context_free(ctx);
        FAIL("Failed to disable hot reload");
    }
    
    hot_reload_context_free(ctx);
    PASS();
}

// Test: Function registration
static int test_function_registration() {
    HotReloadContext* ctx = hot_reload_context_new();
    if (!ctx) FAIL("Failed to create context");
    
    hot_reload_enable(ctx, HOT_RELOAD_CAP_FUNCTION);
    
    // Create mock function
    FuncDeclNode* func = create_mock_function("testFunction");
    if (!func) {
        hot_reload_context_free(ctx);
        FAIL("Failed to create mock function");
    }
    
    // Register function
    int result = hot_reload_register_function(ctx, "testFunction", func, 
                                             HOT_RELOAD_CAP_FUNCTION | HOT_RELOAD_CAP_STATE_MIGRATION);
    if (result != 0) {
        free_mock_function(func);
        hot_reload_context_free(ctx);
        FAIL("Failed to register function");
    }
    
    // Try to register same function again (should fail)
    result = hot_reload_register_function(ctx, "testFunction", func, HOT_RELOAD_CAP_FUNCTION);
    if (result == 0) {
        free_mock_function(func);
        hot_reload_context_free(ctx);
        FAIL("Should not allow duplicate registration");
    }
    
    free_mock_function(func);
    hot_reload_context_free(ctx);
    PASS();
}

// Test: Type registration
static int test_type_registration() {
    HotReloadContext* ctx = hot_reload_context_new();
    if (!ctx) FAIL("Failed to create context");
    
    hot_reload_enable(ctx, HOT_RELOAD_CAP_TYPE);
    
    // Create mock type
    Type* type = calloc(1, sizeof(Type));
    if (!type) {
        hot_reload_context_free(ctx);
        FAIL("Failed to create mock type");
    }
    type->kind = TYPE_STRUCT;
    type->name = "TestStruct";
    
    // Register type
    int result = hot_reload_register_type(ctx, "TestStruct", type, MIGRATION_STRATEGY_DEFAULT);
    if (result != 0) {
        free(type);
        hot_reload_context_free(ctx);
        FAIL("Failed to register type");
    }
    
    // Try different migration strategies
    Type* type2 = calloc(1, sizeof(Type));
    type2->kind = TYPE_STRUCT;
    type2->name = "TestStruct2";
    
    result = hot_reload_register_type(ctx, "TestStruct2", type2, MIGRATION_STRATEGY_SERIALIZE);
    if (result != 0) {
        free(type);
        free(type2);
        hot_reload_context_free(ctx);
        FAIL("Failed to register type with serialize strategy");
    }
    
    free(type);
    free(type2);
    hot_reload_context_free(ctx);
    PASS();
}

// Test: State preservation
static int test_state_preservation() {
    HotReloadContext* ctx = hot_reload_context_new();
    if (!ctx) FAIL("Failed to create context");
    
    hot_reload_enable(ctx, HOT_RELOAD_CAP_STATE_MIGRATION);
    
    // Create test data
    typedef struct {
        int value;
        double factor;
        char name[32];
    } TestData;
    
    TestData original = {
        .value = 42,
        .factor = 3.14,
        .name = "test_data"
    };
    
    // Preserve state
    int result = hot_reload_preserve_state(ctx, "TestData", &original, sizeof(TestData));
    if (result != 0) {
        hot_reload_context_free(ctx);
        FAIL("Failed to preserve state");
    }
    
    // Restore state
    size_t restored_size = 0;
    TestData* restored = (TestData*)hot_reload_restore_state(ctx, "TestData", &restored_size);
    if (!restored || restored_size != sizeof(TestData)) {
        hot_reload_context_free(ctx);
        FAIL("Failed to restore state");
    }
    
    // Verify data integrity
    if (restored->value != original.value ||
        restored->factor != original.factor ||
        strcmp(restored->name, original.name) != 0) {
        free(restored);
        hot_reload_context_free(ctx);
        FAIL("Restored data doesn't match original");
    }
    
    free(restored);
    hot_reload_context_free(ctx);
    PASS();
}

// Test: File watching
static int file_change_detected = 0;

static void file_change_callback(const char* path, void* context) {
    (void)context; // Suppress unused parameter warning
    printf("File change detected: %s\n", path);
    file_change_detected = 1;
}

static int test_file_watching() {
    HotReloadContext* ctx = hot_reload_context_new();
    if (!ctx) FAIL("Failed to create context");
    
    hot_reload_enable(ctx, HOT_RELOAD_CAP_FUNCTION);
    
    // Create a temporary test file
    const char* test_file = "/tmp/hot_reload_test.txt";
    FILE* f = fopen(test_file, "w");
    if (!f) {
        hot_reload_context_free(ctx);
        FAIL("Failed to create test file");
    }
    fprintf(f, "Initial content\n");
    fclose(f);
    
    // Watch the file
    int result = hot_reload_watch_file(ctx, test_file, file_change_callback, NULL);
    if (result != 0) {
        unlink(test_file);
        hot_reload_context_free(ctx);
        FAIL("Failed to watch file");
    }
    
    // Modify the file
    file_change_detected = 0;
    f = fopen(test_file, "a");
    if (f) {
        fprintf(f, "Modified content\n");
        fclose(f);
    }
    
    // Note: In a real implementation, we'd need to wait for the file system
    // event to be detected. For now, we'll just check that watching was set up.
    
    // Unwatch the file
    result = hot_reload_unwatch(ctx, test_file);
    
    // Clean up
    unlink(test_file);
    hot_reload_context_free(ctx);
    PASS();
}

// Test: Hot reload simulation
static int test_hot_reload_simulation() {
    HotReloadContext* ctx = hot_reload_context_new();
    if (!ctx) FAIL("Failed to create context");
    
    hot_reload_enable(ctx, HOT_RELOAD_CAP_FUNCTION);
    
    // Register a function
    FuncDeclNode* func = create_mock_function("hotFunction");
    hot_reload_register_function(ctx, "hotFunction", func, HOT_RELOAD_CAP_FUNCTION);
    
    // Simulate reload
    HotReloadStatus status = hot_reload_function(ctx, "hotFunction");
    if (status != HOT_RELOAD_SUCCESS) {
        free_mock_function(func);
        hot_reload_context_free(ctx);
        FAIL("Hot reload failed");
    }
    
    // Try to reload non-existent function
    status = hot_reload_function(ctx, "nonExistent");
    if (status == HOT_RELOAD_SUCCESS) {
        free_mock_function(func);
        hot_reload_context_free(ctx);
        FAIL("Should fail for non-existent function");
    }
    
    // Test reload when disabled
    hot_reload_disable(ctx);
    status = hot_reload_function(ctx, "hotFunction");
    if (status != HOT_RELOAD_NOT_SUPPORTED) {
        free_mock_function(func);
        hot_reload_context_free(ctx);
        FAIL("Should return NOT_SUPPORTED when disabled");
    }
    
    free_mock_function(func);
    hot_reload_context_free(ctx);
    PASS();
}

// Test: Statistics tracking
static int test_statistics() {
    HotReloadContext* ctx = hot_reload_context_new();
    if (!ctx) FAIL("Failed to create context");
    
    hot_reload_enable(ctx, HOT_RELOAD_CAP_FUNCTION);
    
    // Register multiple functions
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "func_%d", i);
        FuncDeclNode* func = create_mock_function(name);
        hot_reload_register_function(ctx, name, func, HOT_RELOAD_CAP_FUNCTION);
    }
    
    // Perform some reloads
    hot_reload_function(ctx, "func_0");
    hot_reload_function(ctx, "func_1");
    hot_reload_function(ctx, "func_2");
    
    // Print statistics
    printf("\n");
    hot_reload_print_statistics(ctx);
    
    // Clean up (note: we're leaking the mock functions for simplicity)
    hot_reload_context_free(ctx);
    PASS();
}

// Test: Safe points
static int test_safe_points() {
    HotReloadContext* ctx = hot_reload_context_new();
    if (!ctx) FAIL("Failed to create context");
    
    hot_reload_enable(ctx, HOT_RELOAD_CAP_SAFE_POINT);
    
    // Initially should be safe
    if (!hot_reload_is_safe_to_reload(ctx)) {
        hot_reload_context_free(ctx);
        FAIL("Should be safe initially");
    }
    
    // Enter safe point
    int result = hot_reload_enter_safe_point(ctx);
    if (result != 0) {
        hot_reload_context_free(ctx);
        FAIL("Failed to enter safe point");
    }
    
    // Should still be safe
    if (!hot_reload_is_safe_to_reload(ctx)) {
        hot_reload_context_free(ctx);
        FAIL("Should be safe in safe point");
    }
    
    // Exit safe point
    result = hot_reload_exit_safe_point(ctx);
    if (result != 0) {
        hot_reload_context_free(ctx);
        FAIL("Failed to exit safe point");
    }
    
    hot_reload_context_free(ctx);
    PASS();
}

// Main test runner
int main() {
    printf(ANSI_COLOR_CYAN "=== Hot Reload System Tests ===" ANSI_COLOR_RESET "\n");
    
    TEST(context_lifecycle) {} else {}
    TEST(function_registration) {} else {}
    TEST(type_registration) {} else {}
    TEST(state_preservation) {} else {}
    TEST(file_watching) {} else {}
    TEST(hot_reload_simulation) {} else {}
    TEST(statistics) {} else {}
    TEST(safe_points) {} else {}
    
    // Print summary
    printf("\n" ANSI_COLOR_CYAN "=== Test Summary ===" ANSI_COLOR_RESET "\n");
    printf("Total tests: %d\n", results.total);
    printf(ANSI_COLOR_GREEN "Passed: %d" ANSI_COLOR_RESET "\n", results.passed);
    if (results.failed > 0) {
        printf(ANSI_COLOR_RED "Failed: %d" ANSI_COLOR_RESET "\n", results.failed);
    }
    
    double pass_rate = (results.total > 0) ? 
                      (double)results.passed / results.total * 100 : 0;
    printf("Pass rate: %.1f%%\n", pass_rate);
    
    return results.failed > 0 ? 1 : 0;
}