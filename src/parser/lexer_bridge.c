#include "lexer.h"
#include "token.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.tab.h"

// Global lexer instance for Bison integration
Lexer* current_lexer = NULL;

// Token value for Bison
extern YYSTYPE yylval;

// Bison parser function
extern int yyparse(void);

// M10 composite-literal disambiguation: lexer-layer frame stack.
// See docs/M10_LEXER_LAYER_PROBE.md and docs/M10_GRAMMAR_DECISION.md.
//
// The bridge tracks a stack of "cond frames" pushed when emitting IF/FOR/
// MATCH/SELECT/SWITCH and popped when a `{` arrives at the same paren_depth
// the frame was pushed at. The popping `{` is emitted as LBRACE_BODY rather
// than LBRACE; only `block`, `match_expr`, and `select_stmt` accept it, so
// struct_lit (`identifier LBRACE ...`) cannot match at the cond/body
// boundary. Inside parens or brackets, paren_depth rises and the frame
// doesn't fire — so `if (Foo{x:1}.y > 0)` works and `for k := range []int{1,2,3}`
// works (the `[]int{...}` brace is at paren_depth >= 1 due to the leading `[`).
//
// IF_COND has a special transition: a guard-condition `case Foo if x > 0:`
// terminates with COLON, not LBRACE. Without this, the IF frame leaks past
// the case and the next statement's first `{` gets wrongly recolored.

#define M10_FRAME_KIND_IF      1
#define M10_FRAME_KIND_FOR     2
#define M10_FRAME_KIND_MATCH   3
#define M10_FRAME_KIND_SELECT  4
#define M10_FRAME_KIND_SWITCH  5

typedef struct {
    int kind;
    int paren_depth_at_push;
    // Armed when a TYPE-position `[` or `map` appears at this frame's depth, so
    // the composite literal's opening `{` (`for _, v := range []int{...} {`) is
    // classified as a literal LBRACE, not the LBRACE_BODY of the loop/if body.
    // Index-position `[` (`if a[i] > 0 {`) does NOT arm it — see
    // bridge_prev_ends_operand. Cleared by the literal `{`, or by any token
    // that can't continue a type (e.g. `(` in the conversion `[]byte(s)`).
    int literal_pending;
} M10CondFrame;

#define M10_MAX_FRAMES 64
static M10CondFrame m10_frame_stack[M10_MAX_FRAMES];
static int m10_frame_top = -1;
static int m10_paren_depth = 0;
// Brace-nesting depth INSIDE a control-clause composite literal. While > 0 we
// are inside such a literal (`range [][]int{{1},{2}} {`), so every `{` is a
// literal and no cond frame fires until the braces balance and the real body
// `{` arrives.
static int m10_lit_depth = 0;

static void m10_push_frame(int kind) {
    if (m10_frame_top + 1 >= M10_MAX_FRAMES) {
        // Overflow: silently no-op. Realistically unreachable (would require
        // ~64 levels of nested if/for, which the parser would reject anyway
        // on recursion-limit grounds elsewhere). Documented for the audit.
        return;
    }
    m10_frame_top++;
    m10_frame_stack[m10_frame_top].kind = kind;
    m10_frame_stack[m10_frame_top].paren_depth_at_push = m10_paren_depth;
    m10_frame_stack[m10_frame_top].literal_pending = 0;
}

// A following `[` is an INDEX (not a type prefix) when the previous token ends
// an operand — the same distinction the `[]`→RBRACKET_SLICE lookahead relies
// on. Used so `a[i]` never arms literal_pending while `range []int` does.
static int bridge_prev_ends_operand(int bt) {
    switch (bt) {
        case IDENTIFIER: case RBRACKET: case RBRACKET_SLICE: case RPAREN:
        case RBRACE: case STRING_LITERAL: case INT_LITERAL: case FLOAT_LITERAL:
        case CHAR_LITERAL: case TRUE: case FALSE: case NIL:
        case INCREMENT: case DECREMENT:
            return 1;
        default:
            return 0;
    }
}

