// program_dump: the typed-program interchange the front-end migration is
// measured against. One JSON object per node, a type table by id, one plan
// per file. Every kind the walker does not know aborts BY NAME — a dump that
// silently skipped a node would let a differential gate pass on a fixture
// it never actually compared.
#include "program_dump.h"
#include "json_writer.h"
#include "types.h"
#include "token.h"
#include "escape_core.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- type table ------------------------------------------------------------

typedef struct {
    Type** items;
    size_t count, cap;
} TypeTable;

static TypeTable g_types;

static long long type_id(Type* t) {
    if (!t) return -1;
    for (size_t i = 0; i < g_types.count; i++) if (g_types.items[i] == t) return (long long)i;
    if (g_types.count == g_types.cap) {
        g_types.cap = g_types.cap ? g_types.cap * 2 : 64;
        g_types.items = realloc(g_types.items, g_types.cap * sizeof(Type*));
        if (!g_types.items) abort();
    }
    g_types.items[g_types.count] = t;
    return (long long)g_types.count++;
}

// ---- helpers ---------------------------------------------------------------

static ProgramDumpStage g_stage;
static const char* g_selftest;   // GOO_DUMP_SELFTEST, or NULL
static int g_nodes_emitted;
static const char* g_current_file; // name of the file entry a node's pos is compared against

static void die_kind(const char* what, int kind) {
    fprintf(stderr, "program-dump: unsupported %s kind %d\n", what, kind);
    abort();
}

// One name per ASTNodeType, in include/ast.h order. The static_assert pins
// the count to AST_NODE_COUNT: a kind added to the enum without a name here
// is a compile error, not an out-of-range read inside the abort message. The
// probe's red rows are only useful because they name the kind.
static const char* const ast_kind_names[] = {
    "AST_PROGRAM", "AST_PACKAGE_DECL", "AST_IMPORT_SPEC", "AST_FUNC_DECL", "AST_VAR_DECL",
    "AST_CONST_DECL", "AST_TYPE_DECL", "AST_CONCEPT_DECL", "AST_HKT_PARAM", "AST_BLOCK_STMT",
    "AST_EXPR_STMT", "AST_IF_STMT", "AST_IF_LET_STMT", "AST_FOR_STMT", "AST_RETURN_STMT",
    "AST_BREAK_STMT", "AST_CONTINUE_STMT", "AST_DEFER_STMT", "AST_GO_STMT", "AST_SELECT_STMT",
    "AST_SELECT_CASE", "AST_SWITCH_STMT", "AST_CASE_CLAUSE", "AST_DEFAULT_CLAUSE",
    "AST_UNSAFE_STMT", "AST_ASM_STMT", "AST_IDENTIFIER", "AST_LITERAL", "AST_BINARY_EXPR",
    "AST_UNARY_EXPR", "AST_POSTFIX_EXPR", "AST_CALL_EXPR", "AST_INDEX_EXPR",
    "AST_SELECTOR_EXPR", "AST_SLICE_EXPR", "AST_TYPE_ASSERT_EXPR", "AST_PAREN_EXPR",
    "AST_BASIC_TYPE", "AST_ARRAY_TYPE", "AST_SLICE_TYPE", "AST_MAP_TYPE", "AST_CHAN_TYPE",
    "AST_FUNC_TYPE", "AST_INTERFACE_TYPE", "AST_STRUCT_TYPE", "AST_POINTER_TYPE",
    "AST_REFERENCE_TYPE", "AST_ERROR_UNION_TYPE", "AST_NULLABLE_TYPE", "AST_TRY_EXPR",
    "AST_CATCH_EXPR", "AST_COMPTIME_BLOCK", "AST_OWNERSHIP_QUAL", "AST_UNSAFE_PTR_TYPE",
    "AST_PTR_ARITHMETIC", "AST_PTR_DEREF", "AST_ADDR_OF", "AST_PORT_IO", "AST_MMIO_ACCESS",
    "AST_EXTERN_DECL", "AST_ATTRIBUTE", "AST_VOLATILE_EXPR", "AST_PARALLEL_FOR",
    "AST_PARALLEL_REDUCE", "AST_BARRIER_CALL", "AST_ATOMIC_EXPR", "AST_THREAD_LOCAL_DECL",
    "AST_MATCH_EXPR", "AST_MATCH_CASE", "AST_PATTERN", "AST_GUARD_CONDITION", "AST_KERNEL_DECL",
    "AST_KERNEL_LAUNCH", "AST_GPU_MEMORY_ALLOC", "AST_GPU_MEMORY_COPY", "AST_GPU_SYNC",
    "AST_GPU_INTRINSIC", "AST_CONTRACT_CLAUSE", "AST_REQUIRES_CLAUSE", "AST_ENSURES_CLAUSE",
    "AST_INVARIANT_CLAUSE", "AST_ASSERT_STMT", "AST_ASSUME_STMT", "AST_CONTRACT_BLOCK",
    "AST_WASM_EXPORT", "AST_WASM_IMPORT", "AST_WASM_MEMORY", "AST_WASM_TABLE",
    "AST_WASM_GLOBAL", "AST_WASM_TYPE", "AST_WASM_START", "AST_WASM_ELEM", "AST_WASM_DATA",
    "AST_JS_INTEROP", "AST_DOM_ACCESS", "AST_STRUCT_LITERAL", "AST_ENUM_TYPE",
    "AST_ENUM_VARIANT", "AST_SLICE_INDEX_EXPR", "AST_MULTI_ASSIGN", "AST_ARRAY_LITERAL",
    "AST_KEYED_ELEMENT", "AST_FUNC_LIT", "AST_SLICE_CONVERSION", "AST_TYPE_ASSERT",
    "AST_TYPE_SWITCH", "AST_TYPE_CASE", "AST_ARENA_BLOCK", "AST_LABEL_STMT",
    "AST_BREAK_LABEL_STMT", "AST_CONTINUE_LABEL_STMT", "AST_GOTO_STMT", "AST_FALLTHROUGH_STMT",
};
static_assert(sizeof(ast_kind_names) / sizeof(ast_kind_names[0]) == AST_NODE_COUNT,
              "ast_kind_names must list every ASTNodeType in enum order");

