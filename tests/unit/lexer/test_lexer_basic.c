#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Unit tests for lexer component
#include "../../../include/lexer.h"
#include "../../../include/token.h"

// Test basic tokenization
void test_basic_tokens() {
    printf("Testing basic tokenization...\n");
    
    const char* source = "package main func { }";
    Lexer* lexer = lexer_new(source, "test.goo");
    assert(lexer != NULL);
    
    // Test package token
    Token* token = lexer_next_token(lexer);
    assert(token != NULL);
    assert(token->type == TOKEN_PACKAGE);
    assert(strcmp(token->literal, "package") == 0);
    token_free(token);
    
    // Test identifier
    token = lexer_next_token(lexer);
    assert(token != NULL);
    assert(token->type == TOKEN_IDENT);
    assert(strcmp(token->literal, "main") == 0);
    token_free(token);
    
    // Test func keyword
    token = lexer_next_token(lexer);
    assert(token != NULL);
    assert(token->type == TOKEN_FUNC);
    assert(strcmp(token->literal, "func") == 0);
    token_free(token);
    
    // Test left brace
    token = lexer_next_token(lexer);
    assert(token != NULL);
    assert(token->type == TOKEN_LBRACE);
    token_free(token);
    
    // Test right brace
    token = lexer_next_token(lexer);
    assert(token != NULL);
    assert(token->type == TOKEN_RBRACE);
    token_free(token);
    
    // Test EOF
    token = lexer_next_token(lexer);
    assert(token != NULL);
    assert(token->type == TOKEN_EOF);
    token_free(token);
    
    lexer_free(lexer);
    printf("✓ Basic tokenization test passed\n");
}

// Test number literals
void test_number_literals() {
    printf("Testing number literals...\n");
    
    const char* source = "42 3.14 0x1F 0b1010";
    Lexer* lexer = lexer_new(source, "test.goo");
    assert(lexer != NULL);
    
    // Test integer
    Token* token = lexer_next_token(lexer);
    assert(token != NULL);
    assert(token->type == TOKEN_INT);
    assert(strcmp(token->literal, "42") == 0);
    token_free(token);
    
    // Test float
    token = lexer_next_token(lexer);
    assert(token != NULL);
    assert(token->type == TOKEN_FLOAT);
    assert(strcmp(token->literal, "3.14") == 0);
    token_free(token);
    
    lexer_free(lexer);
    printf("✓ Number literals test passed\n");
}

// Test string literals
void test_string_literals() {
    printf("Testing string literals...\n");

    const char* source = "\"hello world\" \"escaped\\nstring\"";
    Lexer* lexer = lexer_new(source, "test.goo");
    assert(lexer != NULL);

    // Test basic string
    Token* token = lexer_next_token(lexer);
    assert(token != NULL);
    assert(token->type == TOKEN_STRING);
    assert(strcmp(token->literal, "hello world") == 0);
    token_free(token);

    // Test escaped string — \n must be processed to a real newline byte
    token = lexer_next_token(lexer);
    assert(token != NULL);
    assert(token->type == TOKEN_STRING);
    assert(token->literal != NULL);
    assert(strcmp(token->literal, "escaped\nstring") == 0);
    assert(token->length == strlen("escaped\nstring"));
    token_free(token);

    lexer_free(lexer);
    printf("✓ String literals test passed\n");
}

// Regression: empty string "" used to SIGSEGV downstream because token_new
// returned token->literal = NULL when length == 0.
void test_empty_string_literal() {
    printf("Testing empty string literal...\n");

    Lexer* lexer = lexer_new("\"\"", "test.goo");
    assert(lexer != NULL);

    Token* token = lexer_next_token(lexer);
    assert(token != NULL);
    assert(token->type == TOKEN_STRING);
    assert(token->literal != NULL);
    assert(token->length == 0);
    assert(token->literal[0] == '\0');
    token_free(token);

    lexer_free(lexer);
    printf("✓ Empty string literal test passed\n");
}

