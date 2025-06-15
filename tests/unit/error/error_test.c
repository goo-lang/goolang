#include "test/test_framework.h"
#include "errors/error.h"
#include <string.h>

// Test fixture for error context
TEST_FIXTURE(ErrorContextFixture) {
    ErrorContext* ctx;
};

SETUP_FIXTURE(ErrorContextFixture) {
    ErrorContextFixture* fixture = GET_FIXTURE(ErrorContextFixture);
    fixture->ctx = error_context_new();
}

TEARDOWN_FIXTURE(ErrorContextFixture) {
    ErrorContextFixture* fixture = GET_FIXTURE(ErrorContextFixture);
    error_context_free(fixture->ctx);
}

// Basic error context tests
TEST(error, context_creation) {
    ErrorContext* ctx = error_context_new();
    ASSERT_NOT_NULL(ctx);
    ASSERT_EQ(0, get_error_count(ctx));
    ASSERT_EQ(0, get_warning_count(ctx));
    ASSERT_FALSE(has_errors(ctx));
    error_context_free(ctx);
    return TEST_PASS;
}

TEST(error, basic_error_reporting) {
    ErrorContext* ctx = error_context_new();
    SourceLocation loc = make_source_location("test.goo", 10, 5, 100, 10);
    
    report_error(ctx, ERROR_TYPE_MISMATCH, "Type mismatch error", loc);
    
    ASSERT_EQ(1, get_error_count(ctx));
    ASSERT_TRUE(has_errors(ctx));
    
    error_context_free(ctx);
    return TEST_PASS;
}

TEST(error, warning_reporting) {
    ErrorContext* ctx = error_context_new();
    SourceLocation loc = make_source_location("test.goo", 5, 1, 50, 5);
    
    report_warning(ctx, ERROR_UNDEFINED_VARIABLE, "Unused variable", loc);
    
    ASSERT_EQ(0, get_error_count(ctx));
    ASSERT_EQ(1, get_warning_count(ctx));
    ASSERT_FALSE(has_errors(ctx));
    
    error_context_free(ctx);
    return TEST_PASS;
}

TEST(error, multiple_errors) {
    ErrorContext* ctx = error_context_new();
    SourceLocation loc1 = make_source_location("test.goo", 10, 5, 100, 10);
    SourceLocation loc2 = make_source_location("test.goo", 15, 2, 150, 8);
    
    report_error(ctx, ERROR_TYPE_MISMATCH, "First error", loc1);
    report_error(ctx, ERROR_UNDEFINED_VARIABLE, "Second error", loc2);
    report_warning(ctx, ERROR_INVALID_CAST, "Warning message", loc1);
    
    ASSERT_EQ(2, get_error_count(ctx));
    ASSERT_EQ(1, get_warning_count(ctx));
    ASSERT_TRUE(has_errors(ctx));
    
    error_context_free(ctx);
    return TEST_PASS;
}

TEST(error, panic_mode) {
    ErrorContext* ctx = error_context_new();
    
    ASSERT_FALSE(is_in_panic_mode(ctx));
    
    enter_panic_mode(ctx, 3);
    ASSERT_TRUE(is_in_panic_mode(ctx));
    
    exit_panic_mode(ctx);
    ASSERT_FALSE(is_in_panic_mode(ctx));
    
    error_context_free(ctx);
    return TEST_PASS;
}

TEST(error, error_with_hint) {
    ErrorContext* ctx = error_context_new();
    SourceLocation loc = make_source_location("test.goo", 20, 10, 200, 15);
    
    report_error_with_hint(ctx, ERROR_INVALID_EXPRESSION, 
                          "Invalid expression syntax",
                          "Try adding parentheses around the expression", 
                          loc);
    
    ASSERT_EQ(1, get_error_count(ctx));
    ASSERT_TRUE(has_errors(ctx));
    
    error_context_free(ctx);
    return TEST_PASS;
}

TEST(error, source_location) {
    SourceLocation loc = make_source_location("example.goo", 42, 10, 500, 20);
    
    ASSERT_TRUE(source_location_is_valid(&loc));
    ASSERT_EQ_STR("example.goo", loc.filename);
    ASSERT_EQ(42, loc.line);
    ASSERT_EQ(10, loc.column);
    ASSERT_EQ(500, loc.offset);
    ASSERT_EQ(20, loc.length);
    
    return TEST_PASS;
}

TEST(error, empty_source_location) {
    SourceLocation loc = empty_source_location();
    
    ASSERT_FALSE(source_location_is_valid(&loc));
    ASSERT_NULL(loc.filename);
    ASSERT_EQ(0, loc.line);
    
    return TEST_PASS;
}

TEST(error, error_code_strings) {
    ASSERT_EQ_STR("E3000", error_code_to_string(ERROR_TYPE_MISMATCH));
    ASSERT_EQ_STR("E3001", error_code_to_string(ERROR_UNDEFINED_VARIABLE));
    ASSERT_EQ_STR("E2000", error_code_to_string(ERROR_UNEXPECTED_TOKEN));
    ASSERT_EQ_STR("E1000", error_code_to_string(ERROR_INVALID_CHARACTER));
    
    return TEST_PASS;
}

TEST(error, severity_strings) {
    ASSERT_EQ_STR("error", error_severity_to_string(ERROR_SEVERITY_ERROR));
    ASSERT_EQ_STR("warning", error_severity_to_string(ERROR_SEVERITY_WARNING));
    ASSERT_EQ_STR("note", error_severity_to_string(ERROR_SEVERITY_NOTE));
    ASSERT_EQ_STR("fatal error", error_severity_to_string(ERROR_SEVERITY_FATAL));
    
    return TEST_PASS;
}

TEST(error, category_strings) {
    ASSERT_EQ_STR("lexer", error_category_to_string(ERROR_CATEGORY_LEXER));
    ASSERT_EQ_STR("parser", error_category_to_string(ERROR_CATEGORY_PARSER));
    ASSERT_EQ_STR("type", error_category_to_string(ERROR_CATEGORY_TYPE));
    ASSERT_EQ_STR("codegen", error_category_to_string(ERROR_CATEGORY_CODEGEN));
    
    return TEST_PASS;
}

TEST(error, error_builder) {
    ErrorContext* ctx = error_context_new();
    SourceLocation loc = make_source_location("builder_test.goo", 25, 8, 300, 12);
    
    ErrorBuilder* builder = error_builder_new(ctx, ERROR_TYPE_MISMATCH);
    ASSERT_NOT_NULL(builder);
    
    error_builder_with_message(builder, "Expected type %s, got %s", "int", "string");
    error_builder_with_hint(builder, "Consider using a type cast");
    error_builder_at_location(builder, loc);
    error_builder_emit(builder); // This frees the builder
    
    ASSERT_EQ(1, get_error_count(ctx));
    
    error_context_free(ctx);
    return TEST_PASS;
}

// Test fixture-based test
TEST_F(ErrorContextFixture, fixture_test) {
    ErrorContextFixture* fixture = GET_FIXTURE(ErrorContextFixture);
    
    ASSERT_NOT_NULL(fixture->ctx);
    ASSERT_EQ(0, get_error_count(fixture->ctx));
    
    SourceLocation loc = make_source_location("fixture_test.goo", 1, 1, 0, 10);
    report_error(fixture->ctx, ERROR_INTERNAL, "Fixture test error", loc);
    
    ASSERT_EQ(1, get_error_count(fixture->ctx));
    
    return TEST_PASS;
}

// Registration function (would be called from test_main.c)
void register_error_tests(void) {
    // Tests are automatically registered via constructors
    // This function is just a placeholder for future manual registration
}