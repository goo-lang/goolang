#include "repl_type_info.h"
#include "repl.h"
#include "types.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// =============================================================================
// Enhanced Type Information System for REPL
// =============================================================================

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN "\033[36m"
#define ANSI_WHITE "\033[37m"

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
// Enhanced Type Formatting with Rich Information
// =============================================================================

char* repl_format_type_detailed(REPLContext* ctx, const Type* type, bool include_docs) {
    if (!ctx || !type) return strdup("unknown");
    
    char buffer[4096];
    buffer[0] = '\0';
    
    // Basic type information
    char* basic_type = repl_format_type(type, false);
    if (basic_type) {
        if (ctx->color_output) {
            snprintf(buffer, sizeof(buffer), "%s%s%s", ANSI_CYAN, basic_type, ANSI_RESET);
        } else {
            strcpy(buffer, basic_type);
        }
        free(basic_type);
    }
    
    // Add size information
    char size_info[128];
    snprintf(size_info, sizeof(size_info), " (size: %zu bytes", type->size);
    
    if (type->align > 0) {
        char align_info[64];
        snprintf(align_info, sizeof(align_info), ", align: %zu", type->align);
        strcat(size_info, align_info);
    }
    strcat(size_info, ")");
    strcat(buffer, size_info);
    
    // Add additional type-specific information
    switch (type->kind) {
        case TYPE_ARRAY:
            if (type->data.array.element_type) {
                char* elem_details = repl_format_type_detailed(ctx, type->data.array.element_type, false);
                char array_info[512];
                snprintf(array_info, sizeof(array_info), "\n  Element type: %s", elem_details);
                strcat(buffer, array_info);
                free(elem_details);
            }
            break;
            
        case TYPE_SLICE:
            if (type->data.slice.element_type) {
                char* elem_details = repl_format_type_detailed(ctx, type->data.slice.element_type, false);
                char slice_info[512];
                snprintf(slice_info, sizeof(slice_info), "\n  Element type: %s", elem_details);
                strcat(buffer, slice_info);
                free(elem_details);
            }
            break;
            
        case TYPE_POINTER:
            if (type->data.pointer.pointee_type) {
                char* pointee_details = repl_format_type_detailed(ctx, type->data.pointer.pointee_type, false);
                char pointer_info[512];
                snprintf(pointer_info, sizeof(pointer_info), "\n  Points to: %s", pointee_details);
                strcat(buffer, pointer_info);
                free(pointee_details);
            }
            break;
            
        case TYPE_ERROR_UNION:
            if (type->data.error_union.value_type) {
                char* value_details = repl_format_type_detailed(ctx, type->data.error_union.value_type, false);
                char error_info[512];
                snprintf(error_info, sizeof(error_info), "\n  Success type: %s", value_details);
                strcat(buffer, error_info);
                free(value_details);
            }
            strcat(buffer, "\n  Error handling: Rust-style error unions");
            break;
            
        case TYPE_NULLABLE:
            if (type->data.nullable.base_type) {
                char* base_details = repl_format_type_detailed(ctx, type->data.nullable.base_type, false);
                char null_info[512];
                snprintf(null_info, sizeof(null_info), "\n  Base type: %s", base_details);
                strcat(buffer, null_info);
                free(base_details);
            }
            strcat(buffer, "\n  Null safety: Enforced at compile time");
            break;
            
        case TYPE_FUNCTION:
            strcat(buffer, "\n  Function properties:");
            strcat(buffer, "\n    - First-class value");
            strcat(buffer, "\n    - Can be stored in variables");
            strcat(buffer, "\n    - Supports closures");
            break;
            
        case TYPE_INTERFACE:
            strcat(buffer, "\n  Interface properties:");
            strcat(buffer, "\n    - Duck typing support");
            strcat(buffer, "\n    - Method set validation");
            break;
            
        default:
            break;
    }
    
    // Add memory layout information for complex types
    if (type->kind == TYPE_STRUCT) {
        strcat(buffer, "\n  Memory layout: Struct with automatic padding");
    }
    
    // Add documentation if requested
    if (include_docs && type->name) {
        char* docs = repl_get_type_documentation(ctx, type->name);
        if (docs) {
            strcat(buffer, "\n  Documentation: ");
            strcat(buffer, docs);
            free(docs);
        }
    }
    
    return strdup(buffer);
}

