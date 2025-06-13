%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "token.h"
#include "lexer.h"

// External lexer interface
extern int yylex(void);
extern void yyerror(const char* msg);
extern Lexer* current_lexer;

// Global AST root
ASTNode* ast_root = NULL;

// Helper functions
static Position get_current_position(void);
static TokenType bison_token_to_token_type(int bison_token);
%}

// Union type for semantic values
%union {
    struct ASTNode* node;
    char* string;
    int integer;
    double real;
    int token;
}

// Token declarations with types
%token <string> IDENTIFIER STRING_LITERAL
%token <integer> INT_LITERAL
%token <real> FLOAT_LITERAL
%token <token> CHAR_LITERAL

// Go Keywords
%token BREAK CASE CHAN CONST CONTINUE DEFAULT DEFER ELSE FALLTHROUGH
%token FOR FUNC GO GOTO IF IMPORT INTERFACE MAP PACKAGE RANGE RETURN
%token SELECT STRUCT SWITCH TYPE VAR
%token TRUE FALSE NIL

// Goo Extension Keywords
%token COMPTIME PUB SUB REQ REP PUSH PULL TRY CATCH UNSAFE
%token OWNED BORROWED SHARED LET

// Operators and punctuation
%token PLUS MINUS MULTIPLY DIVIDE MODULO
%token ASSIGN SHORT_ASSIGN PLUS_ASSIGN MINUS_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN
%token AND_ASSIGN OR_ASSIGN XOR_ASSIGN LSHIFT_ASSIGN RSHIFT_ASSIGN
%token EQ NE LT LE GT GE
%token AND OR NOT
%token BIT_AND BIT_OR BIT_XOR BIT_NOT LSHIFT RSHIFT
%token INCREMENT DECREMENT
%token ARROW

// Goo Extension Operators
%token BANG QUESTION TRY_OP CATCH_OP

// Delimiters
%token LPAREN RPAREN LBRACE RBRACE LBRACKET RBRACKET
%token SEMICOLON COMMA DOT COLON ELLIPSIS
%token NEWLINE

// Special tokens
%token END_OF_FILE

// Non-terminal types
%type <node> program
%type <node> package_clause import_decl_list import_decl import_spec import_spec_list
%type <node> opt_import_decl_list opt_top_level_decl_list
%type <node> top_level_decl_list top_level_decl
%type <node> declaration func_decl var_decl const_decl type_decl short_var_decl
%type <node> func_signature func_params func_param func_result
%type <node> statement_list statement block simple_stmt
%type <node> if_stmt for_stmt return_stmt break_stmt continue_stmt
%type <node> go_stmt select_stmt defer_stmt select_case_list select_case
%type <node> expression primary_expr unary_expr postfix_expr binary_expr
%type <node> call_expr index_expr selector_expr
%type <node> type type_name array_type slice_type map_type chan_type
%type <node> func_type pointer_type reference_type
%type <node> identifier literal
%type <node> expression_list

// Goo Extensions
%type <node> error_union_type nullable_type try_expr catch_expr
%type <node> comptime_block ownership_qualifier if_let_stmt
%type <token> chan_pattern

// Operator precedence (lowest to highest)
%left COMMA
%right TRY CATCH  // Try/catch expressions (low precedence)
%right ASSIGN SHORT_ASSIGN PLUS_ASSIGN MINUS_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN AND_ASSIGN OR_ASSIGN XOR_ASSIGN LSHIFT_ASSIGN RSHIFT_ASSIGN
%right QUESTION COLON  // Ternary operator (if we add it)
%left OR
%left AND
%left BIT_OR
%left BIT_XOR
%left BIT_AND
%left EQ NE
%left LT LE GT GE
%left LSHIFT RSHIFT
%left PLUS MINUS
%left MULTIPLY DIVIDE MODULO
%left ARROW  // Channel operations
%right NOT BIT_NOT BANG  // Unary operators
%right INCREMENT DECREMENT  // Postfix
%left DOT LBRACKET LPAREN  // Member access, indexing, function calls

// Start symbol
%start program

%%

// Program structure
program:
    package_clause opt_import_decl_list opt_top_level_decl_list {
        ProgramNode* prog = ast_program_new(get_current_position());
        prog->imports = $2;
        prog->decls = $3;
        ast_root = (ASTNode*)prog;
        $$ = (ASTNode*)prog;
    }
    ;

