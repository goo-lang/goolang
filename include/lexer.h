#ifndef LEXER_H
#define LEXER_H

#include "token.h"
#include <stddef.h>
#include <stdio.h>

// Lexer state structure
typedef struct {
    const char* input;      // Source code input
    size_t input_length;    // Length of input
    size_t position;        // Current position in input
    size_t read_position;   // Current reading position (after current char)
    char ch;                // Current character under examination
    Position pos;           // Current position info (line, column, etc.)
    const char* filename;   // Name of the file being lexed
    TokenType prev_token_type; // Last significant token, for newline -> ';'
                               // insertion (targeted ASI; see lexer.c).
} Lexer;

// Lexer functions
Lexer* lexer_new(const char* input, const char* filename);
void lexer_free(Lexer* lexer);

// Main lexing function
Token* lexer_next_token(Lexer* lexer);

// Character handling functions
void lexer_read_char(Lexer* lexer);
char lexer_peek_char(Lexer* lexer);
void lexer_skip_whitespace(Lexer* lexer);

// Literal parsing functions
char* lexer_read_identifier(Lexer* lexer, size_t* length);
char* lexer_read_number(Lexer* lexer, size_t* length, int* is_float);
char* lexer_read_string(Lexer* lexer, size_t* length);
char* lexer_read_char_literal(Lexer* lexer, size_t* length);
int lexer_decode_char_value(const char* body, size_t len, long* out);

// Utility functions
int lexer_is_letter(char ch);
int lexer_is_digit(char ch);
int lexer_is_hex_digit(char ch);
int lexer_is_whitespace(char ch);

// Error handling
void lexer_error(Lexer* lexer, const char* message);

#endif // LEXER_H