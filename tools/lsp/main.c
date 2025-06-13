#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include <json-c/json.h>
#include <pthread.h>
#include <signal.h>

// Goo Language Server Protocol Implementation
// Provides IDE integration for the Goo programming language

#define MAX_MESSAGE_SIZE (1024 * 1024)  // 1MB
#define MAX_PATH_LENGTH 1024
#define MAX_LINE_LENGTH 1024
#define MAX_SYMBOL_NAME 256

// LSP Message types
typedef enum {
    LSP_REQUEST,
    LSP_RESPONSE,
    LSP_NOTIFICATION
} LSPMessageType;

// LSP Methods
typedef enum {
    METHOD_INITIALIZE,
    METHOD_INITIALIZED,
    METHOD_SHUTDOWN,
    METHOD_EXIT,
    METHOD_TEXT_DOCUMENT_DID_OPEN,
    METHOD_TEXT_DOCUMENT_DID_CHANGE,
    METHOD_TEXT_DOCUMENT_DID_SAVE,
    METHOD_TEXT_DOCUMENT_DID_CLOSE,
    METHOD_TEXT_DOCUMENT_COMPLETION,
    METHOD_TEXT_DOCUMENT_HOVER,
    METHOD_TEXT_DOCUMENT_DEFINITION,
    METHOD_TEXT_DOCUMENT_REFERENCES,
    METHOD_TEXT_DOCUMENT_DOCUMENT_SYMBOL,
    METHOD_TEXT_DOCUMENT_FORMATTING,
    METHOD_TEXT_DOCUMENT_RANGE_FORMATTING,
    METHOD_TEXT_DOCUMENT_SIGNATURE_HELP,
    METHOD_WORKSPACE_SYMBOL,
    METHOD_UNKNOWN
} LSPMethod;

// Position in a text document
typedef struct {
    int line;
    int character;
} Position;

// Range in a text document
typedef struct {
    Position start;
    Position end;
} Range;

// Text document identifier
typedef struct {
    char uri[MAX_PATH_LENGTH];
    int version;
} TextDocumentIdentifier;

// Symbol information
typedef struct {
    char name[MAX_SYMBOL_NAME];
    int kind;  // SymbolKind
    Position position;
    Range range;
    char container_name[MAX_SYMBOL_NAME];
} SymbolInfo;

// Completion item
typedef struct {
    char label[MAX_SYMBOL_NAME];
    int kind;  // CompletionItemKind
    char detail[512];
    char documentation[1024];
    char insert_text[MAX_SYMBOL_NAME];
} CompletionItem;

// Diagnostic information
typedef struct {
    Range range;
    int severity;  // DiagnosticSeverity
    char message[512];
    char source[64];
} Diagnostic;

// Document state
typedef struct Document {
    char uri[MAX_PATH_LENGTH];
    int version;
    char* content;
    size_t content_length;
    Diagnostic* diagnostics;
    int diagnostic_count;
    SymbolInfo* symbols;
    int symbol_count;
    struct Document* next;
} Document;

// LSP Server state
typedef struct {
    int initialized;
    int shutdown_requested;
    Document* documents;
    pthread_mutex_t documents_mutex;
    
    // Server capabilities
    int supports_completion;
    int supports_hover;
    int supports_goto_definition;
    int supports_find_references;
    int supports_document_symbols;
    int supports_formatting;
    int supports_signature_help;
    
    // Configuration
    char workspace_root[MAX_PATH_LENGTH];
    int trace_level;
} LSPServer;

static LSPServer server = {0};

// Function prototypes
void lsp_server_init(void);
void lsp_server_cleanup(void);
void lsp_message_loop(void);
int lsp_read_message(char** content, size_t* length);
void lsp_write_message(const char* content);
void lsp_send_response(int id, json_object* result);
void lsp_send_error(int id, int code, const char* message);
void lsp_send_notification(const char* method, json_object* params);

// Message handlers
void handle_initialize(int id, json_object* params);
void handle_initialized(json_object* params);
void handle_shutdown(int id);
void handle_exit(void);
void handle_text_document_did_open(json_object* params);
void handle_text_document_did_change(json_object* params);
void handle_text_document_did_save(json_object* params);
void handle_text_document_did_close(json_object* params);
void handle_text_document_completion(int id, json_object* params);
void handle_text_document_hover(int id, json_object* params);
void handle_text_document_definition(int id, json_object* params);
void handle_text_document_references(int id, json_object* params);
void handle_text_document_document_symbol(int id, json_object* params);
void handle_text_document_formatting(int id, json_object* params);
void handle_text_document_signature_help(int id, json_object* params);
void handle_workspace_symbol(int id, json_object* params);

