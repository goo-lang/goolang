#include "test/test_framework.h"
#include "lexer.h"
#include "token.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Helper: lex a single token from input and check type and literal
static int check_token(const char* input, TokenType expected_type, const char* expected_literal) {
    Lexer* lexer = lexer_new(input, "test");
    if (!lexer) return 0;

    Token* tok = lexer_next_token(lexer);
    if (!tok) { lexer_free(lexer); return 0; }

    int ok = (tok->type == expected_type);
    if (expected_literal && tok->literal) {
        ok = ok && (strcmp(tok->literal, expected_literal) == 0);
    }

    token_free(tok);
    lexer_free(lexer);
    return ok;
}

// Helper: lex all tokens until EOF, return count (excluding EOF)
static int count_tokens(const char* input) {
    Lexer* lexer = lexer_new(input, "test");
    if (!lexer) return -1;

    int count = 0;
    for (;;) {
        Token* tok = lexer_next_token(lexer);
        if (!tok) break;
        if (tok->type == TOKEN_EOF) { token_free(tok); break; }
        count++;
        token_free(tok);
    }

    lexer_free(lexer);
    return count;
}

// =========================================================================
// Test: Single-character tokens
// =========================================================================
static TestStatus test_single_char_tokens(void* ctx) {
    (void)ctx;

    struct { const char* input; TokenType type; const char* lit; } cases[] = {
        {"(",  TOKEN_LPAREN,    "("},
        {")",  TOKEN_RPAREN,    ")"},
        {"{",  TOKEN_LBRACE,    "{"},
        {"}",  TOKEN_RBRACE,    "}"},
        {"[",  TOKEN_LBRACKET,  "["},
        {"]",  TOKEN_RBRACKET,  "]"},
        {";",  TOKEN_SEMICOLON, ";"},
        {",",  TOKEN_COMMA,     ","},
        {":",  TOKEN_COLON,     ":"},
        {".",  TOKEN_DOT,       "."},
        {"?",  TOKEN_QUESTION,  "?"},
        {"@",  TOKEN_DEREF,     "@"},
        {"~",  TOKEN_BIT_NOT,   "~"},
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        if (!check_token(cases[i].input, cases[i].type, cases[i].lit)) {
            printf("  FAIL: single-char '%s'\n", cases[i].input);
            return TEST_FAIL;
        }
    }
    return TEST_PASS;
}

// =========================================================================
// Test: Multi-character operators
// =========================================================================
static TestStatus test_multi_char_operators(void* ctx) {
    (void)ctx;

    struct { const char* input; TokenType type; const char* lit; } cases[] = {
        {":=",  TOKEN_SHORT_ASSIGN, ":="},
        {"++",  TOKEN_INCREMENT,    "++"},
        {"--",  TOKEN_DECREMENT,    "--"},
        {"+=",  TOKEN_PLUS_ASSIGN,  "+="},
        {"-=",  TOKEN_MINUS_ASSIGN, "-="},
        {"*=",  TOKEN_MUL_ASSIGN,   "*="},
        {"/=",  TOKEN_DIV_ASSIGN,   "/="},
        {"%=",  TOKEN_MOD_ASSIGN,   "%="},
        {"==",  TOKEN_EQ,           "=="},
        {"!=",  TOKEN_NE,           "!="},
        {"<=",  TOKEN_LE,           "<="},
        {">=",  TOKEN_GE,           ">="},
        {"<<",  TOKEN_LSHIFT,       "<<"},
        {">>",  TOKEN_RSHIFT,       ">>"},
        {"<-",  TOKEN_ARROW,        "<-"},
        {"&&",  TOKEN_AND,          "&&"},
        {"||",  TOKEN_OR,           "||"},
        {"&=",  TOKEN_AND_ASSIGN,   "&="},
        {"|=",  TOKEN_OR_ASSIGN,    "|="},
        {"^=",  TOKEN_XOR_ASSIGN,   "^="},
        {"<<=", TOKEN_LSHIFT_ASSIGN,"<<="},
        {">>=", TOKEN_RSHIFT_ASSIGN,">>="},
        {"...", TOKEN_ELLIPSIS,     "..."},
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        if (!check_token(cases[i].input, cases[i].type, cases[i].lit)) {
            printf("  FAIL: operator '%s'\n", cases[i].input);
            return TEST_FAIL;
        }
    }
    return TEST_PASS;
}

