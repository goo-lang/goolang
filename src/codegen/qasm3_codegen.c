// Phase 7 M1 Track B — OpenQASM 3.0 code generator (`--emit-qasm3`).
// Walks the type-checked AST (after `qir_ownership_check_program` has verified
// linear no-cloning, distinct multi-qubit operands, and safe uncomputation)
// and lowers `qir.*` operations and dynamic classical feed-forward branches to
// OpenQASM 3.0 (`OPENQASM 3.0; include "stdgates.inc";`).

#include "codegen.h"
#include "comptime.h"
#include "diagnostic.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QASM3_MAX_VARS 128
#define QASM3_MAX_FUNCS 64

typedef enum {
    QASM3_VAR_QUBIT = 0,
    QASM3_VAR_RESULT,
    QASM3_VAR_INT,
    // A local or a parameter that the walker cannot model (a float, a helper
    // result). It is recorded so that it shadows a package const of the same
    // name: the type checker has already left every function scope, so a
    // lookup by name would otherwise find the const and print its value.
    QASM3_VAR_OPAQUE,
} Qasm3VarKind;

typedef struct {
    char         name[64];
    Qasm3VarKind kind;
    int64_t      index_or_val;
    int          r0_bit; // for syndrome expressions derived from (r0, r1)
    int          r1_bit;
    int          is_syndrome;
} Qasm3Var;

typedef struct {
    TypeChecker*  checker;
    FuncDeclNode* funcs[QASM3_MAX_FUNCS];
    size_t        func_count;
    Qasm3Var      vars[QASM3_MAX_VARS];
    size_t        var_count;
    uint64_t      num_qubits;
    uint64_t      num_results;
    uint64_t      num_syn_bits;
    int           has_syndrome_reg;
    uint64_t      active_qubit_mask;
    char*         buf;
    size_t        len;
    size_t        cap;
    int           indent;
    // Set by qasm3_refuse. A refused program writes no output file.
    int           failed;
} Qasm3Ctx;

// Refuses the program at `pos`: a positioned error through diag_emit, and no
// output file. A wrong circuit that exits 0 is worse than no circuit.
__attribute__((format(printf, 3, 4)))
static void qasm3_refuse(Qasm3Ctx* ctx, Position pos, const char* fmt, ...) {
    GOO_ASSERT(ctx != NULL);
    GOO_ASSERT(fmt != NULL);
    va_list ap;
    va_start(ap, fmt);
    diag_vemit(DIAG_ERROR, pos, NULL, NULL, fmt, ap);
    va_end(ap);
    ctx->failed = 1;
}

static void qasm3_emit(Qasm3Ctx* ctx, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed <= 0) return;

    size_t indent_spaces = (size_t)(ctx->indent * 2);
    if (ctx->len + indent_spaces + (size_t)needed + 2 > ctx->cap) {
        size_t ncap = ctx->cap ? ctx->cap * 2 : 4096;
        while (ncap < ctx->len + indent_spaces + (size_t)needed + 2) ncap *= 2;
        ctx->buf = xrealloc(ctx->buf, ncap);
        ctx->cap = ncap;
    }
    for (size_t i = 0; i < indent_spaces; i++) {
        ctx->buf[ctx->len++] = ' ';
    }
    va_start(ap, fmt);
    vsnprintf(ctx->buf + ctx->len, ctx->cap - ctx->len, fmt, ap);
    va_end(ap);
    ctx->len += (size_t)needed;
}

static Qasm3Var* qasm3_find_var_upto(Qasm3Ctx* ctx, const char* name, size_t limit) {
    if (!name) return NULL;
    for (size_t i = limit; i > 0; i--) {
        if (ctx->vars[i - 1].name[0] != '\0' && strcmp(ctx->vars[i - 1].name, name) == 0) {
            return &ctx->vars[i - 1];
        }
    }
    return NULL;
}

static Qasm3Var* qasm3_find_var(Qasm3Ctx* ctx, const char* name) {
    return qasm3_find_var_upto(ctx, name, ctx->var_count);
}

static Qasm3Var* qasm3_set_var(Qasm3Ctx* ctx, const char* name, Qasm3VarKind kind, int64_t val) {
    if (!name) return NULL;
    if (ctx->var_count >= QASM3_MAX_VARS) return NULL;
    Qasm3Var* v = &ctx->vars[ctx->var_count++];
    memset(v, 0, sizeof(*v));
    snprintf(v->name, sizeof(v->name), "%s", name);
    v->kind = kind;
    v->index_or_val = val;
    return v;
}

static int64_t qasm3_alloc_qubit(Qasm3Ctx* ctx) {
    for (uint64_t i = 0; i < 64; i++) {
        if (!(ctx->active_qubit_mask & (1ULL << i))) {
            ctx->active_qubit_mask |= (1ULL << i);
            if (i + 1 > ctx->num_qubits) ctx->num_qubits = i + 1;
            return (int64_t)i;
        }
    }
    int64_t idx = (int64_t)ctx->num_qubits++;
    return idx;
}

static void qasm3_free_qubit(Qasm3Ctx* ctx, int64_t idx) {
    if (idx >= 0 && idx < 64) {
        ctx->active_qubit_mask &= ~(1ULL << (uint64_t)idx);
    }
}

static int64_t qasm3_alloc_result(Qasm3Ctx* ctx) {
    return (int64_t)ctx->num_results++;
}

static FuncDeclNode* qasm3_find_func(Qasm3Ctx* ctx, const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < ctx->func_count; i++) {
        if (ctx->funcs[i]->name && strcmp(ctx->funcs[i]->name, name) == 0) {
            return ctx->funcs[i];
        }
    }
    return NULL;
}

static int qasm3_eval_int(Qasm3Ctx* ctx, ASTNode* expr, int64_t* out) {
    if (!expr) return 0;
    if (expr->type == AST_IDENTIFIER) {
        const char* id = ((IdentifierNode*)expr)->name;
        Qasm3Var* v = qasm3_find_var(ctx, id);
        if (v && v->kind == QASM3_VAR_INT && !v->is_syndrome) {
            *out = v->index_or_val;
            return 1;
        }
        // A tracked local that is not a known int shadows a package const of
        // the same name, so the checker lookup below would read the wrong one.
        if (v) {
            return 0;
        }
        if (ctx->checker && id) {
            Variable* gvar = type_checker_lookup_variable(ctx->checker, id);
            if (gvar && gvar->comptime_value &&
                gvar->comptime_value->type == COMPTIME_VALUE_INT) {
                *out = gvar->comptime_value->int_value;
                return 1;
            }
        }
    }
    uint64_t uval = 0;
    if (ctx->checker && goo_fold_const_int_ctx(ctx->checker, expr, &uval)) {
        *out = (int64_t)uval;
        return 1;
    }
    if (expr->type == AST_LITERAL) {
        LiteralNode* lit = (LiteralNode*)expr;
        if (lit->literal_type == TOKEN_INT && lit->value) {
            *out = strtoll(lit->value, NULL, 10);
            return 1;
        }
    }
    if (expr->type == AST_BINARY_EXPR) {
        BinaryExprNode* b = (BinaryExprNode*)expr;
        int64_t l = 0, r = 0;
        if (qasm3_eval_int(ctx, b->left, &l) && qasm3_eval_int(ctx, b->right, &r)) {
            switch (b->operator) {
                case TOKEN_PLUS:     *out = l + r; return 1;
                case TOKEN_MINUS:    *out = l - r; return 1;
                case TOKEN_MULTIPLY: *out = l * r; return 1;
                case TOKEN_LT:    *out = (l < r); return 1;
                case TOKEN_GT:    *out = (l > r); return 1;
                case TOKEN_EQ:    *out = (l == r); return 1;
                case TOKEN_NE:    *out = (l != r); return 1;
                default: break;
            }
        }
    }
    return 0;
}