opt_import_decl_list:
    /* empty */ { $$ = NULL; }
    | import_decl_list { $$ = $1; }
    ;

opt_top_level_decl_list:
    /* empty */ { $$ = NULL; }
    | top_level_decl_list { $$ = $1; }
    ;

package_clause:
    PACKAGE identifier {
        IdentifierNode* ident = (IdentifierNode*)$2;
        PackageDeclNode* pkg = ast_package_decl_new(ident->name, ident->base.pos);
        ast_node_free($2);
        $$ = (ASTNode*)pkg;
    }
    ;

import_decl_list:
    import_decl {
        $$ = $1;
    }
    | import_decl_list import_decl {
        ast_add_child($1, $2);
        $$ = $1;
    }
    ;

import_decl:
    IMPORT import_spec {
        $$ = $2;
    }
    | IMPORT LPAREN import_spec_list RPAREN {
        $$ = $3;
    }
    ;

import_spec_list:
    import_spec {
        $$ = $1;
    }
    | import_spec_list import_spec {
        ast_add_child($1, $2);
        $$ = $1;
    }
    ;

import_spec:
    STRING_LITERAL {
        ImportSpecNode* imp = ast_import_spec_new($1, NULL, get_current_position());
        free($1);
        $$ = (ASTNode*)imp;
    }
    | identifier STRING_LITERAL {
        IdentifierNode* ident = (IdentifierNode*)$1;
        ImportSpecNode* imp = ast_import_spec_new($2, ident->name, ident->base.pos);
        ast_node_free($1);
        free($2);
        $$ = (ASTNode*)imp;
    }
    ;

top_level_decl_list:
    top_level_decl {
        $$ = $1;
    }
    | top_level_decl_list top_level_decl {
        ast_add_child($1, $2);
        $$ = $1;
    }
    ;

top_level_decl:
    declaration { $$ = $1; }
    | func_decl { $$ = $1; }
    ;


declaration:
    var_decl { $$ = $1; }
    | const_decl { $$ = $1; }
    | type_decl { $$ = $1; }
    ;

// Function declaration
func_decl:
    FUNC identifier func_signature block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $4;
        func->params = $3;  // Assign the function signature (parameters)
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | COMPTIME FUNC identifier func_signature block {
        IdentifierNode* ident = (IdentifierNode*)$3;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $5;
        func->params = $4;  // Assign the function signature (parameters)
        func->is_comptime = 1;
        ast_node_free($3);
        $$ = (ASTNode*)func;
    }
    | UNSAFE FUNC identifier func_signature block {
        IdentifierNode* ident = (IdentifierNode*)$3;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $5;
        func->params = $4;  // Assign the function signature (parameters)
        func->is_unsafe = 1;
        ast_node_free($3);
        $$ = (ASTNode*)func;
    }
    ;

func_signature:
    LPAREN RPAREN {
        $$ = NULL; // No parameters, no return type
    }
    | LPAREN func_params RPAREN {
        $$ = $2; // Parameters, no return type
    }
    | LPAREN RPAREN func_result {
        $$ = $3; // No parameters, has return type
    }
    | LPAREN func_params RPAREN func_result {
        // TODO: Combine params and result
        $$ = $2;
    }
    ;

func_params:
    func_param {
        $$ = $1;
    }
    | func_params COMMA func_param {
        ast_add_child($1, $3);
        $$ = $1;
    }
    ;

func_param:
    identifier type {
        // Create a variable declaration node for the parameter
        IdentifierNode* ident = (IdentifierNode*)$1;
        VarDeclNode* param = ast_var_decl_new(get_current_position());
        param->names = malloc(sizeof(char*));
        param->names[0] = strdup(ident->name);
        param->name_count = 1;
        param->type = $2;
        param->values = NULL; // Parameters don't have initial values
        ast_node_free($1);
        $$ = (ASTNode*)param;
    }
    | type {
        // Anonymous parameter - create a var decl with no name
        VarDeclNode* param = ast_var_decl_new(get_current_position());
        param->names = NULL;
        param->name_count = 0;
        param->type = $1;
        param->values = NULL;
        $$ = (ASTNode*)param;
    }
    ;

func_result:
    type { $$ = $1; }
    | LPAREN type RPAREN { $$ = $2; }
    ;

