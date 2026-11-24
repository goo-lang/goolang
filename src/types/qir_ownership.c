// Phase 7 M1 — Linear `qir.Qubit` ownership checker.
// See include/qir_ownership.h and
// docs/superpowers/specs/2026-11-24-quantum-qir-linear-qubits-design.md.

#include "qir_ownership.h"
#include "ownership_checker.h"
#include <stdlib.h>
#include <string.h>

#define QIR_MAX_BINDINGS_PER_FUNC 128

typedef enum {
    QIR_STATE_LIVE = 0,
    QIR_STATE_MOVED,
    QIR_STATE_CONSUMED,
} QirBindingState;

typedef struct {
    const char*     name;              // Borrowed AST identifier string
    QirBindingState state;
    Position        decl_pos;
    Position        transition_pos;
    int             scope_depth;
    int             loop_depth;
    int             is_borrowed_param; // 1 for Qubit params of non-Qubit-returning fns
    int             has_static_id;     // 1 if bound via qir.QubitAt(const_id)
    uint64_t        static_id;
} QirBinding;

typedef struct {
    QirBinding items[QIR_MAX_BINDINGS_PER_FUNC];
    size_t     count;
} QirBindingTable;

typedef struct {
    TypeChecker*    checker;
    QirBindingTable table;
    int             scope_depth;
    int             loop_depth;
    int             block_terminated;
} QirWalkContext;

// --- Type predicates -------------------------------------------------------

static int type_is_qir_qubit(const Type* t) {
    if (!t || t->kind != TYPE_STRUCT) return 0;
    if (!t->owner_package || !t->owner_package->import_path) return 0;
    if (strcmp(t->owner_package->import_path, "qir") != 0) return 0;
    return t->data.struct_type.name &&
           strcmp(t->data.struct_type.name, "Qubit") == 0;
}

// True if `t` is a container, pointer, channel, nullable, error-union, or
// user-defined named struct holding `qir.Qubit`. Anonymous result tuples
// (`func f() (qir.Qubit, qir.Qubit)`, where `struct_type.name == NULL`) are
// permitted so multi-qubit returns work.
static int type_is_illegal_qubit_container(const Type* t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_POINTER:
            return type_is_qir_qubit(t->data.pointer.pointee_type) ||
                   type_is_illegal_qubit_container(t->data.pointer.pointee_type);
        case TYPE_SLICE:
            return type_is_qir_qubit(t->data.slice.element_type) ||
                   type_is_illegal_qubit_container(t->data.slice.element_type);
        case TYPE_ARRAY:
            return type_is_qir_qubit(t->data.array.element_type) ||
                   type_is_illegal_qubit_container(t->data.array.element_type);
        case TYPE_MAP:
            return type_is_qir_qubit(t->data.map.key_type) ||
                   type_is_qir_qubit(t->data.map.value_type) ||
                   type_is_illegal_qubit_container(t->data.map.key_type) ||
                   type_is_illegal_qubit_container(t->data.map.value_type);
        case TYPE_CHANNEL:
            return type_is_qir_qubit(t->data.channel.element_type) ||
                   type_is_illegal_qubit_container(t->data.channel.element_type);
        case TYPE_NULLABLE:
            return type_is_qir_qubit(t->data.nullable.base_type) ||
                   type_is_illegal_qubit_container(t->data.nullable.base_type);
        case TYPE_ERROR_UNION:
            return type_is_qir_qubit(t->data.error_union.value_type) ||
                   type_is_illegal_qubit_container(t->data.error_union.value_type);
        case TYPE_STRUCT: {
            if (type_is_qir_qubit(t)) return 0;
            // Anonymous result-list struct is allowed to carry Qubit results.
            if (!t->data.struct_type.name) return 0;
            for (size_t i = 0; i < t->data.struct_type.field_count; i++) {
                Type* ft = t->data.struct_type.fields[i].type;
                if (type_is_qir_qubit(ft) || type_is_illegal_qubit_container(ft)) {
                    return 1;
                }
            }
            return 0;
        }
        default:
            return 0;
    }
}

static int type_returns_any_qubit(const Type* ret) {
    if (!ret) return 0;
    if (type_is_qir_qubit(ret)) return 1;
    if (ret->kind == TYPE_STRUCT && !ret->data.struct_type.name) {
        for (size_t i = 0; i < ret->data.struct_type.field_count; i++) {
            if (type_is_qir_qubit(ret->data.struct_type.fields[i].type)) return 1;
        }
    }
    return 0;
}

// Resolve `<pkg>.<sel>(...)` against the persistent top-level `TYPE_PACKAGE`
// marker for `"qir"` (alias-safe, identical to `lane_call_is_lanes_selector`).
static int qir_call_is_qir_selector(TypeChecker* checker, ASTNode* call_expr,
                                    const char** out_selector) {
    if (!call_expr || call_expr->type != AST_CALL_EXPR) return 0;
    CallExprNode* call = (CallExprNode*)call_expr;
    if (!call->function || call->function->type != AST_SELECTOR_EXPR) return 0;

    SelectorExprNode* sel = (SelectorExprNode*)call->function;
    if (!sel->expr || sel->expr->type != AST_IDENTIFIER) return 0;
    IdentifierNode* pkg_ident = (IdentifierNode*)sel->expr;

    Variable* pkg_marker = type_checker_lookup_variable(checker, pkg_ident->name);
    if (!pkg_marker || !pkg_marker->package) return 0;
    if (!pkg_marker->package->import_path ||
        strcmp(pkg_marker->package->import_path, "qir") != 0) {
        return 0;
    }

    if (out_selector) *out_selector = sel->selector;
    return 1;
}

static int qir_selector_is_producer(const char* sel) {
    return sel && (strcmp(sel, "Alloc") == 0 || strcmp(sel, "QubitAt") == 0);
}

static int qir_selector_is_consumer(const char* sel) {
    return sel && (strcmp(sel, "FreeZero") == 0 || strcmp(sel, "MeasureReset") == 0);
}

// --- Binding Table Helpers -------------------------------------------------

static QirBinding* qir_find_binding(QirBindingTable* tbl, const char* name) {
    if (!tbl || !name) return NULL;
    // Innermost scope wins (reverse search).
    for (size_t i = tbl->count; i > 0; i--) {
        if (tbl->items[i - 1].name && strcmp(tbl->items[i - 1].name, name) == 0) {
            return &tbl->items[i - 1];
        }
    }
    return NULL;
}