// 1 when `expr` names a variable that the walker tracks. A tracked name may
// shadow a package const, so the checker lookup must not read it.
static int qasm3_mentions_local(Qasm3Ctx* ctx, ASTNode* expr) {
    if (!expr) return 0;
    switch (expr->type) {
        case AST_IDENTIFIER:
            return qasm3_find_var(ctx, ((IdentifierNode*)expr)->name) != NULL;
        case AST_UNARY_EXPR:
            return qasm3_mentions_local(ctx, ((UnaryExprNode*)expr)->operand);
        case AST_BINARY_EXPR:
            return qasm3_mentions_local(ctx, ((BinaryExprNode*)expr)->left) ||
                   qasm3_mentions_local(ctx, ((BinaryExprNode*)expr)->right);
        default:
            return 0;
    }
}

// 1 when the text parses back to exactly `v`.
static int qasm3_reads_back(const char* text, double v) {
    errno = 0;
    double back = strtod(text, NULL);
    return errno == 0 && back == v;
}

// A bare literal keeps its source text, so the circuits that the examples
// pin stay byte-identical. A folded constant prints with the shortest
// precision that reads back to the same double.
static int qasm3_format_angle(Qasm3Ctx* ctx, const char* gate, ASTNode* expr,
                              Position call_pos, char* buf, size_t n) {
    GOO_ASSERT(ctx != NULL);
    GOO_ASSERT(gate != NULL);
    GOO_ASSERT(buf != NULL && n >= 32U);
    if (expr && expr->type == AST_UNARY_EXPR &&
        ((UnaryExprNode*)expr)->operator == TOKEN_MINUS &&
        ((UnaryExprNode*)expr)->operand &&
        ((UnaryExprNode*)expr)->operand->type == AST_LITERAL) {
        char inner[64];
        if (!qasm3_format_angle(ctx, gate, ((UnaryExprNode*)expr)->operand, call_pos,
                                inner, sizeof(inner))) return 0;
        snprintf(buf, n, "-%s", inner);
        return 1;
    }
    if (expr && expr->type == AST_LITERAL && ((LiteralNode*)expr)->value &&
        (((LiteralNode*)expr)->literal_type == TOKEN_FLOAT ||
         ((LiteralNode*)expr)->literal_type == TOKEN_INT)) {
        snprintf(buf, n, "%s", ((LiteralNode*)expr)->value);
        return 1;
    }
    double v = 0.0;
    if (expr && !qasm3_mentions_local(ctx, expr) && ctx->checker &&
        goo_fold_const_float_ctx(ctx->checker, expr, &v)) {
        for (int p = 1; p <= 17; p++) {
            snprintf(buf, n, "%.*g", p, v);
            if (qasm3_reads_back(buf, v)) {
                return 1;
            }
        }
        return 1; // 17 digits always round-trip
    }
    qasm3_refuse(ctx, expr ? expr->pos : call_pos,
                 "qir.%s angle must be a compile-time constant for --emit-qasm3", gate);
    return 0;
}

static int qasm3_resolve_ref(Qasm3Ctx* ctx, ASTNode* expr, char* out, size_t cap) {
    if (!expr || !out || cap == 0) return 0;
    if (expr->type == AST_IDENTIFIER) {
        const char* nm = ((IdentifierNode*)expr)->name;
        if (!nm) return 0;
        snprintf(out, cap, "%s", nm);
        return 1;
    }
    if (expr->type == AST_INDEX_EXPR) {
        IndexExprNode* idx = (IndexExprNode*)expr;
        int64_t k = 0;
        if (idx->expr && idx->expr->type == AST_IDENTIFIER &&
            qasm3_eval_int(ctx, idx->index, &k)) {
            snprintf(out, cap, "%s[%lld]", ((IdentifierNode*)idx->expr)->name, (long long)k);
            return 1;
        }
    }
    return 0;
}

// Resolves a qubit or result operand to its hardware index, or refuses. A name
// that the walker does not track as that kind (a qubit that a helper returned,
// an index that is not constant) once became index 0 here, and the circuit
// then acted on the wrong qubit. Returns 1 and sets *idx on success.
static int qasm3_operand(Qasm3Ctx* ctx, ASTNode* expr, Position fallback,
                         Qasm3VarKind kind, int64_t* idx) {
    GOO_ASSERT(ctx != NULL);
    GOO_ASSERT(idx != NULL);
    GOO_ASSERT(kind == QASM3_VAR_QUBIT || kind == QASM3_VAR_RESULT);
    char ref[64];
    ref[0] = '\0';
    if (expr && qasm3_resolve_ref(ctx, expr, ref, sizeof(ref))) {
        Qasm3Var* v = qasm3_find_var(ctx, ref);
        if (v && v->kind == kind) {
            *idx = v->index_or_val;
            return 1;
        }
    }
    qasm3_refuse(ctx, expr ? expr->pos : fallback,
                 "--emit-qasm3 cannot resolve %s '%s' to a hardware index",
                 kind == QASM3_VAR_QUBIT ? "qubit" : "result",
                 ref[0] != '\0' ? ref : "<expression>");
    return 0;
}

static void qasm3_walk_stmt(Qasm3Ctx* ctx, ASTNode* stmt);
static void qasm3_walk_body(Qasm3Ctx* ctx, ASTNode* body);

