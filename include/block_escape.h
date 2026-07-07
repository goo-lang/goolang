#ifndef BLOCK_ESCAPE_H
#define BLOCK_ESCAPE_H

// Arena leg — Task 7b: per-alloc-site block-escape decisions.
//
// SOUNDNESS SIBLING of include/param_escape.h / src/types/param_escape.c:
// this module adapts that module's taint-propagation engine with two
// coordinates moved — source = each allocation site inside an arena{} block
// (7a's source was a function parameter), escape boundary = the arena
// block (7a's boundary was the whole function). The taint-propagation
// switch, the sink vocabulary, the TaintSet representation, and the local
// fixpoint for loop back-edges are the same shape in both files. A
// soundness fix to that shared taint-propagation shape in ONE of these two
// files MUST be mirrored in the other (until/unless a third consumer
// justifies extracting a shared core).
//
// Pure static analysis: for every allocation site (see below) lexically
// inside an `arena { }` block, computes one boolean:
//
//     escapes_block == true  =>  the allocated value MAY outlive the
//                                  enclosing arena block -> MUST be heap
//     escapes_block == false =>  provably dies with the block -> arena-
//                                  eligible
//
// Soundness invariant identical to 7a's: `true` is always a safe answer
// (worst case: keep it on the heap, lose the arena optimization). `false`
// asserts "provably does not outlive the block" — so every construct this
// module does not precisely understand must default to `true`.
// Under-marking is the only bug class that can dangle a pointer (use-after-
// free once 7c/8 wire arena reset/free in); over-marking only costs
// performance.
//
// Allocation sites (first cut — see the design doc for the full rationale):
//   1. `new(T)`               — AST_CALL_EXPR, function == identifier "new"
//   2. `&<composite literal>` — AST_UNARY_EXPR, operator == &, operand is
//                                 an AST_STRUCT_LITERAL
// Every OTHER implicit allocation the codebase funnels through
// codegen_emit_alloc (interface boxing, map boxing, closure env, go-arg
// boxing, slice-literal backing, ...) is out of scope for this cut: this
// module simply does not classify those nodes as sites, so a 7c lookup
// finds no decision for them -> miss -> heap (conservative, safe).
//
// See docs/superpowers/specs/2026-07-07-arena-7b-block-escape-decision-
// design.md for the full design (escape sinks, block-local classification,
// nested-arena-block handling, and the 15-row test matrix this module must
// satisfy).
//
// Explicitly NOT this module's job: no codegen change, no arena free, no
// promotion (that is task 7c, which CONSUMES this module's decisions —
// arenas stay transparent/never-freed until then). This module does not
// mutate the AST it analyzes.

#include "ast.h"
#include "param_escape.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct BlockEscapeDecision {
    ASTNode* site;          // the alloc-site node (new-call or &composite); borrowed
    bool     escapes_block; // true = must heap; false = arena-eligible
} BlockEscapeDecision;

typedef struct BlockEscapeResult {
    BlockEscapeDecision* decisions; // one per site inside an arena block, pre-order
    size_t count;
} BlockEscapeResult;

// Analyze every allocation site lexically inside an `arena { }` block,
// reachable from `program`'s top-level function/method declarations. Pure:
// does not mutate `program`. `summaries` (typically from
// param_escape_analyze) may be NULL — every call is then treated as
// external/retaining, which is still sound (just more conservative).
// Returns NULL ONLY on allocation failure; a NULL/malformed `program`
// yields a valid, empty (count == 0) result.
BlockEscapeResult* block_escape_analyze(ASTNode* program, const ParamEscapeResult* summaries);
void block_escape_result_free(BlockEscapeResult* result);

// 7c's query: does the value produced at `site` escape its enclosing arena
// block? TRUE on a miss (site is unknown/NULL, or was never classified as
// an allocation site — e.g. it lies outside every arena block) —
// conservative: the caller then keeps the allocation on its default (heap)
// path. This miss-behaviour is part of the soundness contract, not an
// oversight.
bool block_escape_site_escapes(const BlockEscapeResult* result, const ASTNode* site);

#endif // BLOCK_ESCAPE_H
