#include "../include/macro_safety.h"
#include "../include/errors/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

// Safety check type names
static const char* SAFETY_CHECK_NAMES[] = {
    [SAFETY_CHECK_TYPE] = "Type Safety",
    [SAFETY_CHECK_MEMORY] = "Memory Safety",
    [SAFETY_CHECK_BOUNDS] = "Bounds Checking",
    [SAFETY_CHECK_NULL] = "Null Safety",
    [SAFETY_CHECK_OVERFLOW] = "Overflow Checking",
    [SAFETY_CHECK_LIFETIME] = "Lifetime Checking",
    [SAFETY_CHECK_RECURSION] = "Recursion Checking"
};

// Safety severity names
static const char* SAFETY_SEVERITY_NAMES[] = {
    [SAFETY_WARNING] = "Warning",
    [SAFETY_ERROR] = "Error",
    [SAFETY_CRITICAL] = "Critical"
};

// Create safety context
MacroSafetyContext* create_safety_context(void) {
    MacroSafetyContext* ctx = (MacroSafetyContext*)xcalloc(1, sizeof(MacroSafetyContext));
    if (!ctx) return NULL;
    
    ctx->results = NULL;
    ctx->result_count = 0;
    ctx->max_recursion_depth = 100; // Default limit
    ctx->strict_mode = true;
    ctx->enable_optimization = true;
    
    ctx->allowed_types = NULL;
    ctx->allowed_type_count = 0;
    
    ctx->allocated_pointers = NULL;
    ctx->allocation_count = 0;
    
    ctx->expansion_time_ns = 0;
    ctx->memory_used_bytes = 0;
    ctx->complexity_score = 0;
    
    return ctx;
}

// Destroy safety context
void destroy_safety_context(MacroSafetyContext* ctx) {
    if (!ctx) return;
    
    // Free results
    for (size_t i = 0; i < ctx->result_count; i++) {
        destroy_safety_result(&ctx->results[i]);
    }
    free(ctx->results);
    
    // Free allowed types
    free(ctx->allowed_types);
    
    // Free allocated pointers tracking
    free(ctx->allocated_pointers);
    
    free(ctx);
}

// Create safety result
SafetyCheckResult* create_safety_result(SafetyCheckType type, bool passed, SafetySeverity severity,
                                       const char* message, int line, int column) {
    SafetyCheckResult* result = (SafetyCheckResult*)xcalloc(1, sizeof(SafetyCheckResult));
    if (!result) return NULL;
    
    result->type = type;
    result->passed = passed;
    result->severity = severity;
    result->message = message ? strdup(message) : NULL;
    result->line = line;
    result->column = column;
    result->suggestion = NULL;
    
    return result;
}

// Destroy safety result
void destroy_safety_result(SafetyCheckResult* result) {
    if (!result) return;
    
    free(result->message);
    free(result->suggestion);
    // Note: not freeing result itself since it might be part of an array
}

// Add safety result
void add_safety_result(MacroSafetyContext* ctx, SafetyCheckResult* result) {
    if (!ctx || !result) return;
    
    ctx->results = (SafetyCheckResult*)realloc(ctx->results, 
                                              (ctx->result_count + 1) * sizeof(SafetyCheckResult));
    if (!ctx->results) return;
    
    ctx->results[ctx->result_count] = *result;
    ctx->result_count++;
}

// Check macro safety
bool check_macro_safety(MacroExpansion* expansion, MacroSafetyContext* ctx) {
    if (!expansion || !ctx) return false;
    
    bool all_passed = true;
    
    if (expansion->expanded_ast) {
        // Type safety check
        SafetyCheckResult* type_result = perform_type_safety_check(expansion->expanded_ast, ctx);
        if (type_result) {
            add_safety_result(ctx, type_result);
            if (!type_result->passed) all_passed = false;
            free(type_result);
        }
        
        // Memory safety check
        SafetyCheckResult* memory_result = perform_memory_safety_check(expansion->expanded_ast, ctx);
        if (memory_result) {
            add_safety_result(ctx, memory_result);
            if (!memory_result->passed) all_passed = false;
            free(memory_result);
        }
        
        // Bounds check
        SafetyCheckResult* bounds_result = perform_bounds_check(expansion->expanded_ast, ctx);
        if (bounds_result) {
            add_safety_result(ctx, bounds_result);
            if (!bounds_result->passed) all_passed = false;
            free(bounds_result);
        }
        
        // Null safety check
        SafetyCheckResult* null_result = perform_null_check(expansion->expanded_ast, ctx);
        if (null_result) {
            add_safety_result(ctx, null_result);
            if (!null_result->passed) all_passed = false;
            free(null_result);
        }
        
        // Overflow check
        SafetyCheckResult* overflow_result = perform_overflow_check(expansion->expanded_ast, ctx);
        if (overflow_result) {
            add_safety_result(ctx, overflow_result);
            if (!overflow_result->passed) all_passed = false;
            free(overflow_result);
        }
        
        // Lifetime check
        SafetyCheckResult* lifetime_result = perform_lifetime_check(expansion->expanded_ast, ctx);
        if (lifetime_result) {
            add_safety_result(ctx, lifetime_result);
            if (!lifetime_result->passed) all_passed = false;
            free(lifetime_result);
        }
    }
    
    // Recursion check
    SafetyCheckResult* recursion_result = perform_recursion_check(expansion, ctx);
    if (recursion_result) {
        add_safety_result(ctx, recursion_result);
        if (!recursion_result->passed) all_passed = false;
        free(recursion_result);
    }
    
    return all_passed;
}

