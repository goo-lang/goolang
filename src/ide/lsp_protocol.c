#include "lsp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

// =============================================================================
// JSON-RPC Message Handling
// =============================================================================

LSPMessage* lsp_message_parse(const char* json) {
    if (!json) return NULL;
    
    LSPMessage* msg = calloc(1, sizeof(LSPMessage));
    if (!msg) return NULL;
    
    // Parse basic fields
    msg->jsonrpc = lsp_json_get_string(json, "jsonrpc");
    msg->id = lsp_json_get_string(json, "id");
    msg->method = lsp_json_get_string(json, "method");
    msg->params = lsp_json_get_string(json, "params");
    msg->result = lsp_json_get_string(json, "result");
    msg->error = lsp_json_get_string(json, "error");
    
    return msg;
}

void lsp_message_free(LSPMessage* message) {
    if (!message) return;
    
    free(message->jsonrpc);
    free(message->id);
    free(message->method);
    free(message->params);
    free(message->result);
    free(message->error);
    free(message);
}

char* lsp_message_create_response(const char* id, const char* result) {
    if (!id || !result) return NULL;
    
    size_t len = strlen(id) + strlen(result) + 100;
    char* response = malloc(len);
    if (response) {
        snprintf(response, len, 
                "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
                id, result);
    }
    return response;
}

char* lsp_message_create_error(const char* id, int code, const char* message) {
    if (!message) return NULL;
    
    char* escaped_msg = lsp_escape_json_string(message);
    if (!escaped_msg) return NULL;
    
    size_t len = (id ? strlen(id) : 4) + strlen(escaped_msg) + 200;
    char* response = malloc(len);
    if (response) {
        snprintf(response, len,
                "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":%s}}",
                id ? id : "null", code, escaped_msg);
    }
    
    free(escaped_msg);
    return response;
}

char* lsp_message_create_notification(const char* method, const char* params) {
    if (!method) return NULL;
    
    char* escaped_method = lsp_escape_json_string(method);
    if (!escaped_method) return NULL;
    
    size_t len = strlen(escaped_method) + (params ? strlen(params) : 4) + 100;
    char* notification = malloc(len);
    if (notification) {
        snprintf(notification, len,
                "{\"jsonrpc\":\"2.0\",\"method\":%s,\"params\":%s}",
                escaped_method, params ? params : "null");
    }
    
    free(escaped_method);
    return notification;
}

// =============================================================================
// Communication
// =============================================================================

int lsp_read_message(FILE* input, char** content, size_t* length) {
    if (!input || !content || !length) return -1;
    
    char header_line[1024];
    size_t content_length = 0;
    
    // Read headers
    while (fgets(header_line, sizeof(header_line), input)) {
        // Remove trailing newline
        size_t line_len = strlen(header_line);
        if (line_len > 0 && header_line[line_len - 1] == '\n') {
            header_line[line_len - 1] = '\0';
            line_len--;
        }
        if (line_len > 0 && header_line[line_len - 1] == '\r') {
            header_line[line_len - 1] = '\0';
            line_len--;
        }
        
        // Empty line signals end of headers
        if (line_len == 0) break;
        
        // Parse Content-Length header
        if (strncmp(header_line, "Content-Length:", 15) == 0) {
            content_length = atoi(header_line + 15);
        }
    }
    
    if (content_length == 0) {
        lsp_log_error("No Content-Length header found");
        return -1;
    }
    
    // Read content
    *content = malloc(content_length + 1);
    if (!*content) {
        lsp_log_error("Failed to allocate memory for message content");
        return -1;
    }
    
    size_t bytes_read = fread(*content, 1, content_length, input);
    if (bytes_read != content_length) {
        lsp_log_error("Failed to read complete message: expected %zu, got %zu", 
                     content_length, bytes_read);
        free(*content);
        *content = NULL;
        return -1;
    }
    
    (*content)[content_length] = '\0';
    *length = content_length;
    
    return 0;
}

int lsp_write_message(FILE* output, const char* content) {
    if (!output || !content) return -1;
    
    size_t content_length = strlen(content);
    
    // Write headers
    fprintf(output, "Content-Length: %zu\r\n\r\n", content_length);
    
    // Write content
    size_t written = fwrite(content, 1, content_length, output);
    fflush(output);
    
    if (written != content_length) {
        lsp_log_error("Failed to write complete message");
        return -1;
    }
    
    return 0;
}

int lsp_send_response(LSPServer* server, const char* id, const char* result) {
    if (!server || !id || !result) return -1;
    
    char* response = lsp_message_create_response(id, result);
    if (!response) return -1;
    
    int ret = lsp_write_message(server->output_stream, response);
    free(response);
    
    return ret;
}

int lsp_send_error(LSPServer* server, const char* id, int code, const char* message) {
    if (!server || !message) return -1;
    
    char* error_response = lsp_message_create_error(id, code, message);
    if (!error_response) return -1;
    
    int ret = lsp_write_message(server->output_stream, error_response);
    free(error_response);
    
    return ret;
}

