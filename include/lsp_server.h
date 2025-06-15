#ifndef GOO_LSP_SERVER_H
#define GOO_LSP_SERVER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "ast.h"
#include "types.h"
#include "errors/error.h"

// Forward declarations
typedef struct LSPServer LSPServer;
typedef struct LSPDocument LSPDocument;
typedef struct LSPPosition LSPPosition;
typedef struct LSPRange LSPRange;
typedef struct LSPLocation LSPLocation;
typedef struct LSPDiagnostic LSPDiagnostic;
typedef struct LSPCompletionItem LSPCompletionItem;
typedef struct LSPHover LSPHover;
typedef struct LSPSymbolInformation LSPSymbolInformation;

// =============================================================================
// LSP Protocol Types
// =============================================================================

// LSP Position (line, character)
struct LSPPosition {
    uint32_t line;      // 0-based line number
    uint32_t character; // 0-based character offset
};

// LSP Range (start and end positions)
struct LSPRange {
    LSPPosition start;
    LSPPosition end;
};

// LSP Location (URI and range)
struct LSPLocation {
    char* uri;          // Document URI
    LSPRange range;     // Location range
};

// LSP Diagnostic severity levels
typedef enum {
    LSP_DIAGNOSTIC_ERROR = 1,
    LSP_DIAGNOSTIC_WARNING = 2,
    LSP_DIAGNOSTIC_INFORMATION = 3,
    LSP_DIAGNOSTIC_HINT = 4
} LSPDiagnosticSeverity;

// LSP Diagnostic
struct LSPDiagnostic {
    LSPRange range;
    LSPDiagnosticSeverity severity;
    char* code;         // Optional error code
    char* source;       // Source (e.g., "goo")
    char* message;      // Diagnostic message
    struct LSPDiagnostic* next;
};

// LSP Completion Item kinds
typedef enum {
    LSP_COMPLETION_TEXT = 1,
    LSP_COMPLETION_METHOD = 2,
    LSP_COMPLETION_FUNCTION = 3,
    LSP_COMPLETION_CONSTRUCTOR = 4,
    LSP_COMPLETION_FIELD = 5,
    LSP_COMPLETION_VARIABLE = 6,
    LSP_COMPLETION_CLASS = 7,
    LSP_COMPLETION_INTERFACE = 8,
    LSP_COMPLETION_MODULE = 9,
    LSP_COMPLETION_PROPERTY = 10,
    LSP_COMPLETION_UNIT = 11,
    LSP_COMPLETION_VALUE = 12,
    LSP_COMPLETION_ENUM = 13,
    LSP_COMPLETION_KEYWORD = 14,
    LSP_COMPLETION_SNIPPET = 15,
    LSP_COMPLETION_COLOR = 16,
    LSP_COMPLETION_FILE = 17,
    LSP_COMPLETION_REFERENCE = 18,
    LSP_COMPLETION_FOLDER = 19,
    LSP_COMPLETION_ENUM_MEMBER = 20,
    LSP_COMPLETION_CONSTANT = 21,
    LSP_COMPLETION_STRUCT = 22,
    LSP_COMPLETION_EVENT = 23,
    LSP_COMPLETION_OPERATOR = 24,
    LSP_COMPLETION_TYPE_PARAMETER = 25
} LSPCompletionItemKind;

// LSP Completion Item
struct LSPCompletionItem {
    char* label;                    // The label of this completion item
    LSPCompletionItemKind kind;     // The kind of this completion item
    char* detail;                   // A human-readable string with additional information
    char* documentation;            // A human-readable string that represents a doc-comment
    char* sort_text;                // A string that should be used when comparing this item
    char* filter_text;              // A string that should be used when filtering a set of completion items
    char* insert_text;              // A string that should be inserted when selecting this completion
    bool deprecated;                // Indicates if this item is deprecated
    struct LSPCompletionItem* next;
};

// LSP Hover information
struct LSPHover {
    char* contents;     // Markdown or plain text contents
    LSPRange* range;    // Optional range for the hover
};

