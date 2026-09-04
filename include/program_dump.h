#ifndef PROGRAM_DUMP_H
#define PROGRAM_DUMP_H

#include "ast.h"
#include "release_decision.h"
#include <stdio.h>
#include <stddef.h>

typedef enum {
    PROGRAM_DUMP_PARSE,   // after parsing: no type ids, plan is null
    PROGRAM_DUMP_TYPED,   // after type_check_program_files: type ids + plan
} ProgramDumpStage;

// Writes the dump for `nfiles` parsed (or checked) files. `plans` may be NULL,
// or an array of `nfiles` ReleasePlan* where an entry may be NULL (ARC off).
// Aborts, naming the kind, on any AST or type kind it does not handle: the
// fixture gate (scripts/program_dump_probe.sh) is the coverage proof.
void program_dump_write(FILE* out, ASTNode** files, const char** filenames,
                        size_t nfiles, ReleasePlan** plans, ProgramDumpStage stage);

#endif