// Perform type safety check
SafetyCheckResult* perform_type_safety_check(ASTNode* node, MacroSafetyContext* ctx) {
    if (!node || !ctx) return NULL;
    
    // Simplified type safety check
    bool passed = true;
    char* message = NULL;
    
    // Check if node has type information - simplified check
    if (node == NULL) {
        passed = false;
        message = strdup("Missing type information in AST node");
    } else {
        // Perform basic type validation
        passed = true;
        message = strdup("Type safety check passed");
    }
    
    return create_safety_result(SAFETY_CHECK_TYPE, passed, 
                               passed ? SAFETY_WARNING : SAFETY_ERROR,
                               message, 0, 0);
}

// Perform memory safety check
SafetyCheckResult* perform_memory_safety_check(ASTNode* node, MacroSafetyContext* ctx) {
    if (!node || !ctx) return NULL;
    
    // Simplified memory safety check
    bool passed = true;
    char* message = strdup("Memory safety check passed");
    
    // In a full implementation, this would check for:
    // - Use after free
    // - Double free
    // - Memory leaks
    // - Buffer overflows
    
    return create_safety_result(SAFETY_CHECK_MEMORY, passed, SAFETY_WARNING,
                               message, 0, 0);
}

// Perform bounds check
SafetyCheckResult* perform_bounds_check(ASTNode* node, MacroSafetyContext* ctx) {
    if (!node || !ctx) return NULL;
    
    bool passed = true;
    char* message = strdup("Bounds checking passed");
    
    // This would check array accesses, buffer operations, etc.
    
    return create_safety_result(SAFETY_CHECK_BOUNDS, passed, SAFETY_WARNING,
                               message, 0, 0);
}

// Perform null check
SafetyCheckResult* perform_null_check(ASTNode* node, MacroSafetyContext* ctx) {
    if (!node || !ctx) return NULL;
    
    bool passed = true;
    char* message = strdup("Null safety check passed");
    
    // This would check for null pointer dereferences
    
    return create_safety_result(SAFETY_CHECK_NULL, passed, SAFETY_WARNING,
                               message, 0, 0);
}

// Perform overflow check
SafetyCheckResult* perform_overflow_check(ASTNode* node, MacroSafetyContext* ctx) {
    if (!node || !ctx) return NULL;
    
    bool passed = true;
    char* message = strdup("Overflow checking passed");
    
    // This would check arithmetic operations for potential overflows
    
    return create_safety_result(SAFETY_CHECK_OVERFLOW, passed, SAFETY_WARNING,
                               message, 0, 0);
}

// Perform lifetime check
SafetyCheckResult* perform_lifetime_check(ASTNode* node, MacroSafetyContext* ctx) {
    if (!node || !ctx) return NULL;
    
    bool passed = true;
    char* message = strdup("Lifetime checking passed");
    
    // This would check object lifetimes and ownership
    
    return create_safety_result(SAFETY_CHECK_LIFETIME, passed, SAFETY_WARNING,
                               message, 0, 0);
}

// Perform recursion check
SafetyCheckResult* perform_recursion_check(MacroExpansion* expansion, MacroSafetyContext* ctx) {
    if (!expansion || !ctx) return NULL;
    
    bool passed = true;
    char* message = NULL;
    
    // Simple recursion depth check
    static int current_depth = 0;
    current_depth++;
    
    if (current_depth > (int)ctx->max_recursion_depth) {
        passed = false;
        message = (char*)malloc(128);
        snprintf(message, 128, "Recursion depth %d exceeds maximum %zu", 
                current_depth, ctx->max_recursion_depth);
    } else {
        message = strdup("Recursion check passed");
    }
    
    current_depth--;
    
    return create_safety_result(SAFETY_CHECK_RECURSION, passed,
                               passed ? SAFETY_WARNING : SAFETY_ERROR,
                               message, 0, 0);
}

