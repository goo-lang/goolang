#ifndef OWNERSHIP_CHECKER_H
#define OWNERSHIP_CHECKER_H

// Minimal, NARROW extern surface for src/types/ownership_checker.c — a
// move/borrow-checker skeleton that has existed in the tree since before P6
// (see docs/superpowers/specs/2026-07-11-p6-lanes-m1-design.md, "What
// already exists to build on") but has ZERO wired call sites of its own;
// it was dropped from GOO_OBJS in P5.6.
//
// P6 M1 Task 5 adds ownership_checker.c back onto the compiled-in source
// list (GOO_TYPES_SRCS) ONLY because src/types/lane_ownership.c calls this
// ONE function. Nothing else in ownership_checker.c is declared here, and
// nothing else is wired: do NOT read this header's presence as "the
// ownership checker is live" for ordinary Goo code. `variable_new`
// (src/types/types.c) still defaults every Variable to OWNERSHIP_OWNED, so
// calling any of ownership_checker.c's OTHER entry points
// (check_borrow_rules, check_ownership_assignment, ...) against ordinary
// code would move-poison it. See lane_ownership.h's header comment for why
// this one call is safe: it is a best-effort mirror onto Variable.is_moved,
// not lane_ownership.c's source of truth for its own reject decisions.

#include "types.h"

// Mark the Variable currently in scope named `name` as moved (Variable.
// is_moved = 1), updating declared_pos to `pos` for any future diagnostic
// that reads it. A no-op if `name` does not resolve in the checker's
// current scope (e.g. it was a function-local whose Scope has already been
// popped and freed — see lane_ownership.h's LIFETIME CAVEAT for why that is
// the common case at lane_ownership.c's call site).
void mark_variable_moved(TypeChecker* checker, const char* name, Position pos);

#endif // OWNERSHIP_CHECKER_H
