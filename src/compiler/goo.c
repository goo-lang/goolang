// Goo Compiler Driver
// This is the main entry point for the Goo compiler

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <libgen.h>
#include <sys/stat.h>

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "types.h"
#include "codegen.h"
// #include "errors/error.h"  // TODO: Update to use new error API
#include "runtime.h"

// Compiler version
#define GOO_VERSION "0.1.0"

// Compiler options
typedef struct CompilerOptions {
    char* input_file;
    char* output_file;
    bool emit_llvm_ir;
    bool emit_ast;
    bool emit_tokens;
    bool optimize;
    int opt_level;
    bool debug_info;
    bool verbose;
    bool run_after_compile;
    char** link_libs;
    int link_lib_count;
} CompilerOptions;

// Forward declarations
static void print_usage(const char* program_name);
static void print_version(void);
static CompilerOptions* parse_arguments(int argc, char* argv[]);
static bool compile_file(const char* filename, CompilerOptions* options);
static char* read_file(const char* filename);
static bool write_file(const char* filename, const char* content);
static char* get_output_filename(const char* input_file, const char* output_file);

int main(int argc, char* argv[]) {
    // Parse command line arguments
    CompilerOptions* options = parse_arguments(argc, argv);
    if (!options) {
        return 1;
    }
    
    // Initialize error handling system
    // error_init(); // TODO: Update to use new error API
    
    // Compile the input file
    bool success = compile_file(options->input_file, options);
    
    // Cleanup
    free(options->output_file);
    if (options->link_libs) {
        for (int i = 0; i < options->link_lib_count; i++) {
            free(options->link_libs[i]);
        }
        free(options->link_libs);
    }
    free(options);
    
    // error_cleanup(); // TODO: Update to use new error API
    
    return success ? 0 : 1;
}

static void print_usage(const char* program_name) {
    printf("Usage: %s [options] <input-file>\n", program_name);
    printf("Options:\n");
    printf("  -o, --output <file>      Output file name (default: <input>.out)\n");
    printf("  -O, --optimize <level>   Optimization level (0-3, default: 0)\n");
    printf("  -g, --debug              Generate debug information\n");
    printf("  -v, --verbose            Verbose output\n");
    printf("  -r, --run                Run the program after compilation\n");
    printf("  -l, --link <lib>         Link with library\n");
    printf("  --emit-llvm              Emit LLVM IR instead of executable\n");
    printf("  --emit-ast               Emit AST (for debugging)\n");
    printf("  --emit-tokens            Emit tokens (for debugging)\n");
    printf("  -h, --help               Show this help message\n");
    printf("  --version                Show version information\n");
}

static void print_version(void) {
    printf("Goo Compiler v%s\n", GOO_VERSION);
    printf("Copyright (c) 2024 Goo Contributors\n");
#if LLVM_AVAILABLE
    printf("LLVM backend: enabled\n");
#else
    printf("LLVM backend: disabled (using interpreter mode)\n");
#endif
}