static QirBinding* qir_find_active_static_id(QirBindingTable* tbl, uint64_t static_id) {
    if (!tbl) return NULL;
    for (size_t i = 0; i < tbl->count; i++) {
        QirBinding* b = &tbl->items[i];
        if (b->state == QIR_STATE_LIVE && b->has_static_id && b->static_id == static_id) {
            return b;
        }
    }
    return NULL;
}

static void qir_add_binding(QirWalkContext* ctx, const char* name, Position pos,
                            int is_borrowed_param, int has_static_id, uint64_t static_id) {
    if (!name || strcmp(name, "_") == 0) return;
    if (ctx->table.count >= QIR_MAX_BINDINGS_PER_FUNC) return;

    QirBinding* b = &ctx->table.items[ctx->table.count++];
    b->name = name;
    b->state = QIR_STATE_LIVE;
    b->decl_pos = pos;
    b->transition_pos = pos;
    b->scope_depth = ctx->scope_depth;
    b->loop_depth = ctx->loop_depth;
    b->is_borrowed_param = is_borrowed_param;
    b->has_static_id = has_static_id;
    b->static_id = static_id;
}

// Check that `b` is currently LIVE before reading/borrowing it; emit the
// specific moved vs consumed diagnostic if not.
static int qir_require_live(QirWalkContext* ctx, QirBinding* b, Position use_pos) {
    if (!b) return 0;
    if (b->state == QIR_STATE_MOVED) {
        type_error(ctx->checker, use_pos,
            "use of moved linear qubit '%s' (moved at line %d)",
            b->name, b->transition_pos.line);
        return 0;
    }
    if (b->state == QIR_STATE_CONSUMED) {
        type_error(ctx->checker, use_pos,
            "use of consumed linear qubit '%s' (consumed at line %d)",
            b->name, b->transition_pos.line);
        return 0;
    }
    return 1;
}

// Transition `b` to `new_state` (MOVED or CONSUMED), checking loop-invariance
// (Obligation 5) and borrowed-parameter rules (Obligation 3).
static int qir_transition_binding(QirWalkContext* ctx, QirBinding* b,
                                  QirBindingState new_state, Position pos) {
    if (!qir_require_live(ctx, b, pos)) return 0;

    if (b->is_borrowed_param) {
        type_error(ctx->checker, pos,
            "borrowed linear qubit parameter '%s' cannot be moved or consumed",
            b->name);
        return 0;
    }
    if (ctx->loop_depth > b->loop_depth) {
        type_error(ctx->checker, pos,
            "outer linear qubit '%s' (declared at line %d) cannot be moved or consumed inside a loop",
            b->name, b->decl_pos.line);
        return 0;
    }
    b->state = new_state;
    b->transition_pos = pos;
    if (new_state == QIR_STATE_MOVED) {
        mark_variable_moved(ctx->checker, b->name, pos);
    }
    return 1;
}

// Pop all bindings declared at `scope_depth > target_depth`, enforcing
// Obligation 3 (must-consume before scope exit) when `check_unconsumed` is 1.
static void qir_pop_scope_bindings(QirWalkContext* ctx, int target_depth, int check_unconsumed) {
    while (ctx->table.count > 0 &&
           ctx->table.items[ctx->table.count - 1].scope_depth > target_depth) {
        QirBinding* b = &ctx->table.items[ctx->table.count - 1];
        if (check_unconsumed && !b->is_borrowed_param && b->state == QIR_STATE_LIVE) {
            type_error(ctx->checker, b->decl_pos,
                "linear qubit '%s' (declared at line %d) exited scope without being consumed by qir.FreeZero, qir.MeasureReset, or returned",
                b->name, b->decl_pos.line);
        }
        ctx->table.count--;
    }
}

// Check whether an expression produces a fresh `qir.Qubit` (and if so,
// validate/transition its source). Returns 1 if `rhs` produces a valid
// `qir.Qubit`, setting `*out_has_static` and `*out_static_id`.
static void qir_check_expr(QirWalkContext* ctx, ASTNode* expr, int allow_qubit_value);

static int qir_consume_qubit_rhs(QirWalkContext* ctx, ASTNode* rhs,
                                 int* out_has_static, uint64_t* out_static_id) {
    if (out_has_static) *out_has_static = 0;
    if (out_static_id) *out_static_id = 0;
    if (!rhs) return 0;

    if (rhs->type == AST_IDENTIFIER) {
        const char* src_name = ((IdentifierNode*)rhs)->name;
        QirBinding* src = qir_find_binding(&ctx->table, src_name);
        if (!src) return 0;
        if (out_has_static) *out_has_static = src->has_static_id;
        if (out_static_id) *out_static_id = src->static_id;
        qir_transition_binding(ctx, src, QIR_STATE_MOVED, rhs->pos);
        return 1;
    }

    if (rhs->type == AST_CALL_EXPR) {
        CallExprNode* call = (CallExprNode*)rhs;
        const char* sel = NULL;
        if (qir_call_is_qir_selector(ctx->checker, rhs, &sel)) {
            if (sel && strcmp(sel, "Alloc") == 0) {
                return 1;
            }
            if (sel && strcmp(sel, "QubitAt") == 0) {
                ASTNode* id_arg = call->args;
                uint64_t id_val = 0;
                if (!id_arg || !goo_fold_const_int_ctx(ctx->checker, id_arg, &id_val)) {
                    type_error(ctx->checker, rhs->pos,
                        "qir.QubitAt requires a compile-time constant integer id");
                    return 1;
                }
                QirBinding* existing = qir_find_active_static_id(&ctx->table, id_val);
                if (existing) {
                    type_error(ctx->checker, rhs->pos,
                        "static hardware qubit %llu is already active in binding '%s' (declared at line %d)",
                        (unsigned long long)id_val, existing->name, existing->decl_pos.line);
                }
                if (out_has_static) *out_has_static = 1;
                if (out_static_id) *out_static_id = id_val;
                return 1;
            }
        }
        // User function returning `qir.Qubit`: validate the call arguments
        // (including distinctness and moving any owned Qubit args).
        qir_check_expr(ctx, rhs, 1);
        return 1;
    }

    qir_check_expr(ctx, rhs, 1);
    return 1;
}

