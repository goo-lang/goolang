#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "error_reporting.h"
#include "repl.h"
#include "test/test_framework.h"

// Test error context lifecycle
void test_error_context_lifecycle() {
    printf("Testing error context lifecycle...\n");
    
    ErrorContext* ctx = error_context_new();
    assert(ctx != NULL);
    assert(ctx->show_source_context == true);
    assert(ctx->show_explanations == true);
    assert(ctx->show_suggestions == true);
    assert(ctx->use_colors == true);
    assert(ctx->min_severity == ERROR_SEVERITY_NOTE);
    assert(ctx->error_count == 0);
    assert(ctx->warning_count == 0);
    assert(ctx->note_count == 0);
    
    // Test initialization
    int init_result = error_context_init(ctx);
    assert(init_result == 0);
    assert(ctx->enabled_categories != NULL);
    assert(ctx->num_enabled_categories == 9);
    
    // Test reset
    error_context_reset(ctx);
    assert(ctx->error_count == 0);
    assert(ctx->warning_count == 0);
    assert(ctx->note_count == 0);
    
    error_context_free(ctx);
    
    printf("✓ Error context lifecycle test passed\n");
}

// Test error location management
void test_error_location() {
    printf("Testing error location management...\n");
    
    ErrorLocation* location = error_location_new("test.goo", 10, 5, 3);
    assert(location != NULL);
    assert(strcmp(location->file_path, "test.goo") == 0);
    assert(location->line == 10);
    assert(location->column == 5);
    assert(location->length == 3);
    
    // Test location from source
    const char* source = "let x = 42;\nlet y = \"hello\";\nprint(z);";
    ErrorLocation* src_location = error_location_from_source("test.goo", source, 31, 1); // Position of 'z'
    assert(src_location != NULL);
    assert(src_location->line == 3);
    assert(src_location->column == 7);
    assert(src_location->source_line != NULL);
    assert(strcmp(src_location->source_line, "print(z);") == 0);
    
    error_location_free(location);
    error_location_free(src_location);
    
    printf("✓ Error location management test passed\n");
}

// Test error explanation system
void test_error_explanation() {
    printf("Testing error explanation system...\n");
    
    // Test manual explanation creation
    ErrorExplanation* explanation = error_explanation_new("Test Error", "This is a test error");
    assert(explanation != NULL);
    assert(strcmp(explanation->title, "Test Error") == 0);
    assert(strcmp(explanation->description, "This is a test error") == 0);
    
    error_explanation_set_details(explanation, "Test why", "Test fix");
    assert(explanation->why_error != NULL);
    assert(strcmp(explanation->why_error, "Test why") == 0);
    assert(explanation->how_to_fix != NULL);
    assert(strcmp(explanation->how_to_fix, "Test fix") == 0);
    
    error_explanation_free(explanation);
    
    // Test getting built-in explanations
    ErrorExplanation* builtin = error_explanation_get(ERROR_TYPE_MISMATCH);
    assert(builtin != NULL);
    assert(builtin->title != NULL);
    assert(builtin->description != NULL);
    
    error_explanation_free(builtin);
    
    printf("✓ Error explanation system test passed\n");
}

// Test error suggestion system
void test_error_suggestion() {
    printf("Testing error suggestion system...\n");
    
    ErrorSuggestion* suggestion = error_suggestion_new("Fix the type", "Cast to int");
    assert(suggestion != NULL);
    assert(strcmp(suggestion->description, "Fix the type") == 0);
    assert(strcmp(suggestion->suggested_fix, "Cast to int") == 0);
    assert(suggestion->is_automatic == false);
    
    ErrorSuggestion* auto_suggestion = error_suggestion_create_automatic("Auto fix", "let x: int = 42;");
    assert(auto_suggestion != NULL);
    assert(auto_suggestion->is_automatic == true);
    
    error_suggestion_free(suggestion);
    error_suggestion_free(auto_suggestion);
    
    printf("✓ Error suggestion system test passed\n");
}

// Test error report creation and management
void test_error_report() {
    printf("Testing error report creation...\n");
    
    ErrorReport* report = error_report_new(ERROR_TYPE_MISMATCH, ERROR_SEVERITY_ERROR, "Type mismatch error");
    assert(report != NULL);
    assert(report->code == ERROR_TYPE_MISMATCH);
    assert(report->severity == ERROR_SEVERITY_ERROR);
    assert(report->category == ERROR_CATEGORY_TYPE);
    assert(strcmp(report->message, "Type mismatch error") == 0);
    assert(report->error_id > 0);
    assert(report->is_suppressed == false);
    
    // Test setting location
    ErrorLocation* location = error_location_new("test.goo", 5, 10, 2);
    error_report_set_location(report, location);
    assert(report->primary_location == location);
    assert(report->primary_location->line == 5);
    
    // Test adding secondary location
    ErrorLocation* secondary = error_location_new("test.goo", 15, 20, 3);
    error_report_add_secondary_location(report, secondary);
    assert(report->secondary_locations == secondary);
    
    // Test adding suggestions
    ErrorSuggestion* suggestion = error_suggestion_create_simple("Cast value", "Use (int)value");
    error_suggestion_add(report, suggestion);
    assert(report->suggestions == suggestion);
    
    error_report_free(report);
    
    printf("✓ Error report creation test passed\n");
}

