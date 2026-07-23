#ifndef GOO_PARSER_ACTIONS_H
#define GOO_PARSER_ACTIONS_H

// Semantic-action helpers and parser-owned globals for parser.y.
//
// This header is the C-extraction counterpart of parser.y's former
// prologue/epilogue: every function bison's grammar actions call — except
// yyerror, which lives in src/parser/parser_errors.c alongside the rest of
// the parser's error-handling machinery (see
// include/parser/parser_errors.h) — lives in src/parser/parser_actions.c and
// is declared here. parser.y's prologue includes this header instead of
// forward-declaring/defining these directly; parser.tab.c (generated from
// parser.y) therefore links against parser_actions.o and parser_errors.o for
// all of it.
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
//                         (arc 6 (m): `type` carries a typed spec's declared
//                         type, NULL for untyped/bare arms)
//   desugar_const_group  — turn the spec chain into ordinary single const
//                         decls
ASTNode* clone_const_value(const ASTNode* n);
void substitute_iota(ASTNode** slot, long idx);
ASTNode* const_spec_new(ASTNode* name_ident, ASTNode* type, ASTNode* value);
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

// Rule-action hoists (thin-fat-actions pass) ------------------------------
// Each function below is the former inline `{ ... }` body of one or more
// parser.y rule arms, moved verbatim (only $n -> named parameter, $$ ->
// return value). Where multiple arms shared byte-identical construction
// logic and differed only in which pieces they passed in (e.g. call_expr's
// 8 arms), one function serves all of them — a pure move, not a behavior
// change: every arm still runs exactly the code it ran before.

// func_result: the `LPAREN func_params RPAREN` arm. Builds either a bare
// type (single anonymous result — preserves the scalar-return ABI) or a
// StructTypeNode tagged is_result_tuple (>=2 results, or any named result).
// See the call site in parser.y for the full ABI-collapsing rationale.
ASTNode* func_result_from_params(ASTNode* params_list);

// const_decl: builds a single ConstDeclNode. `type` is NULL for the untyped
// arm (`const n = 64`); `is_comptime` is 1 only for `COMPTIME CONST ...`.
ASTNode* const_decl_new(ASTNode* name_ident, ASTNode* type, ASTNode* value, int is_comptime);

// call_expr: builds a CallExprNode. `args` is NULL for the no-arg arm;
// `has_spread` is 1 only for the trailing-`...` spread arm. Shared by all 8
// call_expr arms, which differ only in how `args`/`has_spread` are computed
// (two arms splice a type_call_arg head onto an expression_list tail before
// calling this, via `args->next = ...`), never in CallExprNode construction.
ASTNode* call_expr_new(ASTNode* function, ASTNode* args, int has_spread);

// index_expr: plain indexing `expr[index]`.
ASTNode* index_expr_new(ASTNode* expr, ASTNode* index);

// index_expr: slice/substring `expr[low:high]` and its open-ended forms
// (`expr[low:]`, `expr[:high]`, `expr[:]` — pass NULL for the omitted bound).
ASTNode* slice_index_expr_new(ASTNode* expr, ASTNode* low, ASTNode* high);

// selector_expr: field/method selector `expr.ident`. Consumes (frees)
// ident_node, matching the prior inline action.
ASTNode* selector_expr_new(ASTNode* expr, ASTNode* ident_node);

// selector_expr: type assertion `expr.(type)`.
ASTNode* type_assert_expr_new(ASTNode* expr, ASTNode* asserted_type);

// func_param: builds one VarDeclNode parameter. `name_ident` NULL means the
// anonymous-parameter arm; `is_variadic`/`is_comptime` mirror the `...T` /
// `comptime name T` arms. Covers all 5 func_param arms.
ASTNode* func_param_new(ASTNode* name_ident, ASTNode* type, int is_variadic, int is_comptime);

// var_decl: single-name form `var x [type] [= value]`. Exactly one of
// `type`/`value` is NULL depending on which arm matched (pass NULL for the
// omitted piece). Also serves the ownership-qualifier arm (the qualifier
// itself is parsed but not yet wired to VarDeclNode.ownership — pre-existing
// TODO, unchanged by this hoist; see the call site).
ASTNode* var_decl_new_1(ASTNode* name_ident, ASTNode* type, ASTNode* value);

// var_decl: 2-name no-initializer form `var a, b type`.
ASTNode* var_decl_new_2(ASTNode* name_ident1, ASTNode* name_ident2, ASTNode* type);

// var_decl: 3-name no-initializer form `var a, b, c type`.
ASTNode* var_decl_new_3(ASTNode* name_ident1, ASTNode* name_ident2, ASTNode* name_ident3, ASTNode* type);

// short_var_decl: single-name `x := expr`.
ASTNode* short_var_decl_new_1(ASTNode* name_ident, ASTNode* value);

// short_var_decl: 2-name destructuring `a, b := expr` (RHS must produce a
// multi-field struct; destructured at codegen, not here).
ASTNode* short_var_decl_new_2(ASTNode* name_ident1, ASTNode* name_ident2, ASTNode* value);

// struct_field: the 4 single-name field arms (`name type`, `name : type`,
// and their SEMICOLON-terminated variants) — all build the same VarDeclNode
// shape, differing only in which token position carries the type. Grouped
// (`X, Y, Z T`) and embedded (`Base`, `*Base`) fields keep using the
// existing make_grouped_field/make_embedded_field (parser.y prologue),
// unchanged by this hoist.
ASTNode* struct_field_new(ASTNode* name_ident, ASTNode* type);

// slice_lit: the 5 arms producing a SliceLitNode — untyped `[1,2,3]`, empty
// `[]`, and the typed `[]T{...}` forms (incl. trailing-comma/empty variants).
ASTNode* slice_lit_new(ASTNode* elements, ASTNode* elem_type);

// slice_lit: the 3 arms producing an ArrayLitNode — `[N]T{...}` (incl.
// trailing-comma/empty variants).
ASTNode* array_lit_new(ASTNode* elements, ASTNode* array_type);

// struct_lit: the empty-body arm `Ident{}`. No struct_lit_inits to extract
// field names from, so it builds the StructLiteralNode directly rather than
// through struct_literal_new above.
ASTNode* struct_lit_empty_new(ASTNode* type_ident);

// struct_lit_init: keyed form `name: expr`. Stashes the owned field-name
// string on the value's node_type slot (struct_lit's reducer moves it into
// field_names[] and clears the slot). The positional arm is a 2-line
// passthrough and is not hoisted.
ASTNode* struct_lit_init_keyed(ASTNode* key_ident, ASTNode* value);

// map_entry: `key: value`. Stashes value on key->node_type; map_entry_list
// extracts and re-chains it into a parallel values list.
ASTNode* map_entry_new(ASTNode* key, ASTNode* value);

// map_entry_list: the COMMA-chaining arm — appends one more key/value pair
// onto the parallel keys/values chains map_entry_new started.
ASTNode* map_entry_list_append(ASTNode* keys_head, ASTNode* new_key);

// catch_expr: the P2.9 value-yielding arrow arm, `expression CATCH FAT_ARROW
// expression`. Wraps `fallback` in a synthetic one-statement block (an
// ExprStmt) and builds the ordinary CatchExprNode with no bound error
// variable, so it flows through the exact same value-producing catch path
// (type_check_catch_expr / generate_catch_body_value) as the block form
// `operand catch e { fallback }` — see the call site in parser.y for why no
// new type-check/codegen logic is needed.
CatchExprNode* catch_expr_arrow_new(ASTNode* operand, ASTNode* fallback);

#endif // GOO_PARSER_ACTIONS_H
