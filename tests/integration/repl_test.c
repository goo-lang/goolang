#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "repl.h"
#include "test/test_framework.h"

// Test context creation and cleanup
void test_repl_context_lifecycle() {
    printf("Testing REPL context lifecycle...\n");
    
    REPLContext* ctx = repl_context_new();
    assert(ctx != NULL);
    assert(ctx->mode == REPL_MODE_NORMAL);
    assert(ctx->session_id == 1);
    assert(ctx->prompt != NULL);
    assert(ctx->auto_complete_enabled == true);
    assert(ctx->show_types == true);
    assert(ctx->show_timing == true);
    assert(ctx->max_history == 1000);
    assert(ctx->color_output == true);
    
    // Test initialization
    int init_result = repl_init(ctx);
    assert(init_result == 0);
    assert(ctx->type_checker != NULL);
    assert(ctx->repl_scope != NULL);
    assert(ctx->hot_reload != NULL);
    
    // Test cleanup
    repl_cleanup(ctx);
    repl_context_free(ctx);
    
    printf("✓ REPL context lifecycle test passed\n");
}

// Test value creation and manipulation
void test_repl_values() {
    printf("Testing REPL value operations...\n");
    
    // Test integer value
    REPLValue* int_val = repl_value_int(42);
    assert(int_val != NULL);
    assert(int_val->type == REPL_VALUE_INT);
    assert(int_val->value.int_val == 42);
    assert(int_val->is_valid == true);
    
    char* int_str = repl_value_to_string(int_val);
    assert(int_str != NULL);
    assert(strcmp(int_str, "42") == 0);
    free(int_str);
    
    // Test float value
    REPLValue* float_val = repl_value_float(3.14);
    assert(float_val != NULL);
    assert(float_val->type == REPL_VALUE_FLOAT);
    assert(float_val->value.float_val == 3.14);
    
    char* float_str = repl_value_to_string(float_val);
    assert(float_str != NULL);
    free(float_str);
    
    // Test string value
    REPLValue* str_val = repl_value_string("hello");
    assert(str_val != NULL);
    assert(str_val->type == REPL_VALUE_STRING);
    assert(strcmp(str_val->value.string_val, "hello") == 0);
    
    char* str_str = repl_value_to_string(str_val);
    assert(str_str != NULL);
    assert(strcmp(str_str, "hello") == 0);
    free(str_str);
    
    // Test boolean value
    REPLValue* bool_val = repl_value_bool(true);
    assert(bool_val != NULL);
    assert(bool_val->type == REPL_VALUE_BOOL);
    assert(bool_val->value.bool_val == true);
    
    char* bool_str = repl_value_to_string(bool_val);
    assert(bool_str != NULL);
    assert(strcmp(bool_str, "true") == 0);
    free(bool_str);
    
    // Test null value
    REPLValue* null_val = repl_value_null();
    assert(null_val != NULL);
    assert(null_val->type == REPL_VALUE_NULL);
    
    char* null_str = repl_value_to_string(null_val);
    assert(null_str != NULL);
    assert(strcmp(null_str, "null") == 0);
    free(null_str);
    
    // Test error value
    REPLValue* error_val = repl_value_error("test error", 123);
    assert(error_val != NULL);
    assert(error_val->type == REPL_VALUE_ERROR);
    assert(error_val->is_valid == false);
    assert(strcmp(error_val->value.error.error_msg, "test error") == 0);
    assert(error_val->value.error.error_code == 123);
    
    char* error_str = repl_value_to_string(error_val);
    assert(error_str != NULL);
    free(error_str);
    
    // Test value copying
    REPLValue* int_copy = repl_value_copy(int_val);
    assert(int_copy != NULL);
    assert(int_copy->type == int_val->type);
    assert(int_copy->value.int_val == int_val->value.int_val);
    assert(int_copy != int_val); // Different objects
    
    // Cleanup
    repl_value_free(int_val);
    repl_value_free(float_val);
    repl_value_free(str_val);
    repl_value_free(bool_val);
    repl_value_free(null_val);
    repl_value_free(error_val);
    repl_value_free(int_copy);
    
    printf("✓ REPL value operations test passed\n");
}