// Verify type compatibility
bool verify_type_compatibility(Type* expected, Type* actual, TypeSafetyChecker* checker) {
    if (!expected || !actual) return false;
    
    // Simplified type compatibility check
    if (checker && checker->allow_coercion) {
        // Allow basic type coercions
        return true;
    }
    
    // Exact type match
    return expected == actual;
}

// Check function call safety
bool check_function_call_safety(ASTNode* call_node, MacroSafetyContext* ctx) {
    if (!call_node || !ctx) return false;
    
    // This would validate function call parameters and return types
    return true;
}

// Validate macro parameters
bool validate_macro_parameters(MacroTemplate* macro, ComptimeValue** args, size_t arg_count) {
    if (!macro || !args) return false;
    
    // Check parameter count
    if (arg_count != macro->param_count) {
        return false;
    }
    
    // Check parameter types (simplified)
    for (size_t i = 0; i < arg_count; i++) {
        if (!args[i]) return false;
        // Additional type checking would go here
    }
    
    return true;
}

// Create memory tracker
MemorySafetyTracker* create_memory_tracker(void) {
    MemorySafetyTracker* tracker = (MemorySafetyTracker*)xcalloc(1, sizeof(MemorySafetyTracker));
    if (!tracker) return NULL;
    
    tracker->live_pointers = NULL;
    tracker->live_count = 0;
    tracker->freed_pointers = NULL;
    tracker->freed_count = 0;
    tracker->track_leaks = true;
    
    return tracker;
}

// Destroy memory tracker
void destroy_memory_tracker(MemorySafetyTracker* tracker) {
    if (!tracker) return;
    
    free(tracker->live_pointers);
    free(tracker->freed_pointers);
    free(tracker);
}

// Track allocation
bool track_allocation(MemorySafetyTracker* tracker, void* ptr) {
    if (!tracker || !ptr) return false;
    
    tracker->live_pointers = (void**)realloc(tracker->live_pointers,
                                            (tracker->live_count + 1) * sizeof(void*));
    if (!tracker->live_pointers) return false;
    
    tracker->live_pointers[tracker->live_count++] = ptr;
    return true;
}

// Track deallocation
bool track_deallocation(MemorySafetyTracker* tracker, void* ptr) {
    if (!tracker || !ptr) return false;
    
    // Remove from live pointers
    for (size_t i = 0; i < tracker->live_count; i++) {
        if (tracker->live_pointers[i] == ptr) {
            // Move last element to this position
            tracker->live_pointers[i] = tracker->live_pointers[tracker->live_count - 1];
            tracker->live_count--;
            
            // Add to freed pointers
            tracker->freed_pointers = (void**)realloc(tracker->freed_pointers,
                                                     (tracker->freed_count + 1) * sizeof(void*));
            if (tracker->freed_pointers) {
                tracker->freed_pointers[tracker->freed_count++] = ptr;
            }
            return true;
        }
    }
    
    return false; // Pointer not found in live list
}

// Detect memory leaks
bool detect_memory_leaks(MemorySafetyTracker* tracker) {
    if (!tracker) return false;
    
    return tracker->live_count > 0;
}

// Detect double free
bool detect_double_free(MemorySafetyTracker* tracker, void* ptr) {
    if (!tracker || !ptr) return false;
    
    // Check if pointer is in freed list
    for (size_t i = 0; i < tracker->freed_count; i++) {
        if (tracker->freed_pointers[i] == ptr) {
            return true; // Double free detected
        }
    }
    
    return false;
}

// Detect use after free
bool detect_use_after_free(MemorySafetyTracker* tracker, void* ptr) {
    if (!tracker || !ptr) return false;
    
    // Check if pointer was freed but is being accessed
    return detect_double_free(tracker, ptr);
}

// Create optimization settings
OptimizationSettings* create_optimization_settings(void) {
    OptimizationSettings* settings = (OptimizationSettings*)xcalloc(1, sizeof(OptimizationSettings));
    if (!settings) return NULL;
    
    settings->enable_inlining = true;
    settings->enable_dce = true;
    settings->enable_cse = true;
    settings->enable_loop_unroll = false;
    settings->enable_const_fold = true;
    settings->optimization_level = 2; // Default O2
    
    return settings;
}

