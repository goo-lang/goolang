#include "lexer.h"
#include "token.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

// Count of lexical errors (TOKEN_ERROR) produced while lexing the current
// input. The Bison bridge (src/parser/lexer_bridge.c) maps TOKEN_ERROR to an
// unknown token and silently skips it, so a malformed token can vanish and the
// surrounding program parse + compile to a running binary with no diagnostic.
// The type checker gates on this count (see type_check_program) and refuses to
// emit code when it is non-zero, turning the silent drop into a clean rejection.
// Reset at the start of each lex run in lexer_new so a long-lived host (REPL/
// LSP) does not carry a stale count between parses.
int goo_lexer_error_count = 0;

Lexer* lexer_new(const char* input, const char* filename) {
    goo_lexer_error_count = 0;
    Lexer* lexer = xmalloc(sizeof(Lexer));
    if (!lexer) return NULL;
    
    lexer->input = input;
    lexer->input_length = strlen(input);
    lexer->position = 0;
    lexer->read_position = 0;
    lexer->ch = 0;
    lexer->filename = filename;
    
    // Initialize position
    lexer->pos.line = 1;
    lexer->pos.column = 1;
    lexer->pos.offset = 0;
    lexer->pos.filename = filename;
    lexer->prev_token_type = TOKEN_SEMICOLON; // as if after a separator
    lexer->asi_depth = 0;
    lexer->literal_error = NULL;
    memset(lexer->asi_ctx, 0, sizeof(lexer->asi_ctx));

    // Read first character
    lexer_read_char(lexer);
    
    return lexer;
}

void lexer_free(Lexer* lexer) {
    if (lexer) {
        free(lexer);
    }
}

// Invariant: on return, lexer->pos describes lexer->ch — pos.column is the
// 1-based column OF the current character, not of the one after it.
//
// That requires advancing past the character being LEFT BEHIND, before
// loading the next one. Advancing based on the character just LOADED (the
// previous shape here) was wrong twice over: it made every column one too
// large, and it moved line/column onto the NEXT line the moment ch became
// '\n' — so any position captured while ch == '\n', which is exactly what
// the ASI path in lexer_next_token does, described the newline's successor
// rather than the newline itself. That second effect is why simply seeding
// column at 0 does not work: it turns the same captured position into
// column 0, which is not a valid 1-based column.
void lexer_read_char(Lexer* lexer) {
    // Step over the character currently in ch. Guarded on ch != 0 so the
    // priming read from lexer_new (ch == 0, nothing consumed yet) leaves the
    // first character sitting at 1:1, and so repeated reads at EOF do not
    // walk the column off the end.
    if (lexer->ch == '\n') {
        lexer->pos.line++;
        lexer->pos.column = 1;
    } else if (lexer->ch != 0) {
        lexer->pos.column++;
    }

    if (lexer->read_position >= lexer->input_length) {
        lexer->ch = 0; // EOF
    } else {
        lexer->ch = lexer->input[lexer->read_position];
    }

    lexer->position = lexer->read_position;
    lexer->read_position++;
    lexer->pos.offset = lexer->position;
}

char lexer_peek_char(Lexer* lexer) {
    if (lexer->read_position >= lexer->input_length) {
        return 0;
    }
    return lexer->input[lexer->read_position];
}

void lexer_skip_whitespace(Lexer* lexer) {
    while (lexer_is_whitespace(lexer->ch) && lexer->ch != '\n') {
        lexer_read_char(lexer);
    }
}

// True if a token can end a statement/expression (so a following newline may
// terminate the statement). Used for targeted automatic semicolon insertion.
static int token_ends_value(TokenType t) {
    switch (t) {
        case TOKEN_IDENT:
        case TOKEN_INT:
        case TOKEN_FLOAT:
        case TOKEN_STRING:
        case TOKEN_CHAR:
        case TOKEN_TRUE:
        case TOKEN_FALSE:
        case TOKEN_NIL:         // P5.10: `= nil` ends a statement (Go: nil is
                                // an identifier, so Go's rule covers it too)
        case TOKEN_RPAREN:
        case TOKEN_RBRACKET:
        case TOKEN_RBRACE:
        case TOKEN_INCREMENT:   // x++  — postfix, value-ending
        case TOKEN_DECREMENT:   // x--  — postfix, value-ending
            return 1;
        default:
            return 0;
    }
}

// (P5.10: char_starts_continuation_op was deleted — the generalized rule-1
// ASI below no longer discriminates by the next line's first character
// beyond the ')' / '}' / `...` / `else` leniency exceptions.)