// =========================================================================
// Test: Keywords
// =========================================================================
static TestStatus test_keywords(void* ctx) {
    (void)ctx;

    struct { const char* input; TokenType type; } cases[] = {
        {"break",    TOKEN_BREAK},
        {"case",     TOKEN_CASE},
        {"chan",      TOKEN_CHAN},
        {"const",    TOKEN_CONST},
        {"continue", TOKEN_CONTINUE},
        {"default",  TOKEN_DEFAULT},
        {"defer",    TOKEN_DEFER},
        {"else",     TOKEN_ELSE},
        {"for",      TOKEN_FOR},
        {"func",     TOKEN_FUNC},
        {"go",       TOKEN_GO},
        {"if",       TOKEN_IF},
        {"import",   TOKEN_IMPORT},
        {"interface",TOKEN_INTERFACE},
        {"map",      TOKEN_MAP},
        {"package",  TOKEN_PACKAGE},
        {"range",    TOKEN_RANGE},
        {"return",   TOKEN_RETURN},
        {"select",   TOKEN_SELECT},
        {"struct",   TOKEN_STRUCT},
        {"switch",   TOKEN_SWITCH},
        {"type",     TOKEN_TYPE},
        {"var",      TOKEN_VAR},
        {"true",     TOKEN_TRUE},
        {"false",    TOKEN_FALSE},
        {"nil",      TOKEN_NIL},
        {"try",      TOKEN_TRY},
        {"catch",    TOKEN_CATCH},
        {"unsafe",   TOKEN_UNSAFE},
        {"match",    TOKEN_MATCH},
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        if (!check_token(cases[i].input, cases[i].type, cases[i].input)) {
            printf("  FAIL: keyword '%s'\n", cases[i].input);
            return TEST_FAIL;
        }
    }
    return TEST_PASS;
}

// =========================================================================
// Test: Identifiers
// =========================================================================
static TestStatus test_identifiers(void* ctx) {
    (void)ctx;

    struct { const char* input; const char* expected; } cases[] = {
        {"foo",       "foo"},
        {"_bar",      "_bar"},
        {"camelCase", "camelCase"},
        {"x123",      "x123"},
        {"_",         "_"},
        {"__init__",  "__init__"},
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        if (!check_token(cases[i].input, TOKEN_IDENT, cases[i].expected)) {
            printf("  FAIL: identifier '%s'\n", cases[i].input);
            return TEST_FAIL;
        }
    }
    return TEST_PASS;
}

// =========================================================================
// Test: Integer literals
// =========================================================================
static TestStatus test_integer_literals(void* ctx) {
    (void)ctx;

    struct { const char* input; const char* expected; } cases[] = {
        {"0",       "0"},
        {"42",      "42"},
        {"12345",   "12345"},
        {"9999999", "9999999"},
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        if (!check_token(cases[i].input, TOKEN_INT, cases[i].expected)) {
            printf("  FAIL: integer '%s'\n", cases[i].input);
            return TEST_FAIL;
        }
    }
    return TEST_PASS;
}

// =========================================================================
// Test: Float literals
// =========================================================================
static TestStatus test_float_literals(void* ctx) {
    (void)ctx;

    struct { const char* input; const char* expected; } cases[] = {
        {"3.14",    "3.14"},
        {"0.5",     "0.5"},
        {"1e10",    "1e10"},
        {"2.5E-3",  "2.5E-3"},
        {"1.0e+5",  "1.0e+5"},
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        if (!check_token(cases[i].input, TOKEN_FLOAT, cases[i].expected)) {
            printf("  FAIL: float '%s'\n", cases[i].input);
            return TEST_FAIL;
        }
    }
    return TEST_PASS;
}

// =========================================================================
// Test: String literals
// =========================================================================
static TestStatus test_string_literals(void* ctx) {
    (void)ctx;

    // Basic string
    if (!check_token("\"hello\"", TOKEN_STRING, "hello")) {
        printf("  FAIL: basic string\n");
        return TEST_FAIL;
    }

    // String with escape
    if (!check_token("\"a\\nb\"", TOKEN_STRING, "a\\nb")) {
        printf("  FAIL: string with escape\n");
        return TEST_FAIL;
    }

    // Empty string
    if (!check_token("\"\"", TOKEN_STRING, "")) {
        printf("  FAIL: empty string\n");
        return TEST_FAIL;
    }

    return TEST_PASS;
}