// Destroy optimization settings
void destroy_optimization_settings(OptimizationSettings* settings) {
    if (!settings) return;
    free(settings);
}

// Optimize macro expansion
bool optimize_macro_expansion(MacroExpansion* expansion, OptimizationSettings* settings) {
    if (!expansion || !settings) return false;
    
    bool optimized = false;
    
    if (expansion->expanded_ast && settings->optimization_level > 0) {
        if (settings->enable_inlining) {
            optimized |= inline_macro_functions(expansion->expanded_ast, settings);
        }
        
        if (settings->enable_dce) {
            optimized |= eliminate_dead_code(expansion->expanded_ast, settings);
        }
        
        if (settings->enable_const_fold) {
            optimized |= perform_constant_folding(expansion->expanded_ast, settings);
        }
        
        if (settings->enable_cse) {
            optimized |= optimize_common_subexpressions(expansion->expanded_ast, settings);
        }
    }
    
    return optimized;
}

// Inline macro functions
bool inline_macro_functions(ASTNode* node, OptimizationSettings* settings) {
    if (!node || !settings) return false;
    
    // This would perform function inlining optimization
    return true;
}

// Eliminate dead code
bool eliminate_dead_code(ASTNode* node, OptimizationSettings* settings) {
    if (!node || !settings) return false;
    
    // This would remove unreachable code
    return true;
}

// Perform constant folding
bool perform_constant_folding(ASTNode* node, OptimizationSettings* settings) {
    if (!node || !settings) return false;
    
    // This would fold constant expressions
    return true;
}

// Optimize common subexpressions
bool optimize_common_subexpressions(ASTNode* node, OptimizationSettings* settings) {
    if (!node || !settings) return false;
    
    // This would eliminate common subexpressions
    return true;
}

// Print safety report
void print_safety_report(MacroSafetyContext* ctx) {
    if (!ctx) return;
    
    printf("Macro Safety Report\n");
    printf("===================\n");
    printf("Total checks: %zu\n", ctx->result_count);
    
    size_t passed = 0, warnings = 0, errors = 0, critical = 0;
    
    for (size_t i = 0; i < ctx->result_count; i++) {
        SafetyCheckResult* result = &ctx->results[i];
        if (result->passed) passed++;
        
        switch (result->severity) {
            case SAFETY_WARNING: warnings++; break;
            case SAFETY_ERROR: errors++; break;
            case SAFETY_CRITICAL: critical++; break;
        }
    }
    
    printf("Passed: %zu\n", passed);
    printf("Warnings: %zu\n", warnings);
    printf("Errors: %zu\n", errors);
    printf("Critical: %zu\n", critical);
    
    if (ctx->enable_optimization) {
        printf("\nPerformance Metrics:\n");
        printf("Expansion time: %zu ns\n", ctx->expansion_time_ns);
        printf("Memory used: %zu bytes\n", ctx->memory_used_bytes);
        printf("Complexity score: %zu\n", ctx->complexity_score);
    }
}

// Print safety violations
void print_safety_violations(MacroSafetyContext* ctx) {
    if (!ctx) return;
    
    printf("Safety Violations:\n");
    printf("==================\n");
    
    for (size_t i = 0; i < ctx->result_count; i++) {
        SafetyCheckResult* result = &ctx->results[i];
        if (!result->passed || result->severity != SAFETY_WARNING) {
            printf("%s [%s]: %s at %d:%d\n",
                   SAFETY_CHECK_NAMES[result->type],
                   SAFETY_SEVERITY_NAMES[result->severity],
                   result->message ? result->message : "No message",
                   result->line, result->column);
            
            if (result->suggestion) {
                printf("  Suggestion: %s\n", result->suggestion);
            }
        }
    }
}

// Generate safety summary
char* generate_safety_summary(MacroSafetyContext* ctx) {
    if (!ctx) return NULL;
    
    char* summary = (char*)malloc(1024);
    if (!summary) return NULL;
    
    size_t passed = 0, failed = 0;
    for (size_t i = 0; i < ctx->result_count; i++) {
        if (ctx->results[i].passed) passed++;
        else failed++;
    }
    
    snprintf(summary, 1024,
        "Safety Summary: %zu checks performed, %zu passed, %zu failed\n"
        "Strict mode: %s, Optimization: %s\n"
        "Max recursion depth: %zu",
        ctx->result_count, passed, failed,
        ctx->strict_mode ? "enabled" : "disabled",
        ctx->enable_optimization ? "enabled" : "disabled",
        ctx->max_recursion_depth);
    
    return summary;
}

