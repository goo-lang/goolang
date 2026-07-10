// Runtime defer stack (P3.4) — see the doc comment on goo_defer_frame_t
// (include/runtime.h) for the design. This file holds only the two entry
// points codegen calls: goo_defer_push (once per dynamic `defer` execution)
// and goo_defer_run (once per function-exit path).
#include "runtime.h"

void goo_defer_push(goo_defer_frame_t* f, void (*fn)(void* env), void* env) {
    // Defensive: codegen always passes the address of a live entry-block
    // alloca, so this is never NULL in practice — guarded anyway rather
    // than trusting an invariant a future codegen bug could violate.
    if (!f) return;

    if (f->len >= f->cap) {
        size_t newcap = f->cap ? f->cap * 2 : 4;
        // goo_realloc panics on allocation failure itself (see runtime.c),
        // so there is no NULL-check-then-panic needed here — a return
        // means the grow succeeded.
        f->entries = goo_realloc(f->entries, newcap * sizeof(goo_defer_entry_t));
        f->cap = newcap;
    }

    f->entries[f->len].fn = fn;
    f->entries[f->len].env = env;
    f->len++;
}

void goo_defer_run(goo_defer_frame_t* f) {
    if (!f) return;

    // LIFO: last pushed runs first — same order Go's deferred calls unwind
    // in, and the same order the static per-lexical-defer path already
    // emits (statement_codegen.c's codegen_emit_deferred_calls).
    for (size_t i = f->len; i > 0; i--) {
        goo_defer_entry_t* e = &f->entries[i - 1];
        if (e->fn) e->fn(e->env);
        goo_free(e->env);  // no-op for a NULL env (zero-arg, zero-receiver defer)
    }

    goo_free(f->entries);
    f->entries = NULL;
    f->len = 0;
    f->cap = 0;
}
