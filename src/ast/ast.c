#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// AST node type to string mapping
static const char* ast_node_type_strings[] = {
    [AST_PROGRAM] = "Program",
    [AST_PACKAGE_DECL] = "PackageDecl",
    [AST_IMPORT_SPEC] = "ImportSpec",
    [AST_FUNC_DECL] = "FuncDecl",
    [AST_VAR_DECL] = "VarDecl",
    [AST_CONST_DECL] = "ConstDecl",
    [AST_TYPE_DECL] = "TypeDecl",
    
    [AST_BLOCK_STMT] = "BlockStmt",
    [AST_EXPR_STMT] = "ExprStmt",
    [AST_IF_STMT] = "IfStmt",
    [AST_IF_LET_STMT] = "IfLetStmt",
    [AST_FOR_STMT] = "ForStmt",
    [AST_RETURN_STMT] = "ReturnStmt",
    [AST_BREAK_STMT] = "BreakStmt",
    [AST_CONTINUE_STMT] = "ContinueStmt",
    [AST_DEFER_STMT] = "DeferStmt",
    [AST_GO_STMT] = "GoStmt",
    [AST_SELECT_STMT] = "SelectStmt",
    [AST_SELECT_CASE] = "SelectCase",
    [AST_SWITCH_STMT] = "SwitchStmt",
    [AST_CASE_CLAUSE] = "CaseClause",
    [AST_DEFAULT_CLAUSE] = "DefaultClause",
    
    [AST_IDENTIFIER] = "Identifier",
    [AST_LITERAL] = "Literal",
    [AST_BINARY_EXPR] = "BinaryExpr",
    [AST_UNARY_EXPR] = "UnaryExpr",
    [AST_POSTFIX_EXPR] = "PostfixExpr",
    [AST_CALL_EXPR] = "CallExpr",
    [AST_INDEX_EXPR] = "IndexExpr",
    [AST_SELECTOR_EXPR] = "SelectorExpr",
    [AST_SLICE_EXPR] = "SliceExpr",
    [AST_TYPE_ASSERT_EXPR] = "TypeAssertExpr",
    [AST_PAREN_EXPR] = "ParenExpr",
    
    [AST_BASIC_TYPE] = "BasicType",
    [AST_ARRAY_TYPE] = "ArrayType",
    [AST_SLICE_TYPE] = "SliceType",
    [AST_MAP_TYPE] = "MapType",
    [AST_CHAN_TYPE] = "ChanType",
    [AST_FUNC_TYPE] = "FuncType",
    [AST_INTERFACE_TYPE] = "InterfaceType",
    [AST_STRUCT_TYPE] = "StructType",
    [AST_POINTER_TYPE] = "PointerType",
    [AST_REFERENCE_TYPE] = "ReferenceType",
    
    [AST_ERROR_UNION_TYPE] = "ErrorUnionType",
    [AST_NULLABLE_TYPE] = "NullableType",
    [AST_TRY_EXPR] = "TryExpr",
    [AST_CATCH_EXPR] = "CatchExpr",
    [AST_COMPTIME_BLOCK] = "ComptimeBlock",
    [AST_OWNERSHIP_QUAL] = "OwnershipQual",
};

// Helper function to duplicate strings
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

// Base AST node creation
ASTNode* ast_node_new(ASTNodeType type, Position pos) {
    ASTNode* node = malloc(sizeof(ASTNode));
    if (!node) return NULL;
    
    node->type = type;
    node->pos = pos;
    node->node_type = NULL;
    node->next = NULL;
    
    return node;
}

