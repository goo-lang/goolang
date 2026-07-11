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

// Task 6: the set of "lane-derived" names inside ONE lanes.Run body — the
// body's `*lanes.Lane` ctx parameter plus every name transitively bound
// from it (`t := ctx.Own()`, `u := t`, ...). A flat fixed-capacity array,
// same convention and cap as the moved-name table; borrowed AST strings, so
// nothing here is freed. Over-cap additions are silently dropped
// (under-detect, the safe direction for a rejection pass). Kept on the
// C stack per Run body (nesting handled via the outer pointer below), never
// heap-allocated.
#define LANE_DERIVED_MAX LANE_OWNERSHIP_MAX_MOVES_PER_FUNC

typedef struct {
    const char* names[LANE_DERIVED_MAX];
    size_t      count;
} LaneDerivedSet;

typedef struct {
    TypeChecker* checker;
    const char*  moved_names[LANE_OWNERSHIP_MAX_MOVES_PER_FUNC];
    Position     moved_pos[LANE_OWNERSHIP_MAX_MOVES_PER_FUNC];
    size_t       moved_count;
    LanePartitionBinding* bindings;  // Task 6 side table; see lane_ownership.h
    // Task 6 (obligation 2 bookkeeping): names of Partition bindings already
    // CONSUMED by a lanes.Run call — a second Run on the same binding is a
    // use-after-consume. Same flat-table design and cap as moved_names.
    const char*  consumed_names[LANE_OWNERSHIP_MAX_MOVES_PER_FUNC];
    Position     consumed_pos[LANE_OWNERSHIP_MAX_MOVES_PER_FUNC];
    size_t       consumed_count;
    // Task 6 (obligation 4, category iii): while walking a lanes.Run body,
    // the lane-derived set of the ENCLOSING Run body (NULL at top level), so
    // a nested Run's capture-rule check can reject a body that captures an
    // OUTER lane's context. Borrowed (points at a caller's C-stack
    // LaneDerivedSet); saved/restored around each nested body walk.
    // Shadow-UNAWARE like the rest of this pass (Option A): matching is by
    // bare name, so a nested-Run body capturing an inner variable that
    // shadows an outer lane-derived name CAN false-reject. Same envelope as
    // obligation 1's flat moved_names table — documented, not accidental.
    const LaneDerivedSet* outer_derived;
} LaneWalkContext;

static void lane_ctx_init(LaneWalkContext* ctx, TypeChecker* checker) {
    ctx->checker = checker;
    ctx->moved_count = 0;
    ctx->bindings = NULL;
    ctx->consumed_count = 0;
    ctx->outer_derived = NULL;
}

// Reset per-function state before/after walking one FuncDeclNode's body.
// Obligation 1 does not cross a function boundary (Option A: no
// interprocedural analysis), so both tables start fresh for every function.
static void lane_ctx_reset_for_function(LaneWalkContext* ctx) {
    ctx->moved_count = 0;
    ctx->consumed_count = 0;
    ctx->outer_derived = NULL;
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

// Reassignment (`x = e`, a bare-identifier LHS of a plain `=`) REVIVES a
// previously-moved name — Go semantics: `x` now names a brand-new value
// that was never passed to lanes.Partition, so a later read of `x` must not
// reject. Drops every entry matching `name` from the flat moved-name table
// via compaction (there is normally at most one, but this does not assume
// that): O(moved_count), fine at this table's small fixed cap
// (LANE_OWNERSHIP_MAX_MOVES_PER_FUNC).
static void lane_clear_moved(LaneWalkContext* ctx, const char* name) {
    if (!name) return;
    size_t write = 0;
    for (size_t read = 0; read < ctx->moved_count; read++) {
        if (strcmp(ctx->moved_names[read], name) == 0) continue;  // revived
        if (write != read) {
            ctx->moved_names[write] = ctx->moved_names[read];
            ctx->moved_pos[write] = ctx->moved_pos[read];
        }
        write++;
    }
    ctx->moved_count = write;
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

// Same package-identity detection as lane_call_is_partition, for `lanes.Run`
// — the goroutine-spawning consumer of a Partitioned. Recognized by resolved
// package identity, never by source identifier text (see the DETECTION
// MECHANISM note in lane_ownership.h).
static int lane_call_is_run(TypeChecker* checker, ASTNode* call_expr) {
    const char* selector = NULL;
    if (!lane_call_is_lanes_selector(checker, call_expr, &selector)) return 0;
    return selector && strcmp(selector, "Run") == 0;
}

// --- Task 6 lane-derived set (obligation 3) -------------------------------

static void lane_derived_init(LaneDerivedSet* set) {
    set->count = 0;
}

static int lane_derived_contains(const LaneDerivedSet* set, const char* name) {
    if (!name) return 0;
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->names[i], name) == 0) return 1;
    }
    return 0;
}

