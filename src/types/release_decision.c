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
// assignments. Where a condition needs a value FLOW -- condition 6 -- it reads
// local_escape's taint rather than deriving one from names here.
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
#include "returns_fresh.h"
#include "shim_signatures.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

// ---------------------------------------------------------------------------
// Per-function collection
// ---------------------------------------------------------------------------

typedef struct {
    char*     name;          // owned
    ASTNode*  bound_value;   // the declaration's initial value; may be NULL
    int       binding_count; // declarations plus assignments
    // BREAK-SCOPE nesting at the DECLARATION, and it MUST equal
    // codegen->cfctx.loop_depth at the same point in the same program.
    //
    // NOT "for-loop nesting", which is what it counted until 2026-08-01 and
    // what its old name said. cfctx raises this for a `switch`, a type switch
    // and a `select` as well as a `for` (cfctx_push_break_scope), because each
    // is a target a `break` can leave. Counting only `for` here made a
    // switch-case local and a loop-body local read as the SAME depth, so
    // condition 6 did not mark a store between them -- while codegen filed the
    // case local one level deeper and released it at the switch's `break`,
    // leaving the loop-body local pointing at freed memory.
    //
    // Measured, not reasoned: examples/arc_loop_carried_probe.goo's
    // switchBreak() reported 3 invalid reads through ToUpper, exit 0, plausible
    // output. If cfctx gains or loses a scope kind, this walk must follow.
    int       block_depth;
    int       arena_depth;   // arena nesting at the DECLARATION
    bool      declared;      // a declaration was seen (not only an assignment)

    // CONDITION 6. A local declared at a SHALLOWER depth may hold this local's
    // value, so the value outlives one iteration of this local's declaring
    // loop. Set by mark_block_escapes after the walk, from local_escape's
    // taint. Only consulted when max_decl_depth > 0 -- at function scope the
    // release is at function exit, and another function-scope local holding
    // the value changes nothing about when that exit happens.
    bool      block_escapes;

    // Declared by the loop HEADER -- the `i` of `for i := 0; ...`, or a range
    // clause's key/value name -- rather than by the body.
    //
    // ITS LIFETIME IS THE LOOP, NOT THE ITERATION. `for p := new(int); *p < n;`
    // reads `p` in the condition of every following iteration, so an
    // iteration-end release frees it under the test that is about to run. The
    // body's locals have no such reader, which is the whole distinction
    // condition 6 relaxes condition 4 for. This keeps the header out of it.
    //
    // NOT left to condition 2 to catch. A range key/value is refused there as a
    // view (a NULL binding), and `i` is an integer codegen records no slot for,
    // so both are safe today for reasons that are NOT this one -- which is
    // exactly the "safe by accident" shape the measurement that motivated
    // condition 6 recorded as finding 2.
    bool      loop_header;

    // THE DEPTHS OF EVERY DECLARATION OF THIS NAME, folded. One record holds
    // every local of one name, and block_depth and loop_header above describe
    // the FIRST declaration only. Condition 6 cannot tell which declaration is
    // in scope at a given store, so it takes the answer that refuses more:
    //
    //   min_decl_depth    the SHALLOWEST declaration, which lives longest.
    //                     Used for the HOLDER role.
    //   max_decl_depth    the DEEPEST declaration, which dies first. Used for
    //                     the SOURCE role, and it decides whether condition 6
    //                     is asked at all.
    //   min_header_depth  the shallowest LOOP-HEADER declaration, or INT_MAX.
    //                     A header lives across every iteration of the depth
    //                     it is recorded at.
    //
    // Measured: a holder declared first inside an earlier loop and again at
    // function scope kept the depth of the first, so the view it held was
    // released under it (tests/arc_uaf/loop_view_shadowed_holder.goo).
    int       min_decl_depth;
    int       max_decl_depth;
    int       min_header_depth;

    // EVERY value ever bound to this name, in source order, borrowed AST nodes.
    //
    // A NULL ENTRY IS REFUSED, NOT TRUSTED. It is tempting to read NULL as "no
    // initialiser, so the slot is zeroed and a release is a no-op", and for
    // `var s string` that is true. But note_declaration ALSO takes NULL for a
    // select/type-switch bind (`v := <-ch`), where the slot holds a real value
    // this walk simply did not describe. The two are indistinguishable here, so
    // the rebound path treats NULL as unsafe and `var s string` + reassign waits
    // for a row that separates them.
    //
    // `bound_value` above is the FIRST of these and stays, because conditions 2
    // and owns_elems ask about the declaration specifically. This list is what
    // the REBOUND path needs instead: the release site is the store, and at run
    // time that store cannot tell which value the slot happens to hold, so
    // every value has to be safe to release or none of them is.
    ASTNode** values;
    size_t    value_count;
    size_t    value_cap;

    // A value reached this slot that `values` cannot describe -- today only a
    // COMPOUND assignment, whose stored result is `lhs op rhs` and has no AST
    // node of its own. Poisons the rebound path on its own, because a list that
    // silently omits a value would let that value be freed unexamined.
    bool      has_unsafe_value;

    // `L = append(L, ...)` was seen for this name. A self-append is not a
    // binding, so binding_count does not move, but codegen frees the array the
    // append replaced when the local reads RELEASE_OK
    // (codegen_arc_release_replaced_buffer). That free happens in the MIDDLE
    // of the function, so condition 7 must hold for this local exactly as it
    // must for a rebound one. See decide().
    bool      self_appended;
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

    // Every OPERAND of a `+` expression anywhere this walk reaches. A string
    // concat COPIES both operands into fresh memory, so an operand that is a
    // fresh temporary is unreachable the instant the concat returns and nothing
    // but this concat can ever free it.
    //
    // Classified after the walk, for the same reason the key sites are:
    // binding_is_owned needs the callee summaries.
    //
    // AN INTEGER `+` LANDS HERE TOO, exactly as it does in condition 2's table.
    // This module holds no types and cannot tell the two apart. Recording both
    // costs nothing, because codegen asks only at a string-concat site and it
    // is codegen that has the type. Same two-layer split as everywhere else.
    //
    // FAIL CLOSED: an expression this scanner cannot reach is simply not
    // recorded, so the operand stays borrowed and the program keeps the
    // behaviour it had before this existed.
    ASTNode**    concat_sites;
    size_t       concat_count;
    size_t       concat_cap;

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

    // The function being walked. A bare call `f()` whose name is one of its
    // parameters or named results calls that variable, not a top-level f.
    const FuncDeclNode* decl;

    // CONDITION 7's OTHER TWO SOURCES. has_alias reads every record's values,
    // which is every expression a named local was bound to. Two kinds of
    // holder have no entry there.
    //
    // held_exprs: an expression that reached a local whose recorded value is
    // NULL -- the right-hand side of a count-mismatched `a, b := f(x)`, a type
    // switch's subject, an if-let's nullable. The local is a view of it, and
    // NULL cannot say of what. Borrowed AST nodes.
    ASTNode**    held_exprs;
    size_t       held_count;
    size_t       held_cap;

    // store_log: the NAME of every binding and self-append, in walk order.
    // Borrowed from the records, whose names never move. range_sites: each
    // `for ... range X` with the log span its BODY covers. The range
    // temporary is the one unnamed holder that outlives a statement: it keeps
    // X's header for the whole loop. A store into a local that X names, inside
    // that span, frees the value the loop still iterates.
    const char** store_log;
    size_t       store_count;
    size_t       store_cap;
    struct { ASTNode* expr; size_t begin; size_t end; } *range_sites;
    size_t       range_count;
    size_t       range_cap;

    // CONDITION 8's input: every name whose address `&name` is taken (or that
    // a closure captures), from returns_fresh_addr_taken -- the walk the
    // freshness proof already trusts, not a second copy of it. addr_unknown
    // means that walk met a node it does not model, so ANY name may be one.
    const char** addr_names;
    size_t       addr_count;
    bool         addr_unknown;
} Collected;

