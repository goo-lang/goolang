#include "package/goo_mod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Token types for goo.mod parsing
typedef enum {
    TOKEN_EOF,
    TOKEN_IDENTIFIER,
    TOKEN_STRING,
    TOKEN_NUMBER,
    TOKEN_LBRACE,      // {
    TOKEN_RBRACE,      // }
    TOKEN_LBRACKET,    // [
    TOKEN_RBRACKET,    // ]
    TOKEN_COLON,       // :
    TOKEN_COMMA,       // ,
    TOKEN_NEWLINE,
    TOKEN_COMMENT,
    TOKEN_INVALID
} TokenType;

typedef struct Token {
    TokenType type;
    char* value;
    size_t line;
    size_t column;
} Token;

typedef struct Lexer {
    const char* input;
    size_t position;
    size_t line;
    size_t column;
    char current_char;
} Lexer;

typedef struct Parser {
    Lexer* lexer;
    Token current_token;
    Token peek_token;
    char* error_message;
} Parser;

// Lexer functions
Lexer* lexer_create(const char* input) {
    Lexer* lexer = xmalloc(sizeof(Lexer));
    if (!lexer) {
        return NULL;
    }
    lexer->input = input;
    lexer->position = 0;
    lexer->line = 1;
    lexer->column = 1;
    lexer->current_char = input[0];
    return lexer;
}

void lexer_free(Lexer* lexer) {
    if (lexer) free(lexer);
}

