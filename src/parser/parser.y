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
%token SELECT STRUCT SWITCH TYPE VAR ENUM
%token TRUE FALSE NIL

// Goo Extension Keywords
%token COMPTIME CONCEPT PUB SUB REQ REP PUSH PULL TRY CATCH UNSAFE ASM
%token EXTERN FROM VOLATILE INLINE NO_STD
%token PARALLEL REDUCE BARRIER ATOMIC THREAD_LOCAL
%token OWNED BORROWED SHARED LET MATCH
%token KERNEL DEVICE HOST GLOBAL SHARED_MEM CONSTANT LOCAL

// WebAssembly Keywords
%token WASM EXPORT MEMORY TABLE START ELEM DATA

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
%token BANG QUESTION TRY_OP CATCH_OP DEREF

// Delimiters
%token LPAREN RPAREN LBRACE RBRACE LBRACKET RBRACKET
// LBRACE_BODY: context-sensitive variant emitted by the lexer bridge in place
// of LBRACE when the bridge's frame stack says "this `{` closes a cond/match/
// select context that's been pinned open by a preceding IF/FOR/MATCH/SELECT
// at depth 0." struct_lit's grammar accepts only LBRACE, so the IDENT.LBRACE
// shift/reduce ambiguity never arises at the cond/body boundary. See
// docs/M10_GRAMMAR_DECISION.md + docs/M10_LEXER_LAYER_PROBE.md.
%token LBRACE_BODY
%token SEMICOLON COMMA DOT COLON ELLIPSIS
%token NEWLINE

// Special tokens
%token END_OF_FILE

// Non-terminal types
%type <node> program
%type <node> package_clause import_decl_list import_decl import_spec import_spec_list
%type <node> opt_import_decl_list opt_top_level_decl_list
%type <node> top_level_decl_list top_level_decl
%type <node> declaration func_decl var_decl const_decl type_decl concept_decl short_var_decl extern_decl
%type <node> concept_body concept_requirement_list concept_requirement type_param_list type_param
%type <node> func_signature func_params func_param func_result
%type <node> statement_list statement block simple_stmt
%type <node> if_stmt for_stmt return_stmt break_stmt continue_stmt
%type <node> go_stmt select_stmt defer_stmt select_case_list select_case
%type <node> switch_stmt case_clause_list case_clause
%type <node> unsafe_stmt asm_stmt parallel_for_stmt
%type <node> parallel_reduce_expr barrier_call atomic_expr thread_local_decl
%type <node> expression primary_expr unary_expr postfix_expr binary_expr
%type <node> call_expr index_expr selector_expr
%type <node> type type_name array_type slice_type map_type chan_type
%type <node> func_type pointer_type reference_type unsafe_ptr_type
%type <node> struct_type struct_field_list struct_field
%type <node> enum_type enum_variant_list enum_variant
%type <node> slice_lit
%type <node> multi_return_type_list
%type <node> map_lit map_entry_list map_entry
%type <node> struct_lit struct_lit_inits struct_lit_init
%type <node> identifier literal
%type <node> expression_list

// Goo Extensions
%type <node> error_union_type nullable_type try_expr catch_expr
%type <node> comptime_block ownership_qualifier if_let_stmt
%type <node> attribute attribute_list volatile_expr
%type <node> match_expr match_case_list match_case pattern guard_condition
%type <node> kernel_decl kernel_launch gpu_memory_alloc gpu_memory_copy gpu_sync gpu_intrinsic
%type <node> wasm_export wasm_import wasm_memory wasm_table wasm_global wasm_start
%type <node> js_interop dom_access
%type <token> chan_pattern gpu_memory_qualifier wasm_value_type

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
    | concept_decl { $$ = $1; }
    | kernel_decl { $$ = $1; }
    | wasm_export { $$ = $1; }
    | wasm_import { $$ = $1; }
    | wasm_memory { $$ = $1; }
    | wasm_table { $$ = $1; }
    | wasm_global { $$ = $1; }
    | wasm_start { $$ = $1; }
    ;


declaration:
    var_decl { $$ = $1; }
    | const_decl { $$ = $1; }
    | type_decl { $$ = $1; }
    | extern_decl { $$ = $1; }
    ;

