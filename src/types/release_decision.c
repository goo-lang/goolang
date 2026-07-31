// T4: which locals may codegen release at function exit? See
// include/release_decision.h for the four conditions and the measurement behind
// each one.
//
// THE SOUNDNESS DIRECTION IS INVERTED relative to the three escape passes. There,
// `escapes = true` is the safe answer and the default arm marks everything. Here
// `release = false` is safe, so every path this file does not fully understand
// must refuse, and it must say WHY. A wrong refusal costs reclamation. A wrong
// approval frees live memory.
//
// WHY A SEPARATE WALK. This module needs three facts the escape passes do not
// record: a local's BINDING SITE expression (condition 2), how many times it is
// bound (condition 4), and the loop/arena nesting at its declaration (conditions
// 3 and 4). LocalEscapeResult is a name plus a boolean, so none of them survives
// there. The walk here is deliberately NOT another copy of escape_core's engine:
// it propagates no taint and applies no sink. It only collects declarations and
// assignments.
//
// CONSERVATIVE ON AN UNREADABLE FUNCTION. If the walk meets a statement kind it
// does not recognise, it cannot know whether that statement assigns to a local,
// and a missed assignment would let a rebound local be released. So the whole
// function is marked unreadable and every local in it refuses with
// RELEASE_NO_UNKNOWN. Missing a DECLARATION is already safe (the local gets no
// binding and refuses); missing an ASSIGNMENT is not, which is why the flag is
// per function rather than per local.

#include "release_decision.h"
#include "param_escape.h"
#include "local_escape.h"
#include "shim_signatures.h"
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Per-function collection
// ---------------------------------------------------------------------------

typedef struct {
    char*     name;          // owned
    ASTNode*  bound_value;   // the declaration's initial value; may be NULL
    int       binding_count; // declarations plus assignments
    int       loop_depth;    // loop nesting at the DECLARATION
    int       arena_depth;   // arena nesting at the DECLARATION
    bool      declared;      // a declaration was seen (not only an assignment)
} LocalRecord;

typedef struct {
    LocalRecord* items;
    size_t       count;
    size_t       cap;
    bool         unreadable;   // an unrecognised statement kind was met

    // Every INDEX-expression assignment target's key expression, in source
    // order. `m[k] = v` contributes k. Collected during the walk and CLASSIFIED
    // afterwards, because binding_is_owned needs the callee summaries and those
    // arrive after the walk has finished.
    //
    // This module is pure AST and holds no types, so it cannot tell `m[k] = v`
    // from `arr[i] = v`. It does not try. It answers only "is this expression a
    // fresh temporary", and codegen — which does have the type — decides
    // whether the container is a map. Same two-layer split that lets the
    // integer `+` arm stay approximate.
    ASTNode**    key_sites;
    size_t       key_count;
    size_t       key_cap;

    // Every expression STORED INTO a slice local, paired with that local's
    // name. `p = append(p, X)` contributes (p, X), and a slice literal
    // contributes one entry per element.
    //
    // Same question as a map key, one container along: an element is owned when
    // the expression that produced it is a fresh temporary. Classified after
    // the walk, for the same reason -- binding_is_owned needs the callee
    // summaries.
    //
    // A local with NO entry here owns no elements, which is the safe answer and
    // the behaviour every program had before this existed.
    struct { const char* target; ASTNode* expr; } *elem_sites;
    size_t       elem_count;
    size_t       elem_cap;
} Collected;

typedef struct {
    Collected* out;
    int        loop_depth;
    int        arena_depth;
} WalkCtx;

static LocalRecord* find_record(Collected* c, const char* name) {
    for (size_t i = 0; i < c->count; i++) {
        if (strcmp(c->items[i].name, name) == 0) return &c->items[i];
    }
    return NULL;
}

// Returns NULL on allocation failure, which callers treat as "unreadable".
static LocalRecord* intern_record(Collected* c, const char* name) {
    LocalRecord* existing = find_record(c, name);
    if (existing) return existing;
    if (c->count >= c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 8;
        LocalRecord* grown = realloc(c->items, ncap * sizeof(LocalRecord));
        if (!grown) return NULL;
        c->items = grown;
        c->cap = ncap;
    }
    LocalRecord* r = &c->items[c->count];
    r->name = strdup(name);
    if (!r->name) return NULL;
    r->bound_value = NULL;
    r->binding_count = 0;
    r->loop_depth = 0;
    r->arena_depth = 0;
    r->declared = false;
    c->count++;
    return r;
}

// Defined below, next to the append collector it pairs with.
static void note_literal_elems(WalkCtx* ctx, const char* target, ASTNode* value);

static void note_declaration(WalkCtx* ctx, const char* name, ASTNode* value) {
    if (!name || strcmp(name, "_") == 0) return;
    LocalRecord* r = intern_record(ctx->out, name);
    if (!r) { ctx->out->unreadable = true; return; }
    if (!r->declared) {
        r->declared = true;
        r->bound_value = value;
        r->loop_depth = ctx->loop_depth;
        r->arena_depth = ctx->arena_depth;
        // A slice literal's own elements count as stored values, exactly as
        // appended ones do. `[]string{}` records nothing, which is the usual
        // opening move and leaves the appends to decide.
        note_literal_elems(ctx, r->name, value);
    }
    r->binding_count++;
}

