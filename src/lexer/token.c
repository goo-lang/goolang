#include "token.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Token type to string mapping
static const char* token_strings[] = {
    [TOKEN_EOF] = "EOF",
    [TOKEN_IDENT] = "IDENT",
    [TOKEN_TYPE_IDENT] = "TYPE_IDENT",
    [TOKEN_INT] = "INT",
    [TOKEN_FLOAT] = "FLOAT",
    [TOKEN_STRING] = "STRING",
    [TOKEN_CHAR] = "CHAR",
    [TOKEN_TRUE] = "TRUE",
    [TOKEN_FALSE] = "FALSE",
    [TOKEN_NIL] = "NIL",
    
    // Go Keywords
    [TOKEN_BREAK] = "BREAK",
    [TOKEN_CASE] = "CASE",
    [TOKEN_CHAN] = "CHAN",
    [TOKEN_CONST] = "CONST",
    [TOKEN_CONTINUE] = "CONTINUE",
    [TOKEN_DEFAULT] = "DEFAULT",
    [TOKEN_DEFER] = "DEFER",
    [TOKEN_ELSE] = "ELSE",
    [TOKEN_FALLTHROUGH] = "FALLTHROUGH",
    [TOKEN_FOR] = "FOR",
    [TOKEN_FUNC] = "FUNC",
    [TOKEN_GO] = "GO",
    [TOKEN_GOTO] = "GOTO",
    [TOKEN_IF] = "IF",
    [TOKEN_IMPORT] = "IMPORT",
    [TOKEN_INTERFACE] = "INTERFACE",
    [TOKEN_MAP] = "MAP",
    [TOKEN_PACKAGE] = "PACKAGE",
    [TOKEN_RANGE] = "RANGE",
    [TOKEN_RETURN] = "RETURN",
    [TOKEN_SELECT] = "SELECT",
    [TOKEN_STRUCT] = "STRUCT",
    [TOKEN_SWITCH] = "SWITCH",
    [TOKEN_TYPE] = "TYPE",
    [TOKEN_VAR] = "VAR",
    
    // Goo Extensions
    [TOKEN_COMPTIME] = "COMPTIME",
    [TOKEN_CONCEPT] = "CONCEPT",
    [TOKEN_PUB] = "PUB",
    [TOKEN_SUB] = "SUB",
    [TOKEN_REQ] = "REQ",
    [TOKEN_REP] = "REP",
    [TOKEN_PUSH] = "PUSH",
    [TOKEN_PULL] = "PULL",
    [TOKEN_TRY] = "TRY",
    [TOKEN_CATCH] = "CATCH",
    [TOKEN_UNSAFE] = "UNSAFE",
    [TOKEN_ASM] = "ASM",
    [TOKEN_EXTERN] = "EXTERN",
    [TOKEN_FROM] = "FROM",
    [TOKEN_VOLATILE] = "VOLATILE",
    [TOKEN_INLINE] = "INLINE",
    [TOKEN_NO_STD] = "NO_STD",
    [TOKEN_PARALLEL] = "PARALLEL",
    [TOKEN_REDUCE] = "REDUCE",
    [TOKEN_BARRIER] = "BARRIER",
    [TOKEN_ATOMIC] = "ATOMIC",
    [TOKEN_THREAD_LOCAL] = "THREAD_LOCAL",
    [TOKEN_OWNED] = "OWNED",
    [TOKEN_BORROWED] = "BORROWED",
    [TOKEN_SHARED] = "SHARED",
    [TOKEN_LET] = "LET",
    [TOKEN_MATCH] = "MATCH",
    [TOKEN_KERNEL] = "KERNEL",
    [TOKEN_DEVICE] = "DEVICE",
    [TOKEN_HOST] = "HOST",
    [TOKEN_GLOBAL] = "GLOBAL",
    [TOKEN_SHARED_MEM] = "SHARED_MEM",
    [TOKEN_CONSTANT] = "CONSTANT",
    [TOKEN_LOCAL] = "LOCAL",
    
    // WebAssembly Support
    [TOKEN_WASM] = "WASM",
    [TOKEN_EXPORT] = "EXPORT",
    [TOKEN_MEMORY] = "MEMORY",
    [TOKEN_TABLE] = "TABLE",
    [TOKEN_GLOBAL_WASM] = "GLOBAL_WASM",
    [TOKEN_FUNC_WASM] = "FUNC_WASM",
    [TOKEN_TYPE_WASM] = "TYPE_WASM",
    [TOKEN_START] = "START",
    [TOKEN_ELEM] = "ELEM",
    [TOKEN_DATA] = "DATA",
    
    // Operators
    [TOKEN_PLUS] = "PLUS",
    [TOKEN_MINUS] = "MINUS",
    [TOKEN_MULTIPLY] = "MULTIPLY",
    [TOKEN_DIVIDE] = "DIVIDE",
    [TOKEN_MODULO] = "MODULO",
    [TOKEN_ASSIGN] = "ASSIGN",
    [TOKEN_SHORT_ASSIGN] = "SHORT_ASSIGN",
    [TOKEN_PLUS_ASSIGN] = "PLUS_ASSIGN",
    [TOKEN_MINUS_ASSIGN] = "MINUS_ASSIGN",
    [TOKEN_MUL_ASSIGN] = "MUL_ASSIGN",
    [TOKEN_DIV_ASSIGN] = "DIV_ASSIGN",
    [TOKEN_MOD_ASSIGN] = "MOD_ASSIGN",
    [TOKEN_AND_ASSIGN] = "AND_ASSIGN",
    [TOKEN_OR_ASSIGN] = "OR_ASSIGN",
    [TOKEN_XOR_ASSIGN] = "XOR_ASSIGN",
    [TOKEN_LSHIFT_ASSIGN] = "LSHIFT_ASSIGN",
    [TOKEN_RSHIFT_ASSIGN] = "RSHIFT_ASSIGN",
    
    [TOKEN_EQ] = "EQ",
    [TOKEN_NE] = "NE",
    [TOKEN_LT] = "LT",
    [TOKEN_LE] = "LE",
    [TOKEN_GT] = "GT",
    [TOKEN_GE] = "GE",
    
    [TOKEN_AND] = "AND",
    [TOKEN_OR] = "OR",
    [TOKEN_NOT] = "NOT",
    
    [TOKEN_BIT_AND] = "BIT_AND",
    [TOKEN_BIT_OR] = "BIT_OR",
    [TOKEN_BIT_XOR] = "BIT_XOR",
    [TOKEN_BIT_NOT] = "BIT_NOT",
    [TOKEN_LSHIFT] = "LSHIFT",
    [TOKEN_RSHIFT] = "RSHIFT",
    
    [TOKEN_INCREMENT] = "INCREMENT",
    [TOKEN_DECREMENT] = "DECREMENT",
    
    [TOKEN_ARROW] = "ARROW",
    [TOKEN_BANG] = "BANG",
    [TOKEN_QUESTION] = "QUESTION",
    [TOKEN_TRY_OP] = "TRY_OP",
    [TOKEN_CATCH_OP] = "CATCH_OP",
    [TOKEN_DEREF] = "DEREF",
    [TOKEN_ADDR_OF] = "ADDR_OF",
    [TOKEN_ATTRIBUTE] = "ATTRIBUTE",
    
    [TOKEN_LPAREN] = "LPAREN",
    [TOKEN_RPAREN] = "RPAREN",
    [TOKEN_LBRACE] = "LBRACE",
    [TOKEN_RBRACE] = "RBRACE",
    [TOKEN_LBRACKET] = "LBRACKET",
    [TOKEN_RBRACKET] = "RBRACKET",
    [TOKEN_SEMICOLON] = "SEMICOLON",
    [TOKEN_COMMA] = "COMMA",
    [TOKEN_DOT] = "DOT",
    [TOKEN_COLON] = "COLON",
    [TOKEN_ELLIPSIS] = "ELLIPSIS",
    
    [TOKEN_NEWLINE] = "NEWLINE",
    [TOKEN_ERROR] = "ERROR",
    [TOKEN_UNKNOWN] = "UNKNOWN",
};

