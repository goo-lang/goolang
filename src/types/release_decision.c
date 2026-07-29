// T4: which locals may codegen release at function exit? See
// include/release_decision.h for the five conditions and the measurement behind
// each one.
//
// THE SOUNDNESS DIRECTION IS INVERTED relative to the three escape passes. There,
// `escapes = true` is the safe answer and the default arm marks everything. Here
// `release = false` is safe, so every path this file does not fully understand
// must refuse, and it must say WHY. A wrong refusal costs reclamation. A wrong
// approval frees live memory.
//
// WHY A SEPARATE WALK. This module needs three facts the escape passes do not
// record: a local's BINDING SITE expression (condition 2), how many times it is
// bound (condition 4), and the loop/arena/conditional nesting at its declaration
// (conditions 3, 4 and 5). LocalEscapeResult is a name plus a boolean, so none of them survives
// there. The walk here is deliberately NOT another copy of escape_core's engine:
// it propagates no taint and applies no sink. It only collects declarations and
// assignments.
//
// CONSERVATIVE ON AN UNREADABLE FUNCTION. If the walk meets a statement kind it
// does not recognise, it cannot know whether that statement assigns to a local,
// and a missed assignment would let a rebound local be released. So the whole
// function is marked unreadable and every local in it refuses with
// RELEASE_NO_UNKNOWN. Missing a DECLARATION is already safe (the local gets no
// binding and refuses); missing an ASSIGNMENT is not, which is why the flag is
// per function rather than per local.

#include "release_decision.h"
#include "param_escape.h"
#include "local_escape.h"
#include "shim_signatures.h"
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Per-function collection
// ---------------------------------------------------------------------------

typedef struct {
    char*     name;          // owned
    ASTNode*  bound_value;   // the declaration's initial value; may be NULL
    int       binding_count; // declarations plus assignments
    int       loop_depth;    // loop nesting at the DECLARATION
    int       arena_depth;   // arena nesting at the DECLARATION
    int       cond_depth;    // conditional nesting at the DECLARATION
    bool      declared;      // a declaration was seen (not only an assignment)
} LocalRecord;

typedef struct {
    LocalRecord* items;
    size_t       count;
    size_t       cap;
    bool         unreadable;   // an unrecognised statement kind was met
} Collected;

typedef struct {
    Collected* out;
    int        loop_depth;
    int        arena_depth;
    int        cond_depth;
} WalkCtx;

static LocalRecord* find_record(Collected* c, const char* name) {
    for (size_t i = 0; i < c->count; i++) {
        if (strcmp(c->items[i].name, name) == 0) return &c->items[i];
    }
    return NULL;
}

// Returns NULL on allocation failure, which callers treat as "unreadable".
static LocalRecord* intern_record(Collected* c, const char* name) {
    LocalRecord* existing = find_record(c, name);
    if (existing) return existing;
    if (c->count >= c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 8;
        LocalRecord* grown = realloc(c->items, ncap * sizeof(LocalRecord));
        if (!grown) return NULL;
        c->items = grown;
        c->cap = ncap;
    }
    LocalRecord* r = &c->items[c->count];
    r->name = strdup(name);
    if (!r->name) return NULL;
    r->bound_value = NULL;
    r->binding_count = 0;
    r->loop_depth = 0;
    r->arena_depth = 0;
    r->cond_depth = 0;
    r->declared = false;
    c->count++;
    return r;
}

static void note_declaration(WalkCtx* ctx, const char* name, ASTNode* value) {
    if (!name || strcmp(name, "_") == 0) return;
    LocalRecord* r = intern_record(ctx->out, name);
    if (!r) { ctx->out->unreadable = true; return; }
    if (!r->declared) {
        r->declared = true;
        r->bound_value = value;
        r->loop_depth = ctx->loop_depth;
        r->arena_depth = ctx->arena_depth;
        r->cond_depth = ctx->cond_depth;
    }
    r->binding_count++;
}