// Document management
Document* find_document(const char* uri);
Document* create_document(const char* uri, const char* content);
void update_document(const char* uri, const char* content, int version);
void close_document(const char* uri);
void analyze_document(Document* doc);
void publish_diagnostics(Document* doc);

// Analysis functions
void parse_document_symbols(Document* doc);
void check_document_syntax(Document* doc);
void find_completion_items(Document* doc, Position pos, CompletionItem** items, int* count);
void find_hover_info(Document* doc, Position pos, char** hover_text);
void find_definition(Document* doc, Position pos, Position* def_pos);
void find_references(Document* doc, Position pos, Position** refs, int* count);

// Utility functions
LSPMethod parse_method(const char* method_name);
Position offset_to_position(const char* text, int offset);
int position_to_offset(const char* text, Position pos);
void log_message(const char* format, ...);

int main(int argc, char* argv[]) {
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Goo Language Server v0.1.0\n\n");
            printf("Usage: goo-lsp [options]\n\n");
            printf("Options:\n");
            printf("  --help, -h           Show this help message\n");
            printf("  --stdio              Use stdio for communication (default)\n");
            printf("  --socket <port>      Use TCP socket for communication\n");
            printf("  --trace              Enable trace logging\n");
            printf("  --log-file <file>    Log to file instead of stderr\n");
            return 0;
        } else if (strcmp(argv[i], "--trace") == 0) {
            server.trace_level = 1;
        } else if (strcmp(argv[i], "--stdio") == 0) {
            // Default mode, nothing to do
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            // Socket mode not implemented in this demo
            fprintf(stderr, "Socket mode not implemented\n");
            return 1;
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            // Log file redirection not implemented in this demo
            i++; // Skip the file argument
        }
    }
    
    // Initialize server
    lsp_server_init();
    
    log_message("Goo Language Server starting...");
    
    // Set up signal handlers
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    
    // Main message loop
    lsp_message_loop();
    
    // Cleanup
    lsp_server_cleanup();
    
    log_message("Goo Language Server stopped");
    
    return 0;
}

void lsp_server_init(void) {
    memset(&server, 0, sizeof(server));
    pthread_mutex_init(&server.documents_mutex, NULL);
    
    // Set server capabilities
    server.supports_completion = 1;
    server.supports_hover = 1;
    server.supports_goto_definition = 1;
    server.supports_find_references = 1;
    server.supports_document_symbols = 1;
    server.supports_formatting = 1;
    server.supports_signature_help = 1;
}

void lsp_server_cleanup(void) {
    pthread_mutex_lock(&server.documents_mutex);
    
    // Free all documents
    Document* doc = server.documents;
    while (doc) {
        Document* next = doc->next;
        free(doc->content);
        free(doc->diagnostics);
        free(doc->symbols);
        free(doc);
        doc = next;
    }
    
    pthread_mutex_unlock(&server.documents_mutex);
    pthread_mutex_destroy(&server.documents_mutex);
}