// Check a call expression for QIR obligations (consumer calls, unitary borrow
// calls, multi-qubit distinctness, and discarded Qubit results).
static void qir_check_call_expr(QirWalkContext* ctx, ASTNode* expr, int result_is_bound) {
    CallExprNode* call = (CallExprNode*)expr;
    const char* qir_sel = NULL;
    int is_qir = qir_call_is_qir_selector(ctx->checker, expr, &qir_sel);

    if (is_qir && qir_selector_is_producer(qir_sel)) {
        if (!result_is_bound) {
            type_error(ctx->checker, expr->pos,
                "result of 'qir.%s' is a linear qubit and cannot be discarded",
                qir_sel);
        }
        for (ASTNode* a = call->args; a; a = a->next) {
            qir_check_expr(ctx, a, 0);
        }
        return;
    }

    if (is_qir && qir_selector_is_consumer(qir_sel)) {
        ASTNode* arg0 = call->args;
        if (arg0 && arg0->type == AST_IDENTIFIER) {
            const char* qname = ((IdentifierNode*)arg0)->name;
            QirBinding* b = qir_find_binding(&ctx->table, qname);
            Position pos = call->function ? call->function->pos : expr->pos;
            qir_transition_binding(ctx, b, QIR_STATE_CONSUMED, pos);
        } else if (arg0) {
            // e.g. `qir.FreeZero(qir.Alloc())` — direct producer-to-consumer.
            int dummy_has = 0;
            uint64_t dummy_id = 0;
            qir_consume_qubit_rhs(ctx, arg0, &dummy_has, &dummy_id);
        }
        return;
    }

    // Ordinary call or QIR unitary/measurement call:
    // 1. Check non-Qubit subexpressions and collect Qubit arguments.
    // 2. Enforce Obligation 2: all Qubit arguments in a single call must be
    //    distinct live bindings.
    const char* qubit_arg_names[16];
    QirBinding* qubit_arg_bindings[16];
    size_t      qubit_arg_indices[16];
    size_t      qubit_arg_count = 0;

    int callee_returns_qubit = type_returns_any_qubit(expr->node_type);
    if (!callee_returns_qubit && !result_is_bound) {
        // OK: void or non-qubit return.
    } else if (callee_returns_qubit && !result_is_bound) {
        type_error(ctx->checker, expr->pos,
            "call returns a linear 'qir.Qubit' which cannot be discarded");
    }

    if (!is_qir) {
        qir_check_expr(ctx, call->function, 0);
    }

    size_t arg_idx = 1;
    for (ASTNode* a = call->args; a; a = a->next, arg_idx++) {
        int is_qubit_arg = type_is_qir_qubit(a->node_type);
        if (!is_qubit_arg && a->type == AST_IDENTIFIER) {
            if (qir_find_binding(&ctx->table, ((IdentifierNode*)a)->name) != NULL) {
                is_qubit_arg = 1;
            }
        }
        if (is_qubit_arg) {
            if (a->type == AST_IDENTIFIER) {
                const char* aname = ((IdentifierNode*)a)->name;
                QirBinding* b = qir_find_binding(&ctx->table, aname);
                if (qir_require_live(ctx, b, a->pos)) {
                    // Check for duplicate Qubit argument in the same call (Obligation 2).
                    for (size_t k = 0; k < qubit_arg_count; k++) {
                        int same_name = strcmp(qubit_arg_names[k], aname) == 0;
                        int same_static = b->has_static_id &&
                                          qubit_arg_bindings[k]->has_static_id &&
                                          b->static_id == qubit_arg_bindings[k]->static_id;
                        if (same_name || same_static) {
                            type_error(ctx->checker, a->pos,
                                "quantum operation requires distinct qubit arguments; '%s' passed as both argument %zu and argument %zu",
                                aname, qubit_arg_indices[k], arg_idx);
                            break;
                        }
                    }
                    if (qubit_arg_count < 16) {
                        qubit_arg_names[qubit_arg_count] = aname;
                        qubit_arg_bindings[qubit_arg_count] = b;
                        qubit_arg_indices[qubit_arg_count] = arg_idx;
                        qubit_arg_count++;
                    }
                    // If the user callee returns a Qubit, its Qubit arguments
                    // are moved into the callee (value-threading style).
                    if (!is_qir && callee_returns_qubit) {
                        qir_transition_binding(ctx, b, QIR_STATE_MOVED, a->pos);
                    }
                }
            } else {
                type_error(ctx->checker, a->pos,
                    "linear 'qir.Qubit' argument must be a bound variable identifier");
            }
        } else {
            qir_check_expr(ctx, a, 0);
        }
    }
}

static void qir_walk_stmt(QirWalkContext* ctx, ASTNode* stmt);

