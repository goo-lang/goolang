%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "token.h"
#include "lexer.h"
#include "parser/parser_actions.h"

// External lexer interface
extern int yylex(void);
extern void yyerror(const char* msg);
extern Lexer* current_lexer;

// ast_root, g_func_signature_params/result (globals) and every semantic-
// action helper below except make_embedded_field/make_grouped_field (kept
// here — used only from the struct_field rule) now live in
// src/parser/parser_actions.c; see include/parser/parser_actions.h for their
// declarations, which this file includes above.

// Struct embedding: build the VarDeclNode for an anonymous field `Name` /
// `*Name` / `pkg.Name` / `*pkg.Name` (review parity: qualified embeds). The
// field is stored under the type's UNQUALIFIED name (Go's rule — `sync.
// Mutex` embeds as field `Mutex`), with is_embedded set; the type node
// matches what type_name / pointer_type reductions would have built,
// including the B1 package qualifier when `pkg_node` is non-NULL.
static ASTNode* make_embedded_field(ASTNode* pkg_node, ASTNode* ident_node,
                                    int is_pointer) {
    IdentifierNode* ident = (IdentifierNode*)ident_node;
    VarDeclNode* field = ast_var_decl_new(get_current_position());
    field->names = malloc(sizeof(char*));
    field->names[0] = strdup(ident->name);
    field->name_count = 1;
    BasicTypeNode* basic = (BasicTypeNode*)malloc(sizeof(BasicTypeNode));
    basic->base.type = AST_BASIC_TYPE;
    basic->base.pos = ident->base.pos;
    basic->base.node_type = NULL;
    basic->base.next = NULL;
    basic->name = strdup(ident->name);
    basic->package = pkg_node
        ? strdup(((IdentifierNode*)pkg_node)->name)
        : NULL;
    ASTNode* ty = (ASTNode*)basic;
    if (is_pointer) {
        PointerTypeNode* ptr = (PointerTypeNode*)malloc(sizeof(PointerTypeNode));
        ptr->base.type = AST_POINTER_TYPE;
        ptr->base.pos = ident->base.pos;
        ptr->base.node_type = NULL;
        ptr->base.next = NULL;
        ptr->element_type = ty;
        ty = (ASTNode*)ptr;
    }
    field->type = ty;
    field->values = NULL;
    field->is_embedded = 1;
    if (pkg_node) ast_node_free(pkg_node);
    ast_node_free(ident_node);
    return (ASTNode*)field;
}

// Grouped struct field `X, Y, Z T` — the names share the single trailing
// type T. `first` is the leading identifier; `tail` is a `next`-linked chain
// of the remaining identifiers (built by field_name_tail). Fold them into one
// multi-name VarDeclNode; type_from_ast (AST_STRUCT_TYPE) expands it to one
// StructField per name. Mirrors grouped params, which share a trailing type
// the same way. `tail` is freed whole here — ast_node_free chases ->next.
static ASTNode* make_grouped_field(ASTNode* first, ASTNode* tail, ASTNode* type) {
    int count = 1;
    for (ASTNode* n = tail; n; n = n->next) count++;
    VarDeclNode* field = ast_var_decl_new(get_current_position());
    field->names = malloc(sizeof(char*) * count);
    field->names[0] = strdup(((IdentifierNode*)first)->name);
    int i = 1;
    for (ASTNode* n = tail; n; n = n->next)
        field->names[i++] = strdup(((IdentifierNode*)n)->name);
    field->name_count = count;
    field->type = type;
    field->values = NULL;
    ast_node_free(first);
    ast_node_free(tail);
    return (ASTNode*)field;
}

%}

// Union type for semantic values
// NOTE: `integer` is `long long` (not `int`) so that integer literals larger
// than INT32_MAX (e.g. 9000000000 used by the int64-channel probe) survive the
// lexer→parser boundary without truncation.  The lexer sets this field via
// strtoll(); the literal action uses %lld.  This is a companion requirement of
// the M7 int64-channel codegen work and is safe for all existing source files
// whose literals are within [INT64_MIN, INT64_MAX].
%union {
    struct ASTNode* node;
    char* string;
    // String literals carry an explicit byte length so embedded NUL bytes
    // survive the lexer->parser boundary (strdup would truncate at the first
    // NUL). Only STRING_LITERAL uses this member; IDENTIFIER stays `string`.
    struct { char* data; size_t len; } strlit;
    long long integer;
    double real;
    int token;
    // Type assertions branch, Task 3: the type-switch guard (`x.(type)` /
    // `v := x.(type)`) has to carry TWO values (the optional bind identifier
    // and the operand expression) through one nonterminal reduction — a
    // second anonymous-struct union member alongside `strlit` above, same
    // technique, for the same reason (a single ASTNode* $$ can't hold both).
    struct { struct ASTNode* bind_name; struct ASTNode* expr; } guard;
}

// Token declarations with types
%token <string> IDENTIFIER
%token <strlit> STRING_LITERAL
%token <integer> INT_LITERAL
%token <real> FLOAT_LITERAL
%token <token> CHAR_LITERAL

// Go Keywords
%token BREAK CASE CHAN CONST CONTINUE DEFAULT DEFER ELSE FALLTHROUGH
%token FOR FUNC GO GOTO IF IMPORT INTERFACE MAP PACKAGE RANGE RETURN
%token SELECT STRUCT SWITCH TYPE VAR ENUM
%token TRUE FALSE NIL

// Goo Extension Keywords
%token COMPTIME CONCEPT PUB SUB REQ REP PUSH PULL TRY CATCH UNSAFE ASM ARENA
%token EXTERN FROM VOLATILE INLINE NO_STD
%token PARALLEL REDUCE BARRIER ATOMIC THREAD_LOCAL
%token OWNED BORROWED SHARED LET MATCH
// P5.10: the GPU (KERNEL DEVICE HOST GLOBAL SHARED_MEM CONSTANT LOCAL) and
// WebAssembly (WASM EXPORT MEMORY TABLE START ELEM DATA) token declarations
// were deleted along with their unreachable productions — lexer_bridge.c
// never mapped any of them, so no token stream could contain them.

// Operators and punctuation
%token PLUS MINUS MULTIPLY DIVIDE MODULO
%token ASSIGN SHORT_ASSIGN PLUS_ASSIGN MINUS_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN
%token AND_ASSIGN OR_ASSIGN XOR_ASSIGN LSHIFT_ASSIGN RSHIFT_ASSIGN
%token EQ NE LT LE GT GE
%token AND OR NOT
%token BIT_AND BIT_OR BIT_XOR BIT_NOT LSHIFT RSHIFT
%token AND_NOT  // &^  (Go bit-clear / and-not)
%token INCREMENT DECREMENT
%token ARROW

// Goo Extension Operators
%token BANG QUESTION TRY_OP CATCH_OP DEREF
%token FAT_ARROW  // =>  (value-yielding catch fallback: `f() catch => -1`, P2.9)

// Delimiters
%token LPAREN RPAREN LBRACE RBRACE LBRACKET RBRACKET
// LBRACE_BODY: context-sensitive variant emitted by the lexer bridge in place
// of LBRACE when the bridge's frame stack says "this `{` closes a cond/match/
// select context that's been pinned open by a preceding IF/FOR/MATCH/SELECT
// at depth 0." struct_lit's grammar accepts only LBRACE, so the IDENT.LBRACE
// shift/reduce ambiguity never arises at the cond/body boundary. See
// docs/M10_GRAMMAR_DECISION.md + docs/M10_LEXER_LAYER_PROBE.md.
%token LBRACE_BODY
// RBRACKET_SLICE: context-sensitive variant of RBRACKET emitted by the lexer
// bridge to close an EMPTY `[]` that is immediately followed by a type-starting
// token — i.e. the `[]` is a slice-TYPE prefix (`[]int`, `[]Foo{...}`), not a
// bare empty-slice literal. Using a distinct token keeps the empty-slice
// literal rule (`LBRACKET RBRACKET`) and the slice-type rule
// (`LBRACKET RBRACKET_SLICE type`) in separate parser states, so the
// reduce-vs-shift conflict at `LBRACKET RBRACKET .` (empty literal vs `[]T{...}`
// prefix) never arises. See lexer_bridge.c `[]`-disambiguation. (P3-1)
%token RBRACKET_SLICE
// Grammar maintained under the conflict-baseline discipline.
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
%type <node> const_spec_t const_spec_last const_spec_list_t const_spec_list
%type <node> var_spec var_spec_list var_member
%type <node> concept_body concept_requirement_list concept_requirement type_param_list type_param
%type <node> func_signature func_params func_param func_result
%type <node> statement_list stmt_seq statement final_stmt block simple_stmt
%type <node> if_stmt for_stmt return_stmt break_stmt continue_stmt goto_stmt fallthrough_stmt
%type <node> go_stmt select_stmt defer_stmt select_case_list select_case
%type <node> switch_stmt case_clause_list case_clause case_body
%type <node> type_case_list type_case_clause type_list
%type <guard> type_switch_guard
%type <node> unsafe_stmt asm_stmt parallel_for_stmt arena_stmt
%type <node> parallel_reduce_expr barrier_call atomic_expr thread_local_decl
%type <node> expression primary_expr unary_expr postfix_expr binary_expr
%type <node> call_expr index_expr selector_expr
%type <node> type type_name array_type slice_type map_type chan_type
%type <node> type_call_arg
%type <node> func_type pointer_type reference_type unsafe_ptr_type
%type <node> struct_type struct_field_list struct_field field_name_tail
%type <node> enum_type enum_variant_list enum_variant
%type <node> interface_type interface_method_list interface_member interface_method
%type <node> slice_lit
%type <node> map_lit map_entry_list map_entry
%type <node> struct_lit struct_lit_inits struct_lit_init
%type <node> composite_elem composite_elem_list composite_value
%type <node> identifier literal
%type <node> expression_list

// Goo Extensions
%type <node> error_union_type nullable_type try_expr catch_expr
%type <node> comptime_block ownership_qualifier if_let_stmt
%type <node> attribute attribute_list volatile_expr
%type <node> match_expr match_case_list match_case pattern guard_condition
%type <token> chan_pattern

// Operator precedence (lowest to highest)
%left COMMA
%right TRY CATCH  // Try/catch expressions (low precedence)
%right ASSIGN SHORT_ASSIGN PLUS_ASSIGN MINUS_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN AND_ASSIGN OR_ASSIGN XOR_ASSIGN LSHIFT_ASSIGN RSHIFT_ASSIGN
%right QUESTION COLON  // Ternary operator (if we add it)
// Binary operator precedence matches Go exactly (go.dev/ref/spec#Operator_precedence),
// lowest to highest. Getting this right is a hard requirement for compiling real
// Go source: e.g. `x & m << 8` is `(x & m) << 8` and `1<<32 - 1` is (1<<32)-1.
//
// ARROW sits BELOW all of Go's binary operators (looser than ||), so a channel
// send's RHS absorbs the full expression: `ch <- n + 22` sends n+22, not
// `(ch <- n) + 22`. Declared here (not at :199-ish, above PLUS/MULTIPLY) is
// deliberate — bison's rule precedence for `expression ARROW expression`
// defaults to ARROW's own level, and comparing that against a lookahead
// operator's level is what decides shift-vs-reduce at the send boundary. The
// unary receive prefix (`ARROW unary_expr`, <-ch) is unaffected by this
// position: its reduce is either a forced $default (no competing shift
// exists in that state) or a reduce/reduce tie broken by rule declaration
// order against `expression: unary_expr`, never by ARROW's precedence value
// (bison never uses precedence to resolve reduce/reduce). Verified via
// bison -v state inspection (see task-5 report) before moving this line.
%left ARROW                                   // <-   (send: loosest binary op — looser than ||)
%left OR                                      // ||   (prec 1)
%left AND                                     // &&   (prec 2)
%left EQ NE LT LE GT GE                       // == != < <= > >=  (prec 3)
%left PLUS MINUS BIT_OR BIT_XOR               // + - | ^  (prec 4)
%left MULTIPLY DIVIDE MODULO LSHIFT RSHIFT BIT_AND AND_NOT  // * / % << >> & &^  (prec 5)
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

