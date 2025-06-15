#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "token.h"
#include "parser.h"
#include "ast.h"
#include "types.h"

void print_token(const Token* token) {
    printf("Token: %-15s | Literal: %-10s | Pos: %d:%d\n",
           token_type_string(token->type),
           token->literal ? token->literal : "NULL",
           token->pos.line,
           token->pos.column);
}

void test_lexer(const char* input, const char* test_name) {
    printf("\n=== Lexer Test: %s ===\n", test_name);
    printf("Input: %s\n", input);
    printf("Tokens:\n");
    
    Lexer* lexer = lexer_new(input, "test.goo");
    if (!lexer) {
        printf("Failed to create lexer\n");
        return;
    }
    
    Token* token;
    do {
        token = lexer_next_token(lexer);
        if (token) {
            print_token(token);
            if (token->type == TOKEN_EOF) {
                token_free(token);
                break;
            }
            token_free(token);
        }
    } while (token);
    
    lexer_free(lexer);
    printf("\n");
}

int compile_goo_file(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        printf("Error: Could not open file '%s'\n", filename);
        return 1;
    }
    
    // Read the entire file
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* source = malloc(length + 1);
    if (!source) {
        printf("Error: Memory allocation failed\n");
        fclose(file);
        return 1;
    }
    
    fread(source, 1, length, file);
    source[length] = '\0';
    fclose(file);
    
    printf("🚀 Compiling Goo file: %s\n", filename);
    printf("=" * 40);
    printf("\n");
    
    // Lexical analysis
    printf("📝 Lexical Analysis:\n");
    Lexer* lexer = lexer_new(source, filename);
    if (!lexer) {
        printf("❌ Failed to create lexer\n");
        free(source);
        return 1;
    }
    
    int token_count = 0;
    Token* token;
    do {
        token = lexer_next_token(lexer);
        if (token) {
            if (token_count < 10) {  // Show first 10 tokens
                print_token(token);
            }
            token_count++;
            if (token->type == TOKEN_EOF) {
                token_free(token);
                break;
            }
            token_free(token);
        }
    } while (token);
    
    printf("✅ Lexical analysis complete: %d tokens\n\n", token_count);
    
    // Reset lexer for parsing
    lexer_free(lexer);
    lexer = lexer_new(source, filename);
    
    // Parsing
    printf("🔍 Parsing:\n");
    Parser* parser = parser_new(lexer);
    if (!parser) {
        printf("❌ Failed to create parser\n");
        lexer_free(lexer);
        free(source);
        return 1;
    }
    
    ASTNode* ast = parser_parse_program(parser);
    if (!ast) {
        printf("❌ Parsing failed\n");
        parser_free(parser);
        free(source);
        return 1;
    }
    
    printf("✅ Parsing complete: AST generated\n\n");
    
    // Type checking
    printf("🔬 Type Checking:\n");
    TypeContext* type_ctx = type_context_new();
    if (!type_ctx) {
        printf("❌ Failed to create type context\n");
        ast_node_free(ast);
        parser_free(parser);
        free(source);
        return 1;
    }
    
    bool type_check_result = type_check_program(type_ctx, ast);
    if (!type_check_result) {
        printf("❌ Type checking failed\n");
        type_context_free(type_ctx);
        ast_node_free(ast);
        parser_free(parser);
        free(source);
        return 1;
    }
    
    printf("✅ Type checking complete\n\n");
    
    printf("🎉 Compilation successful!\n");
    printf("📄 File: %s\n", filename);
    printf("📊 Tokens: %d\n", token_count);
    printf("🌳 AST: Generated\n");
    printf("✅ Types: Verified\n");
    
    // Cleanup
    type_context_free(type_ctx);
    ast_node_free(ast);
    parser_free(parser);
    free(source);
    
    return 0;
}

int main(int argc, char* argv[]) {
    printf("🚀 Goo Programming Language Compiler\n");
    printf("=====================================\n\n");
    
    if (argc < 2) {
        printf("Usage: %s <file.goo> [options]\n", argv[0]);
        printf("\nOptions:\n");
        printf("  -t, --tokens    Show detailed token analysis\n");
        printf("  -h, --help      Show this help message\n");
        printf("\nExamples:\n");
        printf("  %s hello_world.goo\n", argv[0]);
        printf("  %s program.goo --tokens\n", argv[0]);
        return 1;
    }
    
    const char* filename = argv[1];
    bool show_tokens = false;
    
    // Parse command line options
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--tokens") == 0) {
            show_tokens = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Goo Compiler Help\n");
            printf("Compiles .goo source files and performs lexical analysis, parsing, and type checking.\n");
            return 0;
        }
    }
    
    // Check if file exists and has .goo extension
    if (strstr(filename, ".goo") == NULL) {
        printf("Warning: File '%s' doesn't have .goo extension\n", filename);
    }
    
    // Compile the file
    int result = compile_goo_file(filename);
    
    if (result == 0) {
        printf("\n🎊 Success! Your Goo program compiled without errors.\n");
    } else {
        printf("\n💥 Compilation failed. Please check your code for errors.\n");
    }
    
    return result;
}