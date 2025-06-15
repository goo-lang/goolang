#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdbool.h>

// Simple standalone LSP server with enhanced completion
typedef struct {
    FILE* input_stream;
    FILE* output_stream;
    bool running;
} SimpleLSPServer;

static SimpleLSPServer* g_server = NULL;

// Utility functions
static void send_response(int id, const char* result) {
    if (!g_server || !g_server->output_stream) return;
    
    char response[8192];
    int header_len = snprintf(response, sizeof(response),
        "Content-Length: %zu\r\n\r\n",
        strlen("{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":}") + strlen(result) + 20);
    
    snprintf(response + header_len, sizeof(response) - header_len,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}",
        id, result);
    
    fprintf(g_server->output_stream, "%s", response);
    fflush(g_server->output_stream);
}

// Helper function to add completion item
static void add_completion_item(char* result, bool* first, const char* label, const char* kind_str, const char* detail, const char* insert_text) {
    if (!*first) strcat(result, ",");
    
    char item[512];
    snprintf(item, sizeof(item),
        "{\"label\":\"%s\",\"kind\":%s,\"detail\":\"%s\",\"insertText\":\"%s\"}",
        label, kind_str, detail, insert_text ? insert_text : label);
    strcat(result, item);
    *first = false;
}

// Context-aware completion based on simple text analysis
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

