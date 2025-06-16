#include "../include/macro_hygiene.h"
#include "../include/macro_safety.h"
#include "../include/advanced_macro_system.h"
#include "../include/comptime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Test helper functions
void test_assert(bool condition, const char* test_name) {
    if (condition) {
        printf("✓ %s\n", test_name);
    } else {
        printf("✗ %s\n", test_name);
        exit(1);
    }
}

// Test hygiene context creation
void test_hygiene_context_creation() {
    MacroHygieneContext* ctx = create_hygiene_context();
    
    test_assert(ctx != NULL, "Hygiene context creation");
    test_assert(ctx->global_scope != NULL, "Global scope created");
    test_assert(ctx->current_scope == ctx->global_scope, "Current scope is global");
    test_assert(ctx->expansion_depth == 0, "Initial expansion depth is zero");
    test_assert(ctx->reserved_count > 0, "Built-in reserved names loaded");
    test_assert(ctx->macro_prefix != NULL, "Macro prefix initialized");
    
    destroy_hygiene_context(ctx);
}

// Test hygiene scope management
void test_hygiene_scope_management() {
    MacroHygieneContext* ctx = create_hygiene_context();
    
    // Enter function scope
    HygieneScope* func_scope = enter_hygiene_scope(ctx, HYGIENE_SCOPE_FUNCTION, "test_func");
    test_assert(func_scope != NULL, "Function scope creation");
    test_assert(ctx->current_scope == func_scope, "Current scope updated");
    test_assert(ctx->expansion_depth == 1, "Expansion depth incremented");
    
    // Enter block scope
    HygieneScope* block_scope = enter_hygiene_scope(ctx, HYGIENE_SCOPE_BLOCK, "test_block");
    test_assert(block_scope != NULL, "Block scope creation");
    test_assert(ctx->expansion_depth == 2, "Expansion depth incremented again");
    
    // Exit block scope
    HygieneScope* parent = exit_hygiene_scope(ctx);
    test_assert(parent == func_scope, "Exit returns parent scope");
    test_assert(ctx->expansion_depth == 1, "Expansion depth decremented");
    
    // Exit function scope
    parent = exit_hygiene_scope(ctx);
    test_assert(parent == ctx->global_scope, "Exit returns global scope");
    test_assert(ctx->expansion_depth == 0, "Expansion depth back to zero");
    
    destroy_hygiene_context(ctx);
}

// Test hygiene binding management
void test_hygiene_binding_management() {
    MacroHygieneContext* ctx = create_hygiene_context();
    HygieneScope* scope = enter_hygiene_scope(ctx, HYGIENE_SCOPE_FUNCTION, "test");
    
    // Create and add binding
    HygieneBinding* binding = create_hygiene_binding("test_var", NULL, HYGIENE_SCOPE_FUNCTION);
    test_assert(binding != NULL, "Hygiene binding creation");
    test_assert(strcmp(binding->name, "test_var") == 0, "Binding name set correctly");
    
    bool added = add_hygiene_binding(scope, binding);
    test_assert(added, "Binding added to scope");
    test_assert(binding->generation == 0, "Generation number assigned");
    
    // Find binding
    HygieneBinding* found = find_hygiene_binding(scope, "test_var");
    test_assert(found == binding, "Binding found in scope");
    
    // Resolve binding from context
    HygieneBinding* resolved = resolve_hygiene_binding(ctx, "test_var");
    test_assert(resolved == binding, "Binding resolved from context");
    
    // Try to add duplicate binding
    HygieneBinding* duplicate = create_hygiene_binding("test_var", NULL, HYGIENE_SCOPE_FUNCTION);
    bool added_duplicate = add_hygiene_binding(scope, duplicate);
    test_assert(!added_duplicate, "Duplicate binding rejected");
    destroy_hygiene_binding(duplicate);
    
    exit_hygiene_scope(ctx);
    destroy_hygiene_context(ctx);
}