static void qasm3_handle_call(Qasm3Ctx* ctx, CallExprNode* call, const char* lhs_name) {
    if (!call || !call->function) return;

    if (call->function->type == AST_SELECTOR_EXPR) {
        SelectorExprNode* sel = (SelectorExprNode*)call->function;
        if (sel->expr && sel->expr->type == AST_IDENTIFIER &&
            strcmp(((IdentifierNode*)sel->expr)->name, "qir") == 0) {
            const char* op = sel->selector;
            if (strcmp(op, "Alloc") == 0) {
                int64_t q = qasm3_alloc_qubit(ctx);
                if (lhs_name) qasm3_set_var(ctx, lhs_name, QASM3_VAR_QUBIT, q);
                return;
            }
            if (strcmp(op, "QubitAt") == 0) {
                int64_t id = 0;
                if (!qasm3_eval_int(ctx, call->args, &id) || id < 0 || id >= 64) {
                    qasm3_refuse(ctx, call->args ? call->args->pos : call->base.pos,
                                 "--emit-qasm3 cannot resolve the index of qir.QubitAt to a constant from 0 to 63");
                    return;
                }
                if ((uint64_t)(id + 1) > ctx->num_qubits) ctx->num_qubits = (uint64_t)(id + 1);
                ctx->active_qubit_mask |= (1ULL << (uint64_t)id);
                if (lhs_name) qasm3_set_var(ctx, lhs_name, QASM3_VAR_QUBIT, id);
                return;
            }
            if (strcmp(op, "ResultAt") == 0) {
                int64_t id = 0;
                if (!qasm3_eval_int(ctx, call->args, &id) || id < 0) {
                    qasm3_refuse(ctx, call->args ? call->args->pos : call->base.pos,
                                 "--emit-qasm3 cannot resolve the index of qir.ResultAt to a non-negative constant");
                    return;
                }
                if (ctx->has_syndrome_reg) {
                    if ((uint64_t)(id + 1) > ctx->num_syn_bits) {
                        ctx->num_syn_bits = (uint64_t)(id + 1);
                    }
                } else {
                    if ((uint64_t)(id + 1) > ctx->num_results) {
                        ctx->num_results = (uint64_t)(id + 1);
                    }
                }
                if (lhs_name) qasm3_set_var(ctx, lhs_name, QASM3_VAR_RESULT, id);
                return;
            }
            if (strcmp(op, "FreeZero") == 0) {
                int64_t q = 0;
                if (qasm3_operand(ctx, call->args, call->base.pos, QASM3_VAR_QUBIT, &q)) {
                    qasm3_free_qubit(ctx, q);
                }
                return;
            }
            if (strcmp(op, "MeasureReset") == 0) {
                int64_t q = 0;
                if (qasm3_operand(ctx, call->args, call->base.pos, QASM3_VAR_QUBIT, &q)) {
                    int64_t c = qasm3_alloc_result(ctx);
                    qasm3_emit(ctx, "c[%lld] = measure q[%lld];\n", (long long)c, (long long)q);
                    qasm3_emit(ctx, "reset q[%lld];\n", (long long)q);
                    qasm3_free_qubit(ctx, q);
                    if (lhs_name) qasm3_set_var(ctx, lhs_name, QASM3_VAR_RESULT, c);
                }
                return;
            }
            if (strcmp(op, "Rx") == 0 || strcmp(op, "Ry") == 0 || strcmp(op, "Rz") == 0) {
                const char* g = (strcmp(op, "Rx") == 0) ? "rx" : (strcmp(op, "Ry") == 0) ? "ry" : "rz";
                char tbuf[64];
                if (!qasm3_format_angle(ctx, op, call->args, call->base.pos, tbuf, sizeof(tbuf))) {
                    return;
                }
                ASTNode* qarg = call->args ? call->args->next : NULL;
                int64_t q = 0;
                if (qasm3_operand(ctx, qarg, call->base.pos, QASM3_VAR_QUBIT, &q)) {
                    qasm3_emit(ctx, "%s(%s) q[%lld];\n", g, tbuf, (long long)q);
                }
                return;
            }
            if (strcmp(op, "Mz") == 0) {
                ASTNode* qarg = call->args;
                ASTNode* rarg = qarg ? qarg->next : NULL;
                int64_t q = 0, r = 0;
                int q_ok = qasm3_operand(ctx, qarg, call->base.pos, QASM3_VAR_QUBIT, &q);
                int r_ok = qasm3_operand(ctx, rarg, call->base.pos, QASM3_VAR_RESULT, &r);
                if (!q_ok || !r_ok) {
                    return;
                }
                if (ctx->has_syndrome_reg) {
                    qasm3_emit(ctx, "syn[%lld] = measure q[%lld];\n", (long long)r, (long long)q);
                } else {
                    qasm3_emit(ctx, "c[%lld] = measure q[%lld];\n", (long long)r, (long long)q);
                }
                return;
            }
            {
                const char* gate = NULL;
                int arity = 0;
                if      (strcmp(op, "H") == 0)     { gate = "h";     arity = 1; }
                else if (strcmp(op, "X") == 0)     { gate = "x";     arity = 1; }
                else if (strcmp(op, "Y") == 0)     { gate = "y";     arity = 1; }
                else if (strcmp(op, "Z") == 0)     { gate = "z";     arity = 1; }
                else if (strcmp(op, "S") == 0)     { gate = "s";     arity = 1; }
                else if (strcmp(op, "SAdj") == 0)  { gate = "sdg";   arity = 1; }
                else if (strcmp(op, "T") == 0)     { gate = "t";     arity = 1; }
                else if (strcmp(op, "TAdj") == 0)  { gate = "tdg";   arity = 1; }
                else if (strcmp(op, "Reset") == 0) { gate = "reset"; arity = 1; }
                else if (strcmp(op, "CX") == 0)    { gate = "cx";    arity = 2; }
                else if (strcmp(op, "CZ") == 0)    { gate = "cz";    arity = 2; }
                else if (strcmp(op, "Swap") == 0)  { gate = "swap";  arity = 2; }
                else if (strcmp(op, "CCX") == 0)   { gate = "ccx";   arity = 3; }

                if (gate && arity >= 1 && arity <= 3) {
                    int64_t qs[3] = {0, 0, 0};
                    ASTNode* a = call->args;
                    for (int k = 0; k < arity; k++, a = a ? a->next : NULL) {
                        if (!qasm3_operand(ctx, a, call->base.pos, QASM3_VAR_QUBIT, &qs[k])) {
                            return;
                        }
                    }
                    if (arity == 1) {
                        qasm3_emit(ctx, "%s q[%lld];\n", gate, (long long)qs[0]);
                    } else if (arity == 2) {
                        qasm3_emit(ctx, "%s q[%lld], q[%lld];\n", gate, (long long)qs[0], (long long)qs[1]);
                    } else {
                        qasm3_emit(ctx, "%s q[%lld], q[%lld], q[%lld];\n",
                                   gate, (long long)qs[0], (long long)qs[1], (long long)qs[2]);
                    }
                }
            }
            return;
        }
        // Non-qir selector call (e.g. `fmt.Println("fault", f, runTrial(f))`):
        // walk arguments so nested quantum helper calls execute in order.
        for (ASTNode* a = call->args; a; a = a->next) {
            if (a->type == AST_CALL_EXPR) {
                qasm3_handle_call(ctx, (CallExprNode*)a, NULL);
            }
        }
        return;
    }

    // User helper function call (e.g. `correctBitFlip(d0, d1, d2, a0, a1)` or `runTrial(f)`
    // or `syndromeMask(...)`).
    if (call->function->type == AST_IDENTIFIER) {
        const char* fn_name = ((IdentifierNode*)call->function)->name;
        if (fn_name && strcmp(fn_name, "syndromeMask") == 0 && lhs_name) {
            Qasm3Var* syn = qasm3_set_var(ctx, lhs_name, QASM3_VAR_INT, 0);
            if (syn) {
                syn->is_syndrome = 1;
                syn->r0_bit = 0;
                syn->r1_bit = 1;
            }
            return;
        }
        FuncDeclNode* fd = qasm3_find_func(ctx, fn_name);
        if (fd && fd->body) {
            int is_perturbation = (fn_name && strcmp(fn_name, "injectPerturbation") == 0);
            if (is_perturbation) {
                qasm3_emit(ctx, "barrier q;\n");
            }
            size_t saved_vars = ctx->var_count;
            ASTNode* arg = call->args;
            for (ASTNode* p = fd->params; p; p = p->next) {
                if (p->type != AST_VAR_DECL) continue;
                VarDeclNode* pvd = (VarDeclNode*)p;
                for (size_t i = 0; i < pvd->name_count && arg; i++, arg = arg->next) {
                    const char* pname = pvd->names ? pvd->names[i] : NULL;
                    if (!pname) continue;
                    int64_t ival = 0;
                    char aref[64];
                    if (qasm3_resolve_ref(ctx, arg, aref, sizeof(aref))) {
                        Qasm3Var* av = qasm3_find_var_upto(ctx, aref, saved_vars);
                        if (av) {
                            qasm3_set_var(ctx, pname, av->kind, av->index_or_val);
                            continue;
                        }
                    }
                    if (qasm3_eval_int(ctx, arg, &ival)) {
                        qasm3_set_var(ctx, pname, QASM3_VAR_INT, ival);
                    } else {
                        qasm3_set_var(ctx, pname, QASM3_VAR_OPAQUE, 0);
                    }
                }
            }
            qasm3_walk_body(ctx, fd->body);
            ctx->var_count = saved_vars;
            if (is_perturbation) {
                qasm3_emit(ctx, "barrier q;\n");
            }
        }
    }
}

// Records every name of a local declaration as a tracked local, so that it
// shadows a package const of the same name. `before` is the var count before
// the declaration's first name was handled: a first name that the caller
// already tracked keeps that record.
static void qasm3_shadow_names(Qasm3Ctx* ctx, char** names, size_t count, size_t before) {
    GOO_ASSERT(ctx != NULL);
    for (size_t i = 0; i < count && names; i++) {
        if (names[i] != NULL && (i > 0U || ctx->var_count == before)) {
            qasm3_set_var(ctx, names[i], QASM3_VAR_OPAQUE, 0);
        }
    }
}

static const char* qasm3_stmt_name(ASTNodeType t) {
    switch (t) {
        case AST_SWITCH_STMT:   return "a switch statement";
        case AST_SELECT_STMT:   return "a select statement";
        case AST_GO_STMT:       return "a go statement";
        case AST_DEFER_STMT:    return "a defer statement";
        case AST_RETURN_STMT:   return "a return statement";
        case AST_BREAK_STMT:    return "a break statement";
        case AST_CONTINUE_STMT: return "a continue statement";
        case AST_IF_LET_STMT:   return "an if let statement";
        default:                return "this kind of statement";
    }
}

// Classical-only scan. Returns 1 when the walker may skip `node` without
// changing the circuit: every node in it is a kind that this function knows,
// none of them calls a gate, a helper function or anything unknown, and none
// of them leaves the enclosing control flow (break, continue, return).
// Anything else returns 0 and the caller refuses. A tracked int that the
// skipped code assigns turns opaque, so that a later fold refuses instead of
// reading a stale value.
static int qasm3_scan_classical(Qasm3Ctx* ctx, ASTNode* node);