// Variable declaration
var_decl:
    VAR identifier type {
        VarDeclNode* var = ast_var_decl_new(get_current_position());
        IdentifierNode* ident = (IdentifierNode*)$2;
        var->names = malloc(sizeof(char*));
        var->names[0] = strdup(ident->name);
        var->name_count = 1;
        var->type = $3;
        ast_node_free($2);
        $$ = (ASTNode*)var;
    }
    | VAR identifier ASSIGN expression {
        VarDeclNode* var = ast_var_decl_new(get_current_position());
        IdentifierNode* ident = (IdentifierNode*)$2;
        var->names = malloc(sizeof(char*));
        var->names[0] = strdup(ident->name);
        var->name_count = 1;
        var->values = $4;
        ast_node_free($2);
        $$ = (ASTNode*)var;
    }
    | VAR identifier type ASSIGN expression {
        VarDeclNode* var = ast_var_decl_new(get_current_position());
        IdentifierNode* ident = (IdentifierNode*)$2;
        var->names = malloc(sizeof(char*));
        var->names[0] = strdup(ident->name);
        var->name_count = 1;
        var->type = $3;
        var->values = $5;
        ast_node_free($2);
        $$ = (ASTNode*)var;
    }
    // Goo extension: ownership qualifiers
    | ownership_qualifier VAR identifier type {
        VarDeclNode* var = ast_var_decl_new(get_current_position());
        IdentifierNode* ident = (IdentifierNode*)$3;
        var->names = malloc(sizeof(char*));
        var->names[0] = strdup(ident->name);
        var->name_count = 1;
        var->type = $4;
        // TODO: Set ownership from $1
        ast_node_free($3);
        $$ = (ASTNode*)var;
    }
    ;

// Short variable declaration
short_var_decl:
    identifier SHORT_ASSIGN expression {
        VarDeclNode* var = ast_var_decl_new(get_current_position());
        IdentifierNode* ident = (IdentifierNode*)$1;
        var->names = malloc(sizeof(char*));
        var->names[0] = strdup(ident->name);
        var->name_count = 1;
        var->values = $3;
        var->is_short_decl = 1;  // Mark as short declaration
        ast_node_free($1);
        $$ = (ASTNode*)var;
    }
    ;

// Constant declaration
const_decl:
    CONST identifier type ASSIGN expression {
        ConstDeclNode* const_node = (ConstDeclNode*)malloc(sizeof(ConstDeclNode));
        const_node->base.type = AST_CONST_DECL;
        const_node->base.pos = get_current_position();
        const_node->base.node_type = NULL;
        const_node->base.next = NULL;
        
        IdentifierNode* ident = (IdentifierNode*)$2;
        const_node->names = malloc(sizeof(char*));
        const_node->names[0] = strdup(ident->name);
        const_node->name_count = 1;
        const_node->type = $3;
        const_node->values = $5;
        const_node->is_comptime = 0;
        
        ast_node_free($2);
        $$ = (ASTNode*)const_node;
    }
    | COMPTIME CONST identifier type ASSIGN expression {
        ConstDeclNode* const_node = (ConstDeclNode*)malloc(sizeof(ConstDeclNode));
        const_node->base.type = AST_CONST_DECL;
        const_node->base.pos = get_current_position();
        const_node->base.node_type = NULL;
        const_node->base.next = NULL;
        
        IdentifierNode* ident = (IdentifierNode*)$3;
        const_node->names = malloc(sizeof(char*));
        const_node->names[0] = strdup(ident->name);
        const_node->name_count = 1;
        const_node->type = $4;
        const_node->values = $6;
        const_node->is_comptime = 1;
        
        ast_node_free($3);
        $$ = (ASTNode*)const_node;
    }
    ;

// Type declaration
type_decl:
    TYPE identifier type {
        TypeDeclNode* type_node = (TypeDeclNode*)malloc(sizeof(TypeDeclNode));
        type_node->base.type = AST_TYPE_DECL;
        type_node->base.pos = get_current_position();
        type_node->base.node_type = NULL;
        type_node->base.next = NULL;
        
        IdentifierNode* ident = (IdentifierNode*)$2;
        type_node->name = strdup(ident->name);
        type_node->type = $3;
        
        ast_node_free($2);
        $$ = (ASTNode*)type_node;
    }
    ;