// Is this `L = append(L, ...)` -- a SELF-APPEND?
//
// It matters because such an assignment does not REBIND `L` to a different
// object. It GROWS L's object: append either writes into the existing buffer or
// reallocs it, and goo_realloc frees the old base itself. One local, one live
// buffer, and ownership never moves. So a release at exit frees exactly the
// buffer `L` still holds, which is correct.
//
// `t := append(s, x)` is a DIFFERENT and dangerous shape, and it is NOT this
// one. There, t and s can hold the SAME buffer when capacity suffices, and two
// owners means a double free. It stays refused by condition 2, because
// call_result_is_owned finds no summary for `append` and answers false.
//
// The arg-0 identifier must match the assignment target by NAME. `a = append(b,
// x)` is a rebind of `a` to b's buffer, not a self-append.
static bool is_self_append(const char* target, ASTNode* rhs) {
    if (!target || !rhs || rhs->type != AST_CALL_EXPR) return false;
    CallExprNode* call = (CallExprNode*)rhs;
    if (!call->function || call->function->type != AST_IDENTIFIER) return false;
    if (strcmp(((IdentifierNode*)call->function)->name, "append") != 0) return false;
    ASTNode* first = call->args;
    if (!first || first->type != AST_IDENTIFIER) return false;
    return strcmp(((IdentifierNode*)first)->name, target) == 0;
}

// Record an index-assignment's KEY expression for later classification.
// `m[a][b] = v` records both a and b, because either container could be a map.
static void note_key_site(WalkCtx* ctx, ASTNode* lhs) {
    if (!lhs || lhs->type != AST_INDEX_EXPR) return;
    IndexExprNode* ie = (IndexExprNode*)lhs;
    Collected* c = ctx->out;
    if (c->key_count >= c->key_cap) {
        size_t ncap = c->key_cap ? c->key_cap * 2 : 4;
        ASTNode** grown = realloc(c->key_sites, ncap * sizeof(ASTNode*));
        // Fail CLOSED: an unrecorded site is simply never owned, so the map
        // keeps today's borrow-everything behaviour.
        if (!grown) return;
        c->key_sites = grown;
        c->key_cap = ncap;
    }
    c->key_sites[c->key_count++] = ie->index;
    note_key_site(ctx, ie->expr);
}

// Record one expression stored into slice local `target`.
static void note_elem_site(WalkCtx* ctx, const char* target, ASTNode* expr) {
    if (!target || !expr) return;
    Collected* c = ctx->out;
    if (c->elem_count >= c->elem_cap) {
        size_t ncap = c->elem_cap ? c->elem_cap * 2 : 4;
        void* grown = realloc(c->elem_sites, ncap * sizeof(*c->elem_sites));
        // Fail CLOSED: an unrecorded element leaves the slice owning nothing.
        if (!grown) return;
        c->elem_sites = grown;
        c->elem_cap = ncap;
    }
    c->elem_sites[c->elem_count].target = target;
    c->elem_sites[c->elem_count].expr = expr;
    c->elem_count++;
}

// Every element expression of a slice literal, or nothing if `value` is not
// one. An EMPTY literal records nothing and is the common opening move --
// `parts := []string{}` -- which leaves ownership decided entirely by the
// appends that follow.
static void note_literal_elems(WalkCtx* ctx, const char* target, ASTNode* value) {
    if (!value || value->type != AST_SLICE_EXPR) return;
    for (ASTNode* e = ((SliceLitNode*)value)->elements; e; e = e->next) {
        note_elem_site(ctx, target, e);
    }
}

// The arguments a self-append adds, which are args 1..n of `append(L, ...)`.
// Arg 0 is L itself and is not a new element.
static void note_append_elems(WalkCtx* ctx, const char* target, ASTNode* rhs) {
    CallExprNode* call = (CallExprNode*)rhs;
    ASTNode* a = call->args;
    if (!a) return;
    for (a = a->next; a; a = a->next) note_elem_site(ctx, target, a);
}

// An ASSIGNMENT, not a declaration. The COUNT is what condition 4 reads, so the
// assigned value needs classifying only far enough to spot a self-append.
static void note_assignment(WalkCtx* ctx, ASTNode* lhs, ASTNode* rhs, bool plain_assign) {
    if (!lhs) return;
    // Before the identifier test below returns: an index target is not a rebind
    // of any local, but its KEY is a candidate for map ownership.
    note_key_site(ctx, lhs);
    if (lhs->type != AST_IDENTIFIER) return;   // a field/index store is not a rebind of the local
    const char* name = ((IdentifierNode*)lhs)->name;
    if (!name || strcmp(name, "_") == 0) return;
    LocalRecord* r = intern_record(ctx->out, name);
    if (!r) { ctx->out->unreadable = true; return; }
    // A self-append grows the same object, so it is not a new binding. Only a
    // plain `=` qualifies; a compound operator is never an append.
    if (plain_assign && is_self_append(name, rhs)) {
        // ...and every argument after arg 0 is a NEW ELEMENT of that object.
        note_append_elems(ctx, r->name, rhs);
        return;
    }
    r->binding_count++;
}