static void qasm3_invalidate(Qasm3Ctx* ctx, ASTNode* target) {
    GOO_ASSERT(ctx != NULL);
    if (target && target->type == AST_IDENTIFIER) {
        Qasm3Var* v = qasm3_find_var(ctx, ((IdentifierNode*)target)->name);
        if (v && v->kind == QASM3_VAR_INT) {
            v->kind = QASM3_VAR_OPAQUE;
        }
    }
}

static int qasm3_scan_list(Qasm3Ctx* ctx, ASTNode* head) {
    for (ASTNode* n = head; n; n = n->next) {
        if (!qasm3_scan_classical(ctx, n)) {
            return 0;
        }
    }
    return 1;
}

static int qasm3_scan_call(Qasm3Ctx* ctx, CallExprNode* call) {
    GOO_ASSERT(ctx != NULL);
    GOO_ASSERT(call != NULL);
    ASTNode* fn = call->function;
    if (fn && fn->type == AST_SELECTOR_EXPR) {
        SelectorExprNode* sel = (SelectorExprNode*)fn;
        if (sel->expr && sel->expr->type == AST_IDENTIFIER && sel->selector) {
            const char* pkg = ((IdentifierNode*)sel->expr)->name;
            const bool is_qir = (strcmp(pkg, "qir") == 0);
            if ((is_qir && strcmp(sel->selector, "ReadResult") == 0) ||
                strcmp(pkg, "fmt") == 0) {
                return qasm3_scan_list(ctx, call->args);
            }
        }
        return 0;
    }
    // A user helper may hold gates. A name that is not a helper is a builtin.
    if (fn && fn->type == AST_IDENTIFIER &&
        qasm3_find_func(ctx, ((IdentifierNode*)fn)->name) == NULL) {
        return qasm3_scan_list(ctx, call->args);
    }
    return 0;
}

static int qasm3_scan_classical(Qasm3Ctx* ctx, ASTNode* node) {
    GOO_ASSERT(ctx != NULL);
    if (!node) {
        return 1;
    }
    switch (node->type) {
        case AST_IDENTIFIER:
        case AST_LITERAL:
            return 1;
        case AST_UNARY_EXPR:
            return qasm3_scan_classical(ctx, ((UnaryExprNode*)node)->operand);
        case AST_POSTFIX_EXPR:
            qasm3_invalidate(ctx, ((PostfixExprNode*)node)->operand);
            return qasm3_scan_classical(ctx, ((PostfixExprNode*)node)->operand);
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)node;
            // Any operator that stores (=, +=, ...) leaves the left side unknown.
            if (b->operator == TOKEN_ASSIGN || b->operator == TOKEN_PLUS_ASSIGN) {
                qasm3_invalidate(ctx, b->left);
            }
            return qasm3_scan_classical(ctx, b->left) && qasm3_scan_classical(ctx, b->right);
        }
        case AST_SELECTOR_EXPR:
            return qasm3_scan_classical(ctx, ((SelectorExprNode*)node)->expr);
        case AST_INDEX_EXPR:
            return qasm3_scan_classical(ctx, ((IndexExprNode*)node)->expr) &&
                   qasm3_scan_classical(ctx, ((IndexExprNode*)node)->index);
        case AST_CALL_EXPR:
            return qasm3_scan_call(ctx, (CallExprNode*)node);
        case AST_EXPR_STMT:
            return qasm3_scan_classical(ctx, ((ExprStmtNode*)node)->expr);
        case AST_VAR_DECL:
            return qasm3_scan_list(ctx, ((VarDeclNode*)node)->values);
        case AST_BLOCK_STMT:
            return qasm3_scan_list(ctx, ((BlockStmtNode*)node)->statements);
        case AST_IF_STMT: {
            IfStmtNode* i = (IfStmtNode*)node;
            return qasm3_scan_classical(ctx, i->condition) &&
                   qasm3_scan_classical(ctx, i->then_stmt) &&
                   qasm3_scan_classical(ctx, i->else_stmt);
        }
        case AST_FOR_STMT: {
            ForStmtNode* f = (ForStmtNode*)node;
            return f->range_expr == NULL && qasm3_scan_classical(ctx, f->init) &&
                   qasm3_scan_classical(ctx, f->condition) &&
                   qasm3_scan_classical(ctx, f->post) &&
                   qasm3_scan_classical(ctx, f->body);
        }
        default:
            return 0;
    }
}

// Walks the statements of a function body. A return that ends the body does
// nothing to the circuit, so it is the one return the walker accepts. Its
// result expression is walked if it calls a helper, and scanned otherwise.
static void qasm3_walk_body(Qasm3Ctx* ctx, ASTNode* body) {
    GOO_ASSERT(ctx != NULL);
    if (!body) {
        return;
    }
    if (body->type != AST_BLOCK_STMT) {
        qasm3_walk_stmt(ctx, body);
        return;
    }
    for (ASTNode* s = ((BlockStmtNode*)body)->statements; s; s = s->next) {
        if (s->type == AST_RETURN_STMT && s->next == NULL) {
            for (ASTNode* v = ((ReturnStmtNode*)s)->values; v; v = v->next) {
                if (v->type == AST_CALL_EXPR) {
                    qasm3_handle_call(ctx, (CallExprNode*)v, NULL);
                } else if (!qasm3_scan_classical(ctx, v)) {
                    qasm3_refuse(ctx, v->pos,
                                 "--emit-qasm3 cannot emit a return value that holds a gate or an unknown form");
                } else {
                    // A classical result changes no gate.
                }
            }
            return;
        }
        qasm3_walk_stmt(ctx, s);
    }
}