static void die_ast_kind(int kind) {
    const char* name = (kind >= 0 && kind < AST_NODE_COUNT) ? ast_kind_names[kind] : "out of range";
    fprintf(stderr, "program-dump: unsupported AST node kind %d (%s)\n", kind, name);
    abort();
}

static void emit_node(JsonW* w, ASTNode* n);

static void emit_list(JsonW* w, const char* key, ASTNode* head) {
    jw_key(w, key);
    jw_begin_array(w);
    for (ASTNode* n = head; n; n = n->next) emit_node(w, n);
    jw_end_array(w);
}

static void emit_child(JsonW* w, const char* key, ASTNode* n) {
    jw_key(w, key);
    if (n) emit_node(w, n); else jw_null(w);
}

static void emit_str(JsonW* w, const char* key, const char* s) { jw_key(w, key); jw_string(w, s); }
static void emit_int(JsonW* w, const char* key, long long v)   { jw_key(w, key); jw_int(w, v); }
static void emit_bool(JsonW* w, const char* key, int v)        { jw_key(w, key); jw_bool(w, v != 0); }

static void emit_names(JsonW* w, const char* key, char** names, size_t count) {
    jw_key(w, key);
    jw_begin_array(w);
    for (size_t i = 0; i < count; i++) jw_string(w, names[i]);
    jw_end_array(w);
}

// Opens the node object and writes the fields every node has. The caller
// writes the kind-specific fields and closes the object.
static void begin_node(JsonW* w, ASTNode* n, const char* kind) {
    jw_begin_object(w);
    emit_str(w, "kind", kind);
    // Teeth for the probe's structural check: drop pos from the first node.
    if (!(g_selftest && strcmp(g_selftest, "nopos") == 0 && g_nodes_emitted == 0)) {
        jw_key(w, "pos");
        jw_begin_array(w);
        jw_int(w, n->pos.line); jw_int(w, n->pos.column); jw_int(w, n->pos.offset);
        jw_end_array(w);
    }
    // A node whose position names a file other than the current file entry
    // (e.g. a synthesized node carrying source from elsewhere) records that
    // file explicitly, so the dump does not silently attribute it to the
    // wrong file.
    if (n->pos.filename && (!g_current_file || strcmp(n->pos.filename, g_current_file) != 0)) {
        emit_str(w, "file", n->pos.filename);
    }
    g_nodes_emitted++;
    if (g_stage == PROGRAM_DUMP_TYPED && n->node_type) emit_int(w, "type", type_id(n->node_type));
}