// Walk an expression. When `allow_qubit_value` is 0, any bare `qir.Qubit`
// identifier or composite literal containing a `qir.Qubit` in this expression
// context (e.g., arithmetic, slice/array/struct literal, address-of) is
// flagged.
static void qir_check_expr(QirWalkContext* ctx, ASTNode* expr, int allow_qubit_value) {
    if (!expr) return;

    switch (expr->type) {
        case AST_IDENTIFIER: {
            const char* name = ((IdentifierNode*)expr)->name;
            QirBinding* b = qir_find_binding(&ctx->table, name);
            if (b) {
                qir_require_live(ctx, b, expr->pos);
                if (!allow_qubit_value) {
                    type_error(ctx->checker, expr->pos,
                        "linear qubit '%s' cannot be copied or used in this expression",
                        name);
                }
            }
            return;
        }
        case AST_LITERAL:
            return;
        case AST_CALL_EXPR:
            qir_check_call_expr(ctx, expr, allow_qubit_value);
            return;
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            qir_check_expr(ctx, b->left, 0);
            qir_check_expr(ctx, b->right, 0);
            return;
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* u = (UnaryExprNode*)expr;
            if (u->operator == TOKEN_AND && u->operand &&
                type_is_qir_qubit(u->operand->node_type)) {
                type_error(ctx->checker, expr->pos,
                    "cannot take address of linear type 'qir.Qubit'");
                return;
            }
            qir_check_expr(ctx, u->operand, 0);
            return;
        }
        case AST_ADDR_OF: {
            AddrOfNode* ao = (AddrOfNode*)expr;
            if (ao->operand && type_is_qir_qubit(ao->operand->node_type)) {
                type_error(ctx->checker, expr->pos,
                    "cannot take address of linear type 'qir.Qubit'");
                return;
            }
            qir_check_expr(ctx, ao->operand, 0);
            return;
        }
        case AST_POSTFIX_EXPR:
            qir_check_expr(ctx, ((PostfixExprNode*)expr)->operand, 0);
            return;
        case AST_INDEX_EXPR: {
            IndexExprNode* idx = (IndexExprNode*)expr;
            qir_check_expr(ctx, idx->expr, 0);
            qir_check_expr(ctx, idx->index, 0);
            return;
        }
        case AST_SLICE_INDEX_EXPR: {
            SliceIndexExprNode* s = (SliceIndexExprNode*)expr;
            qir_check_expr(ctx, s->expr, 0);
            qir_check_expr(ctx, s->low, 0);
            qir_check_expr(ctx, s->high, 0);
            return;
        }
        case AST_SLICE_EXPR: {
            SliceLitNode* lit = (SliceLitNode*)expr;
            if (type_is_illegal_qubit_container(expr->node_type)) {
                type_error(ctx->checker, expr->pos,
                    "linear type 'qir.Qubit' cannot be stored in a struct, slice, array, map, channel, or pointer");
            }
            for (ASTNode* e = lit->elements; e; e = e->next) {
                if (type_is_qir_qubit(e->node_type)) {
                    type_error(ctx->checker, e->pos,
                        "linear type 'qir.Qubit' cannot be stored in a struct, slice, array, map, channel, or pointer");
                }
                qir_check_expr(ctx, e, 0);
            }
            return;
        }
        case AST_ARRAY_LITERAL: {
            ArrayLitNode* lit = (ArrayLitNode*)expr;
            if (type_is_illegal_qubit_container(expr->node_type)) {
                type_error(ctx->checker, expr->pos,
                    "linear type 'qir.Qubit' cannot be stored in a struct, slice, array, map, channel, or pointer");
            }
            for (ASTNode* e = lit->elements; e; e = e->next) {
                qir_check_expr(ctx, e, 0);
            }
            return;
        }
        case AST_PAREN_EXPR: {
            // MapLitNode uses AST_PAREN_EXPR.
            MapLitNode* m = (MapLitNode*)expr;
            if (type_is_illegal_qubit_container(expr->node_type)) {
                type_error(ctx->checker, expr->pos,
                    "linear type 'qir.Qubit' cannot be stored in a struct, slice, array, map, channel, or pointer");
            }
            for (ASTNode* k = m->keys; k; k = k->next) qir_check_expr(ctx, k, 0);
            for (ASTNode* v = m->values; v; v = v->next) qir_check_expr(ctx, v, 0);
            return;
        }
        case AST_STRUCT_LITERAL: {
            StructLiteralNode* sl = (StructLiteralNode*)expr;
            if (type_is_qir_qubit(expr->node_type)) {
                type_error(ctx->checker, expr->pos,
                    "linear type 'qir.Qubit' cannot be constructed via struct literal; use qir.Alloc or qir.QubitAt");
                return;
            }
            for (ASTNode* f = sl->field_values; f; f = f->next) {
                qir_check_expr(ctx, f, 0);
            }
            return;
        }
        case AST_KEYED_ELEMENT: {
            KeyedElementNode* ke = (KeyedElementNode*)expr;
            qir_check_expr(ctx, ke->key, 0);
            qir_check_expr(ctx, ke->value, 0);
            return;
        }
        case AST_SELECTOR_EXPR:
            qir_check_expr(ctx, ((SelectorExprNode*)expr)->expr, 0);
            return;
        case AST_TYPE_ASSERT:
            qir_check_expr(ctx, ((TypeAssertNode*)expr)->expr, 0);
            return;
        case AST_SLICE_CONVERSION:
            qir_check_expr(ctx, ((SliceConvNode*)expr)->operand, 0);
            return;
        case AST_TRY_EXPR:
            qir_check_expr(ctx, ((TryExprNode*)expr)->expr, 0);
            return;
        case AST_CATCH_EXPR: {
            CatchExprNode* ce = (CatchExprNode*)expr;
            qir_check_expr(ctx, ce->expr, 0);
            qir_walk_stmt(ctx, ce->catch_body);
            return;
        }
        case AST_FUNC_LIT: {
            // Obligation 6: closure may not capture any `qir.Qubit` binding.
            FuncLitNode* lit = (FuncLitNode*)expr;
            for (size_t i = 0; i < lit->captured_count; i++) {
                const char* cname = lit->captured_names[i];
                if (cname && qir_find_binding(&ctx->table, cname) != NULL) {
                    type_error(ctx->checker, expr->pos,
                        "closure may not capture linear qubit '%s'",
                        cname);
                }
            }
            // Also check the closure's own body in a fresh isolated context
            // in case it allocates its own local qubits.
            QirWalkContext lit_ctx;
            lit_ctx.checker = ctx->checker;
            lit_ctx.table.count = 0;
            lit_ctx.scope_depth = 0;
            lit_ctx.loop_depth = 0;
            lit_ctx.block_terminated = 0;
            qir_walk_stmt(&lit_ctx, lit->body);
            if (!lit_ctx.block_terminated) {
                qir_pop_scope_bindings(&lit_ctx, -1, 1);
            }
            return;
        }
        case AST_MATCH_EXPR: {
            MatchExprNode* me = (MatchExprNode*)expr;
            qir_check_expr(ctx, me->expr, 0);
            for (ASTNode* c = me->cases; c; c = c->next) {
                MatchCaseNode* mc = (MatchCaseNode*)c;
                if (mc->guard) qir_check_expr(ctx, mc->guard, 0);
                qir_walk_stmt(ctx, mc->body);
            }
            return;
        }
        default:
            return;
    }
}

// --- Statement Handlers ----------------------------------------------------