int lsp_send_notification(LSPServer* server, const char* method, const char* params) {
    if (!server || !method) return -1;
    
    char* notification = lsp_message_create_notification(method, params);
    if (!notification) return -1;
    
    int ret = lsp_write_message(server->output_stream, notification);
    free(notification);
    
    return ret;
}

// =============================================================================
// Protocol Handlers
// =============================================================================

int lsp_handle_initialize(LSPServer* server, const char* request_id, const char* params) {
    if (!server || !request_id) return -1;
    
    lsp_log_info("Handling initialize request");
    
    // Parse initialization parameters
    if (params) {
        char* root_uri = lsp_json_get_string(params, "rootUri");
        char* root_path = lsp_json_get_string(params, "rootPath");
        
        if (root_uri) {
            free(server->root_uri);
            server->root_uri = root_uri;
        }
        
        if (root_path) {
            free(server->root_path);
            server->root_path = root_path;
        }
    }
    
    // Create capabilities response
    char capabilities[] = "{"
        "\"capabilities\":{"
            "\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
            "\"completionProvider\":{\"triggerCharacters\":[\".\"]},"
            "\"hoverProvider\":true,"
            "\"definitionProvider\":true,"
            "\"referencesProvider\":true,"
            "\"documentSymbolProvider\":true,"
            "\"workspaceSymbolProvider\":true"
        "},"
        "\"serverInfo\":{\"name\":\"Goo Language Server\",\"version\":\"1.0.0\"}"
    "}";
    
    return lsp_send_response(server, request_id, capabilities);
}

int lsp_handle_initialized(LSPServer* server) {
    if (!server) return -1;
    
    lsp_log_info("Server initialized");
    server->running = true;
    
    return 0;
}

int lsp_handle_shutdown(LSPServer* server, const char* request_id) {
    if (!server || !request_id) return -1;
    
    lsp_log_info("Handling shutdown request");
    server->running = false;
    
    return lsp_send_response(server, request_id, "null");
}

int lsp_handle_exit(LSPServer* server) {
    if (!server) return -1;
    
    lsp_log_info("Handling exit notification");
    server->running = false;
    
    return 0;
}

int lsp_handle_text_document_did_open(LSPServer* server, const char* params) {
    if (!server || !params) return -1;
    
    lsp_log_debug("Handling textDocument/didOpen");
    
    // Parse document parameters
    char* uri = lsp_json_get_string(params, "uri");
    char* language_id = lsp_json_get_string(params, "languageId");
    char* text = lsp_json_get_string(params, "text");
    double version = lsp_json_get_number(params, "version");
    
    if (!uri || !text) {
        lsp_log_error("Missing required parameters for didOpen");
        free(uri);
        free(language_id);
        free(text);
        return -1;
    }
    
    // Open document
    LSPDocument* doc = lsp_server_open_document(server, uri, language_id, 
                                                (uint32_t)version, text);
    
    if (doc) {
        // Analyze document and publish diagnostics
        if (lsp_document_analyze(doc, server->global_type_checker)) {
            LSPDiagnostic* diagnostics = lsp_create_diagnostics(doc, server->error_context);
            lsp_publish_diagnostics(server, uri, diagnostics);
            lsp_diagnostic_list_free(diagnostics);
        }
    }
    
    free(uri);
    free(language_id);
    free(text);
    
    return 0;
}

int lsp_handle_text_document_did_change(LSPServer* server, const char* params) {
    if (!server || !params) return -1;
    
    lsp_log_debug("Handling textDocument/didChange");
    
    char* uri = lsp_json_get_string(params, "uri");
    double version = lsp_json_get_number(params, "version");
    char* text = lsp_json_get_string(params, "text");
    
    if (!uri) {
        lsp_log_error("Missing URI in didChange");
        return -1;
    }
    
    LSPDocument* doc = lsp_server_get_document(server, uri);
    if (!doc) {
        lsp_log_error("Document not found: %s", uri);
        free(uri);
        free(text);
        return -1;
    }
    
    // Update document
    if (text) {
        lsp_document_update_text(doc, (uint32_t)version, text);
        
        // Re-analyze and publish diagnostics
        if (lsp_document_analyze(doc, server->global_type_checker)) {
            LSPDiagnostic* diagnostics = lsp_create_diagnostics(doc, server->error_context);
            lsp_publish_diagnostics(server, uri, diagnostics);
            lsp_diagnostic_list_free(diagnostics);
        }
    }
    
    free(uri);
    free(text);
    
    return 0;
}

int lsp_handle_text_document_did_close(LSPServer* server, const char* params) {
    if (!server || !params) return -1;
    
    lsp_log_debug("Handling textDocument/didClose");
    
    char* uri = lsp_json_get_string(params, "uri");
    if (!uri) {
        lsp_log_error("Missing URI in didClose");
        return -1;
    }
    
    lsp_server_close_document(server, uri);
    free(uri);
    
    return 0;
}

