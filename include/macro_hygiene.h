#ifndef MACRO_HYGIENE_H
#define MACRO_HYGIENE_H

#include "advanced_macro_system.h"
#include "ast.h"
#include "types.h"
#include <stdbool.h>

// Hygiene scope levels
typedef enum {
    HYGIENE_SCOPE_GLOBAL,       // Global scope
    HYGIENE_SCOPE_MODULE,       // Module scope
    HYGIENE_SCOPE_FUNCTION,     // Function scope
    HYGIENE_SCOPE_BLOCK,        // Block scope
    HYGIENE_SCOPE_MACRO,        // Macro expansion scope
    HYGIENE_SCOPE_COUNT
} HygieneScopeType;

// Hygiene binding information
typedef struct HygieneBinding {
    char* name;                 // Variable/function name
    char* mangled_name;         // Hygiene-mangled name
    Type* type;                 // Type information
    HygieneScopeType scope;     // Scope level
    int generation;             // Hygiene generation number
    bool is_captured;           // Whether variable is captured
    struct HygieneBinding* next; // Linked list
} HygieneBinding;

// Hygiene scope management
typedef struct HygieneScope {
    HygieneScopeType type;      // Scope type
    char* scope_id;             // Unique scope identifier
    HygieneBinding* bindings;   // Variables in this scope
    int generation_counter;     // Counter for unique generations
    struct HygieneScope* parent; // Parent scope
    struct HygieneScope** children; // Child scopes
    size_t child_count;         // Number of child scopes
} HygieneScope;

// Macro hygiene context
typedef struct MacroHygieneContext {
    HygieneScope* current_scope; // Current hygiene scope
    HygieneScope* global_scope;  // Global scope
    int expansion_depth;         // Current expansion depth
    char** reserved_names;       // Reserved identifier names
    size_t reserved_count;       // Number of reserved names
    
    // Hygiene violation tracking
    char** violations;          // List of hygiene violations
    size_t violation_count;     // Number of violations
    
    // Name generation
    int name_counter;           // Counter for unique names
    char* macro_prefix;         // Prefix for macro-generated names
} MacroHygieneContext;

// Hygiene violation types
typedef enum {
    HYGIENE_VIOLATION_CAPTURE,     // Variable capture
    HYGIENE_VIOLATION_SHADOW,      // Variable shadowing
    HYGIENE_VIOLATION_RESERVED,    // Reserved name usage
    HYGIENE_VIOLATION_ESCAPE,      // Variable escape from scope
    HYGIENE_VIOLATION_COUNT
} HygieneViolationType;

// Hygiene violation report
typedef struct HygieneViolation {
    HygieneViolationType type;  // Type of violation
    char* variable_name;        // Variable involved
    char* macro_name;           // Macro that caused violation
    int line;                   // Line number
    int column;                 // Column number
    char* description;          // Human-readable description
} HygieneViolation;

// Core hygiene functions
MacroHygieneContext* create_hygiene_context(void);
void destroy_hygiene_context(MacroHygieneContext* ctx);
bool register_macro_hygiene(MacroTemplate* macro, MacroHygieneContext* hygiene_ctx);

// Scope management
HygieneScope* create_hygiene_scope(HygieneScopeType type, const char* scope_id, HygieneScope* parent);
void destroy_hygiene_scope(HygieneScope* scope);
HygieneScope* enter_hygiene_scope(MacroHygieneContext* ctx, HygieneScopeType type, const char* scope_id);
HygieneScope* exit_hygiene_scope(MacroHygieneContext* ctx);

// Variable binding management
HygieneBinding* create_hygiene_binding(const char* name, Type* type, HygieneScopeType scope);
void destroy_hygiene_binding(HygieneBinding* binding);
bool add_hygiene_binding(HygieneScope* scope, HygieneBinding* binding);
HygieneBinding* find_hygiene_binding(HygieneScope* scope, const char* name);
HygieneBinding* resolve_hygiene_binding(MacroHygieneContext* ctx, const char* name);

// Name mangling for hygiene
char* generate_hygienic_name(MacroHygieneContext* ctx, const char* base_name);
char* mangle_variable_name(const char* var_name, int generation, const char* macro_name);
char* generate_unique_identifier(MacroHygieneContext* ctx, const char* prefix);
bool is_hygienic_name(const char* name);

// Hygiene checking
bool check_macro_hygiene(MacroExpansion* expansion, MacroHygieneContext* ctx);
bool validate_variable_access(const char* var_name, MacroHygieneContext* ctx);
bool check_name_collision(const char* name, MacroHygieneContext* ctx);
HygieneViolation* detect_hygiene_violation(const char* var_name, MacroHygieneContext* ctx);

// AST transformation for hygiene
ASTNode* apply_hygiene_transformation(ASTNode* node, MacroHygieneContext* ctx);
ASTNode* rename_variables_in_ast(ASTNode* node, MacroHygieneContext* ctx);
bool transform_macro_expansion(MacroExpansion* expansion, MacroHygieneContext* ctx);

// Variable capture analysis
bool analyze_variable_capture(ASTNode* macro_body, MacroHygieneContext* ctx);
char** find_captured_variables(ASTNode* node, HygieneScope* scope, size_t* count);
bool prevent_variable_capture(MacroExpansion* expansion, MacroHygieneContext* ctx);

// Reserved name management
bool add_reserved_name(MacroHygieneContext* ctx, const char* name);
bool is_reserved_name(MacroHygieneContext* ctx, const char* name);
void init_builtin_reserved_names(MacroHygieneContext* ctx);

// Hygiene violation reporting
void report_hygiene_violation(MacroHygieneContext* ctx, HygieneViolationType type, 
                             const char* var_name, const char* macro_name, 
                             int line, int column, const char* description);
void print_hygiene_violations(MacroHygieneContext* ctx);
void clear_hygiene_violations(MacroHygieneContext* ctx);

// Hygiene debugging and analysis
void print_hygiene_scope_tree(HygieneScope* scope, int depth);
void print_hygiene_bindings(HygieneScope* scope);
char* get_hygiene_analysis_report(MacroHygieneContext* ctx);
void debug_macro_hygiene(MacroExpansion* expansion, MacroHygieneContext* ctx);

// Integration with macro system
bool integrate_hygiene_with_macros(MacroRegistry* registry, MacroHygieneContext* hygiene_ctx);
MacroExpansion* expand_macro_with_hygiene(MacroTemplate* macro, MacroContext* ctx, 
                                         MacroHygieneContext* hygiene_ctx);
bool validate_macro_expansion_hygiene(MacroExpansion* expansion, MacroHygieneContext* hygiene_ctx);

// Performance optimization hooks
bool optimize_hygienic_names(MacroHygieneContext* ctx);
void cache_hygiene_lookups(MacroHygieneContext* ctx);
bool minimize_name_mangling(MacroHygieneContext* ctx);

#endif // MACRO_HYGIENE_H