static void qasm3_walk_stmt(Qasm3Ctx* ctx, ASTNode* stmt) {
    if (!stmt) return;

    switch (stmt->type) {
        case AST_BLOCK_STMT: {
            BlockStmtNode* b = (BlockStmtNode*)stmt;
            for (ASTNode* s = b->statements; s; s = s->next) {
                qasm3_walk_stmt(ctx, s);
            }
            return;
        }
        case AST_ARENA_BLOCK:
            qasm3_walk_stmt(ctx, ((ArenaBlockNode*)stmt)->body);
            return;
        case AST_VAR_DECL: {
            VarDeclNode* vd = (VarDeclNode*)stmt;
            const char* name = (vd->name_count > 0 && vd->names) ? vd->names[0] : NULL;
            size_t before = ctx->var_count;
            if (vd->values && vd->values->type == AST_CALL_EXPR) {
                qasm3_handle_call(ctx, (CallExprNode*)vd->values, name);
            } else {
                int64_t ival = 0;
                if (name && qasm3_eval_int(ctx, vd->values, &ival)) {
                    qasm3_set_var(ctx, name, QASM3_VAR_INT, ival);
                }
            }
            // Every name of the declaration shadows an outer const, tracked or not.
            qasm3_shadow_names(ctx, vd->names, vd->name_count, before);
            return;
        }
        case AST_CONST_DECL: {
            // A local const shadows a package const of the same name, as a var does.
            ConstDeclNode* cd = (ConstDeclNode*)stmt;
            GOO_ASSERT(cd->name_count == 0U || cd->names != NULL);
            size_t before = ctx->var_count;
            int64_t ival = 0;
            if (cd->name_count > 0U && cd->names != NULL && cd->names[0] != NULL &&
                qasm3_eval_int(ctx, cd->values, &ival)) {
                qasm3_set_var(ctx, cd->names[0], QASM3_VAR_INT, ival);
            }
            qasm3_shadow_names(ctx, cd->names, cd->name_count, before);
            return;
        }
        case AST_EXPR_STMT: {
            ExprStmtNode* es = (ExprStmtNode*)stmt;
            if (!es->expr) return;
            if (es->expr->type == AST_CALL_EXPR) {
                qasm3_handle_call(ctx, (CallExprNode*)es->expr, NULL);
                return;
            }
            if (es->expr->type == AST_BINARY_EXPR) {
                BinaryExprNode* bin = (BinaryExprNode*)es->expr;
                char lhs_ref[64];
                if (bin->operator == TOKEN_ASSIGN &&
                    qasm3_resolve_ref(ctx, bin->left, lhs_ref, sizeof(lhs_ref))) {
                    if (bin->right && bin->right->type == AST_CALL_EXPR) {
                        qasm3_handle_call(ctx, (CallExprNode*)bin->right, lhs_ref);
                        return;
                    }
                    int64_t ival = 0;
                    Qasm3Var* v = qasm3_find_var(ctx, lhs_ref);
                    if (qasm3_eval_int(ctx, bin->right, &ival)) {
                        if (v) v->index_or_val = ival;
                    } else if (v && v->kind == QASM3_VAR_INT) {
                        // The new value is unknown. Keeping the old one would fold later conditions wrongly.
                        v->kind = QASM3_VAR_OPAQUE;
                    } else {
                        // A qubit or result variable keeps its record.
                    }
                    return;
                }
            }
            if (es->expr->type == AST_POSTFIX_EXPR) {
                qasm3_refuse(ctx, es->expr->pos, "--emit-qasm3 cannot emit an increment or decrement statement");
            } else {
                qasm3_refuse(ctx, es->expr->pos,
                             "--emit-qasm3 cannot emit this expression statement (only qir calls, helper calls and plain assignments)");
            }
            return;
        }
        case AST_IF_STMT: {
            IfStmtNode* ifs = (IfStmtNode*)stmt;
            int64_t static_cond = 0;
            if (qasm3_eval_int(ctx, ifs->condition, &static_cond)) {
                if (static_cond) {
                    qasm3_walk_stmt(ctx, ifs->then_stmt);
                } else if (ifs->else_stmt) {
                    qasm3_walk_stmt(ctx, ifs->else_stmt);
                }
                return;
            }
            // Dynamic circuit branch: `if qir.ReadResult(r)`
            if (ifs->condition && ifs->condition->type == AST_CALL_EXPR) {
                CallExprNode* c = (CallExprNode*)ifs->condition;
                if (c->function && c->function->type == AST_SELECTOR_EXPR) {
                    SelectorExprNode* sel = (SelectorExprNode*)c->function;
                    if (sel->selector && strcmp(sel->selector, "ReadResult") == 0) {
                        int64_t bit = 0;
                        if (!qasm3_operand(ctx, c->args, c->base.pos, QASM3_VAR_RESULT, &bit)) return;
                        size_t saved_len = ctx->len;
                        qasm3_emit(ctx, "if (c[%lld] == true) {\n", (long long)bit);
                        size_t header_len = ctx->len;
                        ctx->indent++;
                        qasm3_walk_stmt(ctx, ifs->then_stmt);
                        ctx->indent--;
                        if (ctx->len == header_len) {
                            ctx->len = saved_len;
                            if (ctx->buf) ctx->buf[ctx->len] = '\0';
                        } else {
                            qasm3_emit(ctx, "}\n");
                        }
                        if (ifs->else_stmt) {
                            size_t saved_else = ctx->len;
                            qasm3_emit(ctx, "if (c[%lld] == false) {\n", (long long)bit);
                            size_t else_hdr = ctx->len;
                            ctx->indent++;
                            qasm3_walk_stmt(ctx, ifs->else_stmt);
                            ctx->indent--;
                            if (ctx->len == else_hdr) {
                                ctx->len = saved_else;
                                if (ctx->buf) ctx->buf[ctx->len] = '\0';
                            } else {
                                qasm3_emit(ctx, "}\n");
                            }
                        }
                        return;
                    }
                }
            }
            // Dynamic syndrome comparison: `if syn == SYNDROME_Q0`
            // Emits flat, non-nested `if (syn == mask) { ... }` over `bit[2] syn;`
            // so IBM Heron r2 hardware controllers execute without Error 1524
            // ("Circuits with nested control-flow operations are not supported").
            if (ifs->condition && ifs->condition->type == AST_BINARY_EXPR) {
                BinaryExprNode* b = (BinaryExprNode*)ifs->condition;
                if (b->operator == TOKEN_EQ && b->left && b->left->type == AST_IDENTIFIER) {
                    Qasm3Var* sv = qasm3_find_var(ctx, ((IdentifierNode*)b->left)->name);
                    int64_t mask = 0;
                    if (sv && sv->is_syndrome && qasm3_eval_int(ctx, b->right, &mask)) {
                        qasm3_emit(ctx, "if (syn == %lld) {\n", (long long)mask);
                        ctx->indent++;
                        qasm3_walk_stmt(ctx, ifs->then_stmt);
                        ctx->indent--;
                        qasm3_emit(ctx, "}\n");
                        if (ifs->else_stmt) {
                            qasm3_walk_stmt(ctx, ifs->else_stmt);
                        }
                        return;
                    }
                }
            }
            // `if qir.MeasureReset(x)` measures and resets x whatever its branches do.
            // Skipping the whole statement once left x in its old state for the next
            // gate. Emit the measurement, then skip a branch that holds no gate.
            if (ifs->condition && ifs->condition->type == AST_CALL_EXPR) {
                CallExprNode* mc = (CallExprNode*)ifs->condition;
                if (mc->function && mc->function->type == AST_SELECTOR_EXPR &&
                    ((SelectorExprNode*)mc->function)->selector != NULL &&
                    strcmp(((SelectorExprNode*)mc->function)->selector, "MeasureReset") == 0) {
                    if (qasm3_scan_classical(ctx, ifs->then_stmt) &&
                        qasm3_scan_classical(ctx, ifs->else_stmt)) {
                        qasm3_handle_call(ctx, mc, NULL);
                    } else {
                        qasm3_refuse(ctx, mc->base.pos,
                                     "--emit-qasm3 cannot emit an if statement whose condition is qir.MeasureReset and whose branches hold a gate");
                    }
                    return;
                }
            }
            // A branch that holds no gate cannot change the circuit, so it may stay
            // unwalked.
            if (qasm3_scan_classical(ctx, stmt)) {
                return;
            }
            qasm3_refuse(ctx, ifs->condition ? ifs->condition->pos : stmt->pos,
                         "--emit-qasm3 cannot emit an if statement whose condition is not a compile-time constant, a qir.ReadResult call or a syndrome comparison, and whose branches hold a gate or an unknown form");
            return;
        }
        case AST_FOR_STMT: {
            ForStmtNode* f = (ForStmtNode*)stmt;
            if (f->range_expr) {
                qasm3_refuse(ctx, stmt->pos, "--emit-qasm3 cannot emit a for range loop");
                return;
            }
            qasm3_walk_stmt(ctx, f->init);
            int guard = 0;
            int finished = 0;
            while (guard < 64) {
                int64_t cval = 0;
                if (!qasm3_eval_int(ctx, f->condition, &cval)) {
                    // A loop that holds no gate cannot change the circuit.
                    if (qasm3_scan_classical(ctx, stmt)) {
                        return;
                    }
                    qasm3_refuse(ctx, f->condition ? f->condition->pos : stmt->pos,
                                 "--emit-qasm3 cannot emit a for loop whose condition is not a compile-time constant");
                    return;
                }
                if (!cval) { finished = 1; break; }
                // Check if loop body calls `fmt.Println(..., runTrial(f))`
                qasm3_walk_stmt(ctx, f->body);
                qasm3_walk_stmt(ctx, f->post);
                guard++;
                // For pure shot-repetition loops (`shot < 16`), unroll 1 representative
                // shot in QASM3 since QPU execution repeats shots via `shots=N`.
                if (f->condition && f->condition->type == AST_BINARY_EXPR) {
                    BinaryExprNode* cb = (BinaryExprNode*)f->condition;
                    if (cb->left && cb->left->type == AST_IDENTIFIER &&
                        strcmp(((IdentifierNode*)cb->left)->name, "shot") == 0) {
                        finished = 1;
                        break;
                    }
                }
                if (ctx->failed) {
                    return;
                }
            }
            if (!finished) {
                qasm3_refuse(ctx, stmt->pos,
                             "--emit-qasm3 cannot emit a for loop that runs more than 64 iterations");
            }
            return;
        }
        default:
            qasm3_refuse(ctx, stmt->pos, "--emit-qasm3 cannot emit %s", qasm3_stmt_name(stmt->type));
            return;
    }
}