// Test hygienic name generation
void test_hygienic_name_generation() {
    MacroHygieneContext* ctx = create_hygiene_context();
    
    // Generate hygienic names
    char* name1 = generate_hygienic_name(ctx, "test");
    char* name2 = generate_hygienic_name(ctx, "test");
    
    test_assert(name1 != NULL, "Hygienic name generated");
    test_assert(name2 != NULL, "Second hygienic name generated");
    test_assert(strcmp(name1, name2) != 0, "Hygienic names are unique");
    test_assert(strstr(name1, "__macro_") != NULL, "Hygienic name has macro prefix");
    test_assert(strstr(name1, "test") != NULL, "Hygienic name contains base name");
    
    // Test name mangling
    char* mangled = mangle_variable_name("var", 5, "test_macro");
    test_assert(mangled != NULL, "Variable name mangled");
    test_assert(strstr(mangled, "__hygiene_") != NULL, "Mangled name has hygiene prefix");
    test_assert(strstr(mangled, "var") != NULL, "Mangled name contains variable name");
    test_assert(strstr(mangled, "test_macro") != NULL, "Mangled name contains macro name");
    
    // Test unique identifier generation
    char* id1 = generate_unique_identifier(ctx, "temp");
    char* id2 = generate_unique_identifier(ctx, "temp");
    test_assert(id1 != NULL, "Unique identifier generated");
    test_assert(id2 != NULL, "Second unique identifier generated");
    test_assert(strcmp(id1, id2) != 0, "Unique identifiers are different");
    
    // Test hygienic name detection
    test_assert(is_hygienic_name(name1), "Hygienic name detected correctly");
    test_assert(is_hygienic_name(mangled), "Mangled name detected as hygienic");
    test_assert(!is_hygienic_name("normal_name"), "Normal name not detected as hygienic");
    
    free(name1);
    free(name2);
    free(mangled);
    free(id1);
    free(id2);
    destroy_hygiene_context(ctx);
}

// Test reserved name management
void test_reserved_name_management() {
    MacroHygieneContext* ctx = create_hygiene_context();
    
    // Test built-in reserved names
    test_assert(is_reserved_name(ctx, "if"), "Built-in keyword 'if' is reserved");
    test_assert(is_reserved_name(ctx, "while"), "Built-in keyword 'while' is reserved");
    test_assert(is_reserved_name(ctx, "comptime"), "Goo keyword 'comptime' is reserved");
    test_assert(!is_reserved_name(ctx, "my_variable"), "User variable not reserved");
    
    // Add custom reserved name
    bool added = add_reserved_name(ctx, "custom_reserved");
    test_assert(added, "Custom reserved name added");
    test_assert(is_reserved_name(ctx, "custom_reserved"), "Custom reserved name detected");
    
    destroy_hygiene_context(ctx);
}

// Test safety context creation
void test_safety_context_creation() {
    MacroSafetyContext* ctx = create_safety_context();
    
    test_assert(ctx != NULL, "Safety context creation");
    test_assert(ctx->result_count == 0, "Initial result count is zero");
    test_assert(ctx->max_recursion_depth == 100, "Default recursion depth set");
    test_assert(ctx->strict_mode == true, "Strict mode enabled by default");
    test_assert(ctx->enable_optimization == true, "Optimization enabled by default");
    
    destroy_safety_context(ctx);
}

// Test safety result creation
void test_safety_result_creation() {
    SafetyCheckResult* result = create_safety_result(
        SAFETY_CHECK_TYPE, true, SAFETY_WARNING,
        "Test message", 10, 5
    );
    
    test_assert(result != NULL, "Safety result creation");
    test_assert(result->type == SAFETY_CHECK_TYPE, "Safety check type set");
    test_assert(result->passed == true, "Safety check passed flag set");
    test_assert(result->severity == SAFETY_WARNING, "Safety severity set");
    test_assert(strcmp(result->message, "Test message") == 0, "Safety message set");
    test_assert(result->line == 10, "Line number set");
    test_assert(result->column == 5, "Column number set");
    
    destroy_safety_result(result);
    free(result);
}

