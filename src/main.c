#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "token.h"
#include "parser.h"
#include "ast.h"
#include "types.h"
#include "codegen.h"

// Test function declarations
int test_type_system(void);
int test_codegen(void);
int test_error_union_codegen(void);
void test_wasm_codegen(void);

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

void test_parser(const char* input, const char* test_name) {
    printf("\n=== Parser Test: %s ===\n", test_name);
    printf("Input: %s\n", input);
    printf("AST:\n");
    
    int result = parse_input(input, "test.goo");
    if (result == 0) {
        printf("Parse successful!\n");
        if (ast_root) {
            ast_print(ast_root, 0);
            ast_node_free(ast_root);
            ast_root = NULL;
        } else {
            printf("No AST generated\n");
        }
    } else {
        printf("Parse failed with code %d\n", result);
    }
    printf("\n");
}

int main(int argc, char* argv[]) {
    if (argc > 1) {
        // Parse file with new parser
        printf("Parsing file: %s\n", argv[1]);
        int result = parse_file(argv[1]);
        if (result == 0) {
            printf("Parse successful!\n");
            if (ast_root) {
                printf("Generated AST:\n");
                ast_print(ast_root, 0);
                
                // Type checking
                printf("\nStarting type checking...\n");
                TypeChecker* checker = type_checker_new();
                if (checker && type_check_program(checker, ast_root) == 0) {
                    printf("Type checking successful!\n");
                    
                    // Code generation
                    printf("\nStarting code generation...\n");
                    CodeGenerator* codegen = codegen_new("test_module");
                    if (codegen) {
                        if (codegen_generate_program(codegen, checker, ast_root) == 0) {
                            printf("Code generation successful!\n");
                            
                            // Output LLVM IR
                            if (codegen_emit_llvm_ir(codegen, "output.ll") == 0) {
                                printf("LLVM IR written to output.ll\n");
                            }
                        } else {
                            printf("Code generation failed\n");
                        }
                        codegen_free(codegen);
                    } else {
                        printf("Failed to create code generator (LLVM not available?)\n");
                    }
                } else {
                    printf("Type checking failed\n");
                }
                
                if (checker) {
                    type_checker_free(checker);
                }
                
                ast_node_free(ast_root);
                ast_root = NULL;
            }
        } else {
            printf("Parse failed with code %d\n", result);
        }
    } else {
        // Run built-in tests
        printf("Goo Language Compiler Test Suite\n");
        printf("=================================\n");
        
        // Test type system
        printf("\n--- Type System Tests ---\n");
        test_type_system();
        
        // Test code generator
        printf("\n--- Code Generator Tests ---\n");
        test_codegen();
        
        // Test error union code generation
        printf("\n--- Error Union Code Generation Tests ---\n");
        test_error_union_codegen();
        
        // Test WebAssembly code generation
        printf("\n--- WebAssembly Code Generation Tests ---\n");
        test_wasm_codegen();
        
        printf("\n--- Parser Tests ---\n");
        // Test basic Go syntax parsing
        test_parser("package main", "Package declaration");
        test_parser("package main\nfunc main() { }", "Simple function");
        test_parser("package main\nvar x int", "Variable declaration");
        test_parser("package main\nconst N = 42", "Constant declaration");
        
        // Test expressions
        test_parser("package main\nfunc main() { x + y }", "Binary expression");
        test_parser("package main\nfunc main() { !x }", "Unary expression");
        
        // Test Goo extensions
        test_parser("package main\nfunc readFile() ![]byte { }", "Error union return");
        test_parser("package main\nvar name: ?string", "Nullable type");
        test_parser("package main\ncomptime { const N = 10 }", "Compile-time block");
        
        printf("All tests completed!\n");
    }
    
    return 0;
}