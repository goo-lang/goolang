// Arena leg — Task 7b: per-alloc-site block-escape decisions.
// See include/block_escape.h and
// docs/superpowers/specs/2026-07-07-arena-7b-block-escape-decision-design.md
// for the design this file implements.
//
// SOUNDNESS SIBLING of src/types/param_escape.c (Task 7a): this file adapts
// that module's taint-propagation engine — the same TaintSet
// representation, the same escape_expr_taint/escape_walk_stmt switch shape, the same
// sink vocabulary (return / non-local store / closure capture / goroutine
// arg / retaining call argument), and the same local-fixpoint loop for
// for-loop back-edges — with two coordinates moved: the taint SOURCE is
// each allocation site inside an `arena {}` block (7a's source was a
// function parameter), and the escape BOUNDARY is that arena block (7a's
// boundary was the enclosing function). That shared taint-propagation shape
// USED to be mirrored by hand across these files, and this comment used to say
// so. It is now one module — src/types/escape_core.c — because the third
// consumer arrived and because the hand-mirroring had already dropped a case
// twice. A soundness fix now lands in all three passes at once.
//
// ---------------------------------------------------------------------
// How this differs structurally from param_escape.c (two-pass per block)
// ---------------------------------------------------------------------
// 7a's "sources" (function parameters) are known up front, so it can
// pre-seed a LocalEnv and run straight to a fixpoint. 7b's sources
// (allocation sites) are discovered by WALKING the arena block's body, and
// that discovery must happen ONCE, before the taint fixpoint starts,
// so every site gets a stable bit index across fixpoint passes. So each
// arena-block "unit" gets:
//
//   Pass 1 (discover_stmt/discover_expr): a structural, non-taint walk of
//   the WHOLE program that (a) finds every `arena {}` block (each becomes
//   a Unit, in the order its ArenaBlockNode is first visited — pre-order)
//   and (b) assigns every `new(T)` / `&composite` node to the site list of
//   its INNERMOST enclosing arena block. A site textually inside a nested
//   arena block is never added to an outer block's site list — it is
//   collected as that inner Unit's own, separate site list instead. A site
//   outside every arena block is simply never added anywhere (see row 14
//   of the test matrix): `current_unit == SIZE_MAX` while walking it.
//
//   Pass 2 (escape_expr_taint/escape_walk_stmt, driven by analyze_unit): for each Unit,
//   an independent taint-fixpoint walk of THAT unit's own body only. A
//   name is a "local" of this unit's LocalEnv iff a var/:=/etc. declared
//   it somewhere textually inside the unit's body (escape_walk_stmt transparently
//   descends into a NESTED arena block exactly like param_escape.c does —
//   so a variable declared inside a nested block still counts as "declared
//   textually within" the outer unit too, per the design doc's literal
//   wording). Because ownership was already resolved in Pass 1 purely by
//   AST node pointer identity, when this unit's ctx walk re-encounters a
//   site node that belongs to a DIFFERENT (nested) unit, escape_expr_taint simply
//   does not recognize it as one of ITS OWN sites (find_site_index misses)
//   and falls through to the ordinary structural-recursion case — which
//   correctly still picks up any of THIS unit's own already-tainted
//   locals referenced inside that nested node's subexpressions, without
//   crediting the nested site's own identity to this unit's bit space.
// ---------------------------------------------------------------------

#include "block_escape.h"
#include "escape_core.h"
#include "nonretaining.h"
#include "token.h"
#include <stdlib.h>
#include <string.h>

#define BLOCK_ESCAPE_NO_UNIT ((size_t)-1)

// =============================================================================
// TaintSet, LocalEnv and the whole body walk now live in
// src/types/escape_core.c. They used to be a hand-maintained copy of
// param_escape.c's, and this file's header comment used to explain why the
// duplication was acceptable. It stopped being acceptable when the copies
// drifted — see include/escape_core.h for the two defects that shipped.
//
// This module keeps what is its own: allocation-site discovery, and the hooks
// that tell the shared engine an ALLOC SITE is the source and the ARENA BLOCK
// is the boundary. A slot here is a site index.

