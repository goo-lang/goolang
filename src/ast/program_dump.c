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

static void die_kind(const char* what, int kind) {
    fprintf(stderr, "program-dump: unsupported %s kind %d\n", what, kind);
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
        // Tasks 3-5 add every other live kind here.
        default:
            die_kind("AST node", n->type);
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
