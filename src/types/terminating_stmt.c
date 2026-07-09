// P2.4 (missing-return analysis): the terminating-statement predicate.
// Binding spec: docs/superpowers/specs/2026-07-09-p2-typesystem-b-design.md,
// section T2. This is a pure, stateless AST walk (no TypeChecker/TcFunctionContext
// involvement) — termination is a structural property of the statement tree
// alone, unlike label/goto validity which needs function-wide bookkeeping.
//
// Base rule set is Go's own (https://go.dev/ref/spec#Terminating_statements),
// extended for Goo-specific node shapes:
//   - AST_IF_LET_STMT is a distinct node from AST_IF_STMT (if-let unwrap),
//     given the identical both-branches-terminate treatment as if/else.
//   - AST_TYPE_SWITCH is a distinct node from AST_SWITCH_STMT (type-assert
//     dispatch vs. value dispatch); type switches never have `fallthrough`
//     (type_check_switch_like_body already rejects it), so no delegation
//     chain is needed there, unlike the expression-switch case.
//   - AST_ARENA_BLOCK is transparent: its body is a `block` production
//     (BlockStmtNode) exactly like a plain `{...}`, so it delegates to the
//     same last-statement-terminates rule with no special casing needed
//     beyond recursing into ->body.
//
// Go-spec fidelity beyond the design doc's terse bullet list: the doc's
// summary omits "and there are no break statements referring to it" for
// switch/type-switch (it states this explicitly only for `for` and, via
// the select bullet, for `select`) — but the real Go spec applies that
// clause to for/switch/select uniformly. Omitting it for switch/type-switch
// would be UNSOUND: a clause whose FINAL statement terminates but which
// also contains an earlier `break` that exits the switch on some path
// would be wrongly classified as terminating (see break_targets' doc
// comment below for the concrete counterexample). This file applies the
// break-freedom check to all four break-scope constructs (for, switch,
// type-switch, select) for that reason.
#include "types.h"
#include <string.h>

// Forward declarations — stmt_terminates and stmt_terminates_labeled are
// mutually recursive with block_terminates (a block's termination depends
// on its last statement's termination, which may itself be a block/switch/
// etc.), and break_targets/break_targets_list are mutually recursive with
// each other (a statement's substructure is itself a mix of single
// statements and statement lists).
static int stmt_terminates(ASTNode* stmt);
static int stmt_terminates_labeled(ASTNode* stmt, const char** labels, size_t nlabels);
static int block_terminates(ASTNode* first_stmt);
static int break_targets(ASTNode* stmt, const char** labels, size_t nlabels, int crossed);
static int break_targets_list(ASTNode* first, const char** labels, size_t nlabels, int crossed);

// Last element of a `->next`-chained statement list, or NULL for an empty
// list. Every statement-list holder in this AST (block bodies, case clause
// bodies) uses this exact chaining convention (see BlockStmtNode/
// CaseClauseNode/TypeCaseNode/SelectCaseNode doc comments, ast.h).
static ASTNode* last_stmt(ASTNode* first) {
    if (!first) return NULL;
    ASTNode* s = first;
    while (s->next) s = s->next;
    return s;
}

// `panic(...)` as a statement — AST_EXPR_STMT wrapping an AST_CALL_EXPR
// whose callee is the bare identifier "panic". Mirrors call_codegen.c's own
// `strcmp(func_name->name, "panic") == 0` builtin-detection convention
// (no attempt to rule out a locally-shadowed `panic` — same latitude every
// other builtin-name check in this codebase takes).
static int is_panic_call_stmt(ASTNode* stmt) {
    ExprStmtNode* es = (ExprStmtNode*)stmt;
    ASTNode* e = es->expr;
    if (!e || e->type != AST_CALL_EXPR) return 0;
    CallExprNode* call = (CallExprNode*)e;
    if (!call->function || call->function->type != AST_IDENTIFIER) return 0;
    IdentifierNode* id = (IdentifierNode*)call->function;
    return id->name && strcmp(id->name, "panic") == 0;
}

// A switch/type-switch case clause's LITERAL final statement (chasing any
// wrapping labels — `L: fallthrough` is legal, mirroring
// type_check_switch_like_body's Fix 5b unwrap in type_checker.c) is
// `fallthrough`. Type switches never reach this (fallthrough is rejected
// there before this analysis ever runs); only used by clause_chain_terminates
// below, which is expression-switch-only.
static int clause_ends_in_fallthrough(ASTNode* body) {
    ASTNode* last = last_stmt(body);
    while (last && last->type == AST_LABEL_STMT) last = ((LabelStmtNode*)last)->stmt;
    return last != NULL && last->type == AST_FALLTHROUGH_STMT;
}

