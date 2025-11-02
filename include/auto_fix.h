#ifndef AUTO_FIX_H
#define AUTO_FIX_H

#include "types.h"
#include "ast.h"
#include "repl.h"
#include "errors/error.h"
#include <stdbool.h>
#include <stddef.h>

// =============================================================================
// Automatic Error Correction System for Goo
// =============================================================================

// Forward declarations
typedef struct AutoFixEngine AutoFixEngine;
typedef struct ErrorPattern ErrorPattern;
typedef struct FixSuggestion FixSuggestion;
typedef struct CodeFix CodeFix;
typedef struct FixContext FixContext;

// ErrorCategory is defined in errors/error.h

// Fix confidence levels
typedef enum {
    FIX_CONFIDENCE_HIGH,    // 90%+ confidence, safe to auto-apply
    FIX_CONFIDENCE_MEDIUM,  // 70-90% confidence, suggest with caution
    FIX_CONFIDENCE_LOW,     // 50-70% confidence, require user confirmation
    FIX_CONFIDENCE_UNSAFE   // <50% confidence, informational only
} FixConfidence;

// Fix application scope
typedef enum {
    FIX_SCOPE_SINGLE,       // Single location fix
    FIX_SCOPE_FUNCTION,     // Function-wide fix
    FIX_SCOPE_FILE,         // File-wide fix
    FIX_SCOPE_PROJECT       // Project-wide fix
} FixScope;

// Error pattern for matching compiler errors
struct ErrorPattern {
    char* pattern_id;
    char* description;
    ErrorCategory category;
    char* regex_pattern;      // Regex to match error messages
    char* example_error;      // Example error message
    int priority;             // Higher priority patterns checked first
    struct ErrorPattern* next;
};

// Code fix suggestion
struct CodeFix {
    char* description;
    char* before_code;        // Original code (for display)
    char* after_code;         // Fixed code
    Position start_pos;       // Start position for replacement
    Position end_pos;         // End position for replacement
    FixScope scope;
    char* explanation;        // Why this fix works
    char* side_effects;       // Potential side effects
    struct CodeFix* next;
};

// Fix suggestion with metadata
struct FixSuggestion {
    char* suggestion_id;
    char* title;
    char* description;
    ErrorCategory category;
    FixConfidence confidence;
    
    CodeFix* fixes;           // List of code changes
    int fix_count;
    
    char* reasoning;          // AI reasoning for the fix
    char* examples;           // Usage examples after fix
    char* documentation;      // Relevant documentation links
    
    // Metadata
    bool is_breaking;         // Is this a breaking change?
    bool requires_testing;    // Should user run tests after?
    bool affects_api;         // Does this change public API?
    
    struct FixSuggestion* next;
};

// Context for fix generation
struct FixContext {
    char* filename;
    char* error_message;
    Position error_position;
    ASTNode* ast_context;     // AST node around error
    Type* expected_type;      // Expected type (for type errors)
    Type* actual_type;        // Actual type (for type errors)
    Scope* current_scope;     // Current scope
    char* function_context;   // Current function name
    char* additional_info;    // Extra context
};

// Main auto-fix engine
struct AutoFixEngine {
    ErrorPattern* patterns;   // Error pattern database
    int pattern_count;
    
    // Configuration
    bool auto_apply_safe;     // Auto-apply high confidence fixes
    bool show_explanations;   // Show detailed explanations
    bool batch_mode;          // Enable batch fixing
    FixConfidence min_confidence; // Minimum confidence to show
    
    // Statistics
    int suggestions_made;
    int fixes_applied;
    int patterns_matched;
    int false_positives;
    
    // Integration
    TypeChecker* type_checker;
    REPLContext* repl_context; // For interactive mode
};

// =============================================================================
// Engine Management
// =============================================================================

/**
 * Create a new auto-fix engine
 */
AutoFixEngine* auto_fix_engine_new(void);

/**
 * Free auto-fix engine and all resources
 */
void auto_fix_engine_free(AutoFixEngine* engine);

/**
 * Initialize the engine with built-in patterns
 */
int auto_fix_engine_init(AutoFixEngine* engine);

/**
 * Load custom error patterns from file
 */
int auto_fix_engine_load_patterns(AutoFixEngine* engine, const char* patterns_file);

/**
 * Set integration with type checker
 */
void auto_fix_engine_set_type_checker(AutoFixEngine* engine, TypeChecker* type_checker);

/**
 * Set integration with REPL
 */
void auto_fix_engine_set_repl(AutoFixEngine* engine, REPLContext* repl_context);

// =============================================================================
// Error Analysis and Fix Generation
// =============================================================================

/**
 * Analyze an error and generate fix suggestions
 */
FixSuggestion* auto_fix_analyze_error(AutoFixEngine* engine, const FixContext* context);

/**
 * Generate fixes for a specific error category
 */
FixSuggestion* auto_fix_generate_category_fixes(AutoFixEngine* engine, 
                                               ErrorCategory category,
                                               const FixContext* context);

/**
 * Match error message against known patterns
 */
ErrorPattern* auto_fix_match_pattern(AutoFixEngine* engine, const char* error_message);

/**
 * Generate type error fixes
 */
FixSuggestion* auto_fix_generate_type_fixes(AutoFixEngine* engine, const FixContext* context);

/**
 * Generate ownership error fixes
 */
FixSuggestion* auto_fix_generate_ownership_fixes(AutoFixEngine* engine, const FixContext* context);

/**
 * Generate null safety fixes
 */