// =============================================================================
// Pass 1: Unit/site discovery. A Unit is one `arena {}` block instance.
// site_nodes[] holds this unit's OWN sites (borrowed AST node pointers),
// in source order, excluding any site owned by a nested arena block.
// =============================================================================

typedef struct {
    ASTNode*  body;         // ArenaBlockNode's ->body (borrowed)
    ASTNode** site_nodes;   // owned array of borrowed pointers
    size_t    site_count;
    size_t    site_cap;
} Unit;

typedef struct {
    Unit*  items;
    size_t count;
    size_t cap;
} UnitList;

static void unit_list_free(UnitList* list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) free(list->items[i].site_nodes);
    free(list->items);
    list->items = NULL;
    list->count = list->cap = 0;
}

// Appends a new (empty) unit for `body` and returns its index, or
// BLOCK_ESCAPE_NO_UNIT on allocation failure. Returned index stays valid
// even if a later push reallocs `list->items` — callers must always index
// through `list`, never cache a raw Unit* across a call that can grow it.
static size_t unit_list_push(UnitList* list, ASTNode* body) {
    if (list->count >= list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        Unit* grown = realloc(list->items, new_cap * sizeof(Unit));
        if (!grown) return BLOCK_ESCAPE_NO_UNIT;
        list->items = grown;
        list->cap = new_cap;
    }
    Unit* u = &list->items[list->count];
    u->body = body;
    u->site_nodes = NULL;
    u->site_count = 0;
    u->site_cap = 0;
    return list->count++;
}

// Records `site_node` as belonging to unit `unit_idx` (a no-op, not a
// failure, when unit_idx == BLOCK_ESCAPE_NO_UNIT: a site outside every
// arena block is simply never recorded anywhere — see test row 14).
// Returns false only on allocation failure.
static bool site_append(UnitList* list, size_t unit_idx, ASTNode* site_node) {
    if (unit_idx == BLOCK_ESCAPE_NO_UNIT) return true;
    Unit* u = &list->items[unit_idx]; // re-fetched fresh every call; safe under reallocation
    if (u->site_count >= u->site_cap) {
        size_t new_cap = u->site_cap ? u->site_cap * 2 : 4;
        ASTNode** grown = realloc(u->site_nodes, new_cap * sizeof(ASTNode*));
        if (!grown) return false;
        u->site_nodes = grown;
        u->site_cap = new_cap;
    }
    u->site_nodes[u->site_count++] = site_node;
    return true;
}

static bool is_new_call(ASTNode* expr) {
    if (!expr || expr->type != AST_CALL_EXPR) return false;
    CallExprNode* call = (CallExprNode*)expr;
    return call->function && call->function->type == AST_IDENTIFIER &&
           strcmp(((IdentifierNode*)call->function)->name, "new") == 0;
}

static bool is_addr_of_composite(ASTNode* expr) {
    if (!expr || expr->type != AST_UNARY_EXPR) return false;
    UnaryExprNode* u = (UnaryExprNode*)expr;
    return u->operator == TOKEN_BIT_AND && u->operand && u->operand->type == AST_STRUCT_LITERAL;
}

static bool discover_stmt(ASTNode* stmt, UnitList* units, size_t unit_idx);

