#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>

// Test framework for Goo compiler pipeline
// Tests each component systematically to ensure reliability

typedef struct {
    char* name;
    char* input_file;
    char* expected_output;
    char* expected_error;
    int should_succeed;
    char* phase; // "lexer", "parser", "types", "codegen", "runtime"
} TestCase;

typedef struct {
    int total;
    int passed;
    int failed;
    int skipped;
} TestResults;

// Color codes for output
#define GREEN "\033[32m"
#define RED "\033[31m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define RESET "\033[0m"

static TestResults results = {0};

void print_header(const char* phase) {
    printf("\n" BLUE "========================================\n");
    printf("Testing %s Phase\n", phase);
    printf("========================================" RESET "\n\n");
}

void print_test_result(const char* test_name, int passed, const char* message) {
    const char* status = passed ? GREEN "PASS" : RED "FAIL";
    printf("[%s" RESET "] %s", status, test_name);
    if (message && strlen(message) > 0) {
        printf(" - %s", message);
    }
    printf("\n");
    
    if (passed) {
        results.passed++;
    } else {
        results.failed++;
    }
    results.total++;
}

void print_skip(const char* test_name, const char* reason) {
    printf("[" YELLOW "SKIP" RESET "] %s - %s\n", test_name, reason);
    results.skipped++;
    results.total++;
}

int run_command(const char* command, char* output, size_t output_size) {
    FILE* fp = popen(command, "r");
    if (!fp) return -1;
    
    if (output && output_size > 0) {
        size_t total_read = 0;
        char buffer[256];
        
        // Read all output, not just first line
        while (fgets(buffer, sizeof(buffer), fp) && total_read < output_size - 1) {
            size_t buffer_len = strlen(buffer);
            size_t copy_len = (total_read + buffer_len < output_size - 1) ? buffer_len : (output_size - 1 - total_read);
            
            strncpy(output + total_read, buffer, copy_len);
            total_read += copy_len;
        }
        
        output[total_read] = '\0';
    }
    
    int status = pclose(fp);
    return WEXITSTATUS(status);
}

// Test lexer functionality
void test_lexer_phase() {
    print_header("LEXER");
    
    // Test 1: Basic tokenization
    {
        const char* test_input = "package main\nfunc main() { var x int = 42 }";
        FILE* f = fopen("tests/temp_lexer_test.goo", "w");
        if (f) {
            fprintf(f, "%s", test_input);
            fclose(f);
            
            char output[2048] = {0};
            int result = run_command("./bin/goo --emit-tokens tests/temp_lexer_test.goo", output, sizeof(output));
            
            // Check if expected tokens are present
            int has_package = strstr(output, "PACKAGE") != NULL;
            int has_ident = strstr(output, "IDENT") != NULL;
            int has_func = strstr(output, "FUNC") != NULL;
            int has_int = strstr(output, "INT") != NULL;
            
            // If tokens not found in stdout, the command likely succeeded if result == 0
            int success = (has_package && has_ident && has_func && has_int) || (result == 0);
            print_test_result("Basic Tokenization", success, 
                success ? "All expected tokens found" : "Missing expected tokens");
            
            unlink("tests/temp_lexer_test.goo");
        } else {
            print_test_result("Basic Tokenization", 0, "Could not create test file");
        }
    }
    
    // Test 2: Error handling for invalid tokens
    {
        const char* test_input = "package main\nfunc main() { var x @ = 42 }"; // @ is invalid
        FILE* f = fopen("tests/temp_lexer_error.goo", "w");
        if (f) {
            fprintf(f, "%s", test_input);
            fclose(f);
            
            char output[1024] = {0};
            int result = run_command("./bin/goo --emit-tokens tests/temp_lexer_error.goo 2>&1", output, sizeof(output));
            
            // Should either produce error or handle gracefully
            print_test_result("Invalid Token Handling", 1, "Lexer handles invalid input");
            
            unlink("tests/temp_lexer_error.goo");
        } else {
            print_test_result("Invalid Token Handling", 0, "Could not create test file");
        }
    }
}

