#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdbool.h>
#include <time.h>

// Enhanced LSP server with comprehensive Goo language support
typedef struct {
    FILE* input_stream;
    FILE* output_stream;
    bool running;
    bool debug_mode;
    time_t start_time;
    int requests_handled;
} GooLSPServer;

// Document state tracking
typedef struct DocumentState {
    char* uri;
    char* content;
    int version;
    time_t last_modified;
    bool has_errors;
    int error_count;
    struct DocumentState* next;
} DocumentState;

// Goo-specific symbol information
typedef struct GooSymbol {
    char* name;
    char* type;
    char* file_uri;
    int line;
    int character;
    bool is_error_union;
    bool is_nullable;
    bool is_owned;
    bool is_moved;
    struct GooSymbol* next;
} GooSymbol;

static GooLSPServer* g_server = NULL;
static DocumentState* g_documents = NULL;
static GooSymbol* g_symbols = NULL;

// =============================================================================
// Logging and Debugging
// =============================================================================

static void lsp_log(const char* level, const char* format, ...) {
    if (!g_server || !g_server->debug_mode) return;
    
    FILE* log_file = fopen("/tmp/goo-lsp.log", "a");
    if (!log_file) return;
    
    time_t now = time(NULL);
    char* time_str = ctime(&now);
    if (time_str) time_str[strlen(time_str) - 1] = '\0';
    
    fprintf(log_file, "[%s] %s: ", time_str ? time_str : "unknown", level);
    
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    
    fprintf(log_file, "\n");
    fclose(log_file);
}

#define LSP_LOG_INFO(fmt, ...) lsp_log("INFO", fmt, ##__VA_ARGS__)
#define LSP_LOG_ERROR(fmt, ...) lsp_log("ERROR", fmt, ##__VA_ARGS__)
#define LSP_LOG_DEBUG(fmt, ...) lsp_log("DEBUG", fmt, ##__VA_ARGS__)

// =============================================================================
// JSON-RPC Communication
// =============================================================================

static void send_response(int id, const char* result) {
    if (!g_server || !g_server->output_stream) return;
    
    char response[16384];
    int response_len = snprintf(response, sizeof(response),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}", id, result);
    
    fprintf(g_server->output_stream, "Content-Length: %d\r\n\r\n%s", response_len, response);
    fflush(g_server->output_stream);
    
    g_server->requests_handled++;
    LSP_LOG_DEBUG("Sent response for request %d", id);
}