// Generic node destruction (needs to be extended for specific types)
void ast_node_free(ASTNode* node) {
    if (!node) return;
    
    // Free type-specific data
    switch (node->type) {
        case AST_PROGRAM: {
            ProgramNode* prog = (ProgramNode*)node;
            free(prog->package_name);
            ast_node_free(prog->imports);
            ast_node_free(prog->decls);
            break;
        }
        case AST_PACKAGE_DECL: {
            PackageDeclNode* pkg = (PackageDeclNode*)node;
            free(pkg->name);
            break;
        }
        case AST_IMPORT_SPEC: {
            ImportSpecNode* imp = (ImportSpecNode*)node;
            free(imp->path);
            free(imp->alias);
            break;
        }
        case AST_FUNC_DECL: {
            FuncDeclNode* func = (FuncDeclNode*)node;
            free(func->name);
            ast_node_free(func->params);
            ast_node_free(func->return_type);
            ast_node_free(func->body);
            break;
        }
        case AST_VAR_DECL: {
            VarDeclNode* var = (VarDeclNode*)node;
            for (size_t i = 0; i < var->name_count; i++) {
                free(var->names[i]);
            }
            free(var->names);
            ast_node_free(var->type);
            ast_node_free(var->values);
            break;
        }
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)node;
            free(ident->name);
            break;
        }
        case AST_LITERAL: {
            LiteralNode* lit = (LiteralNode*)node;
            free(lit->value);
            break;
        }
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)node;
            ast_node_free(binary->left);
            ast_node_free(binary->right);
            break;
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)node;
            ast_node_free(unary->operand);
            break;
        }
        case AST_POSTFIX_EXPR: {
            PostfixExprNode* postfix = (PostfixExprNode*)node;
            ast_node_free(postfix->operand);
            break;
        }
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)node;
            ast_node_free(block->statements);
            break;
        }
        case AST_IF_LET_STMT: {
            IfLetStmtNode* if_let = (IfLetStmtNode*)node;
            free(if_let->var_name);
            ast_node_free(if_let->nullable_expr);
            ast_node_free(if_let->then_stmt);
            ast_node_free(if_let->else_stmt);
            break;
        }
        case AST_ERROR_UNION_TYPE: {
            ErrorUnionTypeNode* error_union = (ErrorUnionTypeNode*)node;
            ast_node_free(error_union->value_type);
            ast_node_free(error_union->error_type);
            break;
        }
        case AST_NULLABLE_TYPE: {
            NullableTypeNode* nullable = (NullableTypeNode*)node;
            ast_node_free(nullable->base_type);
            break;
        }
        case AST_TRY_EXPR: {
            TryExprNode* try_expr = (TryExprNode*)node;
            ast_node_free(try_expr->expr);
            break;
        }
        case AST_CATCH_EXPR: {
            CatchExprNode* catch_expr = (CatchExprNode*)node;
            ast_node_free(catch_expr->expr);
            free(catch_expr->error_var);
            ast_node_free(catch_expr->catch_body);
            break;
        }
        case AST_COMPTIME_BLOCK: {
            ComptimeBlockNode* comptime_block = (ComptimeBlockNode*)node;
            ast_node_free(comptime_block->body);
            break;
        }
        case AST_CHAN_TYPE: {
            ChanTypeNode* chan = (ChanTypeNode*)node;
            ast_node_free(chan->element_type);
            free(chan->endpoint);
            break;
        }
        case AST_REFERENCE_TYPE: {
            ReferenceTypeNode* ref = (ReferenceTypeNode*)node;
            ast_node_free(ref->element_type);
            break;
        }
        // Add more cases as needed
        default:
            break;
    }
    
    // Free the next node in the list
    if (node->next) {
        ast_node_free(node->next);
    }
    
    free(node);
}

// Specific node constructors

ProgramNode* ast_program_new(Position pos) {
    ProgramNode* node = (ProgramNode*)malloc(sizeof(ProgramNode));
    if (!node) return NULL;
    
    node->base.type = AST_PROGRAM;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->package_name = NULL;
    node->imports = NULL;
    node->decls = NULL;
    
    return node;
}

