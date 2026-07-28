#ifndef NONRETAINING_H
#define NONRETAINING_H

// Arena leg — Task 7a': non-retaining external-call whitelist.
//
// The escape analyses (param_escape.c / block_escape.c) treat a call to any
// function they cannot resolve to a user body as conservatively RETAINING every
// argument (pure-conservative, so an escaping arg is heap-promoted). That is
// sound but imprecise: a handful of well-known externals provably do NOT retain
// their pointer arguments past the call, so passing an arena value to one of
// them should not force it out of the arena. This predicate is the single
// source of truth for that curated set — all THREE analyses consult it
// (through escape_core.c's shared call_taint) so the list can never drift.
//
// The set has two halves. Plain-identifier builtins (len/cap/print/println)
// live in nonretaining.c. Package selector calls are answered from the shim
// table's `non_retaining` column (src/types/shim_signatures.c), which is the
// declarative home for every other shim fact and carries the per-symbol proof
// beside the row. The six `fmt` entries used to be a second hardcoded list
// here and were moved there so the two can no longer disagree.
//
// SOUNDNESS: every entry is a TRUST assertion — if a listed function actually
// retained an argument, an arena value passed to it would be freed while still
// referenced, a use-after-free. Each entry is justified individually beside
// itself (builtins in nonretaining.c, shims in the shim table) and must stay
// true. Adding an entry needs the same per-function proof. The whitelist only
// ever REDUCES escaping, so a wrong entry is the one direction that can dangle
// a pointer.
//
// The assertion has TWO halves, and both are required, because escape_core.c's
// `whitelisted` branch reads this one predicate for both: the callee must not
// keep a pointer argument past the call, AND its result must not alias one.
// `errors.Unwrap` fails the second half alone — it returns `e->cause`, a
// pointer into its argument — which is why it is excluded and why a
// "does it retain" flag on its own would have been the wrong shape.
//
// Applies ONLY to calls that do not resolve to a user function: a user who
// shadows a builtin name (e.g. their own `len`) is analysed by its real body,
// never this list (the callers check the registry first).
//
// STATUS, by consumer, because it differs:
//
//   - ARENAS (block_escape): still inert. No shim takes a user pointer — the
//     parameter kinds are string, int64, float64, []string and error — so no
//     arena value can reach a whitelisted call. `fmt.Println(node)` with a
//     pointer or struct, the original motivating case, still does not
//     type-check. When fmt gains pointer/struct formatting the whitelist
//     activates for arenas automatically, and its entries must be re-verified
//     against the then-live behaviour.
//
//   - LOCALS (local_escape): LIVE, and it is the difference between an ARC
//     release consumer reclaiming something and reclaiming nothing. Measured
//     on bench/daemon: before the shim rows every local passed to any stdlib
//     call was marked escaping for no reason but the missing summary, and
//     adding them made 30.2% of the retained bytes provably non-escaping. See
//     docs/superpowers/specs/2026-07-28-daemon-alloc-attribution-findings.md.

#include "ast.h"
#include <stdbool.h>

// True iff `call_function` (a CallExprNode.function) names a whitelisted
// non-retaining external: a plain-identifier builtin (len/cap/print/println),
// or a package selector whose shim row sets `non_retaining`. False for
// everything else, including a NULL node. Pure, and does not mutate the AST.
//
// Answers by NAME alone, so a caller must first rule out that the selector's
// base is a local — a variable called `strings` is not the strings package.
// escape_core.c's selector_base_is_local is that guard, pinned by
// param_escape row 23.
bool goo_callee_is_non_retaining(const ASTNode* call_function);

#endif // NONRETAINING_H
