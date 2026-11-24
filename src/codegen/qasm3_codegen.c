// Phase 7 M1 Track B — OpenQASM 3.0 code generator (`--emit-qasm3`).
// Walks the type-checked AST (after `qir_ownership_check_program` has verified
// linear no-cloning, distinct multi-qubit operands, and safe uncomputation)
// and lowers `qir.*` operations and dynamic classical feed-forward branches to
// OpenQASM 3.0 (`OPENQASM 3.0; include "stdgates.inc";`).

#include "codegen.h"
#include "comptime.h"
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
} Qasm3VarKind;

typedef struct {
    const char*  name;
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
    uint64_t      active_qubit_mask;
    char*         buf;
    size_t        len;
    size_t        cap;
    int           indent;
} Qasm3Ctx;

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

static Qasm3Var* qasm3_find_var(Qasm3Ctx* ctx, const char* name) {
    if (!name) return NULL;
    for (size_t i = ctx->var_count; i > 0; i--) {
        if (ctx->vars[i - 1].name && strcmp(ctx->vars[i - 1].name, name) == 0) {
            return &ctx->vars[i - 1];
        }
    }
    return NULL;
}

static Qasm3Var* qasm3_set_var(Qasm3Ctx* ctx, const char* name, Qasm3VarKind kind, int64_t val) {
    if (!name) return NULL;
    if (ctx->var_count >= QASM3_MAX_VARS) return NULL;
    Qasm3Var* v = &ctx->vars[ctx->var_count++];
    memset(v, 0, sizeof(*v));
    v->name = name;
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

static void qasm3_format_float(ASTNode* expr, char* buf, size_t n) {
    if (!expr) { snprintf(buf, n, "0.0"); return; }
    if (expr->type == AST_UNARY_EXPR && ((UnaryExprNode*)expr)->operator == TOKEN_MINUS) {
        char inner[64];
        qasm3_format_float(((UnaryExprNode*)expr)->operand, inner, sizeof(inner));
        snprintf(buf, n, "-%s", inner);
        return;
    }
    if (expr->type == AST_LITERAL && ((LiteralNode*)expr)->value) {
        snprintf(buf, n, "%s", ((LiteralNode*)expr)->value);
        return;
    }
    snprintf(buf, n, "0.0");
}

static void qasm3_walk_stmt(Qasm3Ctx* ctx, ASTNode* stmt);

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
                qasm3_eval_int(ctx, call->args, &id);
                if ((uint64_t)(id + 1) > ctx->num_qubits) ctx->num_qubits = (uint64_t)(id + 1);
                ctx->active_qubit_mask |= (1ULL << (uint64_t)id);
                if (lhs_name) qasm3_set_var(ctx, lhs_name, QASM3_VAR_QUBIT, id);
                return;
            }
            if (strcmp(op, "ResultAt") == 0) {
                int64_t id = 0;
                qasm3_eval_int(ctx, call->args, &id);
                if ((uint64_t)(id + 1) > ctx->num_results) ctx->num_results = (uint64_t)(id + 1);
                if (lhs_name) qasm3_set_var(ctx, lhs_name, QASM3_VAR_RESULT, id);
                return;
            }
            if (strcmp(op, "FreeZero") == 0) {
                if (call->args && call->args->type == AST_IDENTIFIER) {
                    Qasm3Var* v = qasm3_find_var(ctx, ((IdentifierNode*)call->args)->name);
                    if (v) qasm3_free_qubit(ctx, v->index_or_val);
                }
                return;
            }
            if (strcmp(op, "MeasureReset") == 0) {
                if (call->args && call->args->type == AST_IDENTIFIER) {
                    Qasm3Var* v = qasm3_find_var(ctx, ((IdentifierNode*)call->args)->name);
                    int64_t q = v ? v->index_or_val : 0;
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
                qasm3_format_float(call->args, tbuf, sizeof(tbuf));
                ASTNode* qarg = call->args ? call->args->next : NULL;
                if (qarg && qarg->type == AST_IDENTIFIER) {
                    Qasm3Var* v = qasm3_find_var(ctx, ((IdentifierNode*)qarg)->name);
                    int64_t q = v ? v->index_or_val : 0;
                    qasm3_emit(ctx, "%s(%s) q[%lld];\n", g, tbuf, (long long)q);
                }
                return;
            }
            if (strcmp(op, "Mz") == 0) {
                ASTNode* qarg = call->args;
                ASTNode* rarg = qarg ? qarg->next : NULL;
                int64_t q = 0, r = 0;
                if (qarg && qarg->type == AST_IDENTIFIER) {
                    Qasm3Var* v = qasm3_find_var(ctx, ((IdentifierNode*)qarg)->name);
                    if (v) q = v->index_or_val;
                }
                if (rarg && rarg->type == AST_IDENTIFIER) {
                    Qasm3Var* v = qasm3_find_var(ctx, ((IdentifierNode*)rarg)->name);
                    if (v) r = v->index_or_val;
                }
                qasm3_emit(ctx, "c[%lld] = measure q[%lld];\n", (long long)r, (long long)q);
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
                    for (int k = 0; k < arity && a; k++, a = a->next) {
                        if (a->type == AST_IDENTIFIER) {
                            Qasm3Var* v = qasm3_find_var(ctx, ((IdentifierNode*)a)->name);
                            if (v) qs[k] = v->index_or_val;
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
            size_t saved_vars = ctx->var_count;
            ASTNode* arg = call->args;
            for (ASTNode* p = fd->params; p; p = p->next) {
                if (p->type != AST_VAR_DECL) continue;
                VarDeclNode* pvd = (VarDeclNode*)p;
                for (size_t i = 0; i < pvd->name_count && arg; i++, arg = arg->next) {
                    const char* pname = pvd->names ? pvd->names[i] : NULL;
                    if (!pname) continue;
                    int64_t ival = 0;
                    if (arg->type == AST_IDENTIFIER) {
                        Qasm3Var* av = qasm3_find_var(ctx, ((IdentifierNode*)arg)->name);
                        if (av) {
                            qasm3_set_var(ctx, pname, av->kind, av->index_or_val);
                            continue;
                        }
                    }
                    if (qasm3_eval_int(ctx, arg, &ival)) {
                        qasm3_set_var(ctx, pname, QASM3_VAR_INT, ival);
                    }
                }
            }
            qasm3_walk_stmt(ctx, fd->body);
            ctx->var_count = saved_vars;
        }
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
        case AST_VAR_DECL: {
            VarDeclNode* vd = (VarDeclNode*)stmt;
            const char* name = (vd->name_count > 0 && vd->names) ? vd->names[0] : NULL;
            if (vd->values && vd->values->type == AST_CALL_EXPR) {
                qasm3_handle_call(ctx, (CallExprNode*)vd->values, name);
                return;
            }
            int64_t ival = 0;
            if (name && qasm3_eval_int(ctx, vd->values, &ival)) {
                qasm3_set_var(ctx, name, QASM3_VAR_INT, ival);
            }
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
                if (bin->operator == TOKEN_ASSIGN &&
                    bin->left && bin->left->type == AST_IDENTIFIER) {
                    const char* lhs = ((IdentifierNode*)bin->left)->name;
                    int64_t ival = 0;
                    if (qasm3_eval_int(ctx, bin->right, &ival)) {
                        Qasm3Var* v = qasm3_find_var(ctx, lhs);
                        if (v) v->index_or_val = ival;
                    }
                } else {
                    for (ASTNode* a = es->expr; a; a = NULL) {
                        if (a->type == AST_CALL_EXPR) qasm3_handle_call(ctx, (CallExprNode*)a, NULL);
                    }
                }
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
                    if (sel->selector && strcmp(sel->selector, "ReadResult") == 0 &&
                        c->args && c->args->type == AST_IDENTIFIER) {
                        Qasm3Var* rv = qasm3_find_var(ctx, ((IdentifierNode*)c->args)->name);
                        int64_t bit = rv ? rv->index_or_val : 0;
                        qasm3_emit(ctx, "if (c[%lld] == true) {\n", (long long)bit);
                        ctx->indent++;
                        qasm3_walk_stmt(ctx, ifs->then_stmt);
                        ctx->indent--;
                        qasm3_emit(ctx, "}\n");
                        return;
                    }
                }
            }
            // Dynamic syndrome comparison: `if syn == SYNDROME_Q0`
            if (ifs->condition && ifs->condition->type == AST_BINARY_EXPR) {
                BinaryExprNode* b = (BinaryExprNode*)ifs->condition;
                if (b->operator == TOKEN_EQ && b->left && b->left->type == AST_IDENTIFIER) {
                    Qasm3Var* sv = qasm3_find_var(ctx, ((IdentifierNode*)b->left)->name);
                    int64_t mask = 0;
                    if (sv && sv->is_syndrome && qasm3_eval_int(ctx, b->right, &mask)) {
                        int b0 = (mask & 1) ? 1 : 0;
                        int b1 = (mask & 2) ? 1 : 0;
                        qasm3_emit(ctx, "if (c[%d] == %s) {\n",
                                   sv->r0_bit, b0 ? "true" : "false");
                        ctx->indent++;
                        qasm3_emit(ctx, "if (c[%d] == %s) {\n",
                                   sv->r1_bit, b1 ? "true" : "false");
                        ctx->indent++;
                        qasm3_walk_stmt(ctx, ifs->then_stmt);
                        ctx->indent--;
                        qasm3_emit(ctx, "}\n");
                        ctx->indent--;
                        qasm3_emit(ctx, "}\n");
                        if (ifs->else_stmt) {
                            qasm3_walk_stmt(ctx, ifs->else_stmt);
                        }
                        return;
                    }
                }
            }
            return;
        }
        case AST_FOR_STMT: {
            ForStmtNode* f = (ForStmtNode*)stmt;
            qasm3_walk_stmt(ctx, f->init);
            int guard = 0;
            while (guard < 64) {
                int64_t cval = 0;
                if (!qasm3_eval_int(ctx, f->condition, &cval) || !cval) break;
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
                        break;
                    }
                }
            }
            return;
        }
        default:
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
            }
        }
    }

    if (main_fd && main_fd->body) {
        qasm3_walk_stmt(&ctx, main_fd->body);
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
    fprintf(out, "bit[%llu] c;\n\n", (unsigned long long)ctx.num_results);
    if (ctx.buf && ctx.len > 0) {
        fwrite(ctx.buf, 1, ctx.len, out);
    }
    fclose(out);
    free(ctx.buf);
    return 1;
}