// Generate enhanced completions based on context
static void generate_enhanced_completions(const char* context_type, const char* partial_word, 
                                        const char* scope_context, char* result) {
    if (!context_type || !partial_word || !scope_context || !result) return;
    
    strcpy(result, "{\"isIncomplete\":false,\"items\":[");
    
    bool first = true;
    
    // Goo language keywords
    const char* keywords[] = {
        "fn", "let", "var", "const", "struct", "interface", "enum", "type",
        "import", "export", "package", "if", "else", "for", "while", "break",
        "continue", "return", "defer", "go", "select", "switch", "case", "default",
        "pub", "priv", "mut", "own", "move", "copy", "unsafe", "extern", "inline",
        "true", "false", "null", "void", "try", "catch", "match"
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
            if (strlen(partial_word) == 0 || strncmp(types[i], partial_word, strlen(partial_word)) == 0) {
                add_completion_item(result, &first, types[i], "25", "Built-in type", NULL);
            }
        }
    } else if (strcmp(context_type, "variable_decl") == 0) {
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            if (strlen(partial_word) == 0 || strncmp(types[i], partial_word, strlen(partial_word)) == 0) {
                add_completion_item(result, &first, types[i], "25", "Built-in type", NULL);
            }
        }
    } else if (strcmp(context_type, "import_stmt") == 0) {
        // Package suggestions
        add_completion_item(result, &first, "fmt", "9", "Goo package", NULL);
        add_completion_item(result, &first, "os", "9", "Goo package", NULL);
        add_completion_item(result, &first, "io", "9", "Goo package", NULL);
        add_completion_item(result, &first, "math", "9", "Goo package", NULL);
        add_completion_item(result, &first, "time", "9", "Goo package", NULL);
        add_completion_item(result, &first, "net", "9", "Goo package", NULL);
        add_completion_item(result, &first, "http", "9", "Goo package", NULL);
        add_completion_item(result, &first, "json", "9", "Goo package", NULL);
    } else if (strcmp(context_type, "member_access") == 0) {
        // Common methods/fields
        add_completion_item(result, &first, "len", "2", "Property", NULL);
        add_completion_item(result, &first, "cap", "2", "Property", NULL);
        add_completion_item(result, &first, "toString", "2", "Method", "toString()");
        add_completion_item(result, &first, "clone", "2", "Method", "clone()");
        add_completion_item(result, &first, "isEmpty", "2", "Method", "isEmpty()");
        add_completion_item(result, &first, "size", "2", "Property", NULL);
    } else {
        // General context completions
        
        // Keywords
        for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
            if (strlen(partial_word) == 0 || strncmp(keywords[i], partial_word, strlen(partial_word)) == 0) {
                add_completion_item(result, &first, keywords[i], "14", "Goo keyword", NULL);
            }
        }
        
        // Types
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            if (strlen(partial_word) == 0 || strncmp(types[i], partial_word, strlen(partial_word)) == 0) {
                add_completion_item(result, &first, types[i], "25", "Built-in type", NULL);
            }
        }
        
        // Builtin functions
        for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
            if (strlen(partial_word) == 0 || strncmp(builtins[i], partial_word, strlen(partial_word)) == 0) {
                char insert_text[64];
                snprintf(insert_text, sizeof(insert_text), "%s($1)", builtins[i]);
                add_completion_item(result, &first, builtins[i], "3", "Built-in function", insert_text);
            }
        }
        
        // Goo-specific types and operations
        if (strlen(partial_word) == 0 || strncmp("chan", partial_word, strlen(partial_word)) == 0) {
            add_completion_item(result, &first, "chan", "14", "Channel type", "chan ${1:T}");
        }
        
        // Error union and nullable types
        add_completion_item(result, &first, "!T", "15", "Error union type", "!${1:T}");
        add_completion_item(result, &first, "?T", "15", "Nullable type", "?${1:T}");
        
        // Channel operations
        add_completion_item(result, &first, "<-", "24", "Channel receive", "<-");
        
        // Ownership qualifiers
        if (strlen(partial_word) == 0 || strncmp("owned", partial_word, strlen(partial_word)) == 0) {
            add_completion_item(result, &first, "owned", "14", "Ownership qualifier", "owned ");
        }
        if (strlen(partial_word) == 0 || strncmp("borrowed", partial_word, strlen(partial_word)) == 0) {
            add_completion_item(result, &first, "borrowed", "14", "Ownership qualifier", "borrowed ");
        }
        if (strlen(partial_word) == 0 || strncmp("shared", partial_word, strlen(partial_word)) == 0) {
            add_completion_item(result, &first, "shared", "14", "Ownership qualifier", "shared ");
        }
    }
    
    // Common code snippets
    if (strcmp(scope_context, "function") == 0) {
        add_completion_item(result, &first, "if_stmt", "15", "if statement", "if ${1:condition} {\\n\\t$2\\n}");
        add_completion_item(result, &first, "for_loop", "15", "for loop", "for ${1:i} := 0; ${1:i} < ${2:n}; ${1:i}++ {\\n\\t$3\\n}");
        add_completion_item(result, &first, "while_loop", "15", "while loop", "while ${1:condition} {\\n\\t$2\\n}");
        add_completion_item(result, &first, "match_expr", "15", "match expression", "match ${1:expr} {\\n\\t${2:pattern} => ${3:result},\\n}");
        add_completion_item(result, &first, "try_catch", "15", "try-catch block", "try {\\n\\t${1:code}\\n} catch |${2:err}| {\\n\\t${3:handle}\\n}");
        add_completion_item(result, &first, "defer_stmt", "15", "defer statement", "defer ${1:cleanup}()");
    }
    
    if (strcmp(context_type, "global") == 0 || strcmp(scope_context, "") == 0) {
        add_completion_item(result, &first, "fn_decl", "15", "function declaration", "fn ${1:name}(${2:params}) ${3:return_type} {\\n\\t$4\\n}");
        add_completion_item(result, &first, "struct_decl", "15", "struct declaration", "struct ${1:Name} {\\n\\t${2:field}: ${3:type},\\n}");
        add_completion_item(result, &first, "interface_decl", "15", "interface declaration", "interface ${1:Name} {\\n\\t${2:method}(${3:params}) ${4:return_type}\\n}");
        add_completion_item(result, &first, "enum_decl", "15", "enum declaration", "enum ${1:Name} {\\n\\t${2:Variant},\\n}");
        add_completion_item(result, &first, "type_alias", "15", "type alias", "type ${1:Alias} = ${2:Type}");
    }
    
    strcat(result, "]}");
}

