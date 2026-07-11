// P6 M1 Task 5 — see include/lane_ownership.h for the full design/scope
// comment (Option A, detection mechanism, import-alias subtlety, and the
// lifetime caveat this file's move-tracking design rests on). Summary: a
// self-contained, per-function AST walk that (1) recognizes
// `x := lanes.Partition(arr, count)` / `var x = lanes.Partition(arr, count)`
// as a move of `arr`, and (2) flags any later read of `arr` in that same
// function body as ERROR_USE_AFTER_MOVE.

#include "lane_ownership.h"
#include "ownership_checker.h"
#include <stdlib.h>
#include <string.h>

// Per-function walk state. Task 6 extends this struct (adding go/
// lanes.Run-boundary fields for obligations 3/4) rather than introducing a
// second walker — see lane_ownership.h's header comment.
//
// moved_names/moved_pos are a flat, per-function table of source-slice
// names moved by lanes.Partition (obligation 1). Deliberately NOT scope- or
// shadow-aware (Option A: a nested block that re-declares the same name
// would still be treated as "still moved" by this pass) — see the header
// comment's SCOPE note. Fixed-capacity, matching this codebase's existing
// convention for small per-function tables (e.g. TcFunctionContext's
// active_type_params); silently stops tracking additional moves past the
// cap rather than growing unboundedly, which under-detects (safe
// direction: a missed reject, never a false one) rather than crashing.
#define LANE_OWNERSHIP_MAX_MOVES_PER_FUNC 64

typedef struct {
    TypeChecker* checker;
    const char*  moved_names[LANE_OWNERSHIP_MAX_MOVES_PER_FUNC];
    Position     moved_pos[LANE_OWNERSHIP_MAX_MOVES_PER_FUNC];
    size_t       moved_count;
    LanePartitionBinding* bindings;  // Task 6 side table; see lane_ownership.h
} LaneWalkContext;

static void lane_ctx_init(LaneWalkContext* ctx, TypeChecker* checker) {
    ctx->checker = checker;
    ctx->moved_count = 0;
    ctx->bindings = NULL;
}

// Reset per-function state before/after walking one FuncDeclNode's body.
// Obligation 1 does not cross a function boundary (Option A: no
// interprocedural analysis), so both tables start fresh for every function.
static void lane_ctx_reset_for_function(LaneWalkContext* ctx) {
    ctx->moved_count = 0;
    LanePartitionBinding* b = ctx->bindings;
    while (b) {
        LanePartitionBinding* next = b->next;
        free(b);
        b = next;
    }
    ctx->bindings = NULL;
}

static void lane_mark_moved(LaneWalkContext* ctx, const char* name, Position pos) {
    if (!name) return;

    if (ctx->moved_count < LANE_OWNERSHIP_MAX_MOVES_PER_FUNC) {
        ctx->moved_names[ctx->moved_count] = name;
        ctx->moved_pos[ctx->moved_count] = pos;
        ctx->moved_count++;
    }
    // else: cap reached — see the LANE_OWNERSHIP_MAX_MOVES_PER_FUNC comment
    // above; silently stop tracking further moves in this function rather
    // than growing unboundedly.

    // Best-effort mirror onto the shared ownership_checker.c bone (see
    // lane_ownership.h's LIFETIME CAVEAT) — a harmless no-op for the common
    // case (a function-local `name`, whose Scope this pass's own timing has
    // already popped and freed), and a genuine mark for the rare
    // package-level-global case. NOT this pass's source of truth — see
    // lane_name_is_moved below, which is.
    mark_variable_moved(ctx->checker, name, pos);
}

static int lane_name_is_moved(const LaneWalkContext* ctx, const char* name, Position* out_pos) {
    if (!name) return 0;
    for (size_t i = 0; i < ctx->moved_count; i++) {
        if (strcmp(ctx->moved_names[i], name) == 0) {
            if (out_pos) *out_pos = ctx->moved_pos[i];
            return 1;
        }
    }
    return 0;
}