// Test macro safety checking
void test_macro_safety_checking() {
    MacroSafetyContext* ctx = create_safety_context();
    
    // Create a mock macro expansion
    MacroExpansion* expansion = (MacroExpansion*)calloc(1, sizeof(MacroExpansion));
    expansion->success = true;
    expansion->expanded_code = strdup("int x = 42;");
    expansion->expanded_ast = NULL; // Simplified test
    
    // Perform safety check
    bool safe = check_macro_safety(expansion, ctx);
    test_assert(safe, "Basic macro safety check passed");
    test_assert(ctx->result_count > 0, "Safety results generated");
    
    // Check that recursion check was performed
    bool recursion_checked = false;
    for (size_t i = 0; i < ctx->result_count; i++) {
        if (ctx->results[i].type == SAFETY_CHECK_RECURSION) {
            recursion_checked = true;
            break;
        }
    }
    test_assert(recursion_checked, "Recursion safety check performed");
    
    free(expansion->expanded_code);
    free(expansion);
    destroy_safety_context(ctx);
}

// Test memory safety tracking
void test_memory_safety_tracking() {
    MemorySafetyTracker* tracker = create_memory_tracker();
    
    test_assert(tracker != NULL, "Memory tracker creation");
    test_assert(tracker->live_count == 0, "Initial live count is zero");
    test_assert(tracker->freed_count == 0, "Initial freed count is zero");
    test_assert(tracker->track_leaks == true, "Leak tracking enabled");
    
    // Simulate allocation
    void* ptr1 = malloc(100);
    void* ptr2 = malloc(200);
    
    bool tracked1 = track_allocation(tracker, ptr1);
    bool tracked2 = track_allocation(tracker, ptr2);
    test_assert(tracked1, "First allocation tracked");
    test_assert(tracked2, "Second allocation tracked");
    test_assert(tracker->live_count == 2, "Live count updated");
    
    // Simulate deallocation
    bool freed1 = track_deallocation(tracker, ptr1);
    test_assert(freed1, "Deallocation tracked");
    test_assert(tracker->live_count == 1, "Live count decremented");
    test_assert(tracker->freed_count == 1, "Freed count incremented");
    
    // Test double free detection
    bool double_free = detect_double_free(tracker, ptr1);
    test_assert(double_free, "Double free detected");
    
    // Test memory leak detection
    bool has_leaks = detect_memory_leaks(tracker);
    test_assert(has_leaks, "Memory leak detected");
    
    // Clean up
    track_deallocation(tracker, ptr2);
    free(ptr1);
    free(ptr2);
    destroy_memory_tracker(tracker);
}

// Test optimization settings
void test_optimization_settings() {
    OptimizationSettings* settings = create_optimization_settings();
    
    test_assert(settings != NULL, "Optimization settings creation");
    test_assert(settings->enable_inlining == true, "Inlining enabled by default");
    test_assert(settings->enable_dce == true, "Dead code elimination enabled");
    test_assert(settings->enable_const_fold == true, "Constant folding enabled");
    test_assert(settings->optimization_level == 2, "Default optimization level is O2");
    
    // Test optimization level setting
    set_optimization_level(settings, 0);
    test_assert(settings->optimization_level == 0, "Optimization level set to O0");
    test_assert(settings->enable_inlining == false, "Inlining disabled at O0");
    test_assert(settings->enable_dce == false, "DCE disabled at O0");
    
    set_optimization_level(settings, 3);
    test_assert(settings->optimization_level == 3, "Optimization level set to O3");
    test_assert(settings->enable_loop_unroll == true, "Loop unrolling enabled at O3");
    
    destroy_optimization_settings(settings);
}

