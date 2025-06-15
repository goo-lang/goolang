#include "test/test_framework.h"
#include "errors/error.h"

// Simple tests to verify our test framework works
TEST(framework, basic_assertions) {
    ASSERT_TRUE(true);
    ASSERT_FALSE(false);
    ASSERT_EQ(42, 42);
    ASSERT_EQ_STR("hello", "hello");
    
    int value = 10;
    ASSERT_NOT_NULL(&value);
    
    return TEST_PASS;
}

TEST(framework, string_comparison) {
    const char* str1 = "test";
    const char* str2 = "test";
    
    ASSERT_EQ_STR(str1, str2);
    
    return TEST_PASS;
}

TEST(framework, numeric_comparison) {
    ASSERT_EQ(1 + 1, 2);
    ASSERT_EQ_UINT(5U, 5U);
    
    double pi = 3.14159;
    ASSERT_NEAR(pi, 3.14, 0.01);
    
    return TEST_PASS;
}

TEST(error, basic_functionality) {
    ErrorContext* ctx = error_context_new();
    ASSERT_NOT_NULL(ctx);
    
    ASSERT_EQ(0, get_error_count(ctx));
    ASSERT_FALSE(has_errors(ctx));
    
    error_context_free(ctx);
    return TEST_PASS;
}

TEST(error, source_location_creation) {
    SourceLocation loc = make_source_location("test.goo", 10, 5, 100, 5);
    
    ASSERT_TRUE(source_location_is_valid(&loc));
    ASSERT_EQ_STR("test.goo", loc.filename);
    ASSERT_EQ(10, loc.line);
    ASSERT_EQ(5, loc.column);
    
    return TEST_PASS;
}

// Mock registration function for now
void register_error_tests(void) {
    // Tests are auto-registered via constructors
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