#ifndef DERIVE_MACROS_H
#define DERIVE_MACROS_H

#include "advanced_macro_system.h"
#include "ast.h"
#include "types.h"

// Derive macro types
typedef enum {
    DERIVE_DEBUG,       // Generate debug printing
    DERIVE_CLONE,       // Generate deep copy
    DERIVE_COPY,        // Generate shallow copy
    DERIVE_PARTIAL_EQ,  // Generate equality comparison
    DERIVE_EQ,          // Generate full equality (requires PartialEq)
    DERIVE_PARTIAL_ORD, // Generate partial ordering
    DERIVE_ORD,         // Generate total ordering
    DERIVE_HASH,        // Generate hash function
    DERIVE_DEFAULT,     // Generate default constructor
    DERIVE_SERIALIZE,   // Generate serialization
    DERIVE_DESERIALIZE, // Generate deserialization
    DERIVE_DISPLAY,     // Generate string representation
    DERIVE_FROM,        // Generate conversion functions
    DERIVE_INTO,        // Generate conversion functions
    DERIVE_TRY_FROM,    // Generate fallible conversion
    DERIVE_TRY_INTO,    // Generate fallible conversion
    DERIVE_COUNT        // Total number of derive types
} DeriveMacroType;

// Derive macro context
typedef struct {
    ASTNode* target_struct;     // The struct being derived for
    DeriveMacroType derive_type; // Type of derive macro
    char** field_names;         // Struct field names
    Type** field_types;         // Struct field types
    size_t field_count;         // Number of fields
    
    // Code generation settings
    bool generate_comments;     // Whether to add comments
    bool optimize_for_size;     // Optimize for code size vs speed
    char* namespace_prefix;     // Prefix for generated functions
} DeriveMacroContext;

// Generated code result
typedef struct {
    char* function_code;        // Generated function implementations
    char* trait_impl_code;      // Generated trait implementations
    char* type_declarations;    // Any additional type declarations
    bool success;               // Whether generation succeeded
    char* error_message;        // Error message if failed
} DeriveResult;

// Core derive macro functions
MacroRegistry* create_derive_macro_registry(void);
bool register_derive_macros(MacroRegistry* registry);

// Derive macro implementations
DeriveResult* derive_debug(DeriveMacroContext* ctx);
DeriveResult* derive_clone(DeriveMacroContext* ctx);
DeriveResult* derive_copy(DeriveMacroContext* ctx);
DeriveResult* derive_partial_eq(DeriveMacroContext* ctx);
DeriveResult* derive_eq(DeriveMacroContext* ctx);
DeriveResult* derive_partial_ord(DeriveMacroContext* ctx);
DeriveResult* derive_ord(DeriveMacroContext* ctx);
DeriveResult* derive_hash(DeriveMacroContext* ctx);
DeriveResult* derive_default(DeriveMacroContext* ctx);
DeriveResult* derive_serialize(DeriveMacroContext* ctx);
DeriveResult* derive_deserialize(DeriveMacroContext* ctx);
DeriveResult* derive_display(DeriveMacroContext* ctx);

// Utility functions
DeriveMacroContext* create_derive_context(ASTNode* struct_node, DeriveMacroType type);
void destroy_derive_context(DeriveMacroContext* ctx);
void destroy_derive_result(DeriveResult* result);

// Struct analysis functions
bool analyze_struct_fields(ASTNode* struct_node, DeriveMacroContext* ctx);
bool is_derive_type_supported(const char* type_name);
DeriveMacroType parse_derive_type(const char* type_name);
const char* derive_type_to_string(DeriveMacroType type);

// Code generation helpers
char* generate_function_signature(const char* func_name, const char* struct_name, 
                                 const char* return_type, const char* params);
char* generate_field_access_code(DeriveMacroContext* ctx, const char* pattern);
char* escape_string_for_debug(const char* str);

// Built-in derive macro evaluators
ComptimeValue* derive_macro_evaluator(MacroContext* ctx, ComptimeValue** args);

// Error handling
void derive_error(DeriveMacroContext* ctx, const char* format, ...);

// Debug and introspection
void print_derive_context(DeriveMacroContext* ctx);
char* get_derive_macro_info(DeriveMacroType type);

#endif // DERIVE_MACROS_H