// Detection contract (spike-verified, see lane_ownership.h's DETECTION
// MECHANISM note): `call_expr` is `<pkg-ident>.<selector>(...)` where
// <pkg-ident> resolves through the checker's (persistent, never-popped-by-
// this-point) top-level scope to a TYPE_PACKAGE marker whose
// Package.import_path is "lanes". Returns the selector name on match so
// callers can further compare it against "Partition"/"Run"/etc.
static int lane_call_is_lanes_selector(TypeChecker* checker, ASTNode* call_expr,
                                        const char** out_selector) {
    if (!call_expr || call_expr->type != AST_CALL_EXPR) return 0;
    CallExprNode* call = (CallExprNode*)call_expr;
    if (!call->function || call->function->type != AST_SELECTOR_EXPR) return 0;

    SelectorExprNode* sel = (SelectorExprNode*)call->function;
    if (!sel->expr || sel->expr->type != AST_IDENTIFIER) return 0;
    IdentifierNode* pkg_ident = (IdentifierNode*)sel->expr;

    Variable* pkg_marker = type_checker_lookup_variable(checker, pkg_ident->name);
    if (!pkg_marker || !pkg_marker->package) return 0;
    if (!pkg_marker->package->import_path ||
        strcmp(pkg_marker->package->import_path, "lanes") != 0) {
        return 0;
    }

    if (out_selector) *out_selector = sel->selector;
    return 1;
}

static int lane_call_is_partition(TypeChecker* checker, ASTNode* call_expr) {
    const char* selector = NULL;
    if (!lane_call_is_lanes_selector(checker, call_expr, &selector)) return 0;
    return selector && strcmp(selector, "Partition") == 0;
}

static void lane_walk_stmt(LaneWalkContext* ctx, ASTNode* stmt);

// Generic recursive expression walker: flags a read of any name already in
// ctx's moved table. Fail-closed in the direction that matters for a
// rejection pass (see lane_ownership.h's SCOPE note) — an AST_* kind not
// listed here simply is not recursed into; it is never flagged, never
// crashes.
static void lane_check_reads(LaneWalkContext* ctx, ASTNode* expr) {
    if (!expr) return;

    switch (expr->type) {
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)expr;
            Position moved_at;
            if (lane_name_is_moved(ctx, ident->name, &moved_at)) {
                type_error(ctx->checker, expr->pos,
                    "use of '%s' after it was moved into lanes.Partition (moved at %s:%d:%d)",
                    ident->name,
                    moved_at.filename ? moved_at.filename : "<unknown>",
                    moved_at.line, moved_at.column);
            }
            return;
        }
        case AST_LITERAL:
            return;
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            lane_check_reads(ctx, b->left);
            lane_check_reads(ctx, b->right);
            return;
        }
        case AST_UNARY_EXPR:
            lane_check_reads(ctx, ((UnaryExprNode*)expr)->operand);
            return;
        case AST_POSTFIX_EXPR:
            lane_check_reads(ctx, ((PostfixExprNode*)expr)->operand);
            return;
        case AST_INDEX_EXPR: {
            IndexExprNode* idx = (IndexExprNode*)expr;
            lane_check_reads(ctx, idx->expr);
            lane_check_reads(ctx, idx->index);
            return;
        }
        case AST_SLICE_INDEX_EXPR: {
            // `expr[low:high]` substring/slice expression (NOT the same tag
            // as a slice composite literal — see the AST_SLICE_EXPR case
            // below; confusing the two structs here was caught by valgrind
            // as an out-of-bounds read during this task's self-review).
            SliceIndexExprNode* s = (SliceIndexExprNode*)expr;
            lane_check_reads(ctx, s->expr);
            lane_check_reads(ctx, s->low);
            lane_check_reads(ctx, s->high);
            return;
        }
        case AST_SLICE_EXPR: {
            // Slice composite literal `[]T{e, ...}` (SliceLitNode) — despite
            // the enum name, this tag is NOT the substring/slice-index node
            // (that is AST_SLICE_INDEX_EXPR, handled above); see ast.h's
            // comment on SliceLitNode for why the name is reused. Walk each
            // element expression as an ordinary read site.
            SliceLitNode* lit = (SliceLitNode*)expr;
            for (ASTNode* e = lit->elements; e; e = e->next) lane_check_reads(ctx, e);
            return;
        }
        case AST_SELECTOR_EXPR:
            lane_check_reads(ctx, ((SelectorExprNode*)expr)->expr);
            return;
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            lane_check_reads(ctx, call->function);
            for (ASTNode* a = call->args; a; a = a->next) lane_check_reads(ctx, a);
            return;
        }
        case AST_FUNC_LIT:
            // Recurse into a nested closure's body: a moved name read
            // inside a closure defined later in the same function is still
            // a use-after-move (self-review: nested-block/closure reads
            // must reject too).
            lane_walk_stmt(ctx, ((FuncLitNode*)expr)->body);
            return;
        default:
            return;  // fail closed: unrecognized shape -> not flagged
    }
}