// P5.10: generalized rule-1 ASI (lexer.c) terminates every value-ending
// line with a SEMICOLON, so each newline-separated construct below gains a
// member-attached trailing-SEMICOLON arm (the house pattern — see the
// var_member note further down for why list-level SEMICOLON arms were
// rejected: they conflict on "more list vs trailing terminator").
package_clause:
    PACKAGE identifier {
        IdentifierNode* ident = (IdentifierNode*)$2;
        PackageDeclNode* pkg = ast_package_decl_new(ident->name, ident->base.pos);
        ast_node_free($2);
        $$ = (ASTNode*)pkg;
    }
    | PACKAGE identifier SEMICOLON {
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
    | IMPORT LPAREN import_spec_list RPAREN SEMICOLON {
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
        ImportSpecNode* imp = ast_import_spec_new($1.data, NULL, get_current_position());
        free($1.data);
        $$ = (ASTNode*)imp;
    }
    | STRING_LITERAL SEMICOLON {
        ImportSpecNode* imp = ast_import_spec_new($1.data, NULL, get_current_position());
        free($1.data);
        $$ = (ASTNode*)imp;
    }
    | identifier STRING_LITERAL {
        IdentifierNode* ident = (IdentifierNode*)$1;
        ImportSpecNode* imp = ast_import_spec_new($2.data, ident->name, ident->base.pos);
        ast_node_free($1);
        free($2.data);
        $$ = (ASTNode*)imp;
    }
    | identifier STRING_LITERAL SEMICOLON {
        IdentifierNode* ident = (IdentifierNode*)$1;
        ImportSpecNode* imp = ast_import_spec_new($2.data, ident->name, ident->base.pos);
        ast_node_free($1);
        free($2.data);
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

// P5.10: the kernel_decl and wasm_* alternatives were deleted here. Their
// bison tokens (KERNEL, MEMORY, TABLE, START, EXPORT, ...) were never
// mapped by lexer_bridge.c, so every one of those arms was unreachable
// dead grammar — and wasm_memory's `MEMORY expression expression` (two
// ADJACENT expressions) was the sole source of the six 42-conflict
// reduce/reduce families (unary +,-,*,&,^,<- vs expression) that made up
// 252 of the baseline's 256 R/R conflicts. gpu_kernel's hard parse reject
// is pinned by gpu-kernel-reject-probe; real GPU/WASM grammar returns with
// their post-v1 phases (docs/2026-07-08-v1-roadmap.md Phases 6-7).
top_level_decl:
    declaration { $$ = $1; }
    | declaration SEMICOLON { $$ = $1; }
    | func_decl { $$ = $1; }
    | func_decl SEMICOLON { $$ = $1; }
    | concept_decl { $$ = $1; }
    | concept_decl SEMICOLON { $$ = $1; }
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
        reinterpret_grouped_names($4); // Go grouped params `(x, y int)`
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
        reinterpret_grouped_names($4); // Go grouped params `(x, y int)`
        func->params = $4;
        func->return_type = $6;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | FUNC identifier LBRACKET func_params RBRACKET LPAREN RPAREN block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        reinterpret_grouped_names($4);
        func->type_params = $4;
        func->body = $8;
        func->params = NULL;
        func->return_type = NULL;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | FUNC identifier LBRACKET func_params RBRACKET LPAREN func_params RPAREN block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        reinterpret_grouped_names($4);
        func->type_params = $4;
        reinterpret_grouped_names($7);
        func->params = $7;
        func->body = $9;
        func->return_type = NULL;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | FUNC identifier LBRACKET func_params RBRACKET LPAREN RPAREN func_result block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        reinterpret_grouped_names($4);
        func->type_params = $4;
        func->body = $9;
        func->params = NULL;
        func->return_type = $8;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | FUNC identifier LBRACKET func_params RBRACKET LPAREN func_params RPAREN func_result block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        reinterpret_grouped_names($4);
        func->type_params = $4;
        reinterpret_grouped_names($7);
        func->params = $7;
        func->body = $10;
        func->return_type = $9;
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
        reinterpret_grouped_names($7); // Go grouped params `(x, y int)`
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
        reinterpret_grouped_names($7); // Go grouped params `(x, y int)`
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
        g_func_signature_params = NULL;
        g_func_signature_result = NULL;
    }
    | LPAREN func_params RPAREN {
        reinterpret_grouped_names($2); // Go grouped params `(x, y int)`
        $$ = $2; // Parameters, no return type
        g_func_signature_params = $2;
        g_func_signature_result = NULL;
    }
    | LPAREN RPAREN func_result {
        $$ = $3; // No parameters, has return type
        g_func_signature_params = NULL;
        g_func_signature_result = $3;
    }
    | LPAREN func_params RPAREN func_result {
        reinterpret_grouped_names($2); // Go grouped params `(x, y int)`
        // TODO: still drops the result for func_signature's OTHER callers
        // (attribute/comptime/unsafe/extern/kernel decls going through this
        // rule) — pre-existing, tracked separately. func_type reads the
        // side-channel above instead of $$, so it isn't affected by this gap.
        $$ = $2;
        g_func_signature_params = $2;
        g_func_signature_result = $4;
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
        $$ = func_param_new($1, $2, 0, 0);
    }
    | type {
        // Anonymous parameter - create a var decl with no name
        $$ = func_param_new(NULL, $1, 0, 0);
    }
    | identifier ELLIPSIS type {
        // Task 2: variadic parameter `name ...T`. `type` stores the ELEMENT
        // type T as parsed — the checker (declare_function_signature /
        // type_check_function_decl) wraps it in a TYPE_SLICE when building
        // the signature and binding the body param, driven by
        // is_variadic_param below. Go requires this be the LAST parameter;
        // that's a semantic (not grammatical) constraint, enforced at
        // signature-build time so `func f(a ...int, b int)` gets a clean
        // rejection instead of silently misparsing.
        $$ = func_param_new($1, $3, 1, 0);
    }
    | ELLIPSIS type {
        // Anonymous variadic parameter `...T` (Go allows an unnamed variadic
        // param, matching the unnamed `type` alternative above). Included
        // for symmetry since it added no grammar conflicts (verified via
        // the bison conflict-count gate).
        $$ = func_param_new(NULL, $2, 1, 0);
    }
    | COMPTIME identifier type {
        // Comptime value parameter `comptime name type`.
        $$ = func_param_new($2, $3, 0, 1);
    }
    ;

func_result:
    type { $$ = $1; }
    | LPAREN func_params RPAREN {
        // Parenthesized results reuse the func_param machinery, so a
        // named-result list `(x int, y int)` parses with the SAME rules
        // as a parameter list — no new grammar conflicts. The chain is a
        // ->next list of VarDeclNode (P3-5), each carrying an optional
        // name + a type.
        //
        //  - exactly one ANONYMOUS entry -> single return: unwrap to the
        //    bare type so the return ABI stays a scalar (preserves the old
        //    `func f() (int)` form).
        //  - otherwise (>=2 entries, or any named entry) -> wrap in a
        //    StructTypeNode tagged `is_result_tuple`. Anonymous fields get
        //    synthetic `_N` names (so the type checker's AST_STRUCT_TYPE
        //    path, which skips name-less fields, still sees them); named
        //    fields keep their user names so function codegen can bind them
        //    as in-scope zero-initialized locals and a bare `return` can
        //    read their current values. The `is_result_tuple` tag lets the
        //    type system collapse a SINGLE named result (`(r int)`) back to
        //    a scalar return ABI — the common Go form `func f() (err error)`
        //    must be a scalar, not a 1-field struct — while still binding the
        //    name; a >=2-field tuple keeps the multi-return struct ABI.
        $$ = func_result_from_params($2);
    }
    ;

// Variable declaration
var_decl:
    VAR identifier type {
        $$ = var_decl_new_1($2, $3, NULL);
    }
    // Multi-name, no-initializer form: `var a, b int`. Explicit productions
    // (2-name and 3-name), NOT a general list nonterminal — mirrors the
    // short_var_decl convention (parser.y short_var_decl, above) and bounds
    // conflict risk to what's already proven safe there. The VAR prefix
    // disambiguates from short_var_decl's bare `identifier COMMA identifier`
    // forms, so this does not introduce new shift/reduce or reduce/reduce
    // conflicts (see the bison guard in the Makefile gate). 4+ names and the
    // initializer-list form (`var a, b int = 1, 2`) are out of scope: the
    // latter would need codegen changes (walking a values chain in lockstep
    // with names) beyond this task's allowed file set — see decl-surface
    // breadth task-1 report for the follow-up.
    | VAR identifier COMMA identifier type {
        $$ = var_decl_new_2($2, $4, $5);
    }
    | VAR identifier COMMA identifier COMMA identifier type {
        $$ = var_decl_new_3($2, $4, $6, $7);
    }
    | VAR identifier ASSIGN expression {
        $$ = var_decl_new_1($2, NULL, $4);
    }
    | VAR identifier type ASSIGN expression {
        $$ = var_decl_new_1($2, $3, $5);
    }
    // Goo extension: ownership qualifiers
    | ownership_qualifier VAR identifier type {
        // TODO: Set ownership from $1 (not yet wired to VarDeclNode.ownership)
        $$ = var_decl_new_1($3, $4, NULL);
    }
    // P1.2: grouped var block. Desugar into a chain of ordinary single
    // VarDeclNodes (one per spec), same splice mechanism as CONST's grouped
    // block above. Package-scope groups reuse the same `declaration` path a
    // single top-level `var` already goes through, so they inherit that
    // path's existing constant-initializer-only constraint unchanged
    // (non-constant package-scope globals are P3.7, out of scope here).
    | VAR LPAREN var_spec_list RPAREN {
        $$ = desugar_var_group($3);
    }
    ;

// Short variable declaration
short_var_decl:
    identifier SHORT_ASSIGN expression {
        $$ = short_var_decl_new_1($1, $3);
    }
    | identifier COMMA identifier SHORT_ASSIGN expression {
        // Multi-LHS short var decl `a, b := expr`. Destructuring is
        // resolved at codegen time: the RHS must produce a TYPE_STRUCT
        // with at least 2 fields, and a/b are bound to its fields 0
        // and 1 respectively.
        $$ = short_var_decl_new_2($1, $3, $5);
    }
    // F6: `a, b := v1, v2` — two independent RHS values (not a destructure).
    // The COLON-free trailing `COMMA expression` after the first value is what
    // distinguishes this from the single-value destructure rule above.
    | identifier COMMA identifier SHORT_ASSIGN expression COMMA expression {
        $$ = multi_assign_2_new($1, $3, $5, $7, 1);
    }
    ;

// Constant declaration
const_decl:
    CONST identifier type ASSIGN expression {
        $$ = const_decl_new($2, $3, $5, 0);
    }
    | CONST identifier ASSIGN expression {
        // Untyped single const: `const n = 64`. The type is inferred from the
        // initializer by type_check_const_decl (type == NULL), exactly as the
        // grouped-const desugaring already relies on. Real Go source (and
        // math/bits especially) uses untyped consts pervasively.
        $$ = const_decl_new($2, NULL, $4, 0);
    }
    | COMPTIME CONST identifier type ASSIGN expression {
        $$ = const_decl_new($3, $4, $6, 1);
    }
    | CONST LPAREN const_spec_list RPAREN {
        // F4: grouped const block. Desugar into a chain of ordinary single
        // ConstDeclNodes (one per spec) with `iota` resolved to each spec's
        // ordinal — so the existing single-const type-check/codegen path
        // handles them unchanged. The returned chain is spliced into the
        // top-level decl list by ast_add_child (which follows ->next).
        $$ = desugar_const_group($3);
    }
    ;

// Arc 6 (m): one spec inside a grouped const block — bare `NAME` (repeats
// the previous spec's value with iota incremented; carried as values==NULL
// and filled in by desugar_const_group), untyped `NAME = expr`, or typed
// `NAME TYPE = expr`. F4 shipped this untyped-only, fearing an `identifier
// type` vs bare-`identifier` shift/reduce conflict — and in F4's FLAT
// juxtaposed list that conflict is real: a bare spec could reduce on the
// next spec's leading IDENT, which is also exactly the misparse that made
// `y int16 = 300` read as bare `y` plus a constant NAMED `int16`, silently
// SHIFTING every later value in the group across names. The split below
// dissolves both the conflict and the misparse structurally: every
// non-final spec must carry its terminator ON THE SPEC (const_spec_t —
// explicit `;` or the P5.10 generalized rule-1 ASI's, which fires inside
// const groups like everywhere else; the house member-attached-SEMICOLON
// pattern, workarounds.md §4), and only the FINAL spec (const_spec_last,
// no terminator — ASI's `)` leniency exception) may omit it. A bare
// spec's reduce therefore only ever happens on `;` or `)` lookahead,
// never on IDENT — an IDENT after a spec name can only be its TYPE.
// Same-line juxtaposition without `;` (`const ( x y = 2 )`) accordingly
// parses as a typed spec, no longer as two shifted specs; Go rejects that
// spelling outright, so no legitimate program changes meaning.
const_spec_t:
    identifier SEMICOLON {
        $$ = const_spec_new($1, NULL, NULL);
    }
    | identifier ASSIGN expression SEMICOLON {
        $$ = const_spec_new($1, NULL, $3);
    }
    | identifier type ASSIGN expression SEMICOLON {
        $$ = const_spec_new($1, $2, $4);
    }
    ;

const_spec_last:
    identifier {
        $$ = const_spec_new($1, NULL, NULL);
    }
    | identifier ASSIGN expression {
        $$ = const_spec_new($1, NULL, $3);
    }
    | identifier type ASSIGN expression {
        $$ = const_spec_new($1, $2, $4);
    }
    ;

const_spec_list_t:
    const_spec_t {
        $$ = $1;
    }
    | const_spec_list_t const_spec_t {
        ast_add_child($1, $2);
        $$ = $1;
    }
    ;

// A group is 1+ specs: all-terminated (trailing `;` before `)`), or a
// terminated run followed by one unterminated final spec, or a single
// unterminated spec.
const_spec_list:
    const_spec_last {
        $$ = $1;
    }
    | const_spec_list_t {
        $$ = $1;
    }
    | const_spec_list_t const_spec_last {
        ast_add_child($1, $2);
        $$ = $1;
    }
    ;

// P1.2: one spec inside a grouped var block. UNLIKE const_spec_list/
// import_spec_list's plain juxtaposition, var_spec_list is NOT newline-blind:
// a var_spec's `type` can be a result-less func_type, and func_result's FIRST
// set starts with an identifier — so bare juxtaposition lets a result-less
// `f func(int)` absorb the NEXT spec's name as its own return type (the
// newline-blind func-result hazard, workarounds.md §6; e.g. `var (\n f
// func(int)\n g = 2\n)` misparses as one spec `f func(int) g = 2`). This is
// the exact absorption class the interface-body fix (parser.y interface_type/
// interface_member) neutralizes for method specs; var groups get the same
// fix, mirrored: while lexically inside a `var ( ... )` group, the lexer
// (lexer.c asi_ctx, keyed off '(' this time rather than '{') inserts a ';'
// after a newline that follows a value-ending token, and var_member below
// wraps var_spec with an optional trailing SEMICOLON so each spec terminates
// at its own line instead of extending into the next. A list-level
// `var_spec_list SEMICOLON var_spec` + trailing-SEMICOLON pair was not tried
// here for the same reason it was rejected for interface_member: it would
// need to disambiguate "more list to come" from "trailing terminator" on the
// same lookahead, which is exactly the shape that produced a genuine
// shift/reduce conflict there.
//
// UNLIKE const_spec, there is deliberately NO bare `identifier`-alone arm:
// const's bare spec means "repeat the previous value" (iota inheritance,
// see desugar_const_group); var has no such semantics; a Go var spec must
// carry a type and/or an initializer. Omitting that arm means `var (x)`
// (bare name, no type, no value) is already a grammar-level syntax error —
// nothing extra to reject downstream.
var_spec:
    identifier type {
        // `NAME TYPE`, no initializer: values stays NULL, exactly the same
        // representation single `var z int` already uses (ast_var_decl_new
        // zero-inits `values`) — the existing zero-value codegen/typecheck
        // path for a typed-no-initializer var handles this unchanged.
        $$ = var_spec_new($1, $2, NULL);
    }
    | identifier ASSIGN expression {
        $$ = var_spec_new($1, NULL, $3);
    }
    | identifier type ASSIGN expression {
        $$ = var_spec_new($1, $2, $4);
    }
    ;

// Wraps a var_spec with an optional trailing ';' — explicit or ASI-inserted
// (lexer.c asi_ctx, var-group-scoped ASI mirroring interface_member/
// interface_method). SEMICOLON attaches to the MEMBER, not a list-level
// separator arm — see the var_spec_list comment above and interface_member's
// for why the list-level form was rejected (a genuine shift/reduce conflict).
var_member:
    var_spec { $$ = $1; }
    | var_spec SEMICOLON { $$ = $1; }
    ;

var_spec_list:
    var_member {
        $$ = $1;
    }
    | var_spec_list var_member {
        ast_add_child($1, $2);
        $$ = $1;
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

// P5.10: statement lists are Go-shaped — every statement in a list is
// SEMICOLON-terminated EXCEPT that the final statement before the list's
// terminator (RBRACE / CASE / DEFAULT) may omit it (Go spec "Semicolons"
// rule 2, encoded in the grammar exactly as go/parser does). Together with
// the lexer's generalized rule-1 ASI (lexer.c: insert at a newline after a
// value-ending token unless the next char is ')' or '}'), this kills the
// six 42-conflict reduce/reduce families: a bare expression statement can
// now only be FINAL, FOLLOW(final_stmt) contains no expression-starting
// tokens, so `expression: unary_expr` never competes with a unary-operator
// statement start again. See references/conflict-ledger.md (P5.10 entry).
statement_list:
    stmt_seq {
        $$ = $1;
    }
    | stmt_seq final_stmt {
        ast_add_child($1, $2);
        $$ = $1;
    }
    | final_stmt {
        $$ = $1;
    }
    ;

/* Case bodies (select/switch/type-switch) may be empty (Go parity).
   Scoped epsilon: statement_list itself stays epsilon-free — it is shared
   by if/for/block and an epsilon there would have grammar-wide blast
   radius. NULL body = empty, every consumer treats it as a no-op. */
case_body:
    statement_list { $$ = $1; }
    | /* empty */   { $$ = NULL; }
    ;

stmt_seq:
    statement {
        $$ = $1;
    }
    | stmt_seq statement {
        ast_add_child($1, $2);
        $$ = $1;
    }
    ;

// Terminated statements — legal anywhere in a list. Every arm consumes its
// SEMICOLON (explicit in source, or lexer-ASI-inserted at the newline).
statement:
    simple_stmt SEMICOLON { $$ = $1; }
    | if_stmt SEMICOLON { $$ = $1; }
    | if_let_stmt SEMICOLON { $$ = $1; }
    | for_stmt SEMICOLON { $$ = $1; }
    | return_stmt SEMICOLON { $$ = $1; }
    | break_stmt SEMICOLON { $$ = $1; }
    | continue_stmt SEMICOLON { $$ = $1; }
    | goto_stmt SEMICOLON { $$ = $1; }
    | fallthrough_stmt SEMICOLON { $$ = $1; }
    | go_stmt SEMICOLON { $$ = $1; }
    | defer_stmt SEMICOLON { $$ = $1; }
    | select_stmt SEMICOLON { $$ = $1; }
    | switch_stmt SEMICOLON { $$ = $1; }
    | block SEMICOLON { $$ = $1; }
    | comptime_block SEMICOLON { $$ = $1; }  // Goo extension
    | unsafe_stmt SEMICOLON { $$ = $1; }     // Goo extension
    | arena_stmt SEMICOLON { $$ = $1; }      // Goo extension
    | asm_stmt SEMICOLON { $$ = $1; }        // Goo extension
    | parallel_for_stmt SEMICOLON { $$ = $1; } // Goo extension
    // `L: stmt` — labeled TERMINATED statement (the inner statement carries
    // the terminator). Labels are function-scoped in Go (not block-scoped),
    // so duplicate detection lives in the type checker, not here.
    | identifier COLON statement {
        IdentifierNode* lid = (IdentifierNode*)$1;
        LabelStmtNode* label_node = ast_label_stmt_new(lid->name, $3, get_current_position());
        ast_node_free($1);
        $$ = (ASTNode*)label_node;
    }
    // Explicit empty-labeled statement `done: ;` — the SEMICOLON must be
    // consumed here or it dangles (there is no bare-SEMICOLON statement).
    | identifier COLON SEMICOLON {
        IdentifierNode* lid = (IdentifierNode*)$1;
        LabelStmtNode* label_node = ast_label_stmt_new(lid->name, NULL, get_current_position());
        ast_node_free($1);
        $$ = (ASTNode*)label_node;
    }
    ;

// Unterminated (bare) statements — legal ONLY as the last statement of a
// list, i.e. immediately before RBRACE/CASE/DEFAULT. This is what keeps
// single-line blocks (`if x { y() }`) and last-statement-before-`}` shapes
// (the lexer never inserts before '}' ) parsing exactly as before P5.10.
final_stmt:
    simple_stmt { $$ = $1; }
    | if_stmt { $$ = $1; }
    | if_let_stmt { $$ = $1; }
    | for_stmt { $$ = $1; }
    | return_stmt { $$ = $1; }
    | break_stmt { $$ = $1; }
    | continue_stmt { $$ = $1; }
    | goto_stmt { $$ = $1; }
    | fallthrough_stmt { $$ = $1; }
    | go_stmt { $$ = $1; }
    | defer_stmt { $$ = $1; }
    | select_stmt { $$ = $1; }
    | switch_stmt { $$ = $1; }
    | block { $$ = $1; }
    | comptime_block { $$ = $1; }  // Goo extension
    | unsafe_stmt { $$ = $1; }     // Goo extension
    | arena_stmt { $$ = $1; }      // Goo extension
    | asm_stmt { $$ = $1; }        // Goo extension
    | parallel_for_stmt { $$ = $1; } // Goo extension
    // `L: stmt` where stmt is itself final: `for { ... L: y() }`.
    | identifier COLON final_stmt {
        IdentifierNode* lid = (IdentifierNode*)$1;
        LabelStmtNode* label_node = ast_label_stmt_new(lid->name, $3, get_current_position());
        ast_node_free($1);
        $$ = (ASTNode*)label_node;
    }
    // Fix 5a's bare `done:` label with an implicit empty target — by
    // construction this only ever reduced at list-final position (lookahead
    // RBRACE/CASE/DEFAULT via $default), so it lives here now; the wrapped
    // stmt is NULL and every consumer treats that as a no-op fall-through.
    | identifier COLON {
        IdentifierNode* lid = (IdentifierNode*)$1;
        LabelStmtNode* label_node = ast_label_stmt_new(lid->name, NULL, get_current_position());
        ast_node_free($1);
        $$ = (ASTNode*)label_node;
    }
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
    | const_decl { $$ = $1; }  // local const: `const n = 64` inside a function body
    // Single assignment to any lvalue: `x = e`, `s[i] = e`, `p.f = e`. Was an
    // expression-level rule (expression ASSIGN expression); moved here so the
    // LHS shares the `expression` reduction and tuple assignment below has no
    // reduce/reduce conflict. Wrapped in ExprStmt to match the prior AST shape.
    | expression ASSIGN expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(ASSIGN), $3, get_current_position());
        ExprStmtNode* es = (ExprStmtNode*)malloc(sizeof(ExprStmtNode));
        es->base.type = AST_EXPR_STMT;
        es->base.pos = get_current_position();
        es->base.node_type = NULL;
        es->base.next = NULL;
        es->expr = (ASTNode*)binary;
        $$ = (ASTNode*)es;
    }
    // Compound assignment: `x += e`, `x -= e`, ... The compound operator is kept
    // on the BinaryExpr and lowered to load-op-store in codegen (see
    // compound_assign_stmt). Pervasive in real Go source (e.g. bits.OnesCount).
    | expression PLUS_ASSIGN expression   { $$ = compound_assign_stmt($1, TOKEN_PLUS_ASSIGN, $3); }
    | expression MINUS_ASSIGN expression  { $$ = compound_assign_stmt($1, TOKEN_MINUS_ASSIGN, $3); }
    | expression MUL_ASSIGN expression    { $$ = compound_assign_stmt($1, TOKEN_MUL_ASSIGN, $3); }
    | expression DIV_ASSIGN expression    { $$ = compound_assign_stmt($1, TOKEN_DIV_ASSIGN, $3); }
    | expression MOD_ASSIGN expression    { $$ = compound_assign_stmt($1, TOKEN_MOD_ASSIGN, $3); }
    | expression AND_ASSIGN expression    { $$ = compound_assign_stmt($1, TOKEN_AND_ASSIGN, $3); }
    | expression OR_ASSIGN expression     { $$ = compound_assign_stmt($1, TOKEN_OR_ASSIGN, $3); }
    | expression XOR_ASSIGN expression    { $$ = compound_assign_stmt($1, TOKEN_XOR_ASSIGN, $3); }
    | expression LSHIFT_ASSIGN expression { $$ = compound_assign_stmt($1, TOKEN_LSHIFT_ASSIGN, $3); }
    | expression RSHIFT_ASSIGN expression { $$ = compound_assign_stmt($1, TOKEN_RSHIFT_ASSIGN, $3); }
    // F6: `a, b = v1, v2` — tuple assignment to two existing identifiers. Kept
    // as an `identifier`-prefixed rule (alongside the `expression`-prefixed rule
    // below) because short_var_decl uses `identifier COMMA identifier ...`; for a
    // bare-identifier LHS bison resolves the reduce/reduce toward `identifier`
    // (this rule is declared first), so `a, b = b, a` keeps working. Index and
    // selector LHS (`s[i]`) can only reduce as `expression`, so they fall through
    // to the rule below.
    | identifier COMMA identifier ASSIGN expression COMMA expression {
        $$ = multi_assign_2_new($1, $3, $5, $7, 0);
    }
    // `a, b = f()` — tuple assignment from a SINGLE multi-return call. The
    // absence of a trailing `COMMA expression` (present in the rule above)
    // distinguishes this destructuring form. Declared before the general
    // expression-LHS variant so a bare-identifier LHS reduces here.
    | identifier COMMA identifier ASSIGN expression {
        $$ = multi_assign_call_new($1, $3, $5);
    }
    // Tuple assignment to any lvalues: `s[i], s[j] = s[j], s[i]`, `p.a, p.b = …`.
    // Targets are full expressions (addressability checked in the typechecker).
    | expression COMMA expression ASSIGN expression COMMA expression {
        $$ = multi_assign_2_new($1, $3, $5, $7, 0);
    }
    // `p.a, s[i] = f()` — single-call destructure to general lvalue targets.
    | expression COMMA expression ASSIGN expression {
        $$ = multi_assign_call_new($1, $3, $5);
    }
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
    // `if init; cond { }` — the idiomatic Go guard form (e.g. `if v, ok := m[k]; ok {`).
    // Desugared to a wrapping block `{ init; if cond {...} }` (mirrors the C-style
    // `for init; cond; post` shape at parser.y:1220, disambiguated the same way on
    // SEMICOLON) rather than adding an `init` field to IfStmtNode: this way the init
    // var's scope is naturally bounded to the wrapper block (out of scope after the
    // if, matching Go), with zero AST/codegen changes.
    | IF simple_stmt SEMICOLON expression block {
        IfStmtNode* if_node = (IfStmtNode*)malloc(sizeof(IfStmtNode));
        if_node->base.type = AST_IF_STMT;
        if_node->base.pos = get_current_position();
        if_node->base.node_type = NULL;
        if_node->base.next = NULL;
        if_node->condition = $4;
        if_node->then_stmt = $5;
        if_node->else_stmt = NULL;
        BlockStmtNode* wrapper = ast_block_stmt_new(get_current_position());
        wrapper->statements = $2;
        ast_add_child($2, (ASTNode*)if_node);
        $$ = (ASTNode*)wrapper;
    }
    | IF simple_stmt SEMICOLON expression block ELSE block {
        IfStmtNode* if_node = (IfStmtNode*)malloc(sizeof(IfStmtNode));
        if_node->base.type = AST_IF_STMT;
        if_node->base.pos = get_current_position();
        if_node->base.node_type = NULL;
        if_node->base.next = NULL;
        if_node->condition = $4;
        if_node->then_stmt = $5;
        if_node->else_stmt = $7;
        BlockStmtNode* wrapper = ast_block_stmt_new(get_current_position());
        wrapper->statements = $2;
        ast_add_child($2, (ASTNode*)if_node);
        $$ = (ASTNode*)wrapper;
    }
    | IF simple_stmt SEMICOLON expression block ELSE if_stmt {
        IfStmtNode* if_node = (IfStmtNode*)malloc(sizeof(IfStmtNode));
        if_node->base.type = AST_IF_STMT;
        if_node->base.pos = get_current_position();
        if_node->base.node_type = NULL;
        if_node->base.next = NULL;
        if_node->condition = $4;
        if_node->then_stmt = $5;
        if_node->else_stmt = $7;
        BlockStmtNode* wrapper = ast_block_stmt_new(get_current_position());
        wrapper->statements = $2;
        ast_add_child($2, (ASTNode*)if_node);
        $$ = (ASTNode*)wrapper;
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
    | FOR RANGE expression block {
        // `for range expr { … }` — no index/value (Go allows it; used by e.g.
        // utf8.RuneCountInString to count iterations). key/value stay NULL.
        ForStmtNode* for_node = (ForStmtNode*)calloc(1, sizeof(ForStmtNode));
        for_node->base.type = AST_FOR_STMT;
        for_node->base.pos = get_current_position();
        for_node->range_expr = $3;
        for_node->body = $4;
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
        $$ = ast_break_stmt_new(get_current_position());
    }
    | BREAK identifier {
        // `break L` — a distinct node from bare BREAK (see AST_BREAK_LABEL_STMT's
        // enum-site comment); codegen resolves L by walking the break-scope
        // stack, not the innermost frame.
        IdentifierNode* lid = (IdentifierNode*)$2;
        BreakLabelStmtNode* node = ast_break_label_stmt_new(lid->name, get_current_position());
        ast_node_free($2);
        $$ = (ASTNode*)node;
    }
    ;

continue_stmt:
    CONTINUE {
        $$ = ast_continue_stmt_new(get_current_position());
    }
    | CONTINUE identifier {
        // `continue L` — sibling of `break L` above.
        IdentifierNode* lid = (IdentifierNode*)$2;
        ContinueLabelStmtNode* node = ast_continue_label_stmt_new(lid->name, get_current_position());
        ast_node_free($2);
        $$ = (ASTNode*)node;
    }
    ;

goto_stmt:
    GOTO identifier {
        // gofmt-syntax-b Task 2 (P1.6): `goto L`. Unlike break_stmt/
        // continue_stmt above, GOTO has no bare (label-less) alternative —
        // the operand is mandatory — so this single arm carries no
        // competing reduce and is expected to add zero grammar-conflict
        // delta (verified by the tripwire, not assumed).
        IdentifierNode* lid = (IdentifierNode*)$2;
        GotoStmtNode* node = ast_goto_stmt_new(lid->name, get_current_position());
        ast_node_free($2);
        $$ = (ASTNode*)node;
    }
    ;

fallthrough_stmt:
    FALLTHROUGH {
        // gofmt-syntax-b Task 3 (P1.7): `fallthrough` — a bare marker
        // statement (AST_FALLTHROUGH_STMT), no operand ever, unlike every
        // other member of this keyword-statement family except bare
        // BREAK/CONTINUE. FALLTHROUGH is already one of lexer.c's
        // unconditional keyword-terminator ASI tokens (same "Part 1"
        // group as RETURN/BREAK/CONTINUE — see the ledger's gofmt-syntax-b
        // Task 1 entry), so this single-token arm is expected to add zero
        // grammar-conflict delta: there is no competing reduce for the
        // tripwire to newly conflict against (verified, not assumed).
        // Placement legality (final statement of a non-last
        // expression-switch clause; illegal in a type switch, select, or
        // outside any switch) is entirely a type-check-time concern — see
        // type_check_switch_like_body's doc comment, type_checker.c.
        $$ = ast_fallthrough_stmt_new(get_current_position());
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
    CASE expression COLON case_body {
        SelectCaseNode* case_node = ast_select_case_new($2, $4, get_current_position());
        $$ = (ASTNode*)case_node;
    }
    // gofmt-syntax-b Task 4 (P1.10): `case v := <-ch:` — value-binding
    // receive. The grammar accepts any `expression` after `:=` (same shape
    // as the plain-comm arm above, kept zero-new-surface) rather than
    // encoding `ARROW expression` here; receive-ness is validated
    // SEMANTICALLY in type_check_select_stmt ("select case must be a
    // receive operation"), where the diagnostic can name the actual problem
    // instead of a generic parse error. `identifier` is freed right after
    // its name is copied out — `ast_select_case_new` already defaults
    // bind_name/is_declare to NULL/0, so every OTHER call site (plain comm,
    // default, and the comma-ok arm below) is unaffected by this new field.
    | CASE identifier SHORT_ASSIGN expression COLON case_body {
        IdentifierNode* bid = (IdentifierNode*)$2;
        SelectCaseNode* case_node = ast_select_case_new($4, $6, get_current_position());
        case_node->bind_name = strdup(bid->name);
        case_node->is_declare = 1;
        ast_node_free($2);
        $$ = (ASTNode*)case_node;
    }
    // `case v = <-ch:` — bind into an EXISTING, already-declared variable
    // (checked in the type checker: must exist in an enclosing scope and be
    // assignment-compatible with the channel's element type).
    | CASE identifier ASSIGN expression COLON case_body {
        IdentifierNode* bid = (IdentifierNode*)$2;
        SelectCaseNode* case_node = ast_select_case_new($4, $6, get_current_position());
        case_node->bind_name = strdup(bid->name);
        case_node->is_declare = 0;
        ast_node_free($2);
        $$ = (ASTNode*)case_node;
    }
    // `case v, ok := <-ch:` — comma-ok binding. v1 scope cut: `ok` is
    // meaningless without close() (P3.1: hardcoding it true would be a
    // silent lie), so this shape is grammar-accepted only so the type
    // checker can reject it with a specific, positioned "requires close()"
    // diagnostic instead of a bare "syntax error" — see the is_declare = -1
    // sentinel documented on SelectCaseNode in ast.h. Neither identifier's
    // name is kept (both freed here): the case is rejected either way, so
    // nothing downstream needs them.
    | CASE identifier COMMA identifier SHORT_ASSIGN expression COLON case_body {
        SelectCaseNode* case_node = ast_select_case_new($6, $8, get_current_position());
        case_node->is_declare = -1;
        ast_node_free($2);
        ast_node_free($4);
        $$ = (ASTNode*)case_node;
    }
    | DEFAULT COLON case_body {
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
    /* Tagless switch `switch { case cond: ... }` (switch-true): synthesize a
       `true` tag so the existing icmp-eq lowering matches the first case whose
       boolean condition is true. Pervasive in the stdlib (e.g. utf8.RuneLen). */
    | SWITCH LBRACE_BODY case_clause_list RBRACE {
        ASTNode* t = (ASTNode*)ast_literal_new(TOKEN_TRUE, "true", get_current_position());
        $$ = (ASTNode*)ast_switch_stmt_new(t, $3, get_current_position());
    }
    | SWITCH LBRACE case_clause_list RBRACE {
        ASTNode* t = (ASTNode*)ast_literal_new(TOKEN_TRUE, "true", get_current_position());
        $$ = (ASTNode*)ast_switch_stmt_new(t, $3, get_current_position());
    }
    /* Type assertions branch, Task 3: `switch [v :=] x.(type) { case T1: … }`.
       HIGHEST-RISK grammar change in the branch — this arm shares the exact
       `SWITCH … LBRACE_BODY … RBRACE` prefix with the three arms above.
       type_switch_guard's `DOT LPAREN TYPE RPAREN` tail is what commits the
       parse to THIS arm before any CASE is reached: TYPE (the literal `type`
       keyword token) is disjoint from FIRST(type) (the `type` nonterminal
       used by the ordinary `x.(T)` assertion, selector_expr — type_name,
       array_type, slice_type, map_type, chan_type, func_type, pointer_type,
       reference_type, unsafe_ptr_type, error_union_type, nullable_type,
       struct_type, enum_type, interface_type — none of which start with the
       TYPE keyword), so the single token after LPAREN cleanly decides which
       of `expression` (plain switch, possibly containing an `x.(T)` value
       assertion) vs `type_switch_guard` is being parsed — by the time
       type_case_list is reached, case_clause_list is no longer reachable
       from this state. See docs/superpowers/specs/2026-07-04-type-
       assertions-design.md "Grammar (risk center)" and the goo-grammar
       skill's tripwire discipline. */
    | SWITCH type_switch_guard LBRACE_BODY type_case_list RBRACE {
        TypeSwitchNode* tsw = ast_type_switch_new($2.bind_name, $2.expr, $4, get_current_position());
        $$ = (ASTNode*)tsw;
    }
    | SWITCH type_switch_guard LBRACE type_case_list RBRACE {
        TypeSwitchNode* tsw = ast_type_switch_new($2.bind_name, $2.expr, $4, get_current_position());
        $$ = (ASTNode*)tsw;
    }
    // `switch init; tag { }` — the idiomatic Go guard form (e.g.
    // `switch x := 2; x { case 2: ... }`). Desugared to a wrapping block
    // `{ init; switch tag {...} }`, mirroring IF's init arm (parser.y:1270)
    // and FOR's C-style init clause: the init var's scope is naturally
    // bounded to the wrapper block (out of scope after the switch, matching
    // Go), with zero AST/codegen changes to SwitchStmtNode.
    | SWITCH simple_stmt SEMICOLON expression LBRACE_BODY case_clause_list RBRACE {
        ASTNode* switch_node = (ASTNode*)ast_switch_stmt_new($4, $6, get_current_position());
        BlockStmtNode* wrapper = ast_block_stmt_new(get_current_position());
        wrapper->statements = $2;
        ast_add_child($2, switch_node);
        $$ = (ASTNode*)wrapper;
    }
    | SWITCH simple_stmt SEMICOLON expression LBRACE case_clause_list RBRACE {
        ASTNode* switch_node = (ASTNode*)ast_switch_stmt_new($4, $6, get_current_position());
        BlockStmtNode* wrapper = ast_block_stmt_new(get_current_position());
        wrapper->statements = $2;
        ast_add_child($2, switch_node);
        $$ = (ASTNode*)wrapper;
    }
    // `switch init; { }` — the tagless (switch-true) form combined with an
    // init statement (e.g. `switch x := 5; { case x < 10: ... }`). Fix C:
    // Task 2 added init arms for the tagged and type-switch forms only,
    // leaving this one out; mirrors the tagged-init arms above exactly the
    // same way the bare tagless arm (parser.y:1604) mirrors the bare tagged
    // arm (parser.y:1595) — synthesize a `true` tag, then reuse the same
    // init-wrapper desugar (scope bounded to the wrapper block). No new
    // conflict surface: `expression` cannot start with LBRACE/LBRACE_BODY
    // (see composite_value's comment at parser.y:2809), so the token
    // immediately after SEMICOLON already decides between this arm and the
    // expression-tag arms above with one token of lookahead — the same
    // disjointness the bare tagged/tagless pair already relies on.
    | SWITCH simple_stmt SEMICOLON LBRACE_BODY case_clause_list RBRACE {
        ASTNode* t = (ASTNode*)ast_literal_new(TOKEN_TRUE, "true", get_current_position());
        ASTNode* switch_node = (ASTNode*)ast_switch_stmt_new(t, $5, get_current_position());
        BlockStmtNode* wrapper = ast_block_stmt_new(get_current_position());
        wrapper->statements = $2;
        ast_add_child($2, switch_node);
        $$ = (ASTNode*)wrapper;
    }
    | SWITCH simple_stmt SEMICOLON LBRACE case_clause_list RBRACE {
        ASTNode* t = (ASTNode*)ast_literal_new(TOKEN_TRUE, "true", get_current_position());
        ASTNode* switch_node = (ASTNode*)ast_switch_stmt_new(t, $5, get_current_position());
        BlockStmtNode* wrapper = ast_block_stmt_new(get_current_position());
        wrapper->statements = $2;
        ast_add_child($2, switch_node);
        $$ = (ASTNode*)wrapper;
    }
    // `switch init; [v :=] x.(type) { }` — same init-guard shape for the
    // type-switch form. Reuses type_switch_guard unmodified (no new
    // type-switch surface — spec open point 3): the bind form
    // `identifier SHORT_ASSIGN primary_expr DOT LPAREN TYPE RPAREN` already
    // parses at base without init (Task 3, type-assertions branch), so this
    // mirrors the same init-wrapper desugar onto it.
    | SWITCH simple_stmt SEMICOLON type_switch_guard LBRACE_BODY type_case_list RBRACE {
        ASTNode* switch_node = (ASTNode*)ast_type_switch_new($4.bind_name, $4.expr, $6, get_current_position());
        BlockStmtNode* wrapper = ast_block_stmt_new(get_current_position());
        wrapper->statements = $2;
        ast_add_child($2, switch_node);
        $$ = (ASTNode*)wrapper;
    }
    | SWITCH simple_stmt SEMICOLON type_switch_guard LBRACE type_case_list RBRACE {
        ASTNode* switch_node = (ASTNode*)ast_type_switch_new($4.bind_name, $4.expr, $6, get_current_position());
        BlockStmtNode* wrapper = ast_block_stmt_new(get_current_position());
        wrapper->statements = $2;
        ast_add_child($2, switch_node);
        $$ = (ASTNode*)wrapper;
    }
    ;

// Type assertions branch, Task 3: the type-switch guard. The bind-less form
// (`x.(type)`) and the `identifier := x.(type)` bind form are both admitted;
// switch_stmt's action reads $$.bind_name (NULL for the bind-less form) and
// $$.expr uniformly. TYPE here is the literal `type` keyword token (bison
// token TYPE, mapped from TOKEN_TYPE — see parser.y's %token block) — NOT
// the `type` nonterminal used by `primary_expr DOT LPAREN type RPAREN`
// (Task 1's ordinary type assertion, selector_expr above). This literal-
// keyword-vs-nonterminal split is exactly what keeps the two DOT-LPAREN
// forms conflict-free on one token of lookahead (see switch_stmt's comment
// above).
type_switch_guard:
    primary_expr DOT LPAREN TYPE RPAREN {
        $$.bind_name = NULL;
        $$.expr = $1;
    }
    | identifier SHORT_ASSIGN primary_expr DOT LPAREN TYPE RPAREN {
        $$.bind_name = $1;
        $$.expr = $3;
    }
    ;

// Type assertions branch, Task 3: type-switch case clauses. A separate
// clause/list pair from case_clause/case_clause_list (not a reuse) — each
// clause lists TYPES (type_list), not value expressions, and is only ever
// reachable via the type_switch_guard arm above (never the plain
// case_clause_list), so the two never compete for the same LALR state
// despite the structural similarity.
type_case_list:
    type_case_clause {
        $$ = $1;
    }
    | type_case_list type_case_clause {
        ASTNode* current = $1;
        while (current->next) {
            current = current->next;
        }
        current->next = $2;
        $$ = $1;
    }
    ;

type_case_clause:
    CASE type_list COLON case_body {
        $$ = (ASTNode*)ast_type_case_new($2, $4, get_current_position());
    }
    | DEFAULT COLON case_body {
        $$ = (ASTNode*)ast_type_case_new(NULL, $3, get_current_position());
    }
    ;

// `case nil:` matches a nil interface (see design doc) — nil is not a `type`
// (NIL is a distinct token, disjoint from every `type` alternative's FIRST
// set, exactly like TYPE above), so it needs its own alternatives here
// rather than being reachable through the `type` nonterminal. The resulting
// list element is a LiteralNode(TOKEN_NIL) sentinel — type_check_type_switch_stmt
// and codegen_generate_type_switch_stmt both recognize it structurally
// (AST_LITERAL + literal_type == TOKEN_NIL) rather than resolving it via
// type_from_ast.
type_list:
    type {
        $$ = $1;
    }
    | NIL {
        LiteralNode* lit = ast_literal_new(TOKEN_NIL, "nil", get_current_position());
        $$ = (ASTNode*)lit;
    }
    | type_list COMMA type {
        ASTNode* current = $1;
        while (current->next) {
            current = current->next;
        }
        current->next = $3;
        $$ = $1;
    }
    | type_list COMMA NIL {
        LiteralNode* lit = ast_literal_new(TOKEN_NIL, "nil", get_current_position());
        ASTNode* current = $1;
        while (current->next) {
            current = current->next;
        }
        current->next = (ASTNode*)lit;
        $$ = $1;
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
    CASE expression COLON case_body {
        $$ = (ASTNode*)ast_case_clause_new($2, $4, get_current_position());
    }
    | DEFAULT COLON case_body {
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
    | BANG unary_expr {
        // Boolean NOT `!x`. The lexer emits BANG for a lone `!` (TOKEN_NOT
        // was never emitted — the old NOT production was dead grammar).
        // BANG doubles as the error-union type marker (`!T`, the BANG type
        // production in error_union_type). Store TOKEN_NOT so the existing
        // typecheck (bool-only) and codegen (LLVMBuildNot) arms serve the
        // expression unchanged.
        //
        // This production adds ONE shift/reduce conflict (78 -> 79): BANG
        // joins the pre-existing 8-token shift-wins family (IDENTIFIER,
        // FUNC, MAP, UNSAFE, MULTIPLY, BIT_AND, LPAREN, LBRACKET) in the
        // func_signature -> func_result states (285/429 at time of
        // writing), where the default shift resolution = "parse a result
        // type" — the required behavior: after a func signature, `!` must
        // start the `!T` error-union return type. The reduce alternative
        // is unreachable in real programs (function bodies start with
        // LBRACE, and func literals don't parse in expression position).
        UnaryExprNode* unary = ast_unary_expr_new(bison_token_to_token_type(NOT), $2, get_current_position());
        $$ = (ASTNode*)unary;
    }
    | BIT_NOT unary_expr {
        UnaryExprNode* unary = ast_unary_expr_new(bison_token_to_token_type(BIT_NOT), $2, get_current_position());
        $$ = (ASTNode*)unary;
    }
    | BIT_XOR unary_expr %prec BIT_NOT {
        // Go spells bitwise complement `^x` with the same token as binary XOR.
        // %prec BIT_NOT gives the PREFIX form the high unary precedence (not
        // BIT_XOR's low binary precedence), which is what keeps this from
        // introducing the reduce/reduce conflicts a bare rule would.
        UnaryExprNode* unary = ast_unary_expr_new(bison_token_to_token_type(BIT_XOR), $2, get_current_position());
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
    | expression ARROW expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(ARROW), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    // Binary bitwise operators. Tokens, precedence (see the %left block),
    // type-checking, and codegen (Shl/AShr/LShr/And/Or/Xor) already existed;
    // only these productions were missing. BIT_AND is also the unary
    // address-of operator (`&x`), exactly like MULTIPLY's unary-deref dual
    // role above — the unary_expr level and operator precedence disambiguate.
    | expression LSHIFT expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(LSHIFT), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression RSHIFT expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(RSHIFT), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression BIT_AND expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(BIT_AND), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression AND_NOT expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(AND_NOT), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression BIT_OR expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(BIT_OR), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression BIT_XOR expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(BIT_XOR), $3, get_current_position());
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
    /* Task 2 (stdlib unblocker): `[]byte(s)` / `[]T(expr)` conversion form.
       slice_type is NOT itself a primary_expr (it's a TYPE), so this can't
       reuse call_expr's `primary_expr LPAREN ... RPAREN` shape — it needs
       its own alternative directly in primary_expr. v1 only admits
       []byte(string); any other []T(expr) is rejected by the type checker
       (expression_checker.c's AST_SLICE_CONVERSION case), not the grammar. */
    | slice_type LPAREN expression RPAREN {
        SliceConvNode* conv = (SliceConvNode*)malloc(sizeof(SliceConvNode));
        conv->base.type = AST_SLICE_CONVERSION;
        conv->base.pos = get_current_position();
        conv->base.node_type = NULL;
        conv->base.next = NULL;
        conv->slice_type = $1;
        conv->operand = $3;
        $$ = (ASTNode*)conv;
    }
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
    /* Closures Branch B, Task 1: func literal `func(params) result { body }`
       as a primary expression — FUNC enters expression position for the
       first time (previously FUNC only began a top-level func_decl or a
       func_type in TYPE position). Each action builds the FuncLitNode's
       params/return_type/body from its OWN RHS symbols (func_params/
       func_result/block) — NOT the g_func_signature_params/result side
       channel (that stash is func_type's alone; see its declaration
       comment). Mirrors func_decl's four param/result shapes exactly,
       including reinterpret_grouped_names for Go's grouped-name sugar
       `(x, y int)`. Bison risk: verified empirically (see the conflict-
       count gate in the closures design doc / task report) — baseline 79
       shift/reduce + 256 reduce/reduce; a delta requires a written
       conflict-family analysis and full-suite differential verification. */
    | FUNC LPAREN RPAREN block {
        FuncLitNode* lit = ast_func_lit_new(get_current_position());
        lit->params = NULL;
        lit->return_type = NULL;
        lit->body = $4;
        $$ = (ASTNode*)lit;
    }
    | FUNC LPAREN func_params RPAREN block {
        reinterpret_grouped_names($3); // Go grouped params `(x, y int)`
        FuncLitNode* lit = ast_func_lit_new(get_current_position());
        lit->params = $3;
        lit->return_type = NULL;
        lit->body = $5;
        $$ = (ASTNode*)lit;
    }
    | FUNC LPAREN RPAREN func_result block {
        FuncLitNode* lit = ast_func_lit_new(get_current_position());
        lit->params = NULL;
        lit->return_type = $4;
        lit->body = $5;
        $$ = (ASTNode*)lit;
    }
    | FUNC LPAREN func_params RPAREN func_result block {
        reinterpret_grouped_names($3); // Go grouped params `(x, y int)`
        FuncLitNode* lit = ast_func_lit_new(get_current_position());
        lit->params = $3;
        lit->return_type = $5;
        lit->body = $6;
        $$ = (ASTNode*)lit;
    }
    ;

call_expr:
    primary_expr LPAREN RPAREN {
        $$ = call_expr_new($1, NULL, 0);
    }
    | primary_expr LPAREN expression_list RPAREN {
        $$ = call_expr_new($1, $3, 0);
    }
    // Task 4 (trailing comma in call args): gofmt's canonical multi-line
    // call shape (`f(\n    a,\n    b,\n)`) needs this arm — after a COMMA,
    // RPAREN can immediately follow. LR(1)-clean by the same argument as
    // the composite-literal COMMA-before-RBRACE arms (workarounds.md §5):
    // after `expression_list COMMA`, lookahead RPAREN reduces here, any
    // expression-starting token instead shifts into one more element via
    // expression_list's own COMMA-chaining production. Deliberately NOT
    // folded into expression_list itself — that nonterminal is shared with
    // non-call contexts (return, tuple assignment) where a trailing comma
    // must stay illegal (spec open point 2). This arm also covers method
    // calls for free: `obj.Method(a, b,)` reaches here because
    // selector_expr reduces to primary_expr before LPAREN is seen.
    | primary_expr LPAREN expression_list COMMA RPAREN {
        $$ = call_expr_new($1, $3, 0);
    }
    // Task 3 (spread `f(s...)`): identical construction to the plain-arg arm
    // immediately above; only has_spread differs. Spread is grammatically
    // FINAL-ONLY — ELLIPSIS sits directly before RPAREN, so it can only ever
    // apply to the last element of expression_list (Go rejects non-final
    // `...` as a syntax error too, which this shape enforces for free: there
    // is no alternative production for `...` before a COMMA). The typechecker
    // (expression_checker.c's has_spread block) is the ONLY consumer that
    // interprets this flag; codegen's variadic pack builder (call_codegen.c)
    // reads it to bypass the per-element pack and pass the operand's slice
    // value straight through (Go aliasing semantics). Bison tripwire: this
    // arm must leave the conflict count unchanged — the exact expected count
    // is tracked in scripts/grammar-tripwire.sh (EXPECTED_SR/EXPECTED_RR),
    // the single source of truth; see .claude/skills/goo-grammar/ for the
    // procedure and references/conflict-ledger.md for the baseline history.
    | primary_expr LPAREN expression_list ELLIPSIS RPAREN {
        $$ = call_expr_new($1, $3, 1);
    }
    // `make(map[K]V)` / `make([]T, n)`: a type in call-argument position.
    // NOTE: the first tokens here are NOT disjoint from expression's first
    // set — MAP also begins `map_lit` (map_type LBRACE ...) and LBRACKET
    // also begins `slice_lit`, both of which are expressions. The parser
    // stays conflict-free by a LATER split, not a first-token one:
    //   - map_type: after `map[K]V` reduces, the FOLLOW token decides —
    //     LBRACE continues into a map_lit, while RPAREN/COMMA reduce it as
    //     a type_call_arg here. LALR resolves this on that one lookahead.
    //   - slice_type: LBRACKET RBRACKET_SLICE type — the SECOND token,
    //     RBRACKET_SLICE (a lexer-bridge token emitted only for an empty
    //     `[]` immediately followed by a type), is the real disambiguator
    //     versus slice_lit's `[` <elements> `]`.
    // Empirically verified: adding these two alternatives leaves the bison
    // conflict count unchanged at 78 shift/reduce + 256 reduce/reduce.
    // `make` itself stays an ordinary identifier (not a keyword); the type
    // checker rejects any other callee applied to a type argument.
    | primary_expr LPAREN type_call_arg RPAREN {
        $$ = call_expr_new($1, $3, 0);
    }
    // Task 4: trailing comma after make()'s sole type argument, e.g.
    // `make(\n    map[string]int,\n)`. `make` is an ordinary call target
    // (not a keyword) so it gets the same gofmt-shape tolerance as any
    // other call; real Go's own Arguments grammar allows this too
    // (verified with `go run` on `make(map[string]int,)`).
    | primary_expr LPAREN type_call_arg COMMA RPAREN {
        $$ = call_expr_new($1, $3, 0);
    }
    | primary_expr LPAREN type_call_arg COMMA expression_list RPAREN {
        // The type node leads the argument list; splice the rest of
        // expression_list after it (mirrors expression_list's own
        // left-to-right chaining via ast_add_child/->next).
        // Invariant: type_call_arg (map_type/slice_type/chan_type) produces
        // a freshly malloc'd node whose ->next is NULL, so this direct
        // assignment does not drop a pre-existing tail — no ast_add_child
        // walk is needed.
        $3->next = $5;
        $$ = call_expr_new($1, $3, 0);
    }
    // Task 4: trailing comma after make()'s two-arg form, e.g.
    // `make(\n    []int,\n    3,\n)`. Same splice as the arm above, plus
    // the trailing COMMA before RPAREN.
    | primary_expr LPAREN type_call_arg COMMA expression_list COMMA RPAREN {
        $3->next = $5;
        $$ = call_expr_new($1, $3, 0);
    }
    ;

// The type argument accepted by `make(...)` (grammar-only — the type
// checker enforces that the callee is actually `make`). map_type/
// slice_type/chan_type are the type forms make() needs (make(chan T[, n])
// mirrors make([]T, n)'s capacity-arg shape); other type forms (struct_type,
// ...) are deliberately not included here. CHAN is not in primary_expr's
// first set (no expression production starts with it), so this addition is
// disjoint from make()'s ordinary-call alternatives on the first token —
// unlike map_type/slice_type, which need the LALR lookahead split described
// above.
type_call_arg:
    map_type { $$ = $1; }
    | slice_type { $$ = $1; }
    | chan_type { $$ = $1; }
    ;

index_expr:
    primary_expr LBRACKET expression RBRACKET {
        $$ = index_expr_new($1, $3);
    }
    // F5: slice/substring expression `expr[low:high]`. The COLON after the
    // first bound distinguishes it from plain indexing on one lookahead token
    // (RBRACKET vs COLON). Both bounds required in v1.
    | primary_expr LBRACKET expression COLON expression RBRACKET {
        $$ = slice_index_expr_new($1, $3, $5);
    }
    /* Open-ended slices: `s[low:]` (high defaults to len), `s[:high]` (low
       defaults to 0), `s[:]` (whole). A NULL low/high is filled in by codegen.
       Common in Go, e.g. strings.HasSuffix's `s[len(s)-n:]`. */
    | primary_expr LBRACKET expression COLON RBRACKET {
        $$ = slice_index_expr_new($1, $3, NULL);
    }
    | primary_expr LBRACKET COLON expression RBRACKET {
        $$ = slice_index_expr_new($1, NULL, $4);
    }
    | primary_expr LBRACKET COLON RBRACKET {
        $$ = slice_index_expr_new($1, NULL, NULL);
    }
    ;

selector_expr:
    primary_expr DOT identifier {
        $$ = selector_expr_new($1, $3);
    }
    // Type assertions branch, Task 1: `x.(T)` type assertion. DOT identifier
    // (field/method selector, above) vs DOT LPAREN (type assertion, here) is
    // a clean one-token lookahead split after DOT — no LALR ambiguity with
    // the selector arm. The comma-ok vs single-return form is NOT decided
    // here; that's assignment-context-driven at typecheck/codegen (Task 2),
    // exactly like the comma-ok map read.
    | primary_expr DOT LPAREN type RPAREN {
        $$ = type_assert_expr_new($1, $4);
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
    | interface_type { $$ = $1; }
    ;

struct_type:
    STRUCT LBRACE struct_field_list RBRACE {
        StructTypeNode* st = (StructTypeNode*)malloc(sizeof(StructTypeNode));
        st->base.type = AST_STRUCT_TYPE;
        st->base.pos = get_current_position();
        st->base.node_type = NULL;
        st->base.next = NULL;
        st->fields = $3;
        st->is_result_tuple = 0;   // user-written struct type
        $$ = (ASTNode*)st;
    }
    | STRUCT LBRACE RBRACE {
        StructTypeNode* st = (StructTypeNode*)malloc(sizeof(StructTypeNode));
        st->base.type = AST_STRUCT_TYPE;
        st->base.pos = get_current_position();
        st->base.node_type = NULL;
        st->base.next = NULL;
        st->fields = NULL;
        st->is_result_tuple = 0;   // user-written struct type
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
    | identifier LBRACE struct_field_list RBRACE SEMICOLON {
        IdentifierNode* ident = (IdentifierNode*)$1;
        $$ = (ASTNode*)ast_enum_variant_new(ident->name, $3, get_current_position());
        ast_node_free($1);
    }
    | identifier LBRACE RBRACE {
        IdentifierNode* ident = (IdentifierNode*)$1;
        $$ = (ASTNode*)ast_enum_variant_new(ident->name, NULL, get_current_position());
        ast_node_free($1);
    }
    | identifier LBRACE RBRACE SEMICOLON {
        IdentifierNode* ident = (IdentifierNode*)$1;
        $$ = (ASTNode*)ast_enum_variant_new(ident->name, NULL, get_current_position());
        ast_node_free($1);
    }
    ;

// Interface type (P4-1): `interface { Area() int  Less(i int, j int) bool }`
// or the empty `interface {}`. Modeled on enum_type/struct_type (juxtaposed
// list, no separators — relies on ASI/whitespace). Each method is a FuncDeclNode
// signature with body == NULL; type_from_ast (P4-2) walks them into the
// TYPE_INTERFACE method set.
interface_type:
    INTERFACE LBRACE interface_method_list RBRACE {
        $$ = (ASTNode*)ast_interface_type_new($3, get_current_position());
    }
    | INTERFACE LBRACE RBRACE {
        $$ = (ASTNode*)ast_interface_type_new(NULL, get_current_position());
    }
    ;

interface_method_list:
    interface_member { $$ = $1; }
    | interface_method_list interface_member {
        ast_add_child($1, $2);
        $$ = $1;
    }
    ;

// Wraps an interface_method with an optional trailing ';' — explicit or
// ASI-inserted (lexer.c asi_ctx, interface-body-scoped ASI mirroring the
// struct-body pilot). Needed because bare juxtaposition alone is ambiguous
// after a void method: `Inc()` followed by an identifier is grammatically
// valid as BOTH `Inc`'s own func_result (a named return type) AND the next
// method's name, and bison's shift-wins default silently picks the former
// (workarounds.md §6), absorbing the next method's name into this one's
// signature. A ';' sits outside func_result's FIRST set, so it disambiguates
// for free (clean reduce, zero new conflicts). This mirrors struct_field's
// own per-field trailing-SEMICOLON arms (parser.y struct_field, ~2295) —
// SEMICOLON attached to the MEMBER, not a list-level separator arm. A
// list-level `interface_method_list SEMICOLON interface_method` +
// `interface_method_list SEMICOLON` (trailing) pair was tried first and
// empirically produces a genuine LALR shift/reduce conflict (+1, verified
// via bison -Wcounterexamples: "interface_method_list SEMICOLON •" on
// IDENTIFIER lookahead can't distinguish "more list to come" from "trailing
// terminator" without merging lookahead sets across the two rules) — not
// adopted.
interface_member:
    interface_method { $$ = $1; }
    | interface_method SEMICOLON { $$ = $1; }
    ;

// A method signature: `Name`, `Name() ret`, `Name(params)`, `Name(params) ret`.
// Reuses func_params / func_result so signatures parse with the SAME rules as
// function decls (no new param/result grammar). Stored as a bodyless FuncDeclNode.
interface_method:
    identifier LPAREN RPAREN {
        IdentifierNode* ident = (IdentifierNode*)$1;
        FuncDeclNode* m = ast_func_decl_new(ident->name, ident->base.pos);
        m->params = NULL; m->return_type = NULL; m->body = NULL;
        ast_node_free($1);
        $$ = (ASTNode*)m;
    }
    | identifier LPAREN RPAREN func_result {
        IdentifierNode* ident = (IdentifierNode*)$1;
        FuncDeclNode* m = ast_func_decl_new(ident->name, ident->base.pos);
        m->params = NULL; m->return_type = $4; m->body = NULL;
        ast_node_free($1);
        $$ = (ASTNode*)m;
    }
    | identifier LPAREN func_params RPAREN {
        IdentifierNode* ident = (IdentifierNode*)$1;
        FuncDeclNode* m = ast_func_decl_new(ident->name, ident->base.pos);
        reinterpret_grouped_names($3); // Go grouped params `(x, y int)`
        m->params = $3; m->return_type = NULL; m->body = NULL;
        ast_node_free($1);
        $$ = (ASTNode*)m;
    }
    | identifier LPAREN func_params RPAREN func_result {
        IdentifierNode* ident = (IdentifierNode*)$1;
        FuncDeclNode* m = ast_func_decl_new(ident->name, ident->base.pos);
        reinterpret_grouped_names($3); // Go grouped params `(x, y int)`
        m->params = $3; m->return_type = $5; m->body = NULL;
        ast_node_free($1);
        $$ = (ASTNode*)m;
    }
    | identifier {
        // Embedded interface `Reader` — a bare type name in an interface body
        // (Go interface embedding: `interface { Reader; Write(...) }`). Passes
        // through as a bare IdentifierNode; type_from_ast (AST_INTERFACE_TYPE)
        // resolves it to a TYPE_INTERFACE and unions its methods into this set.
        // Distinguished from a method by the absence of '(' — LPAREN shifts into
        // the method arms above; end-of-member (next name or '}') reduces here.
        $$ = $1;
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
        $$ = struct_field_new($1, $2);
    }
    | identifier COLON type {
        // Colon-separated field syntax: `name: type` (Goo extension).
        // Used in enum variant bodies: `Circle{radius: int}`.
        $$ = struct_field_new($1, $3);
    }
    | identifier type SEMICOLON {
        $$ = struct_field_new($1, $2);
    }
    | identifier COLON type SEMICOLON {
        // Colon-separated field with semicolon terminator.
        $$ = struct_field_new($1, $3);
    }
    | identifier field_name_tail type {
        // Grouped field `X, Y, Z T` — the leading `identifier` then a
        // `COMMA identifier` chain (field_name_tail), sharing type T. A bare
        // COMMA right after the first identifier (no COLON/type yet) is what
        // distinguishes this from the single-field arms above and from the
        // `struct_field_list COMMA struct_field` list arm (whose COMMA follows
        // a fully-reduced field like `w: int`).
        $$ = make_grouped_field($1, $2, $3);
    }
    | identifier field_name_tail type SEMICOLON {
        // Grouped field with explicit/ASI-inserted terminator.
        $$ = make_grouped_field($1, $2, $3);
    }
    | identifier SEMICOLON {
        // Embedded (anonymous) field `Base;` — the ';' is explicit in
        // one-liners and ASI-inserted at newlines inside struct bodies.
        $$ = make_embedded_field(NULL, $1, 0);
    }
    | MULTIPLY identifier SEMICOLON {
        // Embedded pointer field `*Base;`.
        $$ = make_embedded_field(NULL, $2, 1);
    }
    | identifier DOT identifier SEMICOLON {
        // Qualified embedded field `pkg.Type;` (review parity, packages-B).
        // Disambiguation from `name pkg.Type` (a NAMED field with a
        // qualified type, via the `identifier type` arm above) is one-token:
        // DOT directly after the FIRST identifier can only start this arm —
        // no type may begin with DOT. Mirrors B1's type_name qualified arm.
        $$ = make_embedded_field($1, $3, 0);
    }
    | MULTIPLY identifier DOT identifier SEMICOLON {
        // Qualified embedded pointer field `*pkg.Type;`.
        $$ = make_embedded_field($2, $4, 1);
    }
    ;

// The `, Y, Z` tail of a grouped field `X, Y, Z T`: one or more trailing
// names, returned as a `next`-linked IdentifierNode chain. Kept separate from
// the struct_field_list COMMA arm so the grouped-name COMMA (right after a
// bare identifier) never competes with the field-separator COMMA.
field_name_tail:
    COMMA identifier { $$ = $2; }
    | field_name_tail COMMA identifier {
        ASTNode* n = $1;
        while (n->next) n = n->next;
        n->next = $3;
        $$ = $1;
    }
    ;

// map[K]V{k: v, …} — tagged AST_PAREN_EXPR (unused enum slot).
// keys and values are stored as two parallel `next`-chained lists.
map_lit:
    map_type LBRACE map_entry_list RBRACE        { $$ = map_literal_new($1, $3); }
    | map_type LBRACE map_entry_list COMMA RBRACE { $$ = map_literal_new($1, $3); }
    | map_type LBRACE RBRACE                      { $$ = map_literal_new($1, NULL); }
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
        $$ = struct_literal_new(strdup(type_ident->name), $3);
        ast_node_free($1);
    }
    | identifier LBRACE struct_lit_inits COMMA RBRACE {
        IdentifierNode* type_ident = (IdentifierNode*)$1;
        $$ = struct_literal_new(strdup(type_ident->name), $3);
        ast_node_free($1);
    }
    | identifier LBRACE RBRACE {
        $$ = struct_lit_empty_new($1);
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
        $$ = struct_lit_init_keyed($1, $3);
    }
    ;

map_entry_list:
    map_entry { $$ = $1; }
    | map_entry_list COMMA map_entry {
        // Append key to keys-chain; append value to values-chain
        // (the values-chain head is stashed on the keys-list-head's
        // node_type field so we can recover both from one stack
        // value).
        $$ = map_entry_list_append($1, $3);
    }
    ;

map_entry:
    expression COLON composite_value {
        // The KEY node is returned. The matching VALUE is stashed
        // on key->node_type as a side-channel; map_entry_list
        // extracts and re-chains it into a parallel values list.
        // VALUE uses composite_value (not bare expression) so a
        // brace-elided inner composite `{...}` is accepted here too,
        // e.g. map[string][]int{"a": {1, 2}} or map[string]P{"p": {X: 1}}.
        // Reuses the same elided-composite machinery (struct_literal_new(NULL, ...)
        // via composite_value's LBRACE arms) that []T{...}/[N]T{...} element
        // elision already uses; the concrete type is resolved from the map's
        // value type V at typecheck.
        $$ = map_entry_new($1, $3);
    }
    ;

slice_lit:
    LBRACKET expression_list RBRACKET {
        // `[1, 2, 3]` — slice literal. Tagged AST_SLICE_EXPR because
        // that enum slot was unused for actual slicing expressions
        // (goolang doesn't parse `arr[i:j]` today). The expression
        // list head is stored in `elements` via the same struct
        // layout (SliceLitNode shares ASTNode base).
        $$ = slice_lit_new($2, NULL);
    }
    | LBRACKET RBRACKET {
        $$ = slice_lit_new(NULL, NULL);
    }
    | slice_type LBRACE composite_elem_list RBRACE {
        // Go-standard typed slice composite literal: `[]int{1, 2, 3}`.
        // The declared slice_type ($1, an AST_SLICE_TYPE) is STORED on the
        // node so the type checker validates each element against the
        // declared element type T (rather than inferring T from the first
        // element) and stamps the literal with the declared slice type. (P3-1)
        $$ = slice_lit_new($3, $1);
    }
    | slice_type LBRACE composite_elem_list COMMA RBRACE {
        // Trailing comma in a typed slice literal: `[]int{1, 2, 3,}`.
        // gofmt emits a trailing comma on every multi-line slice literal,
        // so this is required to parse real vendored Go source. Mirrors the
        // array-literal trailing-comma rule below.
        $$ = slice_lit_new($3, $1);
    }
    | slice_type LBRACE RBRACE {
        // Empty typed slice literal: `[]int{}`. The declared element type
        // ($1) is stored so the checker stamps the correct slice type
        // (e.g. []string{} is []string, not the int32 default). (P3-1)
        $$ = slice_lit_new(NULL, $1);
    }
    /* Array composite literal `[N]T{e...}`. Mirrors the slice-literal rules but
       stores the full array_type ($1, an AST_ARRAY_TYPE carrying N + T). */
    | array_type LBRACE composite_elem_list RBRACE {
        $$ = array_lit_new($3, $1);
    }
    /* Trailing comma: Go allows (and gofmt adds) a trailing comma in a
       composite literal, e.g. the multi-line deBruijn tables `[32]byte{0, 1,}`. */
    | array_type LBRACE composite_elem_list COMMA RBRACE {
        $$ = array_lit_new($3, $1);
    }
    | array_type LBRACE RBRACE {
        $$ = array_lit_new(NULL, $1);
    }
    ;

/* The VALUE of a typed array/slice composite-literal element: either an
   ordinary expression, or an ELIDED composite literal `{...}` whose type is
   inferred from the enclosing element type T (Go's elided-type rule). The
   elided form reuses struct_lit_inits and produces a StructLiteralNode with
   type_name=NULL; the type checker resolves and stamps its type from T.
   `LBRACE` is not a valid expression start, so the alternatives have disjoint
   first-sets (no conflict). */
composite_value:
    expression { $$ = $1; }
    | LBRACE struct_lit_inits RBRACE { $$ = struct_literal_new(NULL, $2); }
    | LBRACE struct_lit_inits COMMA RBRACE { $$ = struct_literal_new(NULL, $2); }
    | LBRACE RBRACE { $$ = struct_literal_new(NULL, NULL); }
    ;

/* An element of a typed array/slice composite literal: a bare value, or a
   KEYED element `index: value` (Go sparse-table form, e.g. the utf8
   `[16]acceptRange{ 0: {locb, hicb}, 4: {locb, 0x8F} }` table). The key is a
   constant integer index; unkeyed elements continue at previous-index + 1. */
composite_elem:
    composite_value { $$ = $1; }
    | expression COLON composite_value {
        KeyedElementNode* k = (KeyedElementNode*)calloc(1, sizeof(KeyedElementNode));
        k->base.type = AST_KEYED_ELEMENT;
        k->base.pos = get_current_position();
        k->key = $1;
        k->value = $3;
        $$ = (ASTNode*)k;
    }
    ;

composite_elem_list:
    composite_elem { $$ = $1; }
    | composite_elem_list COMMA composite_elem {
        ast_add_child($1, $3);
        $$ = $1;
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
        basic->package = NULL;
        ast_node_free($1);
        $$ = (ASTNode*)basic;
    }
    // P4.2/B1: qualified type name `pkg.Type` (var/param/return/field/elem
    // positions only — NOT composite literals, see the LBRACE_BODY scope
    // cut in the goo-grammar skill / P4 sub-B design doc rider B4).
    // Grammar-only: the checker (type_from_ast's AST_BASIC_TYPE arm)
    // resolves `package` against the imported package's exports scope.
    | identifier DOT identifier {
        BasicTypeNode* basic = (BasicTypeNode*)malloc(sizeof(BasicTypeNode));
        basic->base.type = AST_BASIC_TYPE;
        basic->base.pos = get_current_position();
        basic->base.node_type = NULL;
        basic->base.next = NULL;

        IdentifierNode* pkg_ident = (IdentifierNode*)$1;
        IdentifierNode* name_ident = (IdentifierNode*)$3;
        basic->package = strdup(pkg_ident->name);
        basic->name = strdup(name_ident->name);
        ast_node_free($1);
        ast_node_free($3);
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
    LBRACKET RBRACKET_SLICE type {
        // RBRACKET_SLICE (not plain RBRACKET) is emitted by the lexer bridge
        // exactly when an empty `[]` is followed by a type — see the token
        // declaration. This keeps `[]T` (slice type) out of the same parser
        // state as the bare `[]` empty-slice literal, so no conflict is added.
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
        // func_signature's $$ (in $2) is ambiguous between "params only" and
        // "result only" and drops the result in the "both" case (see its
        // action) — build the real FuncTypeNode from the side-channel it
        // populates instead, which carries params and return_type
        // independently and correctly for all four shapes.
        FuncTypeNode* func_type_node = (FuncTypeNode*)malloc(sizeof(FuncTypeNode));
        func_type_node->base.type = AST_FUNC_TYPE;
        func_type_node->base.pos = get_current_position();
        func_type_node->base.node_type = NULL;
        func_type_node->base.next = NULL;
        func_type_node->params = g_func_signature_params;
        func_type_node->return_type = g_func_signature_result;
        g_func_signature_params = NULL;
        g_func_signature_result = NULL;
        $$ = (ASTNode*)func_type_node;
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
    // P2.9 (T3): `f() catch => -1` — the value-yielding fallback form. No
    // bound error variable; the fallback `expression` on the right is the
    // handler's recovery value. Desugars to the SAME CatchExprNode the block
    // form above builds (error_var NULL, catch_body a synthetic one-
    // statement block wrapping the fallback expression), so the existing
    // value-producing catch machinery (type_check_catch_expr's trailing-expr
    // check, generate_catch_body_value's PHI merge) runs unchanged — see
    // catch_expr_arrow_new's doc comment (parser_actions.c).
    | expression CATCH FAT_ARROW expression %prec CATCH {
        // %prec CATCH: without this, bison assigns the rule the precedence
        // of its LAST terminal (FAT_ARROW, which has none declared) instead
        // of inheriting CATCH's %right low precedence the sibling arm above
        // gets implicitly (CATCH is its only terminal). Losing that
        // precedence reopened dozens of shift/reduce ambiguities over how
        // far the trailing `expression` extends (verified: omitting this
        // pushed the tripwire from 121 to 142 S/R) — restoring it returns
        // the arm to the exact same low-precedence right-associative
        // resolution as `expression CATCH identifier block`.
        CatchExprNode* catch_node = catch_expr_arrow_new($1, $4);
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

arena_stmt:
    ARENA block {
        ArenaBlockNode* arena_node = ast_arena_block_new($2, get_current_position());
        $$ = (ASTNode*)arena_node;
    }
    ;

asm_stmt:
    ASM LBRACE STRING_LITERAL RBRACE {
        AsmStmtNode* asm_node = ast_asm_stmt_new($3.data, get_current_position());
        free($3.data);
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
        ExternDeclNode* extern_node = ast_extern_decl_new(ident->name, $2.data, $4, NULL, NULL, get_current_position());
        free($2.data);
        ast_node_free($3);
        $$ = (ASTNode*)extern_node;
    }
    | EXTERN STRING_LITERAL identifier func_signature FROM STRING_LITERAL {
        // extern "C" function_name(params) -> return_type from "library"
        IdentifierNode* ident = (IdentifierNode*)$3;
        ExternDeclNode* extern_node = ast_extern_decl_new(ident->name, $2.data, $4, NULL, $6.data, get_current_position());
        free($2.data);
        free($6.data);
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
        BarrierCallNode* barrier_node = ast_barrier_call_new($3.data, get_current_position());
        free($3.data);
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
        snprintf(int_str, sizeof(int_str), "%lld", $1);
        LiteralNode* lit = ast_literal_new(TOKEN_INT, int_str, get_current_position());
        $$ = (ASTNode*)lit;
    }
    | FLOAT_LITERAL {
        /* Shortest text that round-trips to the lexed double: try increasing
           precision until strtod(text) == value. %.17g always round-trips
           IEEE-754 double (so the loop terminates); shorter wins for
           diagnostics ("0.3", "1e+70", "3.5") and keeps LiteralNode.value
           VALUE-exact for the checker's range checks and codegen's strtod.
           Replaces the old "%f" (6 fractional digits, 64-char cap) which
           corrupted any literal needing more precision — 1e70 > 1e69
           computed false. Over-range literals still arrive here as inf
           (lexer atof saturation) and format as "inf"/"-inf"; the checker's
           finiteness rejection (float64_is_finite) owns that case. */
        char float_str[32];
        for (int prec = 15; prec <= 17; prec++) {
            snprintf(float_str, sizeof(float_str), "%.*g", prec, $1);
            if (strtod(float_str, NULL) == $1) break;
        }
        LiteralNode* lit = ast_literal_new(TOKEN_FLOAT, float_str, get_current_position());
        $$ = (ASTNode*)lit;
    }
    | STRING_LITERAL {
        LiteralNode* lit = ast_string_literal_new($1.data, $1.len, get_current_position());
        free($1.data);
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


%%

// yyerror's definition now lives in src/parser/parser_errors.c (see
// include/parser/parser_errors.h for its declaration); the `extern void
// yyerror(const char* msg);` in this file's prologue is what parser.tab.c's
// generated yyparse() links against.