void lsp_message_loop(void) {
    while (!server.shutdown_requested) {
        char* content = NULL;
        size_t length = 0;
        
        if (lsp_read_message(&content, &length) != 0) {
            if (!server.shutdown_requested) {
                log_message("Failed to read message, exiting");
            }
            break;
        }
        
        if (length == 0) {
            free(content);
            continue;
        }
        
        // Parse JSON message
        json_object* root = json_tokener_parse(content);
        if (!root) {
            log_message("Invalid JSON message");
            free(content);
            continue;
        }
        
        // Extract message components
        json_object* method_obj = NULL;
        json_object* id_obj = NULL;
        json_object* params_obj = NULL;
        
        json_object_object_get_ex(root, "method", &method_obj);
        json_object_object_get_ex(root, "id", &id_obj);
        json_object_object_get_ex(root, "params", &params_obj);
        
        if (method_obj) {
            const char* method_name = json_object_get_string(method_obj);
            LSPMethod method = parse_method(method_name);
            int id = id_obj ? json_object_get_int(id_obj) : -1;
            
            // Dispatch to appropriate handler
            switch (method) {
                case METHOD_INITIALIZE:
                    handle_initialize(id, params_obj);
                    break;
                case METHOD_INITIALIZED:
                    handle_initialized(params_obj);
                    break;
                case METHOD_SHUTDOWN:
                    handle_shutdown(id);
                    break;
                case METHOD_EXIT:
                    handle_exit();
                    break;
                case METHOD_TEXT_DOCUMENT_DID_OPEN:
                    handle_text_document_did_open(params_obj);
                    break;
                case METHOD_TEXT_DOCUMENT_DID_CHANGE:
                    handle_text_document_did_change(params_obj);
                    break;
                case METHOD_TEXT_DOCUMENT_DID_SAVE:
                    handle_text_document_did_save(params_obj);
                    break;
                case METHOD_TEXT_DOCUMENT_DID_CLOSE:
                    handle_text_document_did_close(params_obj);
                    break;
                case METHOD_TEXT_DOCUMENT_COMPLETION:
                    handle_text_document_completion(id, params_obj);
                    break;
                case METHOD_TEXT_DOCUMENT_HOVER:
                    handle_text_document_hover(id, params_obj);
                    break;
                case METHOD_TEXT_DOCUMENT_DEFINITION:
                    handle_text_document_definition(id, params_obj);
                    break;
                case METHOD_TEXT_DOCUMENT_REFERENCES:
                    handle_text_document_references(id, params_obj);
                    break;
                case METHOD_TEXT_DOCUMENT_DOCUMENT_SYMBOL:
                    handle_text_document_document_symbol(id, params_obj);
                    break;
                case METHOD_TEXT_DOCUMENT_FORMATTING:
                    handle_text_document_formatting(id, params_obj);
                    break;
                case METHOD_TEXT_DOCUMENT_SIGNATURE_HELP:
                    handle_text_document_signature_help(id, params_obj);
                    break;
                case METHOD_WORKSPACE_SYMBOL:
                    handle_workspace_symbol(id, params_obj);
                    break;
                case METHOD_UNKNOWN:
                    log_message("Unknown method: %s", method_name);
                    if (id >= 0) {
                        lsp_send_error(id, -32601, "Method not found");
                    }
                    break;
            }
        }
        
        json_object_put(root);
        free(content);
    }
}

int lsp_read_message(char** content, size_t* length) {
    char header[1024];
    int content_length = 0;
    
    // Read headers
    while (1) {
        if (!fgets(header, sizeof(header), stdin)) {
            return -1;
        }
        
        // Check for end of headers
        if (strcmp(header, "\r\n") == 0 || strcmp(header, "\n") == 0) {
            break;
        }
        
        // Parse Content-Length header
        if (strncmp(header, "Content-Length:", 15) == 0) {
            content_length = atoi(header + 15);
        }
    }
    
    if (content_length <= 0 || content_length > MAX_MESSAGE_SIZE) {
        return -1;
    }
    
    // Read content
    *content = malloc(content_length + 1);
    if (!*content) {
        return -1;
    }
    
    size_t total_read = 0;
    while (total_read < content_length) {
        size_t remaining = content_length - total_read;
        size_t read_count = fread(*content + total_read, 1, remaining, stdin);
        
        if (read_count == 0) {
            free(*content);
            return -1;
        }
        
        total_read += read_count;
    }
    
    (*content)[content_length] = '\0';
    *length = content_length;
    
    return 0;
}

void lsp_write_message(const char* content) {
    size_t content_length = strlen(content);
    
    printf("Content-Length: %zu\r\n\r\n%s", content_length, content);
    fflush(stdout);
}

void lsp_send_response(int id, json_object* result) {
    json_object* response = json_object_new_object();
    json_object* jsonrpc = json_object_new_string("2.0");
    json_object* id_obj = json_object_new_int(id);
    
    json_object_object_add(response, "jsonrpc", jsonrpc);
    json_object_object_add(response, "id", id_obj);
    json_object_object_add(response, "result", result);
    
    const char* response_str = json_object_to_json_string(response);
    lsp_write_message(response_str);
    
    json_object_put(response);
}

void lsp_send_error(int id, int code, const char* message) {
    json_object* response = json_object_new_object();
    json_object* jsonrpc = json_object_new_string("2.0");
    json_object* id_obj = json_object_new_int(id);
    json_object* error = json_object_new_object();
    json_object* error_code = json_object_new_int(code);
    json_object* error_message = json_object_new_string(message);
    
    json_object_object_add(error, "code", error_code);
    json_object_object_add(error, "message", error_message);
    
    json_object_object_add(response, "jsonrpc", jsonrpc);
    json_object_object_add(response, "id", id_obj);
    json_object_object_add(response, "error", error);
    
    const char* response_str = json_object_to_json_string(response);
    lsp_write_message(response_str);
    
    json_object_put(response);
}

