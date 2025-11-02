#ifndef COMPILER_PIPELINE_H
#define COMPILER_PIPELINE_H

#include "ast.h"
#include "types.h"

// Compilation phases
typedef enum {
    PHASE_LEXICAL,
    PHASE_PARSING, 
    PHASE_TYPE_CHECKING,
    PHASE_CODE_GENERATION,
    PHASE_COMPLETE
} CompilationPhase;

// Compilation result
typedef struct {
    CompilationPhase phase;
    int success;
    char* error_message;
    
    // Phase outputs
    ASTNode* ast;           // From parsing phase
    TypeChecker* checker;   // From type checking phase
    
    // Statistics
    int token_count;
    int ast_node_count;
    int type_errors;
} CompilationResult;

// Pipeline functions with clear separation
CompilationResult* compile_file(const char* filename);
CompilationResult* compile_source(const char* source, const char* filename);

// Individual phase functions
CompilationResult* phase_lexical_analysis(const char* source, const char* filename);
CompilationResult* phase_parsing(const char* source, const char* filename);
CompilationResult* phase_type_checking(ASTNode* ast);

// Result management
void compilation_result_free(CompilationResult* result);
void compilation_result_print_stats(const CompilationResult* result);

#endif // COMPILER_PIPELINE_H