// Is this `L = append(L, ...)` -- a SELF-APPEND?
//
// It matters because such an assignment does not REBIND `L` to a different
// object. It GROWS L's object: append either writes into the existing buffer or
// reallocs it, and goo_realloc frees the old base itself. One local, one live
// buffer, and ownership never moves. So a release at exit frees exactly the
// buffer `L` still holds, which is correct.
//
// `t := append(s, x)` is a DIFFERENT and dangerous shape, and it is NOT this
// one. There, t and s can hold the SAME buffer when capacity suffices, and two
// owners means a double free. It stays refused by condition 2, because
// call_result_is_owned finds no summary for `append` and answers false.
//
// The arg-0 identifier must match the assignment target by NAME. `a = append(b,
// x)` is a rebind of `a` to b's buffer, not a self-append.
static bool is_self_append(const char* target, ASTNode* rhs) {
    if (!target || !rhs || rhs->type != AST_CALL_EXPR) return false;
    CallExprNode* call = (CallExprNode*)rhs;
    if (!call->function || call->function->type != AST_IDENTIFIER) return false;
    if (strcmp(((IdentifierNode*)call->function)->name, "append") != 0) return false;
    ASTNode* first = call->args;
    if (!first || first->type != AST_IDENTIFIER) return false;
    return strcmp(((IdentifierNode*)first)->name, target) == 0;
}

// An ASSIGNMENT, not a declaration. The COUNT is what condition 4 reads, so the
// assigned value needs classifying only far enough to spot a self-append.
static void note_assignment(WalkCtx* ctx, ASTNode* lhs, ASTNode* rhs, bool plain_assign) {
    if (!lhs) return;
    if (lhs->type != AST_IDENTIFIER) return;   // a field/index store is not a rebind of the local
    const char* name = ((IdentifierNode*)lhs)->name;
    if (!name || strcmp(name, "_") == 0) return;
    LocalRecord* r = intern_record(ctx->out, name);
    if (!r) { ctx->out->unreadable = true; return; }
    // A self-append grows the same object, so it is not a new binding. Only a
    // plain `=` qualifies; a compound operator is never an append.
    if (plain_assign && is_self_append(name, rhs)) return;
    r->binding_count++;
}

static bool is_assign_operator(TokenType op) {
    switch (op) {
        case TOKEN_ASSIGN:
        case TOKEN_PLUS_ASSIGN:
        case TOKEN_MINUS_ASSIGN:
        case TOKEN_MUL_ASSIGN:
        case TOKEN_DIV_ASSIGN:
        case TOKEN_MOD_ASSIGN:
        case TOKEN_AND_ASSIGN:
        case TOKEN_OR_ASSIGN:
        case TOKEN_XOR_ASSIGN:
        case TOKEN_LSHIFT_ASSIGN:
        case TOKEN_RSHIFT_ASSIGN:
            return true;
        default:
            return false;
    }
}

static void walk_stmts(WalkCtx* ctx, ASTNode* stmt);

// Seed `names` from a parallel `values` list. The shape `a, b := f(), g()` and
// `var a, b = x, y` share it. When the counts differ -- `a, b := f()` returning
// two results -- there is no single value per name, so each name is recorded with
// a NULL value and condition 2 then refuses it. That is the safe reading, and it
// is why the daemon's `n, err := strconv.Atoi(f)` would refuse on ownership even
// if it were not already refused on loop scope.
static void seed_names(WalkCtx* ctx, char** names, size_t name_count, ASTNode* values) {
    size_t value_count = 0;
    for (ASTNode* v = values; v; v = v->next) value_count++;

    if (value_count == name_count) {
        ASTNode* v = values;
        for (size_t i = 0; i < name_count; i++) {
            note_declaration(ctx, names[i], v);
            if (v) v = v->next;
        }
        return;
    }
    for (size_t i = 0; i < name_count; i++) note_declaration(ctx, names[i], NULL);
}