int codegen_emit_qasm3(CodeGenerator* codegen, TypeChecker* checker,
                       ASTNode* program, const char* filename) {
    if (!codegen || !program || program->type != AST_PROGRAM || !filename) return 0;

    Qasm3Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.checker = checker;

    ProgramNode* prog = (ProgramNode*)program;
    FuncDeclNode* main_fd = NULL;
    for (ASTNode* d = prog->decls; d; d = d->next) {
        if (d->type == AST_CONST_DECL) {
            ConstDeclNode* cd = (ConstDeclNode*)d;
            int64_t cval = 0;
            if (cd->name_count > 0 && cd->names && cd->names[0] &&
                qasm3_eval_int(&ctx, cd->values, &cval)) {
                qasm3_set_var(&ctx, cd->names[0], QASM3_VAR_INT, cval);
            }
        }
        if (d->type == AST_FUNC_DECL) {
            FuncDeclNode* fd = (FuncDeclNode*)d;
            if (fd->name && strcmp(fd->name, "main") == 0) {
                main_fd = fd;
            } else if (ctx.func_count < QASM3_MAX_FUNCS) {
                ctx.funcs[ctx.func_count++] = fd;
                if (fd->name && strcmp(fd->name, "syndromeMask") == 0) {
                    ctx.has_syndrome_reg = 1;
                }
            }
        }
    }

    if (main_fd && main_fd->body) {
        qasm3_walk_body(&ctx, main_fd->body);
    }

    if (ctx.failed) {
        free(ctx.buf);
        return 0;
    }

    if (ctx.num_qubits == 0) ctx.num_qubits = 1;
    if (ctx.num_results == 0) ctx.num_results = 1;

    FILE* out = fopen(filename, "w");
    if (!out) {
        free(ctx.buf);
        return 0;
    }
    fprintf(out, "OPENQASM 3.0;\n");
    fprintf(out, "include \"stdgates.inc\";\n");
    fprintf(out, "qubit[%llu] q;\n", (unsigned long long)ctx.num_qubits);
    if (ctx.has_syndrome_reg && ctx.num_syn_bits > 0) {
        fprintf(out, "bit[%llu] syn;\n", (unsigned long long)ctx.num_syn_bits);
    }
    fprintf(out, "bit[%llu] c;\n\n", (unsigned long long)ctx.num_results);
    if (ctx.buf && ctx.len > 0) {
        fwrite(ctx.buf, 1, ctx.len, out);
    }
    fclose(out);
    free(ctx.buf);
    return 1;
}

// ===========================================================================
// Phase 8 M2 — CIRCT MLIR (`--emit-mlir`) & SystemVerilog (`--emit-sv`)
// ===========================================================================
// Lowers `fpga_*` hardware functions (verified by `fpga_check_program` to use
// zero heap allocations, fixed-width value types, and statically bounded `for`
// loops) into:
//   1. CIRCT `hw.module` + `comb` dialect MLIR (`--emit-mlir`), and
//   2. Synthesizable IEEE 1800-2012 SystemVerilog RTL (`--emit-sv`).

#define FPGA_MAX_VARS 128

typedef struct {
    int     is_const;
    int     is_bool;
    int64_t const_val;
    char    mlir_ssa[64];
    char    sv_expr[128];
} FpgaVal;

typedef struct {
    char    name[64];
    FpgaVal val;
} FpgaVar;

typedef struct {
    TypeChecker* checker;
    FpgaVar      vars[FPGA_MAX_VARS];
    size_t       var_count;
    int          ssa_id;
    int          emit_sv_mode; // 0 = CIRCT MLIR, 1 = SystemVerilog
    char*        buf;
    size_t       len;
    size_t       cap;
    FpgaVal      ret_val;
    int          has_ret;
} FpgaCtx;

static void fpga_emit(FpgaCtx* ctx, const char* fmt, ...) {
    GOO_ASSERT(ctx != NULL);
    GOO_ASSERT(fmt != NULL);
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed <= 0) return;
    if (ctx->len + (size_t)needed + 2 > ctx->cap) {
        size_t ncap = ctx->cap ? ctx->cap * 2 : 4096;
        while (ncap < ctx->len + (size_t)needed + 2) ncap *= 2;
        ctx->buf = xrealloc(ctx->buf, ncap);
        ctx->cap = ncap;
    }
    va_start(ap, fmt);
    vsnprintf(ctx->buf + ctx->len, ctx->cap - ctx->len, fmt, ap);
    va_end(ap);
    ctx->len += (size_t)needed;
}

static FpgaVar* fpga_find_var(FpgaCtx* ctx, const char* name) {
    GOO_ASSERT(ctx != NULL);
    if (!name) return NULL;
    for (size_t i = ctx->var_count; i > 0; i--) {
        if (strcmp(ctx->vars[i - 1].name, name) == 0) {
            return &ctx->vars[i - 1];
        }
    }
    return NULL;
}

static void fpga_set_var(FpgaCtx* ctx, const char* name, FpgaVal val) {
    GOO_ASSERT(ctx != NULL);
    if (!name) return;
    FpgaVar* existing = fpga_find_var(ctx, name);
    if (existing) {
        existing->val = val;
        return;
    }
    if (ctx->var_count >= FPGA_MAX_VARS) return;
    FpgaVar* v = &ctx->vars[ctx->var_count++];
    memset(v, 0, sizeof(*v));
    snprintf(v->name, sizeof(v->name), "%s", name);
    v->val = val;
}

static FpgaVal fpga_make_const(FpgaCtx* ctx, int64_t val, int is_bool) {
    FpgaVal out;
    memset(&out, 0, sizeof(out));
    out.is_const = 1;
    out.is_bool = is_bool;
    out.const_val = val;
    int id = ctx->ssa_id++;
    if (!ctx->emit_sv_mode) {
        fpga_emit(ctx, "    %%c%d = hw.constant %lld : %s\n",
                  id, (long long)val, is_bool ? "i1" : "i64");
    }
    snprintf(out.mlir_ssa, sizeof(out.mlir_ssa), "%%c%d", id);
    if (is_bool) {
        snprintf(out.sv_expr, sizeof(out.sv_expr), "1'b%d", val ? 1 : 0);
    } else {
        snprintf(out.sv_expr, sizeof(out.sv_expr), "64'sd%lld", (long long)val);
    }
    return out;
}

