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
// source of truth for that curated set — both analyses consult it so the list
// can never drift between them.
//
// SOUNDNESS: every entry is a TRUST assertion — if a listed function actually
// retained an argument, an arena value passed to it would be freed while still
// referenced, a use-after-free. Each entry below is justified individually in
// nonretaining.c and must stay true; adding an entry requires the same
// per-function proof. The whitelist only ever REDUCES escaping, so a wrong
// entry is the one direction that can dangle a pointer.
//
// Applies ONLY to calls that do not resolve to a user function: a user who
// shadows a builtin name (e.g. their own `len`) is analysed by its real body,
// never this list (the callers check the registry first).
//
// STATUS: codegen-inert today. The motivating case, `fmt.Println(node)` with a
// pointer/struct, does not yet type-check (v1 fmt formats only string / integer
// / bool / float), so no compilable program currently routes an arena value
// through a whitelisted call. This lands the analysis + its tests ahead of that
// fmt support (like Task 7c was wired inert before Task 6 activated it); when
// fmt gains pointer/struct formatting the whitelist activates automatically and
// its entries must be re-verified against the then-live behaviour.

#include "ast.h"
#include <stdbool.h>

// True iff `call_function` (a CallExprNode.function) names a whitelisted
// non-retaining external — a plain-identifier builtin (len/cap/print/println)
// or an `fmt` selector (Print/Println/Printf/Sprintf). False for everything
// else, including a NULL node. Pure; does not mutate the AST.
bool goo_callee_is_non_retaining(const ASTNode* call_function);

#endif // NONRETAINING_H
