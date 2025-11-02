#ifndef REPL_TYPE_INFO_H
#define REPL_TYPE_INFO_H

#include "repl.h"
#include "types.h"

// =============================================================================
// Enhanced Type Information System for REPL
// =============================================================================

// Type information detail structure
typedef struct TypeInfoDetail {
    char* name;
    char* signature;
    char* documentation;
    char* source_location;
    char* examples;
    int complexity_score;
    bool is_generic;
    bool has_constraints;
    struct TypeInfoDetail* next;
} TypeInfoDetail;

// =============================================================================
// Enhanced Type Formatting Functions
// =============================================================================

/**
 * Format type with detailed information including size, alignment, and relationships
 * @param ctx REPL context for color formatting
 * @param type The type to format
 * @param include_docs Whether to include documentation
 * @return Formatted type string (caller must free)
 */
char* repl_format_type_detailed(REPLContext* ctx, const Type* type, bool include_docs);

/**
 * Print detailed type hierarchy showing inheritance and relationships
 * @param ctx REPL context
 * @param type The type to analyze
 * @return 0 on success, -1 on error
 */
int repl_print_type_hierarchy_detailed(REPLContext* ctx, const Type* type);

/**
 * Print available methods and operations for a type
 * @param ctx REPL context
 * @param type The type to analyze
 * @return 0 on success, -1 on error
 */
int repl_print_type_methods(REPLContext* ctx, const Type* type);

/**
 * Print usage examples and common patterns for a type
 * @param ctx REPL context
 * @param type The type to show examples for
 * @return 0 on success, -1 on error
 */
int repl_print_type_examples(REPLContext* ctx, const Type* type);

/**
 * Print performance characteristics and complexity information
 * @param ctx REPL context
 * @param type The type to analyze
 * @return 0 on success, -1 on error
 */
int repl_print_type_performance(REPLContext* ctx, const Type* type);

// =============================================================================
// Type Documentation System
// =============================================================================

/**
 * Get documentation for a named type
 * @param ctx REPL context
 * @param type_name Name of the type
 * @return Documentation string (caller must free) or NULL if not found
 */
char* repl_get_type_documentation(REPLContext* ctx, const char* type_name);

/**
 * Enhanced type inspection command - shows comprehensive type information
 * @param ctx REPL context
 * @param args Command arguments (type name or expression)
 * @return 0 on success, -1 on error
 */
int repl_cmd_inspect_type(REPLContext* ctx, const char* args);

// =============================================================================
// Live Type Information Display
// =============================================================================

/**
 * Print live type information during expression evaluation
 * @param ctx REPL context
 * @param expression The expression being evaluated
 * @param type The inferred type
 */
void repl_print_live_type_info(REPLContext* ctx, const char* expression, const Type* type);

// =============================================================================
// Type-aware Auto-completion
// =============================================================================

/**
 * Get auto-completion suggestions for type members (methods, fields)
 * @param ctx REPL context
 * @param type The type to get completions for
 * @param prefix The partial member name
 * @return Completion list (caller must free with repl_completion_free)
 */
REPLCompletion* repl_complete_type_members(REPLContext* ctx, const Type* type, const char* prefix);

// =============================================================================
// Type System Integration
// =============================================================================

/**
 * Create detailed type information structure
 * @param type The type to analyze
 * @return TypeInfoDetail structure (caller must free)
 */
TypeInfoDetail* repl_create_type_info_detail(const Type* type);

/**
 * Free type information detail structure
 * @param detail The structure to free
 */
void repl_free_type_info_detail(TypeInfoDetail* detail);

/**
 * Check if a type supports specific operations
 * @param type The type to check
 * @param operation Operation name (e.g., "indexing", "iteration", "comparison")
 * @return true if operation is supported
 */
bool repl_type_supports_operation(const Type* type, const char* operation);

/**
 * Get type conversion suggestions for incompatible types
 * @param ctx REPL context
 * @param from_type Source type
 * @param to_type Target type
 * @return Conversion suggestion string (caller must free) or NULL
 */
char* repl_get_type_conversion_suggestion(REPLContext* ctx, const Type* from_type, const Type* to_type);

// =============================================================================
// Interactive Type Explorer
// =============================================================================

/**
 * Start interactive type exploration mode
 * @param ctx REPL context
 * @param type_name Optional starting type name
 * @return 0 on success, -1 on error
 */
int repl_start_type_explorer(REPLContext* ctx, const char* type_name);

/**
 * Show type relationships graph
 * @param ctx REPL context
 * @param root_type Root type for the graph
 * @param max_depth Maximum depth to explore
 * @return 0 on success, -1 on error
 */
int repl_print_type_graph(REPLContext* ctx, const Type* root_type, int max_depth);

/**
 * Compare two types and show differences
 * @param ctx REPL context
 * @param type1 First type
 * @param type2 Second type
 * @return 0 on success, -1 on error
 */
int repl_compare_types(REPLContext* ctx, const Type* type1, const Type* type2);

// =============================================================================
// Type Safety Analysis
// =============================================================================

/**
 * Analyze type safety properties
 * @param ctx REPL context
 * @param type The type to analyze
 * @return 0 on success, -1 on error
 */
int repl_analyze_type_safety(REPLContext* ctx, const Type* type);

/**
 * Show memory safety guarantees for a type
 * @param ctx REPL context
 * @param type The type to analyze
 * @return 0 on success, -1 on error
 */
int repl_show_memory_safety(REPLContext* ctx, const Type* type);

/**
 * Display ownership and borrowing rules for a type
 * @param ctx REPL context
 * @param type The type to analyze
 * @return 0 on success, -1 on error
 */
int repl_show_ownership_rules(REPLContext* ctx, const Type* type);

// =============================================================================
// Advanced Type Features
// =============================================================================

/**
 * Show generic type parameters and constraints
 * @param ctx REPL context
 * @param type The generic type
 * @return 0 on success, -1 on error
 */
int repl_show_generic_info(REPLContext* ctx, const Type* type);

/**
 * Display trait/interface implementations
 * @param ctx REPL context
 * @param type The type to analyze
 * @return 0 on success, -1 on error
 */
int repl_show_trait_implementations(REPLContext* ctx, const Type* type);

/**
 * Show type-level computation capabilities
 * @param ctx REPL context
 * @param type The type to analyze
 * @return 0 on success, -1 on error
 */
int repl_show_type_level_features(REPLContext* ctx, const Type* type);

#endif // REPL_TYPE_INFO_H