// P5.10: true when the upcoming characters spell the keyword `else` at a
// word boundary — the one keyword allowed to CONTINUE the previous
// statement across a newline (`}` <nl> `else {`), a pinned Goo leniency
// (asi_else_probe; Go itself rejects that shape). Non-consuming bounded
// lookahead over the raw input buffer.
static int lexer_line_starts_with_else(Lexer* lexer) {
    if (lexer->ch != 'e') return 0;
    if (lexer->read_position + 2 >= lexer->input_length) return 0;
    if (lexer->input[lexer->read_position] != 'l' ||
        lexer->input[lexer->read_position + 1] != 's' ||
        lexer->input[lexer->read_position + 2] != 'e') return 0;
    if (lexer->read_position + 3 < lexer->input_length) {
        char after = lexer->input[lexer->read_position + 3];
        if ((after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') ||
            (after >= '0' && after <= '9') || after == '_') return 0;
    }
    return 1;
}

// Hex-digit value for `\xNN` string escapes, or -1 if `c` is not a hex digit.
static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Validate a numeric literal the scanner already cut out, so an overflowing
// or malformed one becomes a positioned TOKEN_ERROR instead of reaching the
// bridge's strtoull/atof, which wrap to a wrong value or yield inf without a
// word. Returns NULL when the literal is well formed. The scanner only
// accepts digit characters, so this judges digit sets, emptiness, and range.
// Only a value beyond uint64 is an overflow here: a constant between INT64_MAX
// and UINT64_MAX (0xFFFFFFFFFFFFFFFF) is legal when the context is unsigned,
// and that contextual judgement belongs to the type checker, not the lexer.
static const char* lexer_number_error(const char* s, int is_float) {
    static char msg[48]; // one lexer thread at a time; token_new copies it
    unsigned base = 10;
    const char* digits = s;
    const char* kind = "decimal";
    // An explicit prefix counts for a float too, or strtod reads `0b1.5` as 0
    // and `0x1.5` as a hex float. The legacy 0777 octal form is an integer
    // only: `01.5` is the decimal float 1.5, as in Go.
    if (s[0] == '0' && s[1]) {
        char c = s[1] | 0x20; // fold case
        if (c == 'x') { base = 16; digits = s + 2; kind = "hexadecimal"; }
        else if (c == 'b') { base = 2; digits = s + 2; kind = "binary"; }
        else if (c == 'o') { base = 8; digits = s + 2; kind = "octal"; }
        else if (!is_float) { base = 8; digits = s + 1; kind = "octal"; }
    }

    // A binary or octal literal has no exponent in Go. Hex is exempt: its
    // digits include 'e', so `0x1e5` is the integer 485.
    if ((base == 2 || base == 8) && strpbrk(digits, "eE")) {
        return "'e' exponent requires decimal mantissa";
    }
    if ((base == 2 || base == 8) && strchr(digits, '.')) {
        snprintf(msg, sizeof(msg), "invalid radix point in %s literal", kind);
        return msg;
    }
    // The scanner has no 'p' exponent, so a hex float is never complete.
    if (base == 16 && is_float) {
        return "hexadecimal mantissa requires a 'p' exponent";
    }

    if (is_float) {
        const char* e = strpbrk(s, "eE");
        if (e) {
            e++;
            if (*e == '+' || *e == '-') e++;
            if (!lexer_is_digit(*e)) return "exponent has no digits";
        }
        double v = strtod(s, NULL);
        // Underflow to 0 is a representable constant; only inf is an error.
        if (isinf(v)) return "floating-point constant overflow";
        return NULL;
    }

    if (base != 10 && digits != s + 1 && !*digits) {
        snprintf(msg, sizeof(msg), "%s literal has no digits", kind);
        return msg;
    }
    uint64_t v = 0;
    int overflow = 0;
    for (const char* p = digits; *p; p++) {
        int d = hex_digit_value(*p);
        if (d < 0 || (unsigned)d >= base) {
            snprintf(msg, sizeof(msg), "invalid digit '%c' in %s literal", *p, kind);
            return msg;
        }
        if (v > (UINT64_MAX - (uint64_t)d) / base) overflow = 1;
        else v = v * base + (uint64_t)d;
    }
    return overflow ? "integer constant overflow" : NULL;
}

// Decode one escape sequence. `body` points just past the backslash and has
// `avail` bytes. `quote` is the delimiter that may be escaped ('"' in a
// string, '\'' in a rune). On success returns 1, sets *consumed (bytes after
// the backslash), and *val; *is_byte is 1 for \x and octal (a raw byte) and 0
// for a Unicode code point. On failure returns 0 with *err set to the Go
// error text. One routine serves strings and runes so the two cannot drift.
static int lexer_decode_escape(const char* body, size_t avail, char quote,
                               uint32_t* val, int* is_byte, size_t* consumed,
                               const char** err) {
    *is_byte = 0;
    if (avail == 0) { *err = "escape sequence not terminated"; return 0; }
    char c = body[0];
    *consumed = 1;
    switch (c) {
        case 'a': *val = '\a'; return 1;
        case 'b': *val = '\b'; return 1;
        case 'f': *val = '\f'; return 1;
        case 'n': *val = '\n'; return 1;
        case 'r': *val = '\r'; return 1;
        case 't': *val = '\t'; return 1;
        case 'v': *val = '\v'; return 1;
        case '\\': *val = '\\'; return 1;
        default: break;
    }
    if (c == quote) { *val = (uint32_t)(unsigned char)c; return 1; }

    size_t ndig = 0;
    unsigned base = 16;
    if (c == 'x') ndig = 2;
    else if (c == 'u') ndig = 4;
    else if (c == 'U') ndig = 8;
    else if (c >= '0' && c <= '7') { ndig = 3; base = 8; }
    else { *err = "unknown escape sequence"; return 0; }

    size_t start = (base == 8) ? 0 : 1; // an octal escape's first digit is c itself
    if (avail < start + ndig) {
        *err = (base == 8) ? "invalid character in octal escape"
                           : "escape sequence not terminated";
        return 0;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < ndig; i++) {
        int d = hex_digit_value(body[start + i]);
        if (d < 0 || (unsigned)d >= base) {
            *err = (base == 8) ? "invalid character in octal escape"
                               : "invalid character in hexadecimal escape";
            return 0;
        }
        v = v * base + (uint32_t)d;
    }
    *consumed = start + ndig;
    if (base == 8 && v > 255) { *err = "octal escape value > 255"; return 0; }
    if (c == 'u' || c == 'U') {
        if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
            *err = "escape sequence is invalid Unicode code point";
            return 0;
        }
    } else {
        *is_byte = 1;
    }
    *val = v;
    return 1;
}

