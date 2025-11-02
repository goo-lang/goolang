#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "types.h"
#include "codegen.h"

// Forward declarations for helper functions
void lexer_init(const char* source, const char* filename);
ASTNode* parse_program(void);
int codegen_generate(CodeGenerator* codegen, ASTNode* ast);
char* codegen_get_ir_string(CodeGenerator* codegen);

int main() {
    printf("Testing simple integer literal codegen...\n");

    const char* source =
        "package main\n"
        "var x int = 42\n";

    printf("Source:\n%s\n", source);

    // Parse
    lexer_init(source, "test.goo");
    ASTNode* ast = parse_program();

    if (!ast) {
        fprintf(stderr, "Parse failed\n");
        return 1;
    }

    printf("Parse succeeded!\n");

    // Type check
    TypeChecker* checker = type_checker_new();
    int type_check_result = type_check_program(checker, ast);

    if (!type_check_result || checker->error_count > 0) {
        fprintf(stderr, "Type check failed: %d errors\n", checker->error_count);
        type_checker_free(checker);
        ast_node_free(ast);
        return 1;
    }

    printf("Type check succeeded!\n");
    type_checker_free(checker);

    // Generate code
    CodeGenerator* codegen = codegen_new("test_module");
    if (!codegen) {
        fprintf(stderr, "Failed to create code generator\n");
        ast_node_free(ast);
        return 1;
    }

    printf("Code generator created!\n");

    codegen_initialize_target(codegen);
    printf("Target initialized!\n");

    int codegen_result = codegen_generate(codegen, ast);

    if (!codegen_result) {
        fprintf(stderr, "Code generation failed\n");
        codegen_free(codegen);
        ast_node_free(ast);
        return 1;
    }

    printf("Code generation succeeded!\n");

    // Get IR
    char* ir = codegen_get_ir_string(codegen);
    if (!ir) {
        fprintf(stderr, "Failed to get IR string\n");
        codegen_free(codegen);
        ast_node_free(ast);
        return 1;
    }

    printf("IR:\n%s\n", ir);

    free(ir);
    codegen_free(codegen);
    ast_node_free(ast);

    printf("SUCCESS!\n");
    return 0;
}
