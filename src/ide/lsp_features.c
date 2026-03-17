#include "lsp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Helper function
static char* str_dup_safe(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

// =============================================================================
// Diagnostics
// =============================================================================

void lsp_diagnostic_free(LSPDiagnostic* diagnostic) {
    if (!diagnostic) return;
    
    free(diagnostic->code);
    free(diagnostic->source);
    free(diagnostic->message);
    free(diagnostic);
}

void lsp_diagnostic_list_free(LSPDiagnostic* diagnostics) {
    while (diagnostics) {
        LSPDiagnostic* next = diagnostics->next;
        lsp_diagnostic_free(diagnostics);
        diagnostics = next;
    }
}

LSPDiagnostic* lsp_create_diagnostics(LSPDocument* doc, ErrorContext* error_context) {
    if (!doc || !error_context) return NULL;
    
    LSPDiagnostic* first = NULL;
    LSPDiagnostic* last = NULL;
    
    Error* error = error_context->errors;
    while (error) {
        LSPDiagnostic* diagnostic = calloc(1, sizeof(LSPDiagnostic));
        if (!diagnostic) continue;
        
        diagnostic->range = lsp_source_to_lsp_range(&error->location);
        diagnostic->severity = lsp_error_to_diagnostic_severity(error->severity);
        diagnostic->source = str_dup_safe("goo");
        diagnostic->message = str_dup_safe(error->message ? error->message : "Unknown error");
        
        if (error->code != 0) {
            char code_str[32];
            snprintf(code_str, sizeof(code_str), "%d", error->code);
            diagnostic->code = str_dup_safe(code_str);
        }
        
        if (!first) {
            first = last = diagnostic;
        } else {
            last->next = diagnostic;
            last = diagnostic;
        }
        
        error = error->next;
    }
    
    return first;
}

void lsp_publish_diagnostics(LSPServer* server, const char* uri, LSPDiagnostic* diagnostics) {
    if (!server || !uri) return;
    
    // Create diagnostics array
    char diagnostics_json[4096] = "[";
    size_t offset = 1;
    bool first = true;

    LSPDiagnostic* diag = diagnostics;
    while (diag) {
        if (!first && offset < sizeof(diagnostics_json) - 1) {
            offset += snprintf(diagnostics_json + offset,
                               sizeof(diagnostics_json) - offset, ",");
        }
        first = false;

        char* escaped_message = lsp_escape_json_string(diag->message);

        if (offset < sizeof(diagnostics_json) - 1) {
            offset += snprintf(diagnostics_json + offset,
                    sizeof(diagnostics_json) - offset,
                    "{\"range\":{\"start\":{\"line\":%u,\"character\":%u},"
                    "\"end\":{\"line\":%u,\"character\":%u}},"
                    "\"severity\":%d,\"source\":\"goo\",\"message\":%s}",
                    diag->range.start.line, diag->range.start.character,
                    diag->range.end.line, diag->range.end.character,
                    diag->severity, escaped_message ? escaped_message : "\"Unknown error\"");
        }
        free(escaped_message);

        diag = diag->next;
    }
    if (offset < sizeof(diagnostics_json) - 1) {
        snprintf(diagnostics_json + offset, sizeof(diagnostics_json) - offset, "]");
    }
    
    // Create full notification
    char* escaped_uri = lsp_escape_json_string(uri);
    char params[5120];
    snprintf(params, sizeof(params),
            "{\"uri\":%s,\"diagnostics\":%s}",
            escaped_uri ? escaped_uri : "\"\"", diagnostics_json);
    
    lsp_send_notification(server, "textDocument/publishDiagnostics", params);
    
    free(escaped_uri);
}

// =============================================================================
// Completion
// =============================================================================

void lsp_completion_item_free(LSPCompletionItem* item) {
    if (!item) return;
    
    free(item->label);
    free(item->detail);
    free(item->documentation);
    free(item->sort_text);
    free(item->filter_text);
    free(item->insert_text);
    free(item);
}

void lsp_completion_list_free(LSPCompletionItem* items) {
    while (items) {
        LSPCompletionItem* next = items->next;
        lsp_completion_item_free(items);
        items = next;
    }
}

static LSPCompletionItem* create_completion_item(const char* label, 
                                                 LSPCompletionItemKind kind,
                                                 const char* detail,
                                                 const char* documentation) {
    LSPCompletionItem* item = calloc(1, sizeof(LSPCompletionItem));
    if (!item) return NULL;
    
    item->label = str_dup_safe(label);
    item->kind = kind;
    item->detail = str_dup_safe(detail);
    item->documentation = str_dup_safe(documentation);
    item->insert_text = str_dup_safe(label);
    
    return item;
}

LSPCompletionItem* lsp_provide_completion(LSPDocument* doc, LSPPosition position, 
                                          TypeChecker* type_checker) {
    if (!doc || !type_checker) return NULL;
    
    // Ensure document is analyzed
    if (doc->needs_analysis) {
        lsp_document_analyze(doc, type_checker);
    }
    
    LSPCompletionItem* first = NULL;
    LSPCompletionItem* last = NULL;
    
    // Add language keywords
    const char* keywords[] = {
        "fn", "let", "var", "const", "if", "else", "while", "for", 
        "return", "break", "continue", "true", "false", "null",
        "struct", "interface", "enum", "import", "export", "type"
    };
    
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        LSPCompletionItem* item = create_completion_item(
            keywords[i], LSP_COMPLETION_KEYWORD, 
            "Goo keyword", "Built-in language keyword");
        
        if (!item) continue;
        if (!first) {
            first = last = item;
        } else {
            last->next = item;
            last = item;
        }
    }
    
    // Add basic types
    const char* types[] = {
        "int", "float", "string", "bool", "void"
    };
    
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        LSPCompletionItem* item = create_completion_item(
            types[i], LSP_COMPLETION_CLASS,
            "Basic type", "Built-in type");
        
        if (!item) continue;
        if (!first) {
            first = last = item;
        } else {
            last->next = item;
            last = item;
        }
    }
    
    // Add built-in functions
    const char* builtins[] = {
        "print", "println", "len", "sizeof"
    };
    
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        LSPCompletionItem* item = create_completion_item(
            builtins[i], LSP_COMPLETION_FUNCTION,
            "Built-in function", "Built-in function");
        
        if (!item) continue;
        if (!first) {
            first = last = item;
        } else {
            last->next = item;
            last = item;
        }
    }
    
    // TODO: Add context-sensitive completions based on:
    // - Variables in scope
    // - Function parameters
    // - Struct fields
    // - Available methods
    
    return first;
}