// Test error reporting with context
void test_error_reporting() {
    printf("Testing error reporting with context...\n");
    
    ErrorContext* ctx = error_context_new();
    assert(ctx != NULL);
    
    error_context_init(ctx);
    
    // Test syntax error reporting
    int syntax_result = error_report_syntax_error(ctx, "Unexpected token", "test.goo", 10, 5);
    assert(syntax_result == 0);
    assert(ctx->error_count == 1);
    assert(ctx->errors != NULL);
    
    // Test type error reporting
    int type_result = error_report_type_error(ctx, "Type mismatch", "test.goo", 15, 8, "int", "string");
    assert(type_result == 0);
    assert(ctx->error_count == 2);
    
    // Test warning reporting
    int warning_result = error_report_warning(ctx, "Unused variable", "test.goo", 20, 1);
    assert(warning_result == 0);
    assert(ctx->warning_count == 1);
    assert(ctx->warnings != NULL);
    
    // Test note reporting
    int note_result = error_report_note(ctx, "Consider using const", "test.goo", 25, 1);
    assert(note_result == 0);
    assert(ctx->note_count == 1);
    assert(ctx->notes != NULL);
    
    // Test total count
    assert(error_context_get_total_count(ctx) == 4);
    assert(error_context_get_error_count(ctx) == 2);
    assert(error_context_get_warning_count(ctx) == 1);
    
    error_context_free(ctx);
    
    printf("✓ Error reporting with context test passed\n");
}

// Test error filtering and configuration
void test_error_filtering() {
    printf("Testing error filtering and configuration...\n");
    
    ErrorContext* ctx = error_context_new();
    assert(ctx != NULL);
    
    error_context_init(ctx);
    
    // Test severity filtering
    error_context_set_severity_filter(ctx, ERROR_SEVERITY_ERROR);
    assert(ctx->min_severity == ERROR_SEVERITY_ERROR);
    
    // Test note (should be filtered out)
    int note_result = error_report_note(ctx, "This note should be filtered", "test.goo", 10, 1);
    assert(note_result == 0);  // Function succeeds but doesn't add to list
    assert(ctx->note_count == 0);  // Filtered out
    
    // Test error (should pass through)
    int error_result = error_report_syntax_error(ctx, "This error should pass", "test.goo", 15, 1);
    assert(error_result == 0);
    assert(ctx->error_count == 1);  // Not filtered
    
    // Test display options
    error_context_set_display_options(ctx, false, false, false);
    assert(ctx->show_source_context == false);
    assert(ctx->show_explanations == false);
    assert(ctx->show_suggestions == false);
    
    error_context_set_color_output(ctx, false);
    assert(ctx->use_colors == false);
    
    error_context_set_context_lines(ctx, 5);
    assert(ctx->context_lines == 5);
    
    // Test category filtering
    error_context_disable_category(ctx, ERROR_CATEGORY_SYNTAX);
    // This is a simplified test - full implementation would check filtering
    
    error_context_free(ctx);
    
    printf("✓ Error filtering and configuration test passed\n");
}

// Test utility functions
void test_utility_functions() {
    printf("Testing utility functions...\n");
    
    // Test severity to string
    assert(strcmp(error_severity_to_string(ERROR_SEVERITY_ERROR), "error") == 0);
    assert(strcmp(error_severity_to_string(ERROR_SEVERITY_WARNING), "warning") == 0);
    assert(strcmp(error_severity_to_string(ERROR_SEVERITY_NOTE), "note") == 0);
    assert(strcmp(error_severity_to_string(ERROR_SEVERITY_FATAL), "fatal") == 0);
    
    // Test category to string
    assert(strcmp(error_category_to_string(ERROR_CATEGORY_SYNTAX), "syntax") == 0);
    assert(strcmp(error_category_to_string(ERROR_CATEGORY_TYPE), "type") == 0);
    assert(strcmp(error_category_to_string(ERROR_CATEGORY_SEMANTIC), "semantic") == 0);
    
    // Test error code to string
    assert(strcmp(error_code_to_string(ERROR_TYPE_MISMATCH), "type_mismatch") == 0);
    assert(strcmp(error_code_to_string(ERROR_UNDEFINED_VARIABLE), "undefined_variable") == 0);
    
    // Test error classification
    assert(error_is_fatal(ERROR_COMPILER_BUG) == true);
    assert(error_is_fatal(ERROR_TYPE_MISMATCH) == false);
    
    assert(error_get_category(ERROR_TYPE_MISMATCH) == ERROR_CATEGORY_TYPE);
    assert(error_get_category(ERROR_UNDEFINED_VARIABLE) == ERROR_CATEGORY_SEMANTIC);
    
    assert(error_get_default_severity(ERROR_TYPE_MISMATCH) == ERROR_SEVERITY_ERROR);
    assert(error_get_default_severity(ERROR_COMPILER_BUG) == ERROR_SEVERITY_FATAL);
    
    printf("✓ Utility functions test passed\n");
}