typedef struct {
    Collected* out;
    int        block_depth;
    int        arena_depth;
    // Walking a loop's init/post/range clause rather than its body. See
    // LocalRecord.loop_header.
    bool       in_loop_header;
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
    r->name = xstrdup(name);
    if (!r->name) return NULL;
    r->bound_value = NULL;
    r->binding_count = 0;
    r->block_depth = 0;
    r->arena_depth = 0;
    r->declared = false;
    r->block_escapes = false;
    r->loop_header = false;
    r->min_decl_depth = INT_MAX;
    r->max_decl_depth = 0;
    r->min_header_depth = INT_MAX;
    r->values = NULL;
    r->value_count = 0;
    r->value_cap = 0;
    r->has_unsafe_value = false;
    r->self_appended = false;
    c->count++;
    return r;
}

// Appends one bound value. A NULL `value` is recorded deliberately: `var s
// string` leaves the slot zeroed, and goo_release is a no-op on NULL, so it is
// a SAFE entry rather than a missing one.
//
// Returns false on allocation failure only. The caller sets `unreadable`, which
// refuses every local in the function -- losing the record silently would let a
// borrowed value go unseen, and this list decides whether a store may free.
static bool record_value(LocalRecord* r, ASTNode* value) {
    if (r->value_count >= r->value_cap) {
        size_t ncap = r->value_cap ? r->value_cap * 2 : 4;
        ASTNode** grown = realloc(r->values, ncap * sizeof(ASTNode*));
        if (!grown) return false;
        r->values = grown;
        r->value_cap = ncap;
    }
    r->values[r->value_count++] = value;
    return true;
}

// Log one store into `name` for the range rule. A failed append marks the
// function unreadable: a store missing from the log would let a range over the
// stored local pass the rule, and that frees what the loop still reads.
static void log_store(Collected* c, const char* name) {
    if (c->store_count >= c->store_cap) {
        size_t ncap = (c->store_cap != 0U) ? (c->store_cap * 2U) : 8U;
        const char** grown = realloc(c->store_log, ncap * sizeof(*c->store_log));
        if (!grown) { c->unreadable = true; return; }
        c->store_log = grown;
        c->store_cap = ncap;
    }
    c->store_log[c->store_count++] = name;
}

// Record an expression that a NULL-valued binding is a view of. Same failure
// rule as log_store, for the same reason: an alias source that is not recorded
// cannot refuse anything.
static void note_held_expr(Collected* c, ASTNode* expr) {
    if (!expr) {
        return;
    }
    if (c->held_count >= c->held_cap) {
        size_t ncap = (c->held_cap != 0U) ? (c->held_cap * 2U) : 4U;
        ASTNode** grown = realloc(c->held_exprs, ncap * sizeof(*c->held_exprs));
        if (!grown) { c->unreadable = true; return; }
        c->held_exprs = grown;
        c->held_cap = ncap;
    }
    c->held_exprs[c->held_count++] = expr;
}

static void note_held_list(Collected* c, ASTNode* list) {
    for (ASTNode* e = list; e; e = e->next) {
        note_held_expr(c, e);
    }
}

// Record a range loop over `expr` whose body's stores are the log entries from
// `begin` to now. Same failure rule as log_store: an unrecorded range cannot
// refuse.
static void note_range_site(Collected* c, ASTNode* expr, size_t begin) {
    if (c->range_count >= c->range_cap) {
        size_t ncap = (c->range_cap != 0U) ? (c->range_cap * 2U) : 4U;
        void* grown = realloc(c->range_sites, ncap * sizeof(*c->range_sites));
        if (!grown) { c->unreadable = true; return; }
        c->range_sites = grown;
        c->range_cap = ncap;
    }
    c->range_sites[c->range_count].expr = expr;
    c->range_sites[c->range_count].begin = begin;
    c->range_sites[c->range_count].end = c->store_count;
    c->range_count++;
}

// ---------------------------------------------------------------------------
// The concat-operand scanner
// ---------------------------------------------------------------------------

// Record both operands of every `+` in `expr`, then descend.
//
// The default arm is the reason this is safe to extend later: an expression kind
// nobody added records nothing, so its operands stay borrowed. A missed operand
// is a leak we already have.
static void note_concat_operands(WalkCtx* ctx, ASTNode* expr) {
    if (!expr) return;
    Collected* c = ctx->out;

    switch (expr->type) {
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            if (b->operator == TOKEN_PLUS) {
                ASTNode* operands[2] = { b->left, b->right };
                for (int i = 0; i < 2; i++) {
                    if (!operands[i]) continue;
                    if (c->concat_count >= c->concat_cap) {
                        size_t ncap = c->concat_cap ? c->concat_cap * 2 : 8;
                        ASTNode** grown = realloc(c->concat_sites, ncap * sizeof(ASTNode*));
                        // Fail CLOSED, as note_key_site does: an unrecorded
                        // operand is never owned, so nothing frees it.
                        if (!grown) return;
                        c->concat_sites = grown;
                        c->concat_cap = ncap;
                    }
                    c->concat_sites[c->concat_count++] = operands[i];
                }
            }
            note_concat_operands(ctx, b->left);
            note_concat_operands(ctx, b->right);
            return;
        }

        case AST_IDENTIFIER:
        case AST_LITERAL:
            return;

        case AST_UNARY_EXPR:
            note_concat_operands(ctx, ((UnaryExprNode*)expr)->operand);
            return;

        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            note_concat_operands(ctx, call->function);
            for (ASTNode* a = call->args; a; a = a->next) note_concat_operands(ctx, a);
            return;
        }

        case AST_INDEX_EXPR:
            note_concat_operands(ctx, ((IndexExprNode*)expr)->expr);
            note_concat_operands(ctx, ((IndexExprNode*)expr)->index);
            return;

        case AST_SELECTOR_EXPR:
            note_concat_operands(ctx, ((SelectorExprNode*)expr)->expr);
            return;

        case AST_SLICE_INDEX_EXPR: {
            SliceIndexExprNode* s = (SliceIndexExprNode*)expr;
            note_concat_operands(ctx, s->expr);
            note_concat_operands(ctx, s->low);
            note_concat_operands(ctx, s->high);
            return;
        }

        case AST_SLICE_EXPR:
            for (ASTNode* e = ((SliceLitNode*)expr)->elements; e; e = e->next) {
                note_concat_operands(ctx, e);
            }
            return;

        case AST_ARRAY_LITERAL:
            for (ASTNode* e = ((ArrayLitNode*)expr)->elements; e; e = e->next) {
                note_concat_operands(ctx, e);
            }
            return;

        case AST_PAREN_EXPR: {
            MapLitNode* m = (MapLitNode*)expr;
            for (ASTNode* k = m->keys; k; k = k->next) note_concat_operands(ctx, k);
            for (ASTNode* v = m->values; v; v = v->next) note_concat_operands(ctx, v);
            return;
        }

        case AST_STRUCT_LITERAL:
            for (ASTNode* f = ((StructLiteralNode*)expr)->field_values; f; f = f->next) {
                note_concat_operands(ctx, f);
            }
            return;

        default:
            // Records nothing and marks nothing. This scanner has no
            // conservative direction to take: it only ever ADDS a reclamation,
            // so not reaching an expression costs bytes rather than safety.
            return;
    }
}

// Record every `+` operand in a list of expressions.
static void note_concat_operand_list(WalkCtx* ctx, ASTNode* list) {
    for (ASTNode* e = list; e; e = e->next) note_concat_operands(ctx, e);
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
        r->block_depth = ctx->block_depth;
        r->arena_depth = ctx->arena_depth;
        r->loop_header = ctx->in_loop_header;
        // A slice literal's own elements count as stored values, exactly as
        // appended ones do. `[]string{}` records nothing, which is the usual
        // opening move and leaves the appends to decide.
        note_literal_elems(ctx, r->name, value);
    }
    // EVERY declaration, not only the first. See LocalRecord.min_decl_depth.
    // A header declaration anywhere makes the name a header: the release
    // placement that frees a body local at iteration end is wrong for it.
    if (ctx->block_depth < r->min_decl_depth) r->min_decl_depth = ctx->block_depth;
    if (ctx->block_depth > r->max_decl_depth) r->max_decl_depth = ctx->block_depth;
    if (ctx->in_loop_header) {
        r->loop_header = true;
        if (ctx->block_depth < r->min_header_depth) r->min_header_depth = ctx->block_depth;
    }
    if (!record_value(r, value)) { ctx->out->unreadable = true; return; }
    r->binding_count++;
    log_store(ctx->out, r->name);
}