// =============================================================================
// Hover
// =============================================================================

void lsp_hover_free(LSPHover* hover) {
    if (!hover) return;
    
    free(hover->contents);
    free(hover->range);
    free(hover);
}

LSPHover* lsp_provide_hover(LSPDocument* doc, LSPPosition position, TypeChecker* type_checker) {
    if (!doc || !type_checker) return NULL;
    
    // Ensure document is analyzed
    if (doc->needs_analysis) {
        lsp_document_analyze(doc, type_checker);
    }
    
    // Convert LSP position to offset
    size_t offset = lsp_position_to_offset(doc->text, &position);
    
    // Find token at position (simplified)
    if (!doc->text || offset >= doc->text_length) {
        return NULL;
    }
    
    // Find word boundaries
    size_t start = offset;
    while (start > 0 && (isalnum(doc->text[start - 1]) || doc->text[start - 1] == '_')) {
        start--;
    }
    
    size_t end = offset;
    while (end < doc->text_length && (isalnum(doc->text[end]) || doc->text[end] == '_')) {
        end++;
    }
    
    if (start >= end) return NULL;
    
    // Extract word
    size_t word_len = end - start;
    char* word = malloc(word_len + 1);
    if (!word) return NULL;
    
    strncpy(word, doc->text + start, word_len);
    word[word_len] = '\0';
    
    // Create hover information
    LSPHover* hover = calloc(1, sizeof(LSPHover));
    if (!hover) {
        free(word);
        return NULL;
    }
    
    // Provide basic hover information
    char contents[512];
    
    // Check if it's a keyword
    const char* keywords[] = {
        "fn", "let", "var", "const", "if", "else", "while", "for", 
        "return", "break", "continue", "true", "false", "null"
    };
    
    bool is_keyword = false;
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (strcmp(word, keywords[i]) == 0) {
            snprintf(contents, sizeof(contents), 
                    "**%s** (keyword)\n\nGoo language keyword", word);
            is_keyword = true;
            break;
        }
    }
    
    if (!is_keyword) {
        // Check if it's a built-in type
        const char* types[] = {"int", "float", "string", "bool", "void"};
        bool is_type = false;
        
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            if (strcmp(word, types[i]) == 0) {
                snprintf(contents, sizeof(contents),
                        "**%s** (type)\n\nBuilt-in type", word);
                is_type = true;
                break;
            }
        }
        
        if (!is_type) {
            // Generic identifier
            snprintf(contents, sizeof(contents),
                    "**%s** (identifier)\n\nLocal identifier", word);
        }
    }
    
    hover->contents = str_dup_safe(contents);
    
    // Set range
    hover->range = malloc(sizeof(LSPRange));
    if (hover->range) {
        hover->range->start = lsp_offset_to_position(doc->text, start);
        hover->range->end = lsp_offset_to_position(doc->text, end);
    }
    
    free(word);
    return hover;
}