int lsp_handle_completion(LSPServer* server, const char* request_id, const char* params) {
    if (!server || !request_id || !params) return -1;
    
    lsp_log_debug("Handling textDocument/completion");
    
    char* uri = lsp_json_get_string(params, "uri");
    if (!uri) {
        return lsp_send_error(server, request_id, -32602, "Missing textDocument URI");
    }
    
    LSPDocument* doc = lsp_server_get_document(server, uri);
    if (!doc) {
        free(uri);
        return lsp_send_error(server, request_id, -32603, "Document not found");
    }
    
    // Parse position - simplified for now
    LSPPosition position = {0, 0}; // Would parse from params in real implementation
    
    // Get completion items
    LSPCompletionItem* items = lsp_provide_completion(doc, position, server->global_type_checker);
    
    // Create response - simplified JSON
    char response[4096] = "{\"items\":[";
    
    if (items) {
        strcat(response, "{\"label\":\"example\",\"kind\":6}");
    }
    
    strcat(response, "]}");
    
    lsp_completion_list_free(items);
    free(uri);
    
    return lsp_send_response(server, request_id, response);
}

int lsp_handle_hover(LSPServer* server, const char* request_id, const char* params) {
    if (!server || !request_id || !params) return -1;
    
    lsp_log_debug("Handling textDocument/hover");
    
    char* uri = lsp_json_get_string(params, "uri");
    if (!uri) {
        return lsp_send_error(server, request_id, -32602, "Missing textDocument URI");
    }
    
    LSPDocument* doc = lsp_server_get_document(server, uri);
    if (!doc) {
        free(uri);
        return lsp_send_error(server, request_id, -32603, "Document not found");
    }
    
    // Parse position and provide hover
    LSPPosition position = {0, 0}; // Would parse from params
    LSPHover* hover = lsp_provide_hover(doc, position, server->global_type_checker);
    
    char response[1024];
    if (hover && hover->contents) {
        char* escaped_contents = lsp_escape_json_string(hover->contents);
        snprintf(response, sizeof(response), 
                "{\"contents\":{\"kind\":\"markdown\",\"value\":%s}}", 
                escaped_contents);
        free(escaped_contents);
    } else {
        strcpy(response, "null");
    }
    
    lsp_hover_free(hover);
    free(uri);
    
    return lsp_send_response(server, request_id, response);
}

// =============================================================================
// Main server loop
// =============================================================================

int lsp_server_run(LSPServer* server) {
    if (!server) return -1;
    
    lsp_log_info("Starting Goo Language Server");
    
    char* message_content = NULL;
    size_t message_length = 0;
    
    while (true) {
        // Read message
        if (lsp_read_message(server->input_stream, &message_content, &message_length) != 0) {
            if (feof(server->input_stream)) {
                lsp_log_info("Client disconnected");
                break;
            }
            lsp_log_error("Failed to read message");
            continue;
        }
        
        // Parse message
        LSPMessage* msg = lsp_message_parse(message_content);
        if (!msg) {
            lsp_log_error("Failed to parse message");
            free(message_content);
            continue;
        }
        
        // Handle message
        if (msg->method) {
            if (strcmp(msg->method, "initialize") == 0) {
                lsp_handle_initialize(server, msg->id, msg->params);
            } else if (strcmp(msg->method, "initialized") == 0) {
                lsp_handle_initialized(server);
            } else if (strcmp(msg->method, "shutdown") == 0) {
                lsp_handle_shutdown(server, msg->id);
            } else if (strcmp(msg->method, "exit") == 0) {
                lsp_handle_exit(server);
                lsp_message_free(msg);
                free(message_content);
                break;
            } else if (strcmp(msg->method, "textDocument/didOpen") == 0) {
                lsp_handle_text_document_did_open(server, msg->params);
            } else if (strcmp(msg->method, "textDocument/didChange") == 0) {
                lsp_handle_text_document_did_change(server, msg->params);
            } else if (strcmp(msg->method, "textDocument/didClose") == 0) {
                lsp_handle_text_document_did_close(server, msg->params);
            } else if (strcmp(msg->method, "textDocument/completion") == 0) {
                lsp_handle_completion(server, msg->id, msg->params);
            } else if (strcmp(msg->method, "textDocument/hover") == 0) {
                lsp_handle_hover(server, msg->id, msg->params);
            } else {
                lsp_log_debug("Unhandled method: %s", msg->method);
                if (msg->id) {
                    lsp_send_error(server, msg->id, -32601, "Method not found");
                }
            }
            
            if (msg->id) {
                server->requests_handled++;
            } else {
                server->notifications_handled++;
            }
        }
        
        lsp_message_free(msg);
        free(message_content);
        message_content = NULL;
    }
    
    lsp_log_info("Language server shutting down");
    return 0;
}