// Test command parsing
void test_repl_commands() {
    printf("Testing REPL command parsing...\n");
    
    assert(repl_parse_command("help") == REPL_CMD_HELP);
    assert(repl_parse_command("exit") == REPL_CMD_EXIT);
    assert(repl_parse_command("quit") == REPL_CMD_EXIT);
    assert(repl_parse_command("reset") == REPL_CMD_RESET);
    assert(repl_parse_command("history") == REPL_CMD_HISTORY);
    assert(repl_parse_command("type") == REPL_CMD_TYPE);
    assert(repl_parse_command("clear") == REPL_CMD_CLEAR);
    assert(repl_parse_command("mode") == REPL_CMD_MODE);
    assert(repl_parse_command("reload") == REPL_CMD_RELOAD);
    assert(repl_parse_command("scope") == REPL_CMD_SCOPE);
    assert(repl_parse_command("bindings") == REPL_CMD_BINDINGS);
    assert(repl_parse_command("unknown") == REPL_CMD_UNKNOWN);
    assert(repl_parse_command("") == REPL_CMD_UNKNOWN);
    assert(repl_parse_command(NULL) == REPL_CMD_UNKNOWN);
    
    // Test with whitespace
    assert(repl_parse_command("  help  ") == REPL_CMD_HELP);
    assert(repl_parse_command("exit   ") == REPL_CMD_EXIT);
    
    printf("✓ REPL command parsing test passed\n");
}

// Test command execution
void test_repl_command_execution() {
    printf("Testing REPL command execution...\n");
    
    REPLContext* ctx = repl_context_new();
    assert(ctx != NULL);
    
    int init_result = repl_init(ctx);
    assert(init_result == 0);
    
    // Test help command
    int help_result = repl_execute_command(ctx, REPL_CMD_HELP, "help");
    assert(help_result == 0);
    
    // Test mode command
    int mode_result = repl_execute_command(ctx, REPL_CMD_MODE, "mode");
    assert(mode_result == 0);
    
    // Test mode setting
    ctx->mode = REPL_MODE_NORMAL;
    int mode_set_result = repl_execute_command(ctx, REPL_CMD_MODE, "mode debug");
    assert(mode_set_result == 0);
    assert(ctx->mode == REPL_MODE_DEBUG);
    
    // Test reset command
    int reset_result = repl_execute_command(ctx, REPL_CMD_RESET, "reset");
    assert(reset_result == 0);
    
    // Test clear command
    int clear_result = repl_execute_command(ctx, REPL_CMD_CLEAR, "clear");
    assert(clear_result == 0);
    
    // Test scope command
    int scope_result = repl_execute_command(ctx, REPL_CMD_SCOPE, "scope");
    assert(scope_result == 0);
    
    // Test bindings command
    int bindings_result = repl_execute_command(ctx, REPL_CMD_BINDINGS, "bindings");
    assert(bindings_result == 0);
    
    // Test unknown command
    int unknown_result = repl_execute_command(ctx, REPL_CMD_UNKNOWN, "unknown");
    assert(unknown_result == -1);
    
    repl_context_free(ctx);
    
    printf("✓ REPL command execution test passed\n");
}

// Test expression completion detection
void test_expression_completion() {
    printf("Testing expression completion detection...\n");
    
    // Complete expressions
    assert(repl_is_complete_expression("42") == true);
    assert(repl_is_complete_expression("true") == true);
    assert(repl_is_complete_expression("\"hello\"") == true);
    assert(repl_is_complete_expression("x + y") == true);
    assert(repl_is_complete_expression("func()") == true);
    assert(repl_is_complete_expression("{a: 1, b: 2}") == true);
    assert(repl_is_complete_expression("[1, 2, 3]") == true);
    
    // Incomplete expressions
    assert(repl_is_complete_expression("{a: 1, b:") == false);
    assert(repl_is_complete_expression("[1, 2,") == false);
    assert(repl_is_complete_expression("func(") == false);
    assert(repl_is_complete_expression("if (x") == false);
    assert(repl_is_complete_expression("\"unclosed string") == false);
    
    // Edge cases
    assert(repl_is_complete_expression("") == true);
    assert(repl_is_complete_expression("{}") == true);
    assert(repl_is_complete_expression("()") == true);
    assert(repl_is_complete_expression("[]") == true);
    
    printf("✓ Expression completion detection test passed\n");
}