static void walk_stmts(WalkCtx* ctx, ASTNode* stmt) {
    for (; stmt; stmt = stmt->next) {
        switch (stmt->type) {
            case AST_BLOCK_STMT:
                walk_stmts(ctx, ((BlockStmtNode*)stmt)->statements);
                break;

            case AST_EXPR_STMT: {
                ASTNode* e = ((ExprStmtNode*)stmt)->expr;
                if (e && e->type == AST_BINARY_EXPR) {
                    BinaryExprNode* b = (BinaryExprNode*)e;
                    if (is_assign_operator(b->operator)) {
                        note_assignment(ctx, b->left, b->right,
                                        b->operator == TOKEN_ASSIGN);
                    }
                }
                break;
            }

            case AST_VAR_DECL: {
                VarDeclNode* n = (VarDeclNode*)stmt;
                seed_names(ctx, n->names, n->name_count, n->values);
                break;
            }

            case AST_CONST_DECL: {
                ConstDeclNode* n = (ConstDeclNode*)stmt;
                seed_names(ctx, n->names, n->name_count, n->values);
                break;
            }

            case AST_MULTI_ASSIGN: {
                MultiAssignNode* n = (MultiAssignNode*)stmt;
                // `targets` is a NODE LIST, not a name array. `a, b := f()`
                // declares, `a, b = x, y` assigns; either way condition 4 only
                // needs the count. When the list lengths differ -- `a, b := f()`
                // with one two-result call -- there is no single value per name,
                // so the value is recorded NULL and condition 2 refuses. That is
                // the daemon's `n, err := strconv.Atoi(f)` shape.
                size_t tcount = 0, vcount = 0;
                for (ASTNode* t = n->targets; t; t = t->next) tcount++;
                for (ASTNode* v = n->values; v; v = v->next) vcount++;
                ASTNode* v = n->values;
                for (ASTNode* t = n->targets; t; t = t->next) {
                    if (n->is_short_decl) {
                        if (t->type == AST_IDENTIFIER) {
                            note_declaration(ctx, ((IdentifierNode*)t)->name,
                                             (tcount == vcount) ? v : NULL);
                        }
                    } else {
                        note_assignment(ctx, t, v, !n->is_short_decl);
                    }
                    if (v) v = v->next;
                }
                break;
            }

            case AST_IF_STMT: {
                // CONDITION 5. Both arms are raised, and independently: a local
                // declared in EITHER branch has a slot on every path and a value
                // only on the taken one. See decide() for the measurement.
                IfStmtNode* n = (IfStmtNode*)stmt;
                ctx->cond_depth++;
                walk_stmts(ctx, n->then_stmt);
                walk_stmts(ctx, n->else_stmt);
                ctx->cond_depth--;
                break;
            }

            case AST_FOR_STMT: {
                ForStmtNode* n = (ForStmtNode*)stmt;
                ctx->loop_depth++;
                // The init clause declares INSIDE the loop's own scope (`i` in
                // `for i := 0; ...`), so it is counted at the raised depth.
                walk_stmts(ctx, n->init);
                walk_stmts(ctx, n->post);
                // `for k, v := range xs` binds k and v per ITERATION, and both
                // are views derived from the range expression. Recording them
                // keeps the plan honest; the raised loop_depth is what refuses
                // them.
                if (n->range_expr) {
                    if (n->key_name)   note_declaration(ctx, n->key_name, NULL);
                    if (n->value_name) note_declaration(ctx, n->value_name, NULL);
                }
                walk_stmts(ctx, n->body);
                ctx->loop_depth--;
                break;
            }

            case AST_ARENA_BLOCK: {
                ctx->arena_depth++;
                walk_stmts(ctx, ((ArenaBlockNode*)stmt)->body);
                ctx->arena_depth--;
                break;
            }

            case AST_UNSAFE_STMT:
                walk_stmts(ctx, ((UnsafeStmtNode*)stmt)->body);
                break;

            case AST_RETURN_STMT:
            case AST_BREAK_STMT:
            case AST_CONTINUE_STMT:
            case AST_GO_STMT:
            case AST_DEFER_STMT:
                // None of these binds a local. A `go`/`defer` argument can make a
                // local escape, and condition 1 is what refuses it -- local_escape
                // already treats both as escaping.
                break;

            default:
                // An unrecognised statement kind might assign to a local, and a
                // missed assignment would let a rebound local be released. Refuse
                // the whole function rather than guess. This is the same
                // conservative default the escape passes take, pointed the other
                // way because the safe answer here is `false`.
                ctx->out->unreadable = true;
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Condition 2 — ownership, decided at the binding site
// ---------------------------------------------------------------------------

static bool is_new_call(ASTNode* expr) {
    if (!expr || expr->type != AST_CALL_EXPR) return false;
    CallExprNode* call = (CallExprNode*)expr;
    return call->function && call->function->type == AST_IDENTIFIER &&
           strcmp(((IdentifierNode*)call->function)->name, "new") == 0;
}

static bool is_addr_of_composite(ASTNode* expr) {
    if (!expr || expr->type != AST_UNARY_EXPR) return false;
    UnaryExprNode* u = (UnaryExprNode*)expr;
    return u->operator == TOKEN_BIT_AND && u->operand &&
           u->operand->type == AST_STRUCT_LITERAL;
}

// A selector callee whose base names one of THIS function's locals is a method
// call on that local, not a package call. Without this guard a local named
// `strings` would collect the strings package's whitelist -- the same hole
// selector_base_is_local closes in escape_core.c, and param_escape row 23 pins.
static bool selector_base_is_local(Collected* c, ASTNode* selector) {
    if (!selector || selector->type != AST_SELECTOR_EXPR) return false;
    ASTNode* base = ((SelectorExprNode*)selector)->expr;
    if (!base || base->type != AST_IDENTIFIER) return false;
    return find_record(c, ((IdentifierNode*)base)->name) != NULL;
}

static bool call_result_is_owned(Collected* c, const ParamEscapeResult* pe, ASTNode* expr) {
    CallExprNode* call = (CallExprNode*)expr;
    if (!call->function) return false;

    if (call->function->type == AST_IDENTIFIER) {
        const char* name = ((IdentifierNode*)call->function)->name;
        const ParamEscapeSummary* s = param_escape_lookup(pe, name);
        // No summary means an unresolved or external callee. Conservative:
        // assume the result may alias an argument.
        if (!s) return false;
        // return_escapes is "does F return a value derived from one of its own
        // params?" -- exactly the borrowed-result relation. False means the
        // result derives from no argument, so the caller owns it.
        return !s->return_escapes;
    }

    if (call->function->type == AST_SELECTOR_EXPR) {
        if (selector_base_is_local(c, call->function)) return false;
        SelectorExprNode* sel = (SelectorExprNode*)call->function;
        if (!sel->expr || sel->expr->type != AST_IDENTIFIER || !sel->selector) return false;
        const char* pkg = ((IdentifierNode*)sel->expr)->name;
        // non_retaining is audited per runtime body in shim_signatures.c, and
        // that audit states every whitelisted entry COPIES rather than aliasing
        // an argument. errors.Unwrap is 0 because it returns a pointer INTO its
        // argument, so this bit separates owned from borrowed correctly.
        return shim_signature_is_non_retaining(pkg, sel->selector) != 0;
    }

    return false;
}

static bool binding_is_owned(Collected* c, const ParamEscapeResult* pe, ASTNode* value) {
    if (!value) return false;

    if (is_new_call(value) || is_addr_of_composite(value)) return true;

    switch (value->type) {
        // A fresh composite is a fresh allocation, so the local owns it.
        case AST_STRUCT_LITERAL:
        case AST_SLICE_EXPR:      // a slice literal (SliceLitNode)
        case AST_ARRAY_LITERAL:
        case AST_PAREN_EXPR:      // a map literal (MapLitNode)
            return true;

        case AST_CALL_EXPR:
            return call_result_is_owned(c, pe, value);

        // CONCATENATION IS AN ALLOCATION. `s := a + b` on strings lowers to
        // goo_string_concat (src/runtime/runtime.c:523), which always returns
        // fresh goo_alloc memory -- or {NULL, 0} when both operands are empty,
        // and goo_release is a no-op on NULL. It COPIES both operands, so the
        // result aliases neither and a parameter operand is as safe as a local
        // one. That is the whole difference from borrowView's `s[1:]`, which
        // returns a view and stays refused by AST_SLICE_INDEX_EXPR below.
        //
        // WITHOUT THIS ARM THE STRING RELEASE IS WORTH NOTHING: the only owned
        // string bindings left are a Goo call and a non-retaining shim, and
        // concatenation is how ordinary Goo code builds a string.
        //
        // AN INTEGER `+` REACHES HERE TOO, and that is deliberate. This module is
        // pure AST and holds no type information, so it cannot tell the two
        // apart. Approving both costs nothing, because the decision is only HALF
        // the guard: codegen_arc_note_local refuses every slot that is not a
        // pointer, a 3-field slice or a 2-field string, and an int's slot is a
        // bare i64. Do NOT narrow this arm to "recover" precision -- the
        // two-layer split is what keeps each layer simple. Pinned by rows 22-25.
        //
        // ONLY `+`. No other binary operator yields a heap value in Go: the
        // comparisons and the logical operators yield a bool, and the remaining
        // arithmetic operators are numeric.
        case AST_BINARY_EXPR:
            return ((BinaryExprNode*)value)->operator == TOKEN_PLUS;

        // A VIEW into something this local does not own. `c := s[1:]` is the
        // shape include/local_escape.h names, and it needs no call to be wrong.
        case AST_SLICE_INDEX_EXPR:
        case AST_INDEX_EXPR:
        case AST_SELECTOR_EXPR:
            return false;

        // An alias. Only ONE owner may release, or the buffer is freed twice.
        case AST_IDENTIFIER:
            return false;

        default:
            // Includes a literal, which owns no heap object worth releasing, and
            // every construct not listed above.
            return false;
    }
}

// ---------------------------------------------------------------------------
// Plan construction
// ---------------------------------------------------------------------------

static void collected_free(Collected* c) {
    for (size_t i = 0; i < c->count; i++) free(c->items[i].name);
    free(c->items);
    c->items = NULL;
    c->count = c->cap = 0;
}

static ReleaseVerdict decide(const Collected* c, const LocalRecord* r,
                            const ParamEscapeResult* pe,
                            const LocalEscapeResult* le,
                            const char* fn) {
    if (c->unreadable) return RELEASE_NO_UNKNOWN;
    if (!r->declared) return RELEASE_NO_NO_BINDING;

    // Condition 1 first: it is the cheapest, and local_escape is conservative on
    // a miss so an unknown name refuses here.
    if (local_escape_local_escapes(le, fn, r->name)) return RELEASE_NO_ESCAPES;

    // Condition 3 before 4: an arena local inside a loop is refused for the
    // reason that actually makes it dangerous rather than merely wasteful.
    if (r->arena_depth > 0) return RELEASE_NO_ARENA;

    // Condition 4, the loop half. Checked before condition 5 because a local
    // inside a loop is refused for the more fundamental cause: it is rebound
    // every iteration, so a release at exit frees one of N.
    if (r->loop_depth > 0)    return RELEASE_NO_LOOP_SCOPE;

    // CONDITION 5: declared inside a conditional block.
    //
    // This is a SOUNDNESS condition and it closed a LIVE BUG. Measured on
    // `if c { a := new(int) }` called with c false:
    //
    //     Use of uninitialised value of size 8
    //        at goo_release (runtime.c:203)     <- the immortal-count read
    //        by f
    //      Uninitialised value was created by a stack allocation at f
    //
    // and again at runtime.c:215, which is the __atomic_fetch_sub -- a WRITE
    // through the garbage pointer, to an arbitrary address.
    //
    // WHY. codegen_alloc_local_promoted sends an ordinary local to
    // codegen_create_entry_alloca, so the SLOT is hoisted to the entry block
    // while the initialising store stays at the declaration site. The slot
    // therefore exists on every path and holds a VALUE only on the taken one,
    // and an LLVM alloca is undef rather than zero. goo_obj_headerless screens
    // only NULL and goo_zerobase, so nothing downstream catches it.
    //
    // The program exited 0 -- the garbage happened to be benign. Only valgrind
    // saw it, which is why this is a refusal and not a known limitation.
    //
    // AN ENTRY-BLOCK ZERO STORE WOULD RECOVER THE PRECISION, in the shape
    // defer_entry_store_zero already uses for a defer placed in a branch that is
    // never taken. Then an unexecuted declaration leaves NULL and goo_release
    // no-ops. Deliberately a separate increment. Pinned by rows 26-28.
    if (r->cond_depth > 0)    return RELEASE_NO_COND_SCOPE;

    // Condition 4, the re-assignment half.
    if (r->binding_count > 1) return RELEASE_NO_REBOUND;

    // Condition 2 last: it is the one that needs the callee summaries.
    if (!binding_is_owned((Collected*)c, pe, r->bound_value)) return RELEASE_NO_NOT_OWNED;

    return RELEASE_OK;
}

static const char* func_decl_name(ASTNode* decl) {
    if (!decl || decl->type != AST_FUNC_DECL) return NULL;
    return ((FuncDeclNode*)decl)->name;
}

ReleasePlan* release_plan_analyze(ASTNode* program) {
    ReleasePlan* plan = calloc(1, sizeof(ReleasePlan));
    if (!plan) return NULL;
    if (!program || program->type != AST_PROGRAM) return plan;

    ParamEscapeResult* pe = param_escape_analyze(program);
    LocalEscapeResult* le = local_escape_analyze(program, pe);
    if (!le) { param_escape_result_free(pe); return plan; }

    size_t fn_count = 0;
    for (ASTNode* d = ((ProgramNode*)program)->decls; d; d = d->next) {
        if (d->type == AST_FUNC_DECL) fn_count++;
    }
    if (fn_count == 0) {
        local_escape_result_free(le);
        param_escape_result_free(pe);
        return plan;
    }

    plan->functions = calloc(fn_count, sizeof(ReleasePlanFunction));
    if (!plan->functions) {
        local_escape_result_free(le);
        param_escape_result_free(pe);
        free(plan);
        return NULL;
    }

    for (ASTNode* d = ((ProgramNode*)program)->decls; d; d = d->next) {
        if (d->type != AST_FUNC_DECL) continue;
        const char* name = func_decl_name(d);
        if (!name) continue;

        Collected c = { 0 };
        WalkCtx ctx = { .out = &c, .loop_depth = 0, .arena_depth = 0, .cond_depth = 0 };
        walk_stmts(&ctx, ((FuncDeclNode*)d)->body);

        ReleasePlanFunction* pf = &plan->functions[plan->count];
        pf->function_name = strdup(name);
        pf->count = 0;
        pf->decisions = c.count ? calloc(c.count, sizeof(ReleaseDecision)) : NULL;
        if (!pf->function_name || (c.count && !pf->decisions)) {
            free(pf->function_name);
            free(pf->decisions);
            collected_free(&c);
            continue;   // drop this function; a missing entry refuses, which is safe
        }

        for (size_t i = 0; i < c.count; i++) {
            pf->decisions[i].local_name = strdup(c.items[i].name);
            if (!pf->decisions[i].local_name) continue;
            pf->decisions[i].verdict = decide(&c, &c.items[i], pe, le, name);
            pf->count++;
        }
        plan->count++;
        collected_free(&c);
    }

    local_escape_result_free(le);
    param_escape_result_free(pe);
    return plan;
}

void release_plan_free(ReleasePlan* plan) {
    if (!plan) return;
    for (size_t i = 0; i < plan->count; i++) {
        ReleasePlanFunction* pf = &plan->functions[i];
        for (size_t j = 0; j < pf->count; j++) free(pf->decisions[j].local_name);
        free(pf->decisions);
        free(pf->function_name);
    }
    free(plan->functions);
    free(plan);
}

ReleaseVerdict release_plan_verdict(const ReleasePlan* plan, const char* fn, const char* local) {
    if (!plan || !fn || !local) return RELEASE_NO_NO_BINDING;
    for (size_t i = 0; i < plan->count; i++) {
        if (strcmp(plan->functions[i].function_name, fn) != 0) continue;
        for (size_t j = 0; j < plan->functions[i].count; j++) {
            if (strcmp(plan->functions[i].decisions[j].local_name, local) == 0) {
                return plan->functions[i].decisions[j].verdict;
            }
        }
        return RELEASE_NO_NO_BINDING;
    }
    return RELEASE_NO_NO_BINDING;
}

bool release_plan_should_release(const ReleasePlan* plan, const char* fn, const char* local) {
    return release_plan_verdict(plan, fn, local) == RELEASE_OK;
}

const char* release_verdict_name(ReleaseVerdict v) {
    switch (v) {
        case RELEASE_OK:            return "RELEASE_OK";
        case RELEASE_NO_ESCAPES:    return "RELEASE_NO_ESCAPES";
        case RELEASE_NO_NOT_OWNED:  return "RELEASE_NO_NOT_OWNED";
        case RELEASE_NO_ARENA:      return "RELEASE_NO_ARENA";
        case RELEASE_NO_LOOP_SCOPE: return "RELEASE_NO_LOOP_SCOPE";
        case RELEASE_NO_REBOUND:    return "RELEASE_NO_REBOUND";
        case RELEASE_NO_COND_SCOPE: return "RELEASE_NO_COND_SCOPE";
        case RELEASE_NO_NO_BINDING: return "RELEASE_NO_NO_BINDING";
        case RELEASE_NO_UNKNOWN:    return "RELEASE_NO_UNKNOWN";
    }
    return "RELEASE_NO_UNKNOWN";
}