// LSP Symbol kinds
typedef enum {
    LSP_SYMBOL_FILE = 1,
    LSP_SYMBOL_MODULE = 2,
    LSP_SYMBOL_NAMESPACE = 3,
    LSP_SYMBOL_PACKAGE = 4,
    LSP_SYMBOL_CLASS = 5,
    LSP_SYMBOL_METHOD = 6,
    LSP_SYMBOL_PROPERTY = 7,
    LSP_SYMBOL_FIELD = 8,
    LSP_SYMBOL_CONSTRUCTOR = 9,
    LSP_SYMBOL_ENUM = 10,
    LSP_SYMBOL_INTERFACE = 11,
    LSP_SYMBOL_FUNCTION = 12,
    LSP_SYMBOL_VARIABLE = 13,
    LSP_SYMBOL_CONSTANT = 14,
    LSP_SYMBOL_STRING = 15,
    LSP_SYMBOL_NUMBER = 16,
    LSP_SYMBOL_BOOLEAN = 17,
    LSP_SYMBOL_ARRAY = 18,
    LSP_SYMBOL_OBJECT = 19,
    LSP_SYMBOL_KEY = 20,
    LSP_SYMBOL_NULL = 21,
    LSP_SYMBOL_ENUM_MEMBER = 22,
    LSP_SYMBOL_STRUCT = 23,
    LSP_SYMBOL_EVENT = 24,
    LSP_SYMBOL_OPERATOR = 25,
    LSP_SYMBOL_TYPE_PARAMETER = 26
} LSPSymbolKind;

// LSP Symbol Information
struct LSPSymbolInformation {
    char* name;                         // The name of this symbol
    LSPSymbolKind kind;                 // The kind of this symbol
    bool deprecated;                    // Indicates if this symbol is deprecated
    LSPLocation location;               // The location of this symbol
    char* container_name;               // The name of the symbol containing this symbol
    struct LSPSymbolInformation* next;
};

// =============================================================================
// Document Management
// =============================================================================

// LSP Document state
struct LSPDocument {
    char* uri;              // Document URI
    char* language_id;      // Language identifier (e.g., "goo")
    uint32_t version;       // Document version number
    char* text;             // Document text content
    size_t text_length;     // Length of text content
    
    // Analysis results
    ASTNode* ast;                   // Parsed AST
    TypeChecker* type_checker;      // Type analysis
    LSPDiagnostic* diagnostics;     // Current diagnostics
    bool needs_analysis;            // Flag for re-analysis
    
    // Caching
    LSPSymbolInformation* symbols;  // Document symbols
    time_t last_modified;           // Last modification time
    
    struct LSPDocument* next;
};

// =============================================================================
// LSP Server State
// =============================================================================

// LSP Server capabilities
typedef struct {
    // Text document sync
    bool text_document_sync_full;
    bool text_document_sync_incremental;
    
    // Completion
    bool completion_provider;
    char* completion_trigger_characters[10];
    size_t completion_trigger_count;
    
    // Hover
    bool hover_provider;
    
    // Definition
    bool definition_provider;
    
    // References
    bool references_provider;
    
    // Document symbols
    bool document_symbol_provider;
    
    // Workspace symbols
    bool workspace_symbol_provider;
    
    // Code actions
    bool code_action_provider;
    
    // Formatting
    bool document_formatting_provider;
    bool document_range_formatting_provider;
    
    // Rename
    bool rename_provider;
    
    // Semantic tokens
    bool semantic_tokens_provider;
} LSPServerCapabilities;

// LSP Server
struct LSPServer {
    // Communication
    FILE* input_stream;     // Input stream (stdin)
    FILE* output_stream;    // Output stream (stdout)
    bool running;           // Server running state
    
    // Capabilities
    LSPServerCapabilities capabilities;
    
    // Document management
    LSPDocument* documents; // Open documents
    size_t document_count;
    
