#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

// Simple LSP server that responds to basic requests
// This is a minimal implementation to demonstrate LSP integration

static void send_response(const char* id, const char* result) {
    char response[4096];
    snprintf(response, sizeof(response), 
            "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", 
            id, result);
    
    printf("Content-Length: %zu\r\n\r\n%s", strlen(response), response);
    fflush(stdout);
}

static void send_notification(const char* method, const char* params) {
    char notification[4096];
    snprintf(notification, sizeof(notification),
            "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
            method, params ? params : "null");
    
    printf("Content-Length: %zu\r\n\r\n%s", strlen(notification), notification);
    fflush(stdout);
}

static char* read_message() {
    char header[1024];
    size_t content_length = 0;
    
    // Read headers
    while (fgets(header, sizeof(header), stdin)) {
        // Remove trailing newline
        size_t len = strlen(header);
        if (len > 0 && header[len - 1] == '\n') {
            header[len - 1] = '\0';
            len--;
        }
        if (len > 0 && header[len - 1] == '\r') {
            header[len - 1] = '\0';
            len--;
        }
        
        // Empty line signals end of headers
        if (len == 0) break;
        
        // Parse Content-Length
        if (strncmp(header, "Content-Length:", 15) == 0) {
            content_length = atoi(header + 15);
        }
    }
    
    if (content_length == 0) return NULL;
    
    // Read content
    char* content = malloc(content_length + 1);
    if (!content) return NULL;
    
    size_t bytes_read = fread(content, 1, content_length, stdin);
    if (bytes_read != content_length) {
        free(content);
        return NULL;
    }
    
    content[content_length] = '\0';
    return content;
}

static char* get_string_value(const char* json, const char* key) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    
    const char* start = strstr(json, pattern);
    if (!start) return NULL;
    
    start += strlen(pattern);
    while (*start && isspace(*start)) start++;
    
    if (*start != '"') return NULL;
    start++;
    
    const char* end = start;
    while (*end && *end != '"') {
        if (*end == '\\') end++;
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

int main() {
    fprintf(stderr, "Goo Language Server starting...\n");
    
    char* message;
    while ((message = read_message()) != NULL) {
        // Parse method
        char* method = get_string_value(message, "method");
        char* id = get_string_value(message, "id");
        
        if (method) {
            if (strcmp(method, "initialize") == 0) {
                const char* capabilities = "{"
                    "\"capabilities\":{"
                        "\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
                        "\"completionProvider\":{\"triggerCharacters\":[\".\"]},"
                        "\"hoverProvider\":true,"
                        "\"definitionProvider\":true"
                    "},"
                    "\"serverInfo\":{\"name\":\"Goo Language Server\",\"version\":\"1.0.0\"}"
                "}";
                send_response(id, capabilities);
                
            } else if (strcmp(method, "initialized") == 0) {
                fprintf(stderr, "Server initialized\n");
                
            } else if (strcmp(method, "shutdown") == 0) {
                send_response(id, "null");
                
            } else if (strcmp(method, "exit") == 0) {
                break;
                
            } else if (strcmp(method, "textDocument/didOpen") == 0) {
                fprintf(stderr, "Document opened\n");
                
            } else if (strcmp(method, "textDocument/didChange") == 0) {
                fprintf(stderr, "Document changed\n");
                
            } else if (strcmp(method, "textDocument/completion") == 0) {
                const char* completion_result = "{"
                    "\"isIncomplete\":false,"
                    "\"items\":["
                        "{\"label\":\"fn\",\"kind\":14,\"detail\":\"Goo keyword\"},"
                        "{\"label\":\"let\",\"kind\":14,\"detail\":\"Goo keyword\"},"
                        "{\"label\":\"if\",\"kind\":14,\"detail\":\"Goo keyword\"},"
                        "{\"label\":\"print\",\"kind\":3,\"detail\":\"Built-in function\"}"
                    "]"
                "}";
                send_response(id, completion_result);
                
            } else if (strcmp(method, "textDocument/hover") == 0) {
                const char* hover_result = "{"
                    "\"contents\":{"
                        "\"kind\":\"markdown\","
                        "\"value\":\"**Goo Language**\\n\\nHover information for the Goo programming language.\""
                    "}"
                "}";
                send_response(id, hover_result);
                
            } else {
                fprintf(stderr, "Unknown method: %s\n", method);
            }
        }
        
        free(method);
        free(id);
        free(message);
    }
    
    fprintf(stderr, "Goo Language Server stopped\n");
    return 0;
}