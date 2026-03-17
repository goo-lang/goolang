#include "comptime.h"
#include "ast.h"
#include "token.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } \
} while(0)
#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))
#define EXPECT_EQ(a, b) do { \
    if ((a) != (b)) { printf("  FAIL: %s != %s (line %d)\n", #a, #b, __LINE__); return 1; } \
} while(0)
#define EXPECT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { printf("  FAIL: \"%s\" != \"%s\" (line %d)\n", (a), (b), __LINE__); return 1; } \
} while(0)

static Position pos0 = {0};

// =============================================================================
// Value Tests
// =============================================================================

static int test_value_int(void) {
    ComptimeValue v = comptime_value_int(42);
    EXPECT_EQ(v.kind, COMPTIME_INT);
    EXPECT_EQ(v.data.int_val, (int64_t)42);
    EXPECT_TRUE(comptime_value_is_truthy(&v));

    ComptimeValue zero = comptime_value_int(0);
    EXPECT_FALSE(comptime_value_is_truthy(&zero));
    return 0;
}

static int test_value_string(void) {
    ComptimeValue v = comptime_value_string("hello");
    EXPECT_EQ(v.kind, COMPTIME_STRING);
    EXPECT_STR_EQ(v.data.string_val, "hello");

    char* s = comptime_value_to_string(&v);
    EXPECT_STR_EQ(s, "hello");
    free(s);
    comptime_value_free(&v);
    return 0;
}

static int test_value_bool(void) {
    ComptimeValue t = comptime_value_bool(true);
    ComptimeValue f = comptime_value_bool(false);
    EXPECT_TRUE(comptime_value_is_truthy(&t));
    EXPECT_FALSE(comptime_value_is_truthy(&f));
    return 0;
}

// =============================================================================
// Scope Tests
// =============================================================================

static int test_scope_variable(void) {
    ComptimeInterpreter* interp = comptime_interpreter_new();

    comptime_set_variable(interp, "x", comptime_value_int(10), false);
    ComptimeValue* v = comptime_lookup_variable(interp, "x");
    EXPECT_TRUE(v != NULL);
    EXPECT_EQ(v->data.int_val, (int64_t)10);

    // Nested scope
    comptime_push_scope(interp);
    comptime_set_variable(interp, "y", comptime_value_int(20), false);

    ComptimeValue* x = comptime_lookup_variable(interp, "x"); // From parent
    ComptimeValue* y = comptime_lookup_variable(interp, "y");
    EXPECT_TRUE(x != NULL);
    EXPECT_TRUE(y != NULL);
    EXPECT_EQ(x->data.int_val, (int64_t)10);
    EXPECT_EQ(y->data.int_val, (int64_t)20);

    comptime_pop_scope(interp);

    // y should be gone
    y = comptime_lookup_variable(interp, "y");
    EXPECT_TRUE(y == NULL);

    comptime_interpreter_free(interp);
    return 0;
}

static int test_const_reassign_error(void) {
    ComptimeInterpreter* interp = comptime_interpreter_new();

    comptime_set_variable(interp, "PI", comptime_value_float(3.14), true);
    bool ok = comptime_set_variable(interp, "PI", comptime_value_float(3.0), false);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(comptime_has_error(interp));

    comptime_interpreter_free(interp);
    return 0;
}

// =============================================================================
// Expression Evaluation Tests
// =============================================================================

static int test_eval_literal_int(void) {
    ComptimeInterpreter* interp = comptime_interpreter_new();

    LiteralNode* lit = ast_literal_new(TOKEN_INT, "42", pos0);
    ComptimeValue result = comptime_eval_expression(interp, (ASTNode*)lit);
    EXPECT_EQ(result.kind, COMPTIME_INT);
    EXPECT_EQ(result.data.int_val, (int64_t)42);

    ast_node_free((ASTNode*)lit);
    comptime_interpreter_free(interp);
    return 0;
}

