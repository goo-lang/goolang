#ifndef LANE_OWNERSHIP_H
#define LANE_OWNERSHIP_H

// P6 M1 Tasks 5+6 — lanes-specific ownership checks (Component 4 of
// docs/superpowers/specs/2026-07-11-p6-lanes-m1-design.md). This file
// implements all four proof obligations, all as one per-function AST walk:
//
//   Obligation 1 (Task 5): lanes.Partition(arr, count) MOVES arr. Any later
//     read of arr in the same function body is a compile error.
//   Obligation 2 bookkeeping (Task 6): lanes.Run CONSUMES its Partitioned
//     argument; a SECOND lanes.Run on the same binding is use-after-consume
//     ("...consumed by lanes.Run"). The disjoint-view (count,width) split is
//     recorded on LanePartitionBinding (structural for the blessed Partition).
//   Obligation 3 (Task 6): single-goroutine attribution. Inside a lanes.Run
//     body, the *Lane ctx param and every name transitively derived from it
//     (`t := ctx.Own()`, `u := t`) are "lane-derived". A `go` that carries a
//     lane-derived name — as a call argument OR as a captured name of the
//     launched literal — is rejected ("...may not be passed to another
//     goroutine"). Mirrors param_escape.c's Sink #4 two surfaces as a
//     rejection. Transitive-copy depth is UNBOUNDED (single forward pass;
//     Go's declare-before-use makes one pass sufficient).
//   Obligation 4 (Task 6): view-capture rule. A lanes.Run body literal whose
//     captured_names include a Partition binding, a moved source array, or a
//     name lane-derived from an ENCLOSING Run body is rejected ("lane body
//     may not capture ..."). This is a capture/escape rule, NOT index-range
//     analysis (per the amended spec) — captured_names is TRUSTED from the
//     checker (ast.h FuncLitNode; param_escape.c Sink #3 precedent), never
//     recomputed by re-walking closure bodies. Benign captures (outer
//     scalars, non-partition slices) stay legal.
//
// SCOPE (Option A, per the design's "Scope boundary"): this is NOT a
// general borrow checker. It is a single, self-contained, per-function AST
// walk over code shaped around a `lanes.Partition` call — nothing else. A
// program with no `lanes` import does zero extra work beyond package-marker
// lookups that all fail to match "lanes" (there are none to match). The
// walk fails CLOSED in the sense that matters for a rejection pass: an AST
// shape it does not specifically recognize is simply not flagged — never
// crashes. The known EXCEPTIONS to "never rejects code this pass was not
// built to reason about" are the name-shadowing surfaces below (including
// detection-level shadowing of the package name itself) and the
// flow-insensitive branch note — all documented, all clean positioned
// diagnostics, none a crash.
//
// Task 6 extends the SAME per-function walk (see LaneWalkContext in
// lane_ownership.c) to add the view-attribution (obligation 3, at `go`/
// `lanes.Run` boundaries) and capture (obligation 4, closure
// captured_names) checks, rather than introducing a second walker.
//
// UNDER-WALKED STATEMENT KINDS (Task 6 must know this before extending the
// walk): lane_walk_stmt's switch does not descend into every statement
// kind the grammar produces. These are NOT recursed into at all — a
// lanes.Partition move or a moved-name read occurring inside one of them is
// silently invisible to this pass:
//   - AST_IF_LET_STMT     (`if let x = ... { }`)
//   - AST_SELECT_STMT     (`select { case ...: }`)
//   - AST_ARENA_BLOCK     (`arena { ... }`)
//   - AST_UNSAFE_STMT     (`unsafe { ... }`)
//   - AST_COMPTIME_BLOCK  (`comptime { ... }`) — inert compile-time code;
//                          deliberately NOT walked (documented, not a bug).
//   - a local AST_CONST_DECL (function-body `const n = ...`)
// (Task 6 ADDED AST_LABEL_STMT descent — `L: stmt` now recurses into its
// wrapped statement in both lane_walk_stmt and lane_body_walk_stmt — so it
// is no longer on this list.) This is the safe direction for a rejection
// pass (under-reject; the KNOWN false-reject surfaces are (a) name
// shadowing, since every table here is flat and shadow-unaware — Option A
// envelope, documented at LaneWalkContext.outer_derived; (b) DETECTION-
// level shadowing: lane_call_is_lanes_selector resolves the qualifier via
// type_checker_lookup_variable after function-local scopes are popped, so
// a LOCAL VARIABLE named `lanes` with its own Partition/Run method is
// misattributed as the package call and its argument move-poisoned —
// exotic, clean diagnostic, no crash; a code fix needs parse-time scope
// info, out of Option A scope; and (c) flow-insensitivity: a Partition
// inside a conditional branch poisons post-branch reads even when the
// branch may not execute at runtime — conservative move-checker behavior),
// but
// it means obligation 1 (and Task 6's obligations 3/4) are simply not
// enforced inside these shapes today.
//
// NOTE: the obligation-3 body walker (lane_body_walk_stmt) has a NARROWER
// statement coverage than the list above, which describes lane_walk_stmt:
// inside a Run body, taint does not propagate through AST_MULTI_ASSIGN
// targets or plain-assignment ExprStmts (`x = ctx.Own()`), and the body
// walker does not descend into IF_LET/SELECT/ARENA/UNSAFE or into nested
// non-launched FuncLit bodies. All under-reject only. Separately: a bare, unbound
// `lanes.Partition(...)` statement (the
// result discarded, not assigned via `:=`/`var`) records NO move at all —
// only lane_handle_var_decl's VarDeclNode path recognizes a Partition call
// as a move; an ExprStmt-level bare call reaches lane_check_reads/
// lane_handle_expr_stmt as an ordinary (non-move) read site.
//
// DETECTION MECHANISM (spike-verified — see
// docs/superpowers/specs/2026-07-11-p6-lanes-m1-spike-findings.md
// Section 2.1, citing expression_checker.c:4300-4307 / 3630-3642): a
// `lanes.Partition(...)` call is recognized by resolving the callee
// selector's package-qualifier identifier through the checker's TYPE_
// PACKAGE marker (type_checker_lookup_variable -> Variable.package) and
// matching Package.import_path against "lanes" — NOT by comparing the
// literal source-level identifier text used at the call site.
//
// IMPORT-ALIAS SUBTLETY (found while implementing this task; a pre-existing
// gap, not fixed here): the Goo grammar DOES support import aliasing
// (`import lns "lanes"` — parser.y's `import_spec: identifier
// STRING_LITERAL`, ImportSpecNode.alias in ast.h). But src/compiler/goo.c's
// real-package resolution path (compile_resolved_packages, ~goo.c:1018-
// 1034) seeds the TYPE_PACKAGE marker under the resolved package's OWN
// declared name (PkgEntry.name, from its `package lanes` clause) and never
// consults spec->alias for a non-stdlib-shim import — only
// seed_imported_stdlib_markers (goo.c:877-895) honours ->alias, and that
// path is for the hardcoded stdlib shims, not vendored source packages like
// `lanes`. So an aliased `import lns "lanes"` cannot resolve `lns.Partition`
// at all today; the source must literally write `lanes.Partition(...)` to
// compile at all. Consequence for THIS pass: keying on Package.import_path
// (resolved package identity) rather than source-level identifier text is
// still the right design — it is correct today, and remains correct if that
// aliasing gap is ever fixed, since the marker Variable's ->package
// identity is unaffected either way.
//
// WIRING: called once from type_check_program (type_checker.c), AFTER the
// pass-2 body-checking loop. That timing is chosen for Task 6's sake —
// FuncLitNode.captured_names is only fully populated once every function
// body has been type-checked (see the spike's Section 2.0) — Task 5's own
// obligation-1 check does not itself need captured_names, but sharing one
// late-walk call site avoids two separate passes over the same program.
//
// LIFETIME CAVEAT (why this pass keeps its OWN move-tracking table instead
// of relying on Variable.is_moved as its source of truth): by the time this
// pass runs, every function body's own Scope has already been popped and
// freed (type_check_function_decl's scope_pop, type_checker.c:1494) —
// ordinary function-local Variables (e.g. `arr`, `p`) no longer exist in
// any live scope, so a checker-scope lookup for one of them fails. This
// pass therefore builds and consumes its own per-function moved-name table
// (LaneWalkContext, in lane_ownership.c) as the actual source of truth for
// its reject decisions. It still calls mark_variable_moved (declared in
// ownership_checker.h) alongside that — a harmless best-effort mirror for
// the rare case the moved name is a package-level global (still resolvable
// in the persistent top-level scope), and to keep Variable.is_moved
// consistent for any future consumer that might run earlier in the
// pipeline. Package markers themselves ARE still resolvable here (unlike
// ordinary locals) because they live in the top-level scope, seeded before
// any function body is checked and never popped until the whole compile
// finishes — see the DETECTION MECHANISM note above.