static bool is_assign_operator(TokenType op) {
    switch (op) {
        case TOKEN_ASSIGN:
        case TOKEN_PLUS_ASSIGN:
        case TOKEN_MINUS_ASSIGN:
        case TOKEN_MUL_ASSIGN:
        case TOKEN_DIV_ASSIGN:
        case TOKEN_MOD_ASSIGN:
        case TOKEN_AND_ASSIGN:
        case TOKEN_OR_ASSIGN:
        case TOKEN_XOR_ASSIGN:
        case TOKEN_LSHIFT_ASSIGN:
        case TOKEN_RSHIFT_ASSIGN:
            return true;
        default:
            return false;
    }
}

static void walk_stmts(WalkCtx* ctx, ASTNode* stmt);

// Seed `names` from a parallel `values` list. The shape `a, b := f(), g()` and
// `var a, b = x, y` share it. When the counts differ -- `a, b := f()` returning
// two results -- there is no single value per name, so each name is recorded with
// a NULL value and condition 2 then refuses it. That is the safe reading, and it
// is why the daemon's `n, err := strconv.Atoi(f)` would refuse on ownership even
// if it were not already refused on loop scope.
static void seed_names(WalkCtx* ctx, char** names, size_t name_count, ASTNode* values) {
    size_t value_count = 0;
    for (ASTNode* v = values; v; v = v->next) value_count++;

    if (value_count == name_count) {
        ASTNode* v = values;
        for (size_t i = 0; i < name_count; i++) {
            note_declaration(ctx, names[i], v);
            if (v) v = v->next;
        }
        return;
    }
    for (size_t i = 0; i < name_count; i++) note_declaration(ctx, names[i], NULL);
}