static CompilerOptions* parse_arguments(int argc, char* argv[]) {
    CompilerOptions* options = calloc(1, sizeof(CompilerOptions));
    if (!options) {
        fprintf(stderr, "Error: Out of memory\n");
        return NULL;
    }
    
    // Default values
    options->opt_level = 0;
    
    static struct option long_options[] = {
        {"output", required_argument, 0, 'o'},
        {"optimize", required_argument, 0, 'O'},
        {"debug", no_argument, 0, 'g'},
        {"verbose", no_argument, 0, 'v'},
        {"run", no_argument, 0, 'r'},
        {"link", required_argument, 0, 'l'},
        {"emit-llvm", no_argument, 0, 0},
        {"emit-ast", no_argument, 0, 0},
        {"emit-tokens", no_argument, 0, 0},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 0},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "o:O:gvrl:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 0:
                // Long option
                if (strcmp(long_options[option_index].name, "emit-llvm") == 0) {
                    options->emit_llvm_ir = true;
                } else if (strcmp(long_options[option_index].name, "emit-ast") == 0) {
                    options->emit_ast = true;
                } else if (strcmp(long_options[option_index].name, "emit-tokens") == 0) {
                    options->emit_tokens = true;
                } else if (strcmp(long_options[option_index].name, "version") == 0) {
                    print_version();
                    free(options);
                    exit(0);
                }
                break;
                
            case 'o':
                options->output_file = strdup(optarg);
                break;
                
            case 'O':
                options->opt_level = atoi(optarg);
                if (options->opt_level < 0 || options->opt_level > 3) {
                    fprintf(stderr, "Error: Invalid optimization level: %s\n", optarg);
                    free(options);
                    return NULL;
                }
                options->optimize = (options->opt_level > 0);
                break;
                
            case 'g':
                options->debug_info = true;
                break;
                
            case 'v':
                options->verbose = true;
                break;
                
            case 'r':
                options->run_after_compile = true;
                break;
                
            case 'l':
                options->link_libs = realloc(options->link_libs, 
                                           (options->link_lib_count + 1) * sizeof(char*));
                options->link_libs[options->link_lib_count++] = strdup(optarg);
                break;
                
            case 'h':
                print_usage(argv[0]);
                free(options);
                exit(0);
                
            default:
                print_usage(argv[0]);
                free(options);
                return NULL;
        }
    }
    
    // Check for input file
    if (optind >= argc) {
        fprintf(stderr, "Error: No input file specified\n");
        print_usage(argv[0]);
        free(options);
        return NULL;
    }
    
    options->input_file = argv[optind];
    
    // Generate default output filename if not specified
    if (!options->output_file) {
        options->output_file = get_output_filename(options->input_file, NULL);
    }
    
    return options;
}

static char* read_file(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Error: Cannot open file: %s\n", filename);
        return NULL;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Allocate buffer
    char* buffer = malloc(size + 1);
    if (!buffer) {
        fclose(file);
        fprintf(stderr, "Error: Out of memory reading file: %s\n", filename);
        return NULL;
    }
    
    // Read file
    size_t read_size = fread(buffer, 1, size, file);
    buffer[read_size] = '\0';
    
    fclose(file);
    return buffer;
}

static bool write_file(const char* filename, const char* content) {
    FILE* file = fopen(filename, "w");
    if (!file) {
        fprintf(stderr, "Error: Cannot create file: %s\n", filename);
        return false;
    }
    
    fprintf(file, "%s", content);
    fclose(file);
    return true;
}

static char* get_output_filename(const char* input_file, const char* output_file) {
    if (output_file) {
        return strdup(output_file);
    }
    
    // Generate default output name
    char* base = strdup(input_file);
    char* dot = strrchr(base, '.');
    // Accept both Goo's own `.goo` and real Go's `.go` so the compiler can be
    // pointed at actual Go source files (Go-compatibility). Strip either so
    // `foo.go`/`foo.goo` default to the `foo.out` executable name.
    if (dot && (strcmp(dot, ".goo") == 0 || strcmp(dot, ".go") == 0)) {
        *dot = '\0';
    }
    
    // Append .out for executable
    char* result = malloc(strlen(base) + 5);
    sprintf(result, "%s.out", base);
    
    free(base);
    return result;
}

