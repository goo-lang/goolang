#ifndef GOO_MACROS_H
#define GOO_MACROS_H

#include "ast.h"
#include "types.h"
#include "comptime.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// Derive Macros — auto-generate trait implementations
// =============================================================================

typedef enum {
    DERIVE_DEBUG,                  // Generate debug string representation
    DERIVE_CLONE,                  // Generate deep copy
    DERIVE_HASH,                   // Generate hash function
    DERIVE_EQUAL,                  // Generate equality comparison
    DERIVE_SERIALIZE,              // Generate serialization
    DERIVE_DESERIALIZE,            // Generate deserialization
    DERIVE_DEFAULT,                // Generate default constructor
    DERIVE_CUSTOM,                 // User-defined derive
} DeriveKind;

typedef struct DeriveSpec {
    DeriveKind kind;
    const char* custom_name;       // For DERIVE_CUSTOM
    struct DeriveSpec* next;
} DeriveSpec;

// =============================================================================
// Template Macros — pattern-based code generation
// =============================================================================

typedef struct TemplateParam {
    char* name;                    // Parameter name (e.g., "T")
    char* filter;                  // Optional filter (e.g., "lowercase")
    struct TemplateParam* next;
} TemplateParam;

typedef struct TemplateMacro {
    char* name;                    // Macro name
    char* body_template;           // Template body with {{placeholders}}
    TemplateParam* params;
    size_t param_count;
} TemplateMacro;

// =============================================================================
// Macro Expansion Result
// =============================================================================

typedef struct MacroExpansion {
    ASTNode** generated_nodes;     // Generated AST nodes
    size_t node_count;
    size_t node_capacity;

    // Source mapping for debugging
    Position expansion_site;       // Where the macro was invoked
    const char* macro_name;        // Which macro was expanded
} MacroExpansion;

// =============================================================================
// Macro System
// =============================================================================

typedef struct MacroSystem {
    // Registered template macros
    TemplateMacro** templates;
    size_t template_count;
    size_t template_capacity;

    // Comptime integration
    ComptimeInterpreter* comptime;

    // Safety
    size_t max_expansion_depth;    // Prevent infinite recursion
    size_t current_depth;
    bool enforce_hygiene;          // Variable capture prevention

    // Statistics
    struct {
        size_t derives_expanded;
        size_t templates_expanded;
        size_t nodes_generated;
        size_t errors;
    } stats;
} MacroSystem;

// =============================================================================
// API
// =============================================================================

// Lifecycle
MacroSystem* macro_system_new(ComptimeInterpreter* comptime);
void macro_system_free(MacroSystem* ms);

// Derive macros
DeriveSpec* derive_parse_annotation(const char* args);
void derive_spec_free(DeriveSpec* spec);
MacroExpansion* derive_expand(MacroSystem* ms, DeriveSpec* spec, ASTNode* target_struct);

// Template macros
TemplateMacro* template_macro_new(const char* name, const char* body);
void template_macro_free(TemplateMacro* tmpl);
void macro_system_register_template(MacroSystem* ms, TemplateMacro* tmpl);
char* template_substitute(const char* tmpl, const char* param, const char* value);

// Expansion
MacroExpansion* macro_expansion_new(Position site, const char* macro_name);
void macro_expansion_free(MacroExpansion* exp);
void macro_expansion_add_node(MacroExpansion* exp, ASTNode* node);

// Template string utilities
char* template_apply_filter(const char* value, const char* filter);

#endif // GOO_MACROS_H
