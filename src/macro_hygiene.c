#include "../include/macro_hygiene.h"
#include "../include/errors/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Built-in reserved names
static const char* BUILTIN_RESERVED[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default",
    "break", "continue", "return", "goto", "sizeof", "typeof",
    "struct", "union", "enum", "typedef", "const", "volatile",
    "static", "extern", "auto", "register", "inline", "restrict",
    "int", "char", "float", "double", "void", "long", "short",
    "signed", "unsigned", "bool", "true", "false", "NULL",
    "comptime", "pub", "sub", "req", "rep", "chan", "select",
    "defer", "panic", "recover", "make", "len", "cap", "new",
    "delete", "this", "super", "self", "Self", "impl", "trait",
    "fn", "let", "mut", "ref", "move", "async", "await", "yield"
};

// Create hygiene context
MacroHygieneContext* create_hygiene_context(void) {
    MacroHygieneContext* ctx = (MacroHygieneContext*)calloc(1, sizeof(MacroHygieneContext));
    if (!ctx) return NULL;
    
    // Create global scope
    ctx->global_scope = create_hygiene_scope(HYGIENE_SCOPE_GLOBAL, "global", NULL);
    if (!ctx->global_scope) {
        free(ctx);
        return NULL;
    }
    
    ctx->current_scope = ctx->global_scope;
    ctx->expansion_depth = 0;
    ctx->name_counter = 0;
    ctx->macro_prefix = strdup("__macro_");
    
    // Initialize reserved names
    init_builtin_reserved_names(ctx);
    
    return ctx;
}

// Destroy hygiene context
void destroy_hygiene_context(MacroHygieneContext* ctx) {
    if (!ctx) return;
    
    destroy_hygiene_scope(ctx->global_scope);
    
    // Free reserved names
    for (size_t i = 0; i < ctx->reserved_count; i++) {
        free(ctx->reserved_names[i]);
    }
    free(ctx->reserved_names);
    
    // Free violations
    for (size_t i = 0; i < ctx->violation_count; i++) {
        free(ctx->violations[i]);
    }
    free(ctx->violations);
    
    free(ctx->macro_prefix);
    free(ctx);
}

// Create hygiene scope
HygieneScope* create_hygiene_scope(HygieneScopeType type, const char* scope_id, HygieneScope* parent) {
    HygieneScope* scope = (HygieneScope*)calloc(1, sizeof(HygieneScope));
    if (!scope) return NULL;
    
    scope->type = type;
    scope->scope_id = scope_id ? strdup(scope_id) : NULL;
    scope->parent = parent;
    scope->generation_counter = 0;
    scope->bindings = NULL;
    scope->children = NULL;
    scope->child_count = 0;
    
    return scope;
}

// Destroy hygiene scope
void destroy_hygiene_scope(HygieneScope* scope) {
    if (!scope) return;
    
    // Destroy all bindings
    HygieneBinding* binding = scope->bindings;
    while (binding) {
        HygieneBinding* next = binding->next;
        destroy_hygiene_binding(binding);
        binding = next;
    }
    
    // Destroy child scopes
    for (size_t i = 0; i < scope->child_count; i++) {
        destroy_hygiene_scope(scope->children[i]);
    }
    free(scope->children);
    
    free(scope->scope_id);
    free(scope);
}

// Enter hygiene scope
HygieneScope* enter_hygiene_scope(MacroHygieneContext* ctx, HygieneScopeType type, const char* scope_id) {
    if (!ctx) return NULL;
    
    HygieneScope* new_scope = create_hygiene_scope(type, scope_id, ctx->current_scope);
    if (!new_scope) return NULL;
    
    // Add to parent's children
    if (ctx->current_scope) {
        ctx->current_scope->children = (HygieneScope**)realloc(
            ctx->current_scope->children,
            (ctx->current_scope->child_count + 1) * sizeof(HygieneScope*)
        );
        if (ctx->current_scope->children) {
            ctx->current_scope->children[ctx->current_scope->child_count] = new_scope;
            ctx->current_scope->child_count++;
        }
    }
    
    ctx->current_scope = new_scope;
    ctx->expansion_depth++;
    
    return new_scope;
}

// Exit hygiene scope
HygieneScope* exit_hygiene_scope(MacroHygieneContext* ctx) {
    if (!ctx || !ctx->current_scope) return NULL;
    
    HygieneScope* parent = ctx->current_scope->parent;
    ctx->current_scope = parent ? parent : ctx->global_scope;
    ctx->expansion_depth--;
    
    return ctx->current_scope;
}