// =========================================================================
// Test: Unterminated string -> TOKEN_ERROR
// =========================================================================
static TestStatus test_unterminated_string(void* ctx) {
    (void)ctx;
    if (!check_token("\"unterminated", TOKEN_ERROR, NULL)) {
        printf("  FAIL: unterminated string should produce TOKEN_ERROR\n");
        return TEST_FAIL;
    }
    return TEST_PASS;
}

// =========================================================================
// Test: Char literals
// =========================================================================
static TestStatus test_char_literals(void* ctx) {
    (void)ctx;

    if (!check_token("'a'", TOKEN_CHAR, "a")) {
        printf("  FAIL: basic char\n");
        return TEST_FAIL;
    }

    // Escaped char
    if (!check_token("'\\n'", TOKEN_CHAR, "\\n")) {
        printf("  FAIL: escaped char\n");
        return TEST_FAIL;
    }

    return TEST_PASS;
}

// =========================================================================
// Test: Unterminated char literal -> TOKEN_ERROR
// =========================================================================
static TestStatus test_unterminated_char(void* ctx) {
    (void)ctx;
    if (!check_token("'a", TOKEN_ERROR, NULL)) {
        printf("  FAIL: unterminated char should produce TOKEN_ERROR\n");
        return TEST_FAIL;
    }
    return TEST_PASS;
}

// =========================================================================
// Test: Line comments produce no tokens
// =========================================================================
static TestStatus test_line_comments(void* ctx) {
    (void)ctx;

    // A line comment followed by EOF should only produce EOF
    int count = count_tokens("// this is a comment");
    if (count != 0) {
        printf("  FAIL: line comment produced %d tokens (expected 0)\n", count);
        return TEST_FAIL;
    }

    // Comment followed by a token
    Lexer* lexer = lexer_new("// comment\n42", "test");
    Token* tok = lexer_next_token(lexer);
    if (!tok || tok->type != TOKEN_INT || strcmp(tok->literal, "42") != 0) {
        printf("  FAIL: token after line comment\n");
        token_free(tok);
        lexer_free(lexer);
        return TEST_FAIL;
    }
    token_free(tok);
    lexer_free(lexer);

    return TEST_PASS;
}

// =========================================================================
// Test: Block comments produce no tokens
// =========================================================================
static TestStatus test_block_comments(void* ctx) {
    (void)ctx;

    int count = count_tokens("/* block comment */");
    if (count != 0) {
        printf("  FAIL: block comment produced %d tokens (expected 0)\n", count);
        return TEST_FAIL;
    }

    // Token after block comment
    Lexer* lexer = lexer_new("/* comment */ 99", "test");
    Token* tok = lexer_next_token(lexer);
    if (!tok || tok->type != TOKEN_INT || strcmp(tok->literal, "99") != 0) {
        printf("  FAIL: token after block comment\n");
        token_free(tok);
        lexer_free(lexer);
        return TEST_FAIL;
    }
    token_free(tok);
    lexer_free(lexer);

    return TEST_PASS;
}

// =========================================================================
// Test: Empty input -> EOF
// =========================================================================
static TestStatus test_empty_input(void* ctx) {
    (void)ctx;
    if (!check_token("", TOKEN_EOF, NULL)) {
        printf("  FAIL: empty input should produce TOKEN_EOF\n");
        return TEST_FAIL;
    }
    return TEST_PASS;
}

// =========================================================================
// Test: Whitespace-only input -> EOF
// =========================================================================
static TestStatus test_whitespace_only(void* ctx) {
    (void)ctx;
    int count = count_tokens("   \t\t   ");
    if (count != 0) {
        printf("  FAIL: whitespace-only produced %d tokens (expected 0)\n", count);
        return TEST_FAIL;
    }
    return TEST_PASS;
}