// Expression-switch clause termination, delegating through a trailing
// fallthrough chain: a clause ending in (possibly labeled) `fallthrough` is
// terminating iff the clause it falls into is terminating (Go spec: "...or
// a possibly labeled 'fallthrough' statement"). `type_check_switch_like_body`
// has already proven fallthrough is well-formed here (not the switch's last
// clause, not followed by more statements) — this assumes that invariant
// rather than re-validating it.
static int clause_chain_terminates(ASTNode* clause_node) {
    CaseClauseNode* cc = (CaseClauseNode*)clause_node;
    if (clause_ends_in_fallthrough(cc->body)) {
        return clause_node->next ? clause_chain_terminates(clause_node->next) : 0;
    }
    return block_terminates(cc->body);
}

// Does `cases` (expression switch: CaseClauseNode list, `exprs == NULL`
// marks default) terminate? Requires a default clause, no break targeting
// this switch (bare, or labeled matching one of `labels` — this switch's
// own enclosing label chain, if any), and every clause terminating (via
// clause_chain_terminates's fallthrough delegation).
static int switch_terminates(ASTNode* cases, const char** labels, size_t nlabels) {
    int has_default = 0;
    for (ASTNode* c = cases; c; c = c->next) {
        CaseClauseNode* cc = (CaseClauseNode*)c;
        if (cc->exprs == NULL) has_default = 1;
        if (break_targets_list(cc->body, labels, nlabels, 0)) return 0;
    }
    if (!has_default) return 0;
    for (ASTNode* c = cases; c; c = c->next) {
        if (!clause_chain_terminates(c)) return 0;
    }
    return 1;
}

// Type-switch sibling of switch_terminates. TypeCaseNode's `types == NULL`
// marks default (TypeSwitchNode/TypeCaseNode doc comments, ast.h). No
// fallthrough delegation — type switches cannot contain `fallthrough`.
static int type_switch_terminates(ASTNode* cases, const char** labels, size_t nlabels) {
    int has_default = 0;
    for (ASTNode* c = cases; c; c = c->next) {
        TypeCaseNode* tc = (TypeCaseNode*)c;
        if (tc->types == NULL) has_default = 1;
        if (break_targets_list(tc->body, labels, nlabels, 0)) return 0;
    }
    if (!has_default) return 0;
    for (ASTNode* c = cases; c; c = c->next) {
        if (!block_terminates(((TypeCaseNode*)c)->body)) return 0;
    }
    return 1;
}

// select statement, Go rule (design doc T2's select bullet): no default
// requirement (unlike switch — a select with no default always executes
// exactly one ready case, so "every case terminates" alone is enough for
// the whole statement to terminate); `select{}` (zero cases) blocks forever
// and is vacuously terminating (the loop below never runs, has_break stays
// false). `comm == NULL` marks the default case (SelectCaseNode doc
// comment, ast.h) — included in the "every case" walk with no special
// casing, matching the Go spec's "including the default if present".
static int select_terminates(ASTNode* cases, const char** labels, size_t nlabels) {
    for (ASTNode* c = cases; c; c = c->next) {
        SelectCaseNode* sc = (SelectCaseNode*)c;
        if (break_targets_list(sc->body, labels, nlabels, 0)) return 0;
        if (!block_terminates(sc->body)) return 0;
    }
    return 1;
}

