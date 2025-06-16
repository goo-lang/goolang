#ifndef MACRO_SAFETY_H
#define MACRO_SAFETY_H

#include "advanced_macro_system.h"
#include "macro_hygiene.h"
#include "ast.h"
#include "types.h"
#include <stdbool.h>

// Safety check types
typedef enum {
    SAFETY_CHECK_TYPE,          // Type safety verification
    SAFETY_CHECK_MEMORY,        // Memory safety verification
    SAFETY_CHECK_BOUNDS,        // Bounds checking
    SAFETY_CHECK_NULL,          // Null pointer checking
    SAFETY_CHECK_OVERFLOW,      // Integer overflow checking
    SAFETY_CHECK_LIFETIME,      // Lifetime and ownership checking
    SAFETY_CHECK_RECURSION,     // Recursion depth checking
    SAFETY_CHECK_COUNT
} SafetyCheckType;

// Safety violation severity
typedef enum {
    SAFETY_WARNING,             // Warning (non-fatal)
    SAFETY_ERROR,               // Error (compilation failure)
    SAFETY_CRITICAL             // Critical error (potential security issue)
} SafetySeverity;

// Safety check result
typedef struct SafetyCheckResult {
    SafetyCheckType type;       // Type of check performed
    bool passed;                // Whether check passed
    SafetySeverity severity;    // Severity of any issues
    char* message;              // Detailed message
    int line;                   // Line number
    int column;                 // Column number
    char* suggestion;           // Suggested fix
} SafetyCheckResult;

// Macro safety context
typedef struct MacroSafetyContext {
    SafetyCheckResult* results; // Array of check results
    size_t result_count;        // Number of results
    size_t max_recursion_depth; // Maximum allowed recursion depth
    bool strict_mode;           // Whether to use strict safety checks
    bool enable_optimization;   // Whether optimizations are enabled
    
    // Type checking context
    Type** allowed_types;       // Types allowed in macro context
    size_t allowed_type_count;  // Number of allowed types
    
    // Memory tracking
    void** allocated_pointers;  // Pointers allocated in macro context
    size_t allocation_count;    // Number of allocations
    
    // Performance metrics
    size_t expansion_time_ns;   // Time taken for expansion (nanoseconds)
    size_t memory_used_bytes;   // Memory used during expansion
    size_t complexity_score;    // Computed complexity score
} MacroSafetyContext;

// Type safety checking
typedef struct TypeSafetyChecker {
    Type* expected_type;        // Expected type
    Type* actual_type;          // Actual type
    bool allow_coercion;        // Whether type coercion is allowed
    bool check_const;           // Whether to check const correctness
    bool check_nullability;     // Whether to check null safety
} TypeSafetyChecker;

// Memory safety tracking
typedef struct MemorySafetyTracker {
    void** live_pointers;       // Currently live pointers
    size_t live_count;          // Number of live pointers
    void** freed_pointers;      // Previously freed pointers
    size_t freed_count;         // Number of freed pointers
    bool track_leaks;           // Whether to track memory leaks
} MemorySafetyTracker;

// Performance optimization settings
typedef struct OptimizationSettings {
    bool enable_inlining;       // Enable function inlining
    bool enable_dce;            // Enable dead code elimination
    bool enable_cse;            // Enable common subexpression elimination
    bool enable_loop_unroll;    // Enable loop unrolling
    bool enable_const_fold;     // Enable constant folding
    int optimization_level;     // Optimization level (0-3)
} OptimizationSettings;

// Core safety functions
MacroSafetyContext* create_safety_context(void);
void destroy_safety_context(MacroSafetyContext* ctx);
bool register_macro_safety_checks(MacroTemplate* macro, MacroSafetyContext* safety_ctx);

// Safety checking
bool check_macro_safety(MacroExpansion* expansion, MacroSafetyContext* ctx);
SafetyCheckResult* perform_type_safety_check(ASTNode* node, MacroSafetyContext* ctx);
SafetyCheckResult* perform_memory_safety_check(ASTNode* node, MacroSafetyContext* ctx);
SafetyCheckResult* perform_bounds_check(ASTNode* node, MacroSafetyContext* ctx);
SafetyCheckResult* perform_null_check(ASTNode* node, MacroSafetyContext* ctx);
SafetyCheckResult* perform_overflow_check(ASTNode* node, MacroSafetyContext* ctx);
SafetyCheckResult* perform_lifetime_check(ASTNode* node, MacroSafetyContext* ctx);
SafetyCheckResult* perform_recursion_check(MacroExpansion* expansion, MacroSafetyContext* ctx);

// Type safety
bool verify_type_compatibility(Type* expected, Type* actual, TypeSafetyChecker* checker);
bool check_function_call_safety(ASTNode* call_node, MacroSafetyContext* ctx);
bool validate_macro_parameters(MacroTemplate* macro, ComptimeValue** args, size_t arg_count);
bool enforce_const_correctness(ASTNode* node, MacroSafetyContext* ctx);