// Statements
block:
    LBRACE statement_list RBRACE {
        BlockStmtNode* block = ast_block_stmt_new(get_current_position());
        block->statements = $2;
        $$ = (ASTNode*)block;
    }
    | LBRACE RBRACE {
        BlockStmtNode* block = ast_block_stmt_new(get_current_position());
        $$ = (ASTNode*)block;
    }
    ;

statement_list:
    statement {
        $$ = $1;
    }
    | statement_list statement {
        ast_add_child($1, $2);
        $$ = $1;
    }
    ;

statement:
    simple_stmt SEMICOLON { $$ = $1; }
    | simple_stmt { $$ = $1; }  // Allow statements without semicolon
    | if_stmt { $$ = $1; }
    | if_let_stmt { $$ = $1; }
    | for_stmt { $$ = $1; }
    | return_stmt SEMICOLON { $$ = $1; }
    | return_stmt { $$ = $1; }  // Allow return without semicolon
    | break_stmt SEMICOLON { $$ = $1; }
    | break_stmt { $$ = $1; }  // Allow break without semicolon
    | continue_stmt SEMICOLON { $$ = $1; }
    | continue_stmt { $$ = $1; }  // Allow continue without semicolon
    | go_stmt SEMICOLON { $$ = $1; }
    | go_stmt { $$ = $1; }  // Allow go without semicolon
    | defer_stmt SEMICOLON { $$ = $1; }
    | defer_stmt { $$ = $1; }  // Allow defer without semicolon
    | select_stmt { $$ = $1; }
    | block { $$ = $1; }
    | comptime_block { $$ = $1; }  // Goo extension
    ;

simple_stmt:
    expression {
        ExprStmtNode* expr_stmt = (ExprStmtNode*)malloc(sizeof(ExprStmtNode));
        expr_stmt->base.type = AST_EXPR_STMT;
        expr_stmt->base.pos = get_current_position();
        expr_stmt->base.node_type = NULL;
        expr_stmt->base.next = NULL;
        expr_stmt->expr = $1;
        $$ = (ASTNode*)expr_stmt;
    }
    | short_var_decl { $$ = $1; }
    | var_decl { $$ = $1; }
    ;

if_stmt:
    IF expression block {
        IfStmtNode* if_node = (IfStmtNode*)malloc(sizeof(IfStmtNode));
        if_node->base.type = AST_IF_STMT;
        if_node->base.pos = get_current_position();
        if_node->base.node_type = NULL;
        if_node->base.next = NULL;
        if_node->condition = $2;
        if_node->then_stmt = $3;
        if_node->else_stmt = NULL;
        $$ = (ASTNode*)if_node;
    }
    | IF expression block ELSE block {
        IfStmtNode* if_node = (IfStmtNode*)malloc(sizeof(IfStmtNode));
        if_node->base.type = AST_IF_STMT;
        if_node->base.pos = get_current_position();
        if_node->base.node_type = NULL;
        if_node->base.next = NULL;
        if_node->condition = $2;
        if_node->then_stmt = $3;
        if_node->else_stmt = $5;
        $$ = (ASTNode*)if_node;
    }
    | IF expression block ELSE if_stmt {
        IfStmtNode* if_node = (IfStmtNode*)malloc(sizeof(IfStmtNode));
        if_node->base.type = AST_IF_STMT;
        if_node->base.pos = get_current_position();
        if_node->base.node_type = NULL;
        if_node->base.next = NULL;
        if_node->condition = $2;
        if_node->then_stmt = $3;
        if_node->else_stmt = $5;
        $$ = (ASTNode*)if_node;
    }
    ;

if_let_stmt:
    IF LET identifier ASSIGN expression block {
        IdentifierNode* var_ident = (IdentifierNode*)$3;
        IfLetStmtNode* if_let_node = ast_if_let_stmt_new(var_ident->name, $5, $6, NULL, get_current_position());
        ast_node_free($3);  // Free the temporary identifier node
        $$ = (ASTNode*)if_let_node;
    }
    | IF LET identifier ASSIGN expression block ELSE block {
        IdentifierNode* var_ident = (IdentifierNode*)$3;
        IfLetStmtNode* if_let_node = ast_if_let_stmt_new(var_ident->name, $5, $6, $8, get_current_position());
        ast_node_free($3);  // Free the temporary identifier node
        $$ = (ASTNode*)if_let_node;
    }
    | IF LET identifier ASSIGN expression block ELSE if_let_stmt {
        IdentifierNode* var_ident = (IdentifierNode*)$3;
        IfLetStmtNode* if_let_node = ast_if_let_stmt_new(var_ident->name, $5, $6, $8, get_current_position());
        ast_node_free($3);  // Free the temporary identifier node
        $$ = (ASTNode*)if_let_node;
    }
    ;

