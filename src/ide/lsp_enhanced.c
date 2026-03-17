#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdbool.h>
#include "parser.h"
#include "types.h"
#include "ast.h"

// Enhanced LSP server with AST integration
typedef struct {
    FILE* input_stream;
    FILE* output_stream;
    bool running;
    TypeChecker* type_checker;
    ASTNode* last_parsed_ast;
    char* current_document_uri;
    char* current_document_content;
} EnhancedLSPServer;

// Document management
typedef struct LSPDocument {
    char* uri;
    char* content;
    int version;
    ASTNode* ast;
    Type* cached_types;
    struct LSPDocument* next;
} LSPDocument;

static EnhancedLSPServer* g_server = NULL;
static LSPDocument* g_documents = NULL;

// Utility functions
static void send_response(int id, const char* result) {
    if (!g_server || !g_server->output_stream) return;
    
    char response[8192];
    snprintf(response, sizeof(response),
        "Content-Length: %zu\r\n\r\n"
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}",
        strlen("{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":}") + strlen(result) + 20,
        id, result);
    
    fprintf(g_server->output_stream, "%s", response);
    fflush(g_server->output_stream);
}

static void send_notification(const char* method, const char* params) {
    if (!g_server || !g_server->output_stream) return;
    
    char response[8192];
    snprintf(response, sizeof(response),
        "Content-Length: %zu\r\n\r\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
        strlen("{\"jsonrpc\":\"2.0\",\"method\":\"\",\"params\":}") + strlen(method) + strlen(params),
        method, params);
    
    fprintf(g_server->output_stream, "%s", response);
    fflush(g_server->output_stream);
}

// Document management
static LSPDocument* find_document(const char* uri) {
    for (LSPDocument* doc = g_documents; doc; doc = doc->next) {
        if (doc->uri && strcmp(doc->uri, uri) == 0) {
            return doc;
        }
    }
    return NULL;
}

static LSPDocument* create_document(const char* uri, const char* content, int version) {
    LSPDocument* doc = malloc(sizeof(LSPDocument));
    if (!doc) return NULL;
    
    doc->uri = strdup(uri);
    doc->content = strdup(content);
    doc->version = version;
    doc->ast = NULL;
    doc->cached_types = NULL;
    doc->next = g_documents;
    g_documents = doc;
    
    return doc;
}

static void update_document(LSPDocument* doc, const char* content, int version) {
    if (!doc) return;
    
    free(doc->content);
    doc->content = strdup(content);
    doc->version = version;
    
    // Clear cached AST and types
    if (doc->ast) {
        ast_node_free(doc->ast);
        doc->ast = NULL;
    }
    doc->cached_types = NULL;
}

// AST parsing with error handling
static ASTNode* parse_document_content(const char* content, const char* uri) {
    if (!content || !g_server || !g_server->type_checker) {
        return NULL;
    }
    
    // Initialize parser
    parser_init();
    
    // Parse the content
    int result = parse_input(content, uri);
    if (result != 0) {
        // Parse failed, return NULL
        parser_cleanup();
        return NULL;
    }
    
    // Get the AST root
    ASTNode* ast = ast_root;
    
    // Perform type checking
    if (ast) {
        type_check_program(g_server->type_checker, ast);
    }
    
    parser_cleanup();
    return ast;
}