// Recurses into every expression sub-node this front-end can produce,
// looking for allocation-site nodes (see is_new_call/is_addr_of_composite)
// and recording each one against `unit_idx`. Deliberately does NOT recurse
// into a FuncLitNode's body (mirrors param_escape.c's own choice not to
// re-walk closure bodies): a `new(...)`/`&composite` written inside a
// closure literal is not classified as a site of the enclosing arena block
// this cut — conservative miss, documented in the task report, not fixed
// here. Returns false only on allocation failure.
static bool discover_expr(ASTNode* expr, UnitList* units, size_t unit_idx) {
    if (!expr) return true;

    if (is_new_call(expr)) {
        if (!site_append(units, unit_idx, expr)) return false;
        // new(T)'s sole "argument" is a type name, not a value expression;
        // nothing further to recurse into.
        return true;
    }
    if (is_addr_of_composite(expr)) {
        if (!site_append(units, unit_idx, expr)) return false;
        // Still recurse into the composite's own field values below (a
        // field value can itself contain a nested allocation site).
    }

    switch (expr->type) {
        case AST_IDENTIFIER:
        case AST_LITERAL:
            return true;
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            return discover_expr(b->left, units, unit_idx) && discover_expr(b->right, units, unit_idx);
        }
        case AST_UNARY_EXPR:
            return discover_expr(((UnaryExprNode*)expr)->operand, units, unit_idx);
        case AST_POSTFIX_EXPR:
            return discover_expr(((PostfixExprNode*)expr)->operand, units, unit_idx);
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            if (!discover_expr(call->function, units, unit_idx)) return false;
            for (ASTNode* a = call->args; a; a = a->next) {
                if (!discover_expr(a, units, unit_idx)) return false;
            }
            return true;
        }
        case AST_INDEX_EXPR: {
            IndexExprNode* ie = (IndexExprNode*)expr;
            return discover_expr(ie->expr, units, unit_idx) && discover_expr(ie->index, units, unit_idx);
        }
        case AST_SLICE_INDEX_EXPR: {
            SliceIndexExprNode* se = (SliceIndexExprNode*)expr;
            return discover_expr(se->expr, units, unit_idx) &&
                   discover_expr(se->low, units, unit_idx) &&
                   discover_expr(se->high, units, unit_idx);
        }
        case AST_SELECTOR_EXPR:
            return discover_expr(((SelectorExprNode*)expr)->expr, units, unit_idx);
        case AST_FUNC_LIT: {
            // Phase 1a (ADR 0002): a CAPTURING literal allocates an
            // environment struct at function_codegen.c's `env_ptr`, so the
            // literal node IS that allocation's site. A NON-capturing literal
            // allocates nothing (that path keeps env NULL), so registering one
            // would record a decision with no allocation behind it — hence the
            // captured_count guard. captured_count is populated by the type
            // checker, which the header already requires to have run.
            //
            // Still does NOT recurse into the body — see the doc comment
            // above. Registering the closure ITSELF and classifying the
            // allocations written INSIDE it are orthogonal problems.
            FuncLitNode* lit = (FuncLitNode*)expr;
            if (lit->captured_count > 0) {
                if (!site_append(units, unit_idx, expr)) return false;
            }
            return true;
        }
        case AST_STRUCT_LITERAL: {
            StructLiteralNode* sl = (StructLiteralNode*)expr;
            for (ASTNode* v = sl->field_values; v; v = v->next) {
                if (!discover_expr(v, units, unit_idx)) return false;
            }
            return true;
        }
        case AST_SLICE_EXPR: { // slice literal (SliceLitNode)
            SliceLitNode* sl = (SliceLitNode*)expr;
            for (ASTNode* e = sl->elements; e; e = e->next) {
                if (!discover_expr(e, units, unit_idx)) return false;
            }
            return true;
        }
        case AST_ARRAY_LITERAL: {
            ArrayLitNode* al = (ArrayLitNode*)expr;
            for (ASTNode* e = al->elements; e; e = e->next) {
                if (!discover_expr(e, units, unit_idx)) return false;
            }
            return true;
        }
        case AST_KEYED_ELEMENT:
            return discover_expr(((KeyedElementNode*)expr)->value, units, unit_idx);
        case AST_PAREN_EXPR: { // map literal (MapLitNode)
            MapLitNode* ml = (MapLitNode*)expr;
            for (ASTNode* k = ml->keys; k; k = k->next) {
                if (!discover_expr(k, units, unit_idx)) return false;
            }
            for (ASTNode* v = ml->values; v; v = v->next) {
                if (!discover_expr(v, units, unit_idx)) return false;
            }
            return true;
        }
        case AST_SLICE_CONVERSION:
            return discover_expr(((SliceConvNode*)expr)->operand, units, unit_idx);
        case AST_TYPE_ASSERT:
            return discover_expr(((TypeAssertNode*)expr)->expr, units, unit_idx);
        default:
            // Genuinely unhandled expression kind: nothing further
            // discoverable inside it via this walker. Any allocation
            // buried inside is simply never classified as a site (safe
            // miss — 7c's lookup then defaults to heap for it).
            return true;
    }
}