// Keyword lookup table
typedef struct {
    const char* keyword;
    TokenType type;
} KeywordEntry;

static const KeywordEntry keywords[] = {
    // Go keywords
    {"break", TOKEN_BREAK},
    {"case", TOKEN_CASE},
    {"chan", TOKEN_CHAN},
    {"const", TOKEN_CONST},
    {"continue", TOKEN_CONTINUE},
    {"default", TOKEN_DEFAULT},
    {"defer", TOKEN_DEFER},
    {"else", TOKEN_ELSE},
    {"fallthrough", TOKEN_FALLTHROUGH},
    {"false", TOKEN_FALSE},
    {"for", TOKEN_FOR},
    {"func", TOKEN_FUNC},
    {"go", TOKEN_GO},
    {"goto", TOKEN_GOTO},
    {"if", TOKEN_IF},
    {"import", TOKEN_IMPORT},
    {"interface", TOKEN_INTERFACE},
    {"map", TOKEN_MAP},
    {"nil", TOKEN_NIL},
    {"package", TOKEN_PACKAGE},
    {"range", TOKEN_RANGE},
    {"return", TOKEN_RETURN},
    {"select", TOKEN_SELECT},
    {"struct", TOKEN_STRUCT},
    {"switch", TOKEN_SWITCH},
    {"true", TOKEN_TRUE},
    {"type", TOKEN_TYPE},
    {"var", TOKEN_VAR},
    
    // Goo extensions
    {"comptime", TOKEN_COMPTIME},
    {"concept", TOKEN_CONCEPT},
    {"pub", TOKEN_PUB},
    {"sub", TOKEN_SUB},
    {"req", TOKEN_REQ},
    {"rep", TOKEN_REP},
    {"push", TOKEN_PUSH},
    {"pull", TOKEN_PULL},
    {"try", TOKEN_TRY},
    {"catch", TOKEN_CATCH},
    {"unsafe", TOKEN_UNSAFE},
    {"asm", TOKEN_ASM},
    {"extern", TOKEN_EXTERN},
    {"from", TOKEN_FROM},
    {"volatile", TOKEN_VOLATILE},
    {"inline", TOKEN_INLINE},
    {"no_std", TOKEN_NO_STD},
    {"parallel", TOKEN_PARALLEL},
    {"reduce", TOKEN_REDUCE},
    {"barrier", TOKEN_BARRIER},
    {"atomic", TOKEN_ATOMIC},
    {"threadLocal", TOKEN_THREAD_LOCAL},
    {"owned", TOKEN_OWNED},
    {"borrowed", TOKEN_BORROWED},
    {"shared", TOKEN_SHARED},
    {"let", TOKEN_LET},
    {"match", TOKEN_MATCH},
    {"kernel", TOKEN_KERNEL},
    {"device", TOKEN_DEVICE},
    {"host", TOKEN_HOST},
    {"global", TOKEN_GLOBAL},
    {"sharedMem", TOKEN_SHARED_MEM},
    {"constant", TOKEN_CONSTANT},
    {"local", TOKEN_LOCAL},
    
    // WebAssembly keywords
    {"wasm", TOKEN_WASM},
    {"export", TOKEN_EXPORT},
    {"memory", TOKEN_MEMORY},
    {"table", TOKEN_TABLE},
    {"start", TOKEN_START},
    {"elem", TOKEN_ELEM},
    {"data", TOKEN_DATA},
    
    {NULL, TOKEN_UNKNOWN} // Sentinel
};