// Function declaration
func_decl:
    FUNC identifier LPAREN RPAREN block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $5;
        func->params = NULL;
        func->return_type = NULL;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | FUNC identifier LPAREN func_params RPAREN block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $6;
        func->params = $4;
        func->return_type = NULL;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | FUNC identifier LPAREN RPAREN func_result block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $6;
        func->params = NULL;
        func->return_type = $5;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | FUNC identifier LPAREN func_params RPAREN func_result block {
        // The "both params and result" form — previously fell through
        // func_signature's TODO and dropped the result, leaving every
        // function with return_type=NULL and the type checker stamping
        // them all as `() -> void`.
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $7;
        func->params = $4;
        func->return_type = $6;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | FUNC identifier func_signature block {
        // Kept as a fall-back catch (covers attribute_list/COMPTIME/
        // UNSAFE-prefixed forms that still go via func_signature). The
        // attribute variants still drop the result; tracked separately.
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $4;
        func->params = $3;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | attribute_list FUNC identifier func_signature block {
        IdentifierNode* ident = (IdentifierNode*)$3;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $5;
        func->params = $4;  // Assign the function signature (parameters)
        func->annotations = $1;  // Assign annotations
        ast_node_free($3);
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
    | attribute_list COMPTIME FUNC identifier func_signature block {
        IdentifierNode* ident = (IdentifierNode*)$4;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $6;
        func->params = $5;  // Assign the function signature (parameters)
        func->annotations = $1;  // Assign annotations
        func->is_comptime = 1;
        ast_node_free($4);
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
    | attribute_list UNSAFE FUNC identifier func_signature block {
        IdentifierNode* ident = (IdentifierNode*)$4;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $6;
        func->params = $5;  // Assign the function signature (parameters)
        func->annotations = $1;  // Assign annotations
        func->is_unsafe = 1;
        ast_node_free($4);
        $$ = (ASTNode*)func;
    }
    /* Methods: `func (p T) name(params) result { ... }`. The receiver
       (a func_param: name + type) is spliced as the head of params, so
       every params loop in typecheck/codegen handles it uniformly; the
       receiver alias marks the decl as a method for name mangling
       (Type__name). Four forms cover {params?}×{result?}. */
    | FUNC LPAREN func_param RPAREN identifier LPAREN RPAREN block {
        IdentifierNode* ident = (IdentifierNode*)$5;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $8;
        func->return_type = NULL;
        ((ASTNode*)$3)->next = NULL;
        func->params = $3;
        func->receiver = $3;
        ast_node_free($5);
        $$ = (ASTNode*)func;
    }
    | FUNC LPAREN func_param RPAREN identifier LPAREN func_params RPAREN block {
        IdentifierNode* ident = (IdentifierNode*)$5;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $9;
        func->return_type = NULL;
        ((ASTNode*)$3)->next = $7;
        func->params = $3;
        func->receiver = $3;
        ast_node_free($5);
        $$ = (ASTNode*)func;
    }
    | FUNC LPAREN func_param RPAREN identifier LPAREN RPAREN func_result block {
        IdentifierNode* ident = (IdentifierNode*)$5;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $9;
        func->return_type = $8;
        ((ASTNode*)$3)->next = NULL;
        func->params = $3;
        func->receiver = $3;
        ast_node_free($5);
        $$ = (ASTNode*)func;
    }
    | FUNC LPAREN func_param RPAREN identifier LPAREN func_params RPAREN func_result block {
        IdentifierNode* ident = (IdentifierNode*)$5;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        func->body = $10;
        func->return_type = $9;
        ((ASTNode*)$3)->next = $7;
        func->params = $3;
        func->receiver = $3;
        ast_node_free($5);
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
    | LPAREN multi_return_type_list RPAREN {
        // Multi-return: builds an anonymous StructTypeNode whose
        // fields are VarDeclNodes named _0, _1, ... — the type
        // checker reuses its existing AST_STRUCT_TYPE path to
        // build a TYPE_STRUCT. Function codegen then returns the
        // anonymous struct via LLVM insertvalue + ret.
        StructTypeNode* st = (StructTypeNode*)malloc(sizeof(StructTypeNode));
        st->base.type = AST_STRUCT_TYPE;
        st->base.pos = get_current_position();
        st->base.node_type = NULL;
        st->base.next = NULL;
        st->fields = $2;
        $$ = (ASTNode*)st;
    }
    ;

multi_return_type_list:
    type COMMA type {
        // Always at least two types (the COMMA disambiguates from
        // the single-type form above).
        char buf[8];
        VarDeclNode* f0 = ast_var_decl_new(get_current_position());
        f0->names = malloc(sizeof(char*)); snprintf(buf, sizeof(buf), "_0");
        f0->names[0] = strdup(buf); f0->name_count = 1;
        f0->type = $1; f0->values = NULL;
        VarDeclNode* f1 = ast_var_decl_new(get_current_position());
        f1->names = malloc(sizeof(char*)); snprintf(buf, sizeof(buf), "_1");
        f1->names[0] = strdup(buf); f1->name_count = 1;
        f1->type = $3; f1->values = NULL;
        ast_add_child((ASTNode*)f0, (ASTNode*)f1);
        $$ = (ASTNode*)f0;
    }
    | multi_return_type_list COMMA type {
        // Append additional return types. Field index = current
        // length of the chain. Walk to find it.
        ASTNode* tail = $1;
        size_t idx = 1;
        while (tail->next) { tail = tail->next; idx++; }
        char buf[16];
        VarDeclNode* fn = ast_var_decl_new(get_current_position());
        fn->names = malloc(sizeof(char*)); snprintf(buf, sizeof(buf), "_%zu", idx);
        fn->names[0] = strdup(buf); fn->name_count = 1;
        fn->type = $3; fn->values = NULL;
        ast_add_child($1, (ASTNode*)fn);
        $$ = $1;
    }
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
        var->is_short_decl = 1;
        ast_node_free($1);
        $$ = (ASTNode*)var;
    }
    | identifier COMMA identifier SHORT_ASSIGN expression {
        // Multi-LHS short var decl `a, b := expr`. Destructuring is
        // resolved at codegen time: the RHS must produce a TYPE_STRUCT
        // with at least 2 fields, and a/b are bound to its fields 0
        // and 1 respectively.
        VarDeclNode* var = ast_var_decl_new(get_current_position());
        IdentifierNode* i1 = (IdentifierNode*)$1;
        IdentifierNode* i2 = (IdentifierNode*)$3;
        var->names = malloc(sizeof(char*) * 2);
        var->names[0] = strdup(i1->name);
        var->names[1] = strdup(i2->name);
        var->name_count = 2;
        var->values = $5;
        var->is_short_decl = 1;
        ast_node_free($1);
        ast_node_free($3);
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

// Concept declaration
concept_decl:
    CONCEPT identifier concept_body {
        IdentifierNode* ident = (IdentifierNode*)$2;
        ConceptDeclNode* concept = ast_concept_decl_new(ident->name, ident->base.pos);
        concept->requirements = $3;
        ast_node_free($2);
        $$ = (ASTNode*)concept;
    }
    | CONCEPT identifier LBRACKET type_param_list RBRACKET concept_body {
        IdentifierNode* ident = (IdentifierNode*)$2;
        ConceptDeclNode* concept = ast_concept_decl_new(ident->name, ident->base.pos);
        concept->type_params = $4;
        concept->requirements = $6;
        ast_node_free($2);
        $$ = (ASTNode*)concept;
    }
    ;

concept_body:
    LBRACE concept_requirement_list RBRACE {
        $$ = $2;
    }
    | LBRACE RBRACE {
        $$ = NULL;
    }
    ;

concept_requirement_list:
    concept_requirement {
        $$ = $1;
    }
    | concept_requirement_list concept_requirement {
        // Link requirements together
        ASTNode* current = $1;
        while (current->next) {
            current = current->next;
        }
        current->next = $2;
        $$ = $1;
    }
    ;

concept_requirement:
    IDENTIFIER {
        // Simple constraint requirement like "Addable"
        IdentifierNode* req = ast_identifier_new($1, get_current_position());
        free($1);
        $$ = (ASTNode*)req;
    }
    | func_signature {
        // Method requirement
        $$ = $1;
    }
    ;

type_param_list:
    type_param {
        $$ = $1;
    }
    | type_param_list COMMA type_param {
        // Link type parameters together
        ASTNode* current = $1;
        while (current->next) {
            current = current->next;
        }
        current->next = $3;
        $$ = $1;
    }
    ;

type_param:
    identifier {
        // Regular type parameter (e.g., T)
        $$ = $1;
    }
    | identifier LBRACKET RBRACKET {
        // Higher-kinded type parameter (e.g., F[_])
        IdentifierNode* ident = (IdentifierNode*)$1;
        ident->base.type = AST_HKT_PARAM;  // Mark as HKT parameter
        $$ = $1;
    }
    | identifier LBRACKET identifier RBRACKET {
        // Higher-kinded type parameter with named parameter (e.g., F[T])
        IdentifierNode* ident = (IdentifierNode*)$1;
        ident->base.type = AST_HKT_PARAM;  // Mark as HKT parameter
        // TODO: Store the parameter name for future use
        ast_node_free($3);
        $$ = $1;
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
    /* LBRACE_BODY variants: identical AST, different brace token. The lexer
       bridge emits LBRACE_BODY in place of LBRACE when we're transitioning
       from cond/match/select context into the body — see M10 design docs. */
    | LBRACE_BODY statement_list RBRACE {
        BlockStmtNode* block = ast_block_stmt_new(get_current_position());
        block->statements = $2;
        $$ = (ASTNode*)block;
    }
    | LBRACE_BODY RBRACE {
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
    | switch_stmt { $$ = $1; }
    | block { $$ = $1; }
    | comptime_block { $$ = $1; }  // Goo extension
    | unsafe_stmt { $$ = $1; }     // Goo extension
    | asm_stmt SEMICOLON { $$ = $1; } // Goo extension
    | asm_stmt { $$ = $1; }        // Goo extension
    | parallel_for_stmt { $$ = $1; } // Goo extension
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
        ForStmtNode* for_node = (ForStmtNode*)calloc(1, sizeof(ForStmtNode));
        for_node->base.type = AST_FOR_STMT;
        for_node->base.pos = get_current_position();
        for_node->body = $2;
        $$ = (ASTNode*)for_node;
    }
    | FOR expression block {
        ForStmtNode* for_node = (ForStmtNode*)calloc(1, sizeof(ForStmtNode));
        for_node->base.type = AST_FOR_STMT;
        for_node->base.pos = get_current_position();
        for_node->condition = $2;
        for_node->body = $3;
        $$ = (ASTNode*)for_node;
    }
    | FOR simple_stmt SEMICOLON expression SEMICOLON simple_stmt block {
        // C-style three-clause `for init; cond; post { … }`.
        ForStmtNode* for_node = (ForStmtNode*)calloc(1, sizeof(ForStmtNode));
        for_node->base.type = AST_FOR_STMT;
        for_node->base.pos = get_current_position();
        for_node->init = $2;
        for_node->condition = $4;
        for_node->post = $6;
        for_node->body = $7;
        $$ = (ASTNode*)for_node;
    }
    | FOR identifier SHORT_ASSIGN RANGE expression block {
        // `for k := range expr { … }` — key/index-only form.
        ForStmtNode* for_node = (ForStmtNode*)calloc(1, sizeof(ForStmtNode));
        for_node->base.type = AST_FOR_STMT;
        for_node->base.pos = get_current_position();
        IdentifierNode* kid = (IdentifierNode*)$2;
        for_node->key_name = strdup(kid->name);
        for_node->range_expr = $5;
        for_node->body = $6;
        ast_node_free($2);
        $$ = (ASTNode*)for_node;
    }
    | FOR identifier COMMA identifier SHORT_ASSIGN RANGE expression block {
        // `for k, v := range expr { … }` — key+value form.
        ForStmtNode* for_node = (ForStmtNode*)calloc(1, sizeof(ForStmtNode));
        for_node->base.type = AST_FOR_STMT;
        for_node->base.pos = get_current_position();
        IdentifierNode* kid = (IdentifierNode*)$2;
        IdentifierNode* vid = (IdentifierNode*)$4;
        for_node->key_name = strdup(kid->name);
        for_node->value_name = strdup(vid->name);
        for_node->range_expr = $7;
        for_node->body = $8;
        ast_node_free($2);
        ast_node_free($4);
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
    | RETURN expression COMMA expression_list {
        // Multi-value return: `return a, b`. The first expression
        // becomes the head of values; the rest chain via `next`.
        // The codegen detects 2+ values and builds a struct via
        // insertvalue.
        ReturnStmtNode* ret = (ReturnStmtNode*)malloc(sizeof(ReturnStmtNode));
        ret->base.type = AST_RETURN_STMT;
        ret->base.pos = get_current_position();
        ret->base.node_type = NULL;
        ret->base.next = NULL;
        ast_add_child($2, $4);
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
    /* SELECT pushes a SELECT_COND frame; the bridge emits LBRACE_BODY for
       the immediately-following `{`. Both forms accepted so a bridge bug
       doesn't break select parsing. */
    | SELECT LBRACE_BODY select_case_list RBRACE {
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

switch_stmt:
    /* SWITCH pushes a cond frame in the lexer bridge, so the `{` arrives as
       LBRACE_BODY. The plain-LBRACE form is accepted as a fallback so a
       bridge edge case can't break switch parsing (mirrors select_stmt). */
    SWITCH expression LBRACE_BODY case_clause_list RBRACE {
        $$ = (ASTNode*)ast_switch_stmt_new($2, $4, get_current_position());
    }
    | SWITCH expression LBRACE case_clause_list RBRACE {
        $$ = (ASTNode*)ast_switch_stmt_new($2, $4, get_current_position());
    }
    ;

case_clause_list:
    case_clause {
        $$ = $1;
    }
    | case_clause_list case_clause {
        ASTNode* current = $1;
        while (current->next) {
            current = current->next;
        }
        current->next = $2;
        $$ = $1;
    }
    ;

case_clause:
    CASE expression COLON statement_list {
        $$ = (ASTNode*)ast_case_clause_new($2, $4, get_current_position());
    }
    | DEFAULT COLON statement_list {
        $$ = (ASTNode*)ast_case_clause_new(NULL, $3, get_current_position());
    }
    ;

// Expressions
expression:
    unary_expr { $$ = $1; }
    | postfix_expr { $$ = $1; }
    | binary_expr { $$ = $1; }
    | try_expr { $$ = $1; }      // Goo extension
    | catch_expr { $$ = $1; }    // Goo extension
    | match_expr { $$ = $1; }    // Goo extension
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
    | primary_expr INCREMENT {
        PostfixExprNode* postfix = ast_postfix_expr_new($1, bison_token_to_token_type(INCREMENT), get_current_position());
        $$ = (ASTNode*)postfix;
    }
    | primary_expr DECREMENT {
        PostfixExprNode* postfix = ast_postfix_expr_new($1, bison_token_to_token_type(DECREMENT), get_current_position());
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
    | slice_lit { $$ = $1; }
    | map_lit { $$ = $1; }
    | struct_lit { $$ = $1; }
    /* All GPU constructs deliberately disabled in primary_expr.
       kernel_launch (identifier LT LT LT … GT GT GT (…)) was removed
       because bison can't disambiguate `i < 10` from the start of a
       kernel launch on lookahead, so every `if i < N { … }` failed
       to parse. gpu_memory_alloc/copy/sync/intrinsic were each
       overly-broad ident.ident shapes that hijacked package method
       calls (fmt.Println parsed as a CUDA intrinsic). GPU constructs
       need a syntactic context marker (e.g., inside a `kernel { … }`
       block) before they can re-enter primary_expr. */
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
    | unsafe_ptr_type { $$ = $1; }    // Goo extension
    | error_union_type { $$ = $1; }   // Goo extension
    | nullable_type { $$ = $1; }      // Goo extension
    | struct_type { $$ = $1; }
    | enum_type { $$ = $1; }
    ;

struct_type:
    STRUCT LBRACE struct_field_list RBRACE {
        StructTypeNode* st = (StructTypeNode*)malloc(sizeof(StructTypeNode));
        st->base.type = AST_STRUCT_TYPE;
        st->base.pos = get_current_position();
        st->base.node_type = NULL;
        st->base.next = NULL;
        st->fields = $3;
        $$ = (ASTNode*)st;
    }
    | STRUCT LBRACE RBRACE {
        StructTypeNode* st = (StructTypeNode*)malloc(sizeof(StructTypeNode));
        st->base.type = AST_STRUCT_TYPE;
        st->base.pos = get_current_position();
        st->base.node_type = NULL;
        st->base.next = NULL;
        st->fields = NULL;
        $$ = (ASTNode*)st;
    }
    ;

enum_type:
    ENUM LBRACE enum_variant_list RBRACE {
        $$ = (ASTNode*)ast_enum_type_new($3, get_current_position());
    }
    | ENUM LBRACE RBRACE {
        $$ = (ASTNode*)ast_enum_type_new(NULL, get_current_position());
    }
    ;

enum_variant_list:
    enum_variant { $$ = $1; }
    | enum_variant_list enum_variant {
        ast_add_child($1, $2);
        $$ = $1;
    }
    ;

enum_variant:
    identifier LBRACE struct_field_list RBRACE {
        IdentifierNode* ident = (IdentifierNode*)$1;
        $$ = (ASTNode*)ast_enum_variant_new(ident->name, $3, get_current_position());
        ast_node_free($1);
    }
    | identifier LBRACE RBRACE {
        IdentifierNode* ident = (IdentifierNode*)$1;
        $$ = (ASTNode*)ast_enum_variant_new(ident->name, NULL, get_current_position());
        ast_node_free($1);
    }
    ;

struct_field_list:
    struct_field { $$ = $1; }
    | struct_field_list struct_field {
        ast_add_child($1, $2);
        $$ = $1;
    }
    | struct_field_list COMMA struct_field {
        // Comma-separated field syntax: `w: int, h: int`
        ast_add_child($1, $3);
        $$ = $1;
    }
    ;

struct_field:
    identifier type {
        // Reuse VarDeclNode for fields — same shape as a function
        // parameter. type_from_ast for AST_STRUCT_TYPE will walk
        // this chain and build the Type's struct_type.fields[].
        IdentifierNode* ident = (IdentifierNode*)$1;
        VarDeclNode* field = ast_var_decl_new(get_current_position());
        field->names = malloc(sizeof(char*));
        field->names[0] = strdup(ident->name);
        field->name_count = 1;
        field->type = $2;
        field->values = NULL;
        ast_node_free($1);
        $$ = (ASTNode*)field;
    }
    | identifier COLON type {
        // Colon-separated field syntax: `name: type` (Goo extension).
        // Used in enum variant bodies: `Circle{radius: int}`.
        IdentifierNode* ident = (IdentifierNode*)$1;
        VarDeclNode* field = ast_var_decl_new(get_current_position());
        field->names = malloc(sizeof(char*));
        field->names[0] = strdup(ident->name);
        field->name_count = 1;
        field->type = $3;
        field->values = NULL;
        ast_node_free($1);
        $$ = (ASTNode*)field;
    }
    | identifier type SEMICOLON {
        IdentifierNode* ident = (IdentifierNode*)$1;
        VarDeclNode* field = ast_var_decl_new(get_current_position());
        field->names = malloc(sizeof(char*));
        field->names[0] = strdup(ident->name);
        field->name_count = 1;
        field->type = $2;
        field->values = NULL;
        ast_node_free($1);
        $$ = (ASTNode*)field;
    }
    | identifier COLON type SEMICOLON {
        // Colon-separated field with semicolon terminator.
        IdentifierNode* ident = (IdentifierNode*)$1;
        VarDeclNode* field = ast_var_decl_new(get_current_position());
        field->names = malloc(sizeof(char*));
        field->names[0] = strdup(ident->name);
        field->name_count = 1;
        field->type = $3;
        field->values = NULL;
        ast_node_free($1);
        $$ = (ASTNode*)field;
    }
    ;

// map[K]V{k: v, …} — tagged AST_PAREN_EXPR (unused enum slot).
// keys and values are stored as two parallel `next`-chained lists.
map_lit:
    map_type LBRACE map_entry_list RBRACE {
        MapLitNode* lit = (MapLitNode*)malloc(sizeof(MapLitNode));
        lit->base.type = AST_PAREN_EXPR;
        lit->base.pos = get_current_position();
        lit->base.node_type = NULL;
        lit->base.next = NULL;
        lit->map_type = $1;
        lit->keys = $3;  // map_entry_list returns the keys head
        lit->values = (ASTNode*)((ASTNode*)$3)->node_type;  // values stashed on key.node_type
        // node_type field repurposed as a side-channel for the
        // parallel values list head; cleared after extraction so
        // type_check / codegen don't see it.
        ((ASTNode*)$3)->node_type = NULL;
        $$ = (ASTNode*)lit;
    }
    | map_type LBRACE RBRACE {
        MapLitNode* lit = (MapLitNode*)malloc(sizeof(MapLitNode));
        lit->base.type = AST_PAREN_EXPR;
        lit->base.pos = get_current_position();
        lit->base.node_type = NULL;
        lit->base.next = NULL;
        lit->map_type = $1;
        lit->keys = NULL;
        lit->values = NULL;
        $$ = (ASTNode*)lit;
    }
    ;

/* M10 struct literal: `Point{x: 3, y: 4}` (keyed) or `Point{3, 4}` (positional).
   The IDENT.LBRACE shape conflicts with `if X { body }`; resolved at the lexer
   level — the bridge emits LBRACE_BODY for cond-body braces, so struct_lit's
   plain LBRACE is only matched outside cond contexts (or inside parens).
   See docs/M10_GRAMMAR_DECISION.md. Type-check + codegen ship as a separate
   M10-struct-literal-impl child. */
struct_lit:
    identifier LBRACE struct_lit_inits RBRACE {
        IdentifierNode* type_ident = (IdentifierNode*)$1;
        StructLiteralNode* lit = (StructLiteralNode*)calloc(1, sizeof(StructLiteralNode));
        lit->base.type = AST_STRUCT_LITERAL;
        lit->base.pos = get_current_position();
        lit->type_name = strdup(type_ident->name);
        ast_node_free($1);
        ASTNode* head = $3;
        lit->field_values = head;
        lit->field_count = 0;
        for (ASTNode* a = head; a; a = a->next) lit->field_count++;
        /* Collect the field names struct_lit_init stashed on each init's
           node_type (map_entry_list precedent). NULL entry = positional
           init. is_keyed = any name present; a keyed literal with NULL
           holes is the mixed form, rejected at type-check. */
        lit->is_keyed = 0;
        lit->field_names = NULL;
        if (lit->field_count > 0) {
            lit->field_names = calloc(lit->field_count, sizeof(char*));
            size_t i = 0;
            for (ASTNode* a = head; a; a = a->next, i++) {
                lit->field_names[i] = (char*)a->node_type;
                a->node_type = NULL;
                if (lit->field_names[i]) lit->is_keyed = 1;
            }
            if (!lit->is_keyed) {
                free(lit->field_names);
                lit->field_names = NULL;
            }
        }
        $$ = (ASTNode*)lit;
    }
    | identifier LBRACE RBRACE {
        IdentifierNode* type_ident = (IdentifierNode*)$1;
        StructLiteralNode* lit = (StructLiteralNode*)calloc(1, sizeof(StructLiteralNode));
        lit->base.type = AST_STRUCT_LITERAL;
        lit->base.pos = get_current_position();
        lit->type_name = strdup(type_ident->name);
        ast_node_free($1);
        lit->is_keyed = 0;
        lit->field_values = NULL;
        lit->field_names = NULL;
        lit->field_count = 0;
        $$ = (ASTNode*)lit;
    }
    ;

struct_lit_inits:
    struct_lit_init { $$ = $1; }
    | struct_lit_inits COMMA struct_lit_init {
        ast_add_child($1, $3);
        $$ = $1;
    }
    ;

struct_lit_init:
    expression {
        /* Positional init. node_type=NULL signals "no field name." */
        $$ = $1;
        $$->node_type = NULL;
    }
    | identifier COLON expression {
        /* Keyed init. Stash the owned field-name string on the init's
           node_type slot (same parse-time piggyback map_entry_list uses
           for its values chain); struct_lit's reducer moves it into
           field_names[] and clears the slot before type-check runs. */
        IdentifierNode* key = (IdentifierNode*)$1;
        $$ = $3;
        $$->node_type = (Type*)strdup(key->name);
        ast_node_free($1);
    }
    ;

map_entry_list:
    map_entry { $$ = $1; }
    | map_entry_list COMMA map_entry {
        // Append key to keys-chain; append value to values-chain
        // (the values-chain head is stashed on the keys-list-head's
        // node_type field so we can recover both from one stack
        // value).
        ASTNode* keys_head = $1;
        ASTNode* values_head = (ASTNode*)keys_head->node_type;
        ASTNode* new_key = $3;
        ASTNode* new_val = (ASTNode*)new_key->node_type;
        new_key->node_type = NULL;
        ast_add_child(keys_head, new_key);
        ast_add_child(values_head, new_val);
        $$ = keys_head;
    }
    ;

map_entry:
    expression COLON expression {
        // The KEY node is returned. The matching VALUE is stashed
        // on key->node_type as a side-channel; map_entry_list
        // extracts and re-chains it into a parallel values list.
        ASTNode* k = $1;
        k->node_type = (Type*)$3;
        $$ = k;
    }
    ;

slice_lit:
    LBRACKET expression_list RBRACKET {
        // `[1, 2, 3]` — slice literal. Tagged AST_SLICE_EXPR because
        // that enum slot was unused for actual slicing expressions
        // (goolang doesn't parse `arr[i:j]` today). The expression
        // list head is stored in `elements` via the same struct
        // layout (SliceLitNode shares ASTNode base).
        SliceLitNode* lit = (SliceLitNode*)malloc(sizeof(SliceLitNode));
        lit->base.type = AST_SLICE_EXPR;
        lit->base.pos = get_current_position();
        lit->base.node_type = NULL;
        lit->base.next = NULL;
        lit->elements = $2;
        $$ = (ASTNode*)lit;
    }
    | LBRACKET RBRACKET {
        SliceLitNode* lit = (SliceLitNode*)malloc(sizeof(SliceLitNode));
        lit->base.type = AST_SLICE_EXPR;
        lit->base.pos = get_current_position();
        lit->base.node_type = NULL;
        lit->base.next = NULL;
        lit->elements = NULL;
        $$ = (ASTNode*)lit;
    }
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

unsafe_ptr_type:
    UNSAFE DOT identifier {
        // This matches "unsafe.Pointer" syntax (no multiply for now to avoid conflicts)
        IdentifierNode* ident = (IdentifierNode*)$3;
        if (strcmp(ident->name, "Pointer") == 0) {
            UnsafePtrTypeNode* unsafe_ptr = ast_unsafe_ptr_type_new(NULL, get_current_position());
            ast_node_free($3);
            $$ = (ASTNode*)unsafe_ptr;
        } else {
            yyerror("Expected 'Pointer' after 'unsafe.'");
            ast_node_free($3);
            $$ = NULL;
        }
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

unsafe_stmt:
    UNSAFE block {
        UnsafeStmtNode* unsafe_node = ast_unsafe_stmt_new($2, get_current_position());
        $$ = (ASTNode*)unsafe_node;
    }
    ;

asm_stmt:
    ASM LBRACE STRING_LITERAL RBRACE {
        AsmStmtNode* asm_node = ast_asm_stmt_new($3, get_current_position());
        free($3);
        $$ = (ASTNode*)asm_node;
    }
    ;

ownership_qualifier:
    OWNED { $$ = NULL; /* TODO: Create ownership node */ }
    | BORROWED { $$ = NULL; }
    | SHARED { $$ = NULL; }
    ;

extern_decl:
    EXTERN STRING_LITERAL identifier func_signature {
        // extern "C" function_name(params) -> return_type
        IdentifierNode* ident = (IdentifierNode*)$3;
        ExternDeclNode* extern_node = ast_extern_decl_new(ident->name, $2, $4, NULL, NULL, get_current_position());
        free($2);
        ast_node_free($3);
        $$ = (ASTNode*)extern_node;
    }
    | EXTERN STRING_LITERAL identifier func_signature FROM STRING_LITERAL {
        // extern "C" function_name(params) -> return_type from "library"
        IdentifierNode* ident = (IdentifierNode*)$3;
        ExternDeclNode* extern_node = ast_extern_decl_new(ident->name, $2, $4, NULL, $6, get_current_position());
        free($2);
        free($6);
        ast_node_free($3);
        $$ = (ASTNode*)extern_node;
    }
    ;

attribute:
    DEREF identifier {
        // @attribute_name
        IdentifierNode* ident = (IdentifierNode*)$2;
        AttributeNode* attr = ast_attribute_new(ident->name, NULL, get_current_position());
        ast_node_free($2);
        $$ = (ASTNode*)attr;
    }
    | DEREF identifier LPAREN expression_list RPAREN {
        // @attribute_name(args)
        IdentifierNode* ident = (IdentifierNode*)$2;
        AttributeNode* attr = ast_attribute_new(ident->name, $4, get_current_position());
        ast_node_free($2);
        $$ = (ASTNode*)attr;
    }
    ;

attribute_list:
    attribute {
        $$ = $1;
    }
    | attribute_list attribute {
        ast_add_child($1, $2);
        $$ = $1;
    }
    ;

volatile_expr:
    VOLATILE expression {
        VolatileExprNode* volatile_node = ast_volatile_expr_new($2, get_current_position());
        $$ = (ASTNode*)volatile_node;
    }
    ;

parallel_for_stmt:
    PARALLEL FOR identifier SHORT_ASSIGN expression SEMICOLON expression SEMICOLON expression block {
        // parallel for i := 0; i < n; i++ { ... }
        IdentifierNode* ident = (IdentifierNode*)$3;
        VarDeclNode* init = ast_var_decl_new(get_current_position());
        init->names = malloc(sizeof(char*));
        init->names[0] = strdup(ident->name);
        init->name_count = 1;
        init->values = $5;
        init->is_short_decl = 1;
        
        ParallelForNode* parallel_for = ast_parallel_for_new((ASTNode*)init, $7, $9, $10, "dynamic", 0, get_current_position());
        ast_node_free($3);
        $$ = (ASTNode*)parallel_for;
    }
    ;

parallel_reduce_expr:
    PARALLEL REDUCE LPAREN expression COMMA expression COMMA expression RPAREN {
        // parallel reduce(array, init_value, reduce_func)
        ParallelReduceNode* reduce_node = ast_parallel_reduce_new($4, $6, $8, "custom", get_current_position());
        $$ = (ASTNode*)reduce_node;
    }
    ;

barrier_call:
    BARRIER LPAREN RPAREN {
        // barrier()
        BarrierCallNode* barrier_node = ast_barrier_call_new(NULL, get_current_position());
        $$ = (ASTNode*)barrier_node;
    }
    | BARRIER LPAREN STRING_LITERAL RPAREN {
        // barrier("barrier_name")
        BarrierCallNode* barrier_node = ast_barrier_call_new($3, get_current_position());
        free($3);
        $$ = (ASTNode*)barrier_node;
    }
    ;

atomic_expr:
    ATOMIC DOT identifier LPAREN expression RPAREN {
        // atomic.Add(expr)
        IdentifierNode* op_ident = (IdentifierNode*)$3;
        AtomicExprNode* atomic_node = ast_atomic_expr_new($5, op_ident->name, NULL, get_current_position());
        ast_node_free($3);
        $$ = (ASTNode*)atomic_node;
    }
    | ATOMIC DOT identifier LPAREN expression COMMA expression RPAREN {
        // atomic.CompareAndSwap(expr, operand)
        IdentifierNode* op_ident = (IdentifierNode*)$3;
        AtomicExprNode* atomic_node = ast_atomic_expr_new($5, op_ident->name, $7, get_current_position());
        ast_node_free($3);
        $$ = (ASTNode*)atomic_node;
    }
    ;

thread_local_decl:
    THREAD_LOCAL VAR identifier type {
        // threadLocal var name type
        IdentifierNode* ident = (IdentifierNode*)$3;
        ThreadLocalDeclNode* thread_local = ast_thread_local_decl_new(ident->name, $4, NULL, get_current_position());
        ast_node_free($3);
        $$ = (ASTNode*)thread_local;
    }
    | THREAD_LOCAL VAR identifier type ASSIGN expression {
        // threadLocal var name type = init_value
        IdentifierNode* ident = (IdentifierNode*)$3;
        ThreadLocalDeclNode* thread_local = ast_thread_local_decl_new(ident->name, $4, $6, get_current_position());
        ast_node_free($3);
        $$ = (ASTNode*)thread_local;
    }
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

// GPU Programming Support

// Kernel function declaration
kernel_decl:
    KERNEL identifier func_signature block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        KernelDeclNode* kernel = ast_kernel_decl_new(ident->name, $3, NULL, $4, GPU_TARGET_NVPTX, get_current_position());
        ast_node_free($2);
        $$ = (ASTNode*)kernel;
    }
    | DEVICE KERNEL identifier func_signature block {
        IdentifierNode* ident = (IdentifierNode*)$3;
        KernelDeclNode* kernel = ast_kernel_decl_new(ident->name, $4, NULL, $5, GPU_TARGET_NVPTX, get_current_position());
        ast_node_free($3);
        $$ = (ASTNode*)kernel;
    }
    ;

// Kernel launch expression  
kernel_launch:
    identifier LT LT LT expression COMMA expression GT GT GT LPAREN RPAREN {
        // vectorAdd<<<gridSize, blockSize>>>()
        IdentifierNode* kernel_name = (IdentifierNode*)$1;
        KernelLaunchNode* launch = ast_kernel_launch_new($1, $5, $7, NULL, get_current_position());
        $$ = (ASTNode*)launch;
    }
    | identifier LT LT LT expression COMMA expression GT GT GT LPAREN expression_list RPAREN {
        // vectorAdd<<<gridSize, blockSize>>>(args)
        IdentifierNode* kernel_name = (IdentifierNode*)$1;
        KernelLaunchNode* launch = ast_kernel_launch_new($1, $5, $7, $12, get_current_position());
        $$ = (ASTNode*)launch;
    }
    ;

// GPU memory allocation
gpu_memory_alloc:
    identifier DOT identifier LBRACKET type RBRACKET LPAREN expression RPAREN {
        // cuda.Malloc[float32](size)
        IdentifierNode* package = (IdentifierNode*)$1;
        IdentifierNode* func = (IdentifierNode*)$3;
        if (strcmp(package->name, "cuda") == 0 && strcmp(func->name, "Malloc") == 0) {
            GPUMemoryAllocNode* alloc = ast_gpu_memory_alloc_new($8, $5, GPU_MEMORY_GLOBAL, get_current_position());
            ast_node_free($1);
            ast_node_free($3);
            $$ = (ASTNode*)alloc;
        } else {
            yyerror("Unknown GPU memory allocation function");
            $$ = NULL;
        }
    }
    ;

// GPU memory copy
gpu_memory_copy:
    identifier DOT identifier LPAREN expression COMMA expression COMMA expression COMMA identifier RPAREN {
        // cuda.Memcpy(dest, src, size, direction)
        IdentifierNode* package = (IdentifierNode*)$1;
        IdentifierNode* func = (IdentifierNode*)$3;
        IdentifierNode* direction = (IdentifierNode*)$11;
        
        int dir = 0; // Default to HostToDevice
        if (strcmp(direction->name, "HostToDevice") == 0) dir = 0;
        else if (strcmp(direction->name, "DeviceToHost") == 0) dir = 1;
        else if (strcmp(direction->name, "DeviceToDevice") == 0) dir = 2;
        
        if (strcmp(package->name, "cuda") == 0 && strcmp(func->name, "Memcpy") == 0) {
            GPUMemoryCopyNode* copy = ast_gpu_memory_copy_new($5, $7, $9, dir, get_current_position());
            ast_node_free($1);
            ast_node_free($3);
            ast_node_free($11);
            $$ = (ASTNode*)copy;
        } else {
            yyerror("Unknown GPU memory copy function");
            $$ = NULL;
        }
    }
    ;

// GPU synchronization
gpu_sync:
    identifier DOT identifier LPAREN RPAREN {
        // cuda.DeviceSync()
        IdentifierNode* package = (IdentifierNode*)$1;
        IdentifierNode* func = (IdentifierNode*)$3;
        
        int sync_type = 0; // DeviceSync
        if (strcmp(func->name, "DeviceSync") == 0) sync_type = 0;
        else if (strcmp(func->name, "StreamSync") == 0) sync_type = 1;
        
        if (strcmp(package->name, "cuda") == 0) {
            GPUSyncNode* sync = ast_gpu_sync_new(sync_type, NULL, NULL, get_current_position());
            ast_node_free($1);
            ast_node_free($3);
            $$ = (ASTNode*)sync;
        } else {
            yyerror("Unknown GPU sync function");
            $$ = NULL;
        }
    }
    ;

// GPU intrinsic functions
gpu_intrinsic:
    identifier DOT identifier {
        // blockIdx.x, threadIdx.y, etc.
        IdentifierNode* object = (IdentifierNode*)$1;
        IdentifierNode* member = (IdentifierNode*)$3;
        
        char intrinsic_name[64];
        snprintf(intrinsic_name, sizeof(intrinsic_name), "%s.%s", object->name, member->name);
        
        GPUIntrinsicNode* intrinsic = ast_gpu_intrinsic_new(intrinsic_name, NULL, GPU_CONTEXT_KERNEL, get_current_position());
        ast_node_free($1);
        ast_node_free($3);
        $$ = (ASTNode*)intrinsic;
    }
    ;

// GPU memory qualifiers
gpu_memory_qualifier:
    GLOBAL { $$ = (int)GPU_MEMORY_GLOBAL; }
    | SHARED_MEM { $$ = (int)GPU_MEMORY_SHARED; }
    | CONSTANT { $$ = (int)GPU_MEMORY_CONSTANT; }
    | LOCAL { $$ = (int)GPU_MEMORY_LOCAL; }
    ;

// Pattern matching
match_expr:
    MATCH expression LBRACE match_case_list RBRACE {
        MatchExprNode* match_node = ast_match_expr_new($2, $4, get_current_position());
        $$ = (ASTNode*)match_node;
    }
    /* MATCH pushes a MATCH_COND frame so `match Point{x:1} { ... }` works:
       the inner `{` of the struct_lit gets plain LBRACE (paren_depth check
       in the bridge), the outer match body `{` gets LBRACE_BODY. */
    | MATCH expression LBRACE_BODY match_case_list RBRACE {
        MatchExprNode* match_node = ast_match_expr_new($2, $4, get_current_position());
        $$ = (ASTNode*)match_node;
    }
    ;

match_case_list:
    match_case {
        $$ = $1;
    }
    | match_case_list match_case {
        ast_add_child($1, $2);
        $$ = $1;
    }
    ;

match_case:
    CASE pattern COLON statement_list {
        MatchCaseNode* case_node = ast_match_case_new($2, NULL, $4, get_current_position());
        $$ = (ASTNode*)case_node;
    }
    | CASE pattern guard_condition COLON statement_list {
        MatchCaseNode* case_node = ast_match_case_new($2, $3, $5, get_current_position());
        $$ = (ASTNode*)case_node;
    }
    | DEFAULT COLON statement_list {
        // Default case is represented as a wildcard pattern
        PatternNode* wildcard = ast_pattern_new(PATTERN_WILDCARD, get_current_position());
        MatchCaseNode* case_node = ast_match_case_new((ASTNode*)wildcard, NULL, $3, get_current_position());
        $$ = (ASTNode*)case_node;
    }
    ;

pattern:
    literal {
        // Literal pattern (e.g., 42, "hello", true)
        PatternNode* pattern_node = ast_pattern_new(PATTERN_LITERAL, get_current_position());
        pattern_node->data.literal.literal = $1;
        $$ = (ASTNode*)pattern_node;
    }
    | identifier {
        // Variable binding pattern (e.g., x, name) or wildcard pattern (_)
        IdentifierNode* ident = (IdentifierNode*)$1;
        if (strcmp(ident->name, "_") == 0) {
            // Wildcard pattern
            PatternNode* pattern_node = ast_pattern_new(PATTERN_WILDCARD, get_current_position());
            ast_node_free($1);
            $$ = (ASTNode*)pattern_node;
        } else {
            // Variable binding pattern
            PatternNode* pattern_node = ast_pattern_new(PATTERN_IDENTIFIER, get_current_position());
            pattern_node->data.identifier.name = strdup(ident->name);
            pattern_node->data.identifier.type = NULL;
            ast_node_free($1);
            $$ = (ASTNode*)pattern_node;
        }
    }
    | identifier COLON type {
        // Typed variable binding pattern (e.g., err: Error)
        IdentifierNode* ident = (IdentifierNode*)$1;
        PatternNode* pattern_node = ast_pattern_new(PATTERN_IDENTIFIER, get_current_position());
        pattern_node->data.identifier.name = strdup(ident->name);
        pattern_node->data.identifier.type = $3;
        ast_node_free($1);
        $$ = (ASTNode*)pattern_node;
    }
    | identifier LBRACE expression_list RBRACE {
        // Destructuring pattern (e.g., Person{Name: name, Age: age})
        IdentifierNode* ident = (IdentifierNode*)$1;
        PatternNode* pattern_node = ast_pattern_new(PATTERN_DESTRUCTURE, get_current_position());
        pattern_node->data.destructure.type_name = strdup(ident->name);
        pattern_node->data.destructure.fields = $3;
        ast_node_free($1);
        $$ = (ASTNode*)pattern_node;
    }
    ;

guard_condition:
    IF expression {
        GuardConditionNode* guard = ast_guard_condition_new($2, get_current_position());
        $$ = (ASTNode*)guard;
    }
    ;

// WebAssembly Support

wasm_export:
    EXPORT STRING_LITERAL identifier {
        // export "functionName" myFunction
        IdentifierNode* item = (IdentifierNode*)$3;
        WasmExportNode* export_node = ast_wasm_export_new($2, $3, "func", get_current_position());
        free($2);
        $$ = (ASTNode*)export_node;
    }
    ;

wasm_import:
    IMPORT STRING_LITERAL STRING_LITERAL identifier {
        // import "module" "function" localName
        IdentifierNode* local = (IdentifierNode*)$4;
        WasmImportNode* import_node = ast_wasm_import_new($2, $3, local->name, "func", NULL, get_current_position());
        free($2);
        free($3);
        ast_node_free($4);
        $$ = (ASTNode*)import_node;
    }
    ;

wasm_memory:
    MEMORY expression {
        // memory 1 (1 page = 64KB)
        WasmMemoryNode* memory_node = ast_wasm_memory_new($2, NULL, 0, get_current_position());
        $$ = (ASTNode*)memory_node;
    }
    | MEMORY expression expression {
        // memory 1 16 (min 1 page, max 16 pages)
        WasmMemoryNode* memory_node = ast_wasm_memory_new($2, $3, 0, get_current_position());
        $$ = (ASTNode*)memory_node;
    }
    ;

wasm_table:
    TABLE expression wasm_value_type {
        // table 10 funcref
        WasmTableNode* table_node = ast_wasm_table_new((WasmValueType)$3, $2, NULL, get_current_position());
        $$ = (ASTNode*)table_node;
    }
    ;

wasm_global:
    GLOBAL identifier wasm_value_type expression {
        // global myGlobal i32 42
        IdentifierNode* name = (IdentifierNode*)$2;
        WasmGlobalNode* global_node = ast_wasm_global_new(name->name, (WasmValueType)$3, 0, $4, get_current_position());
        ast_node_free($2);
        $$ = (ASTNode*)global_node;
    }
    ;

wasm_start:
    START identifier {
        // start main
        WasmStartNode* start_node = ast_wasm_start_new($2, get_current_position());
        $$ = (ASTNode*)start_node;
    }
    ;

js_interop:
    identifier DOT identifier LPAREN expression_list RPAREN {
        // console.log(args) - JavaScript interop call
        IdentifierNode* obj = (IdentifierNode*)$1;
        IdentifierNode* method = (IdentifierNode*)$3;
        
        if (strcmp(obj->name, "console") == 0 || strcmp(obj->name, "window") == 0 || 
            strcmp(obj->name, "document") == 0) {
            JSInteropNode* js_node = ast_js_interop_new(JS_INTEROP_CALL, obj->name, method->name, $5, WASM_ENV_BROWSER, get_current_position());
            ast_node_free($1);
            ast_node_free($3);
            $$ = (ASTNode*)js_node;
        } else {
            // Regular selector expression
            yyerror("Unknown JavaScript object");
            $$ = NULL;
        }
    }
    ;

dom_access:
    identifier DOT identifier {
        // document.body - DOM property access
        IdentifierNode* api = (IdentifierNode*)$1;
        IdentifierNode* prop = (IdentifierNode*)$3;
        
        if (strcmp(api->name, "document") == 0 || strcmp(api->name, "window") == 0) {
            DOMAccessNode* dom_node = ast_dom_access_new(api->name, prop->name, NULL, 1, get_current_position());
            ast_node_free($1);
            ast_node_free($3);
            $$ = (ASTNode*)dom_node;
        } else {
            yyerror("Unknown DOM API");
            $$ = NULL;
        }
    }
    ;

wasm_value_type:
    identifier {
        // i32, i64, f32, f64, funcref, externref
        IdentifierNode* type_name = (IdentifierNode*)$1;
        if (strcmp(type_name->name, "i32") == 0) {
            $$ = (int)WASM_TYPE_I32;
        } else if (strcmp(type_name->name, "i64") == 0) {
            $$ = (int)WASM_TYPE_I64;
        } else if (strcmp(type_name->name, "f32") == 0) {
            $$ = (int)WASM_TYPE_F32;
        } else if (strcmp(type_name->name, "f64") == 0) {
            $$ = (int)WASM_TYPE_F64;
        } else if (strcmp(type_name->name, "funcref") == 0) {
            $$ = (int)WASM_TYPE_FUNCREF;
        } else if (strcmp(type_name->name, "externref") == 0) {
            $$ = (int)WASM_TYPE_EXTERNREF;
        } else {
            yyerror("Unknown WebAssembly value type");
            $$ = (int)WASM_TYPE_I32; // Default
        }
        ast_node_free($1);
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
        case INCREMENT: return TOKEN_INCREMENT;
        case DECREMENT: return TOKEN_DECREMENT;
        default: return TOKEN_UNKNOWN;
    }
}