static void walk_stmts(WalkCtx* ctx, ASTNode* stmt) {
    for (; stmt; stmt = stmt->next) {
        switch (stmt->type) {
            case AST_BLOCK_STMT:
                walk_stmts(ctx, ((BlockStmtNode*)stmt)->statements);
                break;

            case AST_EXPR_STMT: {
                ASTNode* e = ((ExprStmtNode*)stmt)->expr;
                if (e && e->type == AST_BINARY_EXPR) {
                    BinaryExprNode* b = (BinaryExprNode*)e;
                    if (is_assign_operator(b->operator)) {
                        note_assignment(ctx, b->left, b->right,
                                        b->operator == TOKEN_ASSIGN);
                    }
                }
                break;
            }

            case AST_VAR_DECL: {
                VarDeclNode* n = (VarDeclNode*)stmt;
                seed_names(ctx, n->names, n->name_count, n->values);
                break;
            }

            case AST_CONST_DECL: {
                ConstDeclNode* n = (ConstDeclNode*)stmt;
                seed_names(ctx, n->names, n->name_count, n->values);
                break;
            }

            case AST_MULTI_ASSIGN: {
                MultiAssignNode* n = (MultiAssignNode*)stmt;
                // `targets` is a NODE LIST, not a name array. `a, b := f()`
                // declares, `a, b = x, y` assigns; either way condition 4 only
                // needs the count. When the list lengths differ -- `a, b := f()`
                // with one two-result call -- there is no single value per name,
                // so the value is recorded NULL and condition 2 refuses. That is
                // the daemon's `n, err := strconv.Atoi(f)` shape.
                size_t tcount = 0, vcount = 0;
                for (ASTNode* t = n->targets; t; t = t->next) tcount++;
                for (ASTNode* v = n->values; v; v = v->next) vcount++;
                ASTNode* v = n->values;
                for (ASTNode* t = n->targets; t; t = t->next) {
                    if (n->is_short_decl) {
                        if (t->type == AST_IDENTIFIER) {
                            note_declaration(ctx, ((IdentifierNode*)t)->name,
                                             (tcount == vcount) ? v : NULL);
                        }
                    } else {
                        note_assignment(ctx, t, v, !n->is_short_decl);
                    }
                    if (v) v = v->next;
                }
                break;
            }

            case AST_IF_STMT: {
                // CONDITION 5. Both arms are raised, and independently: a local
                // declared in EITHER branch has a slot on every path and a value
                // only on the taken one. See decide() for the measurement.
                IfStmtNode* n = (IfStmtNode*)stmt;
                walk_stmts(ctx, n->then_stmt);
                walk_stmts(ctx, n->else_stmt);
                break;
            }

            case AST_FOR_STMT: {
                ForStmtNode* n = (ForStmtNode*)stmt;
                ctx->loop_depth++;
                // The init clause declares INSIDE the loop's own scope (`i` in
                // `for i := 0; ...`), so it is counted at the raised depth.
                walk_stmts(ctx, n->init);
                walk_stmts(ctx, n->post);
                // `for k, v := range xs` binds k and v per ITERATION, and both
                // are views derived from the range expression. Recording them
                // keeps the plan honest; the raised loop_depth is what refuses
                // them.
                if (n->range_expr) {
                    if (n->key_name)   note_declaration(ctx, n->key_name, NULL);
                    if (n->value_name) note_declaration(ctx, n->value_name, NULL);
                }
                walk_stmts(ctx, n->body);
                ctx->loop_depth--;
                break;
            }

            case AST_ARENA_BLOCK: {
                ctx->arena_depth++;
                walk_stmts(ctx, ((ArenaBlockNode*)stmt)->body);
                ctx->arena_depth--;
                break;
            }

            case AST_UNSAFE_STMT:
                walk_stmts(ctx, ((UnsafeStmtNode*)stmt)->body);
                break;

            // ---- THE PRECISION CLIFF: switch / type switch / select / if let --
            //
            // Before these four arms every one of them fell to `default:` and
            // marked the WHOLE function unreadable, so each local in it refused
            // with RELEASE_NO_UNKNOWN. Row 17 pinned that.
            //
            // A CASE LIST IS NOT A STATEMENT LIST. The cases hang off `next` the
            // same way statements do, so handing them to walk_stmts would send
            // each one to `default:` and undo the whole arm. Iterate here and
            // walk only the BODIES, which is what escape_core.c does.
            //
            // A case node of an unexpected kind marks the function unreadable
            // rather than being skipped. escape_core skips it, because there
            // `escapes = true` is the safe answer; here the safe answer is the
            // other way and an unwalked body can hide an assignment.

            case AST_SWITCH_STMT: {
                SwitchStmtNode* n = (SwitchStmtNode*)stmt;
                // `default:` is an AST_CASE_CLAUSE with NULL exprs -- the parser
                // never builds AST_DEFAULT_CLAUSE -- so one test covers both.
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type != AST_CASE_CLAUSE) { ctx->out->unreadable = true; continue; }
                    walk_stmts(ctx, ((CaseClauseNode*)c)->body);
                }
                break;
            }

            case AST_TYPE_SWITCH: {
                TypeSwitchNode* n = (TypeSwitchNode*)stmt;
                // The guard's bind is recorded BEFORE the depth rises, on purpose.
                // `v := x.(type)` is a VIEW of the interface's data pointer, so a
                // NULL binding value makes condition 2 refuse it with NOT_OWNED --
                // the more fundamental cause, and one that no zero-store increment
                // could ever lift. Pinned by row 30.
                if (n->bind_name && n->bind_name->type == AST_IDENTIFIER) {
                    note_declaration(ctx, ((IdentifierNode*)n->bind_name)->name, NULL);
                }
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type != AST_TYPE_CASE) { ctx->out->unreadable = true; continue; }
                    walk_stmts(ctx, ((TypeCaseNode*)c)->body);
                }
                break;
            }

            case AST_SELECT_STMT: {
                SelectStmtNode* n = (SelectStmtNode*)stmt;
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type != AST_SELECT_CASE) { ctx->out->unreadable = true; continue; }
                    SelectCaseNode* sc = (SelectCaseNode*)c;
                    // `comm` IS AN EXPRESSION, NOT A STATEMENT. The grammar builds
                    // every select case from `CASE ... expression COLON case_body`
                    // (src/parser/parser.y), so it holds `<-ch` or `ch <- v`.
                    // Handing it to walk_stmts sent it straight to `default:` and
                    // marked the function unreadable -- measured: row 32 read
                    // RELEASE_NO_UNKNOWN instead of RELEASE_NO_REBOUND with the
                    // select arm otherwise complete. Classify it here instead.
                    //
                    // An assignment is a binary expression in this AST, so that is
                    // the one shape that can rebind a local. Anything else -- a
                    // receive, a send, a call -- binds nothing.
                    if (sc->comm && sc->comm->type == AST_BINARY_EXPR) {
                        BinaryExprNode* cb = (BinaryExprNode*)sc->comm;
                        if (is_assign_operator(cb->operator)) {
                            note_assignment(ctx, cb->left, cb->right,
                                            cb->operator == TOKEN_ASSIGN);
                        }
                    }
                    // is_declare IS THE SOUNDNESS-CRITICAL FIELD (include/ast.h):
                    //   1  `case v := <-ch:` declares, scoped to the case body.
                    //   0  `case v = <-ch:` ASSIGNS INTO AN OUTER LOCAL. Missing
                    //      this leaves a rebound local at a count of 1, so it
                    //      would be released while holding whatever the channel
                    //      delivered -- the declaration site can be a clean
                    //      allocation, so condition 2 alone does not catch it.
                    //      Pinned by row 32.
                    //  -1  `v, ok := <-ch`, which type_check_select_stmt ALWAYS
                    //      rejects (close() is unsupported in v1), so it never
                    //      reaches codegen and no release can be emitted for it.
                    //      bind_name is NULL there, so the guard below covers it.
                    if (sc->bind_name && strcmp(sc->bind_name, "_") != 0) {
                        if (sc->is_declare == 1) {
                            note_declaration(ctx, sc->bind_name, NULL);
                        } else if (sc->is_declare == 0) {
                            LocalRecord* r = intern_record(ctx->out, sc->bind_name);
                            if (!r) ctx->out->unreadable = true;
                            else    r->binding_count++;
                        }
                    }
                    walk_stmts(ctx, sc->body);
                }
                break;
            }

            case AST_IF_LET_STMT: {
                IfLetStmtNode* n = (IfLetStmtNode*)stmt;
                // Recorded before the depth rises, for the reason the type-switch
                // bind is: the unwrapped value aliases whatever the nullable held,
                // so NOT_OWNED is the cause that will never stop being true.
                note_declaration(ctx, n->var_name, NULL);
                walk_stmts(ctx, n->then_stmt);
                walk_stmts(ctx, n->else_stmt);
                break;
            }

            case AST_RETURN_STMT:
            case AST_BREAK_STMT:
            case AST_CONTINUE_STMT:
            case AST_GO_STMT:
            case AST_DEFER_STMT:
                // None of these binds a local. A `go`/`defer` argument can make a
                // local escape, and condition 1 is what refuses it -- local_escape
                // already treats both as escaping.
                break;

            default:
                // An unrecognised statement kind might assign to a local, and a
                // missed assignment would let a rebound local be released. Refuse
                // the whole function rather than guess. This is the same
                // conservative default the escape passes take, pointed the other
                // way because the safe answer here is `false`.
                ctx->out->unreadable = true;
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Condition 2 — ownership, decided at the binding site
// ---------------------------------------------------------------------------

