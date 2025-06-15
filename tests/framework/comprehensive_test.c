#include "test/test_framework.h"
#include "errors/error.h"
#include "parser/parser_errors.h"
#include <string.h>

// Test fixture for error handling integration
TEST_FIXTURE(IntegrationFixture) {
    ErrorContext* error_ctx;
    ParserErrorState* old_parser_state;
};

SETUP_FIXTURE(IntegrationFixture) {
    IntegrationFixture* fixture = GET_FIXTURE(IntegrationFixture);
    fixture->error_ctx = error_context_new();
    fixture->old_parser_state = g_parser_error_state;
    parser_error_init(fixture->error_ctx);
}

TEARDOWN_FIXTURE(IntegrationFixture) {
    IntegrationFixture* fixture = GET_FIXTURE(IntegrationFixture);
    parser_error_cleanup();
    g_parser_error_state = fixture->old_parser_state;
    error_context_free(fixture->error_ctx);
}

// Error context comprehensive tests
TEST(error_comprehensive, error_builder_advanced) {
    ErrorContext* ctx = error_context_new();
    SourceLocation loc = make_source_location("advanced_test.goo", 15, 3, 200, 25);
    
    // Test complex error building
    ErrorBuilder* builder = error_builder_new(ctx, ERROR_TYPE_MISMATCH);
    ASSERT_NOT_NULL(builder);
    
    error_builder_with_message(builder, 
        "Type mismatch in assignment: cannot assign %s to variable of type %s",
        "string", "int");
    error_builder_with_hint(builder, 
        "Consider using string conversion: int(myString)");
    error_builder_at_location(builder, loc);
    error_builder_emit(builder);
    
    ASSERT_EQ(1, get_error_count(ctx));
    ASSERT_TRUE(has_errors(ctx));
    
    error_context_free(ctx);
    return TEST_PASS;
}

TEST(error_comprehensive, multiple_error_types) {
    ErrorContext* ctx = error_context_new();
    SourceLocation loc1 = make_source_location("test.goo", 10, 1, 100, 10);
    SourceLocation loc2 = make_source_location("test.goo", 15, 5, 150, 8);
    SourceLocation loc3 = make_source_location("test.goo", 20, 2, 200, 15);
    
    // Mix of errors and warnings
    report_error(ctx, ERROR_UNDEFINED_VARIABLE, "Variable 'x' not declared", loc1);
    report_warning(ctx, ERROR_INVALID_CAST, "Implicit cast may lose precision", loc2);
    report_error_with_hint(ctx, ERROR_TYPE_MISMATCH, 
                          "Cannot call function with these arguments",
                          "Expected: func(int, string), got: func(string, int)", 
                          loc3);
    report_note(ctx, "Function declared here", loc1);
    
    ASSERT_EQ(2, get_error_count(ctx));
    ASSERT_EQ(1, get_warning_count(ctx));
    ASSERT_TRUE(has_errors(ctx));
    
    error_context_free(ctx);
    return TEST_PASS;
}

TEST(error_comprehensive, error_limits) {
    ErrorContext* ctx = error_context_new();
    SourceLocation loc = make_source_location("limit_test.goo", 1, 1, 0, 5);
    
    // Test error limit (default is 100)
    for (int i = 0; i < 150; i++) {
        report_error(ctx, ERROR_INTERNAL, "Test error", loc);
    }
    
    // Should stop at limit
    ASSERT_EQ(100, get_error_count(ctx));
    
    error_context_free(ctx);
    return TEST_PASS;
}

// Parser error integration tests
TEST_F(IntegrationFixture, parser_error_basic) {
    IntegrationFixture* fixture = GET_FIXTURE(IntegrationFixture);
    
    ASSERT_NOT_NULL(g_parser_error_state);
    ASSERT_EQ_PTR(fixture->error_ctx, g_parser_error_state->error_ctx);
    ASSERT_FALSE(parser_is_in_recovery());
    
    // Test basic error reporting
    PARSER_ERROR(ERROR_UNEXPECTED_TOKEN, "Unexpected token");
    
    ASSERT_EQ(1, GET_PARSER_ERROR_COUNT());
    ASSERT_TRUE(PARSER_HAS_ERRORS());
    
    return TEST_PASS;
}

TEST_F(IntegrationFixture, parser_recovery_mode) {
    ASSERT_FALSE(parser_is_in_recovery());
    
    // Enter recovery mode
    ASSERT_TRUE(parser_enter_recovery_mode());
    ASSERT_TRUE(parser_is_in_recovery());
    
    // Nested recovery
    ASSERT_TRUE(parser_enter_recovery_mode());
    ASSERT_TRUE(parser_is_in_recovery());
    
    // Exit recovery (nested)
    parser_exit_recovery_mode();
    ASSERT_TRUE(parser_is_in_recovery()); // Still in recovery
    
    // Exit recovery (final)
    parser_exit_recovery_mode();
    ASSERT_FALSE(parser_is_in_recovery());
    
    return TEST_PASS;
}