for_stmt:
    FOR block {
        ForStmtNode* for_node = (ForStmtNode*)malloc(sizeof(ForStmtNode));
        for_node->base.type = AST_FOR_STMT;
        for_node->base.pos = get_current_position();
        for_node->base.node_type = NULL;
        for_node->base.next = NULL;
        for_node->init = NULL;
        for_node->condition = NULL;
        for_node->post = NULL;
        for_node->body = $2;
        $$ = (ASTNode*)for_node;
    }
    | FOR expression block {
        ForStmtNode* for_node = (ForStmtNode*)malloc(sizeof(ForStmtNode));
        for_node->base.type = AST_FOR_STMT;
        for_node->base.pos = get_current_position();
        for_node->base.node_type = NULL;
        for_node->base.next = NULL;
        for_node->init = NULL;
        for_node->condition = $2;
        for_node->post = NULL;
        for_node->body = $3;
        $$ = (ASTNode*)for_node;
    }
    ;

return_stmt:
    RETURN {
        ReturnStmtNode* ret = (ReturnStmtNode*)malloc(sizeof(ReturnStmtNode));
        ret->base.type = AST_RETURN_STMT;
        ret->base.pos = get_current_position();
        ret->base.node_type = NULL;
        ret->base.next = NULL;
        ret->values = NULL;
        $$ = (ASTNode*)ret;
    }
    | RETURN expression {
        ReturnStmtNode* ret = (ReturnStmtNode*)malloc(sizeof(ReturnStmtNode));
        ret->base.type = AST_RETURN_STMT;
        ret->base.pos = get_current_position();
        ret->base.node_type = NULL;
        ret->base.next = NULL;
        ret->values = $2;
        $$ = (ASTNode*)ret;
    }
    ;

break_stmt:
    BREAK {
        ASTNode* break_node = ast_node_new(AST_BREAK_STMT, get_current_position());
        $$ = break_node;
    }
    ;

continue_stmt:
    CONTINUE {
        ASTNode* continue_node = ast_node_new(AST_CONTINUE_STMT, get_current_position());
        $$ = continue_node;
    }
    ;

go_stmt:
    GO call_expr {
        GoStmtNode* go_node = ast_go_stmt_new($2, get_current_position());
        $$ = (ASTNode*)go_node;
    }
    ;

defer_stmt:
    DEFER call_expr {
        DeferStmtNode* defer_node = ast_defer_stmt_new($2, get_current_position());
        $$ = (ASTNode*)defer_node;
    }
    ;

select_stmt:
    SELECT LBRACE select_case_list RBRACE {
        SelectStmtNode* select_node = ast_select_stmt_new(get_current_position());
        select_node->cases = $3;
        $$ = (ASTNode*)select_node;
    }
    ;

select_case_list:
    select_case {
        $$ = $1;
    }
    | select_case_list select_case {
        // Link the cases together
        ASTNode* current = $1;
        while (current->next) {
            current = current->next;
        }
        current->next = $2;
        $$ = $1;
    }
    ;

select_case:
    CASE expression COLON statement_list {
        SelectCaseNode* case_node = ast_select_case_new($2, $4, get_current_position());
        $$ = (ASTNode*)case_node;
    }
    | DEFAULT COLON statement_list {
        SelectCaseNode* case_node = ast_select_case_new(NULL, $3, get_current_position());
        $$ = (ASTNode*)case_node;
    }
    ;

// Expressions
expression:
    unary_expr { $$ = $1; }
    | postfix_expr { $$ = $1; }
    | binary_expr { $$ = $1; }
    | try_expr { $$ = $1; }      // Goo extension
    | catch_expr { $$ = $1; }    // Goo extension
    ;

