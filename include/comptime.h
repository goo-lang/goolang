#ifndef GOO_COMPTIME_H
#define GOO_COMPTIME_H

#include "ast.h"
#include "types.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// Compile-Time Values
// =============================================================================

typedef enum {
    COMPTIME_INT,
    COMPTIME_FLOAT,
    COMPTIME_BOOL,
    COMPTIME_STRING,
    COMPTIME_NIL,
    COMPTIME_ARRAY,
    COMPTIME_ERROR,
} ComptimeValueKind;

typedef struct ComptimeValue {
    ComptimeValueKind kind;
    union {
        int64_t int_val;
        double float_val;
        bool bool_val;
        char* string_val;
        struct {
            struct ComptimeValue** elements;
            size_t length;
        } array_val;
    } data;
} ComptimeValue;

// =============================================================================
// Compile-Time Environment — variable bindings during evaluation
// =============================================================================

typedef struct ComptimeBinding {
    char* name;
    ComptimeValue value;
    bool is_const;
    struct ComptimeBinding* next;
} ComptimeBinding;

typedef struct ComptimeScope {
    ComptimeBinding* bindings;
    struct ComptimeScope* parent;
} ComptimeScope;

// =============================================================================
// Compile-Time Function Registry
// =============================================================================

typedef struct ComptimeFunc {
    char* name;
    ASTNode* params;               // Parameter list from AST
    ASTNode* body;                 // Function body from AST
    struct ComptimeFunc* next;
} ComptimeFunc;

// =============================================================================
// Compile-Time Interpreter
// =============================================================================

typedef struct ComptimeInterpreter {
    ComptimeScope* current_scope;
    ComptimeFunc* functions;

    // Error tracking
    char* error_message;
    Position error_pos;
    bool has_error;

    // Limits
    size_t max_iterations;         // Loop iteration limit (prevent infinite loops)
    size_t max_recursion;          // Recursion depth limit
    size_t current_recursion;

    // Statistics
    struct {
        size_t expressions_evaluated;
        size_t functions_called;
        size_t constants_produced;
    } stats;
} ComptimeInterpreter;

// =============================================================================
// API
// =============================================================================

// Interpreter lifecycle
ComptimeInterpreter* comptime_interpreter_new(void);
void comptime_interpreter_free(ComptimeInterpreter* interp);

// Value operations
ComptimeValue comptime_value_int(int64_t val);
ComptimeValue comptime_value_float(double val);
ComptimeValue comptime_value_bool(bool val);
ComptimeValue comptime_value_string(const char* val);
ComptimeValue comptime_value_nil(void);
ComptimeValue comptime_value_error(const char* msg);
void comptime_value_free(ComptimeValue* val);
bool comptime_value_is_truthy(const ComptimeValue* val);
char* comptime_value_to_string(const ComptimeValue* val);

// Scope management
void comptime_push_scope(ComptimeInterpreter* interp);
void comptime_pop_scope(ComptimeInterpreter* interp);
bool comptime_set_variable(ComptimeInterpreter* interp, const char* name,
                           ComptimeValue value, bool is_const);
ComptimeValue* comptime_lookup_variable(ComptimeInterpreter* interp, const char* name);

// Function registration
void comptime_register_function(ComptimeInterpreter* interp, const char* name,
                                ASTNode* params, ASTNode* body);

// Evaluation — the core of the interpreter
ComptimeValue comptime_eval_expression(ComptimeInterpreter* interp, ASTNode* expr);
bool comptime_eval_statement(ComptimeInterpreter* interp, ASTNode* stmt);
bool comptime_eval_block(ComptimeInterpreter* interp, ASTNode* block);

// Entry point — evaluate a comptime block and produce constants
bool comptime_process_block(ComptimeInterpreter* interp, ComptimeBlockNode* block);

// Error checking
bool comptime_has_error(ComptimeInterpreter* interp);
const char* comptime_get_error(ComptimeInterpreter* interp);

#endif // GOO_COMPTIME_H
