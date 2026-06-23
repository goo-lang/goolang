#include "lexer.h"
#include "token.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

Lexer* lexer_new(const char* input, const char* filename) {
    Lexer* lexer = malloc(sizeof(Lexer));
    if (!lexer) return NULL;

    lexer->input = input;
    lexer->input_length = strlen(input);
    lexer->position = 0;
    lexer->read_position = 0;
    lexer->ch = 0;
    lexer->filename = filename;
    lexer->type_names = NULL;  // Initialize empty type name list
    // Start as if the previous token were a semicolon, so leading blank lines
    // never trigger automatic semicolon insertion.
    lexer->prev_token_type = TOKEN_SEMICOLON;
    lexer->paren_depth = 0;

    // Initialize position
    lexer->pos.line = 1;
    lexer->pos.column = 1;
    lexer->pos.offset = 0;
    lexer->pos.filename = filename;

    // Read first character
    lexer_read_char(lexer);

    return lexer;
}

void lexer_free(Lexer* lexer) {
    if (lexer) {
        // Free type names list
        lexer_clear_type_names(lexer);
        free(lexer);
    }
}

void lexer_read_char(Lexer* lexer) {
    if (lexer->read_position >= lexer->input_length) {
        lexer->ch = 0; // EOF
    } else {
        lexer->ch = lexer->input[lexer->read_position];
    }
    
    lexer->position = lexer->read_position;
    lexer->read_position++;
    
    // Update position tracking
    if (lexer->ch == '\n') {
        lexer->pos.line++;
        lexer->pos.column = 1;
    } else {
        lexer->pos.column++;
    }
    lexer->pos.offset = lexer->position;
}

char lexer_peek_char(Lexer* lexer) {
    if (lexer->read_position >= lexer->input_length) {
        return 0;
    }
    return lexer->input[lexer->read_position];
}

void lexer_skip_whitespace(Lexer* lexer) {
    while (lexer_is_whitespace(lexer->ch) && lexer->ch != '\n') {
        lexer_read_char(lexer);
    }
}

// Whether a newline immediately following this token triggers automatic
// semicolon insertion (Go's ASI rule). RBRACE and EOF are handled separately.
static int asi_token_ends_statement(TokenType t) {
    switch (t) {
        case TOKEN_IDENT:
        case TOKEN_TYPE_IDENT:
        case TOKEN_INT:
        case TOKEN_FLOAT:
        case TOKEN_STRING:
        case TOKEN_CHAR:
        case TOKEN_TRUE:
        case TOKEN_FALSE:
        case TOKEN_NIL:
        case TOKEN_RETURN:
        case TOKEN_BREAK:
        case TOKEN_CONTINUE:
        case TOKEN_FALLTHROUGH:
        case TOKEN_INCREMENT:
        case TOKEN_DECREMENT:
        case TOKEN_RPAREN:
        case TOKEN_RBRACKET:
            return 1;
        default:
            return 0;
    }
}

