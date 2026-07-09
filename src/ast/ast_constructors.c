#include "ast.h"
#include <stdlib.h>
#include <string.h>

// AST node constructors, one ast_<kind>_new per node type.
// Split from ast.c (refactor, no behavior change). Lifecycle
// (ast_node_free / ast_node_copy) and utilities stay in ast.c.

// Per-file static strdup — house idiom (see types.c, ide/*.c).
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
    node->annotations = NULL;
    node->is_comptime = 0;
    node->is_unsafe = 0;
    node->receiver = NULL;
    node->type_params = NULL;

    return node;
}

// Closures Branch B, Task 1: func literal constructor. See FuncLitNode's doc
// comment (ast.h) for field shapes; the parser assigns params/return_type/
// body from its own RHS symbols after construction.
FuncLitNode* ast_func_lit_new(Position pos) {
    FuncLitNode* node = (FuncLitNode*)malloc(sizeof(FuncLitNode));
    if (!node) return NULL;

    node->base.type = AST_FUNC_LIT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->params = NULL;
    node->return_type = NULL;
    node->body = NULL;
    node->captured_names = NULL;
    node->captured_count = 0;

    return node;
}

ConceptDeclNode* ast_concept_decl_new(const char* name, Position pos) {
    ConceptDeclNode* node = (ConceptDeclNode*)malloc(sizeof(ConceptDeclNode));
    if (!node) return NULL;

    node->base.type = AST_CONCEPT_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->type_params = NULL;
    node->requirements = NULL;
    
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
    node->is_variadic_param = 0;
    node->is_captured = 0;
    node->is_embedded = 0;
    node->is_comptime_param = 0;

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
    // Non-string literals never contain embedded NULs, so strlen is the true
    // byte length. Strings that may contain NULs must use ast_string_literal_new.
    node->length = node->value ? strlen(node->value) : 0;

    return node;
}

LiteralNode* ast_string_literal_new(const char* data, size_t length, Position pos) {
    LiteralNode* node = (LiteralNode*)malloc(sizeof(LiteralNode));
    if (!node) return NULL;

    node->base.type = AST_LITERAL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->literal_type = TOKEN_STRING;
    node->length = length;

    // Copy exactly `length` bytes (embedded NULs preserved) plus a trailing NUL
    // so callers that still read `value` as a C string see a terminated buffer.
    char* buf = (char*)malloc(length + 1);
    if (!buf) { free(node); return NULL; }
    if (data && length) memcpy(buf, data, length);
    buf[length] = '\0';
    node->value = buf;

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
    // gofmt-syntax-b Task 4: default to "no binding" — the plain-comm and
    // default-clause call sites (and any future one) get a safe zero value
    // for free; the binding grammar arms in parser.y overwrite both fields
    // explicitly right after calling this constructor.
    node->bind_name = NULL;
    node->is_declare = 0;

    return node;
}

SwitchStmtNode* ast_switch_stmt_new(ASTNode* tag, ASTNode* cases, Position pos) {
    SwitchStmtNode* node = (SwitchStmtNode*)malloc(sizeof(SwitchStmtNode));
    if (!node) return NULL;

    node->base.type = AST_SWITCH_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->tag = tag;
    node->cases = cases;

    return node;
}

CaseClauseNode* ast_case_clause_new(ASTNode* exprs, ASTNode* body, Position pos) {
    CaseClauseNode* node = (CaseClauseNode*)malloc(sizeof(CaseClauseNode));
    if (!node) return NULL;

    node->base.type = AST_CASE_CLAUSE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->exprs = exprs;
    node->body = body;

    return node;
}

TypeSwitchNode* ast_type_switch_new(ASTNode* bind_name, ASTNode* expr, ASTNode* cases, Position pos) {
    TypeSwitchNode* node = (TypeSwitchNode*)malloc(sizeof(TypeSwitchNode));
    if (!node) return NULL;

    node->base.type = AST_TYPE_SWITCH;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->bind_name = bind_name;
    node->expr = expr;
    node->cases = cases;

    return node;
}