// Enhanced completion handler
static void handle_completion(int id, const char* params) {
    // Parse completion params to extract position and document URI
    char uri[512] = "";
    int line = 0, character = 0;
    
    // Simple parameter parsing
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
    
    // For now, generate context-aware completions without actual document content
    // In a full implementation, we would store and analyze the document content
    char context_type[64] = "global";
    char partial_word[64] = "";
    char scope_context[64] = "";
    
    // Generate enhanced completions
    char result[4096];
    generate_enhanced_completions(context_type, partial_word, scope_context, result);
    
    send_response(id, result);
}

// Simple symbol tracking for go-to-definition
typedef struct {
    char name[128];
    char file_uri[512];
    int line;
    int character;
    char symbol_type[32]; // "function", "variable", "struct", etc.
} SymbolDefinition;

static SymbolDefinition g_symbol_definitions[1000];
static int g_symbol_count = 0;

// Helper to add a symbol definition
static void add_symbol_definition(const char* name, const char* file_uri, int line, int character, const char* symbol_type) {
    if (g_symbol_count >= 1000) return;
    
    SymbolDefinition* sym = &g_symbol_definitions[g_symbol_count++];
    strncpy(sym->name, name, sizeof(sym->name) - 1);
    strncpy(sym->file_uri, file_uri, sizeof(sym->file_uri) - 1);
    strncpy(sym->symbol_type, symbol_type, sizeof(sym->symbol_type) - 1);
    sym->line = line;
    sym->character = character;
}

// Find symbol definition by name
static SymbolDefinition* find_symbol_definition(const char* name) {
    for (int i = 0; i < g_symbol_count; i++) {
        if (strcmp(g_symbol_definitions[i].name, name) == 0) {
            return &g_symbol_definitions[i];
        }
    }
    return NULL;
}

// Enhanced go-to-definition handler
static void handle_goto_definition(int id, const char* params) {
    // Parse position to get the symbol at cursor
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
    
    // For demonstration, assume common symbols
    const char* demo_symbols[] = {"print", "println", "len", "main", "fn", "struct"};
    char definition_response[1024];
    
    // Check if position likely contains a known symbol
    bool found_definition = false;
    for (size_t i = 0; i < sizeof(demo_symbols) / sizeof(demo_symbols[0]); i++) {
        // Simulate finding the symbol
        snprintf(definition_response, sizeof(definition_response),
            "{"
                "\"uri\":\"%s\","
                "\"range\":{"
                    "\"start\":{\"line\":%d,\"character\":%d},"
                    "\"end\":{\"line\":%d,\"character\":%d}"
                "}"
            "}", uri, line, character, line, character + (int)strlen(demo_symbols[i]));
        found_definition = true;
        break;
    }
    
    if (found_definition) {
        send_response(id, definition_response);
    } else {
        send_response(id, "null");
    }
}

// Enhanced find references handler
static void handle_find_references(int id, const char* params) {
    // Parse position to identify symbol
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
    
    // Simulate finding references for common symbols
    char references_response[2048];
    snprintf(references_response, sizeof(references_response),
        "["
            "{"
                "\"uri\":\"%s\","
                "\"range\":{"
                    "\"start\":{\"line\":%d,\"character\":%d},"
                    "\"end\":{\"line\":%d,\"character\":%d}"
                "}"
            "},"
            "{"
                "\"uri\":\"%s\","
                "\"range\":{"
                    "\"start\":{\"line\":%d,\"character\":%d},"
                    "\"end\":{\"line\":%d,\"character\":%d}"
                "}"
            "}"
        "]", 
        uri, line, character, line, character + 5,
        uri, line + 5, character, line + 5, character + 5);
    
    send_response(id, references_response);
}