// Context-aware completion based on AST position
static void get_completion_context(const char* content, int line, int character, 
                                 char* context_type, char* partial_word, char* scope_context) {
    if (!content || !context_type || !partial_word || !scope_context) return;
    
    // Initialize outputs
    strcpy(context_type, "global");
    strcpy(partial_word, "");
    strcpy(scope_context, "");
    
    // Split content into lines
    char* content_copy = strdup(content);
    char* lines[1000];
    int line_count = 0;
    
    char* token = strtok(content_copy, "\n");
    while (token && line_count < 1000) {
        lines[line_count++] = token;
        token = strtok(NULL, "\n");
    }
    
    if (line >= line_count) {
        free(content_copy);
        return;
    }
    
    char* current_line = lines[line];
    int len = strlen(current_line);
    
    // Extract partial word at cursor position
    int start = character;
    while (start > 0 && (isalnum(current_line[start - 1]) || current_line[start - 1] == '_')) {
        start--;
    }
    
    int end = character;
    while (end < len && (isalnum(current_line[end]) || current_line[end] == '_')) {
        end++;
    }
    
    if (end > start) {
        strncpy(partial_word, current_line + start, end - start);
        partial_word[end - start] = '\0';
    }
    
    // Determine context type
    if (strstr(current_line, "fn ") == current_line || strstr(current_line, "func ") == current_line) {
        strcpy(context_type, "function_decl");
    } else if (strstr(current_line, "let ") || strstr(current_line, "var ")) {
        strcpy(context_type, "variable_decl");
    } else if (strstr(current_line, "struct ")) {
        strcpy(context_type, "struct_decl");
    } else if (strstr(current_line, "interface ")) {
        strcpy(context_type, "interface_decl");
    } else if (character > 0 && current_line[character - 1] == '.') {
        strcpy(context_type, "member_access");
    } else if (strstr(current_line, "import ")) {
        strcpy(context_type, "import_stmt");
    } else {
        strcpy(context_type, "expression");
    }
    
    // Determine scope context by looking at previous lines
    for (int i = line - 1; i >= 0; i--) {
        if (strstr(lines[i], "fn ") && strchr(lines[i], '{')) {
            strcpy(scope_context, "function");
            break;
        }
        if (strstr(lines[i], "struct ") && strchr(lines[i], '{')) {
            strcpy(scope_context, "struct");
            break;
        }
        if (strstr(lines[i], "interface ") && strchr(lines[i], '{')) {
            strcpy(scope_context, "interface");
            break;
        }
    }
    
    free(content_copy);
}

// Helper function to add completion item
static void add_completion_item_ex(char* result, size_t result_size, size_t* offset, bool* first,
                                   const char* label, const char* kind_str, const char* detail, const char* insert_text) {
    if (*offset >= result_size - 1) return;
    if (!*first) {
        *offset += snprintf(result + *offset, result_size - *offset, ",");
    }
    if (*offset < result_size - 1) {
        *offset += snprintf(result + *offset, result_size - *offset,
            "{\"label\":\"%s\",\"kind\":%s,\"detail\":\"%s\",\"insertText\":\"%s\"}",
            label, kind_str, detail, insert_text ? insert_text : label);
    }
    *first = false;
}

