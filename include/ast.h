#ifndef AST_H
#define AST_H

#include "token.h"
#include <stddef.h>

// Forward declarations
typedef struct ASTNode ASTNode;
typedef struct Type Type;

// AST Node Types
typedef enum {
    // Declarations
    AST_PROGRAM,
    AST_PACKAGE_DECL,
    AST_IMPORT_SPEC,
    AST_FUNC_DECL,
    AST_VAR_DECL,
    AST_CONST_DECL,
    AST_TYPE_DECL,
    
    // Statements
    AST_BLOCK_STMT,
    AST_EXPR_STMT,
    AST_IF_STMT,
    AST_IF_LET_STMT,
    AST_FOR_STMT,
    AST_RETURN_STMT,
    AST_BREAK_STMT,
    AST_CONTINUE_STMT,
    AST_DEFER_STMT,
    AST_GO_STMT,
    AST_SELECT_STMT,
    AST_SELECT_CASE,
    AST_SWITCH_STMT,
    AST_CASE_CLAUSE,
    AST_DEFAULT_CLAUSE,
    
    // Expressions
    AST_IDENTIFIER,
    AST_LITERAL,
    AST_BINARY_EXPR,
    AST_UNARY_EXPR,
    AST_POSTFIX_EXPR,
    AST_CALL_EXPR,
    AST_INDEX_EXPR,
    AST_SELECTOR_EXPR,
    AST_SLICE_EXPR,
    AST_TYPE_ASSERT_EXPR,
    AST_PAREN_EXPR,
    
    // Types
    AST_BASIC_TYPE,
    AST_ARRAY_TYPE,
    AST_SLICE_TYPE,
    AST_MAP_TYPE,
    AST_CHAN_TYPE,
    AST_FUNC_TYPE,
    AST_INTERFACE_TYPE,
    AST_STRUCT_TYPE,
    AST_POINTER_TYPE,
    AST_REFERENCE_TYPE,
    
    // Goo Extensions
    AST_ERROR_UNION_TYPE,   // !T
    AST_NULLABLE_TYPE,      // ?T
    AST_TRY_EXPR,          // try expression
    AST_CATCH_EXPR,        // catch expression
    AST_COMPTIME_BLOCK,    // comptime { ... }
    AST_OWNERSHIP_QUAL,    // owned, borrowed, shared
    
    AST_NODE_COUNT
} ASTNodeType;

// Channel patterns for Goo extensions
typedef enum {
    CHAN_PATTERN_BASIC,
    CHAN_PATTERN_PUB,
    CHAN_PATTERN_SUB,
    CHAN_PATTERN_REQ,
    CHAN_PATTERN_REP,
    CHAN_PATTERN_PUSH,
    CHAN_PATTERN_PULL
} ChannelPattern;

// Ownership kinds for Goo extensions
typedef enum {
    OWNERSHIP_NONE,
    OWNERSHIP_OWNED,
    OWNERSHIP_BORROWED,
    OWNERSHIP_SHARED
} OwnershipKind;

// Base AST node structure
struct ASTNode {
    ASTNodeType type;
    Position pos;
    Type* node_type;        // Type information (filled during type checking)
    struct ASTNode* next;   // For linked lists of nodes
};

// Program (root node)
typedef struct {
    ASTNode base;
    char* package_name;
    struct ASTNode* imports;    // List of import specs
    struct ASTNode* decls;      // List of declarations
} ProgramNode;

// Package declaration
typedef struct {
    ASTNode base;
    char* name;
} PackageDeclNode;

// Import specification
typedef struct {
    ASTNode base;
    char* path;
    char* alias;    // Optional alias
} ImportSpecNode;

// Function declaration
typedef struct {
    ASTNode base;
    char* name;
    struct ASTNode* params;     // Parameter list
    struct ASTNode* return_type; // Return type (can be error union)
    struct ASTNode* body;       // Function body
    int is_comptime;           // Goo extension: comptime function
    int is_unsafe;             // Goo extension: unsafe function
} FuncDeclNode;

