#ifndef RETURNS_FRESH_H
#define RETURNS_FRESH_H

// returns_fresh: does the caller OWN every value a function returns?
// (finding C2)
//
// Fills ParamEscapeSummary.returns_fresh, which param_escape_analyze leaves
// false. A separate unit because the proof reads local_escape's reasons, and
// local_escape already depends on param_escape: computing it inside
// param_escape_analyze made the two depend on each other.
//
// `pe` and `le` must come from the same `program`, with `le` computed from
// `pe` (local_escape_analyze(program, pe)). local_escape never reads
// returns_fresh, so computing `le` before this call changes nothing.
//
// FAILS CLOSED. Every summary stays false -- and false only leaks -- when `pe`
// or `le` is NULL, when pe->fail_closed is set, or when the program's
// functions do not line up with pe's summaries by count and name.

#include "ast.h"
#include "local_escape.h"
#include "param_escape.h"

void returns_fresh_compute(ASTNode* program, ParamEscapeResult* pe,
                           const LocalEscapeResult* le);

#endif // RETURNS_FRESH_H