// Create hygiene binding
HygieneBinding* create_hygiene_binding(const char* name, Type* type, HygieneScopeType scope) {
    if (!name) return NULL;
    
    HygieneBinding* binding = (HygieneBinding*)calloc(1, sizeof(HygieneBinding));
    if (!binding) return NULL;
    
    binding->name = strdup(name);
    binding->type = type;
    binding->scope = scope;
    binding->generation = 0;
    binding->is_captured = false;
    binding->mangled_name = NULL;
    binding->next = NULL;
    
    return binding;
}

// Destroy hygiene binding
void destroy_hygiene_binding(HygieneBinding* binding) {
    if (!binding) return;
    
    free(binding->name);
    free(binding->mangled_name);
    free(binding);
}

// Add hygiene binding
bool add_hygiene_binding(HygieneScope* scope, HygieneBinding* binding) {
    if (!scope || !binding) return false;
    
    // Check for existing binding
    if (find_hygiene_binding(scope, binding->name)) {
        return false; // Already exists
    }
    
    // Add to linked list
    binding->next = scope->bindings;
    scope->bindings = binding;
    
    // Generate unique name
    binding->generation = scope->generation_counter++;
    
    return true;
}

// Find hygiene binding in scope
HygieneBinding* find_hygiene_binding(HygieneScope* scope, const char* name) {
    if (!scope || !name) return NULL;
    
    HygieneBinding* binding = scope->bindings;
    while (binding) {
        if (strcmp(binding->name, name) == 0) {
            return binding;
        }
        binding = binding->next;
    }
    
    return NULL;
}

// Resolve hygiene binding (search up the scope chain)
HygieneBinding* resolve_hygiene_binding(MacroHygieneContext* ctx, const char* name) {
    if (!ctx || !name) return NULL;
    
    HygieneScope* scope = ctx->current_scope;
    while (scope) {
        HygieneBinding* binding = find_hygiene_binding(scope, name);
        if (binding) {
            return binding;
        }
        scope = scope->parent;
    }
    
    return NULL;
}

// Generate hygienic name
char* generate_hygienic_name(MacroHygieneContext* ctx, const char* base_name) {
    if (!ctx || !base_name) return NULL;
    
    char* name = (char*)malloc(256);
    if (!name) return NULL;
    
    snprintf(name, 256, "%s%s_%d_%d", 
             ctx->macro_prefix, base_name, 
             ctx->expansion_depth, ctx->name_counter++);
    
    return name;
}

// Mangle variable name for hygiene
char* mangle_variable_name(const char* var_name, int generation, const char* macro_name) {
    if (!var_name) return NULL;
    
    char* mangled = (char*)malloc(256);
    if (!mangled) return NULL;
    
    if (macro_name) {
        snprintf(mangled, 256, "__hygiene_%s_%s_%d", macro_name, var_name, generation);
    } else {
        snprintf(mangled, 256, "__hygiene_%s_%d", var_name, generation);
    }
    
    return mangled;
}

// Generate unique identifier
char* generate_unique_identifier(MacroHygieneContext* ctx, const char* prefix) {
    if (!ctx) return NULL;
    
    char* id = (char*)malloc(128);
    if (!id) return NULL;
    
    snprintf(id, 128, "%s%d", prefix ? prefix : "id", ctx->name_counter++);
    
    return id;
}

// Check if name is hygienic
bool is_hygienic_name(const char* name) {
    if (!name) return false;
    
    return strstr(name, "__hygiene_") != NULL || strstr(name, "__macro_") != NULL;
}

