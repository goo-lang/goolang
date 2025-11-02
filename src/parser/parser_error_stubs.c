/**
 * Parser Error Reporting Stubs
 *
 * Temporary implementations to support TDD.
 * These will be replaced with proper error handling later.
 */

#include <stdio.h>
#include <stdlib.h>

// Stub type definitions (to match what parser_errors.c expects)
typedef struct {
    const char* filename;
    int line;
    int column;
} SourceLocation;

/**
 * Create an empty source location
 */
SourceLocation empty_source_location(void) {
    SourceLocation loc = {
        .filename = "<unknown>",
        .line = 0,
        .column = 0
    };
    return loc;
}

/**
 * Create a source location
 */
SourceLocation make_source_location(const char* filename, int line, int column) {
    SourceLocation loc = {
        .filename = filename,
        .line = line,
        .column = column
    };
    return loc;
}

/**
 * Report an error
 */
void report_error(SourceLocation loc, const char* message) {
    fprintf(stderr, "Error at %s:%d:%d: %s\n",
            loc.filename, loc.line, loc.column, message);
}

/**
 * Report an error with a hint
 */
void report_error_with_hint(SourceLocation loc, const char* message, const char* hint) {
    fprintf(stderr, "Error at %s:%d:%d: %s\n",
            loc.filename, loc.line, loc.column, message);
    if (hint) {
        fprintf(stderr, "Hint: %s\n", hint);
    }
}

/**
 * Enter panic mode (error recovery)
 */
void enter_panic_mode(void) {
    // TODO: Implement proper panic mode
}

/**
 * Exit panic mode (error recovery)
 */
void exit_panic_mode(void) {
    // TODO: Implement proper panic mode exit
}