// A VarDeclNode covers BOTH `x := lanes.Partition(...)` (is_short_decl=1)
// and `var x = lanes.Partition(...)` (is_short_decl=0) — the parser lowers
// both to the same node shape (see parser_actions.c's short_var_decl_new_1
// / var_decl_new_1), so no is_short_decl branch is needed here: recognizing
// the RHS call shape handles both forms identically (self-review: BOTH
// `:=` and `var` forms reject).
static void lane_handle_var_decl(LaneWalkContext* ctx, VarDeclNode* vd) {
    if (!vd->values) return;

    if (vd->values->type == AST_CALL_EXPR && lane_call_is_partition(ctx->checker, vd->values)) {
        CallExprNode* call = (CallExprNode*)vd->values;
        ASTNode* arr_arg = call->args;                    // arr — the moved source
        ASTNode* count_arg = arr_arg ? arr_arg->next : NULL;  // comptime count

        if (arr_arg && arr_arg->type == AST_IDENTIFIER) {
            IdentifierNode* src = (IdentifierNode*)arr_arg;
            lane_mark_moved(ctx, src->name, vd->values->pos);

            // Record the partition binding for Task 6 (view attribution /
            // capture rule), if this Partition result is itself bound to a
            // name (`p := ...`) — always true for the shapes this grammar
            // produces (Partition's own call is never used as a bare
            // statement), but guarded defensively rather than assumed.
            if (vd->name_count > 0 && vd->names && vd->names[0]) {
                LanePartitionBinding* binding = malloc(sizeof(LanePartitionBinding));
                if (binding) {  // fail closed: dropped entry, never crash
                    binding->binding_name = vd->names[0];
                    binding->source_name = src->name;
                    binding->partition_pos = vd->values->pos;

                    uint64_t count_val = 0;
                    binding->lane_count_known = count_arg &&
                        goo_fold_const_int_ctx(ctx->checker, count_arg, &count_val);
                    binding->lane_count = binding->lane_count_known ? count_val : 0;

                    binding->next = ctx->bindings;
                    ctx->bindings = binding;
                }
            }
        }

        // Any OTHER argument to this same call (e.g. `count`, if it is
        // itself a non-literal expression) is still an ordinary read site —
        // check every argument after arr_arg normally. arr_arg itself is
        // deliberately NOT read-checked here: at the moment of the move it
        // has not been moved yet (this call site is simultaneous with the
        // move, not a use after it).
        for (ASTNode* a = count_arg; a; a = a->next) lane_check_reads(ctx, a);
        return;
    }

    // Ordinary var-decl: its initializer is an ordinary read site.
    lane_check_reads(ctx, vd->values);
}