// =========================================================================
// Test: Many consecutive newlines (validates Task 4 iteration fix)
// =========================================================================
static TestStatus test_many_newlines(void* ctx) {
    (void)ctx;

    // Build a string of 10000 newlines followed by a token
    size_t n = 10000;
    char* input = malloc(n + 4);
    if (!input) return TEST_ERROR;
    memset(input, '\n', n);
    input[n] = '4';
    input[n+1] = '2';
    input[n+2] = '\0';

    Lexer* lexer = lexer_new(input, "test");
    Token* tok = lexer_next_token(lexer);

    int ok = (tok && tok->type == TOKEN_INT && strcmp(tok->literal, "42") == 0);

    token_free(tok);
    lexer_free(lexer);
    free(input);

    if (!ok) {
        printf("  FAIL: 10000 newlines before token\n");
        return TEST_FAIL;
    }
    return TEST_PASS;
}

// =========================================================================
// Test: Type name register/check/clear round-trip
// =========================================================================
static TestStatus test_type_names(void* ctx) {
    (void)ctx;

    Lexer* lexer = lexer_new("MyType", "test");
    if (!lexer) return TEST_ERROR;

    // Not registered yet — should be TOKEN_IDENT
    Token* tok = lexer_next_token(lexer);
    if (!tok || tok->type != TOKEN_IDENT) {
        printf("  FAIL: unregistered type name should be IDENT\n");
        token_free(tok);
        lexer_free(lexer);
        return TEST_FAIL;
    }
    token_free(tok);
    lexer_free(lexer);

    // Register and lex again
    lexer = lexer_new("MyType", "test");
    lexer_register_type_name(lexer, "MyType");
    tok = lexer_next_token(lexer);
    if (!tok || tok->type != TOKEN_TYPE_IDENT) {
        printf("  FAIL: registered type name should be TYPE_IDENT\n");
        token_free(tok);
        lexer_free(lexer);
        return TEST_FAIL;
    }
    token_free(tok);

    // Clear and check again
    lexer_clear_type_names(lexer);
    if (lexer_is_type_name(lexer, "MyType")) {
        printf("  FAIL: cleared type name should not be found\n");
        lexer_free(lexer);
        return TEST_FAIL;
    }

    lexer_free(lexer);
    return TEST_PASS;
}

// =========================================================================
// Test: Multiple tokens in sequence
// =========================================================================
static TestStatus test_token_sequence(void* ctx) {
    (void)ctx;

    Lexer* lexer = lexer_new("x := 42 + y", "test");
    TokenType expected[] = {TOKEN_IDENT, TOKEN_SHORT_ASSIGN, TOKEN_INT, TOKEN_PLUS, TOKEN_IDENT, TOKEN_EOF};

    for (int i = 0; i < 6; i++) {
        Token* tok = lexer_next_token(lexer);
        if (!tok || tok->type != expected[i]) {
            printf("  FAIL: token %d expected %d got %d\n", i, expected[i], tok ? (int)tok->type : -1);
            token_free(tok);
            lexer_free(lexer);
            return TEST_FAIL;
        }
        token_free(tok);
    }

    lexer_free(lexer);
    return TEST_PASS;
}

// =========================================================================
// Test: Position tracking
// =========================================================================
static TestStatus test_position_tracking(void* ctx) {
    (void)ctx;

    Lexer* lexer = lexer_new("a\nb", "test.goo");
    Token* tok1 = lexer_next_token(lexer);
    Token* tok2 = lexer_next_token(lexer);

    int ok = 1;
    if (!tok1 || tok1->pos.line != 1) { ok = 0; printf("  FAIL: first token line\n"); }
    if (!tok2 || tok2->pos.line != 2) { ok = 0; printf("  FAIL: second token line\n"); }

    token_free(tok1);
    token_free(tok2);
    lexer_free(lexer);
    return ok ? TEST_PASS : TEST_FAIL;
}

// =========================================================================
// Test: Automatic semicolon insertion (ASI)
// =========================================================================

// Collect token types (excluding EOF) into out[], up to max. Returns count.
static int collect_token_types(const char* input, TokenType* out, int max) {
    Lexer* lexer = lexer_new(input, "test");
    if (!lexer) return -1;

    int n = 0;
    for (;;) {
        Token* tok = lexer_next_token(lexer);
        if (!tok) break;
        if (tok->type == TOKEN_EOF) { token_free(tok); break; }
        if (n < max) out[n] = tok->type;
        n++;
        token_free(tok);
    }
    lexer_free(lexer);
    return n;
}

