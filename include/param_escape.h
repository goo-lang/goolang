#ifndef PARAM_ESCAPE_H
#define PARAM_ESCAPE_H

// Arena leg — Task 7a: interprocedural param-escape summaries.
//
// Pure static analysis: for every user-defined function F, computes one
// boolean per parameter NAME (flattened across grouped-name VarDeclNodes,
// receiver spliced in as parameter 0 for methods):
//
//     escapes[i] == true  =>  a value reachable from parameter i MAY outlive
//                              the call to F.
//
// Soundness invariant: `true` is always a safe answer (worst case: promote
// to heap, lose the arena optimization). `false` asserts "provably does not
// outlive" — so every construct this module does not precisely understand
// must default to `true`. Under-marking is the only bug class that can
// dangle a pointer; over-marking only costs performance.
//
// See docs/superpowers/specs/2026-07-07-arena-7a-param-escape-summaries-design.md
// for the full design (escape sinks, taint rules, interprocedural fixpoint,
// and the 15-row test matrix this module must satisfy).
//
// Explicitly NOT this module's job: no codegen change, no arena free, no
// allocation-site promotion decision (that is task 7b, which consumes these
// summaries). This module does not mutate the AST it analyzes.

#include "ast.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct ParamEscapeSummary {
    char*  function_name;   // owned
    bool*  escapes;         // length param_count; true = may outlive the call
    size_t param_count;     // number of parameter NAMES (flattened)
    bool   return_escapes;  // does F return a value derived from one of its
                             // own params? (interprocedural fixpoint signal;
                             // also exposed for callers/tests that want it)
} ParamEscapeSummary;

typedef struct ParamEscapeResult {
    ParamEscapeSummary* summaries;   // one per user-defined function/method
    size_t count;
} ParamEscapeResult;

// Analyze every AST_FUNC_DECL reachable from `program`'s top-level
// declarations (methods included — a method's receiver is parameter 0).
// Pure: does not mutate `program`. Returns NULL ONLY on allocation failure;
// a NULL/malformed `program` yields a valid, empty (count == 0) result.
ParamEscapeResult* param_escape_analyze(ASTNode* program);
void param_escape_result_free(ParamEscapeResult* result);

// Lookup helpers. Conservative on a miss, per the soundness contract: an
// unrecognized function name, or an out-of-range parameter index, must not
// be silently treated as "does not escape" by any consumer.
const ParamEscapeSummary* param_escape_lookup(const ParamEscapeResult* result, const char* fn);
bool param_escape_param_escapes(const ParamEscapeResult* result, const char* fn, size_t param_idx);

#endif // PARAM_ESCAPE_H