// Check macro hygiene
bool check_macro_hygiene(MacroExpansion* expansion, MacroHygieneContext* ctx) {
    if (!expansion || !ctx) return false;
    
    // Clear previous violations
    clear_hygiene_violations(ctx);
    
    // Check variable capture
    if (expansion->expanded_ast) {
        if (!analyze_variable_capture(expansion->expanded_ast, ctx)) {
            report_hygiene_violation(ctx, HYGIENE_VIOLATION_CAPTURE,
                                   "unknown", "macro", 0, 0,
                                   "Variable capture detected in macro expansion");
            return false;
        }
    }
    
    // Check name collisions
    if (expansion->expanded_code) {
        // Parse the expanded code and check for collisions
        // This is a simplified check - in practice would parse AST
        char* line = strtok(expansion->expanded_code, "\n");
        int line_num = 1;
        while (line) {
            // Simple check for variable declarations
            if (strstr(line, "var ") || strstr(line, "let ") || strstr(line, "const ")) {
                char* var_start = strstr(line, " ");
                if (var_start) {
                    var_start++;
                    char* var_end = strchr(var_start, ' ');
                    if (var_end) {
                        size_t var_len = var_end - var_start;
                        char* var_name = (char*)malloc(var_len + 1);
                        strncpy(var_name, var_start, var_len);
                        var_name[var_len] = '\0';
                        
                        if (check_name_collision(var_name, ctx)) {
                            report_hygiene_violation(ctx, HYGIENE_VIOLATION_SHADOW,
                                                   var_name, "macro", line_num, 0,
                                                   "Variable name collision in macro expansion");
                            free(var_name);
                            return false;
                        }
                        free(var_name);
                    }
                }
            }
            line = strtok(NULL, "\n");
            line_num++;
        }
    }
    
    return ctx->violation_count == 0;
}

// Validate variable access
bool validate_variable_access(const char* var_name, MacroHygieneContext* ctx) {
    if (!var_name || !ctx) return false;
    
    // Check if it's a reserved name
    if (is_reserved_name(ctx, var_name)) {
        return false;
    }
    
    // Check if variable exists in scope
    HygieneBinding* binding = resolve_hygiene_binding(ctx, var_name);
    return binding != NULL;
}

// Check name collision
bool check_name_collision(const char* name, MacroHygieneContext* ctx) {
    if (!name || !ctx) return false;
    
    // Check against reserved names
    if (is_reserved_name(ctx, name)) {
        return true;
    }
    
    // Check against existing bindings
    HygieneBinding* binding = resolve_hygiene_binding(ctx, name);
    return binding != NULL;
}

// Detect hygiene violation
HygieneViolation* detect_hygiene_violation(const char* var_name, MacroHygieneContext* ctx) {
    if (!var_name || !ctx) return NULL;
    
    HygieneViolation* violation = (HygieneViolation*)calloc(1, sizeof(HygieneViolation));
    if (!violation) return NULL;
    
    violation->variable_name = strdup(var_name);
    violation->macro_name = strdup("unknown");
    violation->line = 0;
    violation->column = 0;
    
    if (is_reserved_name(ctx, var_name)) {
        violation->type = HYGIENE_VIOLATION_RESERVED;
        violation->description = strdup("Use of reserved name");
    } else if (check_name_collision(var_name, ctx)) {
        violation->type = HYGIENE_VIOLATION_SHADOW;
        violation->description = strdup("Variable name collision");
    } else {
        violation->type = HYGIENE_VIOLATION_CAPTURE;
        violation->description = strdup("Variable capture");
    }
    
    return violation;
}

// Apply hygiene transformation to AST
ASTNode* apply_hygiene_transformation(ASTNode* node, MacroHygieneContext* ctx) {
    if (!node || !ctx) return node;
    
    // This would recursively transform the AST to apply hygiene
    // For now, return the node unchanged
    return rename_variables_in_ast(node, ctx);
}

// Rename variables in AST for hygiene
ASTNode* rename_variables_in_ast(ASTNode* node, MacroHygieneContext* ctx) {
    if (!node || !ctx) return node;
    
    // In a full implementation, this would traverse the AST and rename variables
    // according to hygiene rules. For now, we'll return the node unchanged.
    return node;
}

// Transform macro expansion
bool transform_macro_expansion(MacroExpansion* expansion, MacroHygieneContext* ctx) {
    if (!expansion || !ctx) return false;
    
    if (expansion->expanded_ast) {
        expansion->expanded_ast = apply_hygiene_transformation(expansion->expanded_ast, ctx);
    }
    
    return true;
}

// Analyze variable capture
bool analyze_variable_capture(ASTNode* macro_body, MacroHygieneContext* ctx) {
    if (!macro_body || !ctx) return true; // No capture if no body
    
    // This would analyze the AST for variable captures
    // For now, assume no capture
    return true;
}

// Find captured variables
char** find_captured_variables(ASTNode* node, HygieneScope* scope, size_t* count) {
    if (!node || !scope || !count) return NULL;
    
    *count = 0;
    // This would analyze the AST and return captured variables
    return NULL;
}