// Is this `L = append(L, ...)` -- a SELF-APPEND?
//
// It matters because such an assignment does not REBIND `L` to a different
// object. It GROWS L's object: append either writes into the existing buffer or
// copies it into a new one, and for an approved L codegen releases the replaced
// buffer at the store (codegen_arc_release_replaced_buffer). One local, one live
// buffer, and ownership never moves. So a release at exit frees exactly the
// buffer `L` still holds, which is correct.
//
// NOT A BINDING, BUT A MID-FUNCTION FREE. That release is why a self-append
// sets LocalRecord.self_appended, and why condition 7 runs for such a local
// although its binding count did not move. See decide().
//
// `t := append(s, x)` is a DIFFERENT and dangerous shape, and it is NOT this
// one. There, t and s can hold the SAME buffer when capacity suffices, and two
// owners means a double free. It stays refused by condition 2, because
// call_result_is_owned finds no summary for `append` and answers false.
//
// The arg-0 identifier must match the assignment target by NAME. `a = append(b,
// x)` is a rebind of `a` to b's buffer, not a self-append.
// Exported for codegen, which must NOT emit a release-before-store at a
// self-append: that walks and frees the ELEMENTS, and the new buffer still
// holds every one of them. When append still realloc'd, that was measured on
// bench/daemon/daemon.goo as 7 invalid reads through goo_slice_release_elems and
// goo_release_with, with the program printing nothing at all. Codegen releases
// only the replaced buffer instead. ONE implementation rather than a second copy
// in codegen: the two must agree, and this is the same predicate the rebind rule
// uses.
static bool is_self_append(const char* target, ASTNode* rhs);

bool release_decision_is_self_append(const char* target, ASTNode* rhs) {
    return is_self_append(target, rhs);
}

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
    // Every store reaches the log, the self-append included: each one frees
    // what the slot held, the old value or the replaced array.
    log_store(ctx->out, r->name);
    // A self-append grows the same object, so it is not a new binding. Only a
    // plain `=` qualifies; a compound operator is never an append.
    if (plain_assign && is_self_append(name, rhs)) {
        // ...and every argument after arg 0 is a NEW ELEMENT of that object.
        note_append_elems(ctx, r->name, rhs);
        r->self_appended = true;
        return;
    }
    // A COMPOUND assignment (`s += x`) stores the result of `lhs op rhs`, NOT
    // `rhs`. There is no AST node for that result to record, and recording NULL
    // would be read as "no initialiser, slot is zeroed, safe" -- which is a lie
    // about a slot that holds a real value.
    //
    // So it is refused by its own flag rather than described badly. `s += x` on
    // strings does store a fresh goo_string_concat buffer and IS releasable in
    // principle; relaxing this needs a row that proves it, not an inference
    // here, because this module holds no types and cannot tell that `+=` from an
    // integer one.
    if (!plain_assign) {
        r->has_unsafe_value = true;
    } else if (!record_value(r, rhs)) {
        ctx->out->unreadable = true;
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

// `pkg.Fn(...)` or `x.m(...)`. condition 2 decides whether the base is a
// package and the row is audited; this only keeps a bare Goo call out.
static bool is_selector_call(const ASTNode* v) {
    return v && v->type == AST_CALL_EXPR && ((const CallExprNode*)v)->function
           && ((const CallExprNode*)v)->function->type == AST_SELECTOR_EXPR;
}

// Seed `names` from a parallel `values` list. The shape `a, b := f(), g()` and
// `var a, b = x, y` share it. When ONE value feeds several names -- the daemon's
// `n, err := strconv.Atoi(f)` shape -- that value is attributed to every target.
// Any OTHER count mismatch is a shape this module does not understand, and the
// safe answer there is still NULL, which condition 2 refuses.
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
    // COUNTS DIFFER: `a, b := f()`, one call feeding several names.
    //
    // "No result aliases an ARGUMENT" does not make every result owned: two
    // results can be ONE object. A Goo `return b, b` gave the caller two owners
    // of one box, and both released it (finding C2, review C-2). So a Goo
    // call's results record NULL here, and no Goo summary with several results
    // is returns_fresh either.
    //
    // A SHIM call keeps the attribution. Every multi-result shim is an error
    // union (SHIM_RET_ATOI_RESULT, _STRING_RESULT, _F64_RESULT): one side is
    // filled and the other is zero on every path, so the two results are never
    // one object. condition 2 still asks the audited non_retaining row, and
    // examples/arc_release_tuple_probe.goo pins the bytes.
    ASTNode* single = (value_count == 1 && is_selector_call(values)) ? values : NULL;
    // A NULL value says nothing about what the names now hold, so condition 7
    // reads the right-hand side itself.
    note_held_list(ctx->out, values);
    for (size_t i = 0; i < name_count; i++) note_declaration(ctx, names[i], single);
}

