#ifndef AST_SAFETY_H
#define AST_SAFETY_H

#include "ast.h"
#include <stdint.h>

// Magic numbers for memory corruption detection
#define AST_MAGIC_START 0xDEADBEEF
#define AST_MAGIC_END   0xCAFEBABE

// Enhanced AST node with memory safety features
typedef struct {
    uint32_t magic_start;       // Magic number at start
    ASTNode base;               // Original AST node
    uint32_t ref_count;         // Reference counting
    size_t alloc_size;          // Size of allocated memory
    const char* created_file;   // File where node was created
    int created_line;           // Line where node was created
    uint32_t magic_end;         // Magic number at end
} SafeASTNode;

// Memory safety functions
SafeASTNode* safe_ast_node_new(ASTNodeType type, Position pos, const char* file, int line);
void safe_ast_node_free(SafeASTNode* node);
int safe_ast_node_validate(const SafeASTNode* node);
void safe_ast_node_add_ref(SafeASTNode* node);
void safe_ast_node_release(SafeASTNode* node);

// Enhanced literal creation with safety checks
SafeASTNode* safe_ast_literal_new(TokenType type, const char* value, Position pos, const char* file, int line);

// Macros for easy usage
#define SAFE_AST_NEW(type, pos) safe_ast_node_new(type, pos, __FILE__, __LINE__)
#define SAFE_AST_LITERAL_NEW(tok_type, value, pos) safe_ast_literal_new(tok_type, value, pos, __FILE__, __LINE__)

// Validation and debugging
void ast_memory_report(void);
int ast_memory_check_all(void);

#endif // AST_SAFETY_H