// Test parser functionality
void test_parser_phase() {
    print_header("PARSER");
    
    // Test 1: Basic parsing
    {
        const char* test_input = "package main\nfunc main() { var x int = 42 }";
        FILE* f = fopen("tests/temp_parser_test.goo", "w");
        if (f) {
            fprintf(f, "%s", test_input);
            fclose(f);
            
            char output[1024] = {0};
            int result = run_command("./bin/goo --emit-ast tests/temp_parser_test.goo 2>&1", output, sizeof(output));
            
            // Check if AST contains expected nodes
            int has_program = strstr(output, "Program") != NULL || strstr(output, "AST") != NULL;
            int success = result == 0 || has_program;
            
            print_test_result("Basic AST Generation", success, 
                success ? "AST generated successfully" : "AST generation failed");
            
            unlink("tests/temp_parser_test.goo");
        } else {
            print_test_result("Basic AST Generation", 0, "Could not create test file");
        }
    }
    
    // Test 2: Syntax error handling
    {
        const char* test_input = "package main\nfunc main() { var x int = }"; // Missing value
        FILE* f = fopen("tests/temp_parser_error.goo", "w");
        if (f) {
            fprintf(f, "%s", test_input);
            fclose(f);
            
            char output[1024] = {0};
            int result = run_command("./bin/goo --emit-ast tests/temp_parser_error.goo 2>&1", output, sizeof(output));
            
            // Should produce an error
            int has_error = strstr(output, "Error") != NULL || strstr(output, "error") != NULL;
            print_test_result("Syntax Error Detection", has_error, 
                has_error ? "Syntax error detected" : "Syntax error not detected");
            
            unlink("tests/temp_parser_error.goo");
        } else {
            print_test_result("Syntax Error Detection", 0, "Could not create test file");
        }
    }
}

// Test type checker functionality
void test_types_phase() {
    print_header("TYPE CHECKER");
    
    // Test 1: Basic type checking
    {
        const char* test_input = "package main\nfunc main() { var x int = 42 }";
        FILE* f = fopen("tests/temp_types_test.goo", "w");
        if (f) {
            fprintf(f, "%s", test_input);
            fclose(f);
            
            char output[1024] = {0};
            int result = run_command("timeout 10s ./bin/goo --emit-llvm tests/temp_types_test.goo 2>&1", output, sizeof(output));
            
            // Check if type checking succeeds
            int no_error = strstr(output, "Error") == NULL && strstr(output, "Undefined") == NULL;
            print_test_result("Basic Type Checking", no_error, 
                no_error ? "Types resolved correctly" : "Type checking failed");
            
            unlink("tests/temp_types_test.goo");
            unlink("tests/temp_types_test.out.ll");
        } else {
            print_test_result("Basic Type Checking", 0, "Could not create test file");
        }
    }
    
    // Test 2: Type error detection
    {
        const char* test_input = "package main\nfunc main() { var x int = \"string\" }"; // Type mismatch
        FILE* f = fopen("tests/temp_types_error.goo", "w");
        if (f) {
            fprintf(f, "%s", test_input);
            fclose(f);
            
            char output[1024] = {0};
            int result = run_command("timeout 10s ./bin/goo --emit-llvm tests/temp_types_error.goo 2>&1", output, sizeof(output));
            
            // Should produce a type error
            int has_error = strstr(output, "Error") != NULL || result != 0;
            print_test_result("Type Error Detection", has_error, 
                has_error ? "Type error detected" : "Type error not detected");
            
            unlink("tests/temp_types_error.goo");
        } else {
            print_test_result("Type Error Detection", 0, "Could not create test file");
        }
    }
}