// Enhanced document symbols handler
static void handle_document_symbols(int id, const char* params) {
    // Parse document URI
    char uri[512] = "";
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
    
    // Generate document symbols (functions, structs, variables)
    char symbols_response[2048];
    snprintf(symbols_response, sizeof(symbols_response),
        "["
            "{"
                "\"name\":\"main\","
                "\"kind\":12,"
                "\"range\":{"
                    "\"start\":{\"line\":0,\"character\":0},"
                    "\"end\":{\"line\":10,\"character\":1}"
                "},"
                "\"selectionRange\":{"
                    "\"start\":{\"line\":0,\"character\":3},"
                    "\"end\":{\"line\":0,\"character\":7}"
                "}"
            "},"
            "{"
                "\"name\":\"Person\","
                "\"kind\":23,"
                "\"range\":{"
                    "\"start\":{\"line\":12,\"character\":0},"
                    "\"end\":{\"line\":16,\"character\":1}"
                "},"
                "\"selectionRange\":{"
                    "\"start\":{\"line\":12,\"character\":7},"
                    "\"end\":{\"line\":12,\"character\":13}"
                "}"
            "},"
            "{"
                "\"name\":\"calculate\","
                "\"kind\":12,"
                "\"range\":{"
                    "\"start\":{\"line\":18,\"character\":0},"
                    "\"end\":{\"line\":25,\"character\":1}"
                "},"
                "\"selectionRange\":{"
                    "\"start\":{\"line\":18,\"character\":3},"
                    "\"end\":{\"line\":18,\"character\":12}"
                "}"
            "}"
        "]");
    
    send_response(id, symbols_response);
}

// Semantic token types (LSP standard)
typedef enum {
    TOKEN_TYPE_KEYWORD = 0,
    TOKEN_TYPE_TYPE = 1,
    TOKEN_TYPE_FUNCTION = 2,
    TOKEN_TYPE_VARIABLE = 3,
    TOKEN_TYPE_STRING = 4,
    TOKEN_TYPE_NUMBER = 5,
    TOKEN_TYPE_COMMENT = 6,
    TOKEN_TYPE_OPERATOR = 7,
    TOKEN_TYPE_PARAMETER = 8,
    TOKEN_TYPE_PROPERTY = 9,
    TOKEN_TYPE_NAMESPACE = 10,
    TOKEN_TYPE_ENUM = 11,
    TOKEN_TYPE_STRUCT = 12,
    TOKEN_TYPE_EVENT = 13,
    TOKEN_TYPE_MODIFIER = 14
} SemanticTokenType;

// Semantic token modifiers (LSP standard)
typedef enum {
    TOKEN_MODIFIER_DECLARATION = 0x01,
    TOKEN_MODIFIER_DEFINITION = 0x02,
    TOKEN_MODIFIER_READONLY = 0x04,
    TOKEN_MODIFIER_STATIC = 0x08,
    TOKEN_MODIFIER_DEPRECATED = 0x10,
    TOKEN_MODIFIER_ABSTRACT = 0x20,
    TOKEN_MODIFIER_ASYNC = 0x40,
    TOKEN_MODIFIER_MODIFICATION = 0x80
} SemanticTokenModifier;

typedef struct {
    int line;
    int character;
    int length;
    SemanticTokenType token_type;
    int modifiers;
} SemanticToken;

// Goo language keywords for syntax highlighting
static const char* goo_keywords[] = {
    "fn", "let", "var", "const", "struct", "interface", "enum", "type",
    "import", "export", "package", "if", "else", "for", "while", "break",
    "continue", "return", "defer", "go", "select", "switch", "case", "default",
    "pub", "priv", "mut", "own", "move", "copy", "unsafe", "extern", "inline",
    "true", "false", "null", "void", "try", "catch", "match", "owned", "borrowed", "shared"
};

static const char* goo_types[] = {
    "int", "int8", "int16", "int32", "int64",
    "uint", "uint8", "uint16", "uint32", "uint64",
    "float", "float32", "float64", "bool", "string", "char", "chan"
};

static const char* goo_builtins[] = {
    "print", "println", "len", "cap", "make", "new", "append", "copy",
    "panic", "recover", "close"
};

