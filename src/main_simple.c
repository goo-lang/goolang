#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <unistd.h>
#include "lexer.h"
#include "token.h"
#include "parser.h"
#include "ast.h"
#include "types.h"
#include "codegen.h"
#include "runtime.h"

// Resolve the directory holding the compiled runtime objects (the build dir,
// containing runtime/, errors/, and common/ subdirectories). Precedence:
//   1. $GOO_RUNTIME_DIR, if set, points directly at that directory.
//   2. Derived from the compiler's own location: <root>/bin/goo -> <root>/build,
//      so goo links correctly regardless of the caller's working directory.
//   3. Relative "build" as a last resort (in-tree invocation from repo root).
static const char* goo_runtime_dir(void) {
    static char dir[PATH_MAX];
    const char* env = getenv("GOO_RUNTIME_DIR");
    if (env && env[0]) {
        snprintf(dir, sizeof(dir), "%s", env);
        return dir;
    }
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char* slash = strrchr(exe, '/');          // strip "/goo"
        if (slash) {
            *slash = '\0';                          // <root>/bin
            slash = strrchr(exe, '/');              // strip "/bin"
            if (slash) {
                *slash = '\0';                      // <root>
                snprintf(dir, sizeof(dir), "%s/build", exe);
                return dir;
            }
        }
    }
    snprintf(dir, sizeof(dir), "build");
    return dir;
}

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
    printf("========================================\n");
    
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
    lexer_free(lexer);  // Free the lexer used for token counting
    
    int parse_result = parse_input(source, filename);
    if (parse_result != 0) {
        printf("❌ Parsing failed\n");
        free(source);
        return 1;
    }
    
    // Get the AST from the global parser result
    extern ASTNode* ast_root;
    if (!ast_root) {
        printf("❌ No AST generated\n");
        free(source);
        return 1;
    }
    
    printf("✅ Parsing complete: AST generated\n\n");
    
    // Type checking
    printf("🔬 Type Checking:\n");
    TypeChecker* type_checker = type_checker_new();
    if (!type_checker) {
        printf("❌ Failed to create type checker\n");
        ast_node_free(ast_root);
        free(source);
        return 1;
    }
    
    int type_check_result = type_check_program(type_checker, ast_root);
    if (!type_check_result) {
        printf("❌ Type checking failed\n");
        type_checker_free(type_checker);
        ast_node_free(ast_root);
        free(source);
        return 1;
    }
    
    printf("✅ Type checking complete\n\n");
    
    // Code generation
    printf("🎯 Code Generation:\n");
    CodeGenerator* codegen = codegen_new("main_module");
    if (!codegen) {
        printf("❌ Failed to create code generator\n");
        type_checker_free(type_checker);
        ast_node_free(ast_root);
        free(source);
        return 1;
    }
    
    // Set target for native compilation
    if (!codegen_set_target(codegen, NULL, NULL, NULL)) {
        printf("⚠️ Warning: Could not set target (LLVM may not be available)\n");
    }
    
    // Generate LLVM IR
    if (!codegen_generate_program(codegen, type_checker, ast_root)) {
        printf("❌ Code generation failed\n");
        codegen_free(codegen);
        type_checker_free(type_checker);
        ast_node_free(ast_root);
        free(source);
        return 1;
    }
    
    printf("✅ LLVM IR generation complete\n");
    
    // Verify the generated module
    if (!codegen_verify_module(codegen)) {
        printf("❌ LLVM module verification failed\n");
        codegen_free(codegen);
        type_checker_free(type_checker);
        ast_node_free(ast_root);
        free(source);
        return 1;
    }
    
    printf("✅ LLVM module verification passed\n");
    
    // Generate output files
    char ir_filename[256];
    char obj_filename[256];
    char exe_filename[256];
    
    // Create base filename without extension
    const char* base_name = filename;
    const char* dot = strrchr(filename, '.');
    size_t base_len = dot ? (size_t)(dot - filename) : strlen(filename);
    
    snprintf(ir_filename, sizeof(ir_filename), "%.*s.ll", (int)base_len, base_name);
    snprintf(obj_filename, sizeof(obj_filename), "%.*s.o", (int)base_len, base_name);
    snprintf(exe_filename, sizeof(exe_filename), "%.*s", (int)base_len, base_name);
    
    // Emit LLVM IR
    if (codegen_emit_llvm_ir(codegen, ir_filename)) {
        printf("✅ LLVM IR written to: %s\n", ir_filename);
    } else {
        printf("⚠️ Warning: Could not write LLVM IR file\n");
        codegen_free(codegen);
        type_checker_free(type_checker);
        ast_node_free(ast_root);
        free(source);
        return 1;
    }
    
    // Generate object file using llc
    printf("\n🔧 Object File Generation:\n");
    char llc_command[1024];
    snprintf(llc_command, sizeof(llc_command), "llc -filetype=obj %s -o %s", ir_filename, obj_filename);
    
    int llc_result = system(llc_command);
    if (llc_result != 0) {
        printf("❌ Failed to generate object file using llc\n");
        codegen_free(codegen);
        type_checker_free(type_checker);
        ast_node_free(ast_root);
        free(source);
        return 1;
    }
    printf("✅ Object file generated: %s\n", obj_filename);
    
    // Link to create executable using clang with runtime library
    printf("\n🔗 Executable Linking:\n");
    const char* rt = goo_runtime_dir();
    // Fail early with a clear message if the runtime is missing, instead of
    // letting clang choke on an unexpanded glob.
    char rt_probe[PATH_MAX];
    snprintf(rt_probe, sizeof(rt_probe), "%s/runtime", rt);
    if (access(rt_probe, F_OK) != 0) {
        printf("❌ Runtime objects not found in '%s'.\n"
               "   Build the runtime with `make`, or set GOO_RUNTIME_DIR to the build directory.\n", rt);
        codegen_free(codegen);
        type_checker_free(type_checker);
        ast_node_free(ast_root);
        free(source);
        return 1;
    }
    // Link against the full runtime: runtime/ provides the core runtime, but
    // error_severity_to_string and friends live in errors/, and shared helpers
    // in common/. Omitting either left every program unlinkable.
    char link_command[4 * PATH_MAX];
    snprintf(link_command, sizeof(link_command),
             "clang %s %s/runtime/*.o %s/errors/*.o %s/common/*.o -o %s -lm -lpthread",
             obj_filename, rt, rt, rt, exe_filename);
    
    int link_result = system(link_command);
    if (link_result != 0) {
        printf("❌ Failed to link executable\n");
        codegen_free(codegen);
        type_checker_free(type_checker);
        ast_node_free(ast_root);
        free(source);
        return 1;
    }
    printf("✅ Executable generated: %s\n", exe_filename);
    
    printf("\n🎉 Full compilation successful!\n");
    printf("📄 Source: %s\n", filename);
    printf("📊 Tokens: %d\n", token_count);
    printf("🌳 AST: Generated\n");
    printf("✅ Types: Verified\n");
    printf("🎯 LLVM IR: %s\n", ir_filename);
    printf("🔧 Object File: %s\n", obj_filename);
    printf("🚀 Executable: %s\n", exe_filename);
    
    // Cleanup
    codegen_free(codegen);
    type_checker_free(type_checker);
    ast_node_free(ast_root);
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
    
    (void)show_tokens; // TODO: implement token display mode

    // Compile the file
    int result = compile_goo_file(filename);
    
    if (result == 0) {
        printf("\n🎊 Success! Your Goo program compiled without errors.\n");
    } else {
        printf("\n💥 Compilation failed. Please check your code for errors.\n");
    }
    
    return result;
}