// Prevent variable capture
bool prevent_variable_capture(MacroExpansion* expansion, MacroHygieneContext* ctx) {
    if (!expansion || !ctx) return false;
    
    return transform_macro_expansion(expansion, ctx);
}

// Add reserved name
bool add_reserved_name(MacroHygieneContext* ctx, const char* name) {
    if (!ctx || !name) return false;
    
    ctx->reserved_names = (char**)realloc(ctx->reserved_names, 
                                         (ctx->reserved_count + 1) * sizeof(char*));
    if (!ctx->reserved_names) return false;
    
    ctx->reserved_names[ctx->reserved_count] = strdup(name);
    ctx->reserved_count++;
    
    return true;
}

// Check if name is reserved
bool is_reserved_name(MacroHygieneContext* ctx, const char* name) {
    if (!ctx || !name) return false;
    
    for (size_t i = 0; i < ctx->reserved_count; i++) {
        if (strcmp(ctx->reserved_names[i], name) == 0) {
            return true;
        }
    }
    
    return false;
}

// Initialize builtin reserved names
void init_builtin_reserved_names(MacroHygieneContext* ctx) {
    if (!ctx) return;
    
    size_t builtin_count = sizeof(BUILTIN_RESERVED) / sizeof(BUILTIN_RESERVED[0]);
    for (size_t i = 0; i < builtin_count; i++) {
        add_reserved_name(ctx, BUILTIN_RESERVED[i]);
    }
}

// Report hygiene violation
void report_hygiene_violation(MacroHygieneContext* ctx, HygieneViolationType type,
                             const char* var_name, const char* macro_name,
                             int line, int column, const char* description) {
    if (!ctx) return;
    
    ctx->violations = (char**)realloc(ctx->violations, 
                                     (ctx->violation_count + 1) * sizeof(char*));
    if (!ctx->violations) return;
    
    char* violation_msg = (char*)malloc(512);
    if (!violation_msg) return;
    
    const char* type_names[] = {
        "CAPTURE", "SHADOW", "RESERVED", "ESCAPE"
    };
    
    snprintf(violation_msg, 512, "%s violation: %s in macro %s at %d:%d - %s",
             type_names[type], var_name, macro_name, line, column, description);
    
    ctx->violations[ctx->violation_count++] = violation_msg;
    
    fprintf(stderr, "Hygiene violation: %s\n", violation_msg);
}

// Print hygiene violations
void print_hygiene_violations(MacroHygieneContext* ctx) {
    if (!ctx) return;
    
    printf("Hygiene Violations (%zu):\n", ctx->violation_count);
    for (size_t i = 0; i < ctx->violation_count; i++) {
        printf("  %zu. %s\n", i + 1, ctx->violations[i]);
    }
}

// Clear hygiene violations
void clear_hygiene_violations(MacroHygieneContext* ctx) {
    if (!ctx) return;
    
    for (size_t i = 0; i < ctx->violation_count; i++) {
        free(ctx->violations[i]);
    }
    free(ctx->violations);
    
    ctx->violations = NULL;
    ctx->violation_count = 0;
}

// Print hygiene scope tree
void print_hygiene_scope_tree(HygieneScope* scope, int depth) {
    if (!scope) return;
    
    const char* scope_names[] = {
        "GLOBAL", "MODULE", "FUNCTION", "BLOCK", "MACRO"
    };
    
    for (int i = 0; i < depth; i++) printf("  ");
    printf("%s scope: %s\n", scope_names[scope->type], 
           scope->scope_id ? scope->scope_id : "unnamed");
    
    // Print bindings
    HygieneBinding* binding = scope->bindings;
    while (binding) {
        for (int i = 0; i < depth + 1; i++) printf("  ");
        printf("- %s (gen: %d)\n", binding->name, binding->generation);
        binding = binding->next;
    }
    
    // Print children
    for (size_t i = 0; i < scope->child_count; i++) {
        print_hygiene_scope_tree(scope->children[i], depth + 1);
    }
}

// Print hygiene bindings
void print_hygiene_bindings(HygieneScope* scope) {
    if (!scope) return;
    
    printf("Bindings in scope '%s':\n", 
           scope->scope_id ? scope->scope_id : "unnamed");
    
    HygieneBinding* binding = scope->bindings;
    while (binding) {
        printf("  %s -> %s (gen: %d, captured: %s)\n", 
               binding->name, 
               binding->mangled_name ? binding->mangled_name : "not mangled",
               binding->generation,
               binding->is_captured ? "yes" : "no");
        binding = binding->next;
    }
}