static int test_eval_literal_string(void) {
    ComptimeInterpreter* interp = comptime_interpreter_new();

    LiteralNode* lit = ast_literal_new(TOKEN_STRING, "hello", pos0);
    ComptimeValue result = comptime_eval_expression(interp, (ASTNode*)lit);
    EXPECT_EQ(result.kind, COMPTIME_STRING);
    EXPECT_STR_EQ(result.data.string_val, "hello");

    comptime_value_free(&result);
    ast_node_free((ASTNode*)lit);
    comptime_interpreter_free(interp);
    return 0;
}

static int test_eval_binary_add(void) {
    ComptimeInterpreter* interp = comptime_interpreter_new();

    LiteralNode* left = ast_literal_new(TOKEN_INT, "10", pos0);
    LiteralNode* right = ast_literal_new(TOKEN_INT, "32", pos0);
    BinaryExprNode* bin = ast_binary_expr_new((ASTNode*)left, TOKEN_PLUS, (ASTNode*)right, pos0);

    ComptimeValue result = comptime_eval_expression(interp, (ASTNode*)bin);
    EXPECT_EQ(result.kind, COMPTIME_INT);
    EXPECT_EQ(result.data.int_val, (int64_t)42);

    ast_node_free((ASTNode*)bin);
    comptime_interpreter_free(interp);
    return 0;
}

static int test_eval_binary_multiply(void) {
    ComptimeInterpreter* interp = comptime_interpreter_new();

    LiteralNode* left = ast_literal_new(TOKEN_INT, "6", pos0);
    LiteralNode* right = ast_literal_new(TOKEN_INT, "7", pos0);
    BinaryExprNode* bin = ast_binary_expr_new((ASTNode*)left, TOKEN_MULTIPLY, (ASTNode*)right, pos0);

    ComptimeValue result = comptime_eval_expression(interp, (ASTNode*)bin);
    EXPECT_EQ(result.kind, COMPTIME_INT);
    EXPECT_EQ(result.data.int_val, (int64_t)42);

    ast_node_free((ASTNode*)bin);
    comptime_interpreter_free(interp);
    return 0;
}

static int test_eval_comparison(void) {
    ComptimeInterpreter* interp = comptime_interpreter_new();

    LiteralNode* left = ast_literal_new(TOKEN_INT, "5", pos0);
    LiteralNode* right = ast_literal_new(TOKEN_INT, "10", pos0);
    BinaryExprNode* bin = ast_binary_expr_new((ASTNode*)left, TOKEN_LT, (ASTNode*)right, pos0);

    ComptimeValue result = comptime_eval_expression(interp, (ASTNode*)bin);
    EXPECT_EQ(result.kind, COMPTIME_BOOL);
    EXPECT_TRUE(result.data.bool_val);

    ast_node_free((ASTNode*)bin);
    comptime_interpreter_free(interp);
    return 0;
}

static int test_eval_string_concat(void) {
    ComptimeInterpreter* interp = comptime_interpreter_new();

    LiteralNode* left = ast_literal_new(TOKEN_STRING, "hello ", pos0);
    LiteralNode* right = ast_literal_new(TOKEN_STRING, "world", pos0);
    BinaryExprNode* bin = ast_binary_expr_new((ASTNode*)left, TOKEN_PLUS, (ASTNode*)right, pos0);

    ComptimeValue result = comptime_eval_expression(interp, (ASTNode*)bin);
    EXPECT_EQ(result.kind, COMPTIME_STRING);
    EXPECT_STR_EQ(result.data.string_val, "hello world");

    comptime_value_free(&result);
    ast_node_free((ASTNode*)bin);
    comptime_interpreter_free(interp);
    return 0;
}