static bool is_new_call(ASTNode* expr) {
    if (!expr || expr->type != AST_CALL_EXPR) return false;
    CallExprNode* call = (CallExprNode*)expr;
    return call->function && call->function->type == AST_IDENTIFIER &&
           strcmp(((IdentifierNode*)call->function)->name, "new") == 0;
}

static bool is_addr_of_composite(ASTNode* expr) {
    if (!expr || expr->type != AST_UNARY_EXPR) return false;
    UnaryExprNode* u = (UnaryExprNode*)expr;
    return u->operator == TOKEN_BIT_AND && u->operand &&
           u->operand->type == AST_STRUCT_LITERAL;
}

// A selector callee whose base names one of THIS function's locals is a method
// call on that local, not a package call. Without this guard a local named
// `strings` would collect the strings package's whitelist -- the same hole
// selector_base_is_local closes in escape_core.c, and param_escape row 23 pins.
static bool selector_base_is_local(Collected* c, ASTNode* selector) {
    if (!selector || selector->type != AST_SELECTOR_EXPR) return false;
    ASTNode* base = ((SelectorExprNode*)selector)->expr;
    if (!base || base->type != AST_IDENTIFIER) return false;
    return find_record(c, ((IdentifierNode*)base)->name) != NULL;
}

static bool call_result_is_owned(Collected* c, const ParamEscapeResult* pe, ASTNode* expr) {
    CallExprNode* call = (CallExprNode*)expr;
    if (!call->function) return false;

    if (call->function->type == AST_IDENTIFIER) {
        const char* name = ((IdentifierNode*)call->function)->name;
        const ParamEscapeSummary* s = param_escape_lookup(pe, name);
        // No summary means an unresolved or external callee. Conservative:
        // assume the result may alias an argument.
        if (!s) return false;
        // return_escapes is "does F return a value derived from one of its own
        // params?" -- exactly the borrowed-result relation. False means the
        // result derives from no argument, so the caller owns it.
        return !s->return_escapes;
    }

    if (call->function->type == AST_SELECTOR_EXPR) {
        if (selector_base_is_local(c, call->function)) return false;
        SelectorExprNode* sel = (SelectorExprNode*)call->function;
        if (!sel->expr || sel->expr->type != AST_IDENTIFIER || !sel->selector) return false;
        const char* pkg = ((IdentifierNode*)sel->expr)->name;
        // non_retaining is audited per runtime body in shim_signatures.c, and
        // that audit states every whitelisted entry COPIES rather than aliasing
        // an argument. errors.Unwrap is 0 because it returns a pointer INTO its
        // argument, so this bit separates owned from borrowed correctly.
        return shim_signature_is_non_retaining(pkg, sel->selector) != 0;
    }

    return false;
}