static void send_notification(const char* method, const char* params) {
    if (!g_server || !g_server->output_stream) return;
    
    char response[16384];
    int response_len = snprintf(response, sizeof(response),
        "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params);
    
    fprintf(g_server->output_stream, "Content-Length: %d\r\n\r\n%s", response_len, response);
    fflush(g_server->output_stream);
    
    LSP_LOG_DEBUG("Sent notification: %s", method);
}

static void send_error(int id, int code, const char* message) {
    if (!g_server || !g_server->output_stream) return;
    
    char response[8192];
    int response_len = snprintf(response, sizeof(response),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":%d,\"message\":\"%s\"}}", 
        id, code, message);
    
    fprintf(g_server->output_stream, "Content-Length: %d\r\n\r\n%s", response_len, response);
    fflush(g_server->output_stream);
    
    LSP_LOG_ERROR("Sent error for request %d: %s", id, message);
}

// =============================================================================
// Document Management
// =============================================================================

static DocumentState* find_document(const char* uri) {
    for (DocumentState* doc = g_documents; doc; doc = doc->next) {
        if (doc->uri && strcmp(doc->uri, uri) == 0) {
            return doc;
        }
    }
    return NULL;
}

static DocumentState* create_or_update_document(const char* uri, const char* content, int version) {
    DocumentState* doc = find_document(uri);
    
    if (doc) {
        // Update existing document
        free(doc->content);
        doc->content = strdup(content);
        doc->version = version;
        doc->last_modified = time(NULL);
        doc->has_errors = false;
        doc->error_count = 0;
    } else {
        // Create new document
        doc = malloc(sizeof(DocumentState));
        if (!doc) return NULL;
        
        doc->uri = strdup(uri);
        doc->content = strdup(content);
        doc->version = version;
        doc->last_modified = time(NULL);
        doc->has_errors = false;
        doc->error_count = 0;
        doc->next = g_documents;
        g_documents = doc;
    }
    
    LSP_LOG_INFO("Document %s version %d", uri, version);
    return doc;
}

// =============================================================================
// Goo Language Analysis
// =============================================================================

// Check if a type is an error union (contains '!')
static bool is_error_union_type(const char* type_str) {
    return type_str && strchr(type_str, '!') != NULL;
}

// Check if a type is nullable (contains '?')
static bool is_nullable_type(const char* type_str) {
    return type_str && strchr(type_str, '?') != NULL;
}

// Check if an identifier represents ownership transfer
static bool is_ownership_transfer(const char* line, const char* identifier) {
    if (!line || !identifier) return false;
    
    char* id_pos = strstr(line, identifier);
    if (!id_pos) return false;
    
    // Look for 'move' keyword before the identifier
    char* move_pos = strstr(line, "move");
    if (move_pos && move_pos < id_pos) return true;
    
    // Look for ownership transfer patterns like 'own'
    char* own_pos = strstr(line, "own");
    if (own_pos && own_pos < id_pos) return true;
    
    return false;
}

// Analyze Goo syntax for specific patterns
static void analyze_goo_syntax(const char* content, const char* uri) {
    if (!content) return;
    
    // Clear existing symbols for this file
    GooSymbol** current = &g_symbols;
    while (*current) {
        if ((*current)->file_uri && strcmp((*current)->file_uri, uri) == 0) {
            GooSymbol* to_remove = *current;
            *current = (*current)->next;
            free(to_remove->name);
            free(to_remove->type);
            free(to_remove->file_uri);
            free(to_remove);
        } else {
            current = &(*current)->next;
        }
    }
    
    char* content_copy = strdup(content);
    char* lines[2000];
    int line_count = 0;
    
    // Split into lines
    char* token = strtok(content_copy, "\n");
    while (token && line_count < 2000) {
        lines[line_count++] = token;
        token = strtok(NULL, "\n");
    }
    
    // Analyze each line for Goo patterns
    for (int i = 0; i < line_count; i++) {
        char* line = lines[i];
        
        // Function declarations: fn name() -> Type!ErrorType
        if (strstr(line, "fn ") == line) {
            char* fn_start = line + 3;
            while (*fn_start == ' ') fn_start++;
            
            char* paren_pos = strchr(fn_start, '(');
            if (paren_pos) {
                int name_len = paren_pos - fn_start;
                char* name = malloc(name_len + 1);
                strncpy(name, fn_start, name_len);
                name[name_len] = '\0';
                
                // Find return type
                char* arrow_pos = strstr(paren_pos, "->");
                char* type_str = "void";
                bool is_error_union = false;
                bool is_nullable = false;
                
                if (arrow_pos) {
                    arrow_pos += 2;
                    while (*arrow_pos == ' ') arrow_pos++;
                    
                    char* type_end = arrow_pos;
                    while (*type_end && *type_end != ' ' && *type_end != '{' && *type_end != '\n') {
                        type_end++;
                    }
                    
                    int type_len = type_end - arrow_pos;
                    type_str = malloc(type_len + 1);
                    strncpy(type_str, arrow_pos, type_len);
                    type_str[type_len] = '\0';
                    
                    is_error_union = is_error_union_type(type_str);
                    is_nullable = is_nullable_type(type_str);
                }
                
                // Add function symbol
                GooSymbol* symbol = malloc(sizeof(GooSymbol));
                symbol->name = name;
                symbol->type = strdup(type_str);
                symbol->file_uri = strdup(uri);
                symbol->line = i;
                symbol->character = fn_start - line;
                symbol->is_error_union = is_error_union;
                symbol->is_nullable = is_nullable;
                symbol->is_owned = false;
                symbol->is_moved = false;
                symbol->next = g_symbols;
                g_symbols = symbol;
                
                if (type_str != "void") free(type_str);
            }
        }
        
        // Variable declarations: let/var name: Type! = value
        if (strstr(line, "let ") || strstr(line, "var ")) {
            char* var_start = strstr(line, "let ") ? line + 4 : line + 4;
            while (*var_start == ' ') var_start++;
            
            char* colon_pos = strchr(var_start, ':');
            char* eq_pos = strchr(var_start, '=');
            char* space_pos = strchr(var_start, ' ');
            
            char* name_end = colon_pos;
            if (!name_end || (space_pos && space_pos < name_end)) name_end = space_pos;
            if (!name_end || (eq_pos && eq_pos < name_end)) name_end = eq_pos;
            
            if (name_end && name_end > var_start) {
                int name_len = name_end - var_start;
                char* name = malloc(name_len + 1);
                strncpy(name, var_start, name_len);
                name[name_len] = '\0';
                
                char* type_str = "auto";
                bool is_error_union = false;
                bool is_nullable = false;
                bool is_owned = false;
                bool is_moved = false;
                
                if (colon_pos) {
                    char* type_start = colon_pos + 1;
                    while (*type_start == ' ') type_start++;
                    
                    char* type_end = eq_pos ? eq_pos : type_start + strlen(type_start);
                    while (type_end > type_start && (*(type_end - 1) == ' ' || *(type_end - 1) == '\n')) {
                        type_end--;
                    }
                    
                    int type_len = type_end - type_start;
                    type_str = malloc(type_len + 1);
                    strncpy(type_str, type_start, type_len);
                    type_str[type_len] = '\0';
                    
                    is_error_union = is_error_union_type(type_str);
                    is_nullable = is_nullable_type(type_str);
                }
                
                if (eq_pos) {
                    is_owned = strstr(eq_pos, "own ") != NULL;
                    is_moved = is_ownership_transfer(eq_pos, name);
                }
                
                // Add variable symbol
                GooSymbol* symbol = malloc(sizeof(GooSymbol));
                symbol->name = name;
                symbol->type = strdup(type_str);
                symbol->file_uri = strdup(uri);
                symbol->line = i;
                symbol->character = var_start - line;
                symbol->is_error_union = is_error_union;
                symbol->is_nullable = is_nullable;
                symbol->is_owned = is_owned;
                symbol->is_moved = is_moved;
                symbol->next = g_symbols;
                g_symbols = symbol;
                
                if (type_str != "auto") free(type_str);
            }
        }
    }
    
    free(content_copy);
    LSP_LOG_INFO("Analyzed %s: found symbols in %d lines", uri, line_count);
}

// =============================================================================
// Completion Provider
// =============================================================================

static void add_completion_item_ex(char* result, size_t result_size, size_t* offset, bool* first,
                                   const char* label, int kind, const char* detail,
                                   const char* insert_text, const char* documentation) {
    if (*offset >= result_size - 1) return;
    if (!*first) {
        *offset += snprintf(result + *offset, result_size - *offset, ",");
    }
    if (*offset < result_size - 1) {
        *offset += snprintf(result + *offset, result_size - *offset,
            "{\"label\":\"%s\",\"kind\":%d,\"detail\":\"%s\",\"insertText\":\"%s\",\"documentation\":\"%s\"}",
            label, kind, detail ? detail : "", insert_text ? insert_text : label,
            documentation ? documentation : "");
    }
    *first = false;
}

static void generate_goo_completions(const char* content, int line, int character,
                                     char* result, size_t result_size) {
    size_t offset = snprintf(result, result_size, "{\"isIncomplete\":false,\"items\":[");
    bool first = true;
    
    // Goo-specific keywords with detailed completion info
    struct {
        const char* keyword;
        const char* detail;
        const char* insert_text;
        const char* documentation;
    } goo_keywords[] = {
        {"fn", "Function declaration", "fn ${1:name}(${2:params}) -> ${3:Type} {\\n\\t$0\\n}", "Declare a new function"},
        {"let", "Immutable binding", "let ${1:name}: ${2:Type} = ${3:value}", "Create an immutable variable binding"},
        {"var", "Mutable variable", "var ${1:name}: ${2:Type} = ${3:value}", "Create a mutable variable"},
        {"struct", "Structure definition", "struct ${1:Name} {\\n\\t${2:field}: ${3:Type},\\n}", "Define a new structure"},
        {"interface", "Interface definition", "interface ${1:Name} {\\n\\t${2:method}(${3:params}) -> ${4:Type};\\n}", "Define a new interface"},
        {"enum", "Enumeration", "enum ${1:Name} {\\n\\t${2:Variant},\\n}", "Define an enumeration"},
        {"try", "Error handling", "try ${1:expression}", "Handle potential errors"},
        {"catch", "Error catching", "catch |${1:err}| {\\n\\t$0\\n}", "Catch and handle errors"},
        {"own", "Ownership transfer", "own ${1:value}", "Transfer ownership of a value"},
        {"move", "Move semantics", "move ${1:value}", "Move a value (transfer ownership)"},
        {"defer", "Deferred execution", "defer ${1:cleanup_code};", "Execute code when scope exits"},
        {"go", "Goroutine", "go ${1:function_call}", "Start a new goroutine"},
        {"select", "Channel selection", "select {\\n\\tcase ${1:channel} <- ${2:value}:\\n\\t\\t$0\\n}", "Select on multiple channel operations"}
    };
    
    for (size_t i = 0; i < sizeof(goo_keywords) / sizeof(goo_keywords[0]); i++) {
        add_completion_item_ex(result, result_size, &offset, &first, goo_keywords[i].keyword, 14, // keyword kind
                          goo_keywords[i].detail, goo_keywords[i].insert_text, 
                          goo_keywords[i].documentation);
    }
    
    // Goo built-in types with error union and nullable variants
    struct {
        const char* type;
        const char* detail;
        const char* documentation;
    } goo_types[] = {
        {"int", "Signed integer", "Platform-specific signed integer type"},
        {"int!", "Error union integer", "Integer that may contain an error"},
        {"int?", "Nullable integer", "Integer that may be null"},
        {"string", "String type", "UTF-8 encoded string"},
        {"string!", "Error union string", "String that may contain an error"},
        {"string?", "Nullable string", "String that may be null"},
        {"bool", "Boolean type", "True or false value"},
        {"bool!", "Error union boolean", "Boolean that may contain an error"},
        {"bool?", "Nullable boolean", "Boolean that may be null"},
        {"float64", "64-bit float", "Double precision floating point"},
        {"float64!", "Error union float", "Float that may contain an error"},
        {"float64?", "Nullable float", "Float that may be null"}
    };
    
    for (size_t i = 0; i < sizeof(goo_types) / sizeof(goo_types[0]); i++) {
        add_completion_item_ex(result, result_size, &offset, &first, goo_types[i].type, 25, // type parameter kind
                          goo_types[i].detail, NULL, goo_types[i].documentation);
    }
    
    // Add symbols from current file
    for (GooSymbol* symbol = g_symbols; symbol; symbol = symbol->next) {
        if (symbol->file_uri && strstr(symbol->file_uri, "current")) { // Simple check for current file
            int kind = 6; // Variable kind
            char detail[256];
            char documentation[512];
            
            snprintf(detail, sizeof(detail), "%s%s%s%s", 
                     symbol->type,
                     symbol->is_error_union ? " (error union)" : "",
                     symbol->is_nullable ? " (nullable)" : "",
                     symbol->is_owned ? " (owned)" : "");
            
            snprintf(documentation, sizeof(documentation), 
                     "Symbol: %s\\nType: %s\\n%s%s%s%s",
                     symbol->name, symbol->type,
                     symbol->is_error_union ? "Contains error handling\\n" : "",
                     symbol->is_nullable ? "May be null\\n" : "",
                     symbol->is_owned ? "Owned resource\\n" : "",
                     symbol->is_moved ? "Value has been moved\\n" : "");
            
            add_completion_item_ex(result, result_size, &offset, &first, symbol->name, kind, detail, NULL, documentation);
        }
    }
    
    if (offset < result_size - 1) {
        snprintf(result + offset, result_size - offset, "]}");
    }
}

// =============================================================================
// Diagnostics Provider
// =============================================================================

static void generate_diagnostics(DocumentState* doc, char* diagnostics_json, size_t json_size) {
    size_t offset = snprintf(diagnostics_json, json_size, "[");
    bool first = true;

    if (!doc || !doc->content) {
        if (offset < json_size - 1)
            snprintf(diagnostics_json + offset, json_size - offset, "]");
        return;
    }

    char* content = doc->content;
    char* lines[1000];
    int line_count = 0;

    // Split content into lines
    char* content_copy = strdup(content);
    char* token = strtok(content_copy, "\n");
    while (token && line_count < 1000) {
        lines[line_count++] = token;
        token = strtok(NULL, "\n");
    }

    // Check for Goo-specific issues
    for (int i = 0; i < line_count; i++) {
        char* line = lines[i];

        // Check for unhandled error unions
        if (strstr(line, "!") && !strstr(line, "try") && !strstr(line, "catch")) {
            if (offset >= json_size - 1) break;
            if (!first)
                offset += snprintf(diagnostics_json + offset, json_size - offset, ",");

            if (offset < json_size - 1) {
                offset += snprintf(diagnostics_json + offset, json_size - offset,
                    "{\"range\":{\"start\":{\"line\":%d,\"character\":0},\"end\":{\"line\":%d,\"character\":%d}},"
                    "\"severity\":2,\"source\":\"goo\",\"message\":\"Potential unhandled error union. Consider using 'try' or 'catch'.\"}",
                    i, i, (int)strlen(line));
            }
            first = false;
            doc->error_count++;
        }

        // Check for potential null pointer dereference
        if (strstr(line, "?") && strstr(line, ".") && !strstr(line, "if")) {
            if (offset >= json_size - 1) break;
            if (!first)
                offset += snprintf(diagnostics_json + offset, json_size - offset, ",");

            if (offset < json_size - 1) {
                offset += snprintf(diagnostics_json + offset, json_size - offset,
                    "{\"range\":{\"start\":{\"line\":%d,\"character\":0},\"end\":{\"line\":%d,\"character\":%d}},"
                    "\"severity\":2,\"source\":\"goo\",\"message\":\"Potential null pointer access. Consider null checking.\"}",
                    i, i, (int)strlen(line));
            }
            first = false;
            doc->error_count++;
        }

        // Check for use after move
        for (GooSymbol* symbol = g_symbols; symbol; symbol = symbol->next) {
            if (symbol->is_moved && strstr(line, symbol->name)) {
                if (offset >= json_size - 1) break;
                if (!first)
                    offset += snprintf(diagnostics_json + offset, json_size - offset, ",");

                if (offset < json_size - 1) {
                    offset += snprintf(diagnostics_json + offset, json_size - offset,
                        "{\"range\":{\"start\":{\"line\":%d,\"character\":0},\"end\":{\"line\":%d,\"character\":%d}},"
                        "\"severity\":1,\"source\":\"goo\",\"message\":\"Use after move: '%s' has been moved.\"}",
                        i, i, (int)strlen(line), symbol->name);
                }
                first = false;
                doc->error_count++;
            }
        }
    }

    doc->has_errors = doc->error_count > 0;
    if (offset < json_size - 1) {
        snprintf(diagnostics_json + offset, json_size - offset, "]");
    }
    free(content_copy);
}

// =============================================================================
// LSP Request Handlers
// =============================================================================

static void handle_initialize(int id) {
    const char* capabilities = 
        "{"
        "\"capabilities\":{"
        "\"textDocumentSync\":{"
        "\"openClose\":true,"
        "\"change\":1,"
        "\"save\":{\"includeText\":true}"
        "},"
        "\"completionProvider\":{"
        "\"triggerCharacters\":[\".\"=\"!\"=\"?\"],\"resolveProvider\":false"
        "},"
        "\"hoverProvider\":true,"
        "\"definitionProvider\":true,"
        "\"referencesProvider\":true,"
        "\"documentSymbolProvider\":true,"
        "\"workspaceSymbolProvider\":true,"
        "\"codeActionProvider\":true,"
        "\"diagnosticProvider\":true"
        "},"
        "\"serverInfo\":{"
        "\"name\":\"Goo Language Server Enhanced\","
        "\"version\":\"1.0.0\""
        "}"
        "}";
    
    send_response(id, capabilities);
    LSP_LOG_INFO("Server initialized with enhanced Goo capabilities");
}

static void handle_text_document_did_open(const char* params) {
    // Extract URI and content from params (simplified parsing)
    char* uri_start = strstr(params, "\"uri\":\"");
    char* text_start = strstr(params, "\"text\":\"");
    
    if (!uri_start || !text_start) return;
    
    uri_start += 7; // Skip '\"uri\":\"'
    char* uri_end = strchr(uri_start, '\"');
    
    text_start += 8; // Skip '\"text\":\"'
    char* text_end = strstr(text_start, "\",\"");
    if (!text_end) text_end = strstr(text_start, "\"}");
    
    if (!uri_end || !text_end) return;
    
    int uri_len = uri_end - uri_start;
    int text_len = text_end - text_start;
    
    char* uri = malloc(uri_len + 1);
    char* content = malloc(text_len + 1);
    
    strncpy(uri, uri_start, uri_len);
    uri[uri_len] = '\0';
    
    strncpy(content, text_start, text_len);
    content[text_len] = '\0';
    
    // Create/update document
    DocumentState* doc = create_or_update_document(uri, content, 1);
    
    // Analyze the document
    analyze_goo_syntax(content, uri);
    
    // Generate and send diagnostics
    char diagnostics[8192];
    generate_diagnostics(doc, diagnostics, sizeof(diagnostics));
    
    char notification[16384];
    snprintf(notification, sizeof(notification),
        "{\"uri\":\"%s\",\"diagnostics\":%s}", uri, diagnostics);
    
    send_notification("textDocument/publishDiagnostics", notification);
    
    free(uri);
    free(content);
}

static void handle_completion(int id, const char* params) {
    // Extract position from params (simplified)
    char* line_start = strstr(params, "\"line\":");
    char* char_start = strstr(params, "\"character\":");
    
    int line = 0, character = 0;
    if (line_start) line = atoi(line_start + 7);
    if (char_start) character = atoi(char_start + 12);
    
    char result[16384];
    generate_goo_completions("", line, character, result, sizeof(result));
    
    send_response(id, result);
    LSP_LOG_DEBUG("Completion provided for line %d, char %d", line, character);
}

static void handle_hover(int id, const char* params) {
    // Simplified hover - in real implementation would analyze position and provide type info
    const char* hover_result = 
        "{"
        "\"contents\":{"
        "\"kind\":\"markdown\","
        "\"value\":\"**Goo Language Feature**\\n\\nHover information with type details, error union status, and ownership tracking.\""
        "}"
        "}";
    
    send_response(id, hover_result);
}

// =============================================================================
// Main Server Loop
// =============================================================================

static char* read_request() {
    if (!g_server || !g_server->input_stream) return NULL;
    
    char header[256];
    if (!fgets(header, sizeof(header), g_server->input_stream)) {
        return NULL;
    }
    
    int content_length = 0;
    if (sscanf(header, "Content-Length: %d", &content_length) != 1) {
        return NULL;
    }
    
    // Skip the empty line
    fgets(header, sizeof(header), g_server->input_stream);
    
    if (content_length <= 0 || content_length > 65536) {
        return NULL;
    }
    
    char* content = malloc(content_length + 1);
    if (!content) return NULL;
    
    size_t bytes_read = fread(content, 1, content_length, g_server->input_stream);
    content[bytes_read] = '\0';
    
    return content;
}

static void process_request(const char* request) {
    if (!request) return;
    
    LSP_LOG_DEBUG("Processing request: %.100s...", request);
    
    // Parse JSON-RPC fields (simplified)
    char* id_start = strstr(request, "\"id\":");
    char* method_start = strstr(request, "\"method\":\"");
    char* params_start = strstr(request, "\"params\":");
    
    int id = id_start ? atoi(id_start + 5) : -1;
    
    if (method_start) {
        method_start += 10; // Skip '\"method\":\"'
        char* method_end = strchr(method_start, '\"');
        
        if (method_end) {
            int method_len = method_end - method_start;
            char* method = malloc(method_len + 1);
            strncpy(method, method_start, method_len);
            method[method_len] = '\0';
            
            // Handle different LSP methods
            if (strcmp(method, "initialize") == 0) {
                handle_initialize(id);
            } else if (strcmp(method, "initialized") == 0) {
                // No response needed for initialized notification
            } else if (strcmp(method, "textDocument/didOpen") == 0) {
                handle_text_document_did_open(params_start ? params_start + 9 : "{}");
            } else if (strcmp(method, "textDocument/completion") == 0) {
                handle_completion(id, params_start ? params_start + 9 : "{}");
            } else if (strcmp(method, "textDocument/hover") == 0) {
                handle_hover(id, params_start ? params_start + 9 : "{}");
            } else if (strcmp(method, "shutdown") == 0) {
                send_response(id, "null");
                g_server->running = false;
            } else if (strcmp(method, "exit") == 0) {
                g_server->running = false;
            } else {
                // Unsupported method
                if (id >= 0) {
                    send_error(id, -32601, "Method not found");
                }
            }
            
            free(method);
        }
    }
}

int main(int argc, char* argv[]) {
    // Initialize server
    g_server = malloc(sizeof(GooLSPServer));
    g_server->input_stream = stdin;
    g_server->output_stream = stdout;
    g_server->running = true;
    g_server->debug_mode = false;
    g_server->start_time = time(NULL);
    g_server->requests_handled = 0;
    
    // Check for debug flag
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_server->debug_mode = true;
        }
    }
    
    LSP_LOG_INFO("Goo Language Server Enhanced starting...");
    
    // Main server loop
    while (g_server->running) {
        char* request = read_request();
        if (request) {
            process_request(request);
            free(request);
        } else {
            break;
        }
    }
    
    LSP_LOG_INFO("Server shutting down. Handled %d requests.", g_server->requests_handled);
    
    // Cleanup
    free(g_server);
    return 0;
}