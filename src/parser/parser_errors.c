#include "parser/parser_errors.h"
#include "lexer.h"
#include "token.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Defined in src/parser/lexer_bridge.c; each consumer translation unit
// declares its own extern (see parser.y's own declaration, which
// parser.tab.c's generated code uses).
extern Lexer* current_lexer;

// Global parser error state
ParserErrorState* g_parser_error_state = NULL;

// Initialize parser error handling
void parser_error_init(ErrorContext* ctx) {
    if (g_parser_error_state) {
        parser_error_cleanup();
    }
    
    g_parser_error_state = (ParserErrorState*)xcalloc(1, sizeof(ParserErrorState));
    if (!g_parser_error_state) return;
    
    g_parser_error_state->error_ctx = ctx;
    g_parser_error_state->current_token = NULL;
    g_parser_error_state->in_recovery = false;
    g_parser_error_state->recovery_depth = 0;
    g_parser_error_state->sync_token_count = 0;
    
    // Default synchronization tokens
    parser_add_sync_token(TOKEN_SEMICOLON);
    parser_add_sync_token(TOKEN_RBRACE);
    parser_add_sync_token(TOKEN_EOF);
}

// Cleanup parser error handling
void parser_error_cleanup(void) {
    if (g_parser_error_state) {
        free(g_parser_error_state);
        g_parser_error_state = NULL;
    }
}

// Convert token to source location
SourceLocation token_to_source_location(const Token* token) {
    if (!token) {
        return empty_source_location();
    }
    
    return make_source_location(
        token->pos.filename ? token->pos.filename : "<unknown>",
        token->pos.line,
        token->pos.column,
        token->pos.offset,
        token->length
    );
}

// Report parser error
void parser_error_report(ErrorCode code, const char* message, const char* hint) {
    if (!g_parser_error_state || !g_parser_error_state->error_ctx) return;
    
    SourceLocation loc = empty_source_location();
    if (g_parser_error_state->current_token) {
        loc = token_to_source_location(g_parser_error_state->current_token);
    }
    
    if (hint) {
        report_error_with_hint(g_parser_error_state->error_ctx, code, 
                              message, hint, loc);
    } else {
        report_error(g_parser_error_state->error_ctx, code, message, loc);
    }
}

// Report parser error at specific token
void parser_error_report_at_token(ErrorCode code, const char* message,
                                  const char* hint, Token* token) {
    if (!g_parser_error_state || !g_parser_error_state->error_ctx) return;
    
    SourceLocation loc = token_to_source_location(token);
    
    if (hint) {
        report_error_with_hint(g_parser_error_state->error_ctx, code,
                              message, hint, loc);
    } else {
        report_error(g_parser_error_state->error_ctx, code, message, loc);
    }
}

// Enter recovery mode
bool parser_enter_recovery_mode(void) {
    if (!g_parser_error_state) return false;
    
    if (!g_parser_error_state->in_recovery) {
        g_parser_error_state->in_recovery = true;
        g_parser_error_state->recovery_depth = 0;
        
        // Enter panic mode in error context
        if (g_parser_error_state->error_ctx) {
            enter_panic_mode(g_parser_error_state->error_ctx, 0);
        }
    }
    
    g_parser_error_state->recovery_depth++;
    return true;
}

// Exit recovery mode
void parser_exit_recovery_mode(void) {
    if (!g_parser_error_state || !g_parser_error_state->in_recovery) return;
    
    g_parser_error_state->recovery_depth--;
    
    if (g_parser_error_state->recovery_depth <= 0) {
        g_parser_error_state->in_recovery = false;
        g_parser_error_state->recovery_depth = 0;
        
        // Exit panic mode in error context
        if (g_parser_error_state->error_ctx) {
            exit_panic_mode(g_parser_error_state->error_ctx);
        }
    }
}

// Check if in recovery mode
bool parser_is_in_recovery(void) {
    return g_parser_error_state && g_parser_error_state->in_recovery;
}

