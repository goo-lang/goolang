// Codegen hardening R2a: value_scope implementation — a mechanical wrapper
// over the value-table high-water-mark idiom that used to be hand-rolled at
// every save/truncate site. See include/value_scope.h for the full design
// rationale and docs/superpowers/specs/2026-07-09-codegen-hardening-design.md
// section R2a. No behavior change: vscope_enter reads value_table_size,
// vscope_exit writes it back under the same guard every call site used to
// apply itself, and vscope_add forwards straight to codegen_add_value.
#include "value_scope.h"

#if LLVM_AVAILABLE

size_t vscope_enter(CodeGenerator* codegen) {
    if (!codegen) return 0;
    return codegen->value_table_size;
}

void vscope_exit(CodeGenerator* codegen, size_t mark) {
    if (!codegen) return;
    // Guarded, not unconditional, mirroring every pre-existing truncate
    // site (they never grew value_table_size back up either).
    if (codegen->value_table_size > mark) {
        codegen->value_table_size = mark;
    }
}

int vscope_add(CodeGenerator* codegen, ValueInfo* info) {
    int ok = codegen_add_value(codegen, info);
    // T4: this is the ONE point every local binding passes through, and the only
    // place where an approved local's slot is still live and its name still known.
    // The function body is a block statement, so vscope_exit truncates the table
    // before any exit path runs -- reading the table at exit found only the
    // parameters. See codegen_arc_note_local (statement_codegen.c).
    if (ok) codegen_arc_note_local(codegen, info);
    return ok;
}

#endif // LLVM_AVAILABLE
