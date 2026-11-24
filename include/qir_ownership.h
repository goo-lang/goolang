#ifndef QIR_OWNERSHIP_H
#define QIR_OWNERSHIP_H

// Phase 7 M1 — Compile-time linear ownership checker for `qir.Qubit`.
// See docs/superpowers/specs/2026-11-24-quantum-qir-linear-qubits-design.md.
//
// Enforces six proof obligations on every function in a program that imports
// `"qir"` (programs that do not import `"qir"` pay zero cost):
//
//   Obligation 1 (No-Cloning): `qir.Qubit` is move-only. Assignment from an
//     existing binding moves the source; reading a moved or consumed binding
//     is a compile error; overwriting a live binding without consuming it is
//     a compile error; storing `qir.Qubit` in any container, struct, pointer,
//     channel, interface, or global variable is rejected. Duplicate active
//     `qir.QubitAt(id)` bindings with the same comptime `id` are rejected.
//   Obligation 2 (No-Aliasing in Multi-Qubit Calls): every call taking >= 2
//     `qir.Qubit` arguments (`qir.CX(c, t)`, `qir.CZ`, `qir.Swap`, `qir.CCX`,
//     or user functions) requires distinct live qubit bindings.
//   Obligation 3 (Must-Consume / Safe Uncomputation): every owned `qir.Qubit`
//     binding must be consumed (`qir.FreeZero`, `qir.MeasureReset`) or
//     returned before lexical scope exit and before every `return`.
//   Obligation 4 (Branch-Join Agreement): all non-terminating branches of
//     `if`/`else`, `switch`, `type switch`, `if let`, and `match` must agree
//     on the `LIVE` vs `CONSUMED`/`MOVED` state of every outer `qir.Qubit`.
//   Obligation 5 (Loop Invariance): a `qir.Qubit` declared outside a `for`
//     loop may not be moved or consumed inside the loop; a `qir.Qubit`
//     declared inside a loop body must be consumed before the iteration ends.
//   Obligation 6 (No Goroutine / Closure / Defer Escape): a `qir.Qubit` may
//     not be passed to `go` or `defer`, or captured by a function literal.

#include "types.h"

int qir_ownership_check_program(TypeChecker* checker, ASTNode* program);

#endif // QIR_OWNERSHIP_H