static void lane_derived_add(LaneDerivedSet* set, const char* name) {
    if (!name || lane_derived_contains(set, name)) return;
    if (set->count < LANE_DERIVED_MAX) {
        set->names[set->count++] = name;
    }
    // else: cap reached — under-detect (safe direction), never grow/crash.
}

// Returns the FIRST lane-derived identifier read anywhere in `expr`, or NULL
// if none. Same fail-closed node coverage as lane_check_reads: an AST kind
// not listed here is simply not recursed into (never a false positive). Used
// both to taint a new binding (`x := <expr mentioning a derived name>`) and
// to name the offending view in an obligation-3 diagnostic.
static const char* lane_derived_find_in_expr(const LaneDerivedSet* set, ASTNode* expr) {
    if (!expr) return NULL;

    switch (expr->type) {
        case AST_IDENTIFIER: {
            const char* n = ((IdentifierNode*)expr)->name;
            return lane_derived_contains(set, n) ? n : NULL;
        }
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            const char* l = lane_derived_find_in_expr(set, b->left);
            return l ? l : lane_derived_find_in_expr(set, b->right);
        }
        case AST_UNARY_EXPR:
            return lane_derived_find_in_expr(set, ((UnaryExprNode*)expr)->operand);
        case AST_POSTFIX_EXPR:
            return lane_derived_find_in_expr(set, ((PostfixExprNode*)expr)->operand);
        case AST_INDEX_EXPR: {
            IndexExprNode* idx = (IndexExprNode*)expr;
            const char* e = lane_derived_find_in_expr(set, idx->expr);
            return e ? e : lane_derived_find_in_expr(set, idx->index);
        }
        case AST_SLICE_INDEX_EXPR: {
            SliceIndexExprNode* s = (SliceIndexExprNode*)expr;
            const char* e = lane_derived_find_in_expr(set, s->expr);
            if (e) return e;
            e = lane_derived_find_in_expr(set, s->low);
            return e ? e : lane_derived_find_in_expr(set, s->high);
        }
        case AST_SLICE_EXPR: {
            SliceLitNode* lit = (SliceLitNode*)expr;
            for (ASTNode* e = lit->elements; e; e = e->next) {
                const char* f = lane_derived_find_in_expr(set, e);
                if (f) return f;
            }
            return NULL;
        }
        case AST_SELECTOR_EXPR:
            // `ctx.Own` / `ctx.field` — a selector whose base is lane-derived
            // makes the whole selector (and thus a call on it) lane-derived.
            return lane_derived_find_in_expr(set, ((SelectorExprNode*)expr)->expr);
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            const char* f = lane_derived_find_in_expr(set, call->function);
            if (f) return f;
            for (ASTNode* a = call->args; a; a = a->next) {
                const char* af = lane_derived_find_in_expr(set, a);
                if (af) return af;
            }
            return NULL;
        }
        default:
            return NULL;  // fail closed: unrecognized shape carries no taint
    }
}

