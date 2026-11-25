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

    // Does the caller OWN every value F returns? Filled by
    // returns_fresh_compute (include/returns_fresh.h), not by
    // param_escape_analyze, which leaves it false. True only on a positive
    // proof: every return path gives a fresh allocation, a literal, an
    // owned-result shim call, a call to a function that is itself
    // returns_fresh, or a local that holds only such values and escapes only
    // by being returned.
    //
    // NOT the negation of return_escapes. A value from a global, or one that
    // F also stored in a container, carries no parameter taint, so
    // return_escapes reads false for it -- and ARC freed it (finding C2).
    // Anything this analysis cannot prove reads false, and false only leaks.
    bool   returns_fresh;

    // F has a receiver. A bare call `name()` never calls a method, so the
    // ownership lookup skips a method summary that shares the name.
    bool   is_method;
} ParamEscapeSummary;

typedef struct ParamEscapeResult {
    ParamEscapeSummary* summaries;   // one per user-defined function/method
    size_t count;
    // The fixpoint hit its cap and every summary is a fail-closed guess.
    // returns_fresh_compute then proves nothing.
    bool fail_closed;
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

// Is every value that a bare call `fn()` returns fresh memory? Only a summary
// with no receiver can match, because a bare call never calls a method. The
// answer is true only when EVERY such summary with that name has returns_fresh.
// A miss is false.
bool param_escape_returns_fresh(const ParamEscapeResult* result, const char* fn);

// Does the caller own the result of a bare call `fn()`? That is
// param_escape_returns_fresh AND no matching summary has return_escapes: fresh
// memory that holds an argument (`return &Holder{p: b}`) is owned, but it is
// not free of the caller's locals, and release_decision's alias scan prunes an
// owned call result whole. A miss is false.
bool param_escape_call_result_is_owned(const ParamEscapeResult* result, const char* fn);

#endif // PARAM_ESCAPE_H
