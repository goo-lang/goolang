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
        case TOKEN_FOR: return FOR;
        case TOKEN_FUNC: return FUNC;
        case TOKEN_GO: return GO;
        case TOKEN_GOTO: return GOTO;
        case TOKEN_IF: return IF;
        case TOKEN_IMPORT: return IMPORT;
        case TOKEN_INTERFACE: return INTERFACE;
        case TOKEN_MAP: return MAP;
        case TOKEN_PACKAGE: return PACKAGE;
        case TOKEN_RANGE: return RANGE;
        case TOKEN_RETURN: return RETURN;
        case TOKEN_SELECT: return SELECT;
        case TOKEN_STRUCT: return STRUCT;
        case TOKEN_SWITCH: return SWITCH;
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
        case TOKEN_OWNED: return OWNED;
        case TOKEN_BORROWED: return BORROWED;
        case TOKEN_SHARED: return SHARED;
        case TOKEN_LET: return LET;
        
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
        case TOKEN_LPAREN: return LPAREN;
        case TOKEN_RPAREN: return RPAREN;
        case TOKEN_LBRACE: return LBRACE;
        case TOKEN_RBRACE: return RBRACE;
        case TOKEN_LBRACKET: return LBRACKET;
        case TOKEN_RBRACKET: return RBRACKET;
        case TOKEN_SEMICOLON: return SEMICOLON;
        case TOKEN_COMMA: return COMMA;
        case TOKEN_DOT: return DOT;
        case TOKEN_COLON: return COLON;
        case TOKEN_ELLIPSIS: return ELLIPSIS;
        case TOKEN_NEWLINE: return NEWLINE;
        
        default:
            return -1; // Unknown token
    }
}

// Bison's lexer interface
int yylex(void) {
    if (!current_lexer) {
        return 0; // EOF
    }
    
    Token* token = lexer_next_token(current_lexer);
    if (!token) {
        return 0; // EOF
    }
    
    int bison_token = map_token_to_bison(token->type);
    
    // Set the semantic value based on token type
    switch (token->type) {
        case TOKEN_IDENT:
        case TOKEN_STRING:
            yylval.string = strdup(token->literal);
            break;
        case TOKEN_INT:
            yylval.integer = atoi(token->literal);
            break;
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
        return yylex();
    }
    
    return bison_token;
}

// Initialize parser with input
int parse_input(const char* input, const char* filename) {
    current_lexer = lexer_new(input, filename);
    if (!current_lexer) {
        fprintf(stderr, "Error: Failed to create lexer\n");
        return -1;
    }
    
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