// =============================================================================
// Definition
// =============================================================================

void lsp_location_free(LSPLocation* location) {
    if (!location) return;
    
    free(location->uri);
    free(location);
}

void lsp_location_list_free(LSPLocation* locations) {
    while (locations) {
        LSPLocation* next = locations->next;
        lsp_location_free(locations);
        locations = next;
    }
}

LSPLocation* lsp_provide_definition(LSPDocument* doc, LSPPosition position, 
                                    TypeChecker* type_checker) {
    if (!doc || !type_checker) return NULL;
    
    // Ensure document is analyzed
    if (doc->needs_analysis) {
        lsp_document_analyze(doc, type_checker);
    }
    
    // TODO: Implement actual definition lookup
    // This would involve:
    // 1. Finding the symbol at the position
    // 2. Looking up the symbol in the type checker's symbol table
    // 3. Finding the declaration location
    // 4. Creating an LSPLocation for the definition
    
    // For now, return null (no definition found)
    return NULL;
}

// =============================================================================
// References
// =============================================================================

LSPLocation* lsp_provide_references(LSPDocument* doc, LSPPosition position, 
                                    TypeChecker* type_checker, bool include_declaration) {
    (void)include_declaration; // Unused for now
    
    if (!doc || !type_checker) return NULL;
    
    // Ensure document is analyzed
    if (doc->needs_analysis) {
        lsp_document_analyze(doc, type_checker);
    }
    
    // TODO: Implement reference finding
    // This would involve:
    // 1. Finding the symbol at the position
    // 2. Searching through all documents for references
    // 3. Creating LSPLocation for each reference
    
    return NULL;
}

// =============================================================================
// Document Symbols
// =============================================================================

void lsp_symbol_information_free(LSPSymbolInformation* symbol) {
    if (!symbol) return;
    
    free(symbol->name);
    free(symbol->location.uri);
    free(symbol->container_name);
    free(symbol);
}

void lsp_symbol_list_free(LSPSymbolInformation* symbols) {
    while (symbols) {
        LSPSymbolInformation* next = symbols->next;
        lsp_symbol_information_free(symbols);
        symbols = next;
    }
}