// Tokens that can appear between an armed type-position `[`/`map` and the
// composite literal's `{` — i.e. that keep literal_pending alive. Anything
// else (an operator, `(`, `;`, ...) means the type-prefix did not lead into a
// literal, so literal_pending is cleared.
static int bridge_token_continues_type(int bt) {
    switch (bt) {
        case IDENTIFIER: case LBRACKET: case RBRACKET: case RBRACKET_SLICE:
        case MAP: case CHAN: case MULTIPLY: case DOT:
        case STRUCT: case FUNC: case ENUM: case BANG: case QUESTION:
            return 1;
        default:
            return 0;
    }
}

static void m10_pop_frame(void) {
    if (m10_frame_top >= 0) m10_frame_top--;
}

// `[]` slice-type disambiguation (P3-1).
//
// An empty `[]` is ambiguous between a bare empty-slice literal (`xs := []`)
// and the prefix of a slice TYPE (`[]int`, including the typed composite
// literal `[]Foo{...}`). LALR cannot defer this decision — after `[] .` it
// must either reduce the empty literal or shift a type, and the type-starting
// lookahead tokens overlap FOLLOW(empty-slice), producing shift/reduce
// conflicts. We resolve it in the lexer with one-token lookahead, mirroring
// the existing LBRACE_BODY trick: when an empty `[]` is immediately followed
// by a type-starting token, the closing `]` is emitted as RBRACKET_SLICE so
// the grammar routes it through `slice_type: LBRACKET RBRACKET_SLICE type`,
// out of the empty-literal state. Otherwise plain RBRACKET is emitted and the
// empty-slice literal (and HKT `F[]`) parse exactly as before — zero added
// conflicts.
static int     s_have_pending = 0;   // a peeked token is buffered
static int     s_pending_tok = 0;    // its bison token id
static YYSTYPE s_pending_val;        // its semantic value
static int     s_last_emitted = 0;   // last token yylex returned (for `[]`)
static int     s_last_token_line = 0;// source line of the token bridge_next_mapped last returned

// FIRST(type): if one of these follows an empty `[]`, the `[]` is a
// slice-type prefix, not a bare empty-slice literal. Kept in sync with the
// `type` nonterminal's alternatives in parser.y.
static int bridge_token_starts_type(int bt) {
    switch (bt) {
        case IDENTIFIER: case LBRACKET: case MAP: case CHAN:
        case FUNC: case STRUCT: case ENUM:
        case PUB: case SUB: case REQ: case REP: case PUSH: case PULL:
        case MULTIPLY: case BIT_AND: case UNSAFE: case BANG: case QUESTION:
            return 1;
        default:
            return 0;
    }
}

// Reset the M10 state. Called from parse_input on entry so a parse error
// in a prior invocation doesn't leak frames into the next. Also exposed
// for LSP use (see parser_init/parser_cleanup callers in src/ide/).
void bridge_reset_cond_state(void) {
    m10_frame_top = -1;
    m10_paren_depth = 0;
    m10_lit_depth = 0;
    s_have_pending = 0;
    s_last_emitted = 0;
    s_last_token_line = 0;
}