// Memory safety
MemorySafetyTracker* create_memory_tracker(void);
void destroy_memory_tracker(MemorySafetyTracker* tracker);
bool track_allocation(MemorySafetyTracker* tracker, void* ptr);
bool track_deallocation(MemorySafetyTracker* tracker, void* ptr);
bool detect_memory_leaks(MemorySafetyTracker* tracker);
bool detect_double_free(MemorySafetyTracker* tracker, void* ptr);
bool detect_use_after_free(MemorySafetyTracker* tracker, void* ptr);

// Bounds checking
bool check_array_bounds(ASTNode* array_access, MacroSafetyContext* ctx);
bool validate_buffer_operations(ASTNode* buffer_op, MacroSafetyContext* ctx);
bool check_string_operations(ASTNode* string_op, MacroSafetyContext* ctx);

// Null safety
bool verify_null_safety(ASTNode* node, MacroSafetyContext* ctx);
bool check_null_dereference(ASTNode* deref_node, MacroSafetyContext* ctx);
bool validate_optional_unwrapping(ASTNode* unwrap_node, MacroSafetyContext* ctx);

// Overflow checking
bool check_integer_overflow(ASTNode* arithmetic_node, MacroSafetyContext* ctx);
bool validate_arithmetic_operations(ASTNode* op_node, MacroSafetyContext* ctx);
bool detect_signed_unsigned_issues(ASTNode* node, MacroSafetyContext* ctx);

// Performance optimization
OptimizationSettings* create_optimization_settings(void);
void destroy_optimization_settings(OptimizationSettings* settings);
bool optimize_macro_expansion(MacroExpansion* expansion, OptimizationSettings* settings);
bool inline_macro_functions(ASTNode* node, OptimizationSettings* settings);
bool eliminate_dead_code(ASTNode* node, OptimizationSettings* settings);
bool perform_constant_folding(ASTNode* node, OptimizationSettings* settings);
bool optimize_common_subexpressions(ASTNode* node, OptimizationSettings* settings);

// Zero-cost abstractions
bool verify_zero_cost_abstraction(MacroTemplate* macro, MacroSafetyContext* ctx);
bool analyze_runtime_overhead(MacroExpansion* expansion, MacroSafetyContext* ctx);
bool optimize_for_zero_cost(MacroExpansion* expansion, OptimizationSettings* settings);

// Error reporting and debugging
void add_safety_result(MacroSafetyContext* ctx, SafetyCheckResult* result);
void print_safety_report(MacroSafetyContext* ctx);
void print_safety_violations(MacroSafetyContext* ctx);
char* generate_safety_summary(MacroSafetyContext* ctx);
void debug_macro_safety(MacroExpansion* expansion, MacroSafetyContext* ctx);

// Integration with macro system
bool integrate_safety_with_macros(MacroRegistry* registry, MacroSafetyContext* safety_ctx);
MacroExpansion* expand_macro_with_safety(MacroTemplate* macro, MacroContext* ctx,
                                        MacroSafetyContext* safety_ctx,
                                        OptimizationSettings* opt_settings);
bool validate_macro_expansion_safety(MacroExpansion* expansion, MacroSafetyContext* safety_ctx);

// Performance monitoring
void start_performance_monitoring(MacroSafetyContext* ctx);
void stop_performance_monitoring(MacroSafetyContext* ctx);
void record_expansion_metrics(MacroSafetyContext* ctx, MacroExpansion* expansion);
void print_performance_metrics(MacroSafetyContext* ctx);

// Advanced safety features
bool enable_taint_analysis(MacroSafetyContext* ctx);
bool perform_dataflow_analysis(ASTNode* node, MacroSafetyContext* ctx);
bool check_information_flow(ASTNode* node, MacroSafetyContext* ctx);
bool validate_security_properties(MacroExpansion* expansion, MacroSafetyContext* ctx);

// Configuration and settings
void configure_safety_checks(MacroSafetyContext* ctx, SafetyCheckType* enabled_checks, size_t check_count);
void set_optimization_level(OptimizationSettings* settings, int level);
void enable_strict_mode(MacroSafetyContext* ctx, bool enabled);
void set_max_recursion_depth(MacroSafetyContext* ctx, size_t max_depth);

// Utility functions
SafetyCheckResult* create_safety_result(SafetyCheckType type, bool passed, SafetySeverity severity,
                                       const char* message, int line, int column);
void destroy_safety_result(SafetyCheckResult* result);
const char* safety_check_type_string(SafetyCheckType type);
const char* safety_severity_string(SafetySeverity severity);

#endif // MACRO_SAFETY_H