static void qir_handle_var_decl(QirWalkContext* ctx, VarDeclNode* vd) {
    Type* decl_type = vd->base.node_type;
    if (!decl_type && vd->type) {
        decl_type = vd->type->node_type;
    }
    if (!decl_type && vd->values) {
        decl_type = vd->values->node_type;
    }

    if (type_is_illegal_qubit_container(decl_type)) {
        type_error(ctx->checker, vd->base.pos,
            "linear type 'qir.Qubit' cannot be stored in a struct, slice, array, map, channel, or pointer");
        return;
    }

    // Multi-value call destructuring: `a, b := makeBell()`
    if (vd->name_count > 1 && vd->values && !vd->values->next &&
        vd->values->type == AST_CALL_EXPR &&
        type_returns_any_qubit(vd->values->node_type)) {
        qir_check_call_expr(ctx, vd->values, 1);
        Type* rt = vd->values->node_type;
        for (size_t i = 0; i < vd->name_count; i++) {
            const char* nm = (vd->names && vd->names[i]) ? vd->names[i] : NULL;
            Type* elem_t = (rt && rt->kind == TYPE_STRUCT && i < rt->data.struct_type.field_count)
                ? rt->data.struct_type.fields[i].type : NULL;
            if (nm && type_is_qir_qubit(elem_t)) {
                if (strcmp(nm, "_") == 0) {
                    type_error(ctx->checker, vd->base.pos,
                        "linear 'qir.Qubit' cannot be discarded via blank identifier '_'");
                } else {
                    qir_add_binding(ctx, nm, vd->base.pos, 0, 0, 0);
                }
            }
        }
        return;
    }

    if (type_is_qir_qubit(decl_type) ||
        (vd->values && type_is_qir_qubit(vd->values->node_type))) {
        for (size_t i = 0; i < vd->name_count; i++) {
            const char* nm = (vd->names && vd->names[i]) ? vd->names[i] : NULL;
            if (!nm) continue;
            if (strcmp(nm, "_") == 0) {
                type_error(ctx->checker, vd->base.pos,
                    "linear 'qir.Qubit' cannot be discarded via blank identifier '_'");
                if (vd->values) {
                    int dummy_has = 0;
                    uint64_t dummy_id = 0;
                    qir_consume_qubit_rhs(ctx, vd->values, &dummy_has, &dummy_id);
                }
                continue;
            }
            if (!vd->values) {
                type_error(ctx->checker, vd->base.pos,
                    "linear qubit '%s' cannot be declared without an initializer",
                    nm);
                continue;
            }
            int has_static = 0;
            uint64_t static_id = 0;
            qir_consume_qubit_rhs(ctx, vd->values, &has_static, &static_id);
            qir_add_binding(ctx, nm, vd->base.pos, 0, has_static, static_id);
        }
        return;
    }

    for (ASTNode* v = vd->values; v; v = v->next) {
        qir_check_expr(ctx, v, 0);
    }
}

static void qir_handle_expr_stmt(QirWalkContext* ctx, ASTNode* expr) {
    if (!expr) return;

    if (expr->type == AST_BINARY_EXPR) {
        BinaryExprNode* bin = (BinaryExprNode*)expr;
        if (bin->operator == TOKEN_ASSIGN) {
            // Check if LHS or RHS is a `qir.Qubit`.
            int lhs_is_blank = (bin->left && bin->left->type == AST_IDENTIFIER &&
                                strcmp(((IdentifierNode*)bin->left)->name, "_") == 0);
            QirBinding* lhs_b = (bin->left && bin->left->type == AST_IDENTIFIER)
                ? qir_find_binding(&ctx->table, ((IdentifierNode*)bin->left)->name)
                : NULL;
            int rhs_is_qubit = bin->right && type_is_qir_qubit(bin->right->node_type);
            if (!rhs_is_qubit && bin->right && bin->right->type == AST_IDENTIFIER) {
                if (qir_find_binding(&ctx->table, ((IdentifierNode*)bin->right)->name) != NULL) {
                    rhs_is_qubit = 1;
                }
            }

            if (lhs_is_blank && rhs_is_qubit) {
                type_error(ctx->checker, expr->pos,
                    "linear 'qir.Qubit' cannot be discarded via blank identifier '_'");
                int dummy_has = 0;
                uint64_t dummy_id = 0;
                qir_consume_qubit_rhs(ctx, bin->right, &dummy_has, &dummy_id);
                return;
            }

            if (lhs_b || rhs_is_qubit) {
                if (!lhs_b) {
                    type_error(ctx->checker, expr->pos,
                        "linear type 'qir.Qubit' cannot be stored in a struct, slice, array, map, channel, or interface");
                    int dummy_has = 0;
                    uint64_t dummy_id = 0;
                    qir_consume_qubit_rhs(ctx, bin->right, &dummy_has, &dummy_id);
                    return;
                }
                if (lhs_b->state == QIR_STATE_LIVE) {
                    type_error(ctx->checker, expr->pos,
                        "cannot overwrite live linear qubit '%s' (declared at line %d) without consuming it first",
                        lhs_b->name, lhs_b->decl_pos.line);
                    int dummy_has = 0;
                    uint64_t dummy_id = 0;
                    qir_consume_qubit_rhs(ctx, bin->right, &dummy_has, &dummy_id);
                    return;
                }
                // Rebinding a previously MOVED or CONSUMED qubit variable revives it!
                int has_static = 0;
                uint64_t static_id = 0;
                qir_consume_qubit_rhs(ctx, bin->right, &has_static, &static_id);
                lhs_b->state = QIR_STATE_LIVE;
                lhs_b->decl_pos = expr->pos;
                lhs_b->transition_pos = expr->pos;
                lhs_b->has_static_id = has_static;
                lhs_b->static_id = static_id;
                return;
            }

            qir_check_expr(ctx, bin->right, 0);
            if (bin->left && bin->left->type != AST_IDENTIFIER) {
                qir_check_expr(ctx, bin->left, 0);
            }
            return;
        }
    }

    qir_check_expr(ctx, expr, 0);
}

// Reconcile `branch_tbl` against `joined_tbl` (initialized from the first
// non-terminating branch). Emits Obligation 4 diagnostics on any mismatch for
// bindings that existed before the branching construct (`pre_count`).
static void qir_join_branch_tables(QirWalkContext* ctx,
                                   QirBindingTable* accum,
                                   int* has_accum,
                                   const QirBindingTable* branch_tbl,
                                   size_t pre_count,
                                   Position branch_pos) {
    if (!*has_accum) {
        *accum = *branch_tbl;
        accum->count = pre_count;
        *has_accum = 1;
        return;
    }
    for (size_t i = 0; i < pre_count; i++) {
        const QirBinding* a = &accum->items[i];
        const QirBinding* b = &branch_tbl->items[i];
        int a_live = (a->state == QIR_STATE_LIVE);
        int b_live = (b->state == QIR_STATE_LIVE);
        if (a_live != b_live) {
            Position consume_pos = a_live ? b->transition_pos : a->transition_pos;
            type_error(ctx->checker, branch_pos,
                "linear qubit '%s' (declared at line %d) is consumed on one branch (line %d) but remains live on another branch",
                a->name, a->decl_pos.line, consume_pos.line);
            // Mark CONSUMED in accum so scope exit does not emit a second
            // duplicate unconsumed diagnostic for the same variable.
            accum->items[i].state = QIR_STATE_CONSUMED;
            accum->items[i].transition_pos = consume_pos;
        }
    }
}

