#include "lsp_server.h"
#include "lexer.h"
#include "parser.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>

// =============================================================================
// Logging and utilities
// =============================================================================

static bool lsp_logging_enabled = false;
static FILE* lsp_log_file = NULL;

static void lsp_log_message(const char* level, const char* format, va_list args) {
    if (!lsp_logging_enabled) return;
    
    FILE* output = lsp_log_file ? lsp_log_file : stderr;
    time_t now = time(NULL);
    char* time_str = ctime(&now);
    if (time_str) {
        time_str[strlen(time_str) - 1] = '\0'; // Remove newline
    }
    
    fprintf(output, "[%s] %s: ", time_str ? time_str : "unknown", level);
    vfprintf(output, format, args);
    fprintf(output, "\n");
    fflush(output);
}

void lsp_log_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    lsp_log_message("ERROR", format, args);
    va_end(args);
}

void lsp_log_info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    lsp_log_message("INFO", format, args);
    va_end(args);
}

void lsp_log_debug(const char* format, ...) {
    va_list args;
    va_start(args, format);
    lsp_log_message("DEBUG", format, args);
    va_end(args);
}

// =============================================================================
// String utilities
// =============================================================================

static char* str_dup_safe(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

char* lsp_escape_json_string(const char* str) {
    if (!str) return str_dup_safe("\"\"");
    
    size_t len = strlen(str);
    char* escaped = malloc(len * 2 + 3); // Worst case: all chars need escaping + quotes + null
    if (!escaped) return NULL;
    
    char* p = escaped;
    *p++ = '"';
    
    for (const char* s = str; *s; s++) {
        switch (*s) {
            case '"':  *p++ = '\\'; *p++ = '"'; break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\b': *p++ = '\\'; *p++ = 'b'; break;
            case '\f': *p++ = '\\'; *p++ = 'f'; break;
            case '\n': *p++ = '\\'; *p++ = 'n'; break;
            case '\r': *p++ = '\\'; *p++ = 'r'; break;
            case '\t': *p++ = '\\'; *p++ = 't'; break;
            default:   *p++ = *s; break;
        }
    }
    
    *p++ = '"';
    *p = '\0';
    
    return escaped;
}

char* lsp_uri_to_path(const char* uri) {
    if (!uri) return NULL;
    
    // Simple file:// URI handling
    if (strncmp(uri, "file://", 7) == 0) {
        return str_dup_safe(uri + 7);
    }
    
    return str_dup_safe(uri);
}

char* lsp_path_to_uri(const char* path) {
    if (!path) return NULL;
    
    size_t len = strlen(path) + 8; // "file://" + path + null
    char* uri = malloc(len);
    if (uri) {
        snprintf(uri, len, "file://%s", path);
    }
    return uri;
}

bool lsp_uri_is_file(const char* uri) {
    return uri && strncmp(uri, "file://", 7) == 0;
}

// =============================================================================
// Position utilities
// =============================================================================

LSPPosition lsp_source_to_lsp_position(const SourceLocation* source_loc) {
    LSPPosition pos = {0, 0};
    if (source_loc) {
        pos.line = source_loc->line > 0 ? (uint32_t)(source_loc->line - 1) : 0; // LSP is 0-based
        pos.character = source_loc->column > 0 ? (uint32_t)(source_loc->column - 1) : 0;
    }
    return pos;
}

LSPRange lsp_source_to_lsp_range(const SourceLocation* source_loc) {
    LSPRange range;
    range.start = lsp_source_to_lsp_position(source_loc);
    range.end = range.start;
    if (source_loc && source_loc->length > 0) {
        range.end.character += (uint32_t)source_loc->length;
    }
    return range;
}

SourceLocation lsp_position_to_source(const LSPPosition* lsp_pos, const char* filename) {
    SourceLocation loc = {0};
    if (lsp_pos) {
        loc.filename = filename;
        loc.line = lsp_pos->line + 1; // LSP is 0-based, SourceLocation is 1-based
        loc.column = lsp_pos->character + 1;
    }
    return loc;
}

int lsp_position_compare(const LSPPosition* a, const LSPPosition* b) {
    if (!a || !b) return 0;
    
    if (a->line < b->line) return -1;
    if (a->line > b->line) return 1;
    if (a->character < b->character) return -1;
    if (a->character > b->character) return 1;
    return 0;
}

bool lsp_position_in_range(const LSPPosition* pos, const LSPRange* range) {
    if (!pos || !range) return false;
    
    return lsp_position_compare(pos, &range->start) >= 0 &&
           lsp_position_compare(pos, &range->end) <= 0;
}

LSPPosition lsp_offset_to_position(const char* text, size_t offset) {
    LSPPosition pos = {0, 0};
    if (!text) return pos;
    
    for (size_t i = 0; i < offset && text[i]; i++) {
        if (text[i] == '\n') {
            pos.line++;
            pos.character = 0;
        } else {
            pos.character++;
        }
    }
    
    return pos;
}

size_t lsp_position_to_offset(const char* text, const LSPPosition* position) {
    if (!text || !position) return 0;
    
    size_t offset = 0;
    uint32_t current_line = 0;
    uint32_t current_char = 0;
    
    while (text[offset] && current_line < position->line) {
        if (text[offset] == '\n') {
            current_line++;
            current_char = 0;
        } else {
            current_char++;
        }
        offset++;
    }
    
    while (text[offset] && current_char < position->character && text[offset] != '\n') {
        current_char++;
        offset++;
    }
    
    return offset;
}

// =============================================================================
// Type conversion helpers
// =============================================================================

LSPCompletionItemKind lsp_type_to_completion_kind(const Type* type) {
    if (!type) return LSP_COMPLETION_TEXT;
    
    switch (type->kind) {
        case TYPE_FUNCTION:
            return LSP_COMPLETION_FUNCTION;
        case TYPE_STRUCT:
            return LSP_COMPLETION_STRUCT;
        case TYPE_INTERFACE:
            return LSP_COMPLETION_INTERFACE;
        case TYPE_CONCEPT:
            return LSP_COMPLETION_INTERFACE;
        case TYPE_POINTER:
            return LSP_COMPLETION_VARIABLE;
        case TYPE_ARRAY:
            return LSP_COMPLETION_VALUE;
        default:
            return LSP_COMPLETION_VALUE;
    }
}

LSPSymbolKind lsp_ast_to_symbol_kind(const ASTNode* node) {
    if (!node) return LSP_SYMBOL_FILE;
    
    switch (node->type) {
        case AST_FUNC_DECL:
            return LSP_SYMBOL_FUNCTION;
        case AST_VAR_DECL:
            return LSP_SYMBOL_VARIABLE;
        case AST_STRUCT_TYPE:
            return LSP_SYMBOL_STRUCT;
        case AST_INTERFACE_TYPE:
            return LSP_SYMBOL_INTERFACE;
        case AST_CONCEPT_DECL:
            return LSP_SYMBOL_INTERFACE;
        case AST_CONST_DECL:
            return LSP_SYMBOL_CONSTANT;
        case AST_TYPE_DECL:
            return LSP_SYMBOL_CLASS;
        default:
            return LSP_SYMBOL_VARIABLE;
    }
}

LSPDiagnosticSeverity lsp_error_to_diagnostic_severity(ErrorSeverity severity) {
    switch (severity) {
        case ERROR_SEVERITY_FATAL:
        case ERROR_SEVERITY_ERROR:
            return LSP_DIAGNOSTIC_ERROR;
        case ERROR_SEVERITY_WARNING:
            return LSP_DIAGNOSTIC_WARNING;
        case ERROR_SEVERITY_NOTE:
            return LSP_DIAGNOSTIC_HINT;
        default:
            return LSP_DIAGNOSTIC_ERROR;
    }
}

// =============================================================================
// JSON utilities
// =============================================================================

char* lsp_json_create_string(const char* value) {
    return lsp_escape_json_string(value);
}

char* lsp_json_create_number(double value) {
    char* result = malloc(32);
    if (result) {
        snprintf(result, 32, "%.6g", value);
    }
    return result;
}

char* lsp_json_create_boolean(bool value) {
    return str_dup_safe(value ? "true" : "false");
}

char* lsp_json_create_null(void) {
    return str_dup_safe("null");
}

// Simple JSON parsing - in production, would use a proper JSON library
char* lsp_json_get_string(const char* json, const char* key) {
    if (!json || !key) return NULL;
    
    // Look for "key":
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    
    const char* start = strstr(json, pattern);
    if (!start) return NULL;
    
    start += strlen(pattern);
    
    // Skip whitespace
    while (*start && isspace(*start)) start++;
    
    // Expect opening quote
    if (*start != '"') return NULL;
    start++;
    
    // Find closing quote
    const char* end = start;
    while (*end && *end != '"') {
        if (*end == '\\') end++; // Skip escaped character
        end++;
    }
    
    if (*end != '"') return NULL;
    
    size_t len = end - start;
    char* result = malloc(len + 1);
    if (result) {
        strncpy(result, start, len);
        result[len] = '\0';
    }
    
    return result;
}

double lsp_json_get_number(const char* json, const char* key) {
    char* str_value = lsp_json_get_string(json, key);
    if (!str_value) return 0.0;
    
    double result = atof(str_value);
    free(str_value);
    return result;
}

bool lsp_json_get_boolean(const char* json, const char* key) {
    if (!json || !key) return false;
    
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    
    const char* start = strstr(json, pattern);
    if (!start) return false;
    
    start += strlen(pattern);
    while (*start && isspace(*start)) start++;
    
    return strncmp(start, "true", 4) == 0;
}

bool lsp_json_has_key(const char* json, const char* key) {
    if (!json || !key) return false;
    
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    
    return strstr(json, pattern) != NULL;
}

// =============================================================================
// LSP Document Management
// =============================================================================

LSPDocument* lsp_document_new(const char* uri, const char* language_id, 
                              uint32_t version, const char* text) {
    if (!uri) return NULL;
    
    LSPDocument* doc = calloc(1, sizeof(LSPDocument));
    if (!doc) return NULL;
    
    doc->uri = str_dup_safe(uri);
    doc->language_id = str_dup_safe(language_id);
    doc->version = version;
    doc->text = str_dup_safe(text);
    doc->text_length = text ? strlen(text) : 0;
    doc->needs_analysis = true;
    doc->last_modified = time(NULL);
    
    return doc;
}

void lsp_document_free(LSPDocument* doc) {
    if (!doc) return;
    
    free(doc->uri);
    free(doc->language_id);
    free(doc->text);
    
    if (doc->ast) {
        ast_node_free(doc->ast);
    }
    
    // Cleanup type checker - simplified for now
    
    lsp_diagnostic_list_free(doc->diagnostics);
    lsp_symbol_list_free(doc->symbols);
    
    free(doc);
}

bool lsp_document_update_text(LSPDocument* doc, uint32_t version, const char* text) {
    if (!doc || !text) return false;
    
    free(doc->text);
    doc->text = str_dup_safe(text);
    doc->text_length = strlen(text);
    doc->version = version;
    doc->needs_analysis = true;
    doc->last_modified = time(NULL);
    
    // Invalidate cached analysis
    lsp_document_invalidate_analysis(doc);
    
    return true;
}

void lsp_document_invalidate_analysis(LSPDocument* doc) {
    if (!doc) return;
    
    if (doc->ast) {
        ast_node_free(doc->ast);
        doc->ast = NULL;
    }
    
    lsp_diagnostic_list_free(doc->diagnostics);
    doc->diagnostics = NULL;
    
    lsp_symbol_list_free(doc->symbols);
    doc->symbols = NULL;
    
    doc->needs_analysis = true;
}

bool lsp_document_analyze(LSPDocument* doc, TypeChecker* global_type_checker) {
    (void)global_type_checker; // Unused parameter
    
    if (!doc || !doc->text) return false;
    
    if (!doc->needs_analysis) return true; // Already analyzed
    
    lsp_log_debug("Analyzing document: %s", doc->uri);
    
    // Parse the document
    if (doc->ast) {
        ast_node_free(doc->ast);
        doc->ast = NULL;
    }
    
    // Create lexer and parser
    Lexer* lexer = lexer_new(doc->text, doc->uri);
    if (!lexer) {
        lsp_log_error("Failed to create lexer for %s", doc->uri);
        return false;
    }
    
    // Simplified parsing for now - would call actual parser
    doc->ast = NULL; // TODO: Implement actual parsing
    lexer_free(lexer);
    
    // For now, continue without AST
    // if (!doc->ast) {
    //     lsp_log_error("Failed to parse %s", doc->uri);
    //     return false;
    // }
    
    // Type check the document
    // Simplified type checking for now
    // TODO: Implement full type checking integration
    
    doc->needs_analysis = false;
    lsp_log_debug("Analysis completed for: %s", doc->uri);
    
    return true;
}

// =============================================================================
// LSP Server Management
// =============================================================================

LSPServer* lsp_server_new(void) {
    LSPServer* server = calloc(1, sizeof(LSPServer));
    if (!server) return NULL;
    
    server->input_stream = stdin;
    server->output_stream = stdout;
    server->running = false;
    server->start_time = time(NULL);
    
    // Default capabilities
    server->capabilities.text_document_sync_full = true;
    server->capabilities.completion_provider = true;
    server->capabilities.hover_provider = true;
    server->capabilities.definition_provider = true;
    server->capabilities.references_provider = true;
    server->capabilities.document_symbol_provider = true;
    
    // Initialize type checker - simplified for now
    server->global_type_checker = NULL;
    
    server->error_context = error_context_new();
    
    return server;
}

void lsp_server_free(LSPServer* server) {
    if (!server) return;
    
    // Free documents
    LSPDocument* doc = server->documents;
    while (doc) {
        LSPDocument* next = doc->next;
        lsp_document_free(doc);
        doc = next;
    }
    
    free(server->root_path);
    free(server->root_uri);
    
    // Cleanup type checker - simplified for now
    
    if (server->error_context) {
        error_context_free(server->error_context);
    }
    
    free(server);
}

LSPDocument* lsp_server_get_document(LSPServer* server, const char* uri) {
    if (!server || !uri) return NULL;
    
    LSPDocument* doc = server->documents;
    while (doc) {
        if (doc->uri && strcmp(doc->uri, uri) == 0) {
            return doc;
        }
        doc = doc->next;
    }
    
    return NULL;
}

LSPDocument* lsp_server_open_document(LSPServer* server, const char* uri, 
                                      const char* language_id, uint32_t version, const char* text) {
    if (!server || !uri) return NULL;
    
    // Check if document is already open
    LSPDocument* existing = lsp_server_get_document(server, uri);
    if (existing) {
        lsp_document_update_text(existing, version, text);
        return existing;
    }
    
    // Create new document
    LSPDocument* doc = lsp_document_new(uri, language_id, version, text);
    if (!doc) return NULL;
    
    // Add to server's document list
    doc->next = server->documents;
    server->documents = doc;
    server->document_count++;
    
    lsp_log_info("Opened document: %s", uri);
    
    return doc;
}

void lsp_server_close_document(LSPServer* server, const char* uri) {
    if (!server || !uri) return;
    
    LSPDocument** current = &server->documents;
    while (*current) {
        if ((*current)->uri && strcmp((*current)->uri, uri) == 0) {
            LSPDocument* to_remove = *current;
            *current = (*current)->next;
            lsp_document_free(to_remove);
            server->document_count--;
            lsp_log_info("Closed document: %s", uri);
            return;
        }
        current = &(*current)->next;
    }
}

void lsp_server_set_capabilities(LSPServer* server, const LSPServerCapabilities* capabilities) {
    if (!server || !capabilities) return;
    
    server->capabilities = *capabilities;
}

void lsp_server_set_workspace(LSPServer* server, const char* root_uri, const char* root_path) {
    if (!server) return;
    
    free(server->root_uri);
    free(server->root_path);
    
    server->root_uri = str_dup_safe(root_uri);
    server->root_path = str_dup_safe(root_path);
}

void lsp_server_shutdown(LSPServer* server) {
    if (!server) return;
    
    server->running = false;
}