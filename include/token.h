#ifndef TOKEN_H
#define TOKEN_H

#include <stddef.h>

// Token types for Goo language
typedef enum {
    // End of file
    TOKEN_EOF = 0,
    
    // Literals
    TOKEN_IDENT,        // identifiers (variables, functions)
    TOKEN_TYPE_IDENT,   // type identifiers (struct names, etc.)
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
    TOKEN_CONCEPT,      // concept (concept-based generics)
    TOKEN_PUB,          // pub (channel pattern)
    TOKEN_SUB,          // sub (channel pattern)
    TOKEN_REQ,          // req (channel pattern)
    TOKEN_REP,          // rep (channel pattern)
    TOKEN_PUSH,         // push (channel pattern)
    TOKEN_PULL,         // pull (channel pattern)
    TOKEN_TRY,          // try (error handling)
    TOKEN_CATCH,        // catch (error handling)
    TOKEN_UNSAFE,       // unsafe
    TOKEN_ASM,          // asm (inline assembly)
    TOKEN_EXTERN,       // extern (FFI declarations)
    TOKEN_FROM,         // from (extern library)
    TOKEN_VOLATILE,     // volatile (memory operations)
    TOKEN_INLINE,       // inline (inline functions)
    TOKEN_NO_STD,       // no_std (attribute)
    TOKEN_PARALLEL,     // parallel (data parallel processing)
    TOKEN_REDUCE,       // reduce (parallel reduction)
    TOKEN_BARRIER,      // barrier (synchronization)
    TOKEN_ATOMIC,       // atomic (atomic operations)
    TOKEN_THREAD_LOCAL, // threadLocal (thread-local storage)
    TOKEN_OWNED,        // owned (ownership)
    TOKEN_BORROWED,     // borrowed (ownership)
    TOKEN_SHARED,       // shared (ownership)
    TOKEN_LET,          // let (nullable unwrapping)
    TOKEN_MATCH,        // match (pattern matching)
    TOKEN_KERNEL,       // kernel (GPU kernel function)
    TOKEN_DEVICE,       // device (GPU device qualifier)
    TOKEN_HOST,         // host (CPU host qualifier)
    TOKEN_GLOBAL,       // global (GPU global memory)
    TOKEN_SHARED_MEM,   // shared (GPU shared memory)
    TOKEN_CONSTANT,     // constant (GPU constant memory)
    TOKEN_LOCAL,        // local (GPU local memory)
    
    // WebAssembly Support
    TOKEN_WASM,         // wasm (WebAssembly target)
    TOKEN_EXPORT,       // export (WASM export)
    TOKEN_MEMORY,       // memory (WASM linear memory)
    TOKEN_TABLE,        // table (WASM table)
    TOKEN_GLOBAL_WASM,  // global (WASM global variable)
    TOKEN_FUNC_WASM,    // func (WASM function)
    TOKEN_TYPE_WASM,    // type (WASM type)
    TOKEN_START,        // start (WASM start function)
    TOKEN_ELEM,         // elem (WASM element segment)
    TOKEN_DATA,         // data (WASM data segment)
    
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
    TOKEN_DEREF,        // @ (explicit dereference operator / attribute marker)
    TOKEN_ADDR_OF,      // & (address-of operator, may conflict with BIT_AND)
    TOKEN_ATTRIBUTE,    // @ (context-sensitive: attribute when followed by identifier)
    
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