void lsp_send_notification(const char* method, json_object* params) {
    json_object* notification = json_object_new_object();
    json_object* jsonrpc = json_object_new_string("2.0");
    json_object* method_obj = json_object_new_string(method);
    
    json_object_object_add(notification, "jsonrpc", jsonrpc);
    json_object_object_add(notification, "method", method_obj);
    json_object_object_add(notification, "params", params);
    
    const char* notification_str = json_object_to_json_string(notification);
    lsp_write_message(notification_str);
    
    json_object_put(notification);
}

void handle_initialize(int id, json_object* params) {
    log_message("Handling initialize request");
    
    // Extract workspace root from params
    json_object* root_uri_obj = NULL;
    if (params && json_object_object_get_ex(params, "rootUri", &root_uri_obj)) {
        const char* root_uri = json_object_get_string(root_uri_obj);
        if (root_uri) {
            strncpy(server.workspace_root, root_uri, sizeof(server.workspace_root) - 1);
        }
    }
    
    // Create server capabilities
    json_object* result = json_object_new_object();
    json_object* capabilities = json_object_new_object();
    
    // Text document sync
    json_object* text_sync = json_object_new_int(1); // Full sync
    json_object_object_add(capabilities, "textDocumentSync", text_sync);
    
    // Completion provider
    if (server.supports_completion) {
        json_object* completion = json_object_new_object();
        json_object* resolve_provider = json_object_new_boolean(0);
        json_object* trigger_chars = json_object_new_array();
        json_object_array_add(trigger_chars, json_object_new_string("."));
        json_object_array_add(trigger_chars, json_object_new_string(":"));
        
        json_object_object_add(completion, "resolveProvider", resolve_provider);
        json_object_object_add(completion, "triggerCharacters", trigger_chars);
        json_object_object_add(capabilities, "completionProvider", completion);
    }
    
    // Hover provider
    if (server.supports_hover) {
        json_object_object_add(capabilities, "hoverProvider", json_object_new_boolean(1));
    }
    
    // Definition provider
    if (server.supports_goto_definition) {
        json_object_object_add(capabilities, "definitionProvider", json_object_new_boolean(1));
    }
    
    // References provider
    if (server.supports_find_references) {
        json_object_object_add(capabilities, "referencesProvider", json_object_new_boolean(1));
    }
    
    // Document symbol provider
    if (server.supports_document_symbols) {
        json_object_object_add(capabilities, "documentSymbolProvider", json_object_new_boolean(1));
    }
    
    // Formatting provider
    if (server.supports_formatting) {
        json_object_object_add(capabilities, "documentFormattingProvider", json_object_new_boolean(1));
        json_object_object_add(capabilities, "documentRangeFormattingProvider", json_object_new_boolean(1));
    }
    
    // Signature help provider
    if (server.supports_signature_help) {
        json_object* signature_help = json_object_new_object();
        json_object* trigger_chars = json_object_new_array();
        json_object_array_add(trigger_chars, json_object_new_string("("));
        json_object_array_add(trigger_chars, json_object_new_string(","));
        
        json_object_object_add(signature_help, "triggerCharacters", trigger_chars);
        json_object_object_add(capabilities, "signatureHelpProvider", signature_help);
    }
    
    // Workspace symbol provider
    json_object_object_add(capabilities, "workspaceSymbolProvider", json_object_new_boolean(1));
    
    json_object_object_add(result, "capabilities", capabilities);
    
    lsp_send_response(id, result);
}

void handle_initialized(json_object* params) {
    log_message("Client initialized");
    server.initialized = 1;
}

void handle_shutdown(int id) {
    log_message("Handling shutdown request");
    server.shutdown_requested = 1;
    lsp_send_response(id, json_object_new_null());
}

void handle_exit(void) {
    log_message("Handling exit notification");
    server.shutdown_requested = 1;
}

void handle_text_document_did_open(json_object* params) {
    json_object* text_doc_obj = NULL;
    if (!json_object_object_get_ex(params, "textDocument", &text_doc_obj)) {
        return;
    }
    
    json_object* uri_obj = NULL;
    json_object* text_obj = NULL;
    
    if (!json_object_object_get_ex(text_doc_obj, "uri", &uri_obj) ||
        !json_object_object_get_ex(text_doc_obj, "text", &text_obj)) {
        return;
    }
    
    const char* uri = json_object_get_string(uri_obj);
    const char* text = json_object_get_string(text_obj);
    
    log_message("Document opened: %s", uri);
    
    Document* doc = create_document(uri, text);
    if (doc) {
        analyze_document(doc);
        publish_diagnostics(doc);
    }
}

