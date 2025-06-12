#ifndef PARSER_H
#define PARSER_H

#include "ast.h"

// External AST root (set by parser)
extern ASTNode* ast_root;

// Parser interface functions
int parse_input(const char* input, const char* filename);
int parse_file(const char* filename);

// Bison-generated functions
extern int yyparse(void);
extern void yyerror(const char* msg);

// Parser utility functions
void parser_init(void);
void parser_cleanup(void);

#endif // PARSER_H