// Walks `stmt`'s OWN sub-statements (not `stmt` itself) for a `break`
// (bare, or labeled matching one of `labels`) that refers to the for/
// switch/type-switch/select construct `labels`/`nlabels` was captured for.
// `crossed` is 1 once the walk has descended into the body of a NESTED
// break-scope (another for/switch/type-switch/select) — inside such a
// body, a BARE `break` refers to that inner construct, not the outer one
// this walk started from, so bare matches stop counting; a LABELED break
// can still explicitly target the outer construct from arbitrarily deep
// nesting (Go: labeled break matches ANY enclosing labeled statement, not
// just the innermost), so labeled matches are checked unconditionally.
//
// Why this check exists at all (not redundant with block_terminates' own
// last-statement rule): a clause whose FINAL statement terminates can still
// have an EARLIER statement that breaks out on some path —
//   default:
//       if y { break }
//       return 1
// — block_terminates only inspects the last statement (`return 1`, which
// terminates) and would wrongly call this clause terminating; the y-true
// path never reaches the return at all. Go's spec closes this with the
// explicit "no break refers to it" clause on for/switch/select; this walk
// implements that clause.
//
// Never descends into a nested AST_FUNC_LIT body — labels and break/
// continue targets are per-function in Go, so a break inside a closure
// lexically written inside this construct can never refer to it. Func
// literals only ever appear inside EXPRESSION subtrees (call args, var-decl
// initializers, ...), which this walk — statement-shaped only — never
// descends into in the first place, so no explicit guard is needed for it.
static int break_targets(ASTNode* stmt, const char** labels, size_t nlabels, int crossed) {
    if (!stmt) return 0;
    switch (stmt->type) {
        case AST_BREAK_STMT:
            return !crossed;
        case AST_BREAK_LABEL_STMT: {
            BreakLabelStmtNode* bl = (BreakLabelStmtNode*)stmt;
            for (size_t i = 0; i < nlabels; i++) {
                if (bl->label && labels[i] && strcmp(bl->label, labels[i]) == 0) return 1;
            }
            return 0;
        }
        case AST_BLOCK_STMT:
            return break_targets_list(((BlockStmtNode*)stmt)->statements, labels, nlabels, crossed);
        case AST_IF_STMT: {
            IfStmtNode* i = (IfStmtNode*)stmt;
            return break_targets(i->then_stmt, labels, nlabels, crossed) ||
                   break_targets(i->else_stmt, labels, nlabels, crossed);
        }
        case AST_IF_LET_STMT: {
            IfLetStmtNode* il = (IfLetStmtNode*)stmt;
            return break_targets(il->then_stmt, labels, nlabels, crossed) ||
                   break_targets(il->else_stmt, labels, nlabels, crossed);
        }
        case AST_FOR_STMT:
            // A nested for loop is itself a break scope: bare breaks inside
            // its body target IT, not the construct we're searching for.
            return break_targets(((ForStmtNode*)stmt)->body, labels, nlabels, 1);
        case AST_SWITCH_STMT: {
            SwitchStmtNode* sw = (SwitchStmtNode*)stmt;
            for (ASTNode* c = sw->cases; c; c = c->next) {
                if (break_targets_list(((CaseClauseNode*)c)->body, labels, nlabels, 1)) return 1;
            }
            return 0;
        }
        case AST_TYPE_SWITCH: {
            TypeSwitchNode* ts = (TypeSwitchNode*)stmt;
            for (ASTNode* c = ts->cases; c; c = c->next) {
                if (break_targets_list(((TypeCaseNode*)c)->body, labels, nlabels, 1)) return 1;
            }
            return 0;
        }
        case AST_SELECT_STMT: {
            SelectStmtNode* se = (SelectStmtNode*)stmt;
            for (ASTNode* c = se->cases; c; c = c->next) {
                if (break_targets_list(((SelectCaseNode*)c)->body, labels, nlabels, 1)) return 1;
            }
            return 0;
        }
        case AST_ARENA_BLOCK:
            // Transparent wrapper (not a break scope of its own) — a bare
            // break inside `arena { ... break ... }` still targets whatever
            // for/switch/select lexically encloses the arena block.
            return break_targets(((ArenaBlockNode*)stmt)->body, labels, nlabels, crossed);
        case AST_UNSAFE_STMT:
            return break_targets(((UnsafeStmtNode*)stmt)->body, labels, nlabels, crossed);
        case AST_COMPTIME_BLOCK:
            return break_targets(((ComptimeBlockNode*)stmt)->body, labels, nlabels, crossed);
        case AST_LABEL_STMT:
            return break_targets(((LabelStmtNode*)stmt)->stmt, labels, nlabels, crossed);
        default:
            // Every other statement shape (return, goto, expr, var/const
            // decl, assignment, defer, go, bare break/continue already
            // handled above, fallthrough) has no nested statement
            // substructure that could hold a break.
            return 0;
    }
}

static int break_targets_list(ASTNode* first, const char** labels, size_t nlabels, int crossed) {
    for (ASTNode* s = first; s; s = s->next) {
        if (break_targets(s, labels, nlabels, crossed)) return 1;
    }
    return 0;
}