// Map our tokens to Bison tokens
static int map_token_to_bison(TokenType type) {
    switch (type) {
        case TOKEN_EOF: return 0; // End of input
        
        // Literals
        case TOKEN_IDENT: return IDENTIFIER;
        case TOKEN_INT: return INT_LITERAL;
        case TOKEN_FLOAT: return FLOAT_LITERAL;
        case TOKEN_STRING: return STRING_LITERAL;
        case TOKEN_CHAR: return CHAR_LITERAL;
        case TOKEN_TRUE: return TRUE;
        case TOKEN_FALSE: return FALSE;
        case TOKEN_NIL: return NIL;
        
        // Go Keywords
        case TOKEN_BREAK: return BREAK;
        case TOKEN_CASE: return CASE;
        case TOKEN_CHAN: return CHAN;
        case TOKEN_CONST: return CONST;
        case TOKEN_CONTINUE: return CONTINUE;
        case TOKEN_DEFAULT: return DEFAULT;
        case TOKEN_DEFER: return DEFER;
        case TOKEN_ELSE: return ELSE;
        case TOKEN_FALLTHROUGH: return FALLTHROUGH;
        case TOKEN_FOR:
            m10_push_frame(M10_FRAME_KIND_FOR);
            return FOR;
        case TOKEN_FUNC: return FUNC;
        case TOKEN_GO: return GO;
        case TOKEN_GOTO: return GOTO;
        case TOKEN_IF:
            m10_push_frame(M10_FRAME_KIND_IF);
            return IF;
        case TOKEN_IMPORT: return IMPORT;
        case TOKEN_INTERFACE: return INTERFACE;
        case TOKEN_MAP:
            // `map` is always a type prefix, so `range map[K]V{...} {` arms
            // literal_pending at the cond-frame depth just like a type-position
            // `[` — the map literal's `{` is a literal, not the body.
            if (m10_frame_top >= 0
                && m10_frame_stack[m10_frame_top].paren_depth_at_push == m10_paren_depth
                && m10_lit_depth == 0) {
                m10_frame_stack[m10_frame_top].literal_pending = 1;
            }
            return MAP;
        case TOKEN_PACKAGE: return PACKAGE;
        case TOKEN_RANGE: return RANGE;
        case TOKEN_RETURN: return RETURN;
        case TOKEN_SELECT:
            m10_push_frame(M10_FRAME_KIND_SELECT);
            return SELECT;
        case TOKEN_STRUCT: return STRUCT;
        case TOKEN_SWITCH:
            m10_push_frame(M10_FRAME_KIND_SWITCH);
            return SWITCH;
        case TOKEN_MATCH:
            m10_push_frame(M10_FRAME_KIND_MATCH);
            return MATCH;
        case TOKEN_TYPE: return TYPE;
        case TOKEN_VAR: return VAR;
        
        // Goo Extensions
        case TOKEN_COMPTIME: return COMPTIME;
        case TOKEN_CONCEPT: return CONCEPT;
        case TOKEN_PUB: return PUB;
        case TOKEN_SUB: return SUB;
        case TOKEN_REQ: return REQ;
        case TOKEN_REP: return REP;
        case TOKEN_PUSH: return PUSH;
        case TOKEN_PULL: return PULL;
        case TOKEN_TRY: return TRY;
        case TOKEN_CATCH: return CATCH;
        case TOKEN_UNSAFE: return UNSAFE;
        case TOKEN_ARENA: return ARENA;
        case TOKEN_OWNED: return OWNED;
        case TOKEN_BORROWED: return BORROWED;
        case TOKEN_SHARED: return SHARED;
        case TOKEN_LET: return LET;
        case TOKEN_ENUM: return ENUM;

        // Operators
        case TOKEN_PLUS: return PLUS;
        case TOKEN_MINUS: return MINUS;
        case TOKEN_MULTIPLY: return MULTIPLY;
        case TOKEN_DIVIDE: return DIVIDE;
        case TOKEN_MODULO: return MODULO;
        case TOKEN_ASSIGN: return ASSIGN;
        case TOKEN_SHORT_ASSIGN: return SHORT_ASSIGN;
        case TOKEN_PLUS_ASSIGN: return PLUS_ASSIGN;
        case TOKEN_MINUS_ASSIGN: return MINUS_ASSIGN;
        case TOKEN_MUL_ASSIGN: return MUL_ASSIGN;
        case TOKEN_DIV_ASSIGN: return DIV_ASSIGN;
        case TOKEN_MOD_ASSIGN: return MOD_ASSIGN;
        case TOKEN_AND_ASSIGN: return AND_ASSIGN;
        case TOKEN_OR_ASSIGN: return OR_ASSIGN;
        case TOKEN_XOR_ASSIGN: return XOR_ASSIGN;
        case TOKEN_LSHIFT_ASSIGN: return LSHIFT_ASSIGN;
        case TOKEN_RSHIFT_ASSIGN: return RSHIFT_ASSIGN;
        
        case TOKEN_EQ: return EQ;
        case TOKEN_NE: return NE;
        case TOKEN_LT: return LT;
        case TOKEN_LE: return LE;
        case TOKEN_GT: return GT;
        case TOKEN_GE: return GE;
        
        case TOKEN_AND: return AND;
        case TOKEN_OR: return OR;
        case TOKEN_NOT: return NOT;
        
        case TOKEN_BIT_AND: return BIT_AND;
        case TOKEN_AND_NOT: return AND_NOT;
        case TOKEN_FAT_ARROW: return FAT_ARROW;
        case TOKEN_BIT_OR: return BIT_OR;
        case TOKEN_BIT_XOR: return BIT_XOR;
        case TOKEN_BIT_NOT: return BIT_NOT;
        case TOKEN_LSHIFT: return LSHIFT;
        case TOKEN_RSHIFT: return RSHIFT;
        
        case TOKEN_INCREMENT: return INCREMENT;
        case TOKEN_DECREMENT: return DECREMENT;
        
        case TOKEN_ARROW: return ARROW;
        case TOKEN_BANG: return BANG;
        case TOKEN_QUESTION: return QUESTION;
        
        // Delimiters
        case TOKEN_LPAREN:
            m10_paren_depth++;
            return LPAREN;
        case TOKEN_RPAREN:
            if (m10_paren_depth > 0) m10_paren_depth--;
            return RPAREN;
        case TOKEN_LBRACE:
            // Already inside a control-clause composite literal
            // (`range [][]int{{1},{2}} {`) — every brace here is a literal
            // brace, not the body. Deepen and stay a plain LBRACE.
            if (m10_lit_depth > 0) {
                m10_lit_depth++;
                return LBRACE;
            }
            // If there's a cond frame on top at our paren_depth, this `{`
            // either OPENS a `[`/map-prefixed composite literal (armed
            // literal_pending → plain LBRACE, enter the literal) or closes the
            // cond's expression and opens its body (LBRACE_BODY, pop).
            if (m10_frame_top >= 0
                && m10_frame_stack[m10_frame_top].paren_depth_at_push == m10_paren_depth) {
                if (m10_frame_stack[m10_frame_top].literal_pending) {
                    m10_frame_stack[m10_frame_top].literal_pending = 0;
                    m10_lit_depth = 1;
                    return LBRACE;
                }
                m10_pop_frame();
                return LBRACE_BODY;
            }
            return LBRACE;
        case TOKEN_RBRACE:
            if (m10_lit_depth > 0) m10_lit_depth--;
            return RBRACE;
        case TOKEN_LBRACKET:
            // A TYPE-position `[` at the top cond frame's depth (e.g.
            // `range []int{...}`) arms literal_pending so the composite
            // literal's `{` is classified as a literal, not the body. An
            // INDEX-position `[` (previous token ends an operand, e.g.
            // `if a[i] > 0 {`) does NOT arm — that would misread the body `{`.
            if (m10_frame_top >= 0
                && m10_frame_stack[m10_frame_top].paren_depth_at_push == m10_paren_depth
                && m10_lit_depth == 0
                && !bridge_prev_ends_operand(s_last_emitted)) {
                m10_frame_stack[m10_frame_top].literal_pending = 1;
            }
            // LBRACKET also raises paren_depth so that `[]int{1,2,3}` after
            // an IF/FOR has its struct-lit-like brace at depth >= 1, keeping
            // the cond frame alive until the real body brace at depth 0.
            m10_paren_depth++;
            return LBRACKET;
        case TOKEN_RBRACKET:
            if (m10_paren_depth > 0) m10_paren_depth--;
            return RBRACKET;
        case TOKEN_SEMICOLON: return SEMICOLON;
        case TOKEN_COMMA: return COMMA;
        case TOKEN_DOT: return DOT;
        case TOKEN_COLON:
            // Guard-condition terminator: `case Foo if x > 0:` ends with COLON
            // instead of LBRACE. Pop the topmost IF_COND frame at the same
            // paren_depth so it doesn't leak into the case body.
            if (m10_frame_top >= 0
                && m10_frame_stack[m10_frame_top].kind == M10_FRAME_KIND_IF
                && m10_frame_stack[m10_frame_top].paren_depth_at_push == m10_paren_depth) {
                m10_pop_frame();
            }
            return COLON;
        case TOKEN_ELLIPSIS: return ELLIPSIS;
        case TOKEN_NEWLINE: return NEWLINE;
        
        default:
            return -1; // Unknown token
    }
}