static void walk_stmts(WalkCtx* ctx, ASTNode* stmt) {
    for (; stmt; stmt = stmt->next) {
        switch (stmt->type) {
            case AST_BLOCK_STMT:
                walk_stmts(ctx, ((BlockStmtNode*)stmt)->statements);
                break;

            case AST_EXPR_STMT: {
                ASTNode* e = ((ExprStmtNode*)stmt)->expr;
                note_concat_operands(ctx, e);
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
                note_concat_operand_list(ctx, n->values);
                seed_names(ctx, n->names, n->name_count, n->values);
                break;
            }

            case AST_CONST_DECL: {
                ConstDeclNode* n = (ConstDeclNode*)stmt;
                note_concat_operand_list(ctx, n->values);
                seed_names(ctx, n->names, n->name_count, n->values);
                break;
            }

            case AST_MULTI_ASSIGN: {
                MultiAssignNode* n = (MultiAssignNode*)stmt;
                note_concat_operand_list(ctx, n->values);
                // `targets` is a NODE LIST, not a name array. `a, b := f()`
                // declares, `a, b = x, y` assigns; either way condition 4 only
                // needs the count. When ONE value feeds several names -- the
                // daemon's `n, err := strconv.Atoi(f)` shape -- a SHIM call is
                // attributed to every target and a Goo call records NULL: two
                // results of a Goo call can be one object. See seed_names.
                size_t tcount = 0, vcount = 0;
                for (ASTNode* t = n->targets; t; t = t->next) tcount++;
                for (ASTNode* v = n->values; v; v = v->next) vcount++;
                // Unpaired, the targets record NULL or one shared call, so
                // condition 7 reads the right-hand side itself. See seed_names.
                if (tcount != vcount) {
                    note_held_list(ctx->out, n->values);
                }

                ASTNode* v = n->values;
                for (ASTNode* t = n->targets; t; t = t->next) {
                    if (n->is_short_decl) {
                        if (t->type == AST_IDENTIFIER) {
                            note_declaration(ctx, ((IdentifierNode*)t)->name,
                                             (tcount == vcount) ? v
                                                 : ((vcount == 1 && is_selector_call(n->values))
                                                        ? n->values : NULL));
                        }
                    } else {
                        // Hand the rhs over only when the pairing is real.
                        // Everything else note_assignment does -- the binding
                        // count, the key sites -- still runs.
                        note_assignment(ctx, t, (tcount == vcount) ? v : NULL,
                                        !n->is_short_decl);
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
                ctx->block_depth++;
                // The init clause declares INSIDE the loop's own scope (`i` in
                // `for i := 0; ...`), so it is counted at the raised depth.
                //
                // AND IT IS FLAGGED AS THE HEADER, which is what keeps condition
                // 6 from approving it. `i` is bound ONCE for the whole loop and
                // read by every following iteration's condition, so it is not an
                // iteration-scoped value however it is declared. See
                // LocalRecord.loop_header.
                bool saved_header = ctx->in_loop_header;
                ctx->in_loop_header = true;
                walk_stmts(ctx, n->init);
                walk_stmts(ctx, n->post);
                // `for k, v := range xs` binds k and v per ITERATION, and both
                // are views derived from the range expression. Recording them
                // keeps the plan honest; condition 2 refuses them as views and
                // the header flag refuses them again, on purpose -- one reason
                // that holds is not the same as two.
                if (n->range_expr) {
                    if (n->key_name)   note_declaration(ctx, n->key_name, NULL);
                    if (n->value_name) note_declaration(ctx, n->value_name, NULL);
                }
                ctx->in_loop_header = saved_header;
                // The body's stores are the span the range temporary is live
                // across. Header stores are outside it: the range expression
                // is evaluated after the init clause and before any body.
                size_t body_begin = ctx->out->store_count;
                walk_stmts(ctx, n->body);
                if (n->range_expr) {
                    note_range_site(ctx->out, n->range_expr, body_begin);
                }
                ctx->block_depth--;
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
                // A BREAK SCOPE, so the depth rises exactly as cfctx's does.
                ctx->block_depth++;
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type != AST_CASE_CLAUSE) { ctx->out->unreadable = true; continue; }
                    walk_stmts(ctx, ((CaseClauseNode*)c)->body);
                }
                ctx->block_depth--;
                break;
            }

            case AST_TYPE_SWITCH: {
                TypeSwitchNode* n = (TypeSwitchNode*)stmt;
                // The guard's bind is recorded BEFORE the depth rises, on purpose.
                // `v := x.(type)` is a VIEW of the interface's data pointer, so a
                // NULL binding value makes condition 2 refuse it with NOT_OWNED --
                // the more fundamental cause, and one that no zero-store increment
                // could ever lift. Pinned by row 30.
                // A BREAK SCOPE, and the rise comes BEFORE the bind because
                // codegen_generate_type_switch_stmt pushes its break scope
                // before it generates any case body, and the bind is created
                // inside one. The two counters must agree at every point, which
                // is the invariant the switch defect broke.
                ctx->block_depth++;
                if (n->bind_name && n->bind_name->type == AST_IDENTIFIER) {
                    note_declaration(ctx, ((IdentifierNode*)n->bind_name)->name, NULL);
                    note_held_expr(ctx->out, n->expr);
                }
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type != AST_TYPE_CASE) { ctx->out->unreadable = true; continue; }
                    walk_stmts(ctx, ((TypeCaseNode*)c)->body);
                }
                ctx->block_depth--;
                break;
            }

            case AST_SELECT_STMT: {
                SelectStmtNode* n = (SelectStmtNode*)stmt;
                // A BREAK SCOPE, as cfctx_push_break_scope makes it.
                ctx->block_depth++;
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
                            // A value reaches the slot and NO node is recorded
                            // for it, so the values list is incomplete here. The
                            // rebound path must not read that silence as "no
                            // values, nothing unsafe".
                            else    {
                                r->has_unsafe_value = true;
                                r->binding_count++;
                                log_store(ctx->out, r->name);
                            }
                        }
                    }
                    walk_stmts(ctx, sc->body);
                }
                ctx->block_depth--;
                break;
            }

            case AST_IF_LET_STMT: {
                IfLetStmtNode* n = (IfLetStmtNode*)stmt;
                // Recorded before the depth rises, for the reason the type-switch
                // bind is: the unwrapped value aliases whatever the nullable held,
                // so NOT_OWNED is the cause that will never stop being true.
                note_declaration(ctx, n->var_name, NULL);
                note_held_expr(ctx->out, n->nullable_expr);
                walk_stmts(ctx, n->then_stmt);
                walk_stmts(ctx, n->else_stmt);
                break;
            }

            case AST_RETURN_STMT:
                // THE DAEMON'S OWN SHAPE. Its whole reply is built in the return
                // expression, and before this the arm below reached nothing in
                // it. No local is bound here, so the rest of the arm still holds.
                note_concat_operand_list(ctx, ((ReturnStmtNode*)stmt)->values);
                break;

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
// Condition 6 — the loop-carried store, read from local_escape's taint
// ---------------------------------------------------------------------------

// Mark every loop-declared local whose value a SHALLOWER local may hold. That
// holder outlives one iteration of the local's declaring loop, so a release at
// iteration end frees memory the holder still reaches.
//
// A FLOW, NOT A MENTION, and that is finding C4. The old test marked a local
// only when its NAME appeared in the right-hand side of a store into a
// shallower target. `y := x[1:]; outer = y` names only `y` in the store, so `x`
// was released while `outer` still pointed into its buffer. local_escape's
// taint follows the value through `y`, through `parts[i]`, through a field and
// through any chain of locals, so condition 6 asks it instead of the names.
//
// A NAME DECLARED MORE THAN ONCE is one record, and this function cannot tell
// which declaration is in scope at a store. So a holder is compared by its
// SHALLOWEST declaration and a source by its DEEPEST, which is the pairing
// that refuses more. See LocalRecord.min_decl_depth.
//
// A LOOP-HEADER HOLDER OUTLIVES THE ITERATION AT THE SAME DEPTH. The `p` of
// `for p := ""; p != s; n++` is recorded at the loop's own depth, the depth of
// the body's locals, but it lives across every iteration and the next
// condition reads it. So `p = x` in the body frees `x` under `p` unless the
// header counts as longer-lived. Measured: tests/arc_uaf/loop_view_header_var
// read freed memory in goo_string_eq.
//
// WHY AN UNDECLARED HOLDER IS SKIPPED. A record that was only ever assigned is
// a parameter, a named result or a global. None of them is a local of
// local_escape's LocalEnv, so a store into one is a GLOBAL_STORE sink: every
// value it receives already escapes, and condition 1 refuses its source.
//
// FAILS CLOSED otherwise. local_escape_may_hold answers true for any name it
// does not know, and for every pair when the function has no summary.
static void mark_block_escapes(Collected* c, const LocalEscapeSummary* les) {
    for (size_t i = 0; i < c->count; i++) {
        LocalRecord* r = &c->items[i];
        if (!r->declared || r->max_decl_depth == 0) {
            continue;
        }
        // The record itself is a candidate holder too. Two locals of one name
        // share one record and one taint row, so the shallower one may hold
        // the deeper one's value; the depth test below answers false for a
        // name declared at one depth only.
        for (size_t j = 0; j < c->count && !r->block_escapes; j++) {
            const LocalRecord* holder = &c->items[j];
            bool outlives = holder->declared
                            && (holder->min_decl_depth < r->max_decl_depth
                                || holder->min_header_depth <= r->max_decl_depth);
            if (outlives && local_escape_may_hold(les, holder->name, r->name)) {
                r->block_escapes = true;
            }
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

static bool is_addr_of_composite(const ASTNode* expr) {
    if (!expr || expr->type != AST_UNARY_EXPR) return false;
    const UnaryExprNode* u = (const UnaryExprNode*)expr;
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

static bool var_decl_list_has(const ASTNode* list, const char* name) {
    for (const ASTNode* p = list; p; p = p->next) {
        if (p->type != AST_VAR_DECL) continue;
        const VarDeclNode* vd = (const VarDeclNode*)p;
        for (size_t i = 0; vd->names && i < vd->name_count; i++) {
            if (vd->names[i] && strcmp(vd->names[i], name) == 0) return true;
        }
    }
    return false;
}

// Is `name` a parameter (the receiver included) or a named result of `decl`?
// No decl answers true, which refuses: the caller cannot rule the name out.
static bool decl_binds_name(const FuncDeclNode* decl, const char* name) {
    if (!decl) return true;
    if (var_decl_list_has(decl->params, name)) return true;
    if (decl->return_type && decl->return_type->type == AST_STRUCT_TYPE) {
        return var_decl_list_has(((const StructTypeNode*)decl->return_type)->fields, name);
    }
    return false;
}

static bool call_result_is_owned(Collected* c, const ParamEscapeResult* pe, ASTNode* expr) {
    CallExprNode* call = (CallExprNode*)expr;
    if (!call->function) return false;

    if (call->function->type == AST_IDENTIFIER) {
        const char* name = ((IdentifierNode*)call->function)->name;
        // A local, a parameter or a named result spelled like a function is a
        // func VALUE. Its call reaches whatever it holds, not the summary.
        if (find_record(c, name) || decl_binds_name(c->decl, name)) return false;
        // Owned only on a positive proof (finding C2). `!return_escapes` was
        // used here, and it says only that the result derives from no
        // PARAMETER: a callee returning a global, or a value it also stored in
        // a container, passed that test and the caller freed live memory.
        //
        // The helper also requires !return_escapes, because has_alias prunes
        // an owned call result whole: fresh memory that holds an argument
        // (`return &Holder{p: b}`) must not count as free of our locals. It
        // skips method summaries, since a bare call never calls a method, and
        // a miss -- a package-level func variable, an external -- is false.
        return param_escape_call_result_is_owned(pe, name);
    }

    if (call->function->type == AST_SELECTOR_EXPR) {
        if (selector_base_is_local(c, call->function)) return false;
        SelectorExprNode* sel = (SelectorExprNode*)call->function;
        if (!sel->expr || sel->expr->type != AST_IDENTIFIER || !sel->selector) return false;
        const char* pkg = ((IdentifierNode*)sel->expr)->name;
        // `qir.Qubit` and `qir.Result` are non-ARC opaque QIR handles (no
        // GOO_OBJ_HEADER_SIZE prefix); never classify a `qir.*` call result as
        // an ARC-owned heap buffer.
        if (strcmp(pkg, "qir") == 0) {
            return false;
        }
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
// The REBOUND path: releasing at the store instead of refusing outright
// ---------------------------------------------------------------------------

// Is this one value safe for a store to release?
//
// binding_is_owned answers "does this local OWN the value", which is the right
// question for the single release at scope exit. The store site asks a WIDER
// one, because at run time it frees whatever the slot holds and cannot tell the
// values apart: "would releasing this value ever free memory somebody else
// owns?" Two shapes answer no without being owned:
//
//   A LITERAL. Not owned, and not borrowed either. A string literal's global
//   carries a real header whose count is GOO_RC_IMMORTAL, and goo_release_with
//   checks that sentinel BEFORE the fetch_sub and returns (src/runtime/
//   runtime.c). So the release is a proven no-op, not a gamble.
//   include/runtime.h names `last := ""` in bench/daemon as the exact shape that
//   work was done for -- without this arm the daemon's local is refused by its
//   own initialiser and this whole path reclaims nothing.
//
//   NULL is NOT such a shape -- see LocalRecord.values for why it is refused.
static bool value_is_release_safe(Collected* c, const ParamEscapeResult* pe,
                                  ASTNode* value) {
    if (!value) return false;
    if (value->type == AST_LITERAL) return true;
    return binding_is_owned(c, pe, value);
}

// Every value that ever reached this slot is safe to release.
//
// ALL OR NOTHING, and that is forced rather than chosen: the release site is the
// store, one instruction with no idea which of the values the slot is holding on
// this iteration. One borrowed value therefore poisons the local.
static bool all_values_release_safe(Collected* c, const ParamEscapeResult* pe,
                                    const LocalRecord* r) {
    if (r->has_unsafe_value) return false;
    if (r->value_count == 0) return false;   // nothing recorded: fail closed
    for (size_t i = 0; i < r->value_count; i++) {
        if (!value_is_release_safe(c, pe, r->values[i])) return false;
    }
    return true;
}

// Does any expression under `e` name `target`?
//
// Sets *unknown on a node kind it cannot descend, which the caller reads as
// "assume an alias", and for a good reason: an alias this scanner cannot see is
// a use-after-free, not lost precision.
static bool expr_names_local(ASTNode* e, const char* target, bool* unknown) {
    if (!e) return false;

    switch (e->type) {
        case AST_IDENTIFIER: {
            const char* n = ((IdentifierNode*)e)->name;
            return n && strcmp(n, target) == 0;
        }
        case AST_LITERAL:
            return false;
        case AST_BINARY_EXPR:
            return expr_names_local(((BinaryExprNode*)e)->left, target, unknown)
                || expr_names_local(((BinaryExprNode*)e)->right, target, unknown);
        case AST_UNARY_EXPR:
            return expr_names_local(((UnaryExprNode*)e)->operand, target, unknown);
        case AST_POSTFIX_EXPR:
            return expr_names_local(((PostfixExprNode*)e)->operand, target, unknown);
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)e;
            if (expr_names_local(call->function, target, unknown)) return true;
            for (ASTNode* a = call->args; a; a = a->next) {
                if (expr_names_local(a, target, unknown)) return true;
            }
            return false;
        }
        case AST_INDEX_EXPR:
            return expr_names_local(((IndexExprNode*)e)->expr, target, unknown)
                || expr_names_local(((IndexExprNode*)e)->index, target, unknown);
        case AST_SELECTOR_EXPR:
            return expr_names_local(((SelectorExprNode*)e)->expr, target, unknown);
        case AST_SLICE_INDEX_EXPR: {
            SliceIndexExprNode* s = (SliceIndexExprNode*)e;
            return expr_names_local(s->expr, target, unknown)
                || expr_names_local(s->low, target, unknown)
                || expr_names_local(s->high, target, unknown);
        }
        case AST_SLICE_EXPR:
            for (ASTNode* x = ((SliceLitNode*)e)->elements; x; x = x->next) {
                if (expr_names_local(x, target, unknown)) return true;
            }
            return false;
        case AST_ARRAY_LITERAL:
            for (ASTNode* x = ((ArrayLitNode*)e)->elements; x; x = x->next) {
                if (expr_names_local(x, target, unknown)) return true;
            }
            return false;
        // A MAP LITERAL IS TAGGED AST_PAREN_EXPR (include/ast.h): that enum
        // slot was reserved for parenthesized expressions, which Goo parses
        // inline as identity, so the slot was free.
        case AST_PAREN_EXPR: {
            MapLitNode* m = (MapLitNode*)e;
            for (ASTNode* k = m->keys; k; k = k->next) {
                if (expr_names_local(k, target, unknown)) return true;
            }
            for (ASTNode* v = m->values; v; v = v->next) {
                if (expr_names_local(v, target, unknown)) return true;
            }
            return false;
        }
        case AST_STRUCT_LITERAL:
            for (ASTNode* f = ((StructLiteralNode*)e)->field_values; f; f = f->next) {
                if (expr_names_local(f, target, unknown)) return true;
            }
            return false;
        default:
            *unknown = true;
            return false;
    }
}

// A struct, slice, array or map literal, or the address of one: owned memory
// whose elements can be pointers the literal copied in.
static bool is_composite_value(const ASTNode* v) {
    if (!v) return false;
    switch (v->type) {
        case AST_STRUCT_LITERAL:
        case AST_SLICE_EXPR:      // a slice literal (SliceLitNode)
        case AST_ARRAY_LITERAL:
        case AST_PAREN_EXPR:      // a map literal (MapLitNode)
            return true;
        default:
            return is_addr_of_composite(v);
    }
}

// Does a `for ... range X` loop store into `name` while X may hold its value?
//
// THE RANGE TEMPORARY. `for _, v := range xs` evaluates xs ONCE and iterates
// that header to the end of the loop. It has no name, so no record holds it and
// has_alias cannot see it. A store into xs inside the body frees what the slot
// held -- the old value at a rebind, the replaced array at a self-append -- and
// the loop then reads freed memory. Measured on both paths before this rule:
// tests/arc_uaf/append_alias_range.goo (self-append) and
// tests/arc_uaf/range_rebind_string.goo (rebind, invalid read in
// goo_utf8_decode).
//
// ONLY THE BODY'S STORES. A store before the loop happens before the range
// expression is evaluated, and one after it happens when the temporary is dead.
// An enclosing loop that re-runs the range re-evaluates it, so that is the same
// two cases again.
//
// A range expression this scanner cannot read counts as naming `name`.
static bool range_rebinds_source(Collected* c, const char* name) {
    for (size_t i = 0; i < c->range_count; i++) {
        // A span past the log would read names that were never written, and
        // a NULL name is not a mismatch strcmp can answer.
        GOO_ASSERT(c->range_sites[i].begin <= c->range_sites[i].end
                   && c->range_sites[i].end <= c->store_count);
        bool stores = false;
        for (size_t j = c->range_sites[i].begin; j < c->range_sites[i].end && !stores; j++) {
            stores = strcmp(c->store_log[j], name) == 0;
        }
        if (!stores) {
            continue;
        }
        bool unknown = false;
        bool named = expr_names_local(c->range_sites[i].expr, name, &unknown);
        if (named || unknown) {   // unknown: fail closed
            return true;
        }
    }
    return false;
}

// Does another local hold a second pointer to what this one holds?
//
// THIS IS THE CONDITION NOTHING ELSE PROVIDES. `p := last` keeps a pointer to
// last's buffer WITHOUT making it outlive the function, so local_escape reports
// nothing and condition 1 passes. Releasing at last's next store then leaves `p`
// dangling inside the same function -- an invalid read, not a leak.
//
// COMPLETE BY CONSTRUCTION. Every binding in the function flows through
// note_declaration or note_assignment, so every value another local could have
// taken is in some record's `values` -- or, where that value is NULL, in
// `held_exprs`, which holds what the NULL-valued local is a view of. A
// statement kind the walk cannot read sets `unreadable` and refuses the whole
// function before this is ever consulted.
//
// A HOLDER THAT IS NOT A LOCAL is condition 1's, not this function's. A store
// through a field, an index or a pointer is CONTAINER_STORE, an appended
// element is CALL_OPAQUE, and a return, a global, a channel, a closure, a `go`
// or a `defer` argument each have a reason of their own (escape_core.c). Any
// of them marks the local, and decide() refuses it before this runs.
//
// WIDER THAN A BARE IDENTIFIER, because `p := borrow(last)` aliases just as hard
// as `p := last` when the callee returns its argument. So any MENTION inside a
// value that could carry a pointer out counts.
//
// AN OWNED VALUE IS PRUNED WHOLE, and that is what makes this usable rather than
// merely safe -- with ONE exception. binding_is_owned means "this expression
// yields FRESH memory": a new/composite, a concat, an owned call result, a
// non-retaining shim. A concat copies, new(T) holds nothing, and an owned call
// result holds no argument (param_escape_call_result_is_owned requires
// !return_escapes). A COMPOSITE LITERAL is the exception: it is fresh memory
// that holds its own elements, so it is scanned like any other value.
//
// MEASURED, not reasoned. Without this prune, scanning every mention refused
// `u := strings.ToUpper(last)` -- an ordinary read into a new local, which
// COPIES -- and the daemon shape reclaimed nothing. The probe went from 32 bytes
// back to 15,890. Reading the answer out of condition 2's own table is what
// separates "names it" from "could still be holding it".
static bool has_alias(Collected* c, const ParamEscapeResult* pe, const char* name) {
    for (size_t i = 0; i < c->count; i++) {
        LocalRecord* other = &c->items[i];
        if (strcmp(other->name, name) == 0) continue;   // its own values are not aliases
        for (size_t j = 0; j < other->value_count; j++) {
            ASTNode* v = other->values[j];
            if (!v) continue;                       // nothing bound, nothing held
            // Fresh: cannot alias -- EXCEPT a composite literal, which is
            // fresh memory built FROM its elements. `Holder{p: x}` and
            // `[]*Box{x}` hold x, and the store release on `x = ...` freed the
            // box they still read (review C-1). Scan a composite, nested
            // literals included, like any other value.
            if (binding_is_owned(c, pe, v) && !is_composite_value(v)) continue;
            bool unknown = false;
            if (expr_names_local(v, name, &unknown)) return true;
            if (unknown) return true;   // fail closed
        }
    }
    // No prune here: the local these feed is a VIEW of the expression (a type
    // switch's `v`, an if-let's value, one of several results), so a mention
    // means the view may point into `name`'s memory.
    for (size_t i = 0; i < c->held_count; i++) {
        bool unknown = false;
        bool named = expr_names_local(c->held_exprs[i], name, &unknown);
        if (named || unknown) {   // unknown: fail closed
            return true;
        }
    }
    // The one unnamed holder that outlives a statement.
    return range_rebinds_source(c, name);
}


// ---------------------------------------------------------------------------
// ADR 0005: may a map own a key that is a bare IDENTIFIER?
// ---------------------------------------------------------------------------
//
// binding_is_owned answers FALSE for every AST_IDENTIFIER, and it is right to:
// an identifier means another NAME holds the buffer, and only one owner may
// free it. That refusal costs bench/daemon/daemon.goo 180,000 bytes per 2,000
// requests, because `counts[f] = counts[f] + 1` differs from the accepted
// `counts[strings.ToUpper(f)] = 1` only in SPELLING.
//
// The question the refusal was standing in for is "could anything else free
// this buffer, or read it after the map does". THREE conditions answer it, and
// all three are required. Dropping any one frees memory somebody still reads,
// and examples/arc_map_key_local_probe.goo has a function for each.
//
//   OWNERSHIP  every value ever bound to the name is FRESH. Asked of the
//              BOUND VALUE rather than of the identifier, which is the whole
//              trick -- and asked of EVERY value, not the first, because a
//              rebind means the release site cannot tell them apart. Refuses
//              `v := s[1:]`, a view into a buffer this function never made.
//
//   THE REASON SET  exactly SUBSCRIPT_STORE. Any other reason means some other
//              sink can reach the buffer. Refuses `m[s] = 1; return s`, where
//              the caller still holds it. This is the condition ADR 0005 was
//              written for, and NOTHING ELSE PROVIDES IT: before the reason
//              set, that local and the daemon's were one indistinguishable
//              `escapes = true`.
//
//   THE COUNT  exactly one key site names it. ADR 0005 does NOT name this
//              condition and the reason set cannot supply it: in
//              `a[s] = 1; b[s] = 2` both marks say SUBSCRIPT_STORE, so
//              "escapes only as a subscript" is TRUE and two maps would each
//              free the one buffer.
//
// WHY NO ALIAS CONDITION, unlike the rebound path's has_alias. An alias that
// goes nowhere cannot dangle: `p := f` binds a value binding_is_owned calls
// borrowed, so p is never released, and p dies with the frame the map does. An
// alias that goes ANYWHERE adds a reason to `f` -- `return p` gives it RETURN,
// `append(parts, f)` gives it CALL_RETAIN -- and the reason condition refuses
// it. The reason set is doing has_alias's work here, and doing it wider.
//
// THE COUPLING TO WATCH. The map is safe as sole owner only because the LOCAL
// is never released: it reads RELEASE_NO_ESCAPES, since escaping as a subscript
// is still escaping. If condition 1 ever learns to consult the reason set, this
// function and key row 6 must change in the same commit or both owners free.
static bool identifier_key_is_owned(Collected* c, const ParamEscapeResult* pe,
                                    const LocalEscapeResult* le, const char* fn,
                                    ASTNode* key) {
    if (!key || key->type != AST_IDENTIFIER || !fn) return false;
    const char* name = ((IdentifierNode*)key)->name;
    if (!name) return false;

    // A name this walk never bound is a parameter, a global, or something the
    // walk could not read. None of them is ours to give away.
    LocalRecord* r = find_record(c, name);
    if (!r || r->has_unsafe_value || r->value_count == 0) return false;

    for (size_t i = 0; i < r->value_count; i++) {
        if (!binding_is_owned(c, pe, r->values[i])) return false;
    }

    if (local_escape_local_reasons(le, fn, name) != ESCAPE_REASON_SUBSCRIPT_STORE) {
        return false;
    }

    size_t sites = 0;
    for (size_t i = 0; i < c->key_count; i++) {
        ASTNode* k = c->key_sites[i];
        if (!k || k->type != AST_IDENTIFIER) continue;
        const char* other = ((IdentifierNode*)k)->name;
        if (other && strcmp(other, name) == 0) sites++;
    }
    return sites == 1;
}

// ---------------------------------------------------------------------------
// Plan construction
// ---------------------------------------------------------------------------

static void collected_free(Collected* c) {
    for (size_t i = 0; i < c->count; i++) {
        free(c->items[i].name);
        // The elements are borrowed AST nodes; only the array is ours.
        free(c->items[i].values);
    }
    free(c->items);
    c->items = NULL;
    c->count = c->cap = 0;
    // The elements are borrowed AST nodes; only the arrays are ours.
    free(c->key_sites);
    c->key_sites = NULL;
    c->key_count = c->key_cap = 0;
    free(c->concat_sites);
    c->concat_sites = NULL;
    c->concat_count = c->concat_cap = 0;
    free(c->elem_sites);
    c->elem_sites = NULL;
    c->elem_count = c->elem_cap = 0;
    free(c->held_exprs);
    c->held_exprs = NULL;
    c->held_count = 0;
    c->held_cap = 0;
    free(c->store_log);
    c->store_log = NULL;
    c->store_count = 0;
    c->store_cap = 0;
    free(c->range_sites);
    c->range_sites = NULL;
    c->range_count = 0;
    c->range_cap = 0;
    // The names are borrowed from the AST; only the array is ours.
    free(c->addr_names);
    c->addr_names = NULL;
    c->addr_count = 0;
}

// Is `&name` taken anywhere in the function? Fails closed: an unreadable walk
// answers true for every name.
static bool address_taken(const Collected* c, const char* name) {
    if (c->addr_unknown) {
        return true;
    }
    // A count with no array would read through NULL below.
    GOO_ASSERT(c->addr_count == 0U || c->addr_names != NULL);
    for (size_t i = 0; i < c->addr_count; i++) {
        if (strcmp(c->addr_names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static ReleaseVerdict decide(const Collected* c, const LocalRecord* r,
                            const ParamEscapeResult* pe,
                            const LocalEscapeResult* le,
                            const char* fn) {
    GOO_TESTCASE(c->unreadable);
    if (c->unreadable) return RELEASE_NO_UNKNOWN;
    GOO_TESTCASE(!r->declared);
    if (!r->declared) return RELEASE_NO_NO_BINDING;

    // NO GOO_TESTCASE ON THE FOUR CALL-BASED REFUSALS in this function
    // (local_escape_local_escapes, all_values_release_safe, has_alias,
    // binding_is_owned). A marker re-evaluates its expression, and none of
    // those four is PROVEN side-effect free -- marking them would trade a
    // measurement for a behaviour risk. Giving them markers means hoisting
    // each call into a local first, which is a change to this function's shape
    // rather than an observation of it. The six marked here are field
    // reads, so they cost nothing and can change nothing.
    //
    // Condition 1 first: it is the cheapest, and local_escape is conservative on
    // a miss so an unknown name refuses here.
    if (local_escape_local_escapes(le, fn, r->name)) return RELEASE_NO_ESCAPES;

    // Condition 3 before 4: an arena local inside a loop is refused for the
    // reason that actually makes it dangerous rather than merely wasteful.
    GOO_TESTCASE(r->arena_depth > 0);
    if (r->arena_depth > 0) return RELEASE_NO_ARENA;

    // CONDITION 4's LOOP HALF IS RELAXED, and conditions 6 and the loop-header
    // rule take its place. Codegen releases a loop-declared local at ITERATION
    // end (codegen_emit_loop_scope_releases), so "one of N" is no longer what
    // happens -- every iteration releases its own value.
    //
    // TWO REFUSALS REMAIN, and they are different questions:
    //
    //   loop_header   the local is the loop's own `i`, or a range key/value.
    //                 Bound once for the whole loop and read by the NEXT
    //                 iteration's condition, so iteration-end placement is
    //                 wrong for it. Still RELEASE_NO_LOOP_SCOPE: the cause is
    //                 unchanged, only its reach narrowed.
    //
    //   block_escapes the value is stored into something that outlives the
    //                 iteration. Measured before it was written -- see
    //                 docs/adr/0002-measurements/loop_carried_store_findings.md,
    //                 where this shape and a plain dead local were shown to be
    //                 INDISTINGUISHABLE under the old condition 4.
    // max_decl_depth, not block_depth: a name declared first at function
    // scope and again inside a loop has an iteration-scoped declaration too.
    GOO_TESTCASE(r->max_decl_depth > 0);
    if (r->max_decl_depth > 0) {
        GOO_TESTCASE(r->loop_header);
        if (r->loop_header) return RELEASE_NO_LOOP_SCOPE;
        GOO_TESTCASE(r->block_escapes);
        if (r->block_escapes) return RELEASE_NO_BLOCK_ESCAPE;
    }

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

    // CONDITION 4's RE-ASSIGNMENT HALF IS RELAXED, and conditions 2' and 7 take
    // its place. It used to refuse outright, because a second binding left the
    // first value with no release site. Releasing AT THE STORE is that missing
    // site: the assignment frees what the slot held, and scope exit frees
    // whatever it holds last.
    //
    // NOT WHAT #276 DID. That made a reassigned package-level GLOBAL immortal
    // (3b4757a, 0f25db9, 9068675) so that nothing frees it -- the opposite
    // direction, and no machinery to copy.
    //
    // CONDITION 2' replaces condition 2 for a rebound local:
    // all_values_release_safe -- EVERY value that reached the slot is safe to
    // free, because the store cannot tell them apart at run time. The ordinary
    // form asks only about `bound_value`, and for `last := ""` that is a
    // literal, so it would refuse the very shape this path exists for.
    if (r->binding_count > 1) {
        if (!all_values_release_safe((Collected*)c, pe, r)) return RELEASE_NO_NOT_OWNED;
    } else if (!binding_is_owned((Collected*)c, pe, r->bound_value)) {
        // Condition 2: it is the one that needs the callee summaries.
        return RELEASE_NO_NOT_OWNED;
    } else {
        // Bound once and owned: condition 2 holds.
    }

    // CONDITION 7 -- no other holder reads what a MID-FUNCTION free releases.
    //
    // A release at scope exit needs no alias test when every value in the
    // slot came from a recorded binding: every local of the function dies at
    // the same exit, and condition 6 covers a holder that outlives a loop
    // iteration. Two codegen paths free EARLIER than that, and both free what
    // the slot held while the function can still read it elsewhere:
    //
    //   a rebound local     the store releases the old value
    //                       (codegen_arc_release_before_store)
    //   a self-appended one the append releases the array it replaced
    //                       (codegen_arc_release_replaced_buffer)
    //
    // So condition 7 runs for either, and for an address-taken local (see
    // condition 8). Until PR #376's review it ran for a rebound local only: a
    // self-append is not a binding, so `u := xs; xs = append(xs, 1)` read
    // RELEASE_OK and the release freed u's array (tests/arc_uaf/
    // append_alias_*.goo, release-decision rows 81-88).
    //
    // THE ARGUMENT. Each free releases a value that came from this local's
    // slot. Condition 8 below makes the slot's contents KNOWN: with no `&L`,
    // only a recorded binding or a self-append writes the slot, so conditions
    // 2 and 2' have seen every value it can hold. A pointer into such a value
    // can then be held in three places only:
    //   (a) beyond a local -- a field, an index, a pointer, an appended element,
    //       a return, a global, a channel, a closure, `go`, `defer`, a callee
    //       parameter that is stored. Each is a reason in local_escape, and
    //       condition 1 above refused the local.
    //   (b) another named local. That local gets L's value either from one of
    //       its own bindings -- in its record, or in held_exprs when the record
    //       has NULL -- or from a write through ITS address (`set(&u, xs)`,
    //       `*p = xs`). The write passes L's value to a storing parameter or
    //       through a non-identifier lvalue, which is (a). has_alias refuses
    //       any recorded mention of L that is not a fresh copy.
    //   (c) an unnamed temporary that outlives the statement -- only a range
    //       expression does. has_alias also asks range_rebinds_source, which
    //       refuses a store in the loop's body.
    // A temporary inside the storing statement itself is read before the
    // free: the right-hand side is complete when the release runs.
    //
    // local_escape cannot see (b) or (c), so nothing else refuses them.
    bool addr_taken = address_taken(c, r->name);
    if (r->binding_count > 1 || r->self_appended || addr_taken) {
        if (has_alias((Collected*)c, pe, r->name)) return RELEASE_NO_ALIASED;
    }

    // CONDITION 8 -- no write that no record describes reaches L's slot.
    //
    // THE RULE: a local whose address `&L` appears anywhere in the function
    // (or that a closure captures, which is the same reference) is refused.
    // If a named local holds the address (`p := &L`), its record names L and
    // condition 7 above has already refused L as ALIASED. Every other use --
    // `&L` passed to a call, stored, returned, inside a composite -- reads
    // RELEASE_NO_ADDR_TAKEN.
    //
    // WHY BOTH ARE NEEDED. Conditions 2 and 7 read L's recorded values. A
    // write through the address puts into L's slot a value no binding of L
    // records: `set(&xs, v)` with `*p = v` hands xs the CALLER's v, and the
    // exit release then frees v. Measured: tests/arc_uaf/addr_callee_exit.goo,
    // addr_callee_string.goo (the store release of a rebound string) and
    // addr_local_exit.goo were live use-after-free bugs on main before PR #376;
    // addr_callee_append.goo is the same shape through the replaced-array
    // release. Release-decision rows 93-96.
    //
    // FAILS CLOSED: returns_fresh_addr_taken reports a walk it could not
    // finish, and then every local of the function reads as address-taken.
    if (addr_taken) {
        return RELEASE_NO_ADDR_TAKEN;
    }

    // THE INVARIANT THIS WHOLE MODULE RESTS ON, and until now it lived only in
    // condition 1's ordering. RELEASE_OK means codegen emits a free. If the
    // local escaped, that free dangles a pointer that is still reachable --
    // the exact failure PR #278 shipped and that a green verify-core, 493
    // goldens and an 8/8 teeth run all missed.
    //
    // Condition 1 returns RELEASE_NO_ESCAPES near the top of this function, so
    // reaching here already implies the local does not escape. That is the
    // point: the assert is what NOTICES if a later edit reorders the conditions
    // or adds a path that skips condition 1.
    GOO_ASSERT(!local_escape_local_escapes(le, fn, r->name));
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
    // Condition 2 reads returns_fresh, and only this pass fills it.
    returns_fresh_compute(program, pe, le);

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
        c.decl = (const FuncDeclNode*)d;
        WalkCtx ctx = { .out = &c, .block_depth = 0, .arena_depth = 0,
                        .in_loop_header = false, };
        walk_stmts(&ctx, ((FuncDeclNode*)d)->body);
        c.addr_unknown = !returns_fresh_addr_taken((FuncDeclNode*)d, &c.addr_names,
                                                   &c.addr_count);
        mark_block_escapes(&c, local_escape_find_summary(le, name));

        ReleasePlanFunction* pf = &plan->functions[plan->count];
        pf->function_name = xstrdup(name);
        pf->count = 0;
        pf->decisions = c.count ? calloc(c.count, sizeof(ReleaseDecision)) : NULL;
        if (!pf->function_name || (c.count && !pf->decisions)) {
            free(pf->function_name);
            free(pf->decisions);
            collected_free(&c);
            continue;   // drop this function; a missing entry refuses, which is safe
        }

        for (size_t i = 0; i < c.count; i++) {
            pf->decisions[i].local_name = xstrdup(c.items[i].name);
            if (!pf->decisions[i].local_name) continue;
            pf->decisions[i].verdict = decide(&c, &c.items[i], pe, le, name);
            // DIAGNOSTIC ONLY, captured HERE so it cannot drift. `decide` reads
            // the same `le` on the same line, so this records the reason set
            // the verdict was actually taken from rather than one recomputed
            // later from a table that may have moved on. Nothing branches on
            // it -- see the field's comment in release_decision.h.
            pf->decisions[i].diagnostic_reasons =
                local_escape_local_reasons(le, name, c.items[i].name);
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
                    // Two ways to own a key, and they ask DIFFERENT questions.
                    // binding_is_owned asks whether the key EXPRESSION is a
                    // fresh temporary, which is every row 1-5. The second asks
                    // whether an IDENTIFIER's local is one nobody else can
                    // reach -- see its own comment for the three conditions.
                    if (binding_is_owned(&c, pe, c.key_sites[i])
                        || identifier_key_is_owned(&c, pe, le, name, c.key_sites[i])) {
                        pf->owned_keys[pf->owned_key_count++] = c.key_sites[i];
                    }
                }
            }
            // A NULL array means no key is owned, which is the safe answer.
        }

        // Same timing, same reason: binding_is_owned needs `pe`, which
        // release_plan_analyze frees below.
        if (c.concat_count) {
            pf->owned_concat_operands = calloc(c.concat_count, sizeof(ASTNode*));
            if (pf->owned_concat_operands) {
                for (size_t i = 0; i < c.concat_count; i++) {
                    if (binding_is_owned(&c, pe, c.concat_sites[i])) {
                        pf->owned_concat_operands[pf->owned_concat_count++] =
                            c.concat_sites[i];
                    }
                }
            }
            // A NULL array means no operand is owned, which is the safe answer.
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
        free(pf->owned_concat_operands);
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

// Fails closed on every miss, exactly as local_escape_local_reasons does, so
// the two agree about what "I cannot answer" looks like.
EscapeReasons release_plan_diagnostic_reasons(const ReleasePlan* plan,
                                              const char* fn, const char* local) {
    if (!plan || !fn || !local) return ESCAPE_REASON_ALL;
    for (size_t i = 0; i < plan->count; i++) {
        if (strcmp(plan->functions[i].function_name, fn) != 0) continue;
        const ReleasePlanFunction* pf = &plan->functions[i];
        for (size_t j = 0; j < pf->count; j++) {
            if (strcmp(pf->decisions[j].local_name, local) == 0) {
                return pf->decisions[j].diagnostic_reasons;
            }
        }
        return ESCAPE_REASON_ALL;
    }
    return ESCAPE_REASON_ALL;
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

bool release_plan_concat_operand_is_owned(const ReleasePlan* plan, const char* fn,
                                          const ASTNode* operand) {
    if (!plan || !fn || !operand) return false;
    for (size_t i = 0; i < plan->count; i++) {
        if (strcmp(plan->functions[i].function_name, fn) != 0) continue;
        const ReleasePlanFunction* pf = &plan->functions[i];
        for (size_t j = 0; j < pf->owned_concat_count; j++) {
            if (pf->owned_concat_operands[j] == operand) return true;
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
        case RELEASE_NO_ALIASED:    return "RELEASE_NO_ALIASED";
        case RELEASE_NO_BLOCK_ESCAPE: return "RELEASE_NO_BLOCK_ESCAPE";
        case RELEASE_NO_ADDR_TAKEN: return "RELEASE_NO_ADDR_TAKEN";
        case RELEASE_NO_NO_BINDING: return "RELEASE_NO_NO_BINDING";
        case RELEASE_NO_UNKNOWN:    return "RELEASE_NO_UNKNOWN";
    }
    return "RELEASE_NO_UNKNOWN";
}