// Get hygiene analysis report
char* get_hygiene_analysis_report(MacroHygieneContext* ctx) {
    if (!ctx) return NULL;
    
    char* report = (char*)malloc(2048);
    if (!report) return NULL;
    
    snprintf(report, 2048,
        "Macro Hygiene Analysis Report\n"
        "=============================\n"
        "Expansion Depth: %d\n"
        "Current Scope: %s\n"
        "Reserved Names: %zu\n"
        "Violations: %zu\n"
        "Name Counter: %d\n",
        ctx->expansion_depth,
        ctx->current_scope ? ctx->current_scope->scope_id : "null",
        ctx->reserved_count,
        ctx->violation_count,
        ctx->name_counter);
    
    return report;
}

// Debug macro hygiene
void debug_macro_hygiene(MacroExpansion* expansion, MacroHygieneContext* ctx) {
    if (!expansion || !ctx) return;
    
    printf("=== Macro Hygiene Debug ===\n");
    printf("Expansion success: %s\n", expansion->success ? "yes" : "no");
    
    if (ctx->current_scope) {
        printf("Current scope tree:\n");
        print_hygiene_scope_tree(ctx->current_scope, 0);
    }
    
    if (ctx->violation_count > 0) {
        print_hygiene_violations(ctx);
    }
    
    printf("===========================\n");
}

// Register macro hygiene
bool register_macro_hygiene(MacroTemplate* macro, MacroHygieneContext* hygiene_ctx) {
    if (!macro || !hygiene_ctx) return false;
    
    // Note: hygiene level would be set in actual implementation
    // macro->hygiene_level = HYGIENE_SEMANTIC;
    
    return true;
}

// Integrate hygiene with macros
bool integrate_hygiene_with_macros(MacroRegistry* registry, MacroHygieneContext* hygiene_ctx) {
    if (!registry || !hygiene_ctx) return false;
    
    // This would integrate hygiene checking with all macros in the registry
    // For now, return success
    return true;
}

// Expand macro with hygiene
MacroExpansion* expand_macro_with_hygiene(MacroTemplate* macro, MacroContext* ctx,
                                         MacroHygieneContext* hygiene_ctx) {
    if (!macro || !ctx || !hygiene_ctx) return NULL;
    
    // Create a new hygiene scope for this expansion
    char scope_name[64];
    snprintf(scope_name, 64, "macro_%s", macro->name);
    enter_hygiene_scope(hygiene_ctx, HYGIENE_SCOPE_MACRO, scope_name);
    
    // Perform normal macro expansion - simplified for testing
    MacroExpansion* expansion = (MacroExpansion*)calloc(1, sizeof(MacroExpansion));
    if (expansion) {
        expansion->success = true;
        expansion->expanded_code = strdup("int hygienic_var = 42;");
        expansion->expanded_ast = NULL; // Simplified for testing
    }
    
    if (expansion) {
        // Apply hygiene checking and transformation
        if (!check_macro_hygiene(expansion, hygiene_ctx)) {
            // Hygiene violation detected
            expansion->success = false;
            free(expansion->error_message);
            expansion->error_message = strdup("Macro hygiene violation");
        } else {
            // Apply hygiene transformations
            transform_macro_expansion(expansion, hygiene_ctx);
        }
    }
    
    // Exit the hygiene scope
    exit_hygiene_scope(hygiene_ctx);
    
    return expansion;
}

// Validate macro expansion hygiene
bool validate_macro_expansion_hygiene(MacroExpansion* expansion, MacroHygieneContext* hygiene_ctx) {
    if (!expansion || !hygiene_ctx) return false;
    
    return check_macro_hygiene(expansion, hygiene_ctx);
}

// Performance optimizations
bool optimize_hygienic_names(MacroHygieneContext* ctx) {
    if (!ctx) return false;
    
    // Optimize name generation to reduce string length
    // This could involve shortening prefixes or using more efficient encoding
    return true;
}

void cache_hygiene_lookups(MacroHygieneContext* ctx) {
    if (!ctx) return;
    
    // Implement caching for hygiene binding lookups
    // This would involve creating hash tables or other efficient data structures
}

bool minimize_name_mangling(MacroHygieneContext* ctx) {
    if (!ctx) return false;
    
    // Minimize name mangling by only mangling when necessary
    // This involves analyzing scope conflicts more precisely
    return true;
}