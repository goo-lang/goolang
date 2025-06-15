#include "lexer.h"
#include "token.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return 1;
    }
    
    FILE* file = fopen(argv[1], "r");
    if (!file) {
        fprintf(stderr, "Error: Cannot open file %s\n", argv[1]);
        return 1;
    }
    
    // Read file content
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* content = malloc(size + 1);
    fread(content, 1, size, file);
    content[size] = '\0';
    fclose(file);
    
    // Create lexer
    Lexer* lexer = lexer_new(content, argv[1]);
    
    // Tokenize
    Token* token;
    int count = 0;
    while ((token = lexer_next_token(lexer)) && token->type != TOKEN_EOF && count < 20) {
        printf("Token %d: %s '%s' at %d:%d\n", 
               count, 
               token_type_string(token->type), 
               token->literal ? token->literal : "(null)",
               token->pos.line,
               token->pos.column);
        token_free(token);
        count++;
    }
    
    if (token) token_free(token);
    lexer_free(lexer);
    free(content);
    
    return 0;
}