// Generate intelligent completions based on context
static void generate_intelligent_completions(const char* context_type, const char* partial_word,
                                           const char* scope_context, char* result, size_t result_size) {
    if (!context_type || !partial_word || !scope_context || !result) return;

    size_t offset = snprintf(result, result_size, "{\"isIncomplete\":false,\"items\":[");

    bool first = true;
    
    // Goo language keywords
    const char* keywords[] = {
        "fn", "let", "var", "const", "struct", "interface", "enum", "type",
        "import", "export", "package", "if", "else", "for", "while", "break",
        "continue", "return", "defer", "go", "select", "switch", "case", "default",
        "pub", "priv", "mut", "own", "move", "copy", "unsafe", "extern", "inline",
        "true", "false", "null", "void"
    };
    
    // Goo builtin types
    const char* types[] = {
        "int", "int8", "int16", "int32", "int64",
        "uint", "uint8", "uint16", "uint32", "uint64",
        "float", "float32", "float64", "bool", "string", "char"
    };
    
    // Goo builtin functions
    const char* builtins[] = {
        "print", "println", "len", "cap", "make", "new", "append", "copy",
        "panic", "recover", "close"
    };
    
    // Context-specific completions
    if (strcmp(context_type, "function_decl") == 0) {
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            if (strlen(partial_word) == 0 || strstr(types[i], partial_word) == types[i]) {
                add_completion_item_ex(result, result_size, &offset, &first, types[i], "25", "Built-in type", NULL);
            }
        }
    } else if (strcmp(context_type, "variable_decl") == 0) {
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            if (strlen(partial_word) == 0 || strstr(types[i], partial_word) == types[i]) {
                add_completion_item_ex(result, result_size, &offset, &first, types[i], "25", "Built-in type", NULL);
            }
        }
    } else if (strcmp(context_type, "import_stmt") == 0) {
        // Package suggestions
        add_completion_item_ex(result, result_size, &offset, &first, "fmt", "9", "Goo package", NULL);
        add_completion_item_ex(result, result_size, &offset, &first, "os", "9", "Goo package", NULL);
        add_completion_item_ex(result, result_size, &offset, &first, "io", "9", "Goo package", NULL);
        add_completion_item_ex(result, result_size, &offset, &first, "math", "9", "Goo package", NULL);
        add_completion_item_ex(result, result_size, &offset, &first, "time", "9", "Goo package", NULL);
    } else if (strcmp(context_type, "member_access") == 0) {
        // Common methods/fields
        add_completion_item_ex(result, result_size, &offset, &first, "len", "2", "Property", NULL);
        add_completion_item_ex(result, result_size, &offset, &first, "cap", "2", "Property", NULL);
        add_completion_item_ex(result, result_size, &offset, &first, "toString", "2", "Method", "toString()");
        add_completion_item_ex(result, result_size, &offset, &first, "clone", "2", "Method", "clone()");
    } else {
        // General context completions
        
        // Keywords
        for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
            if (strlen(partial_word) == 0 || strstr(keywords[i], partial_word) == keywords[i]) {
                add_completion_item_ex(result, result_size, &offset, &first, keywords[i], "14", "Goo keyword", NULL);
            }
        }
        
        // Types
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            if (strlen(partial_word) == 0 || strstr(types[i], partial_word) == types[i]) {
                add_completion_item_ex(result, result_size, &offset, &first, types[i], "25", "Built-in type", NULL);
            }
        }
        
        // Builtin functions
        for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
            if (strlen(partial_word) == 0 || strstr(builtins[i], partial_word) == builtins[i]) {
                char insert_text[64];
                snprintf(insert_text, sizeof(insert_text), "%s($1)", builtins[i]);
                add_completion_item_ex(result, result_size, &offset, &first, builtins[i], "3", "Built-in function", insert_text);
            }
        }
        
        // Goo-specific features
        if (strlen(partial_word) == 0 || strstr("try", partial_word) == "try") {
            add_completion_item_ex(result, result_size, &offset, &first, "try", "14", "Goo keyword", "try ");
        }
        if (strlen(partial_word) == 0 || strstr("catch", partial_word) == "catch") {
            add_completion_item_ex(result, result_size, &offset, &first, "catch", "14", "Goo keyword", "catch |$1| {$2}");
        }
        if (strlen(partial_word) == 0 || strstr("match", partial_word) == "match") {
            add_completion_item_ex(result, result_size, &offset, &first, "match", "14", "Goo keyword", "match $1 {$2}");
        }
        
        // Error union and nullable types
        add_completion_item_ex(result, result_size, &offset, &first, "!T", "15", "Error union type", "!${1:T}");
        add_completion_item_ex(result, result_size, &offset, &first, "?T", "15", "Nullable type", "?${1:T}");
        
        // Channel operations
        add_completion_item_ex(result, result_size, &offset, &first, "<-", "24", "Channel receive", "<-");
        add_completion_item_ex(result, result_size, &offset, &first, "chan", "14", "Channel type", "chan ${1:T}");
        
        // Ownership qualifiers
        if (strlen(partial_word) == 0 || strstr("owned", partial_word) == "owned") {
            add_completion_item_ex(result, result_size, &offset, &first, "owned", "14", "Ownership qualifier", "owned ");
        }
        if (strlen(partial_word) == 0 || strstr("borrowed", partial_word) == "borrowed") {
            add_completion_item_ex(result, result_size, &offset, &first, "borrowed", "14", "Ownership qualifier", "borrowed ");
        }
        if (strlen(partial_word) == 0 || strstr("shared", partial_word) == "shared") {
            add_completion_item_ex(result, result_size, &offset, &first, "shared", "14", "Ownership qualifier", "shared ");
        }
    }
    
    // Common code snippets
    if (strcmp(scope_context, "function") == 0) {
        add_completion_item_ex(result, result_size, &offset, &first, "if_stmt", "15", "if statement", "if ${1:condition} {\\n\\t$2\\n}");
        add_completion_item_ex(result, result_size, &offset, &first, "for_loop", "15", "for loop", "for ${1:i} := 0; ${1:i} < ${2:n}; ${1:i}++ {\\n\\t$3\\n}");
        add_completion_item_ex(result, result_size, &offset, &first, "while_loop", "15", "while loop", "while ${1:condition} {\\n\\t$2\\n}");
        add_completion_item_ex(result, result_size, &offset, &first, "match_expr", "15", "match expression", "match ${1:expr} {\\n\\t${2:pattern} => ${3:result},\\n}");
    }
    
    if (strcmp(context_type, "global") == 0) {
        add_completion_item_ex(result, result_size, &offset, &first, "fn_decl", "15", "function declaration", "fn ${1:name}(${2:params}) ${3:return_type} {\\n\\t$4\\n}");
        add_completion_item_ex(result, result_size, &offset, &first, "struct_decl", "15", "struct declaration", "struct ${1:Name} {\\n\\t${2:field}: ${3:type},\\n}");
        add_completion_item_ex(result, result_size, &offset, &first, "interface_decl", "15", "interface declaration", "interface ${1:Name} {\\n\\t${2:method}(${3:params}) ${4:return_type}\\n}");
    }
    
    if (offset < result_size - 1) {
        snprintf(result + offset, result_size - offset, "]}");
    }
}

