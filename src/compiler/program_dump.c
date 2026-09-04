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
        // Tasks 4-5 add every other live kind here (expressions, types).
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
