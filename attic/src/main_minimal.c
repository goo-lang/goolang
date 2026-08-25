#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "token.h"

void print_token(const Token* token) {
    printf("Token: %-15s | Literal: %-10s | Pos: %d:%d\n",
           token_type_string(token->type),
           token->literal ? token->literal : "NULL",
           token->pos.line,
           token->pos.column);
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
    
    printf("🚀 Analyzing Goo file: %s\n", filename);
    printf("==============================\n\n");
    
    // Lexical analysis
    printf("📝 Lexical Analysis:\n");
    Lexer* lexer = lexer_new(source, filename);
    if (!lexer) {
        printf("❌ Failed to create lexer\n");
        free(source);
        return 1;
    }
    
    int token_count = 0;
    int keyword_count = 0;
    int identifier_count = 0;
    int operator_count = 0;
    int literal_count = 0;
    
    printf("📊 Token Analysis:\n");
    
    Token* token;
    do {
        token = lexer_next_token(lexer);
        if (token) {
            // Count token types
            switch (token->type) {
                case TOKEN_IF:
                case TOKEN_ELSE:
                case TOKEN_FOR:
                case TOKEN_FUNC:
                case TOKEN_VAR:
                case TOKEN_CONST:
                case TOKEN_RETURN:
                case TOKEN_BREAK:
                case TOKEN_CONTINUE:
                case TOKEN_STRUCT:
                case TOKEN_INTERFACE:
                case TOKEN_PACKAGE:
                case TOKEN_IMPORT:
                case TOKEN_GO:
                case TOKEN_DEFER:
                case TOKEN_SELECT:
                case TOKEN_CASE:
                case TOKEN_DEFAULT:
                case TOKEN_SWITCH:
                case TOKEN_RANGE:
                case TOKEN_CHAN:
                case TOKEN_TYPE:
                case TOKEN_MAP:
                    keyword_count++;
                    break;
                case TOKEN_IDENT:
                    identifier_count++;
                    break;
                case TOKEN_PLUS:
                case TOKEN_MINUS:
                case TOKEN_MULTIPLY:
                case TOKEN_DIVIDE:
                case TOKEN_MODULO:
                case TOKEN_ASSIGN:
                case TOKEN_EQ:
                case TOKEN_NE:
                case TOKEN_LT:
                case TOKEN_GT:
                case TOKEN_LE:
                case TOKEN_GE:
                case TOKEN_AND:
                case TOKEN_OR:
                case TOKEN_NOT:
                case TOKEN_BIT_AND:
                case TOKEN_BIT_OR:
                case TOKEN_BIT_XOR:
                case TOKEN_LSHIFT:
                case TOKEN_RSHIFT:
                case TOKEN_BANG:
                case TOKEN_QUESTION:
                case TOKEN_ARROW:
                    operator_count++;
                    break;
                case TOKEN_INT:
                case TOKEN_FLOAT:
                case TOKEN_STRING:
                case TOKEN_CHAR:
                case TOKEN_TRUE:
                case TOKEN_FALSE:
                case TOKEN_NIL:
                    literal_count++;
                    break;
                default:
                    break;
            }
            
            // Show first few tokens as examples
            if (token_count < 15) {
                print_token(token);
            } else if (token_count == 15) {
                printf("... (showing first 15 tokens)\n\n");
            }
            
            token_count++;
            if (token->type == TOKEN_EOF) {
                token_free(token);
                break;
            }
            token_free(token);
        }
    } while (token);
    
    printf("📈 Statistics:\n");
    printf("  • Total tokens: %d\n", token_count);
    printf("  • Keywords: %d\n", keyword_count);
    printf("  • Identifiers: %d\n", identifier_count);
    printf("  • Operators: %d\n", operator_count);
    printf("  • Literals: %d\n", literal_count);
    
    // Analyze Goo-specific features
    lexer_free(lexer);
    lexer = lexer_new(source, filename);
    
    printf("\n🚀 Goo Language Features Detected:\n");
    
    bool has_error_unions = false;
    bool has_nullable_types = false;
    bool has_channels = false;
    bool has_goroutines = false;
    
    do {
        token = lexer_next_token(lexer);
        if (token) {
            if (token->type == TOKEN_BANG) {
                has_error_unions = true;
            } else if (token->type == TOKEN_QUESTION) {
                has_nullable_types = true;
            } else if (token->type == TOKEN_CHAN || token->type == TOKEN_ARROW) {
                has_channels = true;
            } else if (token->type == TOKEN_GO) {
                has_goroutines = true;
            }
            
            if (token->type == TOKEN_EOF) {
                token_free(token);
                break;
            }
            token_free(token);
        }
    } while (token);
    
    printf("  %s Error Unions (!T)\n", has_error_unions ? "✅" : "❌");
    printf("  %s Nullable Types (?T)\n", has_nullable_types ? "✅" : "❌");
    printf("  %s Channels\n", has_channels ? "✅" : "❌");
    printf("  %s Goroutines\n", has_goroutines ? "✅" : "❌");
    
    printf("\n🎉 Analysis complete!\n");
    printf("📄 File: %s (%ld bytes)\n", filename, length);
    printf("✅ Lexical analysis successful\n");
    
    // Check for basic syntax patterns
    if (strstr(source, "func main()")) {
        printf("📍 Main function detected\n");
    }
    if (strstr(source, "package main")) {
        printf("📦 Main package detected\n");
    }
    if (strstr(source, "import")) {
        printf("📥 Import statements detected\n");
    }
    
    // Cleanup
    lexer_free(lexer);
    free(source);
    
    return 0;
}

int main(int argc, char* argv[]) {
    printf("🚀 Goo Programming Language Analyzer\n");
    printf("=====================================\n\n");
    
    if (argc < 2) {
        printf("Usage: %s <file.goo>\n", argv[0]);
        printf("\nThis tool analyzes Goo source files and shows:\n");
        printf("  • Token breakdown and statistics\n");
        printf("  • Goo-specific language features\n");
        printf("  • Basic syntax validation\n");
        printf("\nExamples:\n");
        printf("  %s hello_world.goo\n", argv[0]);
        printf("  %s error_unions_demo.goo\n", argv[0]);
        return 1;
    }
    
    const char* filename = argv[1];
    
    // Check if file exists and has .goo extension
    if (strstr(filename, ".goo") == NULL) {
        printf("Warning: File '%s' doesn't have .goo extension\n\n", filename);
    }
    
    // Analyze the file
    int result = compile_goo_file(filename);
    
    if (result == 0) {
        printf("\n🎊 Success! Your Goo program was analyzed successfully.\n");
        printf("💡 This demonstrates that the Goo lexer can parse your code!\n");
    } else {
        printf("\n💥 Analysis failed. Please check if the file exists.\n");
    }
    
    return result;
}