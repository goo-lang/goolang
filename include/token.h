#ifndef TOKEN_H
#define TOKEN_H

#include <stddef.h>

// Token types for Goo language
typedef enum {
    // End of file
    TOKEN_EOF = 0,
    
    // Literals
    TOKEN_IDENT,        // identifiers
    TOKEN_INT,          // integer literals
    TOKEN_FLOAT,        // floating point literals
    TOKEN_STRING,       // string literals
    TOKEN_CHAR,         // character literals
    TOKEN_TRUE,         // true
    TOKEN_FALSE,        // false
    TOKEN_NIL,          // nil
    
    // Go Keywords
    TOKEN_BREAK,        // break
    TOKEN_CASE,         // case
    TOKEN_CHAN,         // chan
    TOKEN_CONST,        // const
    TOKEN_CONTINUE,     // continue
    TOKEN_DEFAULT,      // default
    TOKEN_DEFER,        // defer
    TOKEN_ELSE,         // else
    TOKEN_FALLTHROUGH, // fallthrough
    TOKEN_FOR,          // for
    TOKEN_FUNC,         // func
    TOKEN_GO,           // go
    TOKEN_GOTO,         // goto
    TOKEN_IF,           // if
    TOKEN_IMPORT,       // import
    TOKEN_INTERFACE,    // interface
    TOKEN_MAP,          // map
    TOKEN_PACKAGE,      // package
    TOKEN_RANGE,        // range
    TOKEN_RETURN,       // return
    TOKEN_SELECT,       // select
    TOKEN_STRUCT,       // struct
    TOKEN_SWITCH,       // switch
    TOKEN_TYPE,         // type
    TOKEN_VAR,          // var
    
    // Goo Extensions - Keywords
    TOKEN_COMPTIME,     // comptime
    TOKEN_PUB,          // pub (channel pattern)
    TOKEN_SUB,          // sub (channel pattern)
    TOKEN_REQ,          // req (channel pattern)
    TOKEN_REP,          // rep (channel pattern)
    TOKEN_PUSH,         // push (channel pattern)
    TOKEN_PULL,         // pull (channel pattern)
    TOKEN_TRY,          // try (error handling)
    TOKEN_CATCH,        // catch (error handling)
    TOKEN_UNSAFE,       // unsafe
    TOKEN_OWNED,        // owned (ownership)
    TOKEN_BORROWED,     // borrowed (ownership)
    TOKEN_SHARED,       // shared (ownership)
    TOKEN_LET,          // let (nullable unwrapping)
    
    // Operators and punctuation
    TOKEN_PLUS,         // +
    TOKEN_MINUS,        // -
    TOKEN_MULTIPLY,     // *
    TOKEN_DIVIDE,       // /
    TOKEN_MODULO,       // %
    TOKEN_ASSIGN,       // =
    TOKEN_SHORT_ASSIGN, // :=
    TOKEN_PLUS_ASSIGN,  // +=
    TOKEN_MINUS_ASSIGN, // -=
    TOKEN_MUL_ASSIGN,   // *=
    TOKEN_DIV_ASSIGN,   // /=
    TOKEN_MOD_ASSIGN,   // %=
    TOKEN_AND_ASSIGN,   // &=
    TOKEN_OR_ASSIGN,    // |=
    TOKEN_XOR_ASSIGN,   // ^=
    TOKEN_LSHIFT_ASSIGN,// <<=
    TOKEN_RSHIFT_ASSIGN,// >>=
    
    // Comparison operators
    TOKEN_EQ,           // ==
    TOKEN_NE,           // !=
    TOKEN_LT,           // <
    TOKEN_LE,           // <=
    TOKEN_GT,           // >
    TOKEN_GE,           // >=
    
    // Logical operators
    TOKEN_AND,          // &&
    TOKEN_OR,           // ||
    TOKEN_NOT,          // !
    
    // Bitwise operators
    TOKEN_BIT_AND,      // &
    TOKEN_BIT_OR,       // |
    TOKEN_BIT_XOR,      // ^
    TOKEN_BIT_NOT,      // ~
    TOKEN_LSHIFT,       // <<
    TOKEN_RSHIFT,       // >>
    
    // Increment/Decrement
    TOKEN_INCREMENT,    // ++
    TOKEN_DECREMENT,    // --
    
    // Channel operators
    TOKEN_ARROW,        // <-
    
    // Goo Extensions - Operators
    TOKEN_BANG,         // ! (error union marker)
    TOKEN_QUESTION,     // ? (nullable type marker)
    TOKEN_TRY_OP,       // try operator
    TOKEN_CATCH_OP,     // catch operator
    
    // Delimiters
    TOKEN_LPAREN,       // (
    TOKEN_RPAREN,       // )
    TOKEN_LBRACE,       // {
    TOKEN_RBRACE,       // }
    TOKEN_LBRACKET,     // [
    TOKEN_RBRACKET,     // ]
    TOKEN_SEMICOLON,    // ;
    TOKEN_COMMA,        // ,
    TOKEN_DOT,          // .
    TOKEN_COLON,        // :
    TOKEN_ELLIPSIS,     // ...
    
    // Special
    TOKEN_NEWLINE,      // \n
    TOKEN_ERROR,        // Error token
    TOKEN_UNKNOWN,      // Unknown token
    
    TOKEN_COUNT         // Total number of token types
} TokenType;

// Position information for tokens
typedef struct {
    int line;
    int column;
    int offset;
    const char* filename;
} Position;

// Token structure
typedef struct {
    TokenType type;
    char* literal;      // The actual text of the token
    size_t length;      // Length of the literal
    Position pos;       // Position in source code
} Token;

// Function declarations
const char* token_type_string(TokenType type);
Token* token_new(TokenType type, const char* literal, size_t length, Position pos);
void token_free(Token* token);
Token* token_copy(const Token* token);
int token_is_keyword(const char* literal);
TokenType token_keyword_type(const char* literal);

#endif // TOKEN_H