// Fetch + map the next token, setting yylval. Skips unknown tokens. Returns
// the bison token id (0 = EOF). This is the raw stream; the `[]` lookahead
// in yylex() is layered on top.
static int bridge_next_mapped(void) {
    if (!current_lexer) {
        return 0; // EOF
    }

    Token* token = lexer_next_token(current_lexer);
    if (!token) {
        return 0; // EOF
    }

    // Record the source line of this token so the `[]` lookahead in yylex can
    // tell whether a statement-boundary newline separated the `]` from the
    // peeked token (the lexer elides newlines Go-style, so the token stream
    // alone can't reveal the boundary). On the skip-recursion path below this
    // is overwritten by the recursive call, so it always reflects the token
    // actually returned.
    s_last_token_line = token->pos.line;

    // Depth BEFORE map_token_to_bison mutates it (LBRACKET/LPAREN increment):
    // the clear check below must see the depth the token arrived AT, so tokens
    // nested inside the type's own brackets — e.g. the size `3` in `[3]int` —
    // are recognized as not-at-frame-level and do NOT clear literal_pending.
    int depth_before = m10_paren_depth;
    int bison_token = map_token_to_bison(token->type);

    // Clear a stale literal_pending: it was armed by a type-position `[`/`map`
    // at cond-frame level, but if a token AT THAT LEVEL can't continue a type
    // heading into a composite literal — e.g. `(` in a conversion `[]byte(s)`,
    // or an operator — the type-prefix did not open a literal, so the eventual
    // body `{` must still fire as LBRACE_BODY. Tokens deeper than frame level
    // (array sizes, call args) never clear; the literal `{` clears it in
    // map_token_to_bison (and is excluded here).
    if (m10_frame_top >= 0 && m10_frame_stack[m10_frame_top].literal_pending
        && depth_before == m10_frame_stack[m10_frame_top].paren_depth_at_push
        && bison_token != LBRACE && !bridge_token_continues_type(bison_token)) {
        m10_frame_stack[m10_frame_top].literal_pending = 0;
    }

    // Set the semantic value based on token type
    switch (token->type) {
        case TOKEN_IDENT:
            yylval.string = strdup(token->literal);
            break;
        case TOKEN_STRING: {
            // Carry the exact byte length so embedded NUL bytes survive the
            // boundary (strdup would truncate at the first NUL). The parser's
            // STRING_LITERAL action copies data+len into the AST literal.
            char* buf = malloc(token->length + 1);
            if (buf) {
                if (token->length > 0) memcpy(buf, token->literal, token->length);
                buf[token->length] = '\0';
            }
            yylval.strlit.data = buf;
            yylval.strlit.len = token->length;
            break;
        }
        case TOKEN_INT: {
            // Parse the full unsigned 64-bit range (strtoull, not strtoll); the
            // bit pattern is preserved through the union's long long and the
            // parser's %lld reconstruction, so a value above INT64_MAX round-
            // trips exactly (e.g. 0xFFFFFFFFFFFFFFFF). Base 0 auto-detects 0x
            // hex, 0b binary (glibc), and decimal — but C's octal is a leading
            // zero, not Go's `0o`, so parse the digits after a 0o/0O prefix in
            // base 8 explicitly. Real stdlib source is full of these constants.
            const char* s = token->literal;
            if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
                yylval.integer = (long long)strtoull(s + 2, NULL, 8);
            } else {
                yylval.integer = (long long)strtoull(s, NULL, 0);
            }
            break;
        }
        case TOKEN_FLOAT:
            yylval.real = atof(token->literal);
            break;
        default:
            yylval.token = (int)token->type;
            break;
    }

    token_free(token);

    if (bison_token == -1) {
        // Skip unknown tokens
        return bridge_next_mapped();
    }

    return bison_token;
}