// Does local `name`'s DECLARATION hand it a slice whose elements it owns?
//
// The element sites collected during the walk cover the two ways a program
// STORES into a slice it already has -- a literal's elements, and an append.
// They cannot cover the third way a slice gets its contents, which is arriving
// fully populated from a call. `fields := strings.Split(req, ",")` records no
// element site at all, so before this the slice owned nothing and its parts
// leaked. Measured on bench/daemon/daemon.goo: 273,982 bytes per 2,000
// requests, the largest single item left after PR #274.
//
// Only a SHIM can answer, and only from a table. A Goo callee would need a
// summary saying "the slice I return owns its elements", which param_escape
// does not compute -- return_escapes is about the slice VALUE, not its
// contents. So a Goo call reaches the final `return false` here, and row 10
// pins that.
//
// BOUND ONCE IS REQUIRED, not inherited. The verdict's condition 4 already
// refuses a rebound local, and codegen emits no element release without a
// buffer release to hang it on -- but owns_elems is documented as INDEPENDENT
// of the verdict, so it must answer honestly on its own. A local rebound away
// from its Split result no longer holds those elements.
static bool binding_returns_owned_elems(Collected* c, const char* name) {
    if (!c || !name) return false;

    LocalRecord* r = find_record(c, name);
    if (!r || !r->declared) return false;
    if (r->binding_count != 1) return false;   // see the comment above

    ASTNode* v = r->bound_value;
    if (!v || v->type != AST_CALL_EXPR) return false;

    ASTNode* fn = ((CallExprNode*)v)->function;
    if (!fn || fn->type != AST_SELECTOR_EXPR) return false;

    // A method call on a LOCAL is not a package call, and the table is keyed by
    // package. Same guard call_result_is_owned makes, for the same reason.
    if (selector_base_is_local(c, fn)) return false;

    SelectorExprNode* sel = (SelectorExprNode*)fn;
    if (!sel->expr || sel->expr->type != AST_IDENTIFIER || !sel->selector) return false;

    const char* pkg = ((IdentifierNode*)sel->expr)->name;
    return shim_signature_returns_owned_elems(pkg, sel->selector) != 0;
}

static bool binding_is_owned(Collected* c, const ParamEscapeResult* pe, ASTNode* value) {
    if (!value) return false;

    if (is_new_call(value) || is_addr_of_composite(value)) return true;

    switch (value->type) {
        // A fresh composite is a fresh allocation, so the local owns it.
        case AST_STRUCT_LITERAL:
        case AST_SLICE_EXPR:      // a slice literal (SliceLitNode)
        case AST_ARRAY_LITERAL:
        case AST_PAREN_EXPR:      // a map literal (MapLitNode)
            return true;

        case AST_CALL_EXPR:
            return call_result_is_owned(c, pe, value);

        // CONCATENATION IS AN ALLOCATION. `s := a + b` on strings lowers to
        // goo_string_concat (src/runtime/runtime.c:523), which always returns
        // fresh goo_alloc memory -- or {NULL, 0} when both operands are empty,
        // and goo_release is a no-op on NULL. It COPIES both operands, so the
        // result aliases neither and a parameter operand is as safe as a local
        // one. That is the whole difference from borrowView's `s[1:]`, which
        // returns a view and stays refused by AST_SLICE_INDEX_EXPR below.
        //
        // WITHOUT THIS ARM THE STRING RELEASE IS WORTH NOTHING: the only owned
        // string bindings left are a Goo call and a non-retaining shim, and
        // concatenation is how ordinary Goo code builds a string.
        //
        // AN INTEGER `+` REACHES HERE TOO, and that is deliberate. This module is
        // pure AST and holds no type information, so it cannot tell the two
        // apart. Approving both costs nothing, because the decision is only HALF
        // the guard: codegen_arc_note_local refuses every slot that is not a
        // pointer, a 3-field slice or a 2-field string, and an int's slot is a
        // bare i64. Do NOT narrow this arm to "recover" precision -- the
        // two-layer split is what keeps each layer simple. Pinned by rows 22-25.
        //
        // ONLY `+`. No other binary operator yields a heap value in Go: the
        // comparisons and the logical operators yield a bool, and the remaining
        // arithmetic operators are numeric.
        case AST_BINARY_EXPR:
            return ((BinaryExprNode*)value)->operator == TOKEN_PLUS;

        // A VIEW into something this local does not own. `c := s[1:]` is the
        // shape include/local_escape.h names, and it needs no call to be wrong.
        case AST_SLICE_INDEX_EXPR:
        case AST_INDEX_EXPR:
        case AST_SELECTOR_EXPR:
            return false;

        // An alias. Only ONE owner may release, or the buffer is freed twice.
        case AST_IDENTIFIER:
            return false;

        default:
            // Includes a literal, which owns no heap object worth releasing, and
            // every construct not listed above.
            return false;
    }
}

// ---------------------------------------------------------------------------
// Plan construction
// ---------------------------------------------------------------------------

static void collected_free(Collected* c) {
    for (size_t i = 0; i < c->count; i++) free(c->items[i].name);
    free(c->items);
    c->items = NULL;
    c->count = c->cap = 0;
    // The elements are borrowed AST nodes; only the arrays are ours.
    free(c->key_sites);
    c->key_sites = NULL;
    c->key_count = c->key_cap = 0;
    free(c->elem_sites);
    c->elem_sites = NULL;
    c->elem_count = c->elem_cap = 0;
}