TEST_F(IntegrationFixture, parser_synchronization) {
    // Test sync token management
    parser_add_sync_token(TOKEN_SEMICOLON);
    parser_add_sync_token(TOKEN_RBRACE);
    
    // These would test actual synchronization with real tokens
    // For now, just test the setup
    ASSERT_NOT_NULL(g_parser_error_state);
    ASSERT_EQ(3, g_parser_error_state->sync_token_count); // Including default EOF
    
    return TEST_PASS;
}

// Test framework advanced features
TEST(framework_advanced, expect_vs_assert) {
    // Test non-fatal expectations
    bool result1 = EXPECT_TRUE(true);
    bool result2 = EXPECT_FALSE(false);
    bool result3 = EXPECT_EQ(5, 5);
    
    ASSERT_TRUE(result1);
    ASSERT_TRUE(result2);
    ASSERT_TRUE(result3);
    
    // Test failed expectation (non-fatal)
    bool result4 = EXPECT_EQ(1, 2);
    ASSERT_FALSE(result4);
    
    return TEST_PASS;
}

TEST(framework_advanced, string_utilities) {
    ASSERT_EQ_STR("hello", "hello");
    
    // Test null string handling
    ASSERT_EQ_STR(NULL, NULL);
    
    // Test memory comparison
    char buffer1[] = {1, 2, 3, 4};
    char buffer2[] = {1, 2, 3, 4};
    ASSERT_EQ_MEM(buffer1, buffer2, 4);
    
    return TEST_PASS;
}

TEST(framework_advanced, numeric_precision) {
    double pi = 3.14159265359;
    ASSERT_NEAR(pi, 3.14159, 0.001);
    
    float small = 0.001f;
    ASSERT_NEAR(small, 0.001, 0.0001);
    
    return TEST_PASS;
}

// Error code coverage tests
TEST(error_codes, lexer_errors) {
    ASSERT_EQ_STR("E1000", error_code_to_string(ERROR_INVALID_CHARACTER));
    ASSERT_EQ_STR("E1001", error_code_to_string(ERROR_UNTERMINATED_STRING));
    ASSERT_EQ_STR("E1002", error_code_to_string(ERROR_INVALID_NUMBER));
    ASSERT_EQ_STR("E1003", error_code_to_string(ERROR_INVALID_ESCAPE));
    
    return TEST_PASS;
}

TEST(error_codes, parser_errors) {
    ASSERT_EQ_STR("E2000", error_code_to_string(ERROR_UNEXPECTED_TOKEN));
    ASSERT_EQ_STR("E2001", error_code_to_string(ERROR_MISSING_SEMICOLON));
    ASSERT_EQ_STR("E2002", error_code_to_string(ERROR_MISSING_CLOSING_PAREN));
    ASSERT_EQ_STR("E2003", error_code_to_string(ERROR_MISSING_CLOSING_BRACE));
    
    return TEST_PASS;
}

TEST(error_codes, type_errors) {
    ASSERT_EQ_STR("E3000", error_code_to_string(ERROR_TYPE_MISMATCH));
    ASSERT_EQ_STR("E3001", error_code_to_string(ERROR_UNDEFINED_VARIABLE));
    ASSERT_EQ_STR("E3002", error_code_to_string(ERROR_UNDEFINED_TYPE));
    ASSERT_EQ_STR("E3003", error_code_to_string(ERROR_INVALID_CAST));
    
    return TEST_PASS;
}

// Performance and stress tests
TEST(stress, many_errors) {
    ErrorContext* ctx = error_context_new();
    SourceLocation loc = make_source_location("stress.goo", 1, 1, 0, 1);
    
    double start_time = test_get_time_ms();
    
    // Create many errors quickly
    for (int i = 0; i < 1000; i++) {
        report_error(ctx, ERROR_INTERNAL, "Stress test error", loc);
    }
    
    double end_time = test_get_time_ms();
    double duration = end_time - start_time;
    
    test_log("Created 100 errors in %.2f ms", duration);
    
    // Verify we hit the limit (100)
    ASSERT_EQ(100, get_error_count(ctx));
    
    error_context_free(ctx);
    return TEST_PASS;
}

// Test logging and output
TEST(output, test_logging) {
    test_log("This is a test log message");
    test_log("Formatted message: %d + %d = %d", 2, 3, 5);
    test_log_verbose("This verbose message may not appear");
    
    return TEST_PASS;
}

// Register all test functions
void register_error_tests(void) {
    // Auto-registered via constructors
}

void register_lexer_tests(void) {
    // Placeholder
}

void register_parser_tests(void) {
    // Placeholder
}

void register_type_tests(void) {
    // Placeholder
}

void register_codegen_tests(void) {
    // Placeholder
}