static void qir_walk_stmt(QirWalkContext* ctx, ASTNode* stmt) {
    if (!stmt || ctx->block_terminated) return;

    switch (stmt->type) {
        case AST_BLOCK_STMT: {
            BlockStmtNode* b = (BlockStmtNode*)stmt;
            int saved_depth = ctx->scope_depth;
            ctx->scope_depth++;
            for (ASTNode* s = b->statements; s; s = s->next) {
                qir_walk_stmt(ctx, s);
                if (ctx->block_terminated) break;
            }
            qir_pop_scope_bindings(ctx, saved_depth, !ctx->block_terminated);
            ctx->scope_depth = saved_depth;
            return;
        }
        case AST_VAR_DECL:
            qir_handle_var_decl(ctx, (VarDeclNode*)stmt);
            return;
        case AST_CONST_DECL: {
            ConstDeclNode* cd = (ConstDeclNode*)stmt;
            for (ASTNode* v = cd->values; v; v = v->next) {
                qir_check_expr(ctx, v, 0);
            }
            return;
        }
        case AST_EXPR_STMT: {
            ExprStmtNode* es = (ExprStmtNode*)stmt;
            qir_handle_expr_stmt(ctx, es->expr);
            if (es->expr && es->expr->type == AST_CALL_EXPR) {
                CallExprNode* call = (CallExprNode*)es->expr;
                if (call->function && call->function->type == AST_IDENTIFIER &&
                    ((IdentifierNode*)call->function)->name &&
                    strcmp(((IdentifierNode*)call->function)->name, "panic") == 0) {
                    ctx->block_terminated = 1;
                }
            }
            return;
        }
        case AST_MULTI_ASSIGN: {
            MultiAssignNode* ma = (MultiAssignNode*)stmt;
            // Handle parallel assignment/declaration (`a, b := qir.Alloc(), qir.Alloc()`
            // or `a, b = b, a`). First evaluate all RHS states.
            ASTNode* t = ma->targets;
            ASTNode* v = ma->values;
            for (; t && v; t = t->next, v = v->next) {
                int rhs_is_qubit = type_is_qir_qubit(v->node_type) ||
                    (v->type == AST_IDENTIFIER &&
                     qir_find_binding(&ctx->table, ((IdentifierNode*)v)->name) != NULL);
                if (rhs_is_qubit) {
                    int has_static = 0;
                    uint64_t static_id = 0;
                    qir_consume_qubit_rhs(ctx, v, &has_static, &static_id);
                    if (t->type == AST_IDENTIFIER) {
                        const char* tname = ((IdentifierNode*)t)->name;
                        if (strcmp(tname, "_") == 0) {
                            type_error(ctx->checker, stmt->pos,
                                "linear 'qir.Qubit' cannot be discarded via blank identifier '_'");
                        } else if (ma->is_short_decl) {
                            qir_add_binding(ctx, tname, stmt->pos, 0, has_static, static_id);
                        } else {
                            QirBinding* tb = qir_find_binding(&ctx->table, tname);
                            if (tb && tb->state == QIR_STATE_LIVE) {
                                type_error(ctx->checker, stmt->pos,
                                    "cannot overwrite live linear qubit '%s' (declared at line %d) without consuming it first",
                                    tb->name, tb->decl_pos.line);
                            } else if (tb) {
                                tb->state = QIR_STATE_LIVE;
                                tb->decl_pos = stmt->pos;
                                tb->has_static_id = has_static;
                                tb->static_id = static_id;
                            }
                        }
                    }
                } else {
                    qir_check_expr(ctx, v, 0);
                }
            }
            return;
        }
        case AST_RETURN_STMT: {
            ReturnStmtNode* ret = (ReturnStmtNode*)stmt;
            const char* returned_names[16];
            size_t returned_count = 0;
            for (ASTNode* v = ret->values; v; v = v->next) {
                int is_q = type_is_qir_qubit(v->node_type) ||
                    (v->type == AST_IDENTIFIER &&
                     qir_find_binding(&ctx->table, ((IdentifierNode*)v)->name) != NULL);
                if (is_q) {
                    if (v->type == AST_IDENTIFIER) {
                        const char* rname = ((IdentifierNode*)v)->name;
                        for (size_t k = 0; k < returned_count; k++) {
                            if (strcmp(returned_names[k], rname) == 0) {
                                type_error(ctx->checker, v->pos,
                                    "cannot return linear qubit '%s' multiple times (no-cloning)",
                                    rname);
                                break;
                            }
                        }
                        if (returned_count < 16) returned_names[returned_count++] = rname;
                    }
                    int dummy_has = 0;
                    uint64_t dummy_id = 0;
                    qir_consume_qubit_rhs(ctx, v, &dummy_has, &dummy_id);
                } else {
                    qir_check_expr(ctx, v, 0);
                }
            }
            // Obligation 3: every non-borrowed Qubit binding in the function
            // must be consumed or moved before `return`.
            for (size_t i = 0; i < ctx->table.count; i++) {
                QirBinding* b = &ctx->table.items[i];
                if (!b->is_borrowed_param && b->state == QIR_STATE_LIVE) {
                    type_error(ctx->checker, stmt->pos,
                        "linear qubit '%s' (declared at line %d) exited scope without being consumed by qir.FreeZero, qir.MeasureReset, or returned",
                        b->name, b->decl_pos.line);
                }
            }
            ctx->block_terminated = 1;
            return;
        }
        case AST_IF_STMT:
        case AST_IF_LET_STMT: {
            ASTNode* cond = (stmt->type == AST_IF_STMT)
                ? ((IfStmtNode*)stmt)->condition
                : ((IfLetStmtNode*)stmt)->nullable_expr;
            ASTNode* then_s = (stmt->type == AST_IF_STMT)
                ? ((IfStmtNode*)stmt)->then_stmt
                : ((IfLetStmtNode*)stmt)->then_stmt;
            ASTNode* else_s = (stmt->type == AST_IF_STMT)
                ? ((IfStmtNode*)stmt)->else_stmt
                : ((IfLetStmtNode*)stmt)->else_stmt;

            qir_check_expr(ctx, cond, 0);

            QirBindingTable pre_tbl = ctx->table;
            size_t pre_count = pre_tbl.count;

            QirWalkContext then_ctx = *ctx;
            then_ctx.block_terminated = 0;
            qir_walk_stmt(&then_ctx, then_s);
            qir_pop_scope_bindings(&then_ctx, ctx->scope_depth, !then_ctx.block_terminated);

            QirWalkContext else_ctx = *ctx;
            else_ctx.table = pre_tbl;
            else_ctx.block_terminated = 0;
            if (else_s) {
                qir_walk_stmt(&else_ctx, else_s);
                qir_pop_scope_bindings(&else_ctx, ctx->scope_depth, !else_ctx.block_terminated);
            }

            QirBindingTable accum = pre_tbl;
            int has_accum = 0;
            if (!then_ctx.block_terminated) {
                qir_join_branch_tables(ctx, &accum, &has_accum, &then_ctx.table, pre_count, stmt->pos);
            }
            if (!else_ctx.block_terminated) {
                qir_join_branch_tables(ctx, &accum, &has_accum, &else_ctx.table, pre_count, stmt->pos);
            }

            if (!has_accum) {
                ctx->block_terminated = 1;
            } else {
                ctx->table = accum;
            }
            return;
        }
        case AST_SWITCH_STMT:
        case AST_TYPE_SWITCH: {
            ASTNode* tag = (stmt->type == AST_SWITCH_STMT)
                ? ((SwitchStmtNode*)stmt)->tag
                : ((TypeSwitchNode*)stmt)->expr;
            ASTNode* cases = (stmt->type == AST_SWITCH_STMT)
                ? ((SwitchStmtNode*)stmt)->cases
                : ((TypeSwitchNode*)stmt)->cases;
            qir_check_expr(ctx, tag, 0);

            QirBindingTable pre_tbl = ctx->table;
            size_t pre_count = pre_tbl.count;
            QirBindingTable accum = pre_tbl;
            int has_accum = 0;
            int has_default = 0;

            for (ASTNode* c = cases; c; c = c->next) {
                ASTNode* body = NULL;
                if (stmt->type == AST_SWITCH_STMT) {
                    CaseClauseNode* cc = (CaseClauseNode*)c;
                    if (!cc->exprs) has_default = 1;
                    for (ASTNode* e = cc->exprs; e; e = e->next) qir_check_expr(ctx, e, 0);
                    body = cc->body;
                } else {
                    TypeCaseNode* tc = (TypeCaseNode*)c;
                    if (!tc->types) has_default = 1;
                    body = tc->body;
                }

                QirWalkContext case_ctx = *ctx;
                case_ctx.table = pre_tbl;
                case_ctx.scope_depth = ctx->scope_depth + 1;
                case_ctx.block_terminated = 0;
                for (ASTNode* s = body; s; s = s->next) {
                    qir_walk_stmt(&case_ctx, s);
                    if (case_ctx.block_terminated) break;
                }
                qir_pop_scope_bindings(&case_ctx, ctx->scope_depth, !case_ctx.block_terminated);
                if (!case_ctx.block_terminated) {
                    qir_join_branch_tables(ctx, &accum, &has_accum, &case_ctx.table, pre_count, c->pos);
                }
            }
            if (!has_default) {
                qir_join_branch_tables(ctx, &accum, &has_accum, &pre_tbl, pre_count, stmt->pos);
            }
            if (!has_accum) {
                ctx->block_terminated = 1;
            } else {
                ctx->table = accum;
            }
            return;
        }
        case AST_FOR_STMT: {
            ForStmtNode* f = (ForStmtNode*)stmt;
            int saved_scope = ctx->scope_depth;
            int saved_loop = ctx->loop_depth;
            ctx->scope_depth++;
            qir_walk_stmt(ctx, f->init);
            qir_check_expr(ctx, f->condition, 0);
            qir_check_expr(ctx, f->range_expr, 0);
            ctx->loop_depth++;
            qir_walk_stmt(ctx, f->body);
            qir_walk_stmt(ctx, f->post);
            ctx->loop_depth = saved_loop;
            ctx->block_terminated = 0;
            qir_pop_scope_bindings(ctx, saved_scope, 1);
            ctx->scope_depth = saved_scope;
            return;
        }
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
        case AST_BREAK_LABEL_STMT:
        case AST_CONTINUE_LABEL_STMT: {
            // Obligation 5: any qubit allocated inside the current loop body
            // must be consumed before breaking or continuing.
            for (size_t i = 0; i < ctx->table.count; i++) {
                QirBinding* b = &ctx->table.items[i];
                if (b->loop_depth >= ctx->loop_depth && ctx->loop_depth > 0 &&
                    !b->is_borrowed_param && b->state == QIR_STATE_LIVE) {
                    type_error(ctx->checker, stmt->pos,
                        "linear qubit '%s' (declared at line %d) exited scope without being consumed by qir.FreeZero, qir.MeasureReset, or returned",
                        b->name, b->decl_pos.line);
                }
            }
            ctx->block_terminated = 1;
            return;
        }
        case AST_GO_STMT: {
            GoStmtNode* gs = (GoStmtNode*)stmt;
            if (gs->call && gs->call->type == AST_CALL_EXPR) {
                CallExprNode* c = (CallExprNode*)gs->call;
                for (ASTNode* a = c->args; a; a = a->next) {
                    if (type_is_qir_qubit(a->node_type) ||
                        (a->type == AST_IDENTIFIER &&
                         qir_find_binding(&ctx->table, ((IdentifierNode*)a)->name) != NULL)) {
                        const char* nm = (a->type == AST_IDENTIFIER)
                            ? ((IdentifierNode*)a)->name : "<qubit>";
                        type_error(ctx->checker, stmt->pos,
                            "linear qubit '%s' may not be passed to a goroutine", nm);
                    } else {
                        qir_check_expr(ctx, a, 0);
                    }
                }
                if (c->function && c->function->type == AST_FUNC_LIT) {
                    qir_check_expr(ctx, c->function, 0);
                }
            }
            return;
        }
        case AST_DEFER_STMT: {
            DeferStmtNode* ds = (DeferStmtNode*)stmt;
            if (ds->call && ds->call->type == AST_CALL_EXPR) {
                CallExprNode* c = (CallExprNode*)ds->call;
                for (ASTNode* a = c->args; a; a = a->next) {
                    if (type_is_qir_qubit(a->node_type) ||
                        (a->type == AST_IDENTIFIER &&
                         qir_find_binding(&ctx->table, ((IdentifierNode*)a)->name) != NULL)) {
                        const char* nm = (a->type == AST_IDENTIFIER)
                            ? ((IdentifierNode*)a)->name : "<qubit>";
                        type_error(ctx->checker, stmt->pos,
                            "linear qubit '%s' may not be passed to defer; consume it explicitly", nm);
                    } else {
                        qir_check_expr(ctx, a, 0);
                    }
                }
            }
            return;
        }
        case AST_ARENA_BLOCK:
            qir_walk_stmt(ctx, ((ArenaBlockNode*)stmt)->body);
            return;
        case AST_UNSAFE_STMT:
            qir_walk_stmt(ctx, ((UnsafeStmtNode*)stmt)->body);
            return;
        case AST_COMPTIME_BLOCK:
            return;
        case AST_LABEL_STMT:
            qir_walk_stmt(ctx, ((LabelStmtNode*)stmt)->stmt);
            return;
        case AST_SELECT_STMT: {
            SelectStmtNode* se = (SelectStmtNode*)stmt;
            for (ASTNode* c = se->cases; c; c = c->next) {
                SelectCaseNode* sc = (SelectCaseNode*)c;
                qir_walk_stmt(ctx, sc->comm);
                for (ASTNode* s = sc->body; s; s = s->next) {
                    qir_walk_stmt(ctx, s);
                }
            }
            return;
        }
        case AST_GOTO_STMT:
        case AST_FALLTHROUGH_STMT:
            return;
        default:
            return;
    }
}