// Shared by plain blocks, arena bodies (via stmt_terminates's
// AST_ARENA_BLOCK case delegating into a block), and (indirectly)
// case-clause bodies (clause_chain_terminates/type_switch_terminates call
// this directly on a clause's raw statement list, which is not itself
// wrapped in a BlockStmtNode).
//
// Deliberate divergence from the Go spec's literal wording ("ends in a
// terminating statement" — the FINAL statement only): this asks whether
// ANY statement in the list terminates, not just the last. Go's grammar
// structurally keeps this distinction from mattering in the cases that
// reach here (an expression statement must be a call/receive/send/inc-dec,
// so meaningful dead code after an unconditional return is rare and, where
// it does occur, real Go rejects it exactly like the last-statement-only
// rule would). Goo's grammar is looser (any expression is a valid
// statement — see asi_return_probe.goo's `return` followed by a bare
// `r + 1`), so textually-final-but-dead statements are common and
// codegen_generate_block_stmt (statement_codegen.c) already treats them as
// unreachable and skips emitting them once an earlier statement has
// terminated the current LLVM block. Checking "any" here keeps the type
// checker's notion of termination consistent with what codegen actually
// does — sound, since a statement can only make this predicate return true
// by itself unconditionally terminating (stmt_terminates does no
// cross-statement flow merging), so nothing among the four required reject
// shapes (bare missing return, if-without-else, switch-without-default,
// non-terminating last clause) is weakened: none of those has an EARLIER
// statement that already terminates unconditionally, only "last" vs "any"
// changes which trailing dead code is tolerated.
static int block_terminates(ASTNode* first_stmt) {
    for (ASTNode* s = first_stmt; s; s = s->next) {
        if (stmt_terminates(s)) return 1;
    }
    return 0;
}

// Entry point used for any statement position that has NO enclosing label
// of its own interest to this predicate (if/else branches, block/arena
// final statements) — `labels`/`nlabels` only ever matter for a for/switch/
// type-switch/select statement's OWN self-targeting labeled-break check,
// which stmt_terminates_labeled's AST_LABEL_STMT case supplies by unwrapping
// any label chain immediately before dispatching.
static int stmt_terminates(ASTNode* stmt) {
    return stmt_terminates_labeled(stmt, NULL, 0);
}

static int stmt_terminates_labeled(ASTNode* stmt, const char** labels, size_t nlabels) {
    if (!stmt) return 0;
    switch (stmt->type) {
        case AST_RETURN_STMT:
        case AST_GOTO_STMT:
            return 1;
        case AST_EXPR_STMT:
            return is_panic_call_stmt(stmt);
        case AST_IF_STMT: {
            IfStmtNode* i = (IfStmtNode*)stmt;
            return i->else_stmt != NULL &&
                   stmt_terminates(i->then_stmt) &&
                   stmt_terminates(i->else_stmt);
        }
        case AST_IF_LET_STMT: {
            IfLetStmtNode* il = (IfLetStmtNode*)stmt;
            return il->else_stmt != NULL &&
                   stmt_terminates(il->then_stmt) &&
                   stmt_terminates(il->else_stmt);
        }
        case AST_FOR_STMT: {
            ForStmtNode* f = (ForStmtNode*)stmt;
            // `for {}` only: an explicit condition OR a range clause both
            // give the loop a completion path Go does not consider
            // unconditional (spec: "the loop condition is absent, and the
            // 'for' statement does not use a range clause").
            if (f->condition != NULL || f->range_expr != NULL) return 0;
            return !break_targets(f->body, labels, nlabels, 0);
        }
        case AST_SWITCH_STMT:
            return switch_terminates(((SwitchStmtNode*)stmt)->cases, labels, nlabels);
        case AST_TYPE_SWITCH:
            return type_switch_terminates(((TypeSwitchNode*)stmt)->cases, labels, nlabels);
        case AST_SELECT_STMT:
            return select_terminates(((SelectStmtNode*)stmt)->cases, labels, nlabels);
        case AST_BLOCK_STMT:
            return block_terminates(((BlockStmtNode*)stmt)->statements);
        case AST_ARENA_BLOCK:
            return stmt_terminates(((ArenaBlockNode*)stmt)->body);
        case AST_LABEL_STMT: {
            // Chase a (possibly multi-level, `L1: L2: stmt`) label chain,
            // collecting every name so a for/switch/type-switch/select at
            // the bottom sees ALL of them when checking self-targeting
            // labeled breaks — not just the innermost. Bounded at 8 (no
            // realistic program stacks more labels than that on one
            // statement); a chain longer than that silently stops
            // collecting further outer labels, mirroring this codebase's
            // established "degrade gracefully past a source-bounded cap"
            // convention (e.g. TcFunctionContext.literal_stack, types.h).
            const char* chain[8];
            size_t n = 0;
            ASTNode* inner = stmt;
            while (inner && inner->type == AST_LABEL_STMT) {
                LabelStmtNode* l = (LabelStmtNode*)inner;
                if (n < 8) chain[n++] = l->name;
                inner = l->stmt;
            }
            if (!inner) return 0;
            return stmt_terminates_labeled(inner, chain, n);
        }
        default:
            return 0;
    }
}

// Public entry point (types.h): does `body` — a function or func literal's
// BlockStmtNode body — complete only via a terminating statement? Callers:
// type_check_function_decl (type_checker.c) and type_check_func_lit
// (expression_checker.c), both gated on a non-void declared return type.
int stmt_is_terminating(ASTNode* body) {
    return stmt_terminates(body);
}