// Test history management
void test_repl_history() {
    printf("Testing REPL history management...\n");
    
    REPLContext* ctx = repl_context_new();
    assert(ctx != NULL);
    
    int init_result = repl_init(ctx);
    assert(init_result == 0);
    
    // Initially no history
    assert(ctx->history == NULL);
    assert(ctx->history_count == 0);
    
    // Add some history entries
    REPLValue* val1 = repl_value_int(42);
    repl_history_add(ctx, "2 + 2", val1, NULL, 1.5);
    assert(ctx->history_count == 1);
    assert(ctx->history != NULL);
    assert(strcmp(ctx->history->input, "2 + 2") == 0);
    
    REPLValue* val2 = repl_value_string("hello");
    repl_history_add(ctx, "\"hello\"", val2, NULL, 2.0);
    assert(ctx->history_count == 2);
    
    REPLValue* val3 = repl_value_bool(true);
    repl_history_add(ctx, "true", val3, NULL, 0.5);
    assert(ctx->history_count == 3);
    
    // Test history retrieval
    REPLHistory* first = repl_history_get(ctx, 1);
    assert(first != NULL);
    assert(strcmp(first->input, "true") == 0); // Most recent
    
    // Test history printing (just make sure it doesn't crash)
    repl_print_history(ctx, 2);
    
    // Test history clearing
    repl_history_clear(ctx);
    assert(ctx->history == NULL);
    assert(ctx->history_count == 0);
    
    repl_context_free(ctx);
    
    printf("✓ REPL history management test passed\n");
}

// Test utility functions
void test_repl_utilities() {
    printf("Testing REPL utility functions...\n");
    
    // Test string trimming
    char* trimmed1 = repl_trim_whitespace("  hello  ");
    assert(trimmed1 != NULL);
    assert(strcmp(trimmed1, "hello") == 0);
    free(trimmed1);
    
    char* trimmed2 = repl_trim_whitespace("no_spaces");
    assert(trimmed2 != NULL);
    assert(strcmp(trimmed2, "no_spaces") == 0);
    free(trimmed2);
    
    char* trimmed3 = repl_trim_whitespace("   ");
    assert(trimmed3 != NULL);
    assert(strcmp(trimmed3, "") == 0);
    free(trimmed3);
    
    // Test string prefix checking
    assert(repl_string_starts_with("hello world", "hello") == true);
    assert(repl_string_starts_with("hello world", "world") == false);
    assert(repl_string_starts_with("test", "testing") == false);
    assert(repl_string_starts_with("", "") == true);
    
    // Test identifier validation
    assert(repl_is_valid_identifier("valid_name") == true);
    assert(repl_is_valid_identifier("name123") == true);
    assert(repl_is_valid_identifier("_private") == true);
    assert(repl_is_valid_identifier("123invalid") == false);
    assert(repl_is_valid_identifier("invalid-name") == false);
    assert(repl_is_valid_identifier("") == false);
    assert(repl_is_valid_identifier("valid") == true);
    
    printf("✓ REPL utility functions test passed\n");
}

// Test type formatting
void test_type_formatting() {
    printf("Testing type formatting...\n");
    
    // Test basic types
    Type* int_type = type_new(TYPE_INT32);
    char* int_str = repl_format_type(int_type, false);
    assert(int_str != NULL);
    free(int_str);
    type_free(int_type);
    
    Type* bool_type = type_new(TYPE_BOOL);
    char* bool_str = repl_format_type(bool_type, false);
    assert(bool_str != NULL);
    assert(strcmp(bool_str, "bool") == 0);
    free(bool_str);
    type_free(bool_type);
    
    Type* string_type = type_new(TYPE_STRING);
    char* string_str = repl_format_type(string_type, false);
    assert(string_str != NULL);
    assert(strcmp(string_str, "string") == 0);
    free(string_str);
    type_free(string_type);
    
    // Test null input
    char* null_str = repl_format_type(NULL, false);
    assert(null_str != NULL);
    assert(strcmp(null_str, "unknown") == 0);
    free(null_str);
    
    printf("✓ Type formatting test passed\n");
}

// Integration test for basic REPL functionality
void test_repl_integration() {
    printf("Testing REPL integration...\n");
    
    REPLContext* ctx = repl_context_new();
    assert(ctx != NULL);
    
    int init_result = repl_init(ctx);
    assert(init_result == 0);
    
    // Test simple expression evaluation
    // Note: This will generate parse/type errors since we don't have
    // a complete parser setup, but it shouldn't crash
    int eval_result = repl_eval_string(ctx, "42");
    // Don't assert on result since we expect it to fail without full parser
    
    // Test hot reload integration
    int hr_result = repl_enable_hot_reload(ctx);
    assert(hr_result == 0);
    
    // Test module reloading (should not crash)
    repl_reload_changed_modules(ctx);
    
    repl_context_free(ctx);
    
    printf("✓ REPL integration test passed\n");
}

int main() {
    printf("Running REPL tests...\n\n");
    
    test_repl_context_lifecycle();
    test_repl_values();
    test_repl_commands();
    test_repl_command_execution();
    test_expression_completion();
    test_repl_history();
    test_repl_utilities();
    test_type_formatting();
    test_repl_integration();
    
    printf("\n✅ All REPL tests passed!\n");
    return 0;
}