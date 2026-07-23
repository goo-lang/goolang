#ifndef VALUE_SCOPE_H
#define VALUE_SCOPE_H

#include "codegen.h"
#include <stddef.h>

// Codegen hardening R2a: scoped symbol table API wrapping the value-table
// high-water-mark idiom that was previously hand-rolled at every save/
// truncate site (function start in codegen_enter_function/codegen_exit_
// function, match-arm teardown in composite_codegen.c, block-stmt and
// select-case teardown — the latter from the F3 fix — in
// statement_codegen.c, catch-body teardown in error_union_codegen.c) plus
// every raw codegen_add_value call site. Mechanical only — see
// docs/superpowers/specs/2026-07-09-codegen-hardening-design.md, section
// R2a: this commit adds NO new truncation points (a plain `{ }` block that
// did not already truncate before this commit still leaks after it — that
// is R2b's job). The value here is that value-table scope lifetime now has
// exactly one owner (this file) instead of five call-site-local
// reimplementations of `size_t saved = codegen->value_table_size; ...;
// codegen->value_table_size = saved;`.
//
// vscope_enter/vscope_exit are deliberately mark-based (not a push/pop
// stack owned by CodeGenerator) so they drop in at each existing call site
// with no change to that site's control flow: the mark is just a local
// variable, exactly as before, only obtained/consumed through this API
// instead of reading/writing codegen->value_table_size directly.
// codegen_enter_function/codegen_exit_function (codegen.c) are the one
// exception — their mark lives in codegen->value_table_function_start (a
// CodeGenerator field, not a local) because codegen_generate_func_lit's
// nested-emission save/restore (function_codegen.c) must snapshot and
// restore that specific mark across a NESTED codegen_enter_function/
// codegen_exit_function pair; vscope_enter/vscope_exit still do the actual
// table read/truncate there, the field is just where the mark is kept
// between the two calls instead of a stack local.

// Record the current value-table high-water mark. Call at the start of any
// lexical scope whose bindings must not outlive it (function body, match
// arm, select case, catch-handler block, plain block). The returned mark is
// an opaque token for the matching vscope_exit — callers must not interpret
// it beyond that. Returns 0 if `codegen` is NULL.
size_t vscope_enter(CodeGenerator* codegen);

// Truncate the value table back to `mark` (obtained from a prior
// vscope_enter), discarding every binding added since. Guarded exactly like
// every call site's own truncate was before this API existed: a no-op if
// the table is already at or below `mark` — never grows the table back up.
void vscope_exit(CodeGenerator* codegen, size_t mark);

// Append `info` to the value table. Thin wrapper over codegen_add_value —
// exists so every call site goes through this one name instead of half
// going through codegen_add_value directly. Same contract: returns 1 on
// success, 0 on allocation failure or a NULL argument.
int vscope_add(CodeGenerator* codegen, ValueInfo* info);

#endif // VALUE_SCOPE_H