static TestStatus test_asi(void* ctx) {
    (void)ctx;

    struct {
        const char* name;
        const char* input;
        TokenType expected[8];
        int n;
    } cases[] = {
        // A newline after a value-ending token inserts a semicolon.
        {"after literal", "x := 5\n",
         {TOKEN_IDENT, TOKEN_SHORT_ASSIGN, TOKEN_INT, TOKEN_SEMICOLON}, 4},
        // Each statement line is terminated.
        {"two statements", "a\nb\n",
         {TOKEN_IDENT, TOKEN_SEMICOLON, TOKEN_IDENT, TOKEN_SEMICOLON}, 4},
        // No insertion after an operator (the statement continues next line).
        {"no insert after operator", "a +\nb\n",
         {TOKEN_IDENT, TOKEN_PLUS, TOKEN_IDENT, TOKEN_SEMICOLON}, 4},
        // Newlines inside parentheses are suppressed.
        {"suppressed in parens", "(\na\n)\n",
         {TOKEN_LPAREN, TOKEN_IDENT, TOKEN_RPAREN, TOKEN_SEMICOLON}, 4},
        // Consecutive blank lines never produce consecutive semicolons.
        {"no double semicolon", "a\n\n\nb\n",
         {TOKEN_IDENT, TOKEN_SEMICOLON, TOKEN_IDENT, TOKEN_SEMICOLON}, 4},
    };

    int ok = 1;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        TokenType got[8];
        int n = collect_token_types(cases[c].input, got, 8);
        if (n != cases[c].n) {
            ok = 0;
            printf("  FAIL: %s: expected %d tokens, got %d\n", cases[c].name, cases[c].n, n);
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (got[i] != cases[c].expected[i]) {
                ok = 0;
                printf("  FAIL: %s: token %d expected %d, got %d\n",
                       cases[c].name, i, cases[c].expected[i], got[i]);
            }
        }
    }
    return ok ? TEST_PASS : TEST_FAIL;
}

// =========================================================================
// Main: register all tests
// =========================================================================
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    register_test("lexer", "single_char_tokens",   test_single_char_tokens,   __FILE__, __LINE__);
    register_test("lexer", "multi_char_operators",  test_multi_char_operators, __FILE__, __LINE__);
    register_test("lexer", "keywords",              test_keywords,             __FILE__, __LINE__);
    register_test("lexer", "identifiers",           test_identifiers,          __FILE__, __LINE__);
    register_test("lexer", "integer_literals",       test_integer_literals,     __FILE__, __LINE__);
    register_test("lexer", "float_literals",         test_float_literals,       __FILE__, __LINE__);
    register_test("lexer", "string_literals",        test_string_literals,      __FILE__, __LINE__);
    register_test("lexer", "unterminated_string",    test_unterminated_string,  __FILE__, __LINE__);
    register_test("lexer", "char_literals",          test_char_literals,        __FILE__, __LINE__);
    register_test("lexer", "unterminated_char",      test_unterminated_char,    __FILE__, __LINE__);
    register_test("lexer", "line_comments",          test_line_comments,        __FILE__, __LINE__);
    register_test("lexer", "block_comments",         test_block_comments,       __FILE__, __LINE__);
    register_test("lexer", "empty_input",            test_empty_input,          __FILE__, __LINE__);
    register_test("lexer", "whitespace_only",        test_whitespace_only,      __FILE__, __LINE__);
    register_test("lexer", "many_newlines",          test_many_newlines,        __FILE__, __LINE__);
    register_test("lexer", "type_names",             test_type_names,           __FILE__, __LINE__);
    register_test("lexer", "token_sequence",         test_token_sequence,       __FILE__, __LINE__);
    register_test("lexer", "position_tracking",      test_position_tracking,    __FILE__, __LINE__);
    register_test("lexer", "asi",                    test_asi,                  __FILE__, __LINE__);

    TestOptions opts = {0};
    opts.verbose = true;
    run_all_tests(&opts);

    return 0;
}