// =============================================================================
// Type Hierarchy and Relationship Display
// =============================================================================

int repl_print_type_hierarchy_detailed(REPLContext* ctx, const Type* type) {
    if (!ctx || !type) return -1;
    
    repl_printf(ctx, "%sType Hierarchy:%s\n", 
               ctx->color_output ? ANSI_BOLD : "",
               ctx->color_output ? ANSI_RESET : "");
    
    // Show inheritance hierarchy (if applicable)
    if (type->kind == TYPE_STRUCT || type->kind == TYPE_INTERFACE) {
        repl_printf(ctx, "  %s%s%s\n", 
                   ctx->color_output ? ANSI_CYAN : "",
                   type->name ? type->name : "anonymous",
                   ctx->color_output ? ANSI_RESET : "");
        
        // TODO: Add parent/child relationships when implemented
        repl_printf(ctx, "    └─ Base types: (inheritance system pending)\n");
    }
    
    // Show type relationships
    repl_printf(ctx, "\n%sType Relationships:%s\n",
               ctx->color_output ? ANSI_BOLD : "",
               ctx->color_output ? ANSI_RESET : "");
    
    switch (type->kind) {
        case TYPE_ARRAY:
        case TYPE_SLICE:
            repl_printf(ctx, "  • Container type\n");
            repl_printf(ctx, "  • Element access: O(1)\n");
            repl_printf(ctx, "  • Memory: Contiguous layout\n");
            break;
            
        case TYPE_POINTER:
            repl_printf(ctx, "  • Reference type\n");
            repl_printf(ctx, "  • Memory: Indirection overhead\n");
            repl_printf(ctx, "  • Safety: Manual management\n");
            break;
            
        case TYPE_ERROR_UNION:
            repl_printf(ctx, "  • Sum type (Either/Result pattern)\n");
            repl_printf(ctx, "  • Error propagation: Built-in\n");
            repl_printf(ctx, "  • Memory: Tagged union\n");
            break;
            
        case TYPE_NULLABLE:
            repl_printf(ctx, "  • Option type (Maybe pattern)\n");
            repl_printf(ctx, "  • Null safety: Compile-time checked\n");
            repl_printf(ctx, "  • Memory: Optimized layout\n");
            break;
            
        default:
            repl_printf(ctx, "  • Value type\n");
            repl_printf(ctx, "  • Memory: Direct storage\n");
            break;
    }
    
    return 0;
}

// =============================================================================
// Method and Property Information
// =============================================================================