// Test type safety checking
void test_type_safety_checking() {
    TypeSafetyChecker checker = {0};
    checker.allow_coercion = true;
    checker.check_const = true;
    checker.check_nullability = true;
    
    // Mock types for testing
    Type* int_type = type_new(TYPE_INT64);
    Type* float_type = type_new(TYPE_FLOAT64);
    
    // Test compatible types with coercion
    bool compatible = verify_type_compatibility(int_type, float_type, &checker);
    test_assert(compatible, "Type compatibility with coercion");
    
    // Test without coercion
    checker.allow_coercion = false;
    compatible = verify_type_compatibility(int_type, float_type, &checker);
    test_assert(!compatible, "Type incompatibility without coercion");
    
    // Test exact match
    compatible = verify_type_compatibility(int_type, int_type, &checker);
    test_assert(compatible, "Type exact match");
}

// Test macro parameter validation
void test_macro_parameter_validation() {
    // Create mock macro template
    MacroTemplate* macro = (MacroTemplate*)calloc(1, sizeof(MacroTemplate));
    macro->name = strdup("test_macro");
    macro->type = MACRO_FUNCTION;
    macro->param_count = 2;
    
    // Create mock arguments
    ComptimeValue* arg1 = comptime_value_new(COMPTIME_VALUE_INT);
    arg1->int_value = 42;
    ComptimeValue* arg2 = comptime_value_new(COMPTIME_VALUE_STRING);
    arg2->string_value = strdup("test");
    ComptimeValue* args[] = {arg1, arg2};
    
    // Test valid parameters
    bool valid = validate_macro_parameters(macro, args, 2);
    test_assert(valid, "Valid macro parameters accepted");
    
    // Test invalid parameter count
    valid = validate_macro_parameters(macro, args, 1);
    test_assert(!valid, "Invalid parameter count rejected");
    
    // Clean up
    comptime_value_free(arg1);
    comptime_value_free(arg2);
    free(macro->name);
    free(macro);
}

// Test performance monitoring
void test_performance_monitoring() {
    MacroSafetyContext* ctx = create_safety_context();
    
    // Test performance monitoring
    start_performance_monitoring(ctx);
    
    // Simulate some work
    for (int i = 0; i < 1000; i++) {
        volatile int x = i * i;
        (void)x; // Prevent optimization
    }
    
    stop_performance_monitoring(ctx);
    
    test_assert(ctx->expansion_time_ns > 0, "Performance monitoring recorded time");
    
    destroy_safety_context(ctx);
}

// Test hygiene violation reporting
void test_hygiene_violation_reporting() {
    MacroHygieneContext* ctx = create_hygiene_context();
    
    // Report some violations
    report_hygiene_violation(ctx, HYGIENE_VIOLATION_CAPTURE,
                           "captured_var", "test_macro", 10, 5,
                           "Variable capture detected");
    
    report_hygiene_violation(ctx, HYGIENE_VIOLATION_SHADOW,
                           "shadowed_var", "test_macro", 15, 8,
                           "Variable shadowing detected");
    
    test_assert(ctx->violation_count == 2, "Hygiene violations recorded");
    
    // Test violation detection
    HygieneViolation* violation = detect_hygiene_violation("if", ctx);
    test_assert(violation != NULL, "Hygiene violation detected for reserved name");
    test_assert(violation->type == HYGIENE_VIOLATION_RESERVED, "Correct violation type");
    
    free(violation->variable_name);
    free(violation->macro_name);
    free(violation->description);
    free(violation);
    
    destroy_hygiene_context(ctx);
}

