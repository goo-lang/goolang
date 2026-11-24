// The one piece of parser state that three translation units share.
//
// WHY IT HAS ITS OWN FILE. Measured with nm on the built objects, 2026-08-30:
// src/parser was four files in ONE cycle, and two of the edges closing it
// carried a single symbol each.
//
//   parser_actions.c -> lexer_bridge.c   current_lexer
//   parser_errors.c  -> lexer_bridge.c   current_lexer
//
// Nothing else took parser_actions or parser_errors into the cycle. Moving the
// DEFINITION here leaves both of them acyclic -- 1,038 lines of the package --
// and reduces the cycle to parser.tab.c and lexer_bridge.c.
//
// THAT REMAINDER IS IRREDUCIBLE, and it is not a defect. Bison's generated
// parser calls yylex, and the bridge calls yyparse and reads yylval and
// yylloc. A generated recursive-descent parser and its scanner bridge are
// mutually dependent by construction, so no extraction reaches it.
//
// The variable was already declared by hand with `extern Lexer* current_lexer;`
// at the top of parser_actions.c and parser_errors.c, so its home was already
// implicit rather than declared. This gives it one, and changes nothing about
// how it is reached.
#include "lexer.h"

// The lexer the current parse is reading from. lexer_bridge.c sets it in
// parse_input and clears it when the parse ends; parser_actions.c and
// parser_errors.c read it to attach a source position to a diagnostic.
Lexer* current_lexer = NULL;