void handle_text_document_did_change(json_object* params) {
    json_object* text_doc_obj = NULL;
    json_object* changes_obj = NULL;
    
    if (!json_object_object_get_ex(params, "textDocument", &text_doc_obj) ||
        !json_object_object_get_ex(params, "contentChanges", &changes_obj)) {
        return;
    }
    
    json_object* uri_obj = NULL;
    json_object* version_obj = NULL;
    
    if (!json_object_object_get_ex(text_doc_obj, "uri", &uri_obj) ||
        !json_object_object_get_ex(text_doc_obj, "version", &version_obj)) {
        return;
    }
    
    const char* uri = json_object_get_string(uri_obj);
    int version = json_object_get_int(version_obj);
    
    // For full sync, we expect one change with the full text
    if (json_object_array_length(changes_obj) > 0) {
        json_object* change = json_object_array_get_idx(changes_obj, 0);
        json_object* text_obj = NULL;
        
        if (json_object_object_get_ex(change, "text", &text_obj)) {
            const char* text = json_object_get_string(text_obj);
            
            log_message("Document changed: %s (version %d)", uri, version);
            
            update_document(uri, text, version);
            
            Document* doc = find_document(uri);
            if (doc) {
                analyze_document(doc);
                publish_diagnostics(doc);
            }
        }
    }
}

void handle_text_document_did_save(json_object* params) {
    json_object* text_doc_obj = NULL;
    if (!json_object_object_get_ex(params, "textDocument", &text_doc_obj)) {
        return;
    }
    
    json_object* uri_obj = NULL;
    if (!json_object_object_get_ex(text_doc_obj, "uri", &uri_obj)) {
        return;
    }
    
    const char* uri = json_object_get_string(uri_obj);
    log_message("Document saved: %s", uri);
    
    // Re-analyze on save
    Document* doc = find_document(uri);
    if (doc) {
        analyze_document(doc);
        publish_diagnostics(doc);
    }
}

void handle_text_document_did_close(json_object* params) {
    json_object* text_doc_obj = NULL;
    if (!json_object_object_get_ex(params, "textDocument", &text_doc_obj)) {
        return;
    }
    
    json_object* uri_obj = NULL;
    if (!json_object_object_get_ex(text_doc_obj, "uri", &uri_obj)) {
        return;
    }
    
    const char* uri = json_object_get_string(uri_obj);
    log_message("Document closed: %s", uri);
    
    close_document(uri);
}

void handle_text_document_completion(int id, json_object* params) {
    json_object* text_doc_obj = NULL;
    json_object* position_obj = NULL;
    
    if (!json_object_object_get_ex(params, "textDocument", &text_doc_obj) ||
        !json_object_object_get_ex(params, "position", &position_obj)) {
        lsp_send_error(id, -32602, "Invalid params");
        return;
    }
    
    json_object* uri_obj = NULL;
    if (!json_object_object_get_ex(text_doc_obj, "uri", &uri_obj)) {
        lsp_send_error(id, -32602, "Invalid params");
        return;
    }
    
    const char* uri = json_object_get_string(uri_obj);
    Document* doc = find_document(uri);
    
    if (!doc) {
        lsp_send_error(id, -32602, "Document not found");
        return;
    }
    
    // Extract position
    json_object* line_obj = NULL;
    json_object* char_obj = NULL;
    
    if (!json_object_object_get_ex(position_obj, "line", &line_obj) ||
        !json_object_object_get_ex(position_obj, "character", &char_obj)) {
        lsp_send_error(id, -32602, "Invalid position");
        return;
    }
    
    Position pos = {
        .line = json_object_get_int(line_obj),
        .character = json_object_get_int(char_obj)
    };
    
    // Find completion items
    CompletionItem* items = NULL;
    int count = 0;
    find_completion_items(doc, pos, &items, &count);
    
    // Create response
    json_object* result = json_object_new_array();
    for (int i = 0; i < count; i++) {
        json_object* item = json_object_new_object();
        json_object_object_add(item, "label", json_object_new_string(items[i].label));
        json_object_object_add(item, "kind", json_object_new_int(items[i].kind));
        json_object_object_add(item, "detail", json_object_new_string(items[i].detail));
        json_object_object_add(item, "documentation", json_object_new_string(items[i].documentation));
        json_object_object_add(item, "insertText", json_object_new_string(items[i].insert_text));
        
        json_object_array_add(result, item);
    }
    
    free(items);
    lsp_send_response(id, result);
}