static int test_eval_division_by_zero(void) {
    ComptimeInterpreter* interp = comptime_interpreter_new();

    LiteralNode* left = ast_literal_new(TOKEN_INT, "10", pos0);
    LiteralNode* right = ast_literal_new(TOKEN_INT, "0", pos0);
    BinaryExprNode* bin = ast_binary_expr_new((ASTNode*)left, TOKEN_DIVIDE, (ASTNode*)right, pos0);

    comptime_eval_expression(interp, (ASTNode*)bin);
    EXPECT_TRUE(comptime_has_error(interp));

    ast_node_free((ASTNode*)bin);
    comptime_interpreter_free(interp);
    return 0;
}

static int test_eval_variable_lookup(void) {
    ComptimeInterpreter* interp = comptime_interpreter_new();

    comptime_set_variable(interp, "x", comptime_value_int(100), false);

    IdentifierNode* ident = ast_identifier_new("x", pos0);
    ComptimeValue result = comptime_eval_expression(interp, (ASTNode*)ident);
    EXPECT_EQ(result.kind, COMPTIME_INT);
    EXPECT_EQ(result.data.int_val, (int64_t)100);

    ast_node_free((ASTNode*)ident);
    comptime_interpreter_free(interp);
    return 0;
}

static int test_eval_undefined_variable(void) {
    ComptimeInterpreter* interp = comptime_interpreter_new();

    IdentifierNode* ident = ast_identifier_new("undefined_var", pos0);
    comptime_eval_expression(interp, (ASTNode*)ident);
    EXPECT_TRUE(comptime_has_error(interp));

    ast_node_free((ASTNode*)ident);
    comptime_interpreter_free(interp);
    return 0;
}

static int test_eval_unary_negate(void) {
    ComptimeInterpreter* interp = comptime_interpreter_new();

    LiteralNode* operand = ast_literal_new(TOKEN_INT, "42", pos0);
    UnaryExprNode* unary = ast_unary_expr_new(TOKEN_MINUS, (ASTNode*)operand, pos0);

    ComptimeValue result = comptime_eval_expression(interp, (ASTNode*)unary);
    EXPECT_EQ(result.kind, COMPTIME_INT);
    EXPECT_EQ(result.data.int_val, (int64_t)-42);

    ast_node_free((ASTNode*)unary);
    comptime_interpreter_free(interp);
    return 0;
}

// =============================================================================
// Test Runner
// =============================================================================

typedef struct { const char* name; int (*func)(void); } TestCase;

int main(void) {
    TestCase tests[] = {
        {"value_int",             test_value_int},
        {"value_string",          test_value_string},
        {"value_bool",            test_value_bool},
        {"scope_variable",        test_scope_variable},
        {"const_reassign_error",  test_const_reassign_error},
        {"eval_literal_int",      test_eval_literal_int},
        {"eval_literal_string",   test_eval_literal_string},
        {"eval_binary_add",       test_eval_binary_add},
        {"eval_binary_multiply",  test_eval_binary_multiply},
        {"eval_comparison",       test_eval_comparison},
        {"eval_string_concat",    test_eval_string_concat},
        {"eval_division_by_zero", test_eval_division_by_zero},
        {"eval_variable_lookup",  test_eval_variable_lookup},
        {"eval_undefined_variable", test_eval_undefined_variable},
        {"eval_unary_negate",     test_eval_unary_negate},
    };

    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;

    printf("Running %zu comptime tests...\n\n", total);

    for (size_t i = 0; i < total; i++) {
        printf("[ RUN      ] comptime.%s\n", tests[i].name);
        int result = tests[i].func();
        if (result == 0) {
            printf("[\033[32m       OK\033[0m ] comptime.%s\n", tests[i].name);
            passed++;
        } else {
            printf("[\033[31m  FAILED  \033[0m ] comptime.%s\n", tests[i].name);
        }
    }

    printf("\nTests run: %zu\n\033[32mPassed: %zu\033[0m\n", total, passed);
    if (passed < total) printf("\033[31mFailed: %zu\033[0m\n", total - passed);

    return passed < total ? 1 : 0;
}