// Discover pass over a statement list (mirrors param_escape.c's escape_walk_stmt
// coverage of statement kinds, minus any taint semantics — this pass only
// finds arena blocks (spawning new units) and allocation sites (recording
// them against the current unit). Returns false only on allocation failure.
static bool discover_stmt(ASTNode* stmt, UnitList* units, size_t unit_idx) {
    for (; stmt; stmt = stmt->next) {
        switch (stmt->type) {
            case AST_BLOCK_STMT:
                if (!discover_stmt(((BlockStmtNode*)stmt)->statements, units, unit_idx)) return false;
                break;

            case AST_EXPR_STMT:
                if (!discover_expr(((ExprStmtNode*)stmt)->expr, units, unit_idx)) return false;
                break;

            case AST_IF_STMT: {
                IfStmtNode* n = (IfStmtNode*)stmt;
                if (!discover_expr(n->condition, units, unit_idx)) return false;
                if (!discover_stmt(n->then_stmt, units, unit_idx)) return false;
                if (!discover_stmt(n->else_stmt, units, unit_idx)) return false;
                break;
            }

            case AST_IF_LET_STMT: {
                IfLetStmtNode* n = (IfLetStmtNode*)stmt;
                if (!discover_expr(n->nullable_expr, units, unit_idx)) return false;
                if (!discover_stmt(n->then_stmt, units, unit_idx)) return false;
                if (!discover_stmt(n->else_stmt, units, unit_idx)) return false;
                break;
            }

            case AST_FOR_STMT: {
                ForStmtNode* n = (ForStmtNode*)stmt;
                if (n->range_expr) {
                    if (!discover_expr(n->range_expr, units, unit_idx)) return false;
                } else {
                    if (n->init && !discover_stmt(n->init, units, unit_idx)) return false;
                    if (n->condition && !discover_expr(n->condition, units, unit_idx)) return false;
                    if (n->post && !discover_stmt(n->post, units, unit_idx)) return false;
                }
                if (!discover_stmt(n->body, units, unit_idx)) return false;
                break;
            }

            case AST_RETURN_STMT: {
                ReturnStmtNode* n = (ReturnStmtNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) {
                    if (!discover_expr(v, units, unit_idx)) return false;
                }
                break;
            }

            case AST_GO_STMT:
                if (!discover_expr(((GoStmtNode*)stmt)->call, units, unit_idx)) return false;
                break;

            case AST_DEFER_STMT:
                if (!discover_expr(((DeferStmtNode*)stmt)->call, units, unit_idx)) return false;
                break;

            case AST_BREAK_STMT:
            case AST_CONTINUE_STMT:
                break;

            case AST_VAR_DECL: {
                VarDeclNode* n = (VarDeclNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) {
                    if (!discover_expr(v, units, unit_idx)) return false;
                }
                break;
            }

            case AST_CONST_DECL: {
                ConstDeclNode* n = (ConstDeclNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) {
                    if (!discover_expr(v, units, unit_idx)) return false;
                }
                break;
            }

            case AST_MULTI_ASSIGN: {
                MultiAssignNode* n = (MultiAssignNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) {
                    if (!discover_expr(v, units, unit_idx)) return false;
                }
                for (ASTNode* t = n->targets; t; t = t->next) {
                    if (!discover_expr(t, units, unit_idx)) return false;
                }
                break;
            }

            case AST_SWITCH_STMT: {
                SwitchStmtNode* n = (SwitchStmtNode*)stmt;
                if (n->tag && !discover_expr(n->tag, units, unit_idx)) return false;
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type == AST_CASE_CLAUSE) {
                        CaseClauseNode* cc = (CaseClauseNode*)c;
                        for (ASTNode* e = cc->exprs; e; e = e->next) {
                            if (!discover_expr(e, units, unit_idx)) return false;
                        }
                        if (!discover_stmt(cc->body, units, unit_idx)) return false;
                    }
                }
                break;
            }

            case AST_TYPE_SWITCH: {
                TypeSwitchNode* n = (TypeSwitchNode*)stmt;
                if (!discover_expr(n->expr, units, unit_idx)) return false;
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type == AST_TYPE_CASE) {
                        if (!discover_stmt(((TypeCaseNode*)c)->body, units, unit_idx)) return false;
                    }
                }
                break;
            }

            case AST_SELECT_STMT: {
                SelectStmtNode* n = (SelectStmtNode*)stmt;
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type == AST_SELECT_CASE) {
                        SelectCaseNode* sc = (SelectCaseNode*)c;
                        if (!discover_stmt(sc->comm, units, unit_idx)) return false;
                        if (!discover_stmt(sc->body, units, unit_idx)) return false;
                    }
                }
                break;
            }

            case AST_UNSAFE_STMT:
                if (!discover_stmt(((UnsafeStmtNode*)stmt)->body, units, unit_idx)) return false;
                break;

            case AST_ARENA_BLOCK: {
                ASTNode* body = ((ArenaBlockNode*)stmt)->body;
                size_t new_idx = unit_list_push(units, body);
                if (new_idx == BLOCK_ESCAPE_NO_UNIT) return false;
                // A site declared textually within this block belongs to
                // IT, not to whatever unit enclosed it (innermost-block
                // rule) — walk its body under the NEW unit's index. Any
                // arena block nested even deeper inside `body` is
                // discovered by this same recursive call and becomes its
                // own further-nested unit.
                if (!discover_stmt(body, units, new_idx)) return false;
                break;
            }

            case AST_ASSERT_STMT: {
                AssertStmtNode* n = (AssertStmtNode*)stmt;
                if (!discover_expr(n->condition, units, unit_idx)) return false;
                if (n->message && !discover_expr(n->message, units, unit_idx)) return false;
                break;
            }

            case AST_ASSUME_STMT:
                if (!discover_expr(((AssumeStmtNode*)stmt)->condition, units, unit_idx)) return false;
                break;

            default:
                // Genuinely unhandled statement kind: nothing further
                // discoverable inside it (safe miss, same rationale as
                // discover_expr's default case).
                break;
        }
    }
    return true;
}