void handle_text_document_hover(int id, json_object* params) {
    // Similar structure to completion, but returns hover information
    json_object* result = json_object_new_object();
    json_object* contents = json_object_new_string("Hover information not implemented");
    json_object_object_add(result, "contents", contents);
    
    lsp_send_response(id, result);
}

void handle_text_document_definition(int id, json_object* params) {
    // Returns location of symbol definition
    lsp_send_response(id, json_object_new_null());
}

void handle_text_document_references(int id, json_object* params) {
    // Returns array of reference locations
    json_object* result = json_object_new_array();
    lsp_send_response(id, result);
}

void handle_text_document_document_symbol(int id, json_object* params) {
    // Returns array of document symbols
    json_object* result = json_object_new_array();
    lsp_send_response(id, result);
}

void handle_text_document_formatting(int id, json_object* params) {
    // Returns array of text edits for formatting
    json_object* result = json_object_new_array();
    lsp_send_response(id, result);
}

void handle_text_document_signature_help(int id, json_object* params) {
    // Returns signature help information
    lsp_send_response(id, json_object_new_null());
}

void handle_workspace_symbol(int id, json_object* params) {
    // Returns array of workspace symbols
    json_object* result = json_object_new_array();
    lsp_send_response(id, result);
}

Document* find_document(const char* uri) {
    pthread_mutex_lock(&server.documents_mutex);
    
    Document* doc = server.documents;
    while (doc) {
        if (strcmp(doc->uri, uri) == 0) {
            pthread_mutex_unlock(&server.documents_mutex);
            return doc;
        }
        doc = doc->next;
    }
    
    pthread_mutex_unlock(&server.documents_mutex);
    return NULL;
}

Document* create_document(const char* uri, const char* content) {
    Document* doc = malloc(sizeof(Document));
    if (!doc) {
        return NULL;
    }
    
    strncpy(doc->uri, uri, sizeof(doc->uri) - 1);
    doc->uri[sizeof(doc->uri) - 1] = '\0';
    doc->version = 1;
    doc->content = strdup(content);
    doc->content_length = strlen(content);
    doc->diagnostics = NULL;
    doc->diagnostic_count = 0;
    doc->symbols = NULL;
    doc->symbol_count = 0;
    
    pthread_mutex_lock(&server.documents_mutex);
    doc->next = server.documents;
    server.documents = doc;
    pthread_mutex_unlock(&server.documents_mutex);
    
    return doc;
}

void update_document(const char* uri, const char* content, int version) {
    Document* doc = find_document(uri);
    if (doc) {
        free(doc->content);
        doc->content = strdup(content);
        doc->content_length = strlen(content);
        doc->version = version;
        
        // Clear old analysis results
        free(doc->diagnostics);
        doc->diagnostics = NULL;
        doc->diagnostic_count = 0;
        
        free(doc->symbols);
        doc->symbols = NULL;
        doc->symbol_count = 0;
    }
}

void close_document(const char* uri) {
    pthread_mutex_lock(&server.documents_mutex);
    
    Document** current = &server.documents;
    while (*current) {
        if (strcmp((*current)->uri, uri) == 0) {
            Document* to_remove = *current;
            *current = to_remove->next;
            
            free(to_remove->content);
            free(to_remove->diagnostics);
            free(to_remove->symbols);
            free(to_remove);
            break;
        }
        current = &(*current)->next;
    }
    
    pthread_mutex_unlock(&server.documents_mutex);
}

void analyze_document(Document* doc) {
    if (!doc) return;
    
    log_message("Analyzing document: %s", doc->uri);
    
    // Parse symbols
    parse_document_symbols(doc);
    
    // Check syntax and semantics
    check_document_syntax(doc);
}

void publish_diagnostics(Document* doc) {
    if (!doc) return;
    
    json_object* params = json_object_new_object();
    json_object* uri = json_object_new_string(doc->uri);
    json_object* diagnostics = json_object_new_array();
    
    // Add diagnostics to array
    for (int i = 0; i < doc->diagnostic_count; i++) {
        json_object* diagnostic = json_object_new_object();
        
        // Range
        json_object* range = json_object_new_object();
        json_object* start = json_object_new_object();
        json_object* end = json_object_new_object();
        
        json_object_object_add(start, "line", json_object_new_int(doc->diagnostics[i].range.start.line));
        json_object_object_add(start, "character", json_object_new_int(doc->diagnostics[i].range.start.character));
        json_object_object_add(end, "line", json_object_new_int(doc->diagnostics[i].range.end.line));
        json_object_object_add(end, "character", json_object_new_int(doc->diagnostics[i].range.end.character));
        
        json_object_object_add(range, "start", start);
        json_object_object_add(range, "end", end);
        
        json_object_object_add(diagnostic, "range", range);
        json_object_object_add(diagnostic, "severity", json_object_new_int(doc->diagnostics[i].severity));
        json_object_object_add(diagnostic, "message", json_object_new_string(doc->diagnostics[i].message));
        json_object_object_add(diagnostic, "source", json_object_new_string(doc->diagnostics[i].source));
        
        json_object_array_add(diagnostics, diagnostic);
    }
    
    json_object_object_add(params, "uri", uri);
    json_object_object_add(params, "diagnostics", diagnostics);
    
    lsp_send_notification("textDocument/publishDiagnostics", params);
}