PackageDeclNode* ast_package_decl_new(const char* name, Position pos) {
    PackageDeclNode* node = (PackageDeclNode*)malloc(sizeof(PackageDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_PACKAGE_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    
    return node;
}

ImportSpecNode* ast_import_spec_new(const char* path, const char* alias, Position pos) {
    ImportSpecNode* node = (ImportSpecNode*)malloc(sizeof(ImportSpecNode));
    if (!node) return NULL;
    
    node->base.type = AST_IMPORT_SPEC;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->path = str_dup(path);
    node->alias = str_dup(alias);
    
    return node;
}

FuncDeclNode* ast_func_decl_new(const char* name, Position pos) {
    FuncDeclNode* node = (FuncDeclNode*)malloc(sizeof(FuncDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_FUNC_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->params = NULL;
    node->return_type = NULL;
    node->body = NULL;
    node->is_comptime = 0;
    node->is_unsafe = 0;
    
    return node;
}

VarDeclNode* ast_var_decl_new(Position pos) {
    VarDeclNode* node = (VarDeclNode*)malloc(sizeof(VarDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_VAR_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->names = NULL;
    node->name_count = 0;
    node->type = NULL;
    node->values = NULL;
    node->ownership = OWNERSHIP_NONE;
    node->is_short_decl = 0;
    
    return node;
}

IdentifierNode* ast_identifier_new(const char* name, Position pos) {
    IdentifierNode* node = (IdentifierNode*)malloc(sizeof(IdentifierNode));
    if (!node) return NULL;
    
    node->base.type = AST_IDENTIFIER;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    
    return node;
}

LiteralNode* ast_literal_new(TokenType type, const char* value, Position pos) {
    LiteralNode* node = (LiteralNode*)malloc(sizeof(LiteralNode));
    if (!node) return NULL;
    
    node->base.type = AST_LITERAL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->literal_type = type;
    node->value = str_dup(value);
    
    return node;
}

BinaryExprNode* ast_binary_expr_new(ASTNode* left, TokenType op, ASTNode* right, Position pos) {
    BinaryExprNode* node = (BinaryExprNode*)malloc(sizeof(BinaryExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_BINARY_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->left = left;
    node->operator = op;
    node->right = right;
    
    return node;
}

UnaryExprNode* ast_unary_expr_new(TokenType op, ASTNode* operand, Position pos) {
    UnaryExprNode* node = (UnaryExprNode*)malloc(sizeof(UnaryExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_UNARY_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->operator = op;
    node->operand = operand;
    
    return node;
}

PostfixExprNode* ast_postfix_expr_new(ASTNode* operand, TokenType op, Position pos) {
    PostfixExprNode* node = (PostfixExprNode*)malloc(sizeof(PostfixExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_POSTFIX_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->operand = operand;
    node->operator = op;
    
    return node;
}

BlockStmtNode* ast_block_stmt_new(Position pos) {
    BlockStmtNode* node = (BlockStmtNode*)malloc(sizeof(BlockStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_BLOCK_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->statements = NULL;
    
    return node;
}

IfLetStmtNode* ast_if_let_stmt_new(const char* var_name, ASTNode* nullable_expr, ASTNode* then_stmt, ASTNode* else_stmt, Position pos) {
    IfLetStmtNode* node = (IfLetStmtNode*)malloc(sizeof(IfLetStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_IF_LET_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    
    // Copy variable name
    if (var_name) {
        node->var_name = malloc(strlen(var_name) + 1);
        if (node->var_name) {
            strcpy(node->var_name, var_name);
        }
    } else {
        node->var_name = NULL;
    }
    
    node->nullable_expr = nullable_expr;
    node->then_stmt = then_stmt;
    node->else_stmt = else_stmt;
    
    return node;
}

GoStmtNode* ast_go_stmt_new(ASTNode* call, Position pos) {
    GoStmtNode* node = (GoStmtNode*)malloc(sizeof(GoStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_GO_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->call = call;
    
    return node;
}

SelectStmtNode* ast_select_stmt_new(Position pos) {
    SelectStmtNode* node = (SelectStmtNode*)malloc(sizeof(SelectStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_SELECT_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->cases = NULL;
    
    return node;
}

SelectCaseNode* ast_select_case_new(ASTNode* comm, ASTNode* body, Position pos) {
    SelectCaseNode* node = (SelectCaseNode*)malloc(sizeof(SelectCaseNode));
    if (!node) return NULL;
    
    node->base.type = AST_SELECT_CASE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->comm = comm;
    node->body = body;
    
    return node;
}

DeferStmtNode* ast_defer_stmt_new(ASTNode* call, Position pos) {
    DeferStmtNode* node = (DeferStmtNode*)malloc(sizeof(DeferStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_DEFER_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->call = call;
    
    return node;
}

// Goo extension constructors

ErrorUnionTypeNode* ast_error_union_type_new(ASTNode* value_type, Position pos) {
    ErrorUnionTypeNode* node = (ErrorUnionTypeNode*)malloc(sizeof(ErrorUnionTypeNode));
    if (!node) return NULL;
    
    node->base.type = AST_ERROR_UNION_TYPE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->value_type = value_type;
    node->error_type = NULL; // Default to standard error type
    
    return node;
}

NullableTypeNode* ast_nullable_type_new(ASTNode* base_type, Position pos) {
    NullableTypeNode* node = (NullableTypeNode*)malloc(sizeof(NullableTypeNode));
    if (!node) return NULL;
    
    node->base.type = AST_NULLABLE_TYPE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->base_type = base_type;
    
    return node;
}

TryExprNode* ast_try_expr_new(ASTNode* expr, Position pos) {
    TryExprNode* node = (TryExprNode*)malloc(sizeof(TryExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_TRY_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->expr = expr;
    
    return node;
}

CatchExprNode* ast_catch_expr_new(ASTNode* expr, const char* error_var, ASTNode* catch_body, Position pos) {
    CatchExprNode* node = (CatchExprNode*)malloc(sizeof(CatchExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_CATCH_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->expr = expr;
    node->error_var = str_dup(error_var);
    node->catch_body = catch_body;
    
    return node;
}

ComptimeBlockNode* ast_comptime_block_new(ASTNode* body, Position pos) {
    ComptimeBlockNode* node = (ComptimeBlockNode*)malloc(sizeof(ComptimeBlockNode));
    if (!node) return NULL;
    
    node->base.type = AST_COMPTIME_BLOCK;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->body = body;
    
    return node;
}

ChanTypeNode* ast_chan_type_new(ASTNode* element_type, ChannelPattern pattern, Position pos) {
    ChanTypeNode* node = (ChanTypeNode*)malloc(sizeof(ChanTypeNode));
    if (!node) return NULL;
    
    node->base.type = AST_CHAN_TYPE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->element_type = element_type;
    node->pattern = pattern;
    node->endpoint = NULL;
    
    return node;
}

ReferenceTypeNode* ast_reference_type_new(ASTNode* element_type, int is_mutable, Position pos) {
    ReferenceTypeNode* node = (ReferenceTypeNode*)malloc(sizeof(ReferenceTypeNode));
    if (!node) return NULL;
    
    node->base.type = AST_REFERENCE_TYPE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->element_type = element_type;
    node->is_mutable = is_mutable;
    
    return node;
}

// Utility functions

void ast_add_child(ASTNode* parent, ASTNode* child) {
    if (!parent || !child) return;
    
    if (!parent->next) {
        parent->next = child;
    } else {
        ASTNode* current = parent->next;
        while (current->next) {
            current = current->next;
        }
        current->next = child;
    }
}

const char* ast_node_type_string(ASTNodeType type) {
    if (type >= 0 && type < AST_NODE_COUNT) {
        return ast_node_type_strings[type];
    }
    return "Unknown";
}

void ast_print(const ASTNode* node, int indent) {
    if (!node) return;
    
    // Print indentation
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
    
    printf("%s", ast_node_type_string(node->type));
    
    // Print type-specific information
    switch (node->type) {
        case AST_IDENTIFIER: {
            const IdentifierNode* ident = (const IdentifierNode*)node;
            printf(" (%s)", ident->name ? ident->name : "NULL");
            break;
        }
        case AST_LITERAL: {
            const LiteralNode* lit = (const LiteralNode*)node;
            printf(" (%s: %s)", token_type_string(lit->literal_type), 
                   lit->value ? lit->value : "NULL");
            break;
        }
        case AST_BINARY_EXPR: {
            const BinaryExprNode* binary = (const BinaryExprNode*)node;
            printf(" (%s)", token_type_string(binary->operator));
            break;
        }
        case AST_UNARY_EXPR: {
            const UnaryExprNode* unary = (const UnaryExprNode*)node;
            printf(" (%s)", token_type_string(unary->operator));
            break;
        }
        default:
            break;
    }
    
    printf(" [%d:%d]\n", node->pos.line, node->pos.column);
    
    // Print children (this is a simplified version)
    switch (node->type) {
        case AST_BINARY_EXPR: {
            const BinaryExprNode* binary = (const BinaryExprNode*)node;
            ast_print(binary->left, indent + 1);
            ast_print(binary->right, indent + 1);
            break;
        }
        case AST_UNARY_EXPR: {
            const UnaryExprNode* unary = (const UnaryExprNode*)node;
            ast_print(unary->operand, indent + 1);
            break;
        }
        case AST_BLOCK_STMT: {
            const BlockStmtNode* block = (const BlockStmtNode*)node;
            ast_print(block->statements, indent + 1);
            break;
        }
        default:
            break;
    }
    
    // Print next node in list
    if (node->next) {
        ast_print(node->next, indent);
    }
}