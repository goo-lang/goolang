#ifndef ADVANCED_MACRO_SYSTEM_H
#define ADVANCED_MACRO_SYSTEM_H

#include "ast.h"
#include "types.h"
#include "comptime.h"
#include <stdbool.h>

// Forward declarations
typedef struct MacroContext MacroContext;
typedef struct MacroTemplate MacroTemplate;
typedef struct MacroExpansion MacroExpansion;
typedef struct MacroRegistry MacroRegistry;

// Macro types
typedef enum {
    MACRO_FUNCTION,         // Function-like macros: foo!(args)
    MACRO_ATTRIBUTE,        // Attribute macros: #[derive(...)]
    MACRO_PROCEDURAL,       // Full AST transformation macros
    MACRO_TEMPLATE,         // Template-based code generation
    MACRO_HYGIENIC,         // Hygiene-preserving macros
    MACRO_COMPILE_TIME,     // Compile-time evaluation macros
    MACRO_DSL               // Domain-specific language macros
} MacroType;

// Macro hygiene levels
typedef enum {
    HYGIENE_NONE,           // No hygiene (classic C-style)
    HYGIENE_LEXICAL,        // Lexical scoping preservation
    HYGIENE_SEMANTIC,       // Full semantic hygiene
    HYGIENE_TYPED          // Type-aware hygiene
} HygieneLevel;

// Macro parameter types
typedef enum {
    MACRO_PARAM_EXPR,       // Expression parameter
    MACRO_PARAM_STMT,       // Statement parameter
    MACRO_PARAM_TYPE,       // Type parameter
    MACRO_PARAM_IDENT,      // Identifier parameter
    MACRO_PARAM_LITERAL,    // Literal parameter
    MACRO_PARAM_PATTERN,    // Pattern parameter
    MACRO_PARAM_VARIADIC,   // Variadic parameter (...)
    MACRO_PARAM_STRING      // String parameter
} MacroParamType;

// Macro parameter
typedef struct {
    char* name;             // Parameter name
    MacroParamType type;    // Parameter type
    bool is_optional;       // Whether parameter is optional
    ComptimeValue* default_value; // Default value if optional
    char* constraint;       // Type or pattern constraint
} MacroParameter;

// Macro template - defines the structure of a macro
typedef struct MacroTemplate {
    char* name;             // Macro name
    MacroType type;         // Type of macro
    HygieneLevel hygiene;   // Hygiene level
    
    MacroParameter* parameters; // Macro parameters
    size_t param_count;
    
    ASTNode* body;          // Macro body template
    char* code_template;    // Raw code template (for simple cases)
    
    // Expansion configuration
    bool is_recursive;      // Can macro call itself
    int max_recursion;      // Maximum recursion depth
    bool generate_debug_info; // Whether to generate debug info
    
    // Compile-time evaluation
    ComptimeValue* (*evaluator)(MacroContext* ctx, ComptimeValue** args);
    
    struct MacroTemplate* next; // For linked lists
} MacroTemplate;

// Macro expansion context
typedef struct MacroContext {
    MacroTemplate* macro;   // The macro being expanded
    ComptimeValue** arguments; // Argument values
    size_t arg_count;
    
    // Context information
    ASTNode* call_site;     // Where macro was called
    char* source_file;      // Source file
    int line;               // Line number
    int column;             // Column number
    
    // Hygiene management
    char** hygiene_scope;   // Current hygiene scope
    size_t scope_depth;
    
    // Recursion control
    int recursion_depth;    // Current recursion depth
    MacroContext* parent;   // Parent expansion context
    
    // Error handling
    char* error_message;    // Error message if expansion fails
    bool has_error;         // Whether an error occurred
} MacroContext;

// Macro expansion result
typedef struct MacroExpansion {
    ASTNode* expanded_ast;  // Expanded AST
    char* expanded_code;    // Expanded source code
    bool success;           // Whether expansion succeeded
    char* error_message;    // Error message if failed
    
    // Debug information
    char* expansion_trace;  // Trace of expansion steps
    MacroContext* context;  // Expansion context
} MacroExpansion;

// Macro registry - manages all macros
typedef struct MacroRegistry {
    MacroTemplate* macros;  // Linked list of macros
    size_t macro_count;
    
    // Built-in macros
    MacroTemplate* builtin_macros;
    
    // Configuration
    bool enable_hygiene;    // Global hygiene setting
    bool debug_expansions;  // Whether to debug expansions
    int max_expansion_depth; // Maximum expansion depth
} MacroRegistry;

// Function declarations

// Registry management
MacroRegistry* create_macro_registry(void);
void destroy_macro_registry(MacroRegistry* registry);

// Macro template management
MacroTemplate* create_macro_template(const char* name, MacroType type);
void destroy_macro_template(MacroTemplate* macro);
bool register_macro(MacroRegistry* registry, MacroTemplate* macro);
MacroTemplate* find_macro(MacroRegistry* registry, const char* name);

// Parameter management
bool add_macro_parameter(MacroTemplate* macro, const char* name, MacroParamType type);
bool set_parameter_constraint(MacroTemplate* macro, const char* param_name, const char* constraint);
bool set_parameter_default(MacroTemplate* macro, const char* param_name, ComptimeValue* default_val);

// Macro expansion
MacroExpansion* expand_macro(MacroRegistry* registry, const char* macro_name, 
                           ComptimeValue** args, size_t arg_count, ASTNode* call_site);
MacroExpansion* expand_macro_ast(MacroRegistry* registry, ASTNode* macro_call);

// Context management
MacroContext* create_macro_context(MacroTemplate* macro, ComptimeValue** args, size_t arg_count);
void destroy_macro_context(MacroContext* context);

// Hygiene system
bool preserve_hygiene(MacroContext* context, ASTNode* node);
char* generate_hygiene_name(MacroContext* context, const char* original_name);
bool check_hygiene_conflicts(MacroContext* context, const char* name);

// Built-in macros
void register_builtin_macros(MacroRegistry* registry);
ComptimeValue* builtin_macro_assert(MacroContext* ctx, ComptimeValue** args);
ComptimeValue* builtin_macro_debug_print(MacroContext* ctx, ComptimeValue** args);
ComptimeValue* builtin_macro_compile_time_if(MacroContext* ctx, ComptimeValue** args);
ComptimeValue* builtin_macro_typeof(MacroContext* ctx, ComptimeValue** args);
ComptimeValue* builtin_macro_stringify(MacroContext* ctx, ComptimeValue** args);

// Template processing
char* process_template(const char* template_str, MacroContext* context);
ASTNode* substitute_template_ast(ASTNode* template_ast, MacroContext* context);

// Error handling
void macro_error(MacroContext* context, const char* format, ...);
void print_macro_expansion_trace(MacroExpansion* expansion);

// Debugging and introspection
void print_macro_registry(MacroRegistry* registry);
char* get_macro_info(MacroTemplate* macro);
void debug_macro_expansion(MacroExpansion* expansion);

// Compile-time macro evaluation
ComptimeValue* evaluate_macro_at_compile_time(MacroContext* context);
bool validate_macro_arguments(MacroTemplate* macro, ComptimeValue** args, size_t arg_count);

#endif // ADVANCED_MACRO_SYSTEM_H