void parse_document_symbols(Document* doc) {
    // Simplified symbol parsing - in a real implementation,
    // this would use the actual Goo parser
    
    if (!doc->content) return;
    
    // Count potential symbols (functions, types, etc.)
    int symbol_capacity = 100;
    doc->symbols = malloc(symbol_capacity * sizeof(SymbolInfo));
    doc->symbol_count = 0;
    
    char* line = strtok(strdup(doc->content), "\n");
    int line_num = 0;
    
    while (line && doc->symbol_count < symbol_capacity) {
        // Look for function declarations
        if (strstr(line, "func ")) {
            char* func_start = strstr(line, "func ");
            char* name_start = func_start + 5;
            char* name_end = strchr(name_start, '(');
            
            if (name_end) {
                int name_len = name_end - name_start;
                if (name_len < MAX_SYMBOL_NAME) {
                    strncpy(doc->symbols[doc->symbol_count].name, name_start, name_len);
                    doc->symbols[doc->symbol_count].name[name_len] = '\0';
                    doc->symbols[doc->symbol_count].kind = 12; // Function
                    doc->symbols[doc->symbol_count].position.line = line_num;
                    doc->symbols[doc->symbol_count].position.character = name_start - line;
                    doc->symbol_count++;
                }
            }
        }
        
        // Look for type declarations
        if (strstr(line, "type ")) {
            char* type_start = strstr(line, "type ");
            char* name_start = type_start + 5;
            char* name_end = strchr(name_start, ' ');
            
            if (name_end) {
                int name_len = name_end - name_start;
                if (name_len < MAX_SYMBOL_NAME) {
                    strncpy(doc->symbols[doc->symbol_count].name, name_start, name_len);
                    doc->symbols[doc->symbol_count].name[name_len] = '\0';
                    doc->symbols[doc->symbol_count].kind = 5; // Class/Type
                    doc->symbols[doc->symbol_count].position.line = line_num;
                    doc->symbols[doc->symbol_count].position.character = name_start - line;
                    doc->symbol_count++;
                }
            }
        }
        
        line = strtok(NULL, "\n");
        line_num++;
    }
}

void check_document_syntax(Document* doc) {
    // Simplified syntax checking - in a real implementation,
    // this would use the actual Goo parser and type checker
    
    if (!doc->content) return;
    
    // Basic syntax checks
    int diagnostic_capacity = 50;
    doc->diagnostics = malloc(diagnostic_capacity * sizeof(Diagnostic));
    doc->diagnostic_count = 0;
    
    // Check for simple syntax errors
    char* content = strdup(doc->content);
    char* line = strtok(content, "\n");
    int line_num = 0;
    
    while (line && doc->diagnostic_count < diagnostic_capacity) {
        // Check for missing semicolons in simple cases
        char* trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        
        if (strlen(trimmed) > 0 && !strchr(trimmed, '{') && !strchr(trimmed, '}') &&
            !strstr(trimmed, "//") && !strstr(trimmed, "import") && !strstr(trimmed, "package")) {
            
            char* end = trimmed + strlen(trimmed) - 1;
            while (end > trimmed && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
                end--;
            }
            
            if (end > trimmed && *end != ';' && *end != '{' && *end != '}') {
                doc->diagnostics[doc->diagnostic_count].range.start.line = line_num;
                doc->diagnostics[doc->diagnostic_count].range.start.character = 0;
                doc->diagnostics[doc->diagnostic_count].range.end.line = line_num;
                doc->diagnostics[doc->diagnostic_count].range.end.character = strlen(line);
                doc->diagnostics[doc->diagnostic_count].severity = 2; // Warning
                strcpy(doc->diagnostics[doc->diagnostic_count].message, "Missing semicolon");
                strcpy(doc->diagnostics[doc->diagnostic_count].source, "goo-lsp");
                doc->diagnostic_count++;
            }
        }
        
        line = strtok(NULL, "\n");
        line_num++;
    }
    
    free(content);
}