const char* token_type_string(TokenType type) {
    if (type >= 0 && type < TOKEN_COUNT) {
        return token_strings[type];
    }
    return "INVALID";
}

Token* token_new(TokenType type, const char* literal, size_t length, Position pos) {
    Token* token = malloc(sizeof(Token));
    if (!token) return NULL;
    
    token->type = type;
    token->length = length;
    token->pos = pos;
    
    if (literal && length > 0) {
        token->literal = malloc(length + 1);
        if (!token->literal) {
            free(token);
            return NULL;
        }
        strncpy(token->literal, literal, length);
        token->literal[length] = '\0';
    } else {
        token->literal = NULL;
    }
    
    return token;
}

void token_free(Token* token) {
    if (token) {
        free(token->literal);
        free(token);
    }
}

Token* token_copy(const Token* token) {
    if (!token) return NULL;
    return token_new(token->type, token->literal, token->length, token->pos);
}

int token_is_keyword(const char* literal) {
    for (const KeywordEntry* entry = keywords; entry->keyword; entry++) {
        if (strcmp(literal, entry->keyword) == 0) {
            return 1;
        }
    }
    return 0;
}

TokenType token_keyword_type(const char* literal) {
    for (const KeywordEntry* entry = keywords; entry->keyword; entry++) {
        if (strcmp(literal, entry->keyword) == 0) {
            return entry->type;
        }
    }
    return TOKEN_IDENT;
}