static ReleaseVerdict decide(const Collected* c, const LocalRecord* r,
                            const ParamEscapeResult* pe,
                            const LocalEscapeResult* le,
                            const char* fn) {
    if (c->unreadable) return RELEASE_NO_UNKNOWN;
    if (!r->declared) return RELEASE_NO_NO_BINDING;

    // Condition 1 first: it is the cheapest, and local_escape is conservative on
    // a miss so an unknown name refuses here.
    if (local_escape_local_escapes(le, fn, r->name)) return RELEASE_NO_ESCAPES;

    // Condition 3 before 4: an arena local inside a loop is refused for the
    // reason that actually makes it dangerous rather than merely wasteful.
    if (r->arena_depth > 0) return RELEASE_NO_ARENA;

    // Condition 4, the loop half. Checked before condition 5 because a local
    // inside a loop is refused for the more fundamental cause: it is rebound
    // every iteration, so a release at exit frees one of N.
    if (r->loop_depth > 0)    return RELEASE_NO_LOOP_SCOPE;

    // CONDITION 5 IS RETIRED. It used to refuse any local declared in a
    // conditional block,
    // because the slot is hoisted to the entry block while the initialising store
    // stays at the declaration site -- so an unexecuted declaration left undef and
    // goo_release read, and through __atomic_fetch_sub WROTE, through garbage.
    //
    // codegen_arc_zero_slot (src/codegen/statement_codegen.c) now stores NULL into
    // every release candidate's slot immediately after its alloca, and goo_release
    // is a no-op on NULL. The premise is gone, so the refusal is too, and a local
    // declared inside an `if` or a `switch` case body reclaims like any other.
    //
    // THE GUARANTEE IS FAIL-CLOSED, not assumed: codegen does not record a release
    // site at all unless that zero store was emitted. Rows 26, 27 and 29 pin the
    // release, and examples/arc_release_cond_probe.goo pins that it is valgrind-
    // clean -- removing the zero store makes that probe report 70,000 uninitialised
    // reads, which is what the refusal used to prevent.

    // Condition 4, the re-assignment half.
    if (r->binding_count > 1) return RELEASE_NO_REBOUND;

    // Condition 2 last: it is the one that needs the callee summaries.
    if (!binding_is_owned((Collected*)c, pe, r->bound_value)) return RELEASE_NO_NOT_OWNED;

    return RELEASE_OK;
}

static const char* func_decl_name(ASTNode* decl) {
    if (!decl || decl->type != AST_FUNC_DECL) return NULL;
    return ((FuncDeclNode*)decl)->name;
}

ReleasePlan* release_plan_analyze(ASTNode* program) {
    ReleasePlan* plan = calloc(1, sizeof(ReleasePlan));
    if (!plan) return NULL;
    if (!program || program->type != AST_PROGRAM) return plan;

    ParamEscapeResult* pe = param_escape_analyze(program);
    LocalEscapeResult* le = local_escape_analyze(program, pe);
    if (!le) { param_escape_result_free(pe); return plan; }

    size_t fn_count = 0;
    for (ASTNode* d = ((ProgramNode*)program)->decls; d; d = d->next) {
        if (d->type == AST_FUNC_DECL) fn_count++;
    }
    if (fn_count == 0) {
        local_escape_result_free(le);
        param_escape_result_free(pe);
        return plan;
    }

    plan->functions = calloc(fn_count, sizeof(ReleasePlanFunction));
    if (!plan->functions) {
        local_escape_result_free(le);
        param_escape_result_free(pe);
        free(plan);
        return NULL;
    }

    for (ASTNode* d = ((ProgramNode*)program)->decls; d; d = d->next) {
        if (d->type != AST_FUNC_DECL) continue;
        const char* name = func_decl_name(d);
        if (!name) continue;

        Collected c = { 0 };
        WalkCtx ctx = { .out = &c, .loop_depth = 0, .arena_depth = 0, };
        walk_stmts(&ctx, ((FuncDeclNode*)d)->body);

        ReleasePlanFunction* pf = &plan->functions[plan->count];
        pf->function_name = strdup(name);
        pf->count = 0;
        pf->decisions = c.count ? calloc(c.count, sizeof(ReleaseDecision)) : NULL;
        if (!pf->function_name || (c.count && !pf->decisions)) {
            free(pf->function_name);
            free(pf->decisions);
            collected_free(&c);
            continue;   // drop this function; a missing entry refuses, which is safe
        }

        for (size_t i = 0; i < c.count; i++) {
            pf->decisions[i].local_name = strdup(c.items[i].name);
            if (!pf->decisions[i].local_name) continue;
            pf->decisions[i].verdict = decide(&c, &c.items[i], pe, le, name);
            pf->count++;
        }

        // Does this local's slice own its elements? True only when EVERY value
        // that reached its slots is a fresh temporary. One borrowed element
        // poisons the local, because the release walks the whole buffer and
        // cannot skip an entry.
        //
        // Unlike a map key, ownership here is not per entry: the release site
        // is a single `for i < len` walk, so it is all of them or none.
        //
        // TWO WAYS TO OWN, and they are not symmetric:
        //   `any`         -- the program STORED into the slice, by literal or
        //                    by append, and every stored value was fresh.
        //   `from_binding`-- the slice ARRIVED populated, from a shim whose
        //                    table row proves it allocated each element.
        //
        // `all_owned` gates BOTH. A Split result that later takes a borrowed
        // element by append must read false: the elements Split made are owned,
        // the appended one is not, and one walk cannot tell them apart at
        // runtime. Refusing the whole local is the only safe answer, and elem
        // row 8 pins it.
        for (size_t i = 0; i < pf->count; i++) {
            const char* nm = pf->decisions[i].local_name;
            if (!nm) continue;
            bool any = false, all_owned = true;
            for (size_t j = 0; j < c.elem_count; j++) {
                if (strcmp(c.elem_sites[j].target, nm) != 0) continue;
                any = true;
                if (!binding_is_owned(&c, pe, c.elem_sites[j].expr)) {
                    all_owned = false;
                    break;
                }
            }
            bool from_binding = binding_returns_owned_elems(&c, nm);
            pf->decisions[i].owns_elems = all_owned && (any || from_binding);
        }

        // Classify the key sites NOW, because binding_is_owned needs `pe` and
        // release_plan_analyze frees it below.
        if (c.key_count) {
            pf->owned_keys = calloc(c.key_count, sizeof(ASTNode*));
            if (pf->owned_keys) {
                for (size_t i = 0; i < c.key_count; i++) {
                    if (binding_is_owned(&c, pe, c.key_sites[i])) {
                        pf->owned_keys[pf->owned_key_count++] = c.key_sites[i];
                    }
                }
            }
            // A NULL array means no key is owned, which is the safe answer.
        }

        plan->count++;
        collected_free(&c);
    }

    local_escape_result_free(le);
    param_escape_result_free(pe);
    return plan;
}