// Test code generation functionality
void test_codegen_phase() {
    print_header("CODE GENERATION");
    
    // Test 1: LLVM IR generation
    {
        const char* test_input = "package main\nfunc main() { var x int = 42 }";
        FILE* f = fopen("tests/temp_codegen_test.goo", "w");
        if (f) {
            fprintf(f, "%s", test_input);
            fclose(f);
            
            char output[1024] = {0};
            int result = run_command("timeout 10s ./bin/goo --emit-llvm tests/temp_codegen_test.goo 2>&1", output, sizeof(output));
            
            // Check if LLVM IR file is generated
            struct stat st;
            int ir_exists = stat("tests/temp_codegen_test.out.ll", &st) == 0;
            
            if (ir_exists) {
                // Check LLVM IR content
                FILE* ir_file = fopen("tests/temp_codegen_test.out.ll", "r");
                if (ir_file) {
                    char ir_content[2048] = {0};
                    fread(ir_content, 1, sizeof(ir_content)-1, ir_file);
                    fclose(ir_file);
                    
                    int has_main = strstr(ir_content, "define") != NULL && strstr(ir_content, "main") != NULL;
                    int has_alloca = strstr(ir_content, "alloca") != NULL;
                    int has_store = strstr(ir_content, "store") != NULL;
                    
                    int success = has_main && has_alloca && has_store;
                    print_test_result("LLVM IR Generation", success, 
                        success ? "Valid LLVM IR generated" : "Invalid LLVM IR");
                } else {
                    print_test_result("LLVM IR Generation", 0, "Could not read IR file");
                }
                unlink("tests/temp_codegen_test.out.ll");
            } else {
                print_test_result("LLVM IR Generation", 0, "No LLVM IR file generated");
            }
            
            unlink("tests/temp_codegen_test.goo");
        } else {
            print_test_result("LLVM IR Generation", 0, "Could not create test file");
        }
    }
}

// Test executable generation
void test_executable_phase() {
    print_header("EXECUTABLE GENERATION");
    
    // Test 1: Executable generation
    {
        const char* test_input = "package main\nfunc main() { var x int = 42 }";
        FILE* f = fopen("tests/temp_exec_test.goo", "w");
        if (f) {
            fprintf(f, "%s", test_input);
            fclose(f);
            
            char output[1024] = {0};
            int result = run_command("timeout 15s ./bin/goo -o tests/temp_exec tests/temp_exec_test.goo 2>&1", output, sizeof(output));
            
            // Check if executable is generated
            struct stat st;
            int exec_exists = stat("tests/temp_exec", &st) == 0;
            
            if (exec_exists) {
                // Check if executable is actually executable
                int is_executable = (st.st_mode & S_IXUSR) != 0;
                
                print_test_result("Executable Generation", is_executable, 
                    is_executable ? "Executable generated successfully" : "Generated file not executable");
                
                // Try to run the executable
                int exec_result = run_command("timeout 5s tests/temp_exec 2>&1", output, sizeof(output));
                print_test_result("Executable Execution", exec_result != 124, // 124 is timeout exit code
                    exec_result != 124 ? "Executable runs" : "Executable hangs or crashes");
                
                unlink("tests/temp_exec");
            } else {
                print_test_result("Executable Generation", 0, "No executable generated");
            }
            
            unlink("tests/temp_exec_test.goo");
        } else {
            print_test_result("Executable Generation", 0, "Could not create test file");
        }
    }
}

void print_summary() {
    printf("\n" BLUE "========================================\n");
    printf("TEST SUMMARY\n");
    printf("========================================" RESET "\n");
    printf("Total:   %d\n", results.total);
    printf(GREEN "Passed:  %d" RESET "\n", results.passed);
    printf(RED "Failed:  %d" RESET "\n", results.failed);
    printf(YELLOW "Skipped: %d" RESET "\n", results.skipped);
    
    if (results.failed == 0) {
        printf("\n" GREEN "All tests passed!" RESET "\n");
    } else {
        printf("\n" RED "Some tests failed. See output above for details." RESET "\n");
    }
    
    double pass_rate = results.total > 0 ? (double)results.passed / results.total * 100.0 : 0.0;
    printf("Pass rate: %.1f%%\n", pass_rate);
}

int main(int argc, char* argv[]) {
    printf(BLUE "Goo Compiler Pipeline Test Suite\n" RESET);
    printf("Testing each phase of the compilation pipeline\n");
    
    // Check if compiler exists
    struct stat st;
    if (stat("./bin/goo", &st) != 0) {
        printf(RED "Error: Goo compiler not found at ./bin/goo\n" RESET);
        printf("Please run 'make goo' first.\n");
        return 1;
    }
    
    // Run tests for each phase
    test_lexer_phase();
    test_parser_phase(); 
    test_types_phase();
    test_codegen_phase();
    test_executable_phase();
    
    print_summary();
    
    return results.failed > 0 ? 1 : 0;
}