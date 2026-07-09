#ifndef GOO_PARSER_ACTIONS_H
#define GOO_PARSER_ACTIONS_H

// Semantic-action helpers and parser-owned globals for parser.y.
//
// This header is the C-extraction counterpart of parser.y's former
// prologue/epilogue: every function bison's grammar actions call that isn't
// yyerror itself (kept in parser.y — bison's %define api.* and yyparse's own
// error path expect it colocated with the grammar) lives in
// src/parser/parser_actions.c and is declared here. parser.y's prologue
// includes this header instead of forward-declaring/defining these directly;
// parser.tab.c (generated from parser.y) therefore links against
// parser_actions.o for all of it.
//
// None of this is external API — it exists so parser.tab.c and
// parser_actions.c can see the same declarations, not for callers outside
// the parser subsystem.

#include "ast.h"
#include "token.h"

// Global AST root, set by the `program` grammar rule's action once parsing
// completes. Consumed by parse_input/parse_file callers (see
// include/parser.h, which also declares this extern for non-parser TUs) —
// this declaration is the one parser.tab.c's own action code sees.
extern ASTNode* ast_root;

// func_signature's own $$ collapses params and result into a single
// ASTNode* that can't tell "params only" (case: `(int)`) apart from
// "result only" (case: `() int`), and its "both" case (`(int) int`)
// outright drops the result — every existing caller of func_signature only
// ever read $$ as a params chain, so fixing that would ripple into all of
// them. func_type is the one caller that genuinely needs both pieces (it
// builds a FuncTypeNode), so func_signature additionally stashes them here,
// immediately consumed and cleared by func_type's action right after — safe
// because func_signature always reduces immediately before the func_type
// rule that contains it, and nested func types fully resolve (stash set,
// then consumed) before the enclosing func_signature's own action runs.
extern ASTNode* g_func_signature_params;
extern ASTNode* g_func_signature_result;

// Position / token helpers ---------------------------------------------

// Current lexer position, used by grammar actions to stamp new AST nodes.
Position get_current_position(void);

// Convert a bison token macro (PLUS, MINUS, ...) to the TokenType enum used
// by the AST/type-checker/codegen layers.
TokenType bison_token_to_token_type(int bison_token);

// Grouped-name desugaring -------------------------------------------------

// Go grouped-name shorthand `(x, y int)` == `(x int, y int)` for a parsed
// parameter/result list. Shared by func params and results.
void reinterpret_grouped_names(ASTNode* list);

// F4 grouped-const desugaring -----------------------------------------------
//   clone_const_value   — correct deep-copy of a const-expr node (NOT
//                         ast_node_copy, which under-allocates derived
//                         structs)
//   substitute_iota      — replace the identifier `iota` with its ordinal
//                         literal
//   const_spec_new       — build one ConstDeclNode for a grouped-const spec
//   desugar_const_group  — turn the spec chain into ordinary single const
//                         decls
ASTNode* clone_const_value(const ASTNode* n);
void substitute_iota(ASTNode** slot, long idx);
ASTNode* const_spec_new(ASTNode* name_ident, ASTNode* value);
ASTNode* desugar_const_group(ASTNode* spec_chain);

// P1.2 grouped-var desugaring: mirrors the F4 const-group mechanism above
// (var_spec_list uses the identical newline-blind-but-unambiguous
// juxtaposition as const_spec_list/import_spec_list — no SEMICOLON token),
// but carries NONE of const's iota/value-inheritance semantics.
ASTNode* var_spec_new(ASTNode* name_ident, ASTNode* type, ASTNode* value);
ASTNode* desugar_var_group(ASTNode* spec_chain);

// F6: build a 2-target/2-value MultiAssignNode (`a,b := v1,v2` /
// `a,b = v1,v2`).
ASTNode* multi_assign_2_new(ASTNode* t1, ASTNode* t2,
                             ASTNode* v1, ASTNode* v2, int is_short_decl);

// `a, b = f()` — tuple assignment to two existing lvalues from a SINGLE
// multi-return call. Builds a MultiAssignNode with two targets and one value
// (chain length 1, count 2); the type checker and codegen detect the
// values<targets shape and destructure the call's result struct fields.
ASTNode* multi_assign_call_new(ASTNode* t1, ASTNode* t2, ASTNode* call);

// Build a compound-assignment statement (`x += e`, `x &= e`, ...).
ASTNode* compound_assign_stmt(ASTNode* lhs, TokenType op, ASTNode* rhs);

// Build a StructLiteralNode from an owned type-name string (or NULL for an
// elided composite literal `{...}` whose type is inferred from context) and
// a struct_lit_inits chain. Extracts the field-name piggyback each
// struct_lit_init stashed on its node_type slot. Shared by the `struct_lit`
// rule and the elided-composite element rule.
ASTNode* struct_literal_new(char* type_name_owned, ASTNode* inits);

// Shared by the map_lit arms (with and without trailing comma). Extracts
// the parallel values list that map_entry_list stashes on the keys head's
// node_type side-channel (cleared so type_check/codegen never see it).
ASTNode* map_literal_new(ASTNode* map_type_node, ASTNode* entries);

#endif // GOO_PARSER_ACTIONS_H