static const char* ownership_name(OwnershipKind k) {
    switch (k) {
        case OWNERSHIP_NONE: return "OWNERSHIP_NONE";
        case OWNERSHIP_OWNED: return "OWNERSHIP_OWNED";
        case OWNERSHIP_BORROWED: return "OWNERSHIP_BORROWED";
        case OWNERSHIP_SHARED: return "OWNERSHIP_SHARED";
    }
    die_kind("ownership", k); return NULL;
}

static const char* pattern_name(PatternType p) {
    switch (p) {
        case PATTERN_LITERAL: return "PATTERN_LITERAL";
        case PATTERN_IDENTIFIER: return "PATTERN_IDENTIFIER";
        case PATTERN_WILDCARD: return "PATTERN_WILDCARD";
        case PATTERN_DESTRUCTURE: return "PATTERN_DESTRUCTURE";
        case PATTERN_TYPE: return "PATTERN_TYPE";
        case PATTERN_OR: return "PATTERN_OR";
    }
    die_kind("pattern", p); return NULL;
}

static const char* chan_pattern_name(ChannelPattern p) {
    switch (p) {
        case CHAN_PATTERN_BASIC: return "CHAN_PATTERN_BASIC";
        case CHAN_PATTERN_PUB: return "CHAN_PATTERN_PUB";
        case CHAN_PATTERN_SUB: return "CHAN_PATTERN_SUB";
        case CHAN_PATTERN_REQ: return "CHAN_PATTERN_REQ";
        case CHAN_PATTERN_REP: return "CHAN_PATTERN_REP";
        case CHAN_PATTERN_PUSH: return "CHAN_PATTERN_PUSH";
        case CHAN_PATTERN_PULL: return "CHAN_PATTERN_PULL";
    }
    die_kind("channel pattern", p); return NULL;
}

// token_type_string is the lexer's own name table (src/lexer/token.c:247);
// reusing it keeps the operator spelling identical to --emit-tokens.
static void emit_tok(JsonW* w, const char* key, TokenType t) { emit_str(w, key, token_type_string(t)); }

// ---- nodes -----------------------------------------------------------------