unary_expr:
    primary_expr { $$ = $1; }
    | NOT unary_expr {
        UnaryExprNode* unary = ast_unary_expr_new(bison_token_to_token_type(NOT), $2, get_current_position());
        $$ = (ASTNode*)unary;
    }
    | BIT_NOT unary_expr {
        UnaryExprNode* unary = ast_unary_expr_new(bison_token_to_token_type(BIT_NOT), $2, get_current_position());
        $$ = (ASTNode*)unary;
    }
    | MINUS unary_expr {
        UnaryExprNode* unary = ast_unary_expr_new(bison_token_to_token_type(MINUS), $2, get_current_position());
        $$ = (ASTNode*)unary;
    }
    | PLUS unary_expr {
        UnaryExprNode* unary = ast_unary_expr_new(bison_token_to_token_type(PLUS), $2, get_current_position());
        $$ = (ASTNode*)unary;
    }
    | ARROW unary_expr {
        UnaryExprNode* unary = ast_unary_expr_new(bison_token_to_token_type(ARROW), $2, get_current_position());
        $$ = (ASTNode*)unary;
    }
    | BIT_AND unary_expr {
        UnaryExprNode* unary = ast_unary_expr_new(bison_token_to_token_type(BIT_AND), $2, get_current_position());
        $$ = (ASTNode*)unary;
    }
    | MULTIPLY unary_expr {
        UnaryExprNode* unary = ast_unary_expr_new(bison_token_to_token_type(MULTIPLY), $2, get_current_position());
        $$ = (ASTNode*)unary;
    }
    ;

postfix_expr:
    primary_expr NOT {
        PostfixExprNode* postfix = ast_postfix_expr_new($1, NOT, get_current_position());
        $$ = (ASTNode*)postfix;
    }
    ;

binary_expr:
    expression PLUS expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(PLUS), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression MINUS expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(MINUS), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression MULTIPLY expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(MULTIPLY), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression DIVIDE expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(DIVIDE), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression MODULO expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(MODULO), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression EQ expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(EQ), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression NE expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(NE), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression LT expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(LT), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression LE expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(LE), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression GT expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(GT), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression GE expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(GE), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression AND expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(AND), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression OR expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(OR), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression ASSIGN expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(ASSIGN), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression SHORT_ASSIGN expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(SHORT_ASSIGN), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression ARROW expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(ARROW), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    ;

primary_expr:
    identifier { $$ = $1; }
    | literal { $$ = $1; }
    | call_expr { $$ = $1; }
    | index_expr { $$ = $1; }
    | selector_expr { $$ = $1; }
    | LPAREN expression RPAREN { $$ = $2; }
    ;

call_expr:
    primary_expr LPAREN RPAREN {
        CallExprNode* call = (CallExprNode*)malloc(sizeof(CallExprNode));
        call->base.type = AST_CALL_EXPR;
        call->base.pos = get_current_position();
        call->base.node_type = NULL;
        call->base.next = NULL;
        call->function = $1;
        call->args = NULL;
        $$ = (ASTNode*)call;
    }
    | primary_expr LPAREN expression_list RPAREN {
        CallExprNode* call = (CallExprNode*)malloc(sizeof(CallExprNode));
        call->base.type = AST_CALL_EXPR;
        call->base.pos = get_current_position();
        call->base.node_type = NULL;
        call->base.next = NULL;
        call->function = $1;
        call->args = $3;
        $$ = (ASTNode*)call;
    }
    ;

index_expr:
    primary_expr LBRACKET expression RBRACKET {
        IndexExprNode* index = (IndexExprNode*)malloc(sizeof(IndexExprNode));
        index->base.type = AST_INDEX_EXPR;
        index->base.pos = get_current_position();
        index->base.node_type = NULL;
        index->base.next = NULL;
        index->expr = $1;
        index->index = $3;
        $$ = (ASTNode*)index;
    }
    ;

selector_expr:
    primary_expr DOT identifier {
        IdentifierNode* ident = (IdentifierNode*)$3;
        SelectorExprNode* selector = (SelectorExprNode*)malloc(sizeof(SelectorExprNode));
        selector->base.type = AST_SELECTOR_EXPR;
        selector->base.pos = get_current_position();
        selector->base.node_type = NULL;
        selector->base.next = NULL;
        selector->expr = $1;
        selector->selector = strdup(ident->name);
        ast_node_free($3);
        $$ = (ASTNode*)selector;
    }
    ;

expression_list:
    expression {
        $$ = $1;
    }
    | expression_list COMMA expression {
        ast_add_child($1, $3);
        $$ = $1;
    }
    ;

