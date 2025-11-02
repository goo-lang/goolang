#include "compiler_pipeline.h"
#include "lexer.h"
#include "parser.h"
#include "ast_safety.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CompilationResult* create_result(CompilationPhase phase) {
    CompilationResult* result = malloc(sizeof(CompilationResult));
    if (!result) return NULL;
    
    result->phase = phase;
    result->success = 0;
    result->error_message = NULL;
    result->ast = NULL;
    result->checker = NULL;
    result->token_count = 0;
    result->ast_node_count = 0;
    result->type_errors = 0;
    
    return result;
}

static void set_error(CompilationResult* result, const char* message) {
    result->success = 0;
    if (result->error_message) free(result->error_message);
    result->error_message = strdup(message);
    printf("❌ Compilation Error: %s\n", message);
}

CompilationResult* phase_lexical_analysis(const char* source, const char* filename) {
    printf("📝 Phase 1: Lexical Analysis\n");
    CompilationResult* result = create_result(PHASE_LEXICAL);
    if (!result) return NULL;
    
    Lexer* lexer = lexer_new(source, filename);
    if (!lexer) {
        set_error(result, "Failed to create lexer");
        return result;
    }
    
    // Count tokens and validate
    int token_count = 0;
    Token* token;
    do {
        token = lexer_next_token(lexer);
        if (token) {
            token_count++;
            if (token->type == TOKEN_UNKNOWN) {
                char error_msg[256];
                snprintf(error_msg, sizeof(error_msg), 
                        "Unknown token at line %d, column %d", 
                        token->pos.line, token->pos.column);
                set_error(result, error_msg);
                token_free(token);
                lexer_free(lexer);
                return result;
            }
            if (token->type == TOKEN_EOF) {
                token_free(token);
                break;
            }
            token_free(token);
        }
    } while (token);
    
    lexer_free(lexer);
    
    result->success = 1;
    result->token_count = token_count;
    printf("✅ Lexical analysis complete: %d tokens\n", token_count);
    
    return result;
}

CompilationResult* phase_parsing(const char* source, const char* filename) {
    printf("🔍 Phase 2: Parsing\n");
    CompilationResult* result = create_result(PHASE_PARSING);
    if (!result) return NULL;
    
    // Clear any existing global AST
    extern ASTNode* ast_root;
    if (ast_root) {
        ast_node_free(ast_root);
        ast_root = NULL;
    }
    
    // Parse the input
    int parse_result = parse_input(source, filename);
    if (parse_result != 0) {
        set_error(result, "Parsing failed - syntax error");
        return result;
    }
    
    // Check if AST was generated
    if (!ast_root) {
        set_error(result, "No AST generated during parsing");
        return result;
    }
    
    // Transfer ownership of AST to result
    result->ast = ast_root;
    ast_root = NULL;  // Clear global to prevent double-free
    
    result->success = 1;
    printf("✅ Parsing complete: AST generated (type: %d)\n", result->ast->type);
    
    return result;
}

CompilationResult* phase_type_checking(ASTNode* ast) {
    printf("🔬 Phase 3: Type Checking\n");
    CompilationResult* result = create_result(PHASE_TYPE_CHECKING);
    if (!result) return NULL;
    
    if (!ast) {
        set_error(result, "No AST provided for type checking");
        return result;
    }
    
    // Create type checker
    TypeChecker* checker = type_checker_new();
    if (!checker) {
        set_error(result, "Failed to create type checker");
        return result;
    }
    
    // Run type checking
    printf("Running type analysis...\n");
    int type_result = type_check_program(checker, ast);
    
    if (!type_result) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), 
                "Type checking failed with %d errors", checker->error_count);
        set_error(result, error_msg);
        result->type_errors = checker->error_count;
        type_checker_free(checker);
        return result;
    }
    
    // Success
    result->success = 1;
    result->checker = checker;
    result->type_errors = checker->error_count;
    printf("✅ Type checking complete: %d errors, %d warnings\n", 
           checker->error_count, checker->warning_count);
    
    return result;
}

CompilationResult* compile_source(const char* source, const char* filename) {
    printf("🚀 Starting compilation pipeline for: %s\n", filename);
    printf("================================================\n");
    
    // Phase 1: Lexical Analysis
    CompilationResult* lex_result = phase_lexical_analysis(source, filename);
    if (!lex_result || !lex_result->success) {
        return lex_result;
    }
    
    // Phase 2: Parsing
    CompilationResult* parse_result = phase_parsing(source, filename);
    if (!parse_result || !parse_result->success) {
        compilation_result_free(lex_result);
        return parse_result;
    }
    
    // Transfer lexical stats to parse result
    parse_result->token_count = lex_result->token_count;
    compilation_result_free(lex_result);
    
    // Phase 3: Type Checking  
    CompilationResult* type_result = phase_type_checking(parse_result->ast);
    if (!type_result || !type_result->success) {
        compilation_result_free(parse_result);
        return type_result;
    }
    
    // Combine results into final result
    CompilationResult* final_result = create_result(PHASE_COMPLETE);
    if (final_result) {
        final_result->success = 1;
        final_result->ast = parse_result->ast;
        final_result->checker = type_result->checker;
        final_result->token_count = parse_result->token_count;
        final_result->type_errors = type_result->type_errors;
        
        // Clear transferred ownership
        parse_result->ast = NULL;
        type_result->checker = NULL;
    }
    
    compilation_result_free(parse_result);
    compilation_result_free(type_result);
    
    printf("\n🎉 Compilation pipeline complete!\n");
    return final_result;
}

CompilationResult* compile_file(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        CompilationResult* result = create_result(PHASE_LEXICAL);
        if (result) {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "Could not open file: %s", filename);
            set_error(result, error_msg);
        }
        return result;
    }
    
    // Read file content
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* source = malloc(length + 1);
    if (!source) {
        fclose(file);
        CompilationResult* result = create_result(PHASE_LEXICAL);
        if (result) {
            set_error(result, "Memory allocation failed");
        }
        return result;
    }
    
    fread(source, 1, length, file);
    source[length] = '\0';
    fclose(file);
    
    CompilationResult* result = compile_source(source, filename);
    free(source);
    
    return result;
}

void compilation_result_print_stats(const CompilationResult* result) {
    if (!result) return;
    
    printf("\n📊 Compilation Statistics\n");
    printf("========================\n");
    printf("Phase: %s\n", 
           result->phase == PHASE_COMPLETE ? "Complete" :
           result->phase == PHASE_TYPE_CHECKING ? "Type Checking" :
           result->phase == PHASE_PARSING ? "Parsing" :
           result->phase == PHASE_LEXICAL ? "Lexical" : "Unknown");
    printf("Success: %s\n", result->success ? "Yes" : "No");
    printf("Tokens: %d\n", result->token_count);
    printf("Type Errors: %d\n", result->type_errors);
    
    if (result->error_message) {
        printf("Error: %s\n", result->error_message);
    }
}

void compilation_result_free(CompilationResult* result) {
    if (!result) return;
    
    if (result->error_message) {
        free(result->error_message);
    }
    
    if (result->ast) {
        ast_node_free(result->ast);
    }
    
    if (result->checker) {
        type_checker_free(result->checker);
    }
    
    free(result);
}