#include "types.h"

// One `lanes.Partition(...)` result binding recorded for Task 6 to consume
// (obligations 3/4: view -> goroutine attribution, and the view-capture
// rule). Built fresh per function by lane_ownership_check_program's walk;
// entries are valid only for the duration of that ONE function's
// processing (Option A: no interprocedural analysis) — do not retain a
// pointer to one past that function's walk.
//
// Borrowed strings: `binding_name` and `source_name` point directly into
// AST-owned storage (VarDeclNode.names[i] / IdentifierNode.name) — this
// file never copies or frees them, so they stay valid exactly as long as
// the AST does.
typedef struct LanePartitionBinding {
    const char* binding_name;     // e.g. "p" in `p := lanes.Partition(...)`
    const char* source_name;      // e.g. "data" — the moved slice argument
    Position    partition_pos;    // position of the lanes.Partition(...) call
    uint64_t    lane_count;       // the comptime `count` argument's folded
                                   // value, meaningful only if lane_count_known
    int         lane_count_known; // 1 if lane_count folded to a compile-time
                                   // constant (goo_fold_const_int_ctx); 0
                                   // otherwise — Task 6 MUST treat 0
                                   // defensively, not every count argument is
                                   // a literal or const identifier
    struct LanePartitionBinding* next;
} LanePartitionBinding;

// Entry point. Walks every top-level AST_FUNC_DECL in `program`; within
// each function body, flags any read of a lanes.Partition-moved identifier
// occurring later in that SAME function (obligation 1) via a positioned
// type_error. Returns 1 (ok for type_check_program to proceed) unless this
// call itself added a new error (checker->error_count grew during the
// call) or checker/program is NULL/malformed — matching the call-site
// idiom `if (!lane_ownership_check_program(checker, program)) return 0;`.
int lane_ownership_check_program(TypeChecker* checker, ASTNode* program);

#endif // LANE_OWNERSHIP_H