void release_plan_free(ReleasePlan* plan) {
    if (!plan) return;
    for (size_t i = 0; i < plan->count; i++) {
        ReleasePlanFunction* pf = &plan->functions[i];
        for (size_t j = 0; j < pf->count; j++) free(pf->decisions[j].local_name);
        free(pf->decisions);
        // The elements are borrowed AST nodes; only the array is ours.
        free(pf->owned_keys);
        free(pf->function_name);
    }
    free(plan->functions);
    free(plan);
}

ReleaseVerdict release_plan_verdict(const ReleasePlan* plan, const char* fn, const char* local) {
    if (!plan || !fn || !local) return RELEASE_NO_NO_BINDING;
    for (size_t i = 0; i < plan->count; i++) {
        if (strcmp(plan->functions[i].function_name, fn) != 0) continue;
        for (size_t j = 0; j < plan->functions[i].count; j++) {
            if (strcmp(plan->functions[i].decisions[j].local_name, local) == 0) {
                return plan->functions[i].decisions[j].verdict;
            }
        }
        return RELEASE_NO_NO_BINDING;
    }
    return RELEASE_NO_NO_BINDING;
}

bool release_plan_should_release(const ReleasePlan* plan, const char* fn, const char* local) {
    return release_plan_verdict(plan, fn, local) == RELEASE_OK;
}

bool release_plan_slice_owns_elems(const ReleasePlan* plan, const char* fn,
                                   const char* local) {
    if (!plan || !fn || !local) return false;
    for (size_t i = 0; i < plan->count; i++) {
        if (strcmp(plan->functions[i].function_name, fn) != 0) continue;
        const ReleasePlanFunction* pf = &plan->functions[i];
        for (size_t j = 0; j < pf->count; j++) {
            if (strcmp(pf->decisions[j].local_name, local) == 0) {
                return pf->decisions[j].owns_elems;
            }
        }
        return false;
    }
    return false;
}

bool release_plan_key_is_owned(const ReleasePlan* plan, const char* fn,
                               const ASTNode* key_expr) {
    if (!plan || !fn || !key_expr) return false;
    for (size_t i = 0; i < plan->count; i++) {
        if (strcmp(plan->functions[i].function_name, fn) != 0) continue;
        const ReleasePlanFunction* pf = &plan->functions[i];
        for (size_t j = 0; j < pf->owned_key_count; j++) {
            if (pf->owned_keys[j] == key_expr) return true;
        }
        return false;   // the function is known and this node is not owned
    }
    return false;       // unknown function
}

const char* release_verdict_name(ReleaseVerdict v) {
    switch (v) {
        case RELEASE_OK:            return "RELEASE_OK";
        case RELEASE_NO_ESCAPES:    return "RELEASE_NO_ESCAPES";
        case RELEASE_NO_NOT_OWNED:  return "RELEASE_NO_NOT_OWNED";
        case RELEASE_NO_ARENA:      return "RELEASE_NO_ARENA";
        case RELEASE_NO_LOOP_SCOPE: return "RELEASE_NO_LOOP_SCOPE";
        case RELEASE_NO_REBOUND:    return "RELEASE_NO_REBOUND";
        case RELEASE_NO_NO_BINDING: return "RELEASE_NO_NO_BINDING";
        case RELEASE_NO_UNKNOWN:    return "RELEASE_NO_UNKNOWN";
    }
    return "RELEASE_NO_UNKNOWN";
}