FixSuggestion* auto_fix_generate_null_safety_fixes(AutoFixEngine* engine, const FixContext* context);

/**
 * Generate syntax error fixes
 */
FixSuggestion* auto_fix_generate_syntax_fixes(AutoFixEngine* engine, const FixContext* context);

// =============================================================================
// Fix Application
// =============================================================================

/**
 * Apply a fix suggestion to code
 */
int auto_fix_apply_suggestion(AutoFixEngine* engine, FixSuggestion* suggestion, 
                             const char* filename);

/**
 * Apply multiple fixes in batch
 */
int auto_fix_apply_batch(AutoFixEngine* engine, FixSuggestion** suggestions, 
                        int count, const char* filename);

/**
 * Preview what a fix would do (dry run)
 */
char* auto_fix_preview_fix(FixSuggestion* suggestion, const char* original_code);

/**
 * Validate that a fix is safe to apply
 */
bool auto_fix_validate_fix(AutoFixEngine* engine, FixSuggestion* suggestion);

// =============================================================================
// Interactive Features
// =============================================================================

/**
 * Show fix suggestions in interactive mode
 */
void auto_fix_show_suggestions(AutoFixEngine* engine, FixSuggestion* suggestions);

/**
 * Prompt user to select and apply fixes
 */
int auto_fix_interactive_apply(AutoFixEngine* engine, FixSuggestion* suggestions,
                              const char* filename);

/**
 * REPL command for analyzing current error
 */
int auto_fix_repl_command(REPLContext* ctx, const char* args);

/**
 * Show auto-fix help in REPL
 */
int auto_fix_repl_show_help(REPLContext* ctx);

/**
 * Analyze error in REPL
 */
int auto_fix_repl_analyze(REPLContext* ctx, const char* args);

/**
 * Show auto-fix demo in REPL
 */
int auto_fix_repl_demo(REPLContext* ctx);

/**
 * Show error patterns in REPL
 */
int auto_fix_repl_show_patterns(REPLContext* ctx);

/**
 * Show auto-fix statistics in REPL
 */
int auto_fix_repl_show_stats(REPLContext* ctx);

/**
 * Configure auto-fix settings in REPL
 */
int auto_fix_repl_config(REPLContext* ctx, const char* args);

/**
 * Handle error events in REPL
 */
void auto_fix_repl_on_error(REPLContext* ctx, const char* error_message, Position pos);

/**
 * Cleanup auto-fix resources
 */
void auto_fix_repl_cleanup(void);

// =============================================================================
// Pattern Management
// =============================================================================

/**
 * Add a new error pattern
 */
int auto_fix_add_pattern(AutoFixEngine* engine, const ErrorPattern* pattern);

/**
 * Remove an error pattern
 */
int auto_fix_remove_pattern(AutoFixEngine* engine, const char* pattern_id);

/**
 * List all error patterns
 */
void auto_fix_list_patterns(AutoFixEngine* engine);

/**
 * Test pattern matching against sample errors
 */
int auto_fix_test_patterns(AutoFixEngine* engine, const char* test_file);

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * Create a fix context from error information
 */
FixContext* auto_fix_context_new(const char* filename, const char* error_message,
                                Position pos, ASTNode* ast_context);

/**
 * Free fix context
 */
void auto_fix_context_free(FixContext* context);

/**
 * Free fix suggestion and all associated data
 */
void auto_fix_suggestion_free(FixSuggestion* suggestion);

/**
 * Free code fix
 */
void auto_fix_code_fix_free(CodeFix* fix);

/**
 * Free error pattern
 */
void auto_fix_pattern_free(ErrorPattern* pattern);

/**
 * Generate unique suggestion ID
 */
char* auto_fix_generate_suggestion_id(void);

/**
 * Calculate fix confidence based on context
 */
FixConfidence auto_fix_calculate_confidence(const FixContext* context, 
                                           const CodeFix* fix);

/**
 * Format suggestion for display
 */
char* auto_fix_format_suggestion(const FixSuggestion* suggestion, bool include_code);

/**
 * Check if error message matches category
 */
bool auto_fix_error_matches_category(const char* error_message, ErrorCategory category);

// =============================================================================
// Built-in Fix Generators
// =============================================================================

/**
 * Generate import fixes (add missing imports)
 */
FixSuggestion* auto_fix_generate_import_fixes(AutoFixEngine* engine, const FixContext* context);

/**
 * Generate generic constraint fixes
 */
FixSuggestion* auto_fix_generate_generic_fixes(AutoFixEngine* engine, const FixContext* context);

/**
 * Generate interface implementation fixes
 */
FixSuggestion* auto_fix_generate_interface_fixes(AutoFixEngine* engine, const FixContext* context);

/**
 * Generate error handling fixes
 */
FixSuggestion* auto_fix_generate_error_handling_fixes(AutoFixEngine* engine, const FixContext* context);

/**
 * Generate concurrency fixes
 */
FixSuggestion* auto_fix_generate_concurrency_fixes(AutoFixEngine* engine, const FixContext* context);

// =============================================================================
// Statistics and Reporting
// =============================================================================

/**
 * Print auto-fix engine statistics
 */
void auto_fix_print_statistics(AutoFixEngine* engine);

/**
 * Generate usage report
 */
char* auto_fix_generate_report(AutoFixEngine* engine);

/**
 * Reset statistics
 */
void auto_fix_reset_statistics(AutoFixEngine* engine);

#endif // AUTO_FIX_H