TypeCaseNode* ast_type_case_new(ASTNode* types, ASTNode* body, Position pos) {
    TypeCaseNode* node = (TypeCaseNode*)malloc(sizeof(TypeCaseNode));
    if (!node) return NULL;

    node->base.type = AST_TYPE_CASE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->types = types;
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

UnsafeStmtNode* ast_unsafe_stmt_new(ASTNode* body, Position pos) {
    UnsafeStmtNode* node = (UnsafeStmtNode*)malloc(sizeof(UnsafeStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_UNSAFE_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->body = body;
    
    return node;
}

ArenaBlockNode* ast_arena_block_new(ASTNode* body, Position pos) {
    ArenaBlockNode* node = (ArenaBlockNode*)malloc(sizeof(ArenaBlockNode));
    if (!node) return NULL;
    node->base.type = AST_ARENA_BLOCK;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->body = body;
    return node;
}

// gofmt-syntax-b Task 1 (P1.5): label statement + labeled break/continue.
LabelStmtNode* ast_label_stmt_new(const char* name, ASTNode* stmt, Position pos) {
    LabelStmtNode* node = (LabelStmtNode*)malloc(sizeof(LabelStmtNode));
    if (!node) return NULL;
    node->base.type = AST_LABEL_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->stmt = stmt;
    return node;
}

BreakLabelStmtNode* ast_break_label_stmt_new(const char* label, Position pos) {
    BreakLabelStmtNode* node = (BreakLabelStmtNode*)malloc(sizeof(BreakLabelStmtNode));
    if (!node) return NULL;
    node->base.type = AST_BREAK_LABEL_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->label = str_dup(label);
    return node;
}

ContinueLabelStmtNode* ast_continue_label_stmt_new(const char* label, Position pos) {
    ContinueLabelStmtNode* node = (ContinueLabelStmtNode*)malloc(sizeof(ContinueLabelStmtNode));
    if (!node) return NULL;
    node->base.type = AST_CONTINUE_LABEL_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->label = str_dup(label);
    return node;
}

// gofmt-syntax-b Task 2 (P1.6): goto statement.
GotoStmtNode* ast_goto_stmt_new(const char* label, Position pos) {
    GotoStmtNode* node = (GotoStmtNode*)malloc(sizeof(GotoStmtNode));
    if (!node) return NULL;
    node->base.type = AST_GOTO_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->label = str_dup(label);
    return node;
}

AsmStmtNode* ast_asm_stmt_new(const char* assembly_code, Position pos) {
    AsmStmtNode* node = (AsmStmtNode*)malloc(sizeof(AsmStmtNode));
    if (!node) return NULL;
    
    node->base.type = AST_ASM_STMT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->assembly_code = str_dup(assembly_code);
    node->outputs = NULL;
    node->inputs = NULL;
    node->clobbers = NULL;
    node->clobber_count = 0;
    
    return node;
}

UnsafePtrTypeNode* ast_unsafe_ptr_type_new(ASTNode* element_type, Position pos) {
    UnsafePtrTypeNode* node = (UnsafePtrTypeNode*)malloc(sizeof(UnsafePtrTypeNode));
    if (!node) return NULL;
    
    node->base.type = AST_UNSAFE_PTR_TYPE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->element_type = element_type;
    
    return node;
}

PtrArithmeticNode* ast_ptr_arithmetic_new(ASTNode* pointer, TokenType operation, ASTNode* offset, Position pos) {
    PtrArithmeticNode* node = (PtrArithmeticNode*)malloc(sizeof(PtrArithmeticNode));
    if (!node) return NULL;
    
    node->base.type = AST_PTR_ARITHMETIC;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->pointer = pointer;
    node->operation = operation;
    node->offset = offset;
    
    return node;
}

PtrDerefNode* ast_ptr_deref_new(ASTNode* pointer, Position pos) {
    PtrDerefNode* node = (PtrDerefNode*)malloc(sizeof(PtrDerefNode));
    if (!node) return NULL;
    
    node->base.type = AST_PTR_DEREF;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->pointer = pointer;
    
    return node;
}

AddrOfNode* ast_addr_of_new(ASTNode* operand, Position pos) {
    AddrOfNode* node = (AddrOfNode*)malloc(sizeof(AddrOfNode));
    if (!node) return NULL;
    
    node->base.type = AST_ADDR_OF;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->operand = operand;
    
    return node;
}

PortIONode* ast_port_io_new(ASTNode* port, ASTNode* value, int is_input, int size, Position pos) {
    PortIONode* node = (PortIONode*)malloc(sizeof(PortIONode));
    if (!node) return NULL;
    
    node->base.type = AST_PORT_IO;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->port = port;
    node->value = value;
    node->is_input = is_input;
    node->size = size;
    
    return node;
}

MMIOAccessNode* ast_mmio_access_new(ASTNode* address, ASTNode* value, int is_volatile, int size, Position pos) {
    MMIOAccessNode* node = (MMIOAccessNode*)malloc(sizeof(MMIOAccessNode));
    if (!node) return NULL;
    
    node->base.type = AST_MMIO_ACCESS;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->address = address;
    node->value = value;
    node->is_volatile = is_volatile;
    node->size = size;
    
    return node;
}

ExternDeclNode* ast_extern_decl_new(const char* name, const char* abi, ASTNode* params, ASTNode* return_type, const char* library, Position pos) {
    ExternDeclNode* node = (ExternDeclNode*)malloc(sizeof(ExternDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_EXTERN_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->abi = str_dup(abi);
    node->library = str_dup(library);
    node->params = params;
    node->return_type = return_type;
    
    return node;
}

AttributeNode* ast_attribute_new(const char* name, ASTNode* args, Position pos) {
    AttributeNode* node = (AttributeNode*)malloc(sizeof(AttributeNode));
    if (!node) return NULL;
    
    node->base.type = AST_ATTRIBUTE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->args = args;
    
    return node;
}

VolatileExprNode* ast_volatile_expr_new(ASTNode* expr, Position pos) {
    VolatileExprNode* node = (VolatileExprNode*)malloc(sizeof(VolatileExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_VOLATILE_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->expr = expr;
    
    return node;
}

ParallelForNode* ast_parallel_for_new(ASTNode* init, ASTNode* condition, ASTNode* increment, ASTNode* body, const char* schedule_type, int chunk_size, Position pos) {
    ParallelForNode* node = (ParallelForNode*)malloc(sizeof(ParallelForNode));
    if (!node) return NULL;
    
    node->base.type = AST_PARALLEL_FOR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->init = init;
    node->condition = condition;
    node->increment = increment;
    node->body = body;
    node->schedule_type = str_dup(schedule_type);
    node->chunk_size = chunk_size;
    
    return node;
}

ParallelReduceNode* ast_parallel_reduce_new(ASTNode* array, ASTNode* init_value, ASTNode* reduction_func, const char* operation, Position pos) {
    ParallelReduceNode* node = (ParallelReduceNode*)malloc(sizeof(ParallelReduceNode));
    if (!node) return NULL;
    
    node->base.type = AST_PARALLEL_REDUCE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->array = array;
    node->init_value = init_value;
    node->reduction_func = reduction_func;
    node->operation = str_dup(operation);
    
    return node;
}

BarrierCallNode* ast_barrier_call_new(const char* barrier_name, Position pos) {
    BarrierCallNode* node = (BarrierCallNode*)malloc(sizeof(BarrierCallNode));
    if (!node) return NULL;
    
    node->base.type = AST_BARRIER_CALL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->barrier_name = str_dup(barrier_name);
    
    return node;
}

AtomicExprNode* ast_atomic_expr_new(ASTNode* expr, const char* operation, ASTNode* operand, Position pos) {
    AtomicExprNode* node = (AtomicExprNode*)malloc(sizeof(AtomicExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_ATOMIC_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->expr = expr;
    node->operation = str_dup(operation);
    node->operand = operand;
    
    return node;
}

ThreadLocalDeclNode* ast_thread_local_decl_new(const char* name, ASTNode* type, ASTNode* init_value, Position pos) {
    ThreadLocalDeclNode* node = (ThreadLocalDeclNode*)malloc(sizeof(ThreadLocalDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_THREAD_LOCAL_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->type = type;
    node->init_value = init_value;
    
    return node;
}

MatchExprNode* ast_match_expr_new(ASTNode* expr, ASTNode* cases, Position pos) {
    MatchExprNode* node = (MatchExprNode*)malloc(sizeof(MatchExprNode));
    if (!node) return NULL;
    
    node->base.type = AST_MATCH_EXPR;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->expr = expr;
    node->cases = cases;
    
    return node;
}

MatchCaseNode* ast_match_case_new(ASTNode* pattern, ASTNode* guard, ASTNode* body, Position pos) {
    MatchCaseNode* node = (MatchCaseNode*)malloc(sizeof(MatchCaseNode));
    if (!node) return NULL;
    
    node->base.type = AST_MATCH_CASE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->pattern = pattern;
    node->guard = guard;
    node->body = body;
    
    return node;
}

PatternNode* ast_pattern_new(PatternType pattern_type, Position pos) {
    PatternNode* node = (PatternNode*)malloc(sizeof(PatternNode));
    if (!node) return NULL;
    
    node->base.type = AST_PATTERN;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->pattern_type = pattern_type;
    
    // Initialize the union to safe defaults
    memset(&node->data, 0, sizeof(node->data));
    
    return node;
}

GuardConditionNode* ast_guard_condition_new(ASTNode* condition, Position pos) {
    GuardConditionNode* node = (GuardConditionNode*)malloc(sizeof(GuardConditionNode));
    if (!node) return NULL;
    
    node->base.type = AST_GUARD_CONDITION;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->condition = condition;
    
    return node;
}

KernelDeclNode* ast_kernel_decl_new(const char* name, ASTNode* params, ASTNode* return_type, ASTNode* body, GPUTargetArch target_arch, Position pos) {
    KernelDeclNode* node = (KernelDeclNode*)malloc(sizeof(KernelDeclNode));
    if (!node) return NULL;
    
    node->base.type = AST_KERNEL_DECL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->params = params;
    node->return_type = return_type;
    node->body = body;
    node->target_arch = target_arch;
    node->max_threads_per_block = 1024; // Default CUDA value
    node->shared_memory_size = 0;
    node->is_inline = 0;
    
    return node;
}

KernelLaunchNode* ast_kernel_launch_new(ASTNode* kernel_func, ASTNode* grid_dim, ASTNode* block_dim, ASTNode* args, Position pos) {
    KernelLaunchNode* node = (KernelLaunchNode*)malloc(sizeof(KernelLaunchNode));
    if (!node) return NULL;
    
    node->base.type = AST_KERNEL_LAUNCH;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->kernel_func = kernel_func;
    node->grid_dim = grid_dim;
    node->block_dim = block_dim;
    node->args = args;
    node->shared_mem_size = NULL;
    node->stream = NULL;
    
    return node;
}

GPUMemoryAllocNode* ast_gpu_memory_alloc_new(ASTNode* size, ASTNode* element_type, GPUMemoryType memory_type, Position pos) {
    GPUMemoryAllocNode* node = (GPUMemoryAllocNode*)malloc(sizeof(GPUMemoryAllocNode));
    if (!node) return NULL;
    
    node->base.type = AST_GPU_MEMORY_ALLOC;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->size = size;
    node->element_type = element_type;
    node->memory_type = memory_type;
    node->is_managed = 0;
    node->alignment = 0;
    
    return node;
}

GPUMemoryCopyNode* ast_gpu_memory_copy_new(ASTNode* dest, ASTNode* src, ASTNode* size, int direction, Position pos) {
    GPUMemoryCopyNode* node = (GPUMemoryCopyNode*)malloc(sizeof(GPUMemoryCopyNode));
    if (!node) return NULL;
    
    node->base.type = AST_GPU_MEMORY_COPY;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->dest = dest;
    node->src = src;
    node->size = size;
    node->direction = direction;
    node->is_async = 0;
    node->stream = NULL;
    
    return node;
}

GPUSyncNode* ast_gpu_sync_new(int sync_type, ASTNode* stream, ASTNode* event, Position pos) {
    GPUSyncNode* node = (GPUSyncNode*)malloc(sizeof(GPUSyncNode));
    if (!node) return NULL;
    
    node->base.type = AST_GPU_SYNC;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->sync_type = sync_type;
    node->stream = stream;
    node->event = event;
    
    return node;
}

GPUIntrinsicNode* ast_gpu_intrinsic_new(const char* intrinsic_name, ASTNode* args, GPUExecutionContext context, Position pos) {
    GPUIntrinsicNode* node = (GPUIntrinsicNode*)malloc(sizeof(GPUIntrinsicNode));
    if (!node) return NULL;
    
    node->base.type = AST_GPU_INTRINSIC;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->intrinsic_name = str_dup(intrinsic_name);
    node->args = args;
    node->context = context;
    
    return node;
}

// WebAssembly constructors

WasmExportNode* ast_wasm_export_new(const char* export_name, ASTNode* item, const char* export_type, Position pos) {
    WasmExportNode* node = (WasmExportNode*)malloc(sizeof(WasmExportNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_EXPORT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->export_name = str_dup(export_name);
    node->item = item;
    node->export_type = str_dup(export_type);
    
    return node;
}

WasmImportNode* ast_wasm_import_new(const char* module_name, const char* import_name, const char* local_name, const char* import_type, ASTNode* signature, Position pos) {
    WasmImportNode* node = (WasmImportNode*)malloc(sizeof(WasmImportNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_IMPORT;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->module_name = str_dup(module_name);
    node->import_name = str_dup(import_name);
    node->local_name = str_dup(local_name);
    node->import_type = str_dup(import_type);
    node->signature = signature;
    
    return node;
}

WasmMemoryNode* ast_wasm_memory_new(ASTNode* min_pages, ASTNode* max_pages, int is_shared, Position pos) {
    WasmMemoryNode* node = (WasmMemoryNode*)malloc(sizeof(WasmMemoryNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_MEMORY;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->min_pages = min_pages;
    node->max_pages = max_pages;
    node->is_shared = is_shared;
    node->is_exported = 0;
    node->export_name = NULL;
    
    return node;
}

WasmTableNode* ast_wasm_table_new(WasmValueType element_type, ASTNode* min_size, ASTNode* max_size, Position pos) {
    WasmTableNode* node = (WasmTableNode*)malloc(sizeof(WasmTableNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_TABLE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->element_type = element_type;
    node->min_size = min_size;
    node->max_size = max_size;
    node->is_exported = 0;
    node->export_name = NULL;
    
    return node;
}

WasmGlobalNode* ast_wasm_global_new(const char* name, WasmValueType value_type, int is_mutable, ASTNode* init_value, Position pos) {
    WasmGlobalNode* node = (WasmGlobalNode*)malloc(sizeof(WasmGlobalNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_GLOBAL;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->value_type = value_type;
    node->is_mutable = is_mutable;
    node->init_value = init_value;
    node->is_exported = 0;
    node->export_name = NULL;
    
    return node;
}

WasmTypeNode* ast_wasm_type_new(const char* name, ASTNode* params, ASTNode* results, Position pos) {
    WasmTypeNode* node = (WasmTypeNode*)malloc(sizeof(WasmTypeNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_TYPE;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->name = str_dup(name);
    node->params = params;
    node->results = results;
    
    return node;
}

WasmStartNode* ast_wasm_start_new(ASTNode* function, Position pos) {
    WasmStartNode* node = (WasmStartNode*)malloc(sizeof(WasmStartNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_START;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->function = function;
    
    return node;
}

WasmElemNode* ast_wasm_elem_new(ASTNode* table_index, ASTNode* offset, ASTNode* elements, Position pos) {
    WasmElemNode* node = (WasmElemNode*)malloc(sizeof(WasmElemNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_ELEM;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->table_index = table_index;
    node->offset = offset;
    node->elements = elements;
    
    return node;
}

WasmDataNode* ast_wasm_data_new(ASTNode* memory_index, ASTNode* offset, ASTNode* data, Position pos) {
    WasmDataNode* node = (WasmDataNode*)malloc(sizeof(WasmDataNode));
    if (!node) return NULL;
    
    node->base.type = AST_WASM_DATA;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->memory_index = memory_index;
    node->offset = offset;
    node->data = data;
    
    return node;
}

JSInteropNode* ast_js_interop_new(JSInteropType interop_type, const char* object_name, const char* property_name, ASTNode* args, WasmEnvironment target_env, Position pos) {
    JSInteropNode* node = (JSInteropNode*)malloc(sizeof(JSInteropNode));
    if (!node) return NULL;
    
    node->base.type = AST_JS_INTEROP;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->interop_type = interop_type;
    node->object_name = str_dup(object_name);
    node->property_name = str_dup(property_name);
    node->args = args;
    node->target_env = target_env;
    
    return node;
}

DOMAccessNode* ast_dom_access_new(const char* api_name, const char* method_name, ASTNode* args, int is_property, Position pos) {
    DOMAccessNode* node = (DOMAccessNode*)malloc(sizeof(DOMAccessNode));
    if (!node) return NULL;
    
    node->base.type = AST_DOM_ACCESS;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->api_name = str_dup(api_name);
    node->method_name = str_dup(method_name);
    node->args = args;
    node->is_property = is_property;
    
    return node;
}

// Utility functions

EnumTypeNode* ast_enum_type_new(ASTNode* variants, Position pos) {
    EnumTypeNode* n = (EnumTypeNode*)calloc(1, sizeof(EnumTypeNode));
    if (!n) return NULL;
    n->base.type = AST_ENUM_TYPE;
    n->base.pos = pos;
    n->base.node_type = NULL;
    n->base.next = NULL;
    n->variants = variants;
    return n;
}

InterfaceTypeNode* ast_interface_type_new(ASTNode* methods, Position pos) {
    InterfaceTypeNode* n = (InterfaceTypeNode*)calloc(1, sizeof(InterfaceTypeNode));
    if (!n) return NULL;
    n->base.type = AST_INTERFACE_TYPE;
    n->base.pos = pos;
    n->base.node_type = NULL;
    n->base.next = NULL;
    n->methods = methods;
    return n;
}

EnumVariantNode* ast_enum_variant_new(const char* name, ASTNode* fields, Position pos) {
    EnumVariantNode* n = (EnumVariantNode*)calloc(1, sizeof(EnumVariantNode));
    if (!n) return NULL;
    n->base.type = AST_ENUM_VARIANT;
    n->base.pos = pos;
    n->base.node_type = NULL;
    n->base.next = NULL;
    n->name = name ? strdup(name) : NULL;
    n->fields = fields;
    return n;
}