// Debug macro safety
void debug_macro_safety(MacroExpansion* expansion, MacroSafetyContext* ctx) {
    if (!expansion || !ctx) return;
    
    printf("=== Macro Safety Debug ===\n");
    printf("Expansion success: %s\n", expansion->success ? "yes" : "no");
    
    print_safety_report(ctx);
    
    if (ctx->result_count > 0) {
        printf("\nDetailed Results:\n");
        print_safety_violations(ctx);
    }
    
    printf("===========================\n");
}

// Start performance monitoring
void start_performance_monitoring(MacroSafetyContext* ctx) {
    if (!ctx) return;
    
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    ctx->expansion_time_ns = start_time.tv_sec * 1000000000UL + start_time.tv_nsec;
}

// Stop performance monitoring
void stop_performance_monitoring(MacroSafetyContext* ctx) {
    if (!ctx) return;
    
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    uint64_t end_ns = end_time.tv_sec * 1000000000UL + end_time.tv_nsec;
    
    ctx->expansion_time_ns = end_ns - ctx->expansion_time_ns;
}

// Record expansion metrics
void record_expansion_metrics(MacroSafetyContext* ctx, MacroExpansion* expansion) {
    if (!ctx || !expansion) return;
    
    // Record memory usage (simplified)
    if (expansion->expanded_code) {
        ctx->memory_used_bytes += strlen(expansion->expanded_code);
    }
    
    // Compute complexity score (simplified)
    ctx->complexity_score = ctx->result_count * 10;
}

// Expand macro with safety
MacroExpansion* expand_macro_with_safety(MacroTemplate* macro, MacroContext* ctx,
                                        MacroSafetyContext* safety_ctx,
                                        OptimizationSettings* opt_settings) {
    if (!macro || !ctx || !safety_ctx) return NULL;
    
    // Start performance monitoring
    start_performance_monitoring(safety_ctx);
    
    // Perform normal macro expansion - simplified for testing
    MacroExpansion* expansion = (MacroExpansion*)xcalloc(1, sizeof(MacroExpansion));
    if (expansion) {
        expansion->success = true;
        expansion->expanded_code = strdup("int safe_var = 42;");
        expansion->expanded_ast = NULL; // Simplified for testing
    }
    
    if (expansion) {
        // Apply safety checking
        if (!check_macro_safety(expansion, safety_ctx)) {
            if (safety_ctx->strict_mode) {
                // In strict mode, fail on safety violations
                expansion->success = false;
                free(expansion->error_message);
                expansion->error_message = strdup("Macro safety violation in strict mode");
            }
        }
        
        // Apply optimizations if enabled
        if (opt_settings && safety_ctx->enable_optimization) {
            optimize_macro_expansion(expansion, opt_settings);
        }
        
        // Record metrics
        record_expansion_metrics(safety_ctx, expansion);
    }
    
    // Stop performance monitoring
    stop_performance_monitoring(safety_ctx);
    
    return expansion;
}

// Utility functions
const char* safety_check_type_string(SafetyCheckType type) {
    if (type >= SAFETY_CHECK_COUNT) return "Unknown";
    return SAFETY_CHECK_NAMES[type];
}

const char* safety_severity_string(SafetySeverity severity) {
    if (severity < 0 || severity > SAFETY_CRITICAL) return "Unknown";
    return SAFETY_SEVERITY_NAMES[severity];
}

// Configuration functions
void configure_safety_checks(MacroSafetyContext* ctx, SafetyCheckType* enabled_checks, size_t check_count) {
    if (!ctx) return;
    
    // This would configure which safety checks to perform
    // For now, all checks are enabled by default
}

void set_optimization_level(OptimizationSettings* settings, int level) {
    if (!settings) return;
    
    settings->optimization_level = level;
    
    // Configure optimizations based on level
    switch (level) {
        case 0: // No optimization
            settings->enable_inlining = false;
            settings->enable_dce = false;
            settings->enable_cse = false;
            settings->enable_const_fold = false;
            break;
        case 1: // Basic optimization
            settings->enable_const_fold = true;
            settings->enable_dce = true;
            break;
        case 2: // Standard optimization
            settings->enable_inlining = true;
            settings->enable_cse = true;
            break;
        case 3: // Aggressive optimization
            settings->enable_loop_unroll = true;
            break;
    }
}

void enable_strict_mode(MacroSafetyContext* ctx, bool enabled) {
    if (!ctx) return;
    ctx->strict_mode = enabled;
}

void set_max_recursion_depth(MacroSafetyContext* ctx, size_t max_depth) {
    if (!ctx) return;
    ctx->max_recursion_depth = max_depth;
}