static void emit_node(JsonW* w, ASTNode* n) {
    switch (n->type) {
        case AST_PACKAGE_DECL: {
            PackageDeclNode* p = (PackageDeclNode*)n;
            begin_node(w, n, "PACKAGE_DECL"); emit_str(w, "name", p->name); jw_end_object(w); break;
        }
        case AST_IMPORT_SPEC: {
            ImportSpecNode* p = (ImportSpecNode*)n;
            begin_node(w, n, "IMPORT_SPEC"); emit_str(w, "path", p->path); emit_str(w, "alias", p->alias); jw_end_object(w); break;
        }
        case AST_FUNC_DECL: {
            FuncDeclNode* f = (FuncDeclNode*)n;
            begin_node(w, n, "FUNC_DECL");
            emit_str(w, "name", f->name);
            emit_bool(w, "is_comptime", f->is_comptime);
            emit_bool(w, "is_unsafe", f->is_unsafe);
            emit_bool(w, "has_receiver", f->receiver != NULL);   // receiver is params[0]
            emit_list(w, "type_params", f->type_params);
            emit_list(w, "params", f->params);
            emit_child(w, "return_type", f->return_type);
            emit_list(w, "annotations", f->annotations);
            emit_child(w, "body", f->body);
            jw_end_object(w); break;
        }
        case AST_VAR_DECL: {
            VarDeclNode* v = (VarDeclNode*)n;
            begin_node(w, n, "VAR_DECL");
            emit_names(w, "names", v->names, v->name_count);
            emit_child(w, "decl_type", v->type);
            emit_list(w, "values", v->values);
            emit_str(w, "ownership", ownership_name(v->ownership));
            emit_bool(w, "is_short_decl", v->is_short_decl);
            emit_bool(w, "is_variadic_param", v->is_variadic_param);
            emit_bool(w, "is_captured", v->is_captured);
            emit_bool(w, "is_embedded", v->is_embedded);
            emit_bool(w, "is_comptime_param", v->is_comptime_param);
            jw_end_object(w); break;
        }
        case AST_CONST_DECL: {
            ConstDeclNode* c = (ConstDeclNode*)n;
            begin_node(w, n, "CONST_DECL");
            emit_names(w, "names", c->names, c->name_count);
            emit_child(w, "decl_type", c->type);
            emit_list(w, "values", c->values);
            emit_bool(w, "is_comptime", c->is_comptime);
            jw_end_object(w); break;
        }
        case AST_TYPE_DECL: {
            TypeDeclNode* t = (TypeDeclNode*)n;
            begin_node(w, n, "TYPE_DECL"); emit_str(w, "name", t->name); emit_child(w, "decl_type", t->type); jw_end_object(w); break;
        }
        case AST_EXTERN_DECL: {
            ExternDeclNode* e = (ExternDeclNode*)n;
            begin_node(w, n, "EXTERN_DECL");
            emit_str(w, "name", e->name); emit_str(w, "abi", e->abi);
            emit_list(w, "params", e->params); emit_child(w, "return_type", e->return_type);
            emit_str(w, "library", e->library);
            jw_end_object(w); break;
        }
        case AST_ATTRIBUTE: {
            AttributeNode* a = (AttributeNode*)n;
            begin_node(w, n, "ATTRIBUTE"); emit_str(w, "name", a->name); emit_list(w, "args", a->args); jw_end_object(w); break;
        }
        case AST_BLOCK_STMT: {
            begin_node(w, n, "BLOCK_STMT"); emit_list(w, "statements", ((BlockStmtNode*)n)->statements); jw_end_object(w); break;
        }
        case AST_EXPR_STMT: {
            begin_node(w, n, "EXPR_STMT"); emit_child(w, "expr", ((ExprStmtNode*)n)->expr); jw_end_object(w); break;
        }
        case AST_IF_STMT: {
            IfStmtNode* s = (IfStmtNode*)n;
            begin_node(w, n, "IF_STMT");
            emit_child(w, "condition", s->condition); emit_child(w, "then", s->then_stmt); emit_child(w, "else", s->else_stmt);
            jw_end_object(w); break;
        }
        case AST_IF_LET_STMT: {
            IfLetStmtNode* s = (IfLetStmtNode*)n;
            begin_node(w, n, "IF_LET_STMT");
            emit_str(w, "var_name", s->var_name); emit_child(w, "nullable_expr", s->nullable_expr);
            emit_child(w, "then", s->then_stmt); emit_child(w, "else", s->else_stmt);
            jw_end_object(w); break;
        }
        case AST_FOR_STMT: {
            ForStmtNode* s = (ForStmtNode*)n;
            begin_node(w, n, "FOR_STMT");
            emit_child(w, "init", s->init); emit_child(w, "condition", s->condition); emit_child(w, "post", s->post);
            emit_child(w, "range_expr", s->range_expr); emit_str(w, "key_name", s->key_name); emit_str(w, "value_name", s->value_name);
            emit_child(w, "body", s->body);
            jw_end_object(w); break;
        }
        case AST_RETURN_STMT: {
            begin_node(w, n, "RETURN_STMT"); emit_list(w, "values", ((ReturnStmtNode*)n)->values); jw_end_object(w); break;
        }
        case AST_BREAK_STMT:       begin_node(w, n, "BREAK_STMT"); jw_end_object(w); break;
        case AST_CONTINUE_STMT:    begin_node(w, n, "CONTINUE_STMT"); jw_end_object(w); break;
        case AST_FALLTHROUGH_STMT: begin_node(w, n, "FALLTHROUGH_STMT"); jw_end_object(w); break;
        case AST_DEFER_STMT: {
            begin_node(w, n, "DEFER_STMT"); emit_child(w, "call", ((DeferStmtNode*)n)->call); jw_end_object(w); break;
        }
        case AST_GO_STMT: {
            begin_node(w, n, "GO_STMT"); emit_child(w, "call", ((GoStmtNode*)n)->call); jw_end_object(w); break;
        }
        case AST_SELECT_STMT: {
            begin_node(w, n, "SELECT_STMT"); emit_list(w, "cases", ((SelectStmtNode*)n)->cases); jw_end_object(w); break;
        }
        case AST_SELECT_CASE: {
            SelectCaseNode* c = (SelectCaseNode*)n;
            begin_node(w, n, "SELECT_CASE");
            emit_child(w, "comm", c->comm); emit_list(w, "body", c->body);
            emit_str(w, "bind_name", c->bind_name); emit_int(w, "is_declare", c->is_declare);
            jw_end_object(w); break;
        }
        case AST_SWITCH_STMT: {
            SwitchStmtNode* s = (SwitchStmtNode*)n;
            begin_node(w, n, "SWITCH_STMT"); emit_child(w, "tag", s->tag); emit_list(w, "cases", s->cases); jw_end_object(w); break;
        }
        case AST_CASE_CLAUSE: {
            CaseClauseNode* c = (CaseClauseNode*)n;
            begin_node(w, n, "CASE_CLAUSE"); emit_list(w, "exprs", c->exprs); emit_list(w, "body", c->body); jw_end_object(w); break;
        }
        case AST_TYPE_SWITCH: {
            TypeSwitchNode* s = (TypeSwitchNode*)n;
            begin_node(w, n, "TYPE_SWITCH");
            emit_child(w, "bind_name", s->bind_name); emit_child(w, "expr", s->expr); emit_list(w, "cases", s->cases);
            jw_end_object(w); break;
        }
        case AST_TYPE_CASE: {
            TypeCaseNode* c = (TypeCaseNode*)n;
            begin_node(w, n, "TYPE_CASE"); emit_list(w, "types", c->types); emit_list(w, "body", c->body); jw_end_object(w); break;
        }
        case AST_UNSAFE_STMT: {
            begin_node(w, n, "UNSAFE_STMT"); emit_child(w, "body", ((UnsafeStmtNode*)n)->body); jw_end_object(w); break;
        }
        case AST_ARENA_BLOCK: {
            begin_node(w, n, "ARENA_BLOCK"); emit_child(w, "body", ((ArenaBlockNode*)n)->body); jw_end_object(w); break;
        }
        case AST_COMPTIME_BLOCK: {
            begin_node(w, n, "COMPTIME_BLOCK"); emit_child(w, "body", ((ComptimeBlockNode*)n)->body); jw_end_object(w); break;
        }
        case AST_LABEL_STMT: {
            LabelStmtNode* s = (LabelStmtNode*)n;
            begin_node(w, n, "LABEL_STMT"); emit_str(w, "name", s->name); emit_child(w, "stmt", s->stmt); jw_end_object(w); break;
        }
        case AST_BREAK_LABEL_STMT: {
            begin_node(w, n, "BREAK_LABEL_STMT"); emit_str(w, "label", ((BreakLabelStmtNode*)n)->label); jw_end_object(w); break;
        }
        case AST_CONTINUE_LABEL_STMT: {
            begin_node(w, n, "CONTINUE_LABEL_STMT"); emit_str(w, "label", ((ContinueLabelStmtNode*)n)->label); jw_end_object(w); break;
        }
        case AST_GOTO_STMT: {
            begin_node(w, n, "GOTO_STMT"); emit_str(w, "label", ((GotoStmtNode*)n)->label); jw_end_object(w); break;
        }
        case AST_MULTI_ASSIGN: {
            MultiAssignNode* m = (MultiAssignNode*)n;
            begin_node(w, n, "MULTI_ASSIGN");
            emit_list(w, "targets", m->targets); emit_list(w, "values", m->values);
            emit_int(w, "count", (long long)m->count); emit_bool(w, "is_short_decl", m->is_short_decl);
            jw_end_object(w); break;
        }
        case AST_IDENTIFIER: {
            begin_node(w, n, "IDENTIFIER"); emit_str(w, "name", ((IdentifierNode*)n)->name); jw_end_object(w); break;
        }
        case AST_LITERAL: {
            LiteralNode* l = (LiteralNode*)n;
            begin_node(w, n, "LITERAL");
            emit_tok(w, "literal_type", l->literal_type);
            jw_key(w, "value"); jw_string_len(w, l->value, l->length);
            emit_int(w, "length", (long long)l->length);
            jw_end_object(w); break;
        }
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)n;
            begin_node(w, n, "BINARY_EXPR"); emit_tok(w, "op", b->operator); emit_child(w, "left", b->left); emit_child(w, "right", b->right); jw_end_object(w); break;
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* u = (UnaryExprNode*)n;
            begin_node(w, n, "UNARY_EXPR"); emit_tok(w, "op", u->operator); emit_child(w, "operand", u->operand); jw_end_object(w); break;
        }
        case AST_POSTFIX_EXPR: {
            PostfixExprNode* p = (PostfixExprNode*)n;
            begin_node(w, n, "POSTFIX_EXPR"); emit_tok(w, "op", p->operator); emit_child(w, "operand", p->operand); jw_end_object(w); break;
        }
        case AST_CALL_EXPR: {
            CallExprNode* c = (CallExprNode*)n;
            begin_node(w, n, "CALL_EXPR");
            emit_child(w, "function", c->function); emit_list(w, "args", c->args);
            emit_bool(w, "has_spread", c->has_spread);
            jw_key(w, "type_args"); jw_begin_array(w);
            if (g_stage == PROGRAM_DUMP_TYPED) for (size_t i = 0; i < c->type_arg_count; i++) jw_int(w, type_id(c->type_args[i]));
            jw_end_array(w);
            jw_key(w, "comptime_value_args"); jw_begin_array(w);
            for (size_t i = 0; i < c->comptime_value_arg_count; i++) jw_int(w, (long long)c->comptime_value_args[i]);
            jw_end_array(w);
            jw_end_object(w); break;
        }
        case AST_INDEX_EXPR: {
            IndexExprNode* e = (IndexExprNode*)n;
            begin_node(w, n, "INDEX_EXPR"); emit_child(w, "expr", e->expr); emit_child(w, "index", e->index); jw_end_object(w); break;
        }
        case AST_SELECTOR_EXPR: {
            SelectorExprNode* e = (SelectorExprNode*)n;
            begin_node(w, n, "SELECTOR_EXPR"); emit_child(w, "expr", e->expr); emit_str(w, "selector", e->selector); jw_end_object(w); break;
        }
        case AST_SLICE_EXPR: {   // SliceLitNode: the enum slot is reused for slice literals
            SliceLitNode* s = (SliceLitNode*)n;
            begin_node(w, n, "SLICE_LIT"); emit_list(w, "elements", s->elements); emit_child(w, "elem_type", s->elem_type); jw_end_object(w); break;
        }
        case AST_PAREN_EXPR: {   // MapLitNode: the enum slot is reused for map literals
            MapLitNode* m = (MapLitNode*)n;
            begin_node(w, n, "MAP_LIT"); emit_child(w, "map_type", m->map_type); emit_list(w, "keys", m->keys); emit_list(w, "values", m->values); jw_end_object(w); break;
        }
        case AST_SLICE_INDEX_EXPR: {
            SliceIndexExprNode* e = (SliceIndexExprNode*)n;
            begin_node(w, n, "SLICE_INDEX_EXPR"); emit_child(w, "expr", e->expr); emit_child(w, "low", e->low); emit_child(w, "high", e->high); jw_end_object(w); break;
        }
        case AST_STRUCT_LITERAL: {
            StructLiteralNode* s = (StructLiteralNode*)n;
            begin_node(w, n, "STRUCT_LITERAL");
            emit_str(w, "type_name", s->type_name); emit_bool(w, "is_keyed", s->is_keyed);
            jw_key(w, "field_names"); jw_begin_array(w);
            if (s->field_names) for (size_t i = 0; i < s->field_count; i++) jw_string(w, s->field_names[i]);
            jw_end_array(w);
            emit_list(w, "field_values", s->field_values);
            emit_int(w, "field_count", (long long)s->field_count);
            jw_end_object(w); break;
        }
        case AST_ARRAY_LITERAL: {
            ArrayLitNode* a = (ArrayLitNode*)n;
            begin_node(w, n, "ARRAY_LITERAL"); emit_child(w, "array_type", a->array_type); emit_list(w, "elements", a->elements); jw_end_object(w); break;
        }
        case AST_KEYED_ELEMENT: {
            KeyedElementNode* k = (KeyedElementNode*)n;
            begin_node(w, n, "KEYED_ELEMENT"); emit_child(w, "key", k->key); emit_child(w, "value", k->value); jw_end_object(w); break;
        }
        case AST_FUNC_LIT: {
            FuncLitNode* f = (FuncLitNode*)n;
            begin_node(w, n, "FUNC_LIT");
            emit_list(w, "params", f->params); emit_child(w, "return_type", f->return_type); emit_child(w, "body", f->body);
            emit_names(w, "captured_names", f->captured_names, f->captured_count);
            jw_end_object(w); break;
        }
        case AST_SLICE_CONVERSION: {
            SliceConvNode* s = (SliceConvNode*)n;
            begin_node(w, n, "SLICE_CONVERSION"); emit_child(w, "slice_type", s->slice_type); emit_child(w, "operand", s->operand); jw_end_object(w); break;
        }
        case AST_TYPE_ASSERT: {
            TypeAssertNode* t = (TypeAssertNode*)n;
            begin_node(w, n, "TYPE_ASSERT"); emit_child(w, "expr", t->expr); emit_child(w, "asserted_type", t->asserted_type); jw_end_object(w); break;
        }
        case AST_TRY_EXPR: {
            begin_node(w, n, "TRY_EXPR"); emit_child(w, "expr", ((TryExprNode*)n)->expr); jw_end_object(w); break;
        }
        case AST_CATCH_EXPR: {
            CatchExprNode* c = (CatchExprNode*)n;
            begin_node(w, n, "CATCH_EXPR"); emit_child(w, "expr", c->expr); emit_str(w, "error_var", c->error_var); emit_child(w, "catch_body", c->catch_body); jw_end_object(w); break;
        }
        case AST_MATCH_EXPR: {
            MatchExprNode* m = (MatchExprNode*)n;
            begin_node(w, n, "MATCH_EXPR"); emit_child(w, "expr", m->expr); emit_list(w, "cases", m->cases); jw_end_object(w); break;
        }
        case AST_MATCH_CASE: {
            MatchCaseNode* c = (MatchCaseNode*)n;
            begin_node(w, n, "MATCH_CASE"); emit_child(w, "pattern", c->pattern); emit_child(w, "guard", c->guard); emit_list(w, "body", c->body); jw_end_object(w); break;
        }
        case AST_GUARD_CONDITION: {
            begin_node(w, n, "GUARD_CONDITION"); emit_child(w, "condition", ((GuardConditionNode*)n)->condition); jw_end_object(w); break;
        }
        case AST_PATTERN: {
            PatternNode* p = (PatternNode*)n;
            begin_node(w, n, "PATTERN");
            emit_str(w, "pattern_type", pattern_name(p->pattern_type));
            switch (p->pattern_type) {
                case PATTERN_LITERAL: emit_child(w, "literal", p->data.literal.literal); break;
                case PATTERN_IDENTIFIER: emit_str(w, "name", p->data.identifier.name); emit_child(w, "id_type", p->data.identifier.type); break;
                case PATTERN_WILDCARD: break;
                case PATTERN_DESTRUCTURE:
                case PATTERN_TYPE: emit_str(w, "type_name", p->data.destructure.type_name); emit_list(w, "fields", p->data.destructure.fields); break;
                case PATTERN_OR: emit_list(w, "patterns", p->data.or_pattern.patterns); break;
            }
            jw_end_object(w); break;
        }
        case AST_BASIC_TYPE: {
            BasicTypeNode* t = (BasicTypeNode*)n;
            begin_node(w, n, "BASIC_TYPE"); emit_str(w, "name", t->name); emit_str(w, "package", t->package); jw_end_object(w); break;
        }
        case AST_ARRAY_TYPE: {
            ArrayTypeNode* t = (ArrayTypeNode*)n;
            begin_node(w, n, "ARRAY_TYPE"); emit_child(w, "length", t->length); emit_child(w, "element_type", t->element_type); jw_end_object(w); break;
        }
        case AST_SLICE_TYPE: {
            begin_node(w, n, "SLICE_TYPE"); emit_child(w, "element_type", ((SliceTypeNode*)n)->element_type); jw_end_object(w); break;
        }
        case AST_MAP_TYPE: {
            MapTypeNode* t = (MapTypeNode*)n;
            begin_node(w, n, "MAP_TYPE"); emit_child(w, "key_type", t->key_type); emit_child(w, "value_type", t->value_type); jw_end_object(w); break;
        }
        case AST_CHAN_TYPE: {
            ChanTypeNode* t = (ChanTypeNode*)n;
            begin_node(w, n, "CHAN_TYPE"); emit_child(w, "element_type", t->element_type);
            emit_str(w, "pattern", chan_pattern_name(t->pattern)); emit_str(w, "endpoint", t->endpoint);
            jw_end_object(w); break;
        }
        case AST_FUNC_TYPE: {
            FuncTypeNode* t = (FuncTypeNode*)n;
            begin_node(w, n, "FUNC_TYPE"); emit_list(w, "params", t->params); emit_child(w, "return_type", t->return_type); jw_end_object(w); break;
        }
        case AST_INTERFACE_TYPE: {
            begin_node(w, n, "INTERFACE_TYPE"); emit_list(w, "methods", ((InterfaceTypeNode*)n)->methods); jw_end_object(w); break;
        }
        case AST_STRUCT_TYPE: {
            StructTypeNode* t = (StructTypeNode*)n;
            begin_node(w, n, "STRUCT_TYPE"); emit_list(w, "fields", t->fields); emit_bool(w, "is_result_tuple", t->is_result_tuple); jw_end_object(w); break;
        }
        case AST_ENUM_TYPE: {
            begin_node(w, n, "ENUM_TYPE"); emit_list(w, "variants", ((EnumTypeNode*)n)->variants); jw_end_object(w); break;
        }
        case AST_ENUM_VARIANT: {
            EnumVariantNode* v = (EnumVariantNode*)n;
            begin_node(w, n, "ENUM_VARIANT"); emit_str(w, "name", v->name); emit_list(w, "fields", v->fields); jw_end_object(w); break;
        }
        case AST_POINTER_TYPE: {
            begin_node(w, n, "POINTER_TYPE"); emit_child(w, "element_type", ((PointerTypeNode*)n)->element_type); jw_end_object(w); break;
        }
        case AST_REFERENCE_TYPE: {
            ReferenceTypeNode* t = (ReferenceTypeNode*)n;
            begin_node(w, n, "REFERENCE_TYPE"); emit_child(w, "element_type", t->element_type); emit_bool(w, "is_mutable", t->is_mutable); jw_end_object(w); break;
        }
        case AST_UNSAFE_PTR_TYPE: {
            begin_node(w, n, "UNSAFE_PTR_TYPE"); emit_child(w, "element_type", ((UnsafePtrTypeNode*)n)->element_type); jw_end_object(w); break;
        }
        case AST_ERROR_UNION_TYPE: {
            ErrorUnionTypeNode* t = (ErrorUnionTypeNode*)n;
            begin_node(w, n, "ERROR_UNION_TYPE"); emit_child(w, "value_type", t->value_type); emit_child(w, "error_type", t->error_type); jw_end_object(w); break;
        }
        case AST_NULLABLE_TYPE: {
            begin_node(w, n, "NULLABLE_TYPE"); emit_child(w, "base_type", ((NullableTypeNode*)n)->base_type); jw_end_object(w); break;
        }
        // Task 5 adds nothing new here — every kind above is parse+typed common.
        default:
            die_ast_kind(n->type);
    }
}