// Regression: the string lexer used to copy the raw source bytes verbatim,
// leaving backslashes in the literal instead of processing escapes.
void test_string_escape_processing() {
    printf("Testing string escape processing...\n");

    // \" — embedded quote
    {
        Lexer* lexer = lexer_new("\"a\\\"b\"", "test.goo");
        Token* token = lexer_next_token(lexer);
        assert(token && token->type == TOKEN_STRING);
        assert(strcmp(token->literal, "a\"b") == 0);
        assert(token->length == 3);
        token_free(token);
        lexer_free(lexer);
    }

    // \\ — single backslash
    {
        Lexer* lexer = lexer_new("\"a\\\\b\"", "test.goo");
        Token* token = lexer_next_token(lexer);
        assert(token && token->type == TOKEN_STRING);
        assert(strcmp(token->literal, "a\\b") == 0);
        assert(token->length == 3);
        token_free(token);
        lexer_free(lexer);
    }

    // \t — tab
    {
        Lexer* lexer = lexer_new("\"a\\tb\"", "test.goo");
        Token* token = lexer_next_token(lexer);
        assert(token && token->type == TOKEN_STRING);
        assert(strcmp(token->literal, "a\tb") == 0);
        token_free(token);
        lexer_free(lexer);
    }

    // \0 — embedded NUL (length must reflect the real byte count, not strlen)
    {
        Lexer* lexer = lexer_new("\"a\\0b\"", "test.goo");
        Token* token = lexer_next_token(lexer);
        assert(token && token->type == TOKEN_STRING);
        assert(token->length == 3);
        assert(token->literal[0] == 'a');
        assert(token->literal[1] == '\0');
        assert(token->literal[2] == 'b');
        token_free(token);
        lexer_free(lexer);
    }

    // "\\" — the whole literal is a single escaped backslash. Guards against
    // a regression where a trailing backslash in the raw range gets emitted
    // unescaped instead of being treated as the start of an escape pair.
    {
        Lexer* lexer = lexer_new("\"\\\\\"", "test.goo");
        Token* token = lexer_next_token(lexer);
        assert(token && token->type == TOKEN_STRING);
        assert(token->length == 1);
        assert(token->literal[0] == '\\');
        token_free(token);
        lexer_free(lexer);
    }

    printf("✓ String escape processing test passed\n");
}

// Test operators
void test_operators() {
    printf("Testing operators...\n");
    
    const char* source = "+ - * / = == != < > <= >=";
    Lexer* lexer = lexer_new(source, "test.goo");
    assert(lexer != NULL);
    
    TokenType expected[] = {
        TOKEN_PLUS, TOKEN_MINUS, TOKEN_MULTIPLY, TOKEN_DIVIDE,
        TOKEN_ASSIGN, TOKEN_EQ, TOKEN_NE, TOKEN_LT, TOKEN_GT,
        TOKEN_LE, TOKEN_GE, TOKEN_EOF
    };
    
    for (int i = 0; expected[i] != TOKEN_EOF; i++) {
        Token* token = lexer_next_token(lexer);
        assert(token != NULL);
        assert(token->type == expected[i]);
        token_free(token);
    }
    
    lexer_free(lexer);
    printf("✓ Operators test passed\n");
}

// Test keywords
void test_keywords() {
    printf("Testing keywords...\n");
    
    const char* source = "if else for break continue return var const func";
    Lexer* lexer = lexer_new(source, "test.goo");
    assert(lexer != NULL);
    
    TokenType expected[] = {
        TOKEN_IF, TOKEN_ELSE, TOKEN_FOR,
        TOKEN_BREAK, TOKEN_CONTINUE, TOKEN_RETURN,
        TOKEN_VAR, TOKEN_CONST, TOKEN_FUNC, TOKEN_EOF
    };
    
    for (int i = 0; expected[i] != TOKEN_EOF; i++) {
        Token* token = lexer_next_token(lexer);
        assert(token != NULL);
        assert(token->type == expected[i]);
        token_free(token);
    }
    
    lexer_free(lexer);
    printf("✓ Keywords test passed\n");
}

int main() {
    printf("=== Lexer Unit Tests ===\n");
    
    test_basic_tokens();
    test_number_literals();
    test_string_literals();
    test_empty_string_literal();
    test_string_escape_processing();
    test_operators();
    test_keywords();
    
    printf("\n✅ All lexer unit tests passed!\n");
    return 0;
}