    // Workspace
    char* root_path;        // Workspace root path
    char* root_uri;         // Workspace root URI
    
    // Analysis
    TypeChecker* global_type_checker;   // Global type context
    ErrorContext* error_context;        // Error reporting
    
    // Statistics
    uint64_t requests_handled;
    uint64_t notifications_handled;
    time_t start_time;
};

// =============================================================================
// Server Management
// =============================================================================

// Server lifecycle
LSPServer* lsp_server_new(void);
void lsp_server_free(LSPServer* server);
int lsp_server_run(LSPServer* server);
void lsp_server_shutdown(LSPServer* server);

// Server configuration
void lsp_server_set_capabilities(LSPServer* server, const LSPServerCapabilities* capabilities);
void lsp_server_set_workspace(LSPServer* server, const char* root_uri, const char* root_path);

// =============================================================================
// Document Management
// =============================================================================

// Document lifecycle
LSPDocument* lsp_document_new(const char* uri, const char* language_id, 
                              uint32_t version, const char* text);
void lsp_document_free(LSPDocument* doc);
LSPDocument* lsp_server_get_document(LSPServer* server, const char* uri);
LSPDocument* lsp_server_open_document(LSPServer* server, const char* uri, 
                                      const char* language_id, uint32_t version, const char* text);
void lsp_server_close_document(LSPServer* server, const char* uri);

// Document updates
bool lsp_document_update_text(LSPDocument* doc, uint32_t version, const char* text);
bool lsp_document_apply_changes(LSPDocument* doc, uint32_t version, 
                                const LSPRange* ranges, const char** texts, size_t change_count);

// Document analysis
bool lsp_document_analyze(LSPDocument* doc, TypeChecker* type_checker);
void lsp_document_invalidate_analysis(LSPDocument* doc);

// =============================================================================
// LSP Protocol Handlers
// =============================================================================

// Lifecycle methods
int lsp_handle_initialize(LSPServer* server, const char* request_id, const char* params);
int lsp_handle_initialized(LSPServer* server);
int lsp_handle_shutdown(LSPServer* server, const char* request_id);
int lsp_handle_exit(LSPServer* server);

// Text document synchronization
int lsp_handle_text_document_did_open(LSPServer* server, const char* params);
int lsp_handle_text_document_did_change(LSPServer* server, const char* params);
int lsp_handle_text_document_did_close(LSPServer* server, const char* params);
int lsp_handle_text_document_did_save(LSPServer* server, const char* params);

// Language features
int lsp_handle_completion(LSPServer* server, const char* request_id, const char* params);
int lsp_handle_hover(LSPServer* server, const char* request_id, const char* params);
int lsp_handle_definition(LSPServer* server, const char* request_id, const char* params);
int lsp_handle_references(LSPServer* server, const char* request_id, const char* params);
int lsp_handle_document_symbols(LSPServer* server, const char* request_id, const char* params);
int lsp_handle_workspace_symbols(LSPServer* server, const char* request_id, const char* params);

// =============================================================================
// Language Feature Implementations
// =============================================================================

// Completion
LSPCompletionItem* lsp_provide_completion(LSPDocument* doc, LSPPosition position, 
                                          TypeChecker* type_checker);
void lsp_completion_item_free(LSPCompletionItem* item);
void lsp_completion_list_free(LSPCompletionItem* items);

// Hover
LSPHover* lsp_provide_hover(LSPDocument* doc, LSPPosition position, TypeChecker* type_checker);
void lsp_hover_free(LSPHover* hover);

// Definition
LSPLocation* lsp_provide_definition(LSPDocument* doc, LSPPosition position, 
                                    TypeChecker* type_checker);
void lsp_location_free(LSPLocation* location);
void lsp_location_list_free(LSPLocation* locations);

// References
LSPLocation* lsp_provide_references(LSPDocument* doc, LSPPosition position, 
                                    TypeChecker* type_checker, bool include_declaration);

