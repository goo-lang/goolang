#ifndef LANE_OWNERSHIP_H
#define LANE_OWNERSHIP_H

// P6 M1 Task 5 — lanes-specific ownership checks (Component 4 of
// docs/superpowers/specs/2026-07-11-p6-lanes-m1-design.md). This file
// implements Obligation 1 only:
//
//   lanes.Partition(arr, count) MOVES arr. Any later read of arr in the
//   same function body is a compile error.
//
// SCOPE (Option A, per the design's "Scope boundary"): this is NOT a
// general borrow checker. It is a single, self-contained, per-function AST
// walk over code shaped around a `lanes.Partition` call — nothing else. A
// program with no `lanes` import does zero extra work beyond package-marker
// lookups that all fail to match "lanes" (there are none to match). The
// walk fails CLOSED in the sense that matters for a rejection pass: an AST
// shape it does not specifically recognize is simply not flagged — never
// crashes, never rejects code this pass was not built to reason about.
//
// Task 6 extends the SAME per-function walk (see LaneWalkContext in
// lane_ownership.c) to add the view-attribution (obligation 3, at `go`/
// `lanes.Run` boundaries) and capture (obligation 4, closure
// captured_names) checks, rather than introducing a second walker.
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