// Add synchronization token
void parser_add_sync_token(int token_type) {
    if (!g_parser_error_state) return;
    
    if (g_parser_error_state->sync_token_count < 
        sizeof(g_parser_error_state->synchronization_tokens) / sizeof(int)) {
        g_parser_error_state->synchronization_tokens[g_parser_error_state->sync_token_count++] = token_type;
    }
}

// Check if current token is a sync token
static bool is_sync_token(int token_type) {
    if (!g_parser_error_state) return false;
    
    for (size_t i = 0; i < g_parser_error_state->sync_token_count; i++) {
        if (g_parser_error_state->synchronization_tokens[i] == token_type) {
            return true;
        }
    }
    return false;
}

// Sync to next synchronization token
bool parser_sync_to_token(void) {
    if (!g_parser_error_state || !g_parser_error_state->current_token) {
        return false;
    }
    
    // If we're already at a sync token, we're done
    if (is_sync_token(g_parser_error_state->current_token->type)) {
        return true;
    }
    
    // Skip tokens until we find a sync token or EOF
    while (g_parser_error_state->current_token && 
           g_parser_error_state->current_token->type != TOKEN_EOF) {
        
        // Advance to next token (this would be implemented by the lexer)
        // For now, this is a placeholder
        break; // TODO: Implement token advancement
        
        if (is_sync_token(g_parser_error_state->current_token->type)) {
            return true;
        }
    }
    
    return g_parser_error_state->current_token && 
           g_parser_error_state->current_token->type != TOKEN_EOF;
}

// Skip to sync point (statement level)
bool parser_skip_to_sync_point(void) {
    return parser_sync_to_token();
}

// Sync to statement end
void parser_sync_to_statement_end(void) {
    parser_add_sync_token(TOKEN_SEMICOLON);
    parser_sync_to_token();
}

// Sync to block end
void parser_sync_to_block_end(void) {
    parser_add_sync_token(TOKEN_RBRACE);
    parser_sync_to_token();
}

// Sync to expression end
void parser_sync_to_expression_end(void) {
    parser_add_sync_token(TOKEN_SEMICOLON);
    parser_add_sync_token(TOKEN_COMMA);
    parser_add_sync_token(TOKEN_RPAREN);
    parser_sync_to_token();
}

// Convert token type to string (helper for error messages)
const char* token_type_to_string(int token_type) {
    switch (token_type) {
        case TOKEN_IDENT:         return "identifier";
        case TOKEN_INT:           return "integer";
        case TOKEN_FLOAT:         return "float";
        case TOKEN_STRING:        return "string";
        case TOKEN_CHAR:          return "character";
        case TOKEN_SEMICOLON:     return "';'";
        case TOKEN_LPAREN:        return "'('";
        case TOKEN_RPAREN:        return "')'";
        case TOKEN_LBRACE:        return "'{'";
        case TOKEN_RBRACE:        return "'}'";
        case TOKEN_COMMA:         return "','";
        case TOKEN_DOT:           return "'.'";
        case TOKEN_ASSIGN:        return "'='";
        case TOKEN_PLUS:          return "'+'";
        case TOKEN_MINUS:         return "'-'";
        case TOKEN_MULTIPLY:      return "'*'";
        case TOKEN_DIVIDE:        return "'/'";
        case TOKEN_IF:            return "'if'";
        case TOKEN_ELSE:          return "'else'";
        case TOKEN_FOR:           return "'for'";
        case TOKEN_RETURN:        return "'return'";
        case TOKEN_VAR:           return "'var'";
        case TOKEN_FUNC:          return "'func'";
        case TOKEN_EOF:           return "end of file";
        default:                  return "unknown token";
    }
}

// Bison-invoked error hook (declared in include/parser.h and, for
// parser.y's own translation unit, re-declared extern in its prologue).
// Formerly defined in parser.y's epilogue; moved here as its natural home
// alongside the rest of the parser's error-handling machinery.
void yyerror(const char* msg) {
    if (current_lexer) {
        fprintf(stderr, "Parse error at %s:%d:%d: %s\n",
                current_lexer->filename ? current_lexer->filename : "<stdin>",
                current_lexer->pos.line,
                current_lexer->pos.column,
                msg);
    } else {
        fprintf(stderr, "Parse error: %s\n", msg);
    }
}