// Enhanced completion handler
static void handle_completion(int id, const char* params) {
    // Parse completion params to extract position and document URI
    char uri[512] = "";
    int line = 0, character = 0;
    
    // Simple parameter parsing (in real implementation, use JSON parser)
    const char* uri_start = strstr(params, "\"uri\":\"");
    if (uri_start) {
        uri_start += 7;
        const char* uri_end = strchr(uri_start, '"');
        if (uri_end) {
            size_t uri_len = uri_end - uri_start;
            if (uri_len < sizeof(uri) - 1) {
                strncpy(uri, uri_start, uri_len);
                uri[uri_len] = '\0';
            }
        }
    }
    
    const char* line_start = strstr(params, "\"line\":");
    if (line_start) {
        sscanf(line_start + 7, "%d", &line);
    }
    
    const char* char_start = strstr(params, "\"character\":");
    if (char_start) {
        sscanf(char_start + 12, "%d", &character);
    }
    
    // Find the document
    LSPDocument* doc = find_document(uri);
    if (!doc) {
        // Return basic completions
        const char* basic_result = "{"
            "\"isIncomplete\":false,"
            "\"items\":["
                "{\"label\":\"fn\",\"kind\":14,\"detail\":\"Goo keyword\"},"
                "{\"label\":\"let\",\"kind\":14,\"detail\":\"Goo keyword\"},"
                "{\"label\":\"var\",\"kind\":14,\"detail\":\"Goo keyword\"},"
                "{\"label\":\"struct\",\"kind\":14,\"detail\":\"Goo keyword\"}"
            "]"
        "}";
        send_response(id, basic_result);
        return;
    }
    
    // Get completion context
    char context_type[64];
    char partial_word[64];
    char scope_context[64];
    
    get_completion_context(doc->content, line, character, context_type, partial_word, scope_context);
    
    // Generate intelligent completions
    char result[4096];
    generate_intelligent_completions(context_type, partial_word, scope_context, result, sizeof(result));
    
    send_response(id, result);
}

// Enhanced hover handler
static void handle_hover(int id, const char* params) {
    // Parse hover params
    char uri[512] = "";
    int line = 0, character = 0;
    
    const char* uri_start = strstr(params, "\"uri\":\"");
    if (uri_start) {
        uri_start += 7;
        const char* uri_end = strchr(uri_start, '"');
        if (uri_end) {
            size_t uri_len = uri_end - uri_start;
            if (uri_len < sizeof(uri) - 1) {
                strncpy(uri, uri_start, uri_len);
                uri[uri_len] = '\0';
            }
        }
    }
    
    const char* line_start = strstr(params, "\"line\":");
    if (line_start) {
        sscanf(line_start + 7, "%d", &line);
    }
    
    const char* char_start = strstr(params, "\"character\":");
    if (char_start) {
        sscanf(char_start + 12, "%d", &character);
    }
    
    // Find the document
    LSPDocument* doc = find_document(uri);
    if (!doc) {
        send_response(id, "null");
        return;
    }
    
    // Parse the document to get type information
    if (!doc->ast) {
        doc->ast = parse_document_content(doc->content, doc->uri);
    }
    
    // For now, provide basic hover information
    // In a full implementation, we would traverse the AST to find the symbol at the position
    const char* hover_result = "{"
        "\"contents\":{"
            "\"kind\":\"markdown\","
            "\"value\":\"**Goo Language**\\n\\nHover information for symbol at this position.\\n\\n"
                      "- Language: Goo\\n"
                      "- Features: Error unions, nullable types, ownership tracking\\n"
                      "- Type: To be determined from AST analysis\""
        "}"
    "}";
    
    send_response(id, hover_result);
}