// Bison's lexer interface. Adds one-token lookahead over bridge_next_mapped to
// emit RBRACKET_SLICE for an empty `[]` that prefixes a slice type (see the
// disambiguation note above).
int yylex(void) {
    if (s_have_pending) {
        s_have_pending = 0;
        yylval = s_pending_val;
        s_last_emitted = s_pending_tok;
        return s_pending_tok;
    }

    int tok = bridge_next_mapped();

    // Empty `[]`: the just-fetched RBRACKET closes an immediately-preceding
    // LBRACKET. Peek the next token; if it starts a type, this `[]` is a
    // slice-type prefix → emit RBRACKET_SLICE instead. The peeked token is
    // buffered (with its yylval) and returned on the next call. RBRACKET /
    // RBRACKET_SLICE carry no semantic value, so overwriting yylval here is
    // harmless.
    if (tok == RBRACKET && s_last_emitted == LBRACKET) {
        int rbracket_line = s_last_token_line;
        int nxt = bridge_next_mapped();
        int nxt_line = s_last_token_line;
        s_pending_tok = nxt;
        s_pending_val = yylval;
        s_have_pending = 1;
        // Only treat the empty `[]` as a slice-TYPE prefix when the following
        // type-starting token sits on the SAME source line. A newline between
        // `]` and the next token is a statement boundary — the lexer elides
        // newlines Go-style, so no separator token survives — and in that case
        // the `[]` is a bare empty-slice literal whose statement has ended; the
        // next line is a NEW statement, not the slice element type. Without the
        // same-line check, `xs := []` <nl> `fmt.Println(...)` mis-lexed the
        // following identifier as the element type and failed to parse. `[]int`
        // / `[]Foo{...}` always keep their element type on the same line, so
        // they still route through RBRACKET_SLICE unchanged.
        if (nxt_line == rbracket_line && bridge_token_starts_type(nxt)) {
            s_last_emitted = RBRACKET_SLICE;
            return RBRACKET_SLICE;
        }
        s_last_emitted = RBRACKET;
        return RBRACKET;
    }

    s_last_emitted = tok;
    return tok;
}

// Initialize parser with input
int parse_input(const char* input, const char* filename) {
    current_lexer = lexer_new(input, filename);
    if (!current_lexer) {
        fprintf(stderr, "Error: Failed to create lexer\n");
        return -1;
    }

    // Reset M10 disambiguation state so a parse error in a prior call doesn't
    // leak frames or paren-depth into this parse.
    bridge_reset_cond_state();

    int result = yyparse();

    lexer_free(current_lexer);
    current_lexer = NULL;

    return result;
}

// Parse a file
int parse_file(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Error: Cannot open file %s\n", filename);
        return -1;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Read file content
    char* content = malloc(size + 1);
    if (!content) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        fclose(file);
        return -1;
    }
    
    fread(content, 1, size, file);
    content[size] = '\0';
    fclose(file);
    
    int result = parse_input(content, filename);
    free(content);
    
    return result;
}