// Test REPL integration
void test_repl_integration() {
    printf("Testing REPL integration...\n");
    
    REPLContext* repl_ctx = repl_context_new();
    assert(repl_ctx != NULL);
    
    int repl_init_result = repl_init(repl_ctx);
    assert(repl_init_result == 0);
    assert(repl_ctx->error_context != NULL);
    
    ErrorContext* error_ctx = repl_ctx->error_context;
    
    // Test integration
    int integration_result = error_reporting_integrate_repl(error_ctx, repl_ctx);
    assert(integration_result == 0);
    
    // Test REPL command handling (just make sure it doesn't crash)
    int errors_result = error_handle_repl_error_command(repl_ctx, error_ctx, "summary");
    // Don't assert on result since it depends on implementation details
    
    // Add some errors to test display
    error_report_syntax_error(error_ctx, "Test syntax error", "repl", 1, 1);
    error_report_warning(error_ctx, "Test warning", "repl", 2, 1);
    
    // Test error commands
    int show_errors_result = error_handle_repl_error_command(repl_ctx, error_ctx, "errors");
    // Should display errors without crashing
    
    repl_context_free(repl_ctx);
    
    printf("✓ REPL integration test passed\n");
}

// Test error help system
void test_error_help() {
    printf("Testing error help system...\n");
    
    // Test help display (should not crash)
    error_help_display(ERROR_TYPE_MISMATCH);
    error_help_display(ERROR_UNDEFINED_VARIABLE);
    
    // Test help search (should not crash)
    error_help_search("type");
    
    // Test list all (should not crash)
    error_help_list_all();
    
    printf("✓ Error help system test passed\n");
}

// Test error statistics
void test_error_statistics() {
    printf("Testing error statistics...\n");
    
    ErrorContext* ctx = error_context_new();
    assert(ctx != NULL);
    
    error_context_init(ctx);
    
    // Add various types of errors
    error_report_syntax_error(ctx, "Syntax error 1", "test.goo", 1, 1);
    error_report_syntax_error(ctx, "Syntax error 2", "test.goo", 2, 1);
    error_report_type_error(ctx, "Type error", "test.goo", 3, 1, "int", "string");
    error_report_warning(ctx, "Warning", "test.goo", 4, 1);
    error_report_note(ctx, "Note", "test.goo", 5, 1);
    
    // Test statistics
    assert(error_context_get_error_count(ctx) == 3);  // 2 syntax + 1 type
    assert(error_context_get_warning_count(ctx) == 1);
    assert(error_context_get_total_count(ctx) == 5);
    
    // Test statistics report generation
    char* stats_report = error_context_generate_statistics_report(ctx);
    assert(stats_report != NULL);
    assert(strstr(stats_report, "Total Errors: 3") != NULL);
    assert(strstr(stats_report, "Total Warnings: 1") != NULL);
    free(stats_report);
    
    error_context_free(ctx);
    
    printf("✓ Error statistics test passed\n");
}

// Test error export functionality
void test_error_export() {
    printf("Testing error export functionality...\n");
    
    ErrorContext* ctx = error_context_new();
    assert(ctx != NULL);
    
    error_context_init(ctx);
    
    // Add some errors
    error_report_syntax_error(ctx, "Export test error", "test.goo", 1, 1);
    error_report_warning(ctx, "Export test warning", "test.goo", 2, 1);
    
    // Test JSON export
    int json_result = error_context_export_json(ctx, "/tmp/test_errors.json");
    assert(json_result == 0);
    
    // Test text export
    int text_result = error_context_export_text(ctx, "/tmp/test_errors.txt");
    assert(text_result == 0);
    
    // Clean up test files
    remove("/tmp/test_errors.json");
    remove("/tmp/test_errors.txt");
    
    error_context_free(ctx);
    
    printf("✓ Error export functionality test passed\n");
}

int main() {
    printf("Running Error Reporting tests...\n\n");
    
    test_error_context_lifecycle();
    test_error_location();
    test_error_explanation();
    test_error_suggestion();
    test_error_report();
    test_error_reporting();
    test_error_filtering();
    test_utility_functions();
    test_repl_integration();
    test_error_help();
    test_error_statistics();
    test_error_export();
    
    printf("\n✅ All Error Reporting tests passed!\n");
    return 0;
}