// Types
type:
    type_name { $$ = $1; }
    | array_type { $$ = $1; }
    | slice_type { $$ = $1; }
    | map_type { $$ = $1; }
    | chan_type { $$ = $1; }
    | func_type { $$ = $1; }
    | pointer_type { $$ = $1; }
    | reference_type { $$ = $1; }     // Goo extension
    | error_union_type { $$ = $1; }   // Goo extension
    | nullable_type { $$ = $1; }      // Goo extension
    ;

type_name:
    identifier {
        BasicTypeNode* basic = (BasicTypeNode*)malloc(sizeof(BasicTypeNode));
        basic->base.type = AST_BASIC_TYPE;
        basic->base.pos = get_current_position();
        basic->base.node_type = NULL;
        basic->base.next = NULL;
        
        IdentifierNode* ident = (IdentifierNode*)$1;
        basic->name = strdup(ident->name);
        ast_node_free($1);
        $$ = (ASTNode*)basic;
    }
    ;

array_type:
    LBRACKET expression RBRACKET type {
        ArrayTypeNode* array = (ArrayTypeNode*)malloc(sizeof(ArrayTypeNode));
        array->base.type = AST_ARRAY_TYPE;
        array->base.pos = get_current_position();
        array->base.node_type = NULL;
        array->base.next = NULL;
        array->length = $2;
        array->element_type = $4;
        $$ = (ASTNode*)array;
    }
    ;

slice_type:
    LBRACKET RBRACKET type {
        SliceTypeNode* slice = (SliceTypeNode*)malloc(sizeof(SliceTypeNode));
        slice->base.type = AST_SLICE_TYPE;
        slice->base.pos = get_current_position();
        slice->base.node_type = NULL;
        slice->base.next = NULL;
        slice->element_type = $3;
        $$ = (ASTNode*)slice;
    }
    ;

map_type:
    MAP LBRACKET type RBRACKET type {
        MapTypeNode* map = (MapTypeNode*)malloc(sizeof(MapTypeNode));
        map->base.type = AST_MAP_TYPE;
        map->base.pos = get_current_position();
        map->base.node_type = NULL;
        map->base.next = NULL;
        map->key_type = $3;
        map->value_type = $5;
        $$ = (ASTNode*)map;
    }
    ;

chan_type:
    CHAN type {
        ChanTypeNode* chan = ast_chan_type_new($2, CHAN_PATTERN_BASIC, get_current_position());
        $$ = (ASTNode*)chan;
    }
    | chan_pattern CHAN type {
        ChanTypeNode* chan = ast_chan_type_new($3, (ChannelPattern)$1, get_current_position());
        $$ = (ASTNode*)chan;
    }
    ;

chan_pattern:
    PUB { $$ = (int)CHAN_PATTERN_PUB; }
    | SUB { $$ = (int)CHAN_PATTERN_SUB; }
    | REQ { $$ = (int)CHAN_PATTERN_REQ; }
    | REP { $$ = (int)CHAN_PATTERN_REP; }
    | PUSH { $$ = (int)CHAN_PATTERN_PUSH; }
    | PULL { $$ = (int)CHAN_PATTERN_PULL; }
    ;

func_type:
    FUNC func_signature {
        // TODO: Create proper function type
        $$ = $2;
    }
    ;

pointer_type:
    MULTIPLY type {
        PointerTypeNode* ptr = (PointerTypeNode*)malloc(sizeof(PointerTypeNode));
        ptr->base.type = AST_POINTER_TYPE;
        ptr->base.pos = get_current_position();
        ptr->base.node_type = NULL;
        ptr->base.next = NULL;
        ptr->element_type = $2;
        $$ = (ASTNode*)ptr;
    }
    ;

reference_type:
    BIT_AND type {
        ReferenceTypeNode* ref = ast_reference_type_new($2, 0, get_current_position());
        $$ = (ASTNode*)ref;
    }
    ;

// Goo Extensions

error_union_type:
    BANG type {
        ErrorUnionTypeNode* error_union = ast_error_union_type_new($2, get_current_position());
        $$ = (ASTNode*)error_union;
    }
    ;

nullable_type:
    QUESTION type {
        NullableTypeNode* nullable = ast_nullable_type_new($2, get_current_position());
        $$ = (ASTNode*)nullable;
    }
    ;