void lexer_advance(Lexer* lexer) {
    if (lexer->current_char == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    
    lexer->position++;
    if (lexer->position >= strlen(lexer->input)) {
        lexer->current_char = '\0';
    } else {
        lexer->current_char = lexer->input[lexer->position];
    }
}

void lexer_skip_whitespace(Lexer* lexer) {
    while (lexer->current_char == ' ' || lexer->current_char == '\t' || lexer->current_char == '\r') {
        lexer_advance(lexer);
    }
}

char* lexer_read_string(Lexer* lexer) {
    size_t start = lexer->position + 1; // Skip opening quote
    lexer_advance(lexer); // Skip opening quote
    
    while (lexer->current_char != '\0' && lexer->current_char != '"') {
        if (lexer->current_char == '\\') {
            lexer_advance(lexer); // Skip escape character
        }
        lexer_advance(lexer);
    }
    
    size_t length = lexer->position - start;
    char* result = malloc(length + 1);
    if (!result) {
        return NULL; // allocation sized by untrusted goo.mod input; don't write through NULL
    }
    strncpy(result, lexer->input + start, length);
    result[length] = '\0';
    
    if (lexer->current_char == '"') {
        lexer_advance(lexer); // Skip closing quote
    }
    
    return result;
}

char* lexer_read_identifier(Lexer* lexer) {
    size_t start = lexer->position;
    
    while (isalnum(lexer->current_char) || lexer->current_char == '_' || 
           lexer->current_char == '-' || lexer->current_char == '.' || 
           lexer->current_char == '/' || lexer->current_char == '@') {
        lexer_advance(lexer);
    }
    
    size_t length = lexer->position - start;
    char* result = malloc(length + 1);
    if (!result) {
        return NULL; // allocation sized by untrusted goo.mod input; don't write through NULL
    }
    strncpy(result, lexer->input + start, length);
    result[length] = '\0';
    
    return result;
}

char* lexer_read_comment(Lexer* lexer) {
    size_t start = lexer->position;
    
    while (lexer->current_char != '\0' && lexer->current_char != '\n') {
        lexer_advance(lexer);
    }
    
    size_t length = lexer->position - start;
    char* result = malloc(length + 1);
    if (!result) {
        return NULL; // allocation sized by untrusted goo.mod input; don't write through NULL
    }
    strncpy(result, lexer->input + start, length);
    result[length] = '\0';
    
    return result;
}

Token lexer_next_token(Lexer* lexer) {
    Token token = {0};
    token.line = lexer->line;
    token.column = lexer->column;
    
    while (lexer->current_char != '\0') {
        if (isspace(lexer->current_char)) {
            if (lexer->current_char == '\n') {
                token.type = TOKEN_NEWLINE;
                lexer_advance(lexer);
                return token;
            }
            lexer_skip_whitespace(lexer);
            continue;
        }
        
        if (lexer->current_char == '#') {
            token.type = TOKEN_COMMENT;
            token.value = lexer_read_comment(lexer);
            return token;
        }
        
        if (lexer->current_char == '"') {
            token.type = TOKEN_STRING;
            token.value = lexer_read_string(lexer);
            return token;
        }
        
        if (isalpha(lexer->current_char) || lexer->current_char == '_') {
            token.type = TOKEN_IDENTIFIER;
            token.value = lexer_read_identifier(lexer);
            return token;
        }
        
        if (isdigit(lexer->current_char)) {
            token.type = TOKEN_NUMBER;
            token.value = lexer_read_identifier(lexer); // Numbers can contain dots
            return token;
        }
        
        switch (lexer->current_char) {
            case '{':
                token.type = TOKEN_LBRACE;
                lexer_advance(lexer);
                return token;
            case '}':
                token.type = TOKEN_RBRACE;
                lexer_advance(lexer);
                return token;
            case '[':
                token.type = TOKEN_LBRACKET;
                lexer_advance(lexer);
                return token;
            case ']':
                token.type = TOKEN_RBRACKET;
                lexer_advance(lexer);
                return token;
            case ':':
                token.type = TOKEN_COLON;
                lexer_advance(lexer);
                return token;
            case ',':
                token.type = TOKEN_COMMA;
                lexer_advance(lexer);
                return token;
            default:
                token.type = TOKEN_INVALID;
                lexer_advance(lexer);
                return token;
        }
    }
    
    token.type = TOKEN_EOF;
    return token;
}

// Parser functions
Parser* parser_create(const char* input) {
    Parser* parser = xmalloc(sizeof(Parser));
    if (!parser) {
        return NULL;
    }
    parser->lexer = lexer_create(input);
    if (!parser->lexer) {
        free(parser);
        return NULL;
    }
    parser->error_message = NULL;
    
    // Initialize tokens
    parser->current_token = lexer_next_token(parser->lexer);
    parser->peek_token = lexer_next_token(parser->lexer);
    
    return parser;
}

void parser_free(Parser* parser) {
    if (!parser) return;
    lexer_free(parser->lexer);
    free(parser->current_token.value);
    free(parser->peek_token.value);
    free(parser->error_message);
    free(parser);
}

void parser_advance(Parser* parser) {
    free(parser->current_token.value);
    parser->current_token = parser->peek_token;
    parser->peek_token = lexer_next_token(parser->lexer);
}

bool parser_expect(Parser* parser, TokenType expected) {
    if (parser->current_token.type == expected) {
        parser_advance(parser);
        return true;
    }
    
    // Set error message
    free(parser->error_message);
    parser->error_message = malloc(256);
    if (!parser->error_message) {
        return false;
    }
    snprintf(parser->error_message, 256,
            "Expected token type %d, got %d at line %zu", 
            expected, parser->current_token.type, parser->current_token.line);
    return false;
}

void parser_skip_newlines(Parser* parser) {
    while (parser->current_token.type == TOKEN_NEWLINE || 
           parser->current_token.type == TOKEN_COMMENT) {
        parser_advance(parser);
    }
}

// Parse string value (with or without quotes)
char* parser_parse_string_value(Parser* parser) {
    if (parser->current_token.type == TOKEN_STRING) {
        char* value = string_duplicate(parser->current_token.value);
        parser_advance(parser);
        return value;
    } else if (parser->current_token.type == TOKEN_IDENTIFIER) {
        char* value = string_duplicate(parser->current_token.value);
        parser_advance(parser);
        return value;
    }
    return NULL;
}

// Parse array of strings
char** parser_parse_string_array(Parser* parser, size_t* count) {
    *count = 0;
    
    if (!parser_expect(parser, TOKEN_LBRACKET)) {
        return NULL;
    }
    
    parser_skip_newlines(parser);
    
    // Count elements first
    size_t capacity = 4;
    char** array = malloc(capacity * sizeof(char*));
    if (!array) {
        *count = 0;
        return NULL;
    }

    while (parser->current_token.type != TOKEN_RBRACKET &&
           parser->current_token.type != TOKEN_EOF) {
        
        char* value = parser_parse_string_value(parser);
        if (!value) break;
        
        if (*count >= capacity) {
            capacity *= 2;
            char** grown = realloc(array, capacity * sizeof(char*));
            if (!grown) {
                // realloc failed: free what we hold (incl. the just-parsed value)
                // and the original block, rather than leak it and write through NULL.
                free(value);
                for (size_t i = 0; i < *count; i++) {
                    free(array[i]);
                }
                free(array);
                *count = 0;
                return NULL;
            }
            array = grown;
        }

        array[*count] = value;
        (*count)++;
        
        parser_skip_newlines(parser);
        
        if (parser->current_token.type == TOKEN_COMMA) {
            parser_advance(parser);
            parser_skip_newlines(parser);
        }
    }
    
    parser_expect(parser, TOKEN_RBRACKET);
    return array;
}

// Parse dependencies section
Dependency* parser_parse_dependencies(Parser* parser) {
    if (!parser_expect(parser, TOKEN_LBRACE)) {
        return NULL;
    }
    
    parser_skip_newlines(parser);
    
    Dependency* deps = NULL;
    
    while (parser->current_token.type != TOKEN_RBRACE && 
           parser->current_token.type != TOKEN_EOF) {
        
        if (parser->current_token.type != TOKEN_IDENTIFIER && 
            parser->current_token.type != TOKEN_STRING) {
            parser_advance(parser);
            continue;
        }
        
        char* name = string_duplicate(parser->current_token.value);
        parser_advance(parser);
        
        if (!parser_expect(parser, TOKEN_COLON)) {
            free(name);
            continue;
        }
        
        char* version = parser_parse_string_value(parser);
        if (!version) {
            free(name);
            continue;
        }
        
        Dependency* dep = dependency_create(name, version);
        if (dep) {
            dep->next = deps;
            deps = dep;
        }
        
        free(name);
        free(version);
        
        parser_skip_newlines(parser);
        
        if (parser->current_token.type == TOKEN_COMMA) {
            parser_advance(parser);
            parser_skip_newlines(parser);
        }
    }
    
    parser_expect(parser, TOKEN_RBRACE);
    return deps;
}

// Parse features section
FeatureConfig* parser_parse_features(Parser* parser) {
    if (!parser_expect(parser, TOKEN_LBRACE)) {
        return NULL;
    }
    
    parser_skip_newlines(parser);
    
    FeatureConfig* features = NULL;
    
    while (parser->current_token.type != TOKEN_RBRACE && 
           parser->current_token.type != TOKEN_EOF) {
        
        if (parser->current_token.type != TOKEN_IDENTIFIER && 
            parser->current_token.type != TOKEN_STRING) {
            parser_advance(parser);
            continue;
        }
        
        char* name = string_duplicate(parser->current_token.value);
        parser_advance(parser);
        
        if (!parser_expect(parser, TOKEN_COLON)) {
            free(name);
            continue;
        }
        
        FeatureConfig* feature = feature_config_create(name, false);
        
        // Parse feature configuration
        if (parser->current_token.type == TOKEN_LBRACE) {
            parser_advance(parser);
            parser_skip_newlines(parser);
            
            while (parser->current_token.type != TOKEN_RBRACE && 
                   parser->current_token.type != TOKEN_EOF) {
                
                if (parser->current_token.type == TOKEN_IDENTIFIER) {
                    char* key = string_duplicate(parser->current_token.value);
                    parser_advance(parser);
                    
                    if (parser_expect(parser, TOKEN_COLON)) {
                        if (strcmp(key, "default") == 0) {
                            if (parser->current_token.type == TOKEN_IDENTIFIER) {
                                feature->default_enabled = (strcmp(parser->current_token.value, "true") == 0);
                                parser_advance(parser);
                            }
                        }
                        // TODO: Parse requires, conflicts, etc.
                    }
                    
                    free(key);
                }
                
                parser_skip_newlines(parser);
                
                if (parser->current_token.type == TOKEN_COMMA) {
                    parser_advance(parser);
                    parser_skip_newlines(parser);
                }
            }
            
            parser_expect(parser, TOKEN_RBRACE);
        }
        
        feature->next = features;
        features = feature;
        
        free(name);
        
        parser_skip_newlines(parser);
        
        if (parser->current_token.type == TOKEN_COMMA) {
            parser_advance(parser);
            parser_skip_newlines(parser);
        }
    }
    
    parser_expect(parser, TOKEN_RBRACE);
    return features;
}

// Main parsing function
GooMod* goo_mod_parse_string(const char* content) {
    if (!content) return NULL;
    
    Parser* parser = parser_create(content);
    if (!parser) return NULL;
    
    GooMod* gmod = xcalloc(1, sizeof(GooMod));
    if (!gmod) {
        parser_free(parser);
        return NULL;
    }
    
    // Set defaults
    gmod->created_at = time(NULL);
    gmod->updated_at = gmod->created_at;
    
    parser_skip_newlines(parser);
    
    while (parser->current_token.type != TOKEN_EOF) {
        if (parser->current_token.type == TOKEN_IDENTIFIER) {
            char* key = string_duplicate(parser->current_token.value);
            parser_advance(parser);
            
            if (parser_expect(parser, TOKEN_COLON) || 
                parser->current_token.type == TOKEN_LBRACE) {
                
                if (strcmp(key, "module") == 0) {
                    gmod->module_path = parser_parse_string_value(parser);
                } else if (strcmp(key, "version") == 0) {
                    char* version_str = parser_parse_string_value(parser);
                    if (version_str) {
                        gmod->version = version_parse(version_str);
                        free(version_str);
                    }
                } else if (strcmp(key, "description") == 0) {
                    gmod->description = parser_parse_string_value(parser);
                } else if (strcmp(key, "license") == 0) {
                    gmod->license = parser_parse_string_value(parser);
                } else if (strcmp(key, "goo_version") == 0) {
                    gmod->goo_version = parser_parse_string_value(parser);
                } else if (strcmp(key, "dependencies") == 0) {
                    gmod->dependencies = parser_parse_dependencies(parser);
                } else if (strcmp(key, "dev_dependencies") == 0) {
                    gmod->dev_dependencies = parser_parse_dependencies(parser);
                } else if (strcmp(key, "features") == 0) {
                    gmod->features = parser_parse_features(parser);
                } else if (strcmp(key, "authors") == 0) {
                    gmod->authors = parser_parse_string_array(parser, &gmod->author_count);
                } else {
                    // Skip unknown sections
                    if (parser->current_token.type == TOKEN_LBRACE) {
                        int brace_count = 1;
                        parser_advance(parser);
                        while (brace_count > 0 && parser->current_token.type != TOKEN_EOF) {
                            if (parser->current_token.type == TOKEN_LBRACE) {
                                brace_count++;
                            } else if (parser->current_token.type == TOKEN_RBRACE) {
                                brace_count--;
                            }
                            parser_advance(parser);
                        }
                    } else {
                        parser_advance(parser); // Skip value
                    }
                }
            }
            
            free(key);
        } else {
            parser_advance(parser);
        }
        
        parser_skip_newlines(parser);
    }
    
    // Validate parsed module
    char* error_msg = NULL;
    if (!module_validate((Module*)gmod, &error_msg)) {
        printf("Warning: Module validation failed: %s\n", error_msg);
        free(error_msg);
    }
    
    parser_free(parser);
    return gmod;
}

GooMod* goo_mod_parse_file(const char* filepath) {
    if (!filepath || !file_exists(filepath)) {
        return NULL;
    }
    
    char* content = read_file_contents(filepath);
    if (!content) return NULL;
    
    GooMod* gmod = goo_mod_parse_string(content);
    free(content);
    
    return gmod;
}

// Serialization functions
char* goo_mod_to_string(const GooMod* gmod) {
    if (!gmod) return NULL;
    
    size_t buffer_size = 8192;
    char* buffer = malloc(buffer_size);
    size_t written = 0;
    
    // Module header
    if (gmod->module_path) {
        written += snprintf(buffer + written, buffer_size - written,
            "module \"%s\" {\n", gmod->module_path);
    }
    
    if (gmod->version) {
        char* version_str = version_to_string(gmod->version);
        written += snprintf(buffer + written, buffer_size - written,
            "    version: \"%s\"\n", version_str);
        free(version_str);
    }
    
    if (gmod->goo_version) {
        written += snprintf(buffer + written, buffer_size - written,
            "    goo_version: \"%s\"\n", gmod->goo_version);
    }
    
    if (gmod->description) {
        written += snprintf(buffer + written, buffer_size - written,
            "    description: \"%s\"\n", gmod->description);
    }
    
    if (gmod->license) {
        written += snprintf(buffer + written, buffer_size - written,
            "    license: \"%s\"\n", gmod->license);
    }
    
    // Dependencies
    if (gmod->dependencies) {
        written += snprintf(buffer + written, buffer_size - written,
            "\n    dependencies: {\n");
        
        for (Dependency* dep = gmod->dependencies; dep; dep = dep->next) {
            char* constraint_str = version_constraint_to_string(dep->constraint);
            written += snprintf(buffer + written, buffer_size - written,
                "        \"%s\": \"%s\"\n", dep->name, constraint_str);
            free(constraint_str);
        }
        
        written += snprintf(buffer + written, buffer_size - written, "    }\n");
    }
    
    // Features
    if (gmod->features) {
        written += snprintf(buffer + written, buffer_size - written,
            "\n    features: {\n");
        
        for (FeatureConfig* feature = gmod->features; feature; feature = feature->next) {
            written += snprintf(buffer + written, buffer_size - written,
                "        \"%s\": { default: %s }\n", 
                feature->name, feature->default_enabled ? "true" : "false");
        }
        
        written += snprintf(buffer + written, buffer_size - written, "    }\n");
    }
    
    written += snprintf(buffer + written, buffer_size - written, "}\n");
    
    return buffer;
}

bool goo_mod_save_file(const GooMod* gmod, const char* filepath) {
    if (!gmod || !filepath) return false;
    
    char* content = goo_mod_to_string(gmod);
    if (!content) return false;
    
    bool result = write_file_contents(filepath, content);
    free(content);
    return result;
}