// Message dispatcher
static void handle_request(const char* method, int id, const char* params) {
    if (strcmp(method, "initialize") == 0) {
        const char* capabilities = "{"
            "\"capabilities\":{"
                "\"textDocumentSync\":1,"
                "\"completionProvider\":{"
                    "\"resolveProvider\":false,"
                    "\"triggerCharacters\":[\".\",,\"(\",\"[\",\"{\"]\""
                "},"
                "\"hoverProvider\":true,"
                "\"definitionProvider\":true,"
                "\"referencesProvider\":true,"
                "\"documentHighlightProvider\":true,"
                "\"documentSymbolProvider\":true,"
                "\"workspaceSymbolProvider\":true,"
                "\"codeActionProvider\":true,"
                "\"documentFormattingProvider\":true"
            "},"
            "\"serverInfo\":{"
                "\"name\":\"goo-language-server\","
                "\"version\":\"1.0.0\""
            "}"
        "}";
        send_response(id, capabilities);
    } else if (strcmp(method, "textDocument/completion") == 0) {
        handle_completion(id, params);
    } else if (strcmp(method, "textDocument/hover") == 0) {
        handle_hover(id, params);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        // Go-to-definition placeholder
        send_response(id, "null");
    } else if (strcmp(method, "textDocument/references") == 0) {
        // Find references placeholder
        send_response(id, "[]");
    } else {
        // Unsupported method
        char error_response[512];
        snprintf(error_response, sizeof(error_response),
            "{\"error\":{\"code\":-32601,\"message\":\"Method not found: %s\"}}", method);
        send_response(id, error_response);
    }
}

static void handle_notification(const char* method, const char* params) {
    if (strcmp(method, "textDocument/didOpen") == 0) {
        // Parse didOpen params
        char uri[512] = "";
        char text[8192] = "";
        int version = 1;
        
        // Simple parsing (use proper JSON parser in production)
        const char* uri_start = strstr(params, "\"uri\":\"");
        if (uri_start) {
            uri_start += 7;
            const char* uri_end = strchr(uri_start, '"');
            if (uri_end) {
                size_t uri_len = uri_end - uri_start;
                if (uri_len < sizeof(uri) - 1) {
                    strncpy(uri, uri_start, uri_len);
                    uri[uri_len] = '\0';
                }
            }
        }
        
        const char* text_start = strstr(params, "\"text\":\"");
        if (text_start) {
            text_start += 8;
            const char* text_end = strstr(text_start, "\",\"");
            if (!text_end) text_end = strstr(text_start, "\"}");
            if (text_end) {
                size_t text_len = text_end - text_start;
                if (text_len < sizeof(text) - 1) {
                    strncpy(text, text_start, text_len);
                    text[text_len] = '\0';
                }
            }
        }
        
        create_document(uri, text, version);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        // Handle document changes
        char uri[512] = "";
        char text[8192] = "";
        int version = 1;
        
        // Parse change params (simplified)
        const char* uri_start = strstr(params, "\"uri\":\"");
        if (uri_start) {
            uri_start += 7;
            const char* uri_end = strchr(uri_start, '"');
            if (uri_end) {
                size_t uri_len = uri_end - uri_start;
                if (uri_len < sizeof(uri) - 1) {
                    strncpy(uri, uri_start, uri_len);
                    uri[uri_len] = '\0';
                }
            }
        }
        
        LSPDocument* doc = find_document(uri);
        if (doc) {
            // In a full implementation, apply incremental changes
            // For now, we'll wait for the full document sync
        }
    }
}