static int program_imports_qir(ASTNode* program) {
    if (!program || program->type != AST_PROGRAM) return 0;
    ProgramNode* prog = (ProgramNode*)program;
    for (ASTNode* imp = prog->imports; imp; imp = imp->next) {
        if (imp->type != AST_IMPORT_SPEC) continue;
        ImportSpecNode* spec = (ImportSpecNode*)imp;
        if (spec->path && strcmp(spec->path, "qir") == 0) return 1;
    }
    return 0;
}

int qir_ownership_check_program(TypeChecker* checker, ASTNode* program) {
    if (!checker || !program || program->type != AST_PROGRAM) return 0;

    // Fast path: if `"qir"` is not imported in this file and never seeded in
    // the checker, do zero extra work.
    if (!program_imports_qir(program) && !type_checker_find_package(checker, "qir")) {
        return 1;
    }

    int errors_before = checker->error_count;
    ProgramNode* prog = (ProgramNode*)program;

    for (ASTNode* decl = prog->decls; decl; decl = decl->next) {
        if (decl->type == AST_VAR_DECL) {
            VarDeclNode* vd = (VarDeclNode*)decl;
            Type* t = vd->base.node_type;
            if (!t && vd->type) t = type_from_ast(checker, vd->type);
            if (!t && vd->values) t = vd->values->node_type;
            if (type_is_qir_qubit(t) || type_is_illegal_qubit_container(t)) {
                type_error(checker, decl->pos,
                    "linear type 'qir.Qubit' cannot be stored in a global variable");
            }
            continue;
        }
        if (decl->type == AST_TYPE_DECL) {
            TypeDeclNode* td = (TypeDeclNode*)decl;
            Variable* tv = td->name ? type_checker_lookup_variable(checker, td->name) : NULL;
            if (tv && tv->type && type_is_illegal_qubit_container(tv->type)) {
                type_error(checker, decl->pos,
                    "linear type 'qir.Qubit' cannot be stored in a struct, slice, array, map, channel, or pointer");
            }
            continue;
        }
        if (decl->type != AST_FUNC_DECL) continue;

        FuncDeclNode* func = (FuncDeclNode*)decl;
        QirWalkContext ctx;
        ctx.checker = checker;
        ctx.table.count = 0;
        ctx.scope_depth = 0;
        ctx.loop_depth = 0;
        ctx.block_terminated = 0;

        Type* ret_type = func->return_type ? type_from_ast(checker, func->return_type) : NULL;
        if (type_is_illegal_qubit_container(ret_type)) {
            type_error(checker, func->base.pos,
                "linear type 'qir.Qubit' cannot be stored in a struct, slice, array, map, channel, or pointer");
        }
        int fn_returns_qubit = type_returns_any_qubit(ret_type);

        // Seed `qir.Qubit` parameters into scope depth 0.
        for (ASTNode* p = func->params; p; p = p->next) {
            if (p->type != AST_VAR_DECL) continue;
            VarDeclNode* pvd = (VarDeclNode*)p;
            Type* pt = pvd->type ? type_from_ast(checker, pvd->type) : pvd->base.node_type;
            if (type_is_illegal_qubit_container(pt)) {
                type_error(checker, p->pos,
                    "linear type 'qir.Qubit' cannot be stored in a struct, slice, array, map, channel, or pointer");
                continue;
            }
            if (type_is_qir_qubit(pt)) {
                for (size_t i = 0; i < pvd->name_count; i++) {
                    if (pvd->names && pvd->names[i]) {
                        qir_add_binding(&ctx, pvd->names[i], p->pos,
                                        !fn_returns_qubit, 0, 0);
                    }
                }
            }
        }

        // Walk the function body's statements at scope_depth 0 so top-level
        // locals and owned parameters share the function-exit check.
        if (func->body && func->body->type == AST_BLOCK_STMT) {
            BlockStmtNode* body = (BlockStmtNode*)func->body;
            for (ASTNode* s = body->statements; s; s = s->next) {
                qir_walk_stmt(&ctx, s);
                if (ctx.block_terminated) break;
            }
            if (!ctx.block_terminated) {
                qir_pop_scope_bindings(&ctx, -1, 1);
            }
        }
    }

    return checker->error_count == errors_before;
}