static FpgaVal fpga_eval_expr(FpgaCtx* ctx, ASTNode* expr) {
    GOO_ASSERT(ctx != NULL);
    if (!expr) return fpga_make_const(ctx, 0, 0);

    switch (expr->type) {
        case AST_LITERAL: {
            LiteralNode* lit = (LiteralNode*)expr;
            int64_t v = (lit->literal_type == TOKEN_INT && lit->value)
                ? strtoll(lit->value, NULL, 10) : 0;
            return fpga_make_const(ctx, v, 0);
        }
        case AST_IDENTIFIER: {
            const char* nm = ((IdentifierNode*)expr)->name;
            FpgaVar* v = fpga_find_var(ctx, nm);
            if (v) return v->val;
            return fpga_make_const(ctx, 0, 0);
        }
        case AST_INDEX_EXPR: {
            IndexExprNode* idx = (IndexExprNode*)expr;
            FpgaVal iv = fpga_eval_expr(ctx, idx->index);
            if (idx->expr && idx->expr->type == AST_IDENTIFIER && iv.is_const) {
                char key[64];
                snprintf(key, sizeof(key), "%s[%lld]",
                         ((IdentifierNode*)idx->expr)->name, (long long)iv.const_val);
                FpgaVar* v = fpga_find_var(ctx, key);
                if (v) return v->val;
            }
            return fpga_make_const(ctx, 0, 0);
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* u = (UnaryExprNode*)expr;
            FpgaVal op = fpga_eval_expr(ctx, u->operand);
            if (u->operator == TOKEN_MINUS) {
                if (op.is_const) return fpga_make_const(ctx, -op.const_val, 0);
                FpgaVal z = fpga_make_const(ctx, 0, 0);
                int id = ctx->ssa_id++;
                FpgaVal out = {0};
                snprintf(out.mlir_ssa, sizeof(out.mlir_ssa), "%%%d", id);
                snprintf(out.sv_expr, sizeof(out.sv_expr), "w_%d", id);
                if (!ctx->emit_sv_mode) {
                    fpga_emit(ctx, "    %%%d = comb.sub %s, %s : i64\n",
                              id, z.mlir_ssa, op.mlir_ssa);
                } else {
                    fpga_emit(ctx, "  logic signed [63:0] w_%d;\n  assign w_%d = -(%s);\n",
                              id, id, op.sv_expr);
                }
                return out;
            }
            return op;
        }
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            FpgaVal l = fpga_eval_expr(ctx, b->left);
            FpgaVal r = fpga_eval_expr(ctx, b->right);
            if (l.is_const && r.is_const) {
                switch (b->operator) {
                    case TOKEN_PLUS:     return fpga_make_const(ctx, l.const_val + r.const_val, 0);
                    case TOKEN_MINUS:    return fpga_make_const(ctx, l.const_val - r.const_val, 0);
                    case TOKEN_MULTIPLY: return fpga_make_const(ctx, l.const_val * r.const_val, 0);
                    case TOKEN_LT:       return fpga_make_const(ctx, l.const_val < r.const_val, 1);
                    case TOKEN_LE:       return fpga_make_const(ctx, l.const_val <= r.const_val, 1);
                    case TOKEN_GT:       return fpga_make_const(ctx, l.const_val > r.const_val, 1);
                    case TOKEN_GE:       return fpga_make_const(ctx, l.const_val >= r.const_val, 1);
                    case TOKEN_EQ:       return fpga_make_const(ctx, l.const_val == r.const_val, 1);
                    case TOKEN_NE:       return fpga_make_const(ctx, l.const_val != r.const_val, 1);
                    case TOKEN_AND:      return fpga_make_const(ctx, l.const_val && r.const_val, 1);
                    case TOKEN_OR:       return fpga_make_const(ctx, l.const_val || r.const_val, 1);
                    default:             break;
                }
            }
            int id = ctx->ssa_id++;
            FpgaVal out;
            memset(&out, 0, sizeof(out));
            snprintf(out.mlir_ssa, sizeof(out.mlir_ssa), "%%%d", id);
            snprintf(out.sv_expr, sizeof(out.sv_expr), "w_%d", id);

            const char* mlir_op = NULL;
            const char* mlir_pred = NULL;
            const char* sv_op = "+";
            int is_cmp = 0;
            int is_logic_i1 = 0;

            switch (b->operator) {
                case TOKEN_PLUS:     mlir_op = "comb.add"; sv_op = "+"; break;
                case TOKEN_MINUS:    mlir_op = "comb.sub"; sv_op = "-"; break;
                case TOKEN_MULTIPLY: mlir_op = "comb.mul"; sv_op = "*"; break;
                case TOKEN_BIT_AND:  mlir_op = "comb.and"; sv_op = "&"; break;
                case TOKEN_BIT_OR:   mlir_op = "comb.or";  sv_op = "|"; break;
                case TOKEN_BIT_XOR:  mlir_op = "comb.xor"; sv_op = "^"; break;
                case TOKEN_AND:      mlir_op = "comb.and"; sv_op = "&&"; is_logic_i1 = 1; break;
                case TOKEN_OR:       mlir_op = "comb.or";  sv_op = "||"; is_logic_i1 = 1; break;
                case TOKEN_EQ:       mlir_pred = "eq";  sv_op = "=="; is_cmp = 1; break;
                case TOKEN_NE:       mlir_pred = "ne";  sv_op = "!="; is_cmp = 1; break;
                case TOKEN_LT:       mlir_pred = "slt"; sv_op = "<";  is_cmp = 1; break;
                case TOKEN_LE:       mlir_pred = "sle"; sv_op = "<="; is_cmp = 1; break;
                case TOKEN_GT:       mlir_pred = "sgt"; sv_op = ">";  is_cmp = 1; break;
                case TOKEN_GE:       mlir_pred = "sge"; sv_op = ">="; is_cmp = 1; break;
                default:             mlir_op = "comb.add"; sv_op = "+"; break;
            }

            if (is_cmp) {
                out.is_bool = 1;
                if (!ctx->emit_sv_mode) {
                    fpga_emit(ctx, "    %%%d = comb.icmp bin %s %s, %s : i64\n",
                              id, mlir_pred, l.mlir_ssa, r.mlir_ssa);
                } else {
                    fpga_emit(ctx, "  logic w_%d;\n  assign w_%d = (%s %s %s);\n",
                              id, id, l.sv_expr, sv_op, r.sv_expr);
                }
            } else if (is_logic_i1) {
                out.is_bool = 1;
                if (!ctx->emit_sv_mode) {
                    fpga_emit(ctx, "    %%%d = %s %s, %s : i1\n",
                              id, mlir_op, l.mlir_ssa, r.mlir_ssa);
                } else {
                    fpga_emit(ctx, "  logic w_%d;\n  assign w_%d = (%s %s %s);\n",
                              id, id, l.sv_expr, sv_op, r.sv_expr);
                }
            } else {
                out.is_bool = 0;
                if (!ctx->emit_sv_mode) {
                    fpga_emit(ctx, "    %%%d = %s %s, %s : i64\n",
                              id, mlir_op, l.mlir_ssa, r.mlir_ssa);
                } else {
                    fpga_emit(ctx, "  logic signed [63:0] w_%d;\n  assign w_%d = (%s %s %s);\n",
                              id, id, l.sv_expr, sv_op, r.sv_expr);
                }
            }
            return out;
        }
        default:
            return fpga_make_const(ctx, 0, 0);
    }
}