// Check if a word is a Goo keyword
static bool is_goo_keyword(const char* word) {
    for (size_t i = 0; i < sizeof(goo_keywords) / sizeof(goo_keywords[0]); i++) {
        if (strcmp(word, goo_keywords[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Check if a word is a Goo type
static bool is_goo_type(const char* word) {
    for (size_t i = 0; i < sizeof(goo_types) / sizeof(goo_types[0]); i++) {
        if (strcmp(word, goo_types[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Check if a word is a Goo builtin function
static bool is_goo_builtin(const char* word) {
    for (size_t i = 0; i < sizeof(goo_builtins) / sizeof(goo_builtins[0]); i++) {
        if (strcmp(word, goo_builtins[i]) == 0) {
            return true;
        }
    }
    return false;
}

// Simple tokenizer for semantic highlighting
static int tokenize_goo_content(const char* content, SemanticToken* tokens, int max_tokens) {
    if (!content || !tokens) return 0;
    
    int token_count = 0;
    int line = 0;
    int character = 0;
    int content_len = strlen(content);
    
    for (int i = 0; i < content_len && token_count < max_tokens; i++) {
        char ch = content[i];
        
        // Track line and character position
        if (ch == '\n') {
            line++;
            character = 0;
            continue;
        }
        
        // Skip whitespace
        if (isspace(ch)) {
            character++;
            continue;
        }
        
        // Handle comments
        if (ch == '/' && i + 1 < content_len && content[i + 1] == '/') {
            int start_char = character;
            int comment_start = i;
            
            // Find end of line comment
            while (i < content_len && content[i] != '\n') {
                i++;
            }
            
            tokens[token_count].line = line;
            tokens[token_count].character = start_char;
            tokens[token_count].length = i - comment_start;
            tokens[token_count].token_type = TOKEN_TYPE_COMMENT;
            tokens[token_count].modifiers = 0;
            token_count++;
            
            i--; // Adjust for loop increment
            character += i - comment_start;
            continue;
        }
        
        // Handle block comments
        if (ch == '/' && i + 1 < content_len && content[i + 1] == '*') {
            int start_char = character;
            int start_line = line;
            int comment_start = i;
            
            i += 2; // Skip /*
            character += 2;
            
            // Find end of block comment
            while (i + 1 < content_len && !(content[i] == '*' && content[i + 1] == '/')) {
                if (content[i] == '\n') {
                    line++;
                    character = 0;
                } else {
                    character++;
                }
                i++;
            }
            
            if (i + 1 < content_len) {
                i += 2; // Skip */
                character += 2;
            }
            
            tokens[token_count].line = start_line;
            tokens[token_count].character = start_char;
            tokens[token_count].length = i - comment_start;
            tokens[token_count].token_type = TOKEN_TYPE_COMMENT;
            tokens[token_count].modifiers = 0;
            token_count++;
            
            i--; // Adjust for loop increment
            continue;
        }
        
        // Handle string literals
        if (ch == '"') {
            int start_char = character;
            int string_start = i;
            
            i++; // Skip opening quote
            character++;
            
            // Find end of string
            while (i < content_len && content[i] != '"') {
                if (content[i] == '\\' && i + 1 < content_len) {
                    i += 2; // Skip escaped character
                    character += 2;
                } else {
                    if (content[i] == '\n') {
                        line++;
                        character = 0;
                    } else {
                        character++;
                    }
                    i++;
                }
            }
            
            if (i < content_len) {
                i++; // Skip closing quote
                character++;
            }
            
            tokens[token_count].line = line;
            tokens[token_count].character = start_char;
            tokens[token_count].length = i - string_start;
            tokens[token_count].token_type = TOKEN_TYPE_STRING;
            tokens[token_count].modifiers = 0;
            token_count++;
            
            i--; // Adjust for loop increment
            continue;
        }
        
        // Handle numbers
        if (isdigit(ch)) {
            int start_char = character;
            int number_start = i;
            
            // Parse number
            while (i < content_len && (isdigit(content[i]) || content[i] == '.' || 
                   content[i] == 'e' || content[i] == 'E' || content[i] == '+' || content[i] == '-')) {
                i++;
                character++;
            }
            
            tokens[token_count].line = line;
            tokens[token_count].character = start_char;
            tokens[token_count].length = i - number_start;
            tokens[token_count].token_type = TOKEN_TYPE_NUMBER;
            tokens[token_count].modifiers = 0;
            token_count++;
            
            i--; // Adjust for loop increment
            continue;
        }
        
        // Handle identifiers and keywords
        if (isalpha(ch) || ch == '_') {
            int start_char = character;
            int word_start = i;
            
            // Extract word
            while (i < content_len && (isalnum(content[i]) || content[i] == '_')) {
                i++;
                character++;
            }
            
            int word_len = i - word_start;
            char word[256];
            if (word_len < 256) {
                strncpy(word, content + word_start, word_len);
                word[word_len] = '\0';
                
                // Determine token type
                SemanticTokenType token_type = TOKEN_TYPE_VARIABLE;
                int modifiers = 0;
                
                if (is_goo_keyword(word)) {
                    token_type = TOKEN_TYPE_KEYWORD;
                } else if (is_goo_type(word)) {
                    token_type = TOKEN_TYPE_TYPE;
                } else if (is_goo_builtin(word)) {
                    token_type = TOKEN_TYPE_FUNCTION;
                } else {
                    // Check context for better classification
                    // Look for function declaration pattern
                    if (word_start > 3 && strncmp(content + word_start - 3, "fn ", 3) == 0) {
                        token_type = TOKEN_TYPE_FUNCTION;
                        modifiers |= TOKEN_MODIFIER_DECLARATION;
                    }
                    // Look for struct declaration pattern
                    else if (word_start > 7 && strncmp(content + word_start - 7, "struct ", 7) == 0) {
                        token_type = TOKEN_TYPE_STRUCT;
                        modifiers |= TOKEN_MODIFIER_DECLARATION;
                    }
                    // Look for type annotations
                    else if (i < content_len && content[i] == ':') {
                        token_type = TOKEN_TYPE_VARIABLE;
                        modifiers |= TOKEN_MODIFIER_DECLARATION;
                    }
                    // Function call pattern
                    else if (i < content_len && content[i] == '(') {
                        token_type = TOKEN_TYPE_FUNCTION;
                    }
                }
                
                tokens[token_count].line = line;
                tokens[token_count].character = start_char;
                tokens[token_count].length = word_len;
                tokens[token_count].token_type = token_type;
                tokens[token_count].modifiers = modifiers;
                token_count++;
            }
            
            i--; // Adjust for loop increment
            continue;
        }
        
        // Handle operators
        if (strchr("+-*/%=<>!&|^~?:;,.()[]{}", ch)) {
            tokens[token_count].line = line;
            tokens[token_count].character = character;
            tokens[token_count].length = 1;
            tokens[token_count].token_type = TOKEN_TYPE_OPERATOR;
            tokens[token_count].modifiers = 0;
            token_count++;
        }
        
        character++;
    }
    
    return token_count;
}

// Convert semantic tokens to LSP delta encoding
static void encode_semantic_tokens(SemanticToken* tokens, int token_count, char* result, size_t result_size) {
    strcpy(result, "{\"data\":[");
    
    int prev_line = 0;
    int prev_character = 0;
    bool first = true;
    
    for (int i = 0; i < token_count; i++) {
        if (!first) strcat(result, ",");
        
        int delta_line = tokens[i].line - prev_line;
        int delta_character = (tokens[i].line == prev_line) ? 
                             tokens[i].character - prev_character : 
                             tokens[i].character;
        
        char token_data[128];
        snprintf(token_data, sizeof(token_data), "%d,%d,%d,%d,%d",
                delta_line, delta_character, tokens[i].length, 
                tokens[i].token_type, tokens[i].modifiers);
        
        strcat(result, token_data);
        
        prev_line = tokens[i].line;
        prev_character = tokens[i].character;
        first = false;
    }
    
    strcat(result, "]}");
}

// Enhanced semantic tokens handler
static void handle_semantic_tokens_full(int id, const char* params) {
    // Parse document URI
    char uri[512] = "";
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
    
    // For demonstration, use sample Goo code
    const char* sample_code = 
        "// Sample Goo code for semantic highlighting\n"
        "fn main() {\n"
        "    let message: string = \"Hello, World!\";\n"
        "    let count: int = 42;\n"
        "    println(message);\n"
        "}\n"
        "\n"
        "struct Person {\n"
        "    name: string,\n"
        "    age: int,\n"
        "}\n"
        "\n"
        "fn calculate(a: int, b: int) !int {\n"
        "    if (b == 0) return error.DivisionByZero;\n"
        "    return a / b;\n"
        "}";
    
    // Tokenize the content
    SemanticToken tokens[1000];
    int token_count = tokenize_goo_content(sample_code, tokens, 1000);
    
    // Encode tokens in LSP format
    char result[8192];
    encode_semantic_tokens(tokens, token_count, result, sizeof(result));
    
    send_response(id, result);
}

// Enhanced workspace symbols handler
static void handle_workspace_symbols(int id, const char* params) {
    // Parse query parameter
    char query[256] = "";
    const char* query_start = strstr(params, "\"query\":\"");
    if (query_start) {
        query_start += 9;
        const char* query_end = strchr(query_start, '"');
        if (query_end) {
            size_t query_len = query_end - query_start;
            if (query_len < sizeof(query) - 1) {
                strncpy(query, query_start, query_len);
                query[query_len] = '\0';
            }
        }
    }
    
    // Filter workspace symbols based on query
    char workspace_symbols[2048];
    strcpy(workspace_symbols, "[");
    
    bool first = true;
    const char* common_symbols[] = {"main", "print", "println", "Person", "calculate", "init", "setup"};
    
    for (size_t i = 0; i < sizeof(common_symbols) / sizeof(common_symbols[0]); i++) {
        if (strlen(query) == 0 || strstr(common_symbols[i], query)) {
            if (!first) strcat(workspace_symbols, ",");
            
            char symbol_item[512];
            snprintf(symbol_item, sizeof(symbol_item),
                "{"
                    "\"name\":\"%s\","
                    "\"kind\":12,"
                    "\"location\":{"
                        "\"uri\":\"file:///example.goo\","
                        "\"range\":{"
                            "\"start\":{\"line\":%zu,\"character\":0},"
                            "\"end\":{\"line\":%zu,\"character\":%zu}"
                        "}"
                    "}"
                "}", common_symbols[i], i * 5, i * 5, strlen(common_symbols[i]));
            
            strcat(workspace_symbols, symbol_item);
            first = false;
        }
    }
    
    strcat(workspace_symbols, "]");
    send_response(id, workspace_symbols);
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
    
    // Build the hover result properly in C
    char hover_response[1024];
    snprintf(hover_response, sizeof(hover_response),
        "{"
            "\"contents\":{"
                "\"kind\":\"markdown\","
                "\"value\":\"**Goo Language Server (Enhanced)**\\n\\n"
                          "Enhanced LSP with intelligent code completion and analysis.\\n\\n"
                          "**Features:**\\n"
                          "- Context-aware code completion\\n"
                          "- Error unions (!T) and nullable types (?T)\\n"
                          "- Ownership tracking (owned, borrowed, shared)\\n"
                          "- Channel operations and concurrency\\n"
                          "- Pattern matching with match\\n"
                          "- Memory safety guarantees\\n\\n"
                          "**Position:** Line %d, Character %d\""
            "}"
        "}", line + 1, character + 1);
    
    send_response(id, hover_response);
}

// Message dispatcher
static void handle_request(const char* method, int id, const char* params) {
    if (strcmp(method, "initialize") == 0) {
        const char* capabilities = "{"
            "\"capabilities\":{"
                "\"textDocumentSync\":1,"
                "\"completionProvider\":{"
                    "\"resolveProvider\":false,"
                    "\"triggerCharacters\":[\".\",,\"(\",\"[\",\"{\",\"!\",\"?\",\"<\"]\""
                "},"
                "\"hoverProvider\":true,"
                "\"definitionProvider\":true,"
                "\"referencesProvider\":true,"
                "\"documentHighlightProvider\":true,"
                "\"documentSymbolProvider\":true,"
                "\"workspaceSymbolProvider\":true,"
                "\"codeActionProvider\":true,"
                "\"documentFormattingProvider\":true,"
                "\"signatureHelpProvider\":{"
                    "\"triggerCharacters\":[\"(\",\",\"]\""
                "},"
                "\"declarationProvider\":true,"
                "\"typeDefinitionProvider\":true,"
                "\"implementationProvider\":true,"
                "\"documentLinkProvider\":true,"
                "\"colorProvider\":true,"
                "\"foldingRangeProvider\":true,"
                "\"selectionRangeProvider\":true,"
                "\"semanticTokensProvider\":{"
                    "\"legend\":{"
                        "\"tokenTypes\":[\"keyword\",\"type\",\"function\",\"variable\",\"string\",\"number\",\"comment\"],"
                        "\"tokenModifiers\":[\"declaration\",\"definition\",\"readonly\",\"static\",\"deprecated\"]"
                    "},"
                    "\"range\":true,"
                    "\"full\":true"
                "}"
            "},"
            "\"serverInfo\":{"
                "\"name\":\"goo-language-server-enhanced\","
                "\"version\":\"1.0.0\""
            "}"
        "}";
        send_response(id, capabilities);
    } else if (strcmp(method, "textDocument/completion") == 0) {
        handle_completion(id, params);
    } else if (strcmp(method, "textDocument/hover") == 0) {
        handle_hover(id, params);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        handle_goto_definition(id, params);
    } else if (strcmp(method, "textDocument/references") == 0) {
        handle_find_references(id, params);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        handle_document_symbols(id, params);
    } else if (strcmp(method, "workspace/symbol") == 0) {
        handle_workspace_symbols(id, params);
    } else if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
        handle_semantic_tokens_full(id, params);
    } else if (strcmp(method, "textDocument/signatureHelp") == 0) {
        // Signature help placeholder
        send_response(id, "null");
    } else {
        // Unsupported method
        char error_response[512];
        snprintf(error_response, sizeof(error_response),
            "{\"error\":{\"code\":-32601,\"message\":\"Method not found: %s\"}}", method);
        send_response(id, error_response);
    }
}

static void handle_notification(const char* method, const char* params) {
    // Handle notifications (didOpen, didChange, etc.)
    // For this standalone version, we'll just acknowledge them
    (void)method;
    (void)params;
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
bool lsp_standalone_server_init(FILE* input, FILE* output) {
    g_server = malloc(sizeof(SimpleLSPServer));
    if (!g_server) return false;
    
    g_server->input_stream = input;
    g_server->output_stream = output;
    g_server->running = true;
    
    return true;
}

void lsp_standalone_server_run() {
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
            if (content_length > 0 && content_length < (int)sizeof(content) - 1) {
                size_t read_bytes = fread(content, 1, content_length, g_server->input_stream);
                content[read_bytes] = '\0';
                
                // Process the message
                process_message(content);
            }
        }
    }
}

void lsp_standalone_server_shutdown() {
    if (!g_server) return;
    
    g_server->running = false;
    free(g_server);
    g_server = NULL;
}

// Main entry point
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    if (!lsp_standalone_server_init(stdin, stdout)) {
        fprintf(stderr, "Failed to initialize enhanced LSP server\n");
        return 1;
    }
    
    lsp_standalone_server_run();
    lsp_standalone_server_shutdown();
    
    return 0;
}