// Document symbols
LSPSymbolInformation* lsp_provide_document_symbols(LSPDocument* doc, TypeChecker* type_checker);
void lsp_symbol_information_free(LSPSymbolInformation* symbol);
void lsp_symbol_list_free(LSPSymbolInformation* symbols);

// Diagnostics
LSPDiagnostic* lsp_create_diagnostics(LSPDocument* doc, ErrorContext* error_context);
void lsp_diagnostic_free(LSPDiagnostic* diagnostic);
void lsp_diagnostic_list_free(LSPDiagnostic* diagnostics);
void lsp_publish_diagnostics(LSPServer* server, const char* uri, LSPDiagnostic* diagnostics);

// =============================================================================
// JSON-RPC Communication
// =============================================================================

// Message parsing and creation
typedef struct {
    char* jsonrpc;      // Protocol version ("2.0")
    char* id;           // Request ID (null for notifications)
    char* method;       // Method name
    char* params;       // Parameters (JSON string)
    char* result;       // Result (for responses)
    char* error;        // Error (for error responses)
} LSPMessage;

// Message handling
LSPMessage* lsp_message_parse(const char* json);
void lsp_message_free(LSPMessage* message);
char* lsp_message_create_response(const char* id, const char* result);
char* lsp_message_create_error(const char* id, int code, const char* message);
char* lsp_message_create_notification(const char* method, const char* params);

// Communication
int lsp_read_message(FILE* input, char** content, size_t* length);
int lsp_write_message(FILE* output, const char* content);
int lsp_send_response(LSPServer* server, const char* id, const char* result);
int lsp_send_error(LSPServer* server, const char* id, int code, const char* message);
int lsp_send_notification(LSPServer* server, const char* method, const char* params);

// =============================================================================
// JSON Utilities
// =============================================================================

// JSON creation helpers
char* lsp_json_create_object(const char* key_values[], size_t count);
char* lsp_json_create_array(const char* items[], size_t count);
char* lsp_json_create_string(const char* value);
char* lsp_json_create_number(double value);
char* lsp_json_create_boolean(bool value);
char* lsp_json_create_null(void);

// JSON parsing helpers
char* lsp_json_get_string(const char* json, const char* key);
double lsp_json_get_number(const char* json, const char* key);
bool lsp_json_get_boolean(const char* json, const char* key);
bool lsp_json_has_key(const char* json, const char* key);

// Position and range helpers
LSPPosition lsp_source_to_lsp_position(const SourceLocation* source_loc);
LSPRange lsp_source_to_lsp_range(const SourceLocation* source_loc);
SourceLocation lsp_position_to_source(const LSPPosition* lsp_pos, const char* filename);

// Type conversion helpers
LSPCompletionItemKind lsp_type_to_completion_kind(const Type* type);
LSPSymbolKind lsp_ast_to_symbol_kind(const ASTNode* node);
LSPDiagnosticSeverity lsp_error_to_diagnostic_severity(ErrorSeverity severity);

// =============================================================================
// Utility Functions
// =============================================================================

// String utilities
char* lsp_escape_json_string(const char* str);
char* lsp_uri_to_path(const char* uri);
char* lsp_path_to_uri(const char* path);
bool lsp_uri_is_file(const char* uri);

// Position utilities
int lsp_position_compare(const LSPPosition* a, const LSPPosition* b);
bool lsp_position_in_range(const LSPPosition* pos, const LSPRange* range);
LSPPosition lsp_offset_to_position(const char* text, size_t offset);
size_t lsp_position_to_offset(const char* text, const LSPPosition* position);

// Error handling
void lsp_log_error(const char* message, ...);
void lsp_log_info(const char* message, ...);
void lsp_log_debug(const char* message, ...);

// Configuration
typedef struct {
    bool enable_logging;
    char* log_file;
    bool enable_debug;
    int max_completions;
    int completion_timeout_ms;
} LSPServerConfig;

LSPServerConfig* lsp_config_load(const char* config_file);
void lsp_config_free(LSPServerConfig* config);

#endif // GOO_LSP_SERVER_H