// Main message loop
static void process_message(const char* content) {
    if (!content) return;
    
    // Parse JSON-RPC message (simplified)
    char method[128] = "";
    int id = -1;
    char params[4096] = "";
    
    const char* method_start = strstr(content, "\"method\":\"");
    if (method_start) {
        method_start += 10;
        const char* method_end = strchr(method_start, '"');
        if (method_end) {
            size_t method_len = method_end - method_start;
            if (method_len < sizeof(method) - 1) {
                strncpy(method, method_start, method_len);
                method[method_len] = '\0';
            }
        }
    }
    
    const char* id_start = strstr(content, "\"id\":");
    if (id_start) {
        sscanf(id_start + 5, "%d", &id);
    }
    
    const char* params_start = strstr(content, "\"params\":");
    if (params_start) {
        params_start += 9;
        // Find the matching closing brace/bracket
        int depth = 0;
        const char* p = params_start;
        const char* params_end = NULL;
        
        while (*p) {
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) {
                    params_end = p + 1;
                    break;
                }
            }
            p++;
        }
        
        if (params_end) {
            size_t params_len = params_end - params_start;
            if (params_len < sizeof(params) - 1) {
                strncpy(params, params_start, params_len);
                params[params_len] = '\0';
            }
        }
    }
    
    if (strlen(method) > 0) {
        if (id >= 0) {
            handle_request(method, id, params);
        } else {
            handle_notification(method, params);
        }
    }
}

// LSP server initialization and main loop
bool lsp_enhanced_server_init(FILE* input, FILE* output) {
    g_server = malloc(sizeof(EnhancedLSPServer));
    if (!g_server) return false;
    
    g_server->input_stream = input;
    g_server->output_stream = output;
    g_server->running = true;
    g_server->type_checker = type_checker_new();
    g_server->last_parsed_ast = NULL;
    g_server->current_document_uri = NULL;
    g_server->current_document_content = NULL;
    
    if (!g_server->type_checker) {
        free(g_server);
        g_server = NULL;
        return false;
    }
    
    // Initialize builtin types and functions
    type_checker_init_builtins(g_server->type_checker);
    type_checker_add_builtin_functions(g_server->type_checker);
    
    return true;
}

void lsp_enhanced_server_run() {
    if (!g_server) return;
    
    char buffer[8192];
    char content[8192];
    
    while (g_server->running && fgets(buffer, sizeof(buffer), g_server->input_stream)) {
        // Handle Content-Length header
        if (strncmp(buffer, "Content-Length:", 15) == 0) {
            int content_length = atoi(buffer + 16);
            
            // Read the empty line
            fgets(buffer, sizeof(buffer), g_server->input_stream);
            
            // Read the JSON content
            if (content_length > 0 && content_length < sizeof(content) - 1) {
                size_t read_bytes = fread(content, 1, content_length, g_server->input_stream);
                content[read_bytes] = '\0';
                
                // Process the message
                process_message(content);
            }
        }
    }
}

void lsp_enhanced_server_shutdown() {
    if (!g_server) return;
    
    g_server->running = false;
    
    if (g_server->type_checker) {
        type_checker_free(g_server->type_checker);
    }
    
    if (g_server->last_parsed_ast) {
        ast_node_free(g_server->last_parsed_ast);
    }
    
    free(g_server->current_document_uri);
    free(g_server->current_document_content);
    
    // Free documents
    while (g_documents) {
        LSPDocument* next = g_documents->next;
        free(g_documents->uri);
        free(g_documents->content);
        if (g_documents->ast) {
            ast_node_free(g_documents->ast);
        }
        free(g_documents);
        g_documents = next;
    }
    
    free(g_server);
    g_server = NULL;
}

// Main entry point
int main(int argc, char* argv[]) {
    if (!lsp_enhanced_server_init(stdin, stdout)) {
        fprintf(stderr, "Failed to initialize enhanced LSP server\n");
        return 1;
    }
    
    lsp_enhanced_server_run();
    lsp_enhanced_server_shutdown();
    
    return 0;
}