// Append code point `cp` (already validated) to `out` as UTF-8; returns bytes written.
static size_t utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    // lexer_decode_escape refuses a code point above U+10FFFF, so a larger one
    // here would write a 4-byte sequence that no UTF-8 decoder accepts.
    GOO_ASSERT(cp <= 0x10FFFF);
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

Token* lexer_next_token(Lexer* lexer) {
    Token* token = NULL;
    Position current_pos = lexer->pos;

    // P0-3: iterate rather than tail-recurse over skipped newlines. A run of
    // blank lines previously recursed once per line (the `return
    // lexer_next_token(lexer)` in the '\n' case), overflowing the stack on
    // large inputs (1,000,000 blank lines -> SIGSEGV). The loop body is kept
    // at the original indentation to keep this a minimal, reviewable diff; the
    // only behavioural change is recursion -> `continue`. Every non-newline
    // path still falls through to the single `return token` that closes it.
    for (;;) {
    token = NULL;

    lexer_skip_whitespace(lexer);
    current_pos = lexer->pos; // Update position after skipping whitespace

    switch (lexer->ch) {
        case 0:
            token = token_new(TOKEN_EOF, NULL, 0, current_pos);
            break;
            
        case '\n':
            // Targeted automatic semicolon insertion. Newlines are normally
            // skipped (the grammar tolerates statements without semicolons when
            // the next token can't continue the previous one). But when the
            // previous token ends a value AND the next line begins with a
            // binary-operator char, the parser would greedily join them
            // (e.g. `p := &x` <nl> `*p = v` -> `&x * p`). Insert a semicolon in
            // exactly that case so the statement terminates at the newline.
            lexer_read_char(lexer); // consume '\n'
            while (lexer->ch == ' ' || lexer->ch == '\t' || lexer->ch == '\r') {
                lexer_read_char(lexer);
            }
            // Part 1 — keyword terminators (unconditional). Go always ends the
            // statement after these keywords; their operand (a return
            // expression, a break/continue label) must sit on the same line, so
            // a newline here is unambiguously a boundary. This is separate from
            // the value-ending guard below because `return` <nl> `expr` has an
            // identifier (not an operator/bracket) starting line 2, which the
            // next-char guard would not catch.
            if (lexer->prev_token_type == TOKEN_RETURN ||
                lexer->prev_token_type == TOKEN_BREAK ||
                lexer->prev_token_type == TOKEN_CONTINUE ||
                lexer->prev_token_type == TOKEN_FALLTHROUGH) {
                lexer->prev_token_type = TOKEN_SEMICOLON;
                return token_new(TOKEN_SEMICOLON, ";", 1, current_pos);
            }
            // Part 2 — generalized rule-1 ASI (P5.10). Go's actual rule: a
            // newline after a value-ending token terminates the statement,
            // period. This replaced the old hazard-targeted guard (insert
            // only before continuation ops / `(` `[` `.` / `<-`) when the
            // grammar moved to Go-shaped SEMICOLON-terminated statement
            // lists — the parser now NEEDS the terminator between any two
            // statements, so the lexer supplies it at every value-ending
            // newline. Two deliberate exceptions, both pinned by golden
            // fixtures (Goo is more lenient than Go here — Go would insert
            // and then reject):
            //   ')'  — a multi-line call's closing paren may follow the last
            //          argument without a trailing comma (asi_multiline_probe).
            //   '}'  — a composite literal's closing brace may follow the last
            //          element without a trailing comma, and a block's final
            //          statement needs no terminator (the grammar's final_stmt
            //          arm consumes the bare form).
            // EOF (ch == 0) deliberately gets the semicolon: the top-level
            // decl arms carry member-attached trailing-SEMICOLON tolerance.
            // Two more pinned Goo leniencies survive from the old targeted
            // rule (both are loud Go errors, tolerated here by fixtures):
            //   `...`  — a spread flowing onto its own line (`sum(a` <nl>
            //            `...)`, asi_spread_probe). There is no `..` operator
            //            in Goo, so the two-char peek is unambiguous.
            //   `else` — an else on the line after the closing `}`
            //            (asi_else_probe). Word-boundary checked so an
            //            identifier merely STARTING with "else" still gets
            //            the terminator.
            if (token_ends_value(lexer->prev_token_type) &&
                lexer->ch != ')' && lexer->ch != '}' &&
                !(lexer->ch == '.' && lexer_peek_char(lexer) == '.') &&
                !lexer_line_starts_with_else(lexer)) {
                lexer->prev_token_type = TOKEN_SEMICOLON;
                return token_new(TOKEN_SEMICOLON, ";", 1, current_pos);
            }
            // Struct/interface-body and var-group ASI (embedding; method
            // specs; grouped var specs): inside a struct body, interface
            // body, or `var ( ... )` group, a newline after a value-ending
            // token ends the member — Go's semicolon rule scoped to these
            // bodies, so a 1-token embedded field (`Base`), a void method
            // spec (`Inc()`), or a result-less func-typed var spec (`f
            // func(int)`) stops at the line break instead of absorbing the
            // next line's token (e.g. a following method/spec name mistaken
            // for a return type via func_result's identifier-starting FIRST
            // set).
            if (lexer->asi_depth > 0 &&
                lexer->asi_depth <= (int)sizeof(lexer->asi_ctx) &&
                lexer->asi_ctx[lexer->asi_depth - 1] &&
                token_ends_value(lexer->prev_token_type)) {
                lexer->prev_token_type = TOKEN_SEMICOLON;
                return token_new(TOKEN_SEMICOLON, ";", 1, current_pos);
            }
            continue; // otherwise just get the next token (was: tail recursion)
            
        // Single character tokens
        case '(':
            token = token_new(TOKEN_LPAREN, "(", 1, current_pos);
            // Var-group-scoped ASI: a '(' immediately following `var` opens a
            // grouped var block (`var ( ... )`), sharing the same depth stack
            // as struct/interface '{' (see include/lexer.h asi_ctx). All
            // other '(' (call args, parenthesized expressions, const/import
            // groups, func param lists) push a no-emit (0) entry.
            if (lexer->asi_depth >= 0 && lexer->asi_depth < (int)sizeof(lexer->asi_ctx)) {
                lexer->asi_ctx[lexer->asi_depth] =
                    (lexer->prev_token_type == TOKEN_VAR) ? 1 : 0;
            }
            lexer->asi_depth++;
            lexer_read_char(lexer);
            break;
        case ')':
            token = token_new(TOKEN_RPAREN, ")", 1, current_pos);
            if (lexer->asi_depth > 0) lexer->asi_depth--;
            lexer_read_char(lexer);
            break;
        case '{':
            token = token_new(TOKEN_LBRACE, "{", 1, current_pos);
            if (lexer->asi_depth >= 0 && lexer->asi_depth < (int)sizeof(lexer->asi_ctx)) {
                lexer->asi_ctx[lexer->asi_depth] =
                    (lexer->prev_token_type == TOKEN_STRUCT ||
                     lexer->prev_token_type == TOKEN_INTERFACE) ? 1 : 0;
            }
            lexer->asi_depth++;
            lexer_read_char(lexer);
            break;
        case '}':
            token = token_new(TOKEN_RBRACE, "}", 1, current_pos);
            if (lexer->asi_depth > 0) lexer->asi_depth--;
            lexer_read_char(lexer);
            break;
        case '[':
            token = token_new(TOKEN_LBRACKET, "[", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case ']':
            token = token_new(TOKEN_RBRACKET, "]", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case ';':
            token = token_new(TOKEN_SEMICOLON, ";", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case ',':
            token = token_new(TOKEN_COMMA, ",", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case ':':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_SHORT_ASSIGN, ":=", 2, current_pos);
            } else {
                token = token_new(TOKEN_COLON, ":", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
        case '~':
            token = token_new(TOKEN_BIT_NOT, "~", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case '?':
            token = token_new(TOKEN_QUESTION, "?", 1, current_pos);
            lexer_read_char(lexer);
            break;
        case '@':
            token = token_new(TOKEN_DEREF, "@", 1, current_pos);
            lexer_read_char(lexer);
            break;
            
        // Multi-character operators
        case '+':
            if (lexer_peek_char(lexer) == '+') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_INCREMENT, "++", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_PLUS_ASSIGN, "+=", 2, current_pos);
            } else {
                token = token_new(TOKEN_PLUS, "+", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '-':
            if (lexer_peek_char(lexer) == '-') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_DECREMENT, "--", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_MINUS_ASSIGN, "-=", 2, current_pos);
            } else {
                token = token_new(TOKEN_MINUS, "-", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '*':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_MUL_ASSIGN, "*=", 2, current_pos);
            } else {
                token = token_new(TOKEN_MULTIPLY, "*", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '/':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_DIV_ASSIGN, "/=", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '/') {
                // Line comment - skip to end of line
                while (lexer->ch != '\n' && lexer->ch != 0) {
                    lexer_read_char(lexer);
                }
                // P0-5: iterate rather than tail-recurse over skipped comments,
                // mirroring the newline path above (P0-3). A run of consecutive
                // `//` comments previously recursed once per comment (`return
                // lexer_next_token(lexer)`), overflowing the stack on large
                // inputs (400,000 comment lines -> SIGSEGV). `continue` re-enters
                // the enclosing `for (;;)` at the top, which re-runs
                // lexer_skip_whitespace and re-derives current_pos exactly as the
                // recursive call did — same behavior, no new stack frame.
                // prev_token_type is untouched here (as it was in the recursive
                // version, which only updates it via the `if (token) ...` line
                // below on the eventual real token), so ASI across a skipped
                // comment behaves identically to ASI across a skipped blank line.
                continue;
            } else if (lexer_peek_char(lexer) == '*') {
                // Block comment - skip to */
                lexer_read_char(lexer); // skip /
                lexer_read_char(lexer); // skip *
                while (lexer->ch != 0) {
                    if (lexer->ch == '*' && lexer_peek_char(lexer) == '/') {
                        lexer_read_char(lexer); // skip *
                        lexer_read_char(lexer); // skip /
                        break;
                    }
                    lexer_read_char(lexer);
                }
                // See the P0-5 note above the `//` case: same iterate-not-
                // recurse fix, same prev_token_type preservation. A block
                // comment may itself span newlines (e.g. `/* line1\nline2 */`);
                // those interior newlines are consumed silently by the skip loop
                // above and never reach the '\n' case's ASI logic — this matches
                // the pre-existing recursive behavior exactly (the recursive
                // call also started scanning fresh AFTER the closing `*/`, never
                // seeing the interior newlines as lexer_next_token input), so a
                // block comment spanning a newline does not trigger ASI even
                // when it sits where a real newline would have.
                continue;
            } else {
                token = token_new(TOKEN_DIVIDE, "/", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '%':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_MOD_ASSIGN, "%=", 2, current_pos);
            } else {
                token = token_new(TOKEN_MODULO, "%", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '=':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_EQ, "==", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '>') {
                // `=>` (fat arrow): the value-yielding catch fallback form,
                // `f() catch => -1` (P2.9). Lexed as one token so `catch =`
                // followed by `>` can never split into ASSIGN then GT.
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_FAT_ARROW, "=>", 2, current_pos);
            } else {
                token = token_new(TOKEN_ASSIGN, "=", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '!':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_NE, "!=", 2, current_pos);
            } else {
                token = token_new(TOKEN_BANG, "!", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '<':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_LE, "<=", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '<') {
                lexer_read_char(lexer);
                if (lexer_peek_char(lexer) == '=') {
                    lexer_read_char(lexer);
                    lexer_read_char(lexer);
                    token = token_new(TOKEN_LSHIFT_ASSIGN, "<<=", 3, current_pos);
                } else {
                    lexer_read_char(lexer);
                    token = token_new(TOKEN_LSHIFT, "<<", 2, current_pos);
                }
            } else if (lexer_peek_char(lexer) == '-') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_ARROW, "<-", 2, current_pos);
            } else {
                token = token_new(TOKEN_LT, "<", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '>':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_GE, ">=", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '>') {
                lexer_read_char(lexer);
                if (lexer_peek_char(lexer) == '=') {
                    lexer_read_char(lexer);
                    lexer_read_char(lexer);
                    token = token_new(TOKEN_RSHIFT_ASSIGN, ">>=", 3, current_pos);
                } else {
                    lexer_read_char(lexer);
                    token = token_new(TOKEN_RSHIFT, ">>", 2, current_pos);
                }
            } else {
                token = token_new(TOKEN_GT, ">", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '&':
            if (lexer_peek_char(lexer) == '&') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_AND, "&&", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '^') {
                // Go's bit-clear operator `&^` (AND NOT). Lexed as one token so
                // it does not split into `&` then unary `^`.
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_AND_NOT, "&^", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_AND_ASSIGN, "&=", 2, current_pos);
            } else {
                token = token_new(TOKEN_BIT_AND, "&", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '|':
            if (lexer_peek_char(lexer) == '|') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_OR, "||", 2, current_pos);
            } else if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_OR_ASSIGN, "|=", 2, current_pos);
            } else {
                token = token_new(TOKEN_BIT_OR, "|", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '^':
            if (lexer_peek_char(lexer) == '=') {
                lexer_read_char(lexer);
                lexer_read_char(lexer);
                token = token_new(TOKEN_XOR_ASSIGN, "^=", 2, current_pos);
            } else {
                token = token_new(TOKEN_BIT_XOR, "^", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '.':
            if (lexer_peek_char(lexer) == '.') {
                lexer_read_char(lexer);
                if (lexer_peek_char(lexer) == '.') {
                    lexer_read_char(lexer);
                    lexer_read_char(lexer);
                    token = token_new(TOKEN_ELLIPSIS, "...", 3, current_pos);
                } else {
                    // This is an error - two dots without a third
                    token = token_new(TOKEN_ERROR, "..", 2, current_pos);
                    lexer_read_char(lexer);
                }
            } else {
                token = token_new(TOKEN_DOT, ".", 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
            
        case '"':
            {
                size_t length;
                char* string_literal = lexer_read_string(lexer, &length);
                if (string_literal) {
                    token = token_new(TOKEN_STRING, string_literal, length, current_pos);
                    free(string_literal);
                } else {
                    const char* why = lexer->literal_error ? lexer->literal_error
                                                           : "unterminated string";
                    token = token_new(TOKEN_ERROR, why, strlen(why), current_pos);
                }
            }
            break;
            
        case '`':
            {
                // Go spec raw string literal: content between backticks is
                // taken literally — backslashes are ordinary data, never an
                // escape introducer (`\n` here is backslash+n, two bytes,
                // not a newline). May span multiple lines; those interior
                // newlines are content and never reach the '\n' case above
                // (the whole literal is consumed in this one call), so they
                // cannot trigger ASI — this token still ends as TOKEN_STRING,
                // matching a regular string, so prev_token_type is
                // value-ending afterward exactly like `"..."` is.
                size_t length;
                char* raw_literal = lexer_read_raw_string(lexer, &length);
                if (raw_literal) {
                    token = token_new(TOKEN_STRING, raw_literal, length, current_pos);
                    free(raw_literal);
                } else {
                    token = token_new(TOKEN_ERROR, "unterminated raw string", 24, current_pos);
                }
            }
            break;

        case '\'':
            {
                size_t length;
                char* char_literal = lexer_read_char_literal(lexer, &length);
                long rune_val;
                const char* rune_err = NULL;
                if (char_literal && lexer_decode_char_value(char_literal, length, &rune_val, &rune_err)) {
                    // A rune literal is an untyped integer constant (rune = int32
                    // = `int` today): emit it as TOKEN_INT carrying the decimal
                    // value, so it flows through the existing int path (parser
                    // INT_LITERAL rule, TYPE_INT32, arithmetic, fmt.Println).
                    // Emitting an int token rather than a distinct CHAR_LITERAL
                    // grammar terminal means the parser conflict count is
                    // unaffected. v1 supports single-byte ASCII runes + common
                    // escapes; multi-byte UTF-8 runes are deferred.
                    char int_str[16];
                    int n = snprintf(int_str, sizeof(int_str), "%ld", rune_val);
                    token = token_new(TOKEN_INT, int_str, (size_t)n, current_pos);
                } else {
                    const char* why = rune_err ? rune_err
                                    : lexer->literal_error ? lexer->literal_error
                                    : "invalid character literal";
                    token = token_new(TOKEN_ERROR, why, strlen(why), current_pos);
                }
                if (char_literal) free(char_literal);
            }
            break;
            
        default:
            if (lexer_is_letter(lexer->ch)) {
                size_t length;
                char* identifier = lexer_read_identifier(lexer, &length);
                if (identifier) {
                    TokenType type = token_is_keyword(identifier) ? 
                                   token_keyword_type(identifier) : TOKEN_IDENT;
                    token = token_new(type, identifier, length, current_pos);
                    free(identifier);
                } else {
                    token = token_new(TOKEN_ERROR, "invalid identifier", 18, current_pos);
                }
            } else if (lexer_is_digit(lexer->ch)) {
                size_t length;
                int is_float;
                char* number = lexer_read_number(lexer, &length, &is_float);
                const char* bad = number ? lexer_number_error(number, is_float) : NULL;
                if (number && bad) {
                    token = token_new(TOKEN_ERROR, bad, strlen(bad), current_pos);
                    free(number);
                } else if (number) {
                    TokenType type = is_float ? TOKEN_FLOAT : TOKEN_INT;
                    token = token_new(type, number, length, current_pos);
                    free(number);
                } else {
                    token = token_new(TOKEN_ERROR, "invalid number", 14, current_pos);
                }
            } else {
                // Capture the offending byte as the token's literal (instead of
                // NULL) so the P0.4 gate below can name it in the diagnostic —
                // e.g. `#` — rather than printing an empty/placeholder string.
                char unknown_ch[2] = { lexer->ch, '\0' };
                token = token_new(TOKEN_UNKNOWN, unknown_ch, 1, current_pos);
                lexer_read_char(lexer);
            }
            break;
    }

    // Surface lexical errors so the Bison bridge's silent skip of TOKEN_ERROR /
    // TOKEN_UNKNOWN cannot drop a malformed or unrecognized token (e.g. '',
    // '\z', an unterminated 'a, or a stray '#') and let the surrounding
    // program compile to a running binary. We record the error (and print a
    // positioned diagnostic); the type checker refuses to emit code while the
    // count is non-zero, which is the actual "rejected cleanly" gate.
    // TOKEN_UNKNOWN is only ever produced by the `default` case above, for a
    // byte that is neither a letter, digit, nor any recognized punctuation —
    // there is no legitimate Goo construct that lexes to it (a `#` inside a
    // string literal, for instance, is consumed byte-for-byte by
    // lexer_read_string and never reaches this switch).
    if (token && token->type == TOKEN_ERROR) {
        goo_lexer_error_count++;
        fprintf(stderr, "%s:%d:%d: error: %s\n",
                token->pos.filename ? token->pos.filename : "<input>",
                token->pos.line, token->pos.column,
                token->literal ? token->literal : "lexical error");
    } else if (token && token->type == TOKEN_UNKNOWN) {
        goo_lexer_error_count++;
        fprintf(stderr, "%s:%d:%d: error: unknown token '%s'\n",
                token->pos.filename ? token->pos.filename : "<input>",
                token->pos.line, token->pos.column,
                token->literal ? token->literal : "?");
    }

    if (token) lexer->prev_token_type = token->type; // for newline-driven ASI
    return token;
    } // for (;;) — only reached via `continue` on a skipped newline (P0-3)
}

char* lexer_read_identifier(Lexer* lexer, size_t* length) {
    size_t start_pos = lexer->position;
    
    while (lexer_is_letter(lexer->ch) || lexer_is_digit(lexer->ch)) {
        lexer_read_char(lexer);
    }
    
    size_t len = lexer->position - start_pos;
    if (length) *length = len;
    
    char* identifier = malloc(len + 1);
    if (!identifier) return NULL;
    
    strncpy(identifier, &lexer->input[start_pos], len);
    identifier[len] = '\0';
    
    return identifier;
}

char* lexer_read_number(Lexer* lexer, size_t* length, int* is_float) {
    size_t start_pos = lexer->position;
    *is_float = 0;

    // Non-decimal integer bases (Go): 0x/0X hex, 0o/0O octal, 0b/0B binary.
    // These are always integers (no fractional/exponent part) — read the prefix
    // then the base-appropriate digits and return early. Real stdlib source
    // (math/bits masks, tables) is full of hex constants like 0x5555555555555555.
    int non_decimal = 0;
    if (lexer->ch == '0') {
        char next = lexer_peek_char(lexer);
        if (next == 'x' || next == 'X' || next == 'b' || next == 'B' ||
            next == 'o' || next == 'O') {
            non_decimal = 1;
            lexer_read_char(lexer);      // consume '0'
            char base = lexer->ch;
            lexer_read_char(lexer);      // consume the base letter
            if (base == 'x' || base == 'X') {
                while (lexer_is_hex_digit(lexer->ch)) lexer_read_char(lexer);
            } else {
                // Binary and octal: take every decimal digit, not only the
                // valid ones, so lexer_number_error names `0b2` / `0o9`
                // instead of splitting it into a literal and a stray int.
                while (lexer_is_digit(lexer->ch)) lexer_read_char(lexer);
            }
        }
    }

    // Read integer part (decimal)
    while (!non_decimal && lexer_is_digit(lexer->ch)) {
        lexer_read_char(lexer);
    }
    
    // Check for decimal point
    if (lexer->ch == '.' && lexer_is_digit(lexer_peek_char(lexer))) {
        *is_float = 1;
        lexer_read_char(lexer); // consume '.'
        
        // Read fractional part
        while (lexer_is_digit(lexer->ch)) {
            lexer_read_char(lexer);
        }
    }
    
    // Check for exponent. Go has no exponent after a binary or octal prefix,
    // so such a literal stays an integer here. The letter is still read into
    // the literal, so lexer_number_error refuses the whole token at its start.
    if (lexer->ch == 'e' || lexer->ch == 'E') {
        if (!non_decimal) *is_float = 1;
        lexer_read_char(lexer); // consume 'e' or 'E'
        
        if (lexer->ch == '+' || lexer->ch == '-') {
            lexer_read_char(lexer); // consume sign
        }
        
        while (lexer_is_digit(lexer->ch)) {
            lexer_read_char(lexer);
        }
    }
    
    size_t len = lexer->position - start_pos;
    if (length) *length = len;
    
    char* number = malloc(len + 1);
    if (!number) return NULL;
    
    strncpy(number, &lexer->input[start_pos], len);
    number[len] = '\0';
    
    return number;
}

char* lexer_read_string(Lexer* lexer, size_t* length) {
    lexer->literal_error = NULL;
    lexer_read_char(lexer); // consume opening quote
    size_t start_pos = lexer->position;

    while (lexer->ch != '"' && lexer->ch != 0) {
        if (lexer->ch == '\\') {
            lexer_read_char(lexer);
            if (lexer->ch != 0) {
                lexer_read_char(lexer);
            }
        } else {
            lexer_read_char(lexer);
        }
    }

    if (lexer->ch != '"') {
        return NULL; // Unterminated string
    }

    size_t raw_len = lexer->position - start_pos;
    char* out = malloc(raw_len + 1); // escapes can only shrink, never grow
    if (!out) return NULL;

    size_t out_len = 0;
    lexer->literal_error = NULL;
    for (size_t i = 0; i < raw_len; i++) {
        char c = lexer->input[start_pos + i];
        if (c != '\\') {
            out[out_len++] = c;
            continue;
        }
        // A backslash is always followed by something: the scan loop above
        // would otherwise have swallowed the closing quote as an escape.
        uint32_t val; int is_byte; size_t used; const char* err;
        if (!lexer_decode_escape(lexer->input + start_pos + i + 1, raw_len - i - 1,
                                 '"', &val, &is_byte, &used, &err)) {
            lexer->literal_error = err;
            free(out);
            lexer_read_char(lexer); // consume closing quote: no cascade
            return NULL;
        }
        // The decoder reads only inside the `avail` bytes it was given.
        GOO_ASSERT(used <= raw_len - i - 1);
        i += used;
        if (is_byte) out[out_len++] = (char)val;
        else out_len += utf8_encode(val, out + out_len);
    }
    out[out_len] = '\0';
    if (length) *length = out_len;

    lexer_read_char(lexer); // consume closing quote
    return out;
}

// Raw string literal (backtick-delimited), per the Go spec: content is taken
// literally between the backticks — no escape processing at all, so a
// backslash is just a data byte (unlike lexer_read_string above, which
// interprets `\n`, `\xNN`, etc.). May span multiple source lines; interior
// newlines are copied through as content. The one transformation the spec
// still requires is dropping carriage-return bytes (0x0D) so that raw
// strings read from CRLF source files are line-ending-independent, matching
// gc's cmd/compile behavior. Returns NULL on EOF before the closing
// backtick (unterminated), mirroring lexer_read_string's contract so the
// caller's TOKEN_ERROR path is identical.
char* lexer_read_raw_string(Lexer* lexer, size_t* length) {
    lexer_read_char(lexer); // consume opening backtick
    size_t start_pos = lexer->position;

    while (lexer->ch != '`' && lexer->ch != 0) {
        lexer_read_char(lexer);
    }

    if (lexer->ch != '`') {
        return NULL; // Unterminated raw string
    }

    size_t raw_len = lexer->position - start_pos;
    char* out = malloc(raw_len + 1); // CR-stripping only ever shrinks
    if (!out) return NULL;

    size_t out_len = 0;
    for (size_t i = 0; i < raw_len; i++) {
        char c = lexer->input[start_pos + i];
        if (c == '\r') continue; // Go spec: CR bytes are dropped, not content
        out[out_len++] = c;
    }
    out[out_len] = '\0';
    if (length) *length = out_len;

    lexer_read_char(lexer); // consume closing backtick
    return out;
}

// Decode a char-literal body (the raw text between the quotes, escapes still
// in source form) into its integer rune value. Returns 1 and writes *out on
// success, 0 if the body is empty, an unknown escape, or a multi-byte (UTF-8)
// rune — all of which are deferred in v1. On a 0 return the caller emits
// TOKEN_ERROR, which goo_lexer_error_count records and the type checker gates on
// (type_check_program), so these forms are genuinely rejected (positioned
// diagnostic + non-zero exit, no binary) rather than silently dropped by the
// Bison bridge. Note: '\0' decodes to value 0, which is why success is signalled
// separately from the value.
int lexer_decode_char_value(const char* body, size_t len, long* out, const char** err) {
    const char* dummy;
    if (!err) err = &dummy;
    *err = NULL;
    if (!body || !out || len == 0) return 0;
    if (body[0] != '\\') {
        // Plain rune: only single-byte ASCII supported in v1.
        if (len != 1) return 0;
        if (body[0] == '\'') {
            *err = "empty rune literal or unescaped ' in rune literal";
            return 0;
        }
        *out = (unsigned char)body[0];
        return 1;
    }
    uint32_t val; int is_byte; size_t used;
    if (!lexer_decode_escape(body + 1, len - 1, '\'', &val, &is_byte, &used, err)) {
        return 0;
    }
    if (used != len - 1) return 0; // trailing bytes after the escape
    *out = (long)val;
    return 1;
}

char* lexer_read_char_literal(Lexer* lexer, size_t* length) {
    lexer->literal_error = NULL;
    lexer_read_char(lexer); // consume opening quote
    size_t start_pos = lexer->position;

    if (lexer->ch == '\'') {
        // `''` is empty and `'''` holds an unescaped quote. Consume the whole
        // run so the stray quotes do not start a second literal.
        lexer_read_char(lexer);
        if (lexer->ch == '\'') lexer_read_char(lexer);
        lexer->literal_error = "empty rune literal or unescaped ' in rune literal";
        return NULL;
    }

    if (lexer->ch == '\\') {
        lexer_read_char(lexer); // skip backslash
        char esc = lexer->ch;
        if (esc != 0) {
            lexer_read_char(lexer); // skip the escape selector (n, t, x, u, U, ...)
        }
        // Numeric escapes carry digits after the selector: \xNN, \uXXXX,
        // \UXXXXXXXX, and \ooo (the first octal digit is the selector).
        size_t ndig = (esc == 'x') ? 2 : (esc == 'u') ? 4 : (esc == 'U') ? 8
                    : (esc >= '0' && esc <= '7') ? 2 : 0;
        for (size_t i = 0; i < ndig && lexer->ch != 0 && lexer->ch != '\''; i++) {
            lexer_read_char(lexer);
        }
    } else if (lexer->ch != 0) {
        lexer_read_char(lexer);
    }

    if (lexer->ch != '\'') {
        return NULL; // Unterminated or invalid character literal
    }

    size_t len = lexer->position - start_pos;
    if (length) *length = len;

    char* char_lit = malloc(len + 1);
    if (!char_lit) return NULL;

    strncpy(char_lit, &lexer->input[start_pos], len);
    char_lit[len] = '\0';

    lexer_read_char(lexer); // consume closing quote

    return char_lit;
}

int lexer_is_letter(char ch) {
    return isalpha(ch) || ch == '_';
}

int lexer_is_digit(char ch) {
    return isdigit(ch);
}

int lexer_is_hex_digit(char ch) {
    return isxdigit(ch);
}

int lexer_is_whitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r';
}

void lexer_error(Lexer* lexer, const char* message) {
    fprintf(stderr, "Lexer error at %s:%d:%d: %s\n", 
            lexer->pos.filename ? lexer->pos.filename : "<stdin>",
            lexer->pos.line, lexer->pos.column, message);
}