Token* lexer_next_token(Lexer* lexer) {
    Token* token = NULL;

    for (;;) {
    Position current_pos = lexer->pos;

    lexer_skip_whitespace(lexer);
    current_pos = lexer->pos; // Update position after skipping whitespace

    switch (lexer->ch) {
        case 0:
            token = token_new(TOKEN_EOF, NULL, 0, current_pos);
            break;

        case '\n':
            // Automatic semicolon insertion (Go ASI): a newline following a
            // token that can end a statement becomes a synthesized semicolon.
            // Otherwise the newline is insignificant.
            if (lexer->paren_depth == 0 &&
                asi_token_ends_statement(lexer->prev_token_type)) {
                token = token_new(TOKEN_SEMICOLON, ";", 1, current_pos);
                lexer_read_char(lexer);
                break;
            }
            lexer_read_char(lexer);
            continue; // Get next token iteratively
            
        // Single character tokens
        case '(':
            token = token_new(TOKEN_LPAREN, "(", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case ')':
            token = token_new(TOKEN_RPAREN, ")", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case '{':
            token = token_new(TOKEN_LBRACE, "{", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case '}':
            token = token_new(TOKEN_RBRACE, "}", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case '[':
            token = token_new(TOKEN_LBRACKET, "[", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case ']':
            token = token_new(TOKEN_RBRACKET, "]", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case ';':
            token = token_new(TOKEN_SEMICOLON, ";", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case ',':
            token = token_new(TOKEN_COMMA, ",", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case ':':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_SHORT_ASSIGN, ":=", 2, current_pos);
            } else {
                token = token_new(TOKEN_COLON, ":", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
        case '~':
            token = token_new(TOKEN_BIT_NOT, "~", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case '?':
            token = token_new(TOKEN_QUESTION, "?", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case '@':
            token = token_new(TOKEN_DEREF, "@", 1, current_pos);
            lexer_read_char(lexer);
            break;
            
        // Multi-character operators
        case '+':
            if (lexer_peek_char(lexer) == '+') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_INCREMENT, "++", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_PLUS_ASSIGN, "+=", 2, current_pos);
            } else {
                token = token_new(TOKEN_PLUS, "+", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '-':
            if (lexer_peek_char(lexer) == '-') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_DECREMENT, "--", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_MINUS_ASSIGN, "-=", 2, current_pos);
            } else {
                token = token_new(TOKEN_MINUS, "-", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '*':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_MUL_ASSIGN, "*=", 2, current_pos);
            } else {
                token = token_new(TOKEN_MULTIPLY, "*", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '/':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_DIV_ASSIGN, "/=", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '/') {
                // Line comment - skip to end of line
                while (lexer->ch != '\n' && lexer->ch != 0) {
                    lexer_read_char(lexer);
                }
                continue; // Get next token iteratively
            } else if (lexer_peek_char(lexer) == '*') {
                // Block comment - skip to */
                lexer_read_char(lexer); // skip /
                lexer_read_char(lexer); // skip *
                while (lexer->ch != 0) {
                    if (lexer->ch == '*' && lexer_peek_char(lexer) == '/') {
                        lexer_read_char(lexer); // skip *
                        lexer_read_char(lexer); // skip /
                        break;
                    }
                    lexer_read_char(lexer);
                }
                continue; // Get next token iteratively
            } else {
                token = token_new(TOKEN_DIVIDE, "/", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '%':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_MOD_ASSIGN, "%=", 2, current_pos);
            } else {
                token = token_new(TOKEN_MODULO, "%", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '=':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_EQ, "==", 2, current_pos);
            } else {
                token = token_new(TOKEN_ASSIGN, "=", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '!':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_NE, "!=", 2, current_pos);
            } else {
                token = token_new(TOKEN_BANG, "!", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '<':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_LE, "<=", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '<') {
                lexer_read_char(lexer);
                if (lexer_peek_char(lexer) == '=') {
                    lexer_read_char(lexer);
                    lexer_read_char(lexer);
                    token = token_new(TOKEN_LSHIFT_ASSIGN, "<<=", 3, current_pos);
                } else {
                    lexer_read_char(lexer);
                    token = token_new(TOKEN_LSHIFT, "<<", 2, current_pos);
                }
            } else if (lexer_peek_char(lexer) == '-') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_ARROW, "<-", 2, current_pos);
            } else {
                token = token_new(TOKEN_LT, "<", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '>':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_GE, ">=", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '>') {
                lexer_read_char(lexer);
                if (lexer_peek_char(lexer) == '=') {
                    lexer_read_char(lexer);
                    lexer_read_char(lexer);
                    token = token_new(TOKEN_RSHIFT_ASSIGN, ">>=", 3, current_pos);
                } else {
                    lexer_read_char(lexer);
                    token = token_new(TOKEN_RSHIFT, ">>", 2, current_pos);
                }
            } else {
                token = token_new(TOKEN_GT, ">", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '&':
            if (lexer_peek_char(lexer) == '&') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_AND, "&&", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_AND_ASSIGN, "&=", 2, current_pos);
            } else {
                token = token_new(TOKEN_BIT_AND, "&", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '|':
            if (lexer_peek_char(lexer) == '|') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_OR, "||", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_OR_ASSIGN, "|=", 2, current_pos);
            } else {
                token = token_new(TOKEN_BIT_OR, "|", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '^':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_XOR_ASSIGN, "^=", 2, current_pos);
            } else {
                token = token_new(TOKEN_BIT_XOR, "^", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '.':
            if (lexer_peek_char(lexer) == '.') {
                lexer_read_char(lexer);
                if (lexer_peek_char(lexer) == '.') {
                    lexer_read_char(lexer);
                    lexer_read_char(lexer);
                    token = token_new(TOKEN_ELLIPSIS, "...", 3, current_pos);
                } else {
                    // This is an error - two dots without a third
                    token = token_new(TOKEN_ERROR, "..", 2, current_pos);
                    lexer_read_char(lexer);
                }
            } else {
                token = token_new(TOKEN_DOT, ".", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '"':
            {
                size_t length;
                char* string_literal = lexer_read_string(lexer, &length);
                if (string_literal) {
                    token = token_new(TOKEN_STRING, string_literal, length, current_pos);
                    free(string_literal);
                } else {
                    token = token_new(TOKEN_ERROR, "unterminated string", 18, current_pos);
                }
            }
            break;
            
        case '\'':
            {
                size_t length;
                char* char_literal = lexer_read_char_literal(lexer, &length);
                if (char_literal) {
                    token = token_new(TOKEN_CHAR, char_literal, length, current_pos);
                    free(char_literal);
                } else {
                    token = token_new(TOKEN_ERROR, "unterminated character", 22, current_pos);
                }
            }
            break;
            
        default:
            if (lexer_is_letter(lexer->ch)) {
                size_t length;
                char* identifier = lexer_read_identifier(lexer, &length);
                if (identifier) {
                    TokenType type;
                    if (token_is_keyword(identifier)) {
                        type = token_keyword_type(identifier);
                    } else if (lexer_is_type_name(lexer, identifier)) {
                        type = TOKEN_TYPE_IDENT;
                    } else {
                        type = TOKEN_IDENT;
                    }
                    token = token_new(type, identifier, length, current_pos);
                    free(identifier);
                } else {
                    token = token_new(TOKEN_ERROR, "invalid identifier", 18, current_pos);
                }
            } else if (lexer_is_digit(lexer->ch)) {
                size_t length;
                int is_float;
                char* number = lexer_read_number(lexer, &length, &is_float);
                if (number) {
                    TokenType type = is_float ? TOKEN_FLOAT : TOKEN_INT;
                    token = token_new(type, number, length, current_pos);
                    free(number);
                } else {
                    token = token_new(TOKEN_ERROR, "invalid number", 14, current_pos);
                }
            } else {
                token = token_new(TOKEN_UNKNOWN, NULL, 0, current_pos);
                lexer_read_char(lexer);
            }
            break;
    }

    // Track ()/[] nesting so ASI is suppressed inside multi-line expressions,
    // and record the token so the next newline can decide on ASI. Updating
    // prev for synthesized semicolons too means consecutive blank lines never
    // produce consecutive semicolons.
    if (token) {
        if (token->type == TOKEN_LPAREN || token->type == TOKEN_LBRACKET) {
            lexer->paren_depth++;
        } else if ((token->type == TOKEN_RPAREN || token->type == TOKEN_RBRACKET) &&
                   lexer->paren_depth > 0) {
            lexer->paren_depth--;
        }
        lexer->prev_token_type = token->type;
    }
    return token;
    } // end for(;;)
}

char* lexer_read_identifier(Lexer* lexer, size_t* length) {
    size_t start_pos = lexer->position;
    
    while (lexer_is_letter(lexer->ch) || lexer_is_digit(lexer->ch)) {
        lexer_read_char(lexer);
    }
    
    size_t len = lexer->position - start_pos;
    if (length) *length = len;
    
    char* identifier = malloc(len + 1);
    if (!identifier) return NULL;
    
    strncpy(identifier, &lexer->input[start_pos], len);
    identifier[len] = '\0';
    
    return identifier;
}

char* lexer_read_number(Lexer* lexer, size_t* length, int* is_float) {
    size_t start_pos = lexer->position;
    *is_float = 0;
    
    // Read integer part
    while (lexer_is_digit(lexer->ch)) {
        lexer_read_char(lexer);
    }
    
    // Check for decimal point
    if (lexer->ch == '.' && lexer_is_digit(lexer_peek_char(lexer))) {
        *is_float = 1;
        lexer_read_char(lexer); // consume '.'
        
        // Read fractional part
        while (lexer_is_digit(lexer->ch)) {
            lexer_read_char(lexer);
        }
    }
    
    // Check for exponent
    if (lexer->ch == 'e' || lexer->ch == 'E') {
        *is_float = 1;
        lexer_read_char(lexer); // consume 'e' or 'E'
        
        if (lexer->ch == '+' || lexer->ch == '-') {
            lexer_read_char(lexer); // consume sign
        }
        
        while (lexer_is_digit(lexer->ch)) {
            lexer_read_char(lexer);
        }
    }
    
    size_t len = lexer->position - start_pos;
    if (length) *length = len;
    
    char* number = malloc(len + 1);
    if (!number) return NULL;
    
    strncpy(number, &lexer->input[start_pos], len);
    number[len] = '\0';
    
    return number;
}

char* lexer_read_string(Lexer* lexer, size_t* length) {
    lexer_read_char(lexer); // consume opening quote
    size_t start_pos = lexer->position;
    
    while (lexer->ch != '"' && lexer->ch != 0) {
        if (lexer->ch == '\\') {
            lexer_read_char(lexer); // skip escape character
            if (lexer->ch != 0) {
                lexer_read_char(lexer); // skip escaped character
            }
        } else {
            lexer_read_char(lexer);
        }
    }
    
    if (lexer->ch != '"') {
        return NULL; // Unterminated string
    }
    
    size_t len = lexer->position - start_pos;
    if (length) *length = len;
    
    char* string = malloc(len + 1);
    if (!string) return NULL;
    
    strncpy(string, &lexer->input[start_pos], len);
    string[len] = '\0';
    
    lexer_read_char(lexer); // consume closing quote
    
    return string;
}

char* lexer_read_char_literal(Lexer* lexer, size_t* length) {
    lexer_read_char(lexer); // consume opening quote
    size_t start_pos = lexer->position;
    
    if (lexer->ch == '\\') {
        lexer_read_char(lexer); // skip escape character
        if (lexer->ch != 0) {
            lexer_read_char(lexer); // skip escaped character
        }
    } else if (lexer->ch != 0) {
        lexer_read_char(lexer);
    }
    
    if (lexer->ch != '\'') {
        return NULL; // Unterminated or invalid character literal
    }
    
    size_t len = lexer->position - start_pos;
    if (length) *length = len;
    
    char* char_lit = malloc(len + 1);
    if (!char_lit) return NULL;
    
    strncpy(char_lit, &lexer->input[start_pos], len);
    char_lit[len] = '\0';
    
    lexer_read_char(lexer); // consume closing quote
    
    return char_lit;
}

int lexer_is_letter(char ch) {
    return isalpha(ch) || ch == '_';
}

int lexer_is_digit(char ch) {
    return isdigit(ch);
}

int lexer_is_hex_digit(char ch) {
    return isxdigit(ch);
}

int lexer_is_whitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r';
}

void lexer_error(Lexer* lexer, const char* message) {
    fprintf(stderr, "Lexer error at %s:%d:%d: %s\n", 
            lexer->pos.filename ? lexer->pos.filename : "<stdin>",
            lexer->pos.line, lexer->pos.column, message);
}
// Type name tracking functions

void lexer_register_type_name(Lexer* lexer, const char* name) {
    if (!lexer || !name) return;
    
    // Check if already registered
    if (lexer_is_type_name(lexer, name)) {
        return;
    }
    
    // Create new node
    TypeNameNode* node = malloc(sizeof(TypeNameNode));
    if (!node) return;
    
    node->name = strdup(name);
    node->next = lexer->type_names;
    lexer->type_names = node;
}

int lexer_is_type_name(Lexer* lexer, const char* name) {
    if (!lexer || !name) return 0;
    
    TypeNameNode* current = lexer->type_names;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return 1;
        }
        current = current->next;
    }
    return 0;
}

void lexer_clear_type_names(Lexer* lexer) {
    if (!lexer) return;
    
    TypeNameNode* current = lexer->type_names;
    while (current) {
        TypeNameNode* next = current->next;
        free(current->name);
        free(current);
        current = next;
    }
    lexer->type_names = NULL;
}