int repl_print_type_methods(REPLContext* ctx, const Type* type) {
    if (!ctx || !type) return -1;
    
    repl_printf(ctx, "%sAvailable Methods:%s\n",
               ctx->color_output ? ANSI_BOLD : "",
               ctx->color_output ? ANSI_RESET : "");
    
    // Built-in methods based on type
    switch (type->kind) {
        case TYPE_STRING:
            repl_printf(ctx, "  • %slen()%s → int - Returns string length\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %ssubstr(start, end)%s → string - Extract substring\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %scontains(substr)%s → bool - Check if contains substring\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            break;
            
        case TYPE_ARRAY:
        case TYPE_SLICE:
            repl_printf(ctx, "  • %slen()%s → int - Returns element count\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %scap()%s → int - Returns capacity\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sappend(elem)%s → slice - Add element\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            break;
            
        case TYPE_ERROR_UNION:
            repl_printf(ctx, "  • %sis_ok()%s → bool - Check if contains value\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sis_err()%s → bool - Check if contains error\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sunwrap()%s → T - Extract value (panics on error)\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sunwrap_or(default)%s → T - Extract value or default\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            break;
            
        case TYPE_NULLABLE:
            repl_printf(ctx, "  • %sis_some()%s → bool - Check if has value\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sis_none()%s → bool - Check if is null\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sunwrap()%s → T - Extract value (panics on null)\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sunwrap_or(default)%s → T - Extract value or default\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            break;
            
        default:
            repl_printf(ctx, "  • %stoString()%s → string - Convert to string representation\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sequals(other)%s → bool - Compare for equality\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            break;
    }
    
    return 0;
}

// =============================================================================
// Type Examples and Usage Patterns
// =============================================================================

int repl_print_type_examples(REPLContext* ctx, const Type* type) {
    if (!ctx || !type) return -1;
    
    repl_printf(ctx, "%sUsage Examples:%s\n",
               ctx->color_output ? ANSI_BOLD : "",
               ctx->color_output ? ANSI_RESET : "");
    
    switch (type->kind) {
        case TYPE_ERROR_UNION:
            repl_printf(ctx, "  %s// Error union declaration and usage%s\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  result: !int = divide(10, 2)\n");
            repl_printf(ctx, "  if result.is_ok() {\n");
            repl_printf(ctx, "      value := result.unwrap()\n");
            repl_printf(ctx, "      goo_printf(\"Result: %%d\\n\", value)\n");
            repl_printf(ctx, "  } else {\n");
            repl_printf(ctx, "      error := result.unwrap_err()\n");
            repl_printf(ctx, "      goo_printf(\"Error: %%s\\n\", error.message)\n");
            repl_printf(ctx, "  }\n");
            break;
            
        case TYPE_NULLABLE:
            repl_printf(ctx, "  %s// Nullable type declaration and usage%s\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  value: ?int = get_optional_value()\n");
            repl_printf(ctx, "  if value.is_some() {\n");
            repl_printf(ctx, "      actual := value.unwrap()\n");
            repl_printf(ctx, "      goo_printf(\"Value: %%d\\n\", actual)\n");
            repl_printf(ctx, "  } else {\n");
            repl_printf(ctx, "      goo_printf(\"No value available\\n\")\n");
            repl_printf(ctx, "  }\n");
            break;
            
        case TYPE_ARRAY:
            repl_printf(ctx, "  %s// Array declaration and usage%s\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  numbers: [5]int = {1, 2, 3, 4, 5}\n");
            repl_printf(ctx, "  for i := 0; i < numbers.len(); i++ {\n");
            repl_printf(ctx, "      goo_printf(\"%%d \", numbers[i])\n");
            repl_printf(ctx, "  }\n");
            break;
            
        case TYPE_SLICE:
            repl_printf(ctx, "  %s// Slice declaration and usage%s\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  numbers: []int = make_slice(int, 0, 10)\n");
            repl_printf(ctx, "  numbers = numbers.append(42)\n");
            repl_printf(ctx, "  goo_printf(\"Length: %%d, Capacity: %%d\\n\", numbers.len(), numbers.cap())\n");
            break;
            
        case TYPE_STRING:
            repl_printf(ctx, "  %s// String operations%s\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  text: string = \"Hello, Goo!\"\n");
            repl_printf(ctx, "  length := text.len()\n");
            repl_printf(ctx, "  substring := text.substr(0, 5)\n");
            repl_printf(ctx, "  contains_goo := text.contains(\"Goo\")\n");
            break;
            
        default:
            repl_printf(ctx, "  %s// Basic usage%s\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  value: %s = default_value\n", 
                       type->name ? type->name : "type");
            repl_printf(ctx, "  goo_printf(\"Value: %%s\\n\", value.toString())\n");
            break;
    }
    
    return 0;
}

// =============================================================================
// Type Documentation System
// =============================================================================

char* repl_get_type_documentation(REPLContext* ctx, const char* type_name) {
    if (!ctx || !type_name) return NULL;
    
    // Built-in type documentation
    if (strcmp(type_name, "int") == 0 || strcmp(type_name, "int32") == 0) {
        return strdup("32-bit signed integer. Range: -2,147,483,648 to 2,147,483,647. Memory efficient for most numeric operations.");
    }
    
    if (strcmp(type_name, "int64") == 0) {
        return strdup("64-bit signed integer. Range: -9,223,372,036,854,775,808 to 9,223,372,036,854,775,807. Use for large numbers or when precision is critical.");
    }
    
    if (strcmp(type_name, "string") == 0) {
        return strdup("UTF-8 encoded string type. Immutable by default. Supports efficient concatenation and substring operations. Memory managed automatically.");
    }
    
    if (strcmp(type_name, "bool") == 0) {
        return strdup("Boolean type with values true or false. Used for logical operations and control flow. Memory optimized (1 byte).");
    }
    
    if (strcmp(type_name, "float32") == 0) {
        return strdup("32-bit IEEE 754 floating point number. Precision: ~7 decimal digits. Use for graphics, scientific computing where memory is a concern.");
    }
    
    if (strcmp(type_name, "float64") == 0) {
        return strdup("64-bit IEEE 754 floating point number. Precision: ~15 decimal digits. Default choice for floating point operations.");
    }
    
    // TODO: Look up documentation from type system or external docs
    return NULL;
}

// =============================================================================
// Performance Characteristics
// =============================================================================

int repl_print_type_performance(REPLContext* ctx, const Type* type) {
    if (!ctx || !type) return -1;
    
    repl_printf(ctx, "%sPerformance Characteristics:%s\n",
               ctx->color_output ? ANSI_BOLD : "",
               ctx->color_output ? ANSI_RESET : "");
    
    switch (type->kind) {
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT32:
        case TYPE_INT64:
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64:
            repl_printf(ctx, "  • %sAccess time:%s O(1) - Direct memory access\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sArithmetic:%s CPU native operations\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sCache friendly:%s Excellent locality\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            break;
            
        case TYPE_ARRAY:
            repl_printf(ctx, "  • %sAccess time:%s O(1) - Array indexing\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sMemory layout:%s Contiguous, cache optimal\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sIteration:%s Excellent performance\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            break;
            
        case TYPE_SLICE:
            repl_printf(ctx, "  • %sAccess time:%s O(1) - Array indexing\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sAppend:%s O(1) amortized (may resize)\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sMemory:%s Dynamic allocation overhead\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            break;
            
        case TYPE_STRING:
            repl_printf(ctx, "  • %sAccess:%s O(1) for length, O(n) for search\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sConcatenation:%s O(n) - Creates new string\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sUTF-8:%s Variable byte encoding\n",
                       ctx->color_output ? ANSI_BLUE : "",
                       ctx->color_output ? ANSI_RESET : "");
            break;
            
        case TYPE_POINTER:
            repl_printf(ctx, "  • %sAccess time:%s O(1) + memory indirection\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sMemory:%s Additional pointer storage\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sCache:%s May reduce locality\n",
                       ctx->color_output ? ANSI_RED : "",
                       ctx->color_output ? ANSI_RESET : "");
            break;
            
        case TYPE_ERROR_UNION:
            repl_printf(ctx, "  • %sAccess:%s O(1) - Tagged union check\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sMemory:%s Size of largest variant + tag\n",
                       ctx->color_output ? ANSI_YELLOW : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sBranching:%s Predictable for error paths\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            break;
            
        case TYPE_NULLABLE:
            repl_printf(ctx, "  • %sAccess:%s O(1) - Null check optimization\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sMemory:%s Minimal overhead (often optimized)\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            repl_printf(ctx, "  • %sSafety:%s Compile-time null checking\n",
                       ctx->color_output ? ANSI_GREEN : "",
                       ctx->color_output ? ANSI_RESET : "");
            break;
            
        default:
            repl_printf(ctx, "  • Performance characteristics vary by implementation\n");
            break;
    }
    
    return 0;
}

// =============================================================================
// Enhanced Type Inspection Command
// =============================================================================

int repl_cmd_inspect_type(REPLContext* ctx, const char* args) {
    if (!ctx || !args) return -1;
    
    // Extract type name or expression from args
    const char* expr_start = args;
    while (*expr_start && !isspace(*expr_start)) expr_start++; // Skip command
    while (isspace(*expr_start)) expr_start++; // Skip whitespace
    
    if (!*expr_start) {
        repl_error_printf(ctx, "Usage: :inspect <type_or_expression>\n");
        repl_error_printf(ctx, "Examples:\n");
        repl_error_printf(ctx, "  :inspect int\n");
        repl_error_printf(ctx, "  :inspect []string\n");
        repl_error_printf(ctx, "  :inspect !int\n");
        repl_error_printf(ctx, "  :inspect variable_name\n");
        return -1;
    }
    
    // Try to infer the type
    Type* type = repl_infer_type(ctx, expr_start);
    if (!type) {
        repl_error_printf(ctx, "Failed to determine type for: %s\n", expr_start);
        return -1;
    }
    
    // Print comprehensive type information
    repl_printf(ctx, "%s=== Type Information ===%s\n",
               ctx->color_output ? ANSI_BOLD ANSI_BLUE : "",
               ctx->color_output ? ANSI_RESET : "");
    
    char* detailed_type = repl_format_type_detailed(ctx, type, true);
    repl_printf(ctx, "%s\n", detailed_type);
    free(detailed_type);
    
    repl_printf(ctx, "\n");
    repl_print_type_hierarchy_detailed(ctx, type);
    
    repl_printf(ctx, "\n");
    repl_print_type_methods(ctx, type);
    
    repl_printf(ctx, "\n");
    repl_print_type_performance(ctx, type);
    
    repl_printf(ctx, "\n");
    repl_print_type_examples(ctx, type);
    
    return 0;
}

// =============================================================================
// Live Type Information for Interactive Evaluation
// =============================================================================

void repl_print_live_type_info(REPLContext* ctx, const char* expression, const Type* type) {
    if (!ctx || !expression || !type) return;
    
    if (!ctx->show_types) return;
    
    // Compact type display for live evaluation
    char* type_str = repl_format_type(type, false);
    if (type_str) {
        repl_printf(ctx, "%s// %s: %s%s\n",
                   ctx->color_output ? ANSI_BLUE : "",
                   expression,
                   type_str,
                   ctx->color_output ? ANSI_RESET : "");
        free(type_str);
    }
    
    // Show additional hints for complex types
    if (type->kind == TYPE_ERROR_UNION) {
        repl_printf(ctx, "%s// Hint: Use .is_ok() or .is_err() to check result%s\n",
                   ctx->color_output ? ANSI_YELLOW : "",
                   ctx->color_output ? ANSI_RESET : "");
    } else if (type->kind == TYPE_NULLABLE) {
        repl_printf(ctx, "%s// Hint: Use .is_some() or .is_none() to check value%s\n",
                   ctx->color_output ? ANSI_YELLOW : "",
                   ctx->color_output ? ANSI_RESET : "");
    }
}

// =============================================================================
// Type-aware Auto-completion Enhancements
// =============================================================================

REPLCompletion* repl_complete_type_members(REPLContext* ctx, const Type* type, const char* prefix) {
    if (!ctx || !type || !prefix) return NULL;
    
    REPLCompletion* completions = NULL;
    REPLCompletion* last = NULL;
    
    // Add method completions based on type
    const char** methods = NULL;
    int method_count = 0;
    
    switch (type->kind) {
        case TYPE_STRING: {
            static const char* string_methods[] = {"len", "substr", "contains", "starts_with", "ends_with"};
            methods = string_methods;
            method_count = 5;
            break;
        }
        case TYPE_ARRAY:
        case TYPE_SLICE: {
            static const char* array_methods[] = {"len", "cap", "append", "get", "set"};
            methods = array_methods;
            method_count = 5;
            break;
        }
        case TYPE_ERROR_UNION: {
            static const char* error_methods[] = {"is_ok", "is_err", "unwrap", "unwrap_or", "unwrap_err"};
            methods = error_methods;
            method_count = 5;
            break;
        }
        case TYPE_NULLABLE: {
            static const char* nullable_methods[] = {"is_some", "is_none", "unwrap", "unwrap_or"};
            methods = nullable_methods;
            method_count = 4;
            break;
        }
        default:
            break;
    }
    
    // Add matching methods to completion list
    size_t prefix_len = strlen(prefix);
    for (int i = 0; i < method_count; i++) {
        if (strncmp(methods[i], prefix, prefix_len) == 0) {
            REPLCompletion* comp = calloc(1, sizeof(REPLCompletion));
            if (comp) {
                comp->text = strdup(methods[i]);
                
                // Create description based on method
                char desc[256];
                snprintf(desc, sizeof(desc), "Method of %s", 
                        type->name ? type->name : "type");
                comp->description = strdup(desc);
                
                // Add to list
                if (!completions) {
                    completions = comp;
                    last = comp;
                } else {
                    last->next = comp;
                    last = comp;
                }
            }
        }
    }
    
    return completions;
}