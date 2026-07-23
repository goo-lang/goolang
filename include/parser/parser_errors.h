#ifndef GOO_PARSER_ERRORS_H
#define GOO_PARSER_ERRORS_H

#include "errors/error.h"
#include "token.h"

// Parser error recovery state
typedef struct ParserErrorState {
    ErrorContext* error_ctx;
    Token* current_token;
    bool in_recovery;
    int recovery_depth;
    int synchronization_tokens[10]; // Tokens to sync on
    size_t sync_token_count;
} ParserErrorState;

// Global parser error state
extern ParserErrorState* g_parser_error_state;

// Parser error recovery functions
void parser_error_init(ErrorContext* ctx);
void parser_error_cleanup(void);
void parser_error_report(ErrorCode code, const char* message, const char* hint);
void parser_error_report_at_token(ErrorCode code, const char* message, 
                                  const char* hint, Token* token);

// Error recovery functions
bool parser_enter_recovery_mode(void);
void parser_exit_recovery_mode(void);
bool parser_is_in_recovery(void);
void parser_add_sync_token(int token_type);
bool parser_sync_to_token(void);
bool parser_skip_to_sync_point(void);

// Synchronization helpers
void parser_sync_to_statement_end(void);
void parser_sync_to_block_end(void);
void parser_sync_to_expression_end(void);

// Error reporting macros for parser
#define PARSER_ERROR(code, msg) \
    parser_error_report((code), (msg), NULL)

#define PARSER_ERROR_WITH_HINT(code, msg, hint) \
    parser_error_report((code), (msg), (hint))

#define PARSER_ERROR_AT_TOKEN(code, msg, token) \
    parser_error_report_at_token((code), (msg), NULL, (token))

#define PARSER_ERROR_AT_TOKEN_WITH_HINT(code, msg, hint, token) \
    parser_error_report_at_token((code), (msg), (hint), (token))

// Recovery macros
#define TRY_RECOVERY(action) \
    do { \
        if (parser_is_in_recovery()) { \
            if (!(action)) { \
                parser_skip_to_sync_point(); \
                return NULL; \
            } \
        } \
    } while (0)

#define SYNC_ON_ERROR(expected_token, error_msg) \
    do { \
        if (parser_is_in_recovery()) { \
            PARSER_ERROR(ERROR_UNEXPECTED_TOKEN, error_msg); \
            parser_add_sync_token(expected_token); \
            if (!parser_sync_to_token()) { \
                return NULL; \
            } \
        } \
    } while (0)

#define RECOVER_OR_FAIL(condition, error_code, error_msg) \
    do { \
        if (!(condition)) { \
            PARSER_ERROR(error_code, error_msg); \
            if (!parser_enter_recovery_mode()) { \
                return NULL; \
            } \
        } \
    } while (0)

// Common parser error patterns
#define EXPECT_TOKEN(token_type, error_msg) \
    RECOVER_OR_FAIL(current_token && current_token->type == (token_type), \
                    ERROR_UNEXPECTED_TOKEN, error_msg)

#define EXPECT_SEMICOLON() \
    EXPECT_TOKEN(TOKEN_SEMICOLON, "Expected ';' after statement")

#define EXPECT_CLOSING_PAREN() \
    EXPECT_TOKEN(TOKEN_RPAREN, "Expected ')' to close parentheses")

#define EXPECT_CLOSING_BRACE() \
    EXPECT_TOKEN(TOKEN_RBRACE, "Expected '}' to close block")

#define EXPECT_IDENTIFIER(var_name) \
    do { \
        EXPECT_TOKEN(TOKEN_IDENTIFIER, "Expected identifier"); \
        var_name = current_token->value.string_value; \
    } while (0)

// Parser validation macros
#define VALIDATE_AST_NODE(node, error_msg) \
    do { \
        if (!(node)) { \
            PARSER_ERROR(ERROR_INTERNAL, error_msg); \
            if (parser_is_in_recovery()) { \
                parser_skip_to_sync_point(); \
            } \
            return NULL; \
        } \
    } while (0)

#define VALIDATE_EXPRESSION(expr, error_msg) \
    VALIDATE_AST_NODE(expr, error_msg)

#define VALIDATE_STATEMENT(stmt, error_msg) \
    VALIDATE_AST_NODE(stmt, error_msg)

// Memory allocation with error handling
#define PARSER_ALLOC(type) \
    ({ \
        type* ptr = (type*)malloc(sizeof(type)); \
        if (!ptr) { \
            PARSER_ERROR(ERROR_OUT_OF_MEMORY, "Out of memory during parsing"); \
            return NULL; \
        } \
        memset(ptr, 0, sizeof(type)); \
        ptr; \
    })

#define PARSER_ALLOC_ARRAY(type, count) \
    ({ \
        type* ptr = (type*)calloc((count), sizeof(type)); \
        if (!ptr) { \
            PARSER_ERROR(ERROR_OUT_OF_MEMORY, "Out of memory during parsing"); \
            return NULL; \
        } \
        ptr; \
    })

// Error context helpers
#define GET_PARSER_ERROR_COUNT() \
    (g_parser_error_state ? get_error_count(g_parser_error_state->error_ctx) : 0)

#define GET_PARSER_WARNING_COUNT() \
    (g_parser_error_state ? get_warning_count(g_parser_error_state->error_ctx) : 0)

#define PARSER_HAS_ERRORS() \
    (g_parser_error_state && has_errors(g_parser_error_state->error_ctx))

#define PARSER_HAS_FATAL_ERRORS() \
    (g_parser_error_state && has_fatal_errors(g_parser_error_state->error_ctx))

// Token position helpers
SourceLocation token_to_source_location(const Token* token);
const char* token_type_to_string(int token_type);

// Bison error hook (also declared in include/parser.h for callers outside
// the parser subsystem, and re-declared extern in parser.y's own prologue
// for parser.tab.c's generated yyparse()). Defined in parser_errors.c.
void yyerror(const char* msg);

#endif // GOO_PARSER_ERRORS_H