void find_completion_items(Document* doc, Position pos, CompletionItem** items, int* count) {
    // Simplified completion - in a real implementation,
    // this would analyze the context and provide relevant suggestions
    
    *count = 5;
    *items = malloc(*count * sizeof(CompletionItem));
    
    // Basic keyword completions
    strcpy((*items)[0].label, "func");
    (*items)[0].kind = 14; // Keyword
    strcpy((*items)[0].detail, "Function declaration");
    strcpy((*items)[0].documentation, "Declares a new function");
    strcpy((*items)[0].insert_text, "func ${1:name}(${2:params}) ${3:return_type} {\n    ${0}\n}");
    
    strcpy((*items)[1].label, "type");
    (*items)[1].kind = 14; // Keyword
    strcpy((*items)[1].detail, "Type declaration");
    strcpy((*items)[1].documentation, "Declares a new type");
    strcpy((*items)[1].insert_text, "type ${1:Name} ${2:definition}");
    
    strcpy((*items)[2].label, "if");
    (*items)[2].kind = 14; // Keyword
    strcpy((*items)[2].detail, "If statement");
    strcpy((*items)[2].documentation, "Conditional statement");
    strcpy((*items)[2].insert_text, "if ${1:condition} {\n    ${0}\n}");
    
    strcpy((*items)[3].label, "for");
    (*items)[3].kind = 14; // Keyword
    strcpy((*items)[3].detail, "For loop");
    strcpy((*items)[3].documentation, "Loop statement");
    strcpy((*items)[3].insert_text, "for ${1:condition} {\n    ${0}\n}");
    
    strcpy((*items)[4].label, "fmt.Println");
    (*items)[4].kind = 3; // Function
    strcpy((*items)[4].detail, "func Println(args ...any) (n int, err error)");
    strcpy((*items)[4].documentation, "Prints arguments to stdout with newline");
    strcpy((*items)[4].insert_text, "fmt.Println(${1:args})");
}

LSPMethod parse_method(const char* method_name) {
    if (strcmp(method_name, "initialize") == 0) return METHOD_INITIALIZE;
    if (strcmp(method_name, "initialized") == 0) return METHOD_INITIALIZED;
    if (strcmp(method_name, "shutdown") == 0) return METHOD_SHUTDOWN;
    if (strcmp(method_name, "exit") == 0) return METHOD_EXIT;
    if (strcmp(method_name, "textDocument/didOpen") == 0) return METHOD_TEXT_DOCUMENT_DID_OPEN;
    if (strcmp(method_name, "textDocument/didChange") == 0) return METHOD_TEXT_DOCUMENT_DID_CHANGE;
    if (strcmp(method_name, "textDocument/didSave") == 0) return METHOD_TEXT_DOCUMENT_DID_SAVE;
    if (strcmp(method_name, "textDocument/didClose") == 0) return METHOD_TEXT_DOCUMENT_DID_CLOSE;
    if (strcmp(method_name, "textDocument/completion") == 0) return METHOD_TEXT_DOCUMENT_COMPLETION;
    if (strcmp(method_name, "textDocument/hover") == 0) return METHOD_TEXT_DOCUMENT_HOVER;
    if (strcmp(method_name, "textDocument/definition") == 0) return METHOD_TEXT_DOCUMENT_DEFINITION;
    if (strcmp(method_name, "textDocument/references") == 0) return METHOD_TEXT_DOCUMENT_REFERENCES;
    if (strcmp(method_name, "textDocument/documentSymbol") == 0) return METHOD_TEXT_DOCUMENT_DOCUMENT_SYMBOL;
    if (strcmp(method_name, "textDocument/formatting") == 0) return METHOD_TEXT_DOCUMENT_FORMATTING;
    if (strcmp(method_name, "textDocument/rangeFormatting") == 0) return METHOD_TEXT_DOCUMENT_RANGE_FORMATTING;
    if (strcmp(method_name, "textDocument/signatureHelp") == 0) return METHOD_TEXT_DOCUMENT_SIGNATURE_HELP;
    if (strcmp(method_name, "workspace/symbol") == 0) return METHOD_WORKSPACE_SYMBOL;
    
    return METHOD_UNKNOWN;
}

void log_message(const char* format, ...) {
    if (!server.trace_level) return;
    
    va_list args;
    va_start(args, format);
    
    fprintf(stderr, "[LSP] ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    fflush(stderr);
    
    va_end(args);
}