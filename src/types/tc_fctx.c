// Codegen-hardening R1-TC: TcFunctionContext implementation. Mechanical
// mirror of src/codegen/cfctx.c's cfctx_save/cfctx_restore/cfctx_reset for
// the type checker's own per-function scratch state (TcFunctionContext,
// types.h). No behavior change: every save/reset/restore site this replaces
// (type_check_function_decl, type_checker.c; type_check_func_lit,
// expression_checker.c) previously hand-enumerated the same fields as
// separate local variables — see docs/superpowers/specs/2026-07-09-codegen-
// hardening-design.md, section R1-TC.
#include "types.h"
#include <string.h>

void tc_fctx_save(TcFunctionContext* out, const TcFunctionContext* ctx) {
    if (!out || !ctx) return;
    *out = *ctx;
}

void tc_fctx_restore(TcFunctionContext* ctx, const TcFunctionContext* saved) {
    if (!ctx || !saved) return;
    *ctx = *saved;
}

void tc_fctx_reset(TcFunctionContext* ctx) {
    if (!ctx) return;
    ctx->label_count = 0;
    ctx->goto_label_count = 0;
}