// Generic recursive statement walker. Same fail-closed contract as
// lane_check_reads: an unrecognized statement kind is simply not
// recursed into.
static void lane_walk_stmt(LaneWalkContext* ctx, ASTNode* stmt) {
    if (!stmt) return;

    switch (stmt->type) {
        case AST_BLOCK_STMT: {
            BlockStmtNode* b = (BlockStmtNode*)stmt;
            for (ASTNode* s = b->statements; s; s = s->next) lane_walk_stmt(ctx, s);
            return;
        }
        case AST_EXPR_STMT:
            lane_check_reads(ctx, ((ExprStmtNode*)stmt)->expr);
            return;
        case AST_VAR_DECL:
            lane_handle_var_decl(ctx, (VarDeclNode*)stmt);
            return;
        case AST_IF_STMT: {
            IfStmtNode* i = (IfStmtNode*)stmt;
            lane_check_reads(ctx, i->condition);
            lane_walk_stmt(ctx, i->then_stmt);
            lane_walk_stmt(ctx, i->else_stmt);
            return;
        }
        case AST_FOR_STMT: {
            ForStmtNode* f = (ForStmtNode*)stmt;
            lane_walk_stmt(ctx, f->init);
            lane_check_reads(ctx, f->condition);
            lane_walk_stmt(ctx, f->post);
            lane_check_reads(ctx, f->range_expr);
            lane_walk_stmt(ctx, f->body);
            return;
        }
        case AST_RETURN_STMT:
            lane_check_reads(ctx, ((ReturnStmtNode*)stmt)->values);
            return;
        case AST_DEFER_STMT:
            lane_check_reads(ctx, ((DeferStmtNode*)stmt)->call);
            return;
        case AST_GO_STMT:
            lane_check_reads(ctx, ((GoStmtNode*)stmt)->call);
            return;
        case AST_SWITCH_STMT: {
            SwitchStmtNode* sw = (SwitchStmtNode*)stmt;
            lane_check_reads(ctx, sw->tag);
            for (ASTNode* c = sw->cases; c; c = c->next) lane_walk_stmt(ctx, c);
            return;
        }
        case AST_CASE_CLAUSE: {
            CaseClauseNode* cc = (CaseClauseNode*)stmt;
            for (ASTNode* e = cc->exprs; e; e = e->next) lane_check_reads(ctx, e);
            for (ASTNode* s = cc->body; s; s = s->next) lane_walk_stmt(ctx, s);
            return;
        }
        case AST_MULTI_ASSIGN:
            // Only `values` (the RHS) are read sites. `targets` are
            // deliberately skipped: for the `:=` form they are brand-new
            // declarations, not reads, and flagging one because its name
            // happens to match an earlier moved name (shadowing) would be
            // a false reject on legal Go-shaped code — out of scope for
            // this narrow pass (Option A).
            for (ASTNode* v = ((MultiAssignNode*)stmt)->values; v; v = v->next) {
                lane_check_reads(ctx, v);
            }
            return;
        default:
            return;  // fail closed: not a shape this pass reasons about
    }
}

int lane_ownership_check_program(TypeChecker* checker, ASTNode* program) {
    if (!checker || !program || program->type != AST_PROGRAM) return 0;

    int errors_before = checker->error_count;
    ProgramNode* prog = (ProgramNode*)program;

    LaneWalkContext ctx;
    lane_ctx_init(&ctx, checker);

    for (ASTNode* decl = prog->decls; decl; decl = decl->next) {
        if (decl->type != AST_FUNC_DECL) continue;
        FuncDeclNode* func = (FuncDeclNode*)decl;

        lane_ctx_reset_for_function(&ctx);  // Option A: no cross-function analysis
        lane_walk_stmt(&ctx, func->body);
    }

    lane_ctx_reset_for_function(&ctx);  // free the last function's bindings list

    return checker->error_count == errors_before;
}