static bool compile_file(const char* filename, CompilerOptions* options) {
    if (options->verbose) {
        printf("Compiling %s...\n", filename);
    }
    
    // Read source file
    char* source = read_file(filename);
    if (!source) {
        return false;
    }
    
    // Phase 1: Lexical Analysis
    if (options->verbose) {
        printf("Phase 1: Lexical analysis...\n");
    }
    
    Lexer* lexer = lexer_new(source, filename);
    if (!lexer) {
        free(source);
        return false;
    }
    
    // Emit tokens if requested
    if (options->emit_tokens) {
        printf("=== TOKENS ===\n");
        Token* token;
        while ((token = lexer_next_token(lexer)) && token->type != TOKEN_EOF) {
            printf("%-15s %s\n", token_type_string(token->type), 
                   token->literal ? token->literal : "");
            token_free(token);
        }
        if (token) token_free(token);
        
        // Reset lexer
        lexer_free(lexer);
        lexer = lexer_new(source, filename);
    }
    
    // Phase 2: Parsing
    if (options->verbose) {
        printf("Phase 2: Parsing...\n");
    }
    
    // Set up parser with current lexer
    extern Lexer* current_lexer;
    current_lexer = lexer;
    
    // Parse the input
    if (parse_input(source, filename) != 0) {
        fprintf(stderr, "Error: Parse failed\n");
        lexer_free(lexer);
        free(source);
        return false;
    }
    
    // Get the AST root
    extern ASTNode* ast_root;
    ASTNode* ast = ast_root;
    if (!ast) {
        fprintf(stderr, "Error: No AST generated\n");
        lexer_free(lexer);
        free(source);
        return false;
    }
    
    // Emit AST if requested
    if (options->emit_ast) {
        printf("=== AST ===\n");
        ast_print(ast, 0);
        printf("\n");
    }
    
    // Phase 3: Type Checking
    if (options->verbose) {
        printf("Phase 3: Type checking...\n");
    }
    
    TypeChecker* type_checker = type_checker_new();
    if (!type_checker) {
        ast_node_free(ast);
            lexer_free(lexer);
        free(source);
        return false;
    }
    
    if (!type_check_program(type_checker, ast)) {
        type_checker_free(type_checker);
        ast_node_free(ast);
        lexer_free(lexer);
        free(source);
        return false;
    }
    
    // Phase 4: Code Generation
    if (options->verbose) {
        printf("Phase 4: Code generation...\n");
    }
    
#if LLVM_AVAILABLE
    CodeGenerator* codegen = codegen_new(basename(options->output_file));
    if (!codegen) {
        type_checker_free(type_checker);
        ast_node_free(ast);
            lexer_free(lexer);
        free(source);
        return false;
    }
    
    // Generate code
    if (!codegen_generate_program(codegen, type_checker, ast)) {
        codegen_free(codegen);
        type_checker_free(type_checker);
        ast_node_free(ast);
        lexer_free(lexer);
        free(source);
        return false;
    }
    
    // Emit LLVM IR if requested
    if (options->emit_llvm_ir) {
        char* ir_filename = malloc(strlen(options->output_file) + 4);
        sprintf(ir_filename, "%s.ll", options->output_file);
        codegen_emit_llvm_ir(codegen, ir_filename);
        if (options->verbose) {
            printf("LLVM IR written to: %s\n", ir_filename);
        }
        free(ir_filename);
    }
    
    // Generate executable
    if (!options->emit_llvm_ir || options->emit_llvm_ir) {
        if (!codegen_emit_executable(codegen, options->output_file)) {
            codegen_free(codegen);
            type_checker_free(type_checker);
            ast_node_free(ast);
            lexer_free(lexer);
            free(source);
            return false;
        }
        
        // Make executable
        chmod(options->output_file, 0755);
        
        if (options->verbose) {
            printf("Executable written to: %s\n", options->output_file);
        }
    }
    
    codegen_free(codegen);
#else
    // Fallback: interpreter mode
    fprintf(stderr, "Warning: LLVM not available, cannot generate native code\n");
    fprintf(stderr, "Consider installing LLVM to enable code generation\n");
    
    // For now, just validate the program
    if (options->verbose) {
        printf("Program validated successfully (interpreter mode)\n");
    }
#endif
    
    // Cleanup
    type_checker_free(type_checker);
    ast_node_free(ast);
    lexer_free(lexer);
    free(source);
    
    // Run program if requested
    if (options->run_after_compile && !options->emit_llvm_ir) {
        if (options->verbose) {
            printf("\nRunning %s...\n", options->output_file);
            printf("================\n");
        }
        
        char command[1024];
        snprintf(command, sizeof(command), "./%s", options->output_file);
        int result = system(command);
        
        if (options->verbose) {
            printf("================\n");
            printf("Exit code: %d\n", WEXITSTATUS(result));
        }
    }
    
    return true;
}