try_expr:
    TRY expression {
        TryExprNode* try_node = ast_try_expr_new($2, get_current_position());
        $$ = (ASTNode*)try_node;
    }
    ;

catch_expr:
    expression CATCH identifier block {
        IdentifierNode* ident = (IdentifierNode*)$3;
        CatchExprNode* catch_node = ast_catch_expr_new($1, ident->name, $4, get_current_position());
        ast_node_free($3);
        $$ = (ASTNode*)catch_node;
    }
    ;

comptime_block:
    COMPTIME block {
        ComptimeBlockNode* comptime_node = ast_comptime_block_new($2, get_current_position());
        $$ = (ASTNode*)comptime_node;
    }
    ;

ownership_qualifier:
    OWNED { $$ = NULL; /* TODO: Create ownership node */ }
    | BORROWED { $$ = NULL; }
    | SHARED { $$ = NULL; }
    ;

// Basic elements
identifier:
    IDENTIFIER {
        IdentifierNode* ident = ast_identifier_new($1, get_current_position());
        free($1);
        $$ = (ASTNode*)ident;
    }
    ;

literal:
    INT_LITERAL {
        char int_str[32];
        snprintf(int_str, sizeof(int_str), "%d", $1);
        LiteralNode* lit = ast_literal_new(TOKEN_INT, int_str, get_current_position());
        $$ = (ASTNode*)lit;
    }
    | FLOAT_LITERAL {
        char float_str[64];
        snprintf(float_str, sizeof(float_str), "%f", $1);
        LiteralNode* lit = ast_literal_new(TOKEN_FLOAT, float_str, get_current_position());
        $$ = (ASTNode*)lit;
    }
    | STRING_LITERAL {
        LiteralNode* lit = ast_literal_new(TOKEN_STRING, $1, get_current_position());
        free($1);
        $$ = (ASTNode*)lit;
    }
    | TRUE {
        LiteralNode* lit = ast_literal_new(TOKEN_TRUE, "true", get_current_position());
        $$ = (ASTNode*)lit;
    }
    | FALSE {
        LiteralNode* lit = ast_literal_new(TOKEN_FALSE, "false", get_current_position());
        $$ = (ASTNode*)lit;
    }
    | NIL {
        LiteralNode* lit = ast_literal_new(TOKEN_NIL, "nil", get_current_position());
        $$ = (ASTNode*)lit;
    }
    ;

%%

// Error reporting function
void yyerror(const char* msg) {
    if (current_lexer) {
        fprintf(stderr, "Parse error at %s:%d:%d: %s\n",
                current_lexer->filename ? current_lexer->filename : "<stdin>",
                current_lexer->pos.line,
                current_lexer->pos.column,
                msg);
    } else {
        fprintf(stderr, "Parse error: %s\n", msg);
    }
}

// Helper function to get current position
static Position get_current_position(void) {
    Position pos = {1, 1, 0, "<unknown>"};
    if (current_lexer) {
        pos = current_lexer->pos;
    }
    return pos;
}

// Convert Bison tokens to TokenType enum values
static TokenType bison_token_to_token_type(int bison_token) {
    switch (bison_token) {
        case PLUS: return TOKEN_PLUS;
        case MINUS: return TOKEN_MINUS;
        case MULTIPLY: return TOKEN_MULTIPLY;
        case DIVIDE: return TOKEN_DIVIDE;
        case MODULO: return TOKEN_MODULO;
        case ASSIGN: return TOKEN_ASSIGN;
        case SHORT_ASSIGN: return TOKEN_SHORT_ASSIGN;
        case EQ: return TOKEN_EQ;
        case NE: return TOKEN_NE;
        case LT: return TOKEN_LT;
        case LE: return TOKEN_LE;
        case GT: return TOKEN_GT;
        case GE: return TOKEN_GE;
        case AND: return TOKEN_AND;
        case OR: return TOKEN_OR;
        case NOT: return TOKEN_NOT;
        case BIT_AND: return TOKEN_BIT_AND;
        case BIT_OR: return TOKEN_BIT_OR;
        case BIT_XOR: return TOKEN_BIT_XOR;
        case BIT_NOT: return TOKEN_BIT_NOT;
        case LSHIFT: return TOKEN_LSHIFT;
        case RSHIFT: return TOKEN_RSHIFT;
        case ARROW: return TOKEN_ARROW;
        default: return TOKEN_UNKNOWN;
    }
}