static void collect_symbols_from_ast(ASTNode* node, const char* uri, 
                                     LSPSymbolInformation** first, LSPSymbolInformation** last) {
    if (!node || !uri) return;
    
    // Helper function to add symbol
    
    // Process current node
    switch (node->type) {
        case AST_FUNCTION_DECL: {
            FunctionDeclNode* func = (FunctionDeclNode*)node;
            if (func->name && func->name->value) {
                LSPSymbolInformation* symbol = calloc(1, sizeof(LSPSymbolInformation));
                if (symbol) {
                    symbol->name = str_dup_safe(func->name->value);
                    symbol->kind = LSP_SYMBOL_FUNCTION;
                    symbol->location.uri = str_dup_safe(uri);
                    symbol->location.range = lsp_source_to_lsp_range(&node->location);
                    
                    if (!*first) {
                        *first = *last = symbol;
                    } else {
                        (*last)->next = symbol;
                        *last = symbol;
                    }
                }
            }
            break;
        }
        
        case AST_VARIABLE_DECL: {
            VariableDeclNode* var = (VariableDeclNode*)node;
            if (var->name && var->name->value) {
                LSPSymbolInformation* symbol = calloc(1, sizeof(LSPSymbolInformation));
                if (symbol) {
                    symbol->name = str_dup_safe(var->name->value);
                    symbol->kind = LSP_SYMBOL_VARIABLE;
                    symbol->location.uri = str_dup_safe(uri);
                    symbol->location.range = lsp_source_to_lsp_range(&node->location);
                    
                    if (!*first) {
                        *first = *last = symbol;
                    } else {
                        (*last)->next = symbol;
                        *last = symbol;
                    }
                }
            }
            break;
        }
        
        case AST_STRUCT_DECL: {
            StructDeclNode* struct_decl = (StructDeclNode*)node;
            if (struct_decl->name && struct_decl->name->value) {
                LSPSymbolInformation* symbol = calloc(1, sizeof(LSPSymbolInformation));
                if (symbol) {
                    symbol->name = str_dup_safe(struct_decl->name->value);
                    symbol->kind = LSP_SYMBOL_STRUCT;
                    symbol->location.uri = str_dup_safe(uri);
                    symbol->location.range = lsp_source_to_lsp_range(&node->location);
                    
                    if (!*first) {
                        *first = *last = symbol;
                    } else {
                        (*last)->next = symbol;
                        *last = symbol;
                    }
                }
            }
            break;
        }
        
        case AST_CONST_DECL: {
            ConstDeclNode* const_decl = (ConstDeclNode*)node;
            if (const_decl->name && const_decl->name->value) {
                LSPSymbolInformation* symbol = calloc(1, sizeof(LSPSymbolInformation));
                if (symbol) {
                    symbol->name = str_dup_safe(const_decl->name->value);
                    symbol->kind = LSP_SYMBOL_CONSTANT;
                    symbol->location.uri = str_dup_safe(uri);
                    symbol->location.range = lsp_source_to_lsp_range(&node->location);
                    
                    if (!*first) {
                        *first = *last = symbol;
                    } else {
                        (*last)->next = symbol;
                        *last = symbol;
                    }
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    // Recursively process children
    for (size_t i = 0; i < node->child_count; i++) {
        if (node->children[i]) {
            collect_symbols_from_ast(node->children[i], uri, first, last);
        }
    }
}

LSPSymbolInformation* lsp_provide_document_symbols(LSPDocument* doc, TypeChecker* type_checker) {
    if (!doc || !type_checker) return NULL;
    
    // Ensure document is analyzed
    if (doc->needs_analysis) {
        lsp_document_analyze(doc, type_checker);
    }
    
    if (!doc->ast) return NULL;
    
    LSPSymbolInformation* first = NULL;
    LSPSymbolInformation* last = NULL;
    
    collect_symbols_from_ast(doc->ast, doc->uri, &first, &last);
    
    return first;
}