// Variable declaration
typedef struct {
    ASTNode base;
    char** names;              // Variable names
    size_t name_count;
    struct ASTNode* type;      // Type specification
    struct ASTNode* values;    // Initial values
    OwnershipKind ownership;   // Goo extension: ownership qualifier
    int is_short_decl;         // True for := declarations
} VarDeclNode;

// Constant declaration
typedef struct {
    ASTNode base;
    char** names;
    size_t name_count;
    struct ASTNode* type;
    struct ASTNode* values;
    int is_comptime;           // Goo extension: compile-time constant
} ConstDeclNode;

// Type declaration
typedef struct {
    ASTNode base;
    char* name;
    struct ASTNode* type;
} TypeDeclNode;

// Block statement
typedef struct {
    ASTNode base;
    struct ASTNode* statements; // List of statements
} BlockStmtNode;

// Expression statement
typedef struct {
    ASTNode base;
    struct ASTNode* expr;
} ExprStmtNode;

// If statement
typedef struct {
    ASTNode base;
    struct ASTNode* condition;
    struct ASTNode* then_stmt;
    struct ASTNode* else_stmt;  // Optional
} IfStmtNode;

// If-let statement for nullable unwrapping
typedef struct {
    ASTNode base;
    char* var_name;             // Variable name to bind the unwrapped value
    struct ASTNode* nullable_expr; // Expression that evaluates to nullable type
    struct ASTNode* then_stmt;  // Block to execute if not null
    struct ASTNode* else_stmt;  // Optional else block
} IfLetStmtNode;

// For statement
typedef struct {
    ASTNode base;
    struct ASTNode* init;       // Optional initialization
    struct ASTNode* condition;  // Optional condition
    struct ASTNode* post;       // Optional post statement
    struct ASTNode* body;
} ForStmtNode;

// Return statement
typedef struct {
    ASTNode base;
    struct ASTNode* values;     // Return values
} ReturnStmtNode;

// Go statement
typedef struct {
    ASTNode base;
    struct ASTNode* call;       // Function call to execute as goroutine
} GoStmtNode;

// Select statement
typedef struct {
    ASTNode base;
    struct ASTNode* cases;      // List of case clauses
} SelectStmtNode;

// Select case clause
typedef struct {
    ASTNode base;
    struct ASTNode* comm;       // Communication operation (send/recv)
    struct ASTNode* body;       // Case body
} SelectCaseNode;

// Defer statement  
typedef struct {
    ASTNode base;
    struct ASTNode* call;       // Function call to defer
} DeferStmtNode;

// Identifier
typedef struct {
    ASTNode base;
    char* name;
} IdentifierNode;

// Literal
typedef struct {
    ASTNode base;
    TokenType literal_type;
    char* value;
} LiteralNode;

// Binary expression
typedef struct {
    ASTNode base;
    struct ASTNode* left;
    TokenType operator;
    struct ASTNode* right;
} BinaryExprNode;

// Unary expression
typedef struct {
    ASTNode base;
    TokenType operator;
    struct ASTNode* operand;
} UnaryExprNode;

// Postfix expression (for operators like forced unwrap !)
typedef struct {
    ASTNode base;
    struct ASTNode* operand;
    TokenType operator;
} PostfixExprNode;

// Function call
typedef struct {
    ASTNode base;
    struct ASTNode* function;
    struct ASTNode* args;       // Argument list
} CallExprNode;

// Index expression
typedef struct {
    ASTNode base;
    struct ASTNode* expr;
    struct ASTNode* index;
} IndexExprNode;

// Selector expression (dot notation)
typedef struct {
    ASTNode base;
    struct ASTNode* expr;
    char* selector;
} SelectorExprNode;

// Basic type
typedef struct {
    ASTNode base;
    char* name;
} BasicTypeNode;

// Array type
typedef struct {
    ASTNode base;
    struct ASTNode* length;     // Array length expression
    struct ASTNode* element_type;
} ArrayTypeNode;

// Slice type
typedef struct {
    ASTNode base;
    struct ASTNode* element_type;
} SliceTypeNode;

// Map type
typedef struct {
    ASTNode base;
    struct ASTNode* key_type;
    struct ASTNode* value_type;
} MapTypeNode;