// ---- types (Task 5 fills emit_type_entry) ----------------------------------

static void emit_type_entry(JsonW* w, size_t id, Type* t);

static void emit_type_table(JsonW* w) {
    jw_key(w, "types");
    jw_begin_array(w);
    // Component types are appended to g_types while entries are emitted, so
    // loop on the live count until the table is closed under reference.
    for (size_t i = 0; i < g_types.count; i++) emit_type_entry(w, i, g_types.items[i]);
    jw_end_array(w);
}

// ---- plan (Task 5 fills emit_plan) ----------------------------------------

static void emit_plan(JsonW* w, ReleasePlan* plan);

// ---- entry -----------------------------------------------------------------

void program_dump_write(FILE* out, ASTNode** files, const char** filenames,
                        size_t nfiles, ReleasePlan** plans, ProgramDumpStage stage) {
    JsonW w; jw_init(&w, out);
    g_stage = stage;
    g_selftest = getenv("GOO_DUMP_SELFTEST");
    g_nodes_emitted = 0;
    g_types.count = 0;

    jw_begin_object(&w);
    emit_int(&w, "goo_program_dump", 1);
    emit_str(&w, "stage", stage == PROGRAM_DUMP_PARSE ? "parse" : "typed");
    if (g_selftest && strcmp(g_selftest, "nonce") == 0) emit_int(&w, "nonce", (long long)getpid());
    jw_key(&w, "files");
    jw_begin_array(&w);
    for (size_t i = 0; i < nfiles; i++) {
        ProgramNode* p = (ProgramNode*)files[i];
        g_current_file = filenames[i];
        jw_begin_object(&w);
        emit_str(&w, "file", filenames[i]);
        emit_str(&w, "package", p->package_name);
        emit_list(&w, "imports", p->imports);
        emit_list(&w, "decls", p->decls);
        jw_key(&w, "plan");
        if (stage == PROGRAM_DUMP_TYPED && plans && plans[i]) emit_plan(&w, plans[i]); else jw_null(&w);
        jw_end_object(&w);
    }
    jw_end_array(&w);
    emit_type_table(&w);
    jw_end_object(&w);
    fputc('\n', out);
    fflush(out);
}

// Placeholders that Task 5 replaces. They exist so this task links; the typed
// stage is not wired into the driver until Task 5, so neither is reachable.
static void emit_type_entry(JsonW* w, size_t id, Type* t) { (void)w; (void)id; (void)t; abort(); }
static void emit_plan(JsonW* w, ReleasePlan* plan)          { (void)w; (void)plan; abort(); }