static void lane_walk_stmt(LaneWalkContext* ctx, ASTNode* stmt);
static void lane_handle_run_boundary(LaneWalkContext* ctx, CallExprNode* call);

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
            // Task 6: a `lanes.Run(...)` call is the sole place obligations
            // 3/4 and the Run-consumption check fire. lane_handle_run_boundary
            // FULLY processes it (obligation 1 inside the body included), so
            // do NOT fall through to the generic arg walk below — that would
            // walk the body FuncLit a second time.
            if (lane_call_is_run(ctx->checker, expr)) {
                lane_handle_run_boundary(ctx, call);
                return;
            }
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

            // Move-site provenance: use the `lanes.Partition` SELECTOR's own
            // ->pos (call->function — already verified non-NULL by
            // lane_call_is_lanes_selector before this branch can be taken),
            // NOT the enclosing CallExprNode's ->pos (vd->values->pos). The
            // selector's pos is recorded at parse time right after the
            // `Partition` identifier token is consumed (selector_expr_new,
            // parser_actions.c), before the argument list/closing paren are
            // parsed. The call expression's own pos (call_expr_new) is
            // recorded only after the parser has looked ahead past the
            // statement's terminator to decide whether to reduce — for a
            // trailing call this lookahead routinely lands on the START of
            // the NEXT source line, which is what produced the reviewer-
            // observed "moved at 9:1" for a Partition call actually on line
            // 8. The selector's pos does not suffer this lag.
            Position move_pos = call->function->pos;
            lane_mark_moved(ctx, src->name, move_pos);

            // Record the partition binding for Task 6 (view attribution /
            // capture rule), if this Partition result is itself bound to a
            // name (`p := ...`) — always true for the shapes this grammar
            // produces (Partition's own call is never used as a bare
            // statement), but guarded defensively rather than assumed.
            // obligation 2: with comptime count and w = len(arr)/count, the
            // blessed Partition's views are the intervals [i*w, (i+1)*w) —
            // pairwise disjoint by constant-interval arithmetic (spec
            // Component 4, obligation 2). Recording (count, source) here IS
            // the obligation-2 bookkeeping; disjointness is structural.
            if (vd->name_count > 0 && vd->names && vd->names[0]) {
                LanePartitionBinding* binding = xmalloc(sizeof(LanePartitionBinding));
                if (binding) {  // fail closed: dropped entry, never crash
                    binding->binding_name = vd->names[0];
                    binding->source_name = src->name;
                    binding->partition_pos = move_pos;

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

// A `simple_stmt` lowers to AST_EXPR_STMT wrapping either an ordinary
// expression-statement (e.g. a bare call) or — per parser.y's
// `expression ASSIGN expression` rule (parser.y:1137-1146) and
// compound_assign_stmt (parser_actions.c) — a top-level BinaryExprNode whose
// ->operator is TOKEN_ASSIGN (`x = e`) or one of the TOKEN_*_ASSIGN compound
// tokens (`x += e`, ...). Assignment cannot occur anywhere else in this
// grammar: the `expression` nonterminal itself has no ASSIGN production (see
// the parser.y comment on why the rule lives in simple_stmt, not
// expression), so a BinaryExprNode reached from a nested read context
// (lane_check_reads's own AST_BINARY_EXPR case, e.g. `a + b`) can never be
// one of these — only this top-level entry point needs to special-case
// assignment; lane_check_reads' generic BINARY_EXPR handling (both sides are
// reads) stays correct and untouched for every other use.
//
// COMPOUND-ASSIGN AST FACT (established by reading parser.y:1137-1146 next
// to parser_actions.c's compound_assign_stmt): `x += e` is NOT desugared
// into ASSIGN plus a synthesized read of `x`. The compound operator's own
// distinct TokenType (TOKEN_PLUS_ASSIGN, TOKEN_MINUS_ASSIGN, ...) is kept on
// the BinaryExprNode as-is; codegen (not the parser) is what later lowers it
// to load-op-store. So this walker CAN and DOES tell `x = e` apart from
// `x += e` by checking ->operator == TOKEN_ASSIGN — the distinction is real
// and reachable, not dead code.
static void lane_handle_expr_stmt(LaneWalkContext* ctx, ASTNode* expr) {
    if (!expr || expr->type != AST_BINARY_EXPR) {
        lane_check_reads(ctx, expr);
        return;
    }

    BinaryExprNode* bin = (BinaryExprNode*)expr;

    if (bin->operator != TOKEN_ASSIGN) {
        // Compound assign (`x += e`, ...) reads AND writes its LHS, so this
        // is exactly the generic "both sides are reads" shape — which also
        // correctly covers an ordinary (non-assignment) binary expression
        // used as a bare statement (e.g. `a + b` alone on a line).
        lane_check_reads(ctx, bin->left);
        lane_check_reads(ctx, bin->right);
        return;
    }

    // Plain `=`. RHS is always an ordinary read site.
    lane_check_reads(ctx, bin->right);

    if (bin->left && bin->left->type == AST_IDENTIFIER) {
        // Bare-identifier LHS of `=` (`data = ...`): a pure write, never a
        // read (the false-reject this fix addresses) — and Go-shaped
        // reassignment REVIVES the name: `data` now names a value that was
        // never passed to lanes.Partition, so a later read of `data` must
        // not reject.
        lane_clear_moved(ctx, ((IdentifierNode*)bin->left)->name);
        return;
    }

    // Compound LHS shape (`s[i] = e`, `p.f = e`, `*p = e`, ...): the base
    // being indexed/selected/dereferenced is still a READ — indexing or
    // field-accessing a moved slice must still reject. Only a bare
    // identifier LHS is a pure write; walk every other LHS shape like any
    // other read site.
    lane_check_reads(ctx, bin->left);
}

// --- Task 6 obligations 3/4 at the lanes.Run boundary ---------------------

static int lane_name_is_partition_binding(const LaneWalkContext* ctx, const char* name) {
    if (!name) return 0;
    for (const LanePartitionBinding* b = ctx->bindings; b; b = b->next) {
        if (b->binding_name && strcmp(b->binding_name, name) == 0) return 1;
    }
    return 0;
}

// The name of a func literal's FIRST parameter — for a lanes.Run body this
// is the `*lanes.Lane` context (Run's frozen signature: `func(ctx *lanes.
// Lane)`). Seeds the lane-derived set. Per the task brief we key off the
// PARAMETER (whatever it is named), not a hardcoded `ctx`. A FuncLit's
// params are a next-chained VarDeclNode list (ast.h FuncLitNode comment).
static const char* lane_funclit_first_param_name(FuncLitNode* body) {
    if (!body || !body->params || body->params->type != AST_VAR_DECL) return NULL;
    VarDeclNode* p = (VarDeclNode*)body->params;
    if (p->name_count > 0 && p->names && p->names[0]) return p->names[0];
    return NULL;
}

// Obligation 4 (view-capture rule): a lane body may reach no partition state
// except its own *Lane context. Reject any captured name that is a Partition
// binding, a moved source array, or lane-derived from an ENCLOSING Run body
// (`outer_derived`, non-NULL only for a nested Run). captured_names is the
// checker-populated capture list — TRUSTED, not recomputed (ast.h FuncLit
// comment; param_escape.c's Sink #3 precedent).
static void lane_check_capture_rule(LaneWalkContext* ctx, FuncLitNode* body,
                                    const LaneDerivedSet* outer_derived) {
    for (size_t i = 0; i < body->captured_count; i++) {
        const char* name = body->captured_names[i];
        if (!name) continue;
        int violates = lane_name_is_partition_binding(ctx, name) ||
                       lane_name_is_moved(ctx, name, NULL) ||
                       (outer_derived && lane_derived_contains(outer_derived, name));
        if (violates) {
            type_error(ctx->checker, body->base.pos,
                "lane body may not capture '%s' — a lane body may reach no "
                "partition state except its own *Lane context", name);
        }
    }
}

// Obligation 3 (single-goroutine attribution): recursively walk a lanes.Run
// body tracking the lane-derived set (seeded with the *Lane ctx param,
// extended transitively at each `x := <expr mentioning a derived name>`).
// At every `go` inside the body, reject if any call argument OR any captured
// name of a launched literal is lane-derived — the view would then be shared
// across two goroutines. Mirrors Sink #4's two surfaces (args + callee
// captures) as a checker rejection.
//
// Does NOT descend into nested FuncLit/closure bodies: a nested closure has
// its own scope, and its captures (not bare-identifier reads of THIS body's
// derived names) are the boundary that matters — handled either at a `go`
// launching it (surface 2 here) or, for a nested lanes.Run, by that Run's
// own boundary handler (reached via the obligation-1 walk). Nested lanes.Run
// bodies are likewise not re-processed here.
static void lane_body_walk_stmt(LaneWalkContext* ctx, ASTNode* stmt,
                                LaneDerivedSet* derived) {
    if (!stmt) return;

    switch (stmt->type) {
        case AST_BLOCK_STMT: {
            BlockStmtNode* b = (BlockStmtNode*)stmt;
            for (ASTNode* s = b->statements; s; s = s->next) {
                lane_body_walk_stmt(ctx, s, derived);
            }
            return;
        }
        case AST_VAR_DECL: {
            // Transitive taint: `t := ctx.Own()`, `u := t` — any binding
            // whose initializer reads a lane-derived name is itself
            // lane-derived. Single forward pass suffices (Go declares before
            // use), so arbitrary copy depth is tracked.
            VarDeclNode* vd = (VarDeclNode*)stmt;
            if (vd->values && lane_derived_find_in_expr(derived, vd->values)) {
                for (size_t i = 0; i < vd->name_count; i++) {
                    if (vd->names && vd->names[i]) lane_derived_add(derived, vd->names[i]);
                }
            }
            return;
        }
        case AST_GO_STMT: {
            GoStmtNode* gs = (GoStmtNode*)stmt;
            if (!gs->call || gs->call->type != AST_CALL_EXPR) return;
            CallExprNode* c = (CallExprNode*)gs->call;

            // Known under-detects (spec-conformant: the brief scopes
            // obligation 3 to call args + launched-literal captures):
            // `go t.Method()` — the view as RECEIVER (c->function) is not
            // walked; and closure indirection (`f := func(){ go leak(t) };
            // go f()`) — FuncLit values are never lane-tainted, so `f`
            // launching later escapes detection. Both under-reject only.

            // Surface 1: call arguments.
            for (ASTNode* a = c->args; a; a = a->next) {
                const char* view = lane_derived_find_in_expr(derived, a);
                if (view) {
                    type_error(ctx->checker, stmt->pos,
                        "lane view '%s' (derived from the *Lane context) may "
                        "not be passed to another goroutine", view);
                }
            }
            // Surface 2: a launched literal's captured names.
            if (c->function && c->function->type == AST_FUNC_LIT) {
                FuncLitNode* lit = (FuncLitNode*)c->function;
                for (size_t i = 0; i < lit->captured_count; i++) {
                    const char* name = lit->captured_names[i];
                    if (lane_derived_contains(derived, name)) {
                        type_error(ctx->checker, stmt->pos,
                            "lane view '%s' (derived from the *Lane context) "
                            "may not be passed to another goroutine", name);
                    }
                }
            }
            return;
        }
        case AST_IF_STMT: {
            IfStmtNode* i = (IfStmtNode*)stmt;
            lane_body_walk_stmt(ctx, i->then_stmt, derived);
            lane_body_walk_stmt(ctx, i->else_stmt, derived);
            return;
        }
        case AST_FOR_STMT: {
            ForStmtNode* f = (ForStmtNode*)stmt;
            lane_body_walk_stmt(ctx, f->init, derived);
            lane_body_walk_stmt(ctx, f->post, derived);
            lane_body_walk_stmt(ctx, f->body, derived);
            return;
        }
        case AST_SWITCH_STMT: {
            SwitchStmtNode* sw = (SwitchStmtNode*)stmt;
            for (ASTNode* c = sw->cases; c; c = c->next) {
                lane_body_walk_stmt(ctx, c, derived);
            }
            return;
        }
        case AST_CASE_CLAUSE: {
            CaseClauseNode* cc = (CaseClauseNode*)stmt;
            for (ASTNode* s = cc->body; s; s = s->next) {
                lane_body_walk_stmt(ctx, s, derived);
            }
            return;
        }
        case AST_LABEL_STMT:
            lane_body_walk_stmt(ctx, ((LabelStmtNode*)stmt)->stmt, derived);
            return;
        default:
            return;  // fail closed: no taint tracked, no reject
    }
}

// Process one `lanes.Run(p, steps, func(ctx *lanes.Lane){ body })` call:
// obligation 2 bookkeeping (Run consumes `p`), obligation 4 (view-capture
// rule), obligation 1 inside the body (moved-name reads), and obligation 3
// (single-goroutine attribution). Fully owns the body — the caller must NOT
// also walk the body FuncLit.
static void lane_handle_run_boundary(LaneWalkContext* ctx, CallExprNode* call) {
    ASTNode* a0 = call->args;                  // p — the Partitioned binding
    ASTNode* a1 = a0 ? a0->next : NULL;        // steps/count
    ASTNode* a2 = a1 ? a1->next : NULL;        // body func literal

    // Non-body args are ordinary read sites (a moved name read in `steps`
    // still rejects). a0 is normally the partition binding identifier — not
    // a moved name — so read-checking it is harmless.
    if (a0) lane_check_reads(ctx, a0);
    if (a1) lane_check_reads(ctx, a1);

    // Obligation 2 bookkeeping: Run CONSUMES its Partitioned argument (same
    // move-machinery spirit as obligation 1). A second Run on the SAME
    // binding is use-after-consume; a Run on a DIFFERENT binding is legal
    // (the table keys on the identifier — see lanes_repartition_probe).
    if (a0 && a0->type == AST_IDENTIFIER) {
        const char* pname = ((IdentifierNode*)a0)->name;
        int already = 0;
        for (size_t i = 0; i < ctx->consumed_count; i++) {
            if (strcmp(ctx->consumed_names[i], pname) == 0) {
                Position at = ctx->consumed_pos[i];
                type_error(ctx->checker, a0->pos,
                    "use of '%s' after it was consumed by lanes.Run "
                    "(consumed at %s:%d:%d)", pname,
                    at.filename ? at.filename : "<unknown>", at.line, at.column);
                already = 1;
                break;
            }
        }
        if (!already && ctx->consumed_count < LANE_OWNERSHIP_MAX_MOVES_PER_FUNC) {
            ctx->consumed_names[ctx->consumed_count] = pname;
            ctx->consumed_pos[ctx->consumed_count] = a0->pos;
            ctx->consumed_count++;
        }
    }

    if (!a2 || a2->type != AST_FUNC_LIT) return;  // fail closed on odd shapes
    FuncLitNode* body = (FuncLitNode*)a2;

    // Obligation 4: view-capture rule (captures vs partition bindings, moved
    // sources, and — for a nested Run — the enclosing lane-derived set).
    lane_check_capture_rule(ctx, body, ctx->outer_derived);

    // Obligation 3: build the lane-derived set (seeded with the *Lane ctx
    // param) and reject go-escapes. Built BEFORE the obligation-1 walk so
    // that walk can publish it as `outer_derived` for any nested Run.
    LaneDerivedSet derived;
    lane_derived_init(&derived);
    const char* ctx_param = lane_funclit_first_param_name(body);
    if (ctx_param) lane_derived_add(&derived, ctx_param);
    lane_body_walk_stmt(ctx, body->body, &derived);

    // Obligation 1 inside the body: moved-name reads. This walk is also the
    // ONE place a NESTED lanes.Run is detected/processed (via lane_check_
    // reads); publish this body's derived set so the nested Run's capture
    // rule can enforce category (iii).
    const LaneDerivedSet* saved = ctx->outer_derived;
    ctx->outer_derived = &derived;
    lane_walk_stmt(ctx, body->body);
    ctx->outer_derived = saved;
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
            lane_handle_expr_stmt(ctx, ((ExprStmtNode*)stmt)->expr);
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
        case AST_LABEL_STMT:
            // `L: stmt` wraps an arbitrary ordinary Go statement (Task 6
            // carry-forward from Task 5's review — previously under-walked).
            // Descend into the wrapped statement so a moved-name read or a
            // lanes.Run under a label is still seen by this pass.
            lane_walk_stmt(ctx, ((LabelStmtNode*)stmt)->stmt);
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