// Test safety report generation
void test_safety_report_generation() {
    MacroSafetyContext* ctx = create_safety_context();
    
    // Add some test results
    SafetyCheckResult* result1 = create_safety_result(
        SAFETY_CHECK_TYPE, true, SAFETY_WARNING, "Type check passed", 0, 0
    );
    SafetyCheckResult* result2 = create_safety_result(
        SAFETY_CHECK_MEMORY, false, SAFETY_ERROR, "Memory issue detected", 5, 10
    );
    
    add_safety_result(ctx, result1);
    add_safety_result(ctx, result2);
    
    test_assert(ctx->result_count == 2, "Safety results added");
    
    // Generate summary
    char* summary = generate_safety_summary(ctx);
    test_assert(summary != NULL, "Safety summary generated");
    test_assert(strstr(summary, "2 checks performed") != NULL, "Check count in summary");
    test_assert(strstr(summary, "1 passed") != NULL, "Passed count in summary");
    test_assert(strstr(summary, "1 failed") != NULL, "Failed count in summary");
    
    free(result1);
    free(result2);
    free(summary);
    destroy_safety_context(ctx);
}

// Test integrated hygiene and safety
void test_integrated_hygiene_safety() {
    MacroHygieneContext* hygiene_ctx = create_hygiene_context();
    MacroSafetyContext* safety_ctx = create_safety_context();
    OptimizationSettings* opt_settings = create_optimization_settings();
    
    // Create mock macro and context
    MacroTemplate* macro = (MacroTemplate*)calloc(1, sizeof(MacroTemplate));
    macro->name = strdup("test_macro");
    macro->type = MACRO_FUNCTION;
    // Note: hygiene_level would be set in actual implementation
    
    MacroContext* ctx = (MacroContext*)calloc(1, sizeof(MacroContext));
    ctx->macro = macro;
    ctx->arg_count = 0;
    
    // Test integrated expansion (this would normally call the actual expand_macro)
    // For testing, we'll create a mock expansion
    MacroExpansion* expansion = (MacroExpansion*)calloc(1, sizeof(MacroExpansion));
    expansion->success = true;
    expansion->expanded_code = strdup("int hygiene_test = 42;");
    
    // Test safety checking
    bool safe = check_macro_safety(expansion, safety_ctx);
    test_assert(safe, "Integrated safety check passed");
    
    // Test optimization
    bool optimized = optimize_macro_expansion(expansion, opt_settings);
    test_assert(optimized, "Macro expansion optimized");
    
    // Clean up
    free(expansion->expanded_code);
    free(expansion);
    free(ctx);
    free(macro->name);
    free(macro);
    destroy_optimization_settings(opt_settings);
    destroy_safety_context(safety_ctx);
    destroy_hygiene_context(hygiene_ctx);
}

// Test utility functions
void test_utility_functions() {
    // Test safety check type strings
    test_assert(strcmp(safety_check_type_string(SAFETY_CHECK_TYPE), "Type Safety") == 0,
               "Safety check type string");
    test_assert(strcmp(safety_check_type_string(SAFETY_CHECK_MEMORY), "Memory Safety") == 0,
               "Memory safety check type string");
    
    // Test safety severity strings
    test_assert(strcmp(safety_severity_string(SAFETY_WARNING), "Warning") == 0,
               "Safety warning string");
    test_assert(strcmp(safety_severity_string(SAFETY_ERROR), "Error") == 0,
               "Safety error string");
    test_assert(strcmp(safety_severity_string(SAFETY_CRITICAL), "Critical") == 0,
               "Safety critical string");
}

int main() {
    printf("Running Macro Hygiene and Safety Tests...\n\n");
    
    test_hygiene_context_creation();
    test_hygiene_scope_management();
    test_hygiene_binding_management();
    test_hygienic_name_generation();
    test_reserved_name_management();
    test_safety_context_creation();
    test_safety_result_creation();
    test_macro_safety_checking();
    test_memory_safety_tracking();
    test_optimization_settings();
    test_type_safety_checking();
    test_macro_parameter_validation();
    test_performance_monitoring();
    test_hygiene_violation_reporting();
    test_safety_report_generation();
    test_integrated_hygiene_safety();
    test_utility_functions();
    
    printf("\n✓ All macro hygiene and safety tests passed!\n");
    return 0;
}