// Channel type
typedef struct {
    ASTNode base;
    struct ASTNode* element_type;
    ChannelPattern pattern;     // Goo extension: channel pattern
    char* endpoint;             // Goo extension: network endpoint
} ChanTypeNode;

// Function type
typedef struct {
    ASTNode base;
    struct ASTNode* params;
    struct ASTNode* return_type;
} FuncTypeNode;

// Pointer type
typedef struct {
    ASTNode base;
    struct ASTNode* element_type;
} PointerTypeNode;

// Reference type (for borrowing)
typedef struct {
    ASTNode base;
    struct ASTNode* element_type;
    int is_mutable;  // mut &T vs &T
} ReferenceTypeNode;

// Goo Extensions

// Error union type (!T)
typedef struct {
    ASTNode base;
    struct ASTNode* value_type;
    struct ASTNode* error_type; // Optional, defaults to standard error
} ErrorUnionTypeNode;

// Nullable type (?T)
typedef struct {
    ASTNode base;
    struct ASTNode* base_type;
} NullableTypeNode;

// Try expression
typedef struct {
    ASTNode base;
    struct ASTNode* expr;
} TryExprNode;

// Catch expression
typedef struct {
    ASTNode base;
    struct ASTNode* expr;
    char* error_var;            // Error variable name
    struct ASTNode* catch_body;
} CatchExprNode;

// Compile-time block
typedef struct {
    ASTNode base;
    struct ASTNode* body;
} ComptimeBlockNode;

// Function declarations for AST manipulation
ASTNode* ast_node_new(ASTNodeType type, Position pos);
void ast_node_free(ASTNode* node);
ASTNode* ast_node_copy(const ASTNode* node);

// Specific node constructors
ProgramNode* ast_program_new(Position pos);
PackageDeclNode* ast_package_decl_new(const char* name, Position pos);
ImportSpecNode* ast_import_spec_new(const char* path, const char* alias, Position pos);
FuncDeclNode* ast_func_decl_new(const char* name, Position pos);
VarDeclNode* ast_var_decl_new(Position pos);
IdentifierNode* ast_identifier_new(const char* name, Position pos);
LiteralNode* ast_literal_new(TokenType type, const char* value, Position pos);
BinaryExprNode* ast_binary_expr_new(ASTNode* left, TokenType op, ASTNode* right, Position pos);
UnaryExprNode* ast_unary_expr_new(TokenType op, ASTNode* operand, Position pos);
PostfixExprNode* ast_postfix_expr_new(ASTNode* operand, TokenType op, Position pos);
BlockStmtNode* ast_block_stmt_new(Position pos);
IfLetStmtNode* ast_if_let_stmt_new(const char* var_name, ASTNode* nullable_expr, ASTNode* then_stmt, ASTNode* else_stmt, Position pos);
GoStmtNode* ast_go_stmt_new(ASTNode* call, Position pos);
SelectStmtNode* ast_select_stmt_new(Position pos);
SelectCaseNode* ast_select_case_new(ASTNode* comm, ASTNode* body, Position pos);
DeferStmtNode* ast_defer_stmt_new(ASTNode* call, Position pos);

// Goo extension constructors
ErrorUnionTypeNode* ast_error_union_type_new(ASTNode* value_type, Position pos);
NullableTypeNode* ast_nullable_type_new(ASTNode* base_type, Position pos);
TryExprNode* ast_try_expr_new(ASTNode* expr, Position pos);
CatchExprNode* ast_catch_expr_new(ASTNode* expr, const char* error_var, ASTNode* catch_body, Position pos);
ComptimeBlockNode* ast_comptime_block_new(ASTNode* body, Position pos);
ChanTypeNode* ast_chan_type_new(ASTNode* element_type, ChannelPattern pattern, Position pos);
ReferenceTypeNode* ast_reference_type_new(ASTNode* element_type, int is_mutable, Position pos);

// Utility functions
void ast_add_child(ASTNode* parent, ASTNode* child);
const char* ast_node_type_string(ASTNodeType type);
void ast_print(const ASTNode* node, int indent);

#endif // AST_H