// =============================================================================
// Pass 2: per-unit taint fixpoint. The engine itself is
// src/types/escape_core.c; what follows is only this pass's half of the
// contract — its owner struct and its hooks.
// =============================================================================

// Carried through EscapeCtx.owner.
typedef struct {
    const ParamEscapeResult* summaries; // may be NULL; borrowed
    ASTNode** site_nodes;  // this unit's OWN sites, borrowed, same order as reasons[]
    size_t    site_count;
} BlockOwner;

// Linear scan — a unit's own site count is expected to stay small (it is
// bounded by how many new(...)/&composite expressions appear directly
// inside one arena block, excluding nested sub-blocks).
static size_t find_site_index(EscapeCtx* ctx, ASTNode* node) {
    BlockOwner* own = (BlockOwner*)ctx->owner;
    for (size_t i = 0; i < own->site_count; i++) {
        if (own->site_nodes[i] == node) return i;
    }
    return ESCAPE_NO_SLOT;
}

// THE source hook, and this is the only pass that installs one. An allocation
// site is an EXPRESSION (`&T{}`, `new(T)`, a closure literal), where a
// parameter and a local are both NAMES — which is why the other two passes
// seed the environment instead and leave this NULL.
//
// find_site_index is the real discriminator: only registered sites are in
// site_nodes, so an `&composite` that belongs to a NESTED unit misses here and
// the engine falls through to ordinary recursion. That fall-through is
// deliberate and is what stops a nested site's identity being credited to this
// unit's bit space.
static bool block_expr_source_slot(EscapeCtx* ctx, ASTNode* expr, size_t* out_slot) {
    size_t idx = find_site_index(ctx, expr);
    if (idx == ESCAPE_NO_SLOT) return false;
    *out_slot = idx;
    return true;
}

// An alloc site is bound to no name, so binding is an ordinary env union.
static bool block_bind(EscapeCtx* ctx, const char* name, const TaintSet* value) {
    return escape_env_add_or_union(ctx->env, name, value);
}

static bool block_callee_retention(EscapeCtx* ctx, const char* name,
                                   const bool** out_escapes, size_t* out_count,
                                   bool* out_return_escapes) {
    BlockOwner* own = (BlockOwner*)ctx->owner;
    const ParamEscapeSummary* callee = param_escape_lookup(own->summaries, name);
    if (!callee) return false;
    *out_escapes = callee->escapes;
    *out_count = callee->param_count;
    *out_return_escapes = callee->return_escapes;
    return true;
}

static const EscapeHooks BLOCK_HOOKS = {
    .bind = block_bind,
    .expr_source_slot = block_expr_source_slot,
    // No interprocedural return signal: that is param_escape's business.
    .on_return = NULL,
    .callee_retention = block_callee_retention,
    // TRUE, and this is the one place the boundary genuinely forces a
    // different answer from param_escape. A defer'd call runs at the enclosing
    // FUNCTION's exit, which is always AFTER this arena block closed and freed
    // its arena, so a deferred argument outlives the block exactly the way a
    // goroutine argument does. Treating it as an ordinary call would arena-free
    // the value before the deferred call reads it.
    .defer_is_like_go = true,
};

// Sink #2 (store to a non-block-local location). Membership in ctx->env IS
// this module's definition of "a plain local of THIS unit" — see this
// file's header comment: it is seeded ONLY by var/:=/etc. encountered
// while walking this unit's own body, so a function parameter, an
// outer-scope (pre-block) local, or a package global is correctly absent
// and therefore a sink target. `_` is a pure discard: neither a sink nor a
// local. Any non-identifier lvalue (*p, obj.field, arr[k], ...) is
// unconditionally a sink, same conservative simplification param_escape.c
// documents for its own sink #2.

// Sink #5 (retaining call argument) + the call-result taint rule, using
// `summaries` (7a's ParamEscapeResult) in place of param_escape.c's own
// Registry lookup. param_escape_lookup/param_escape_param_escapes already
// return the conservative "true"/NULL on an unknown function or a NULL
// result, so "summaries == NULL" and "external/unregistered callee" fall
// out through the exact same code path here.

// Sink #4 (goroutine): every argument of the launched call escapes
// unconditionally, independent of the callee's own summary — identical to
// param_escape.c's handle_go_call.

// A defer'd call runs at the enclosing FUNCTION's exit, which always happens
// AFTER this arena block has closed and freed its arena. So a value passed to
// a defer outlives the block exactly the way a goroutine argument does — its
// arguments (snapshotted at defer-time and read at function exit) must escape
// the block, or they would be arena-freed before the deferred call reads them
// (a use-after-free). This is the ONE place block-escape must diverge from
// param_escape.c: at FUNCTION granularity a defer runs within the frame, so
// param_escape.c correctly treats it as an ordinary call; at BLOCK granularity
// the defer fires past the block boundary, so here it is an unconditional
// escape — identical treatment to handle_go_call above (sink #4), NOT the
// retention-based sink #5.

// Seeds/updates locals for a VarDeclNode/ConstDeclNode-shaped
// names/name_count/values triple. Identical to param_escape.c's
// seed_names_from_values.

// Runs one unit's intraprocedural taint analysis to a LOCAL fixpoint (same
// termination argument as param_escape.c's analyze_function_body: the
// taint map only grows, so repeating the whole-body walk until nothing
// changes is sound and also handles for-loop back-edges). LocalEnv starts
// EMPTY here — unlike a function's params, an arena block has no
// pre-existing "parameters" to seed; every local comes from a var/:=/etc.
// encountered during the walk itself.
static void analyze_unit(const ParamEscapeResult* summaries, Unit* u, EscapeReasons* reasons) {
    LocalEnv env = {0};
    BlockOwner own = {
        .summaries = summaries,
        .site_nodes = u->site_nodes,
        .site_count = u->site_count,
    };
    EscapeCtx ctx = {
        .env = &env,
        .slot_count = u->site_count,
        .reasons = reasons,
        .hooks = &BLOCK_HOOKS,
        .owner = &own,
    };

    // Defensive backstop, not the termination argument (see
    // param_escape.c's identical comment on its own cap): the taint map is
    // a finite monotone lattice, so while(changed) always terminates on
    // its own. Fails CLOSED (marks every site escaping) if ever hit.
    const size_t MAX_LOCAL_PASSES = 4096;
    bool changed = true;
    size_t pass = 0;
    while (changed) {
        changed = false;
        pass++;
        if (pass > MAX_LOCAL_PASSES) {
            for (size_t i = 0; i < u->site_count; i++) reasons[i] = ESCAPE_REASON_UNCLASSIFIED;
            break;
        }
        escape_walk_stmt(&ctx, u->body, &changed);
    }

    escape_env_free(&env);
}

// =============================================================================
// Public API
// =============================================================================

BlockEscapeResult* block_escape_analyze(ASTNode* program, const ParamEscapeResult* summaries) {
    UnitList units = {0};

    if (program && program->type == AST_PROGRAM) {
        ProgramNode* prog = (ProgramNode*)program;
        for (ASTNode* d = prog->decls; d; d = d->next) {
            if (d->type == AST_FUNC_DECL) {
                FuncDeclNode* fd = (FuncDeclNode*)d;
                if (!discover_stmt(fd->body, &units, BLOCK_ESCAPE_NO_UNIT)) {
                    unit_list_free(&units);
                    return NULL;
                }
            }
        }
    }

    size_t total = 0;
    for (size_t i = 0; i < units.count; i++) total += units.items[i].site_count;

    BlockEscapeResult* result = xmalloc(sizeof(BlockEscapeResult));
    if (!result) {
        unit_list_free(&units);
        return NULL;
    }
    result->count = total;
    result->decisions = NULL;
    if (total > 0) {
        result->decisions = calloc(total, sizeof(BlockEscapeDecision));
        if (!result->decisions) {
            free(result);
            unit_list_free(&units);
            return NULL;
        }
    }

    size_t out_idx = 0;
    for (size_t i = 0; i < units.count; i++) {
        Unit* u = &units.items[i];
        EscapeReasons* reasons = u->site_count ? calloc(u->site_count, sizeof(EscapeReasons)) : NULL;
        if (u->site_count && !reasons) {
            free(result->decisions);
            free(result);
            unit_list_free(&units);
            return NULL;
        }

        analyze_unit(summaries, u, reasons);

        for (size_t s = 0; s < u->site_count; s++) {
            result->decisions[out_idx].site = u->site_nodes[s];
            result->decisions[out_idx].escapes_block = reasons[s] != ESCAPE_REASON_NONE;
            out_idx++;
        }
        free(reasons);
    }

    unit_list_free(&units);
    return result;
}

void block_escape_result_free(BlockEscapeResult* result) {
    if (!result) return;
    free(result->decisions); // decisions[].site is borrowed, not owned
    free(result);
}

bool block_escape_site_escapes(const BlockEscapeResult* result, const ASTNode* site) {
    if (!result || !site) return true; // conservative miss
    for (size_t i = 0; i < result->count; i++) {
        if (result->decisions[i].site == site) return result->decisions[i].escapes_block;
    }
    return true; // unknown/not classified as a site: conservative miss
}