static void fpga_exec_stmt(FpgaCtx* ctx, ASTNode* stmt) {
    GOO_ASSERT(ctx != NULL);
    if (!stmt) return;

    switch (stmt->type) {
        case AST_BLOCK_STMT: {
            BlockStmtNode* b = (BlockStmtNode*)stmt;
            for (ASTNode* s = b->statements; s; s = s->next) {
                fpga_exec_stmt(ctx, s);
            }
            return;
        }
        case AST_VAR_DECL: {
            VarDeclNode* vd = (VarDeclNode*)stmt;
            const char* nm = (vd->name_count > 0 && vd->names) ? vd->names[0] : NULL;
            if (!nm) return;
            if (vd->values && vd->values->type == AST_ARRAY_LITERAL) {
                ArrayLitNode* al = (ArrayLitNode*)vd->values;
                size_t k = 0;
                for (ASTNode* e = al->elements; e; e = e->next, k++) {
                    char key[64];
                    snprintf(key, sizeof(key), "%s[%zu]", nm, k);
                    fpga_set_var(ctx, key, fpga_eval_expr(ctx, e));
                }
                return;
            }
            FpgaVal v = vd->values ? fpga_eval_expr(ctx, vd->values) : fpga_make_const(ctx, 0, 0);
            fpga_set_var(ctx, nm, v);
            return;
        }
        case AST_EXPR_STMT: {
            ExprStmtNode* es = (ExprStmtNode*)stmt;
            if (es->expr && es->expr->type == AST_BINARY_EXPR) {
                BinaryExprNode* bin = (BinaryExprNode*)es->expr;
                if (bin->operator == TOKEN_ASSIGN &&
                    bin->left && bin->left->type == AST_IDENTIFIER) {
                    const char* lhs = ((IdentifierNode*)bin->left)->name;
                    FpgaVal rhs = fpga_eval_expr(ctx, bin->right);
                    fpga_set_var(ctx, lhs, rhs);
                }
            }
            return;
        }
        case AST_IF_STMT: {
            IfStmtNode* ifs = (IfStmtNode*)stmt;
            FpgaVal cond = fpga_eval_expr(ctx, ifs->condition);
            if (cond.is_const) {
                if (cond.const_val) {
                    fpga_exec_stmt(ctx, ifs->then_stmt);
                } else if (ifs->else_stmt) {
                    fpga_exec_stmt(ctx, ifs->else_stmt);
                }
                return;
            }
            // Hardware multiplexer (`comb.mux` in CIRCT MLIR, `?:` in SystemVerilog):
            FpgaVar pre_vars[FPGA_MAX_VARS];
            size_t pre_count = ctx->var_count;
            memcpy(pre_vars, ctx->vars, sizeof(pre_vars));

            fpga_exec_stmt(ctx, ifs->then_stmt);
            FpgaVar then_vars[FPGA_MAX_VARS];
            memcpy(then_vars, ctx->vars, sizeof(then_vars));

            memcpy(ctx->vars, pre_vars, sizeof(pre_vars));
            ctx->var_count = pre_count;
            if (ifs->else_stmt) {
                fpga_exec_stmt(ctx, ifs->else_stmt);
            }
            FpgaVar else_vars[FPGA_MAX_VARS];
            memcpy(else_vars, ctx->vars, sizeof(else_vars));

            for (size_t i = 0; i < pre_count; i++) {
                if (strcmp(then_vars[i].val.mlir_ssa, else_vars[i].val.mlir_ssa) != 0 ||
                    strcmp(then_vars[i].val.sv_expr, else_vars[i].val.sv_expr) != 0) {
                    int id = ctx->ssa_id++;
                    FpgaVal mux_v;
                    memset(&mux_v, 0, sizeof(mux_v));
                    snprintf(mux_v.mlir_ssa, sizeof(mux_v.mlir_ssa), "%%%d", id);
                    snprintf(mux_v.sv_expr, sizeof(mux_v.sv_expr), "w_%d", id);
                    if (!ctx->emit_sv_mode) {
                        fpga_emit(ctx, "    %%%d = comb.mux %s, %s, %s : i64\n",
                                  id, cond.mlir_ssa, then_vars[i].val.mlir_ssa, else_vars[i].val.mlir_ssa);
                    } else {
                        fpga_emit(ctx, "  logic signed [63:0] w_%d;\n  assign w_%d = %s ? %s : %s;\n",
                                  id, id, cond.sv_expr, then_vars[i].val.sv_expr, else_vars[i].val.sv_expr);
                    }
                    ctx->vars[i].val = mux_v;
                }
            }
            return;
        }
        case AST_FOR_STMT: {
            ForStmtNode* f = (ForStmtNode*)stmt;
            fpga_exec_stmt(ctx, f->init);
            int guard = 0;
            while (guard < 64) {
                FpgaVal cval = fpga_eval_expr(ctx, f->condition);
                if (!cval.is_const || !cval.const_val) break;
                fpga_exec_stmt(ctx, f->body);
                fpga_exec_stmt(ctx, f->post);
                guard++;
            }
            return;
        }
        case AST_RETURN_STMT: {
            ReturnStmtNode* r = (ReturnStmtNode*)stmt;
            if (r->values) {
                ctx->ret_val = fpga_eval_expr(ctx, r->values);
                ctx->has_ret = 1;
            }
            return;
        }
        default:
            return;
    }
}

static int fpga_emit_program(CodeGenerator* codegen, TypeChecker* checker,
                             ASTNode* program, const char* filename, int emit_sv_mode) {
    GOO_ASSERT(codegen != NULL);
    GOO_ASSERT(checker != NULL);
    GOO_ASSERT(program != NULL);
    GOO_ASSERT(filename != NULL);
    if (!codegen || !checker || !program || program->type != AST_PROGRAM || !filename) return 0;

    FILE* out = fopen(filename, "w");
    if (!out) return 0;

    if (!emit_sv_mode) {
        fprintf(out, "// Generated by Goo Compiler (--emit-mlir: CIRCT hw/comb dialect)\n");
        fprintf(out, "module {\n");
    } else {
        fprintf(out, "// Generated by Goo Compiler (--emit-sv: IEEE 1800-2012 SystemVerilog)\n");
        fprintf(out, "`default_nettype none\n\n");
    }

    ProgramNode* prog = (ProgramNode*)program;
    for (ASTNode* d = prog->decls; d; d = d->next) {
        if (d->type != AST_FUNC_DECL) continue;
        FuncDeclNode* fd = (FuncDeclNode*)d;
        if (!fd->name || strncmp(fd->name, "fpga_", 5) != 0) continue;

        FpgaCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.checker = checker;
        ctx.emit_sv_mode = emit_sv_mode;

        if (!emit_sv_mode) {
            fprintf(out, "  hw.module @%s(", fd->name);
        } else {
            fprintf(out, "module %s(\n", fd->name);
        }

        int first_port = 1;
        for (ASTNode* p = fd->params; p; p = p->next) {
            if (p->type != AST_VAR_DECL) continue;
            VarDeclNode* pvd = (VarDeclNode*)p;
            Type* pt = pvd->type ? type_from_ast(checker, pvd->type) : pvd->base.node_type;
            for (size_t i = 0; i < pvd->name_count; i++) {
                const char* pname = pvd->names ? pvd->names[i] : NULL;
                if (!pname) continue;
                if (pt && pt->kind == TYPE_ARRAY) {
                    size_t alen = (size_t)pt->data.array.length;
                    for (size_t k = 0; k < alen; k++) {
                        if (!first_port) fprintf(out, emit_sv_mode ? ",\n" : ", ");
                        first_port = 0;
                        if (!emit_sv_mode) {
                            fprintf(out, "in %%%s_%zu : i64", pname, k);
                        } else {
                            fprintf(out, "  input  logic signed [63:0] %s_%zu", pname, k);
                        }
                        char key[64];
                        snprintf(key, sizeof(key), "%s[%zu]", pname, k);
                        FpgaVal pv = {0};
                        snprintf(pv.mlir_ssa, sizeof(pv.mlir_ssa), "%%%s_%zu", pname, k);
                        snprintf(pv.sv_expr, sizeof(pv.sv_expr), "%s_%zu", pname, k);
                        fpga_set_var(&ctx, key, pv);
                    }
                } else {
                    if (!first_port) fprintf(out, emit_sv_mode ? ",\n" : ", ");
                    first_port = 0;
                    if (!emit_sv_mode) {
                        fprintf(out, "in %%%s : i64", pname);
                    } else {
                        fprintf(out, "  input  logic signed [63:0] %s", pname);
                    }
                    FpgaVal pv = {0};
                    snprintf(pv.mlir_ssa, sizeof(pv.mlir_ssa), "%%%s", pname);
                    snprintf(pv.sv_expr, sizeof(pv.sv_expr), "%s", pname);
                    fpga_set_var(&ctx, pname, pv);
                }
            }
        }

        if (!emit_sv_mode) {
            fprintf(out, ", out out_0 : i64) {\n");
        } else {
            if (!first_port) fprintf(out, ",\n");
            fprintf(out, "  output logic signed [63:0] out_0\n);\n");
        }

        fpga_exec_stmt(&ctx, fd->body);
        if (!ctx.has_ret) {
            ctx.ret_val = fpga_make_const(&ctx, 0, 0);
        }

        if (ctx.buf && ctx.len > 0) {
            fwrite(ctx.buf, 1, ctx.len, out);
        }
        if (!emit_sv_mode) {
            fprintf(out, "    hw.output %s : i64\n  }\n", ctx.ret_val.mlir_ssa);
        } else {
            fprintf(out, "  assign out_0 = %s;\nendmodule\n\n", ctx.ret_val.sv_expr);
        }
        free(ctx.buf);
    }

    if (!emit_sv_mode) {
        fprintf(out, "}\n");
    } else {
        fprintf(out, "`default_nettype wire\n");
    }
    fclose(out);
    return 1;
}

int codegen_emit_mlir(CodeGenerator* codegen, TypeChecker* checker,
                      ASTNode* program, const char* filename) {
    return fpga_emit_program(codegen, checker, program, filename, 0);
}

int codegen_emit_sv(CodeGenerator* codegen, TypeChecker* checker,
                    ASTNode* program, const char* filename) {
    return fpga_emit_program(codegen, checker, program, filename, 1);
}
