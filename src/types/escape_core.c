// The taint-propagation engine shared by param_escape, block_escape and
// local_escape. See include/escape_core.h for the contract, the soundness
// invariant, and the two drift defects that motivated the extraction.
//
// PROVENANCE: every function below was lifted from src/types/block_escape.c,
// which held the most complete copy (it is the only pass with expression-level
// sources). The bodies are byte-faithful to it apart from the four hook sites,
// each of which carries its own comment. param_escape.c and local_escape.c
// each held a near-identical copy; the differences were measured before the
// move, and all of them are now hooks:
//
//   walk_stmt         200 lines. param differed by ONE line (the return
//                     signal); local differed only by which bind function it
//                     called, at five sites.
//   expr_taint        param and local differed from block ONLY by block's
//                     three source arms. They have no expression sources.
//   call_taint        differed only by where the callee summary comes from.
//   handle_go_call    byte-identical in all three.
//   assign_to_lvalue  differed only by the slot-count field name.
//   the taint set and LocalEnv helpers were byte-identical in all three.

#include "escape_core.h"
#include "nonretaining.h"
#include <stdlib.h>
#include <string.h>

// =============================================================================
// TaintSet
// =============================================================================

TaintSet escape_taint_new(size_t n) {
    TaintSet t;
    t.n = n;
    t.bits = n ? calloc(n, sizeof(bool)) : NULL;
    return t;
}

void escape_taint_free(TaintSet* t) {
    if (!t) return;
    free(t->bits);
    t->bits = NULL;
    t->n = 0;
}

bool escape_taint_empty(const TaintSet* t) {
    for (size_t i = 0; i < t->n; i++) {
        if (t->bits[i]) return false;
    }
    return true;
}

bool escape_taint_union_into(TaintSet* dst, const TaintSet* src) {
    bool changed = false;
    size_t n = dst->n < src->n ? dst->n : src->n;
    for (size_t i = 0; i < n; i++) {
        if (src->bits[i] && !dst->bits[i]) {
            dst->bits[i] = true;
            changed = true;
        }
    }
    return changed;
}

TaintSet escape_taint_copy(const TaintSet* src) {
    TaintSet t = escape_taint_new(src->n);
    if (t.bits && src->bits) memcpy(t.bits, src->bits, src->n * sizeof(bool));
    return t;
}

TaintSet escape_taint_all(size_t n) {
    TaintSet t = escape_taint_new(n);
    for (size_t i = 0; i < n; i++) t.bits[i] = true;
    return t;
}

// =============================================================================
// LocalEnv
// =============================================================================

void escape_env_free(LocalEnv* env) {
    for (size_t i = 0; i < env->count; i++) {
        free(env->vars[i].name);
        escape_taint_free(&env->vars[i].taint);
    }
    free(env->vars);
    env->vars = NULL;
    env->count = env->capacity = 0;
}

LocalVar* escape_env_find(LocalEnv* env, const char* name) {
    for (size_t i = 0; i < env->count; i++) {
        if (strcmp(env->vars[i].name, name) == 0) return &env->vars[i];
    }
    return NULL;
}

bool escape_env_add_or_union(LocalEnv* env, const char* name, const TaintSet* value) {
    LocalVar* lv = escape_env_find(env, name);
    if (lv) {
        return escape_taint_union_into(&lv->taint, value);
    }
    if (env->count >= env->capacity) {
        size_t new_cap = env->capacity ? env->capacity * 2 : 8;
        LocalVar* grown = realloc(env->vars, new_cap * sizeof(LocalVar));
        if (!grown) return false;
        env->vars = grown;
        env->capacity = new_cap;
    }
    env->vars[env->count].name = strdup(name);
    env->vars[env->count].taint = escape_taint_copy(value);
    env->count++;
    return true;
}

// =============================================================================
// Accumulator
// =============================================================================

// An empty reason set is raised to UNCLASSIFIED rather than dropped. A mark
// that records no reason would leave the slot reading "does not escape", and a
// release consumer frees on that answer. Losing precision is the safe failure;
// losing the mark is the one that dangles a pointer.
static inline EscapeReasons reason_or_unclassified(EscapeReasons why) {
    return why ? why : ESCAPE_REASON_UNCLASSIFIED;
}

// THE ONLY NAME TABLE FOR EscapeReasons. Both readers -- local_escape_test's
// failure output and codegen's GOO_ARC_DEBUG line -- come through here, so a
// reason added to the header without a name here is caught by that test's
// ESCAPE_REASON_ALL case rather than by a reader silently printing a gap.
//
// Order is BIT ORDER, not alphabetical, so a printed set reads the same way
// twice and can be diffed between two runs.
const char* escape_reason_names(EscapeReasons why, char* buf, size_t n) {
    static const struct { EscapeReasons bit; const char* name; } NAMES[] = {
        { ESCAPE_REASON_UNCLASSIFIED,    "UNCLASSIFIED"    },
        { ESCAPE_REASON_RETURN,          "RETURN"          },
        { ESCAPE_REASON_GLOBAL_STORE,    "GLOBAL_STORE"    },
        { ESCAPE_REASON_CONTAINER_STORE, "CONTAINER_STORE" },
        { ESCAPE_REASON_SUBSCRIPT_STORE, "SUBSCRIPT_STORE" },
        { ESCAPE_REASON_CALL_RETAIN,     "CALL_RETAIN"     },
        { ESCAPE_REASON_CALLEE_VALUE,    "CALLEE_VALUE"    },
        { ESCAPE_REASON_GO_ARG,          "GO_ARG"          },
        { ESCAPE_REASON_DEFER_ARG,       "DEFER_ARG"       },
        { ESCAPE_REASON_CHAN_SEND,       "CHAN_SEND"       },
        { ESCAPE_REASON_CLOSURE_CAPTURE, "CLOSURE_CAPTURE" },
        { ESCAPE_REASON_CALL_OPAQUE,     "CALL_OPAQUE"     },
        { ESCAPE_REASON_CALL_VARIADIC,   "CALL_VARIADIC"   },
    };
    if (!buf || n == 0) return "";
    buf[0] = '\0';

    size_t used = 0;
    const char* sep = "";
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (!(why & NAMES[i].bit)) continue;
        // Truncate rather than overrun: stop the moment the next name plus its
        // separator would not fit, leaving what is already written intact.
        size_t need = strlen(sep) + strlen(NAMES[i].name);
        if (used + need + 1 > n) break;
        memcpy(buf + used, sep, strlen(sep));
        used += strlen(sep);
        memcpy(buf + used, NAMES[i].name, strlen(NAMES[i].name));
        used += strlen(NAMES[i].name);
        buf[used] = '\0';
        sep = "|";
    }
    if (used == 0) {
        // NONE and "the buffer was too small for even the first name" are not
        // the same thing, but both leave nothing to print. NONE is the honest
        // answer for a zero set, and a set this narrow cannot arise from a
        // caller that sized its buffer with ESCAPE_REASON_NAMES_MAX.
        const char* none = (why == ESCAPE_REASON_NONE) ? "NONE" : "?";
        size_t len = strlen(none);
        if (len + 1 <= n) memcpy(buf, none, len + 1);
    }
    return buf;
}

void escape_mark(EscapeCtx* ctx, const TaintSet* t, EscapeReasons why) {
    why = reason_or_unclassified(why);
    size_t n = t->n < ctx->slot_count ? t->n : ctx->slot_count;
    for (size_t i = 0; i < n; i++) {
        if (t->bits[i]) ctx->reasons[i] |= why;
    }
}

void escape_mark_all(EscapeCtx* ctx, EscapeReasons why) {
    why = reason_or_unclassified(why);
    for (size_t i = 0; i < ctx->slot_count; i++) ctx->reasons[i] |= why;
}

// Slot of an expression that is ITSELF a source, or ESCAPE_NO_SLOT.
//
// Only block_escape installs this hook: its sources are allocation sites,
// which are expressions. A parameter and a local are bound to NAMES, so
// param_escape and local_escape leave it NULL and every arm below falls
// through to the ordinary recursion — which is exactly what their hand-written
// copies did.
static size_t source_slot(EscapeCtx* ctx, ASTNode* expr) {
    size_t slot = ESCAPE_NO_SLOT;
    if (ctx->hooks->expr_source_slot
        && ctx->hooks->expr_source_slot(ctx, expr, &slot)) {
        return slot;
    }
    return ESCAPE_NO_SLOT;
}

static bool is_assign_op(TokenType op);
static void assign_to_lvalue(EscapeCtx* ctx, ASTNode* lhs, const TaintSet* rhs_taint,
                             bool* env_changed);
static TaintSet call_taint(EscapeCtx* ctx, CallExprNode* call);
static void handle_go_call(EscapeCtx* ctx, ASTNode* call_node, EscapeReasons why);
static void handle_defer_call(EscapeCtx* ctx, ASTNode* call_node);

static bool is_assign_op(TokenType op) {
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

// Mark every SUBSCRIPT inside an assignment target. See sink #2b below for
// why. Recursive so that `m[a][b] = v` marks both `a` and `b`, and so that a
// subscript buried under a field selector (`obj.field[k] = v`) is still seen.
//
// Deliberately marks only the INDEX, never the base. `m[k] = v` stores into m,
// which says nothing about whether m itself outlives the boundary — that is
// decided by how m is used elsewhere. Marking the base would over-mark every
// container assignment and cost most of the analysis's precision.
static void mark_lvalue_subscripts(EscapeCtx* ctx, ASTNode* lhs) {
    if (!lhs) return;
    switch (lhs->type) {
        case AST_INDEX_EXPR: {
            IndexExprNode* ie = (IndexExprNode*)lhs;
            TaintSet idx = escape_expr_taint(ctx, ie->index);
            escape_mark(ctx, &idx, ESCAPE_REASON_SUBSCRIPT_STORE);
            escape_taint_free(&idx);
            mark_lvalue_subscripts(ctx, ie->expr);
            break;
        }
        case AST_SELECTOR_EXPR:
            mark_lvalue_subscripts(ctx, ((SelectorExprNode*)lhs)->expr);
            break;
        case AST_UNARY_EXPR:
            mark_lvalue_subscripts(ctx, ((UnaryExprNode*)lhs)->operand);
            break;
        default:
            break;
    }
}

// THE SELF-STORE RULE — is `lhs` an index of a plain local, and which one?
//
// `m[k] = m[k] + 1` stores m's OWN contents back into m. That says nothing
// about whether m outlives the boundary, but the store sink below marked it
// anyway: the right side carries m's bit out of the AST_INDEX_EXPR arm, and a
// non-identifier lvalue marks the whole of rhs_taint.
//
// Measured at bench/daemon/daemon.goo:31 — this ONE shape was why the daemon's
// `counts` map refused to release. Narrowed first: a plain write, a parameter
// key, a write in a loop and a write with an import ALL released before this
// rule existed. Only the compound update refused.
//
// Worth 640,000 of 2,209,982 bytes per 2,000 requests (29.0%), MEASURED — not
// the 822,000 .handoff.md projected. The map's record is 902,000, but 262,000
// of it is KEY storage, and goo_map_set_sv stores keys verbatim so the map
// owns none of it. goo_map_dtor correctly leaves keys alone, so they change
// leak category (indirect -> definite) instead of going away. See
// docs/adr/0002-measurements/t4_self_store_findings.md.
//
// Returns the base's LocalVar so the caller can subtract its taint, or NULL
// when the rule does not apply. NULL for a non-index lvalue, for a compound
// base (`obj.field[k]`), and for a base that is not a plain local — a package
// global is absent from the environment, and marking stays conservative there.
static LocalVar* self_store_base(EscapeCtx* ctx, ASTNode* lhs) {
    if (!lhs || lhs->type != AST_INDEX_EXPR) return NULL;
    ASTNode* base = ((IndexExprNode*)lhs)->expr;
    if (!base || base->type != AST_IDENTIFIER) return NULL;
    return escape_env_find(ctx->env, ((IdentifierNode*)base)->name);
}

static void assign_to_lvalue(EscapeCtx* ctx, ASTNode* lhs, const TaintSet* rhs_taint, bool* env_changed) {
    if (!lhs) {
        escape_mark(ctx, rhs_taint, ESCAPE_REASON_UNCLASSIFIED);
        return;
    }
    if (lhs->type == AST_IDENTIFIER) {
        const char* name = ((IdentifierNode*)lhs)->name;
        if (strcmp(name, "_") == 0) {
            return;
        }
        LocalVar* lv = escape_env_find(ctx->env, name);
        if (lv) {
            if (escape_taint_union_into(&lv->taint, rhs_taint)) *env_changed = true;
            return;
        }
        escape_mark(ctx, rhs_taint, ESCAPE_REASON_GLOBAL_STORE);
        return;
    }
    // WHY SUBTRACTING THE BASE'S WHOLE TAINT IS SOUND, and not only its own
    // bit: every bit in the base's taint is marked WHENEVER the base is
    // marked, because marking the base marks that same set. So a bit removed
    // here stays attached to the base's fate through every other sink. Local
    // row 32 pins exactly that, with `g = m` after a self-store.
    //
    // It also needs no seventh EscapeHooks member. A "self-bit" is per-pass —
    // local_escape finds one by name, block_escape matches an alloc site — but
    // this function is shared by all three, and the base's taint is a thing
    // every pass already has.
    //
    // `escapes[]` only ever goes true (see escape_core.h), so a subtraction can
    // never RETRACT a mark an earlier fixpoint iterate made. That makes the
    // rule safe against iteration order, and it also means the rule must hold
    // on the FIRST walk to give anything back.
    LocalVar* base_lv = self_store_base(ctx, lhs);
    TaintSet reduced = { 0 };
    bool use_reduced = false;
    if (base_lv) {
        reduced = escape_taint_copy(rhs_taint);
        // Fail CLOSED. A failed copy leaves bits NULL with n non-zero, and
        // marking that set would mark NOTHING — under-marking is the one bug
        // class that dangles a pointer, so fall back to the unreduced set.
        use_reduced = (reduced.n == 0) || (reduced.bits != NULL);
        if (use_reduced) {
            size_t n = reduced.n < base_lv->taint.n ? reduced.n : base_lv->taint.n;
            if (!base_lv->taint.bits) n = 0;
            for (size_t i = 0; i < n; i++) {
                if (base_lv->taint.bits[i]) reduced.bits[i] = false;
            }
        }
    }
    escape_mark(ctx, use_reduced ? &reduced : rhs_taint, ESCAPE_REASON_CONTAINER_STORE);
    escape_taint_free(&reduced);

    // Sink #2b: a SUBSCRIPT of the target is a stored reference too.
    //
    // `m[k] = v` stores BOTH v and k. goo_map_set_sv keeps the key pointer
    // verbatim and never frees it (src/runtime/runtime.c), so a map that
    // outlives the boundary holds k for as long as it lives. Marking only
    // rhs_taint left k unmarked, which is UNDER-marking — the one bug class
    // that can dangle a pointer.
    //
    // Measured before this arm existed: in `m[k] = 1; return m` the map
    // escaped and k did not. Pinned by local-escape row 15. The slice
    // equivalent was already sound (row 16), because `append(parts, k)` is an
    // ordinary call and the call sink covers it — which is why this hole
    // survived: it exists only in the one position that is not a call.
    mark_lvalue_subscripts(ctx, lhs);
}


// True when the callee is `base.Sel` and `base` is a LOCAL, not a package.
//
// goo_callee_is_non_retaining answers by package name and selector, so a local
// variable that happens to be named `strings` would otherwise collect the
// `strings` package's whitelist and `s.Split(x)` would be treated as not
// retaining x. Membership in ctx->env is the engine's existing definition of
// "a plain local of this unit", so it is exactly the discriminator needed.
//
// Conservative on anything unexpected: a non-identifier base (`pkgs[0].F()`)
// answers true, which only ever DISABLES the whitelist. A plain-identifier
// callee is not a selector at all, so builtins are unaffected.
static bool selector_base_is_local(EscapeCtx* ctx, ASTNode* fn) {
    if (!fn || fn->type != AST_SELECTOR_EXPR) return false;
    ASTNode* base = ((SelectorExprNode*)fn)->expr;
    if (!base || base->type != AST_IDENTIFIER) return true;
    return escape_env_find(ctx->env, ((IdentifierNode*)base)->name) != NULL;
}

static TaintSet call_taint(EscapeCtx* ctx, CallExprNode* call) {
    size_t n = ctx->slot_count;

    size_t argc = 0;
    for (ASTNode* a = call->args; a; a = a->next) argc++;

    TaintSet* arg_taints = NULL;
    if (argc > 0) arg_taints = calloc(argc, sizeof(TaintSet));
    size_t i = 0;
    for (ASTNode* a = call->args; a; a = a->next, i++) {
        arg_taints[i] = escape_expr_taint(ctx, a);
    }

    const char* callee_name = NULL;
    // KEPT ALIVE TO THE RESULT CONSTRUCTION BELOW. An identifier callee leaves
    // this empty, so the union there is a no-op for an ordinary function call.
    TaintSet callee_taint = escape_taint_new(n);
    if (call->function && call->function->type == AST_IDENTIFIER) {
        callee_name = ((IdentifierNode*)call->function)->name;
    } else {
        // The CALLEE's taint escapes, exactly as in handle_go_call above.
        //
        // This used to compute the taint and DISCARD it, which was the same
        // under-marking hole handle_go_call closed for `go p.m()`, left open
        // on the ORDINARY call path: `p.m()` inside an arena block leaves `p`
        // arena-eligible, because a method's receiver is NOT a member of
        // call->args, so the retain-all rule for an unresolved callee below
        // never reaches it. A method that stores its receiver then dangles
        // once the block frees the arena. Verified at the IR level before the
        // fix (`&T{}` emitted goo_arena_alloc, valgrind: invalid read).
        //
        // Only the non-identifier shape is marked. An IDENTIFIER callee keeps
        // the old behaviour on purpose: calling a closure through a local
        // (`c()`) reads its captured cells but cannot leak the environment
        // itself, which is what row 29 pins.
        //
        // param_escape.c carries the identical fix — soundness sibling.
        escape_taint_free(&callee_taint);   // discard the empty set before rebinding
        callee_taint = escape_expr_taint(ctx, call->function);
        escape_mark(ctx, &callee_taint, ESCAPE_REASON_CALLEE_VALUE);
    }
    // Two passes answer this from different tables — param_escape from its own
    // in-progress Registry (it is COMPUTING these summaries and must see the
    // current iterate), the other two from a finished ParamEscapeResult. Same
    // (count, escapes[]) shape either way, so the hook returns that pair.
    const bool* callee_escapes = NULL;
    size_t callee_count = 0;
    bool callee_return_escapes = false;
    bool callee = callee_name && ctx->hooks->callee_retention
                  && ctx->hooks->callee_retention(ctx, callee_name,
                                                  &callee_escapes, &callee_count,
                                                  &callee_return_escapes);
    // 7a' non-retaining whitelist: only for calls that do NOT resolve to a user
    // function (callee == NULL) — a user body, even one shadowing a builtin
    // name, is analysed by its real summary above.
    bool whitelisted = !callee
                       && !selector_base_is_local(ctx, call->function)
                       && goo_callee_is_non_retaining(call->function);

    // `append` DOES NOT RETAIN ITS SLICE ARGUMENT, and until this arm it was
    // treated as an unregistered callee, so arg 0 was marked escaping and every
    // appended slice became unreleasable. That is why the daemon's `parts` stayed
    // refused while `fields` (from strings.Split) did not.
    //
    // The truth: append reads arg 0's buffer and either writes into it or
    // reallocs, and it stores arg 0 NOWHERE that outlives the call. What it does
    // do is RETURN a value that may alias arg 0's buffer -- and the result taint
    // below already unions every argument's taint on the no-callee path, so if
    // that result escapes, arg 0 is marked at THAT sink instead. The information
    // is not lost, only moved to where it is true.
    //
    // The ELEMENTS (args 1..n) keep retains = true. The buffer keeps a copy of
    // each, so a pointer element genuinely outlives the call inside the slice.
    //
    // Guarded the same way the whitelist is: a user function named `append`, or a
    // local shadowing the name, must not collect this rule.
    bool is_append = !callee && !whitelisted && callee_name
                     && strcmp(callee_name, "append") == 0
                     && !selector_base_is_local(ctx, call->function);

    // EACH ARM NAMES WHY IT MARKS, and the three answers are not the same fact.
    // CALL_RETAIN means param_escape READ the callee and measured retention.
    // CALL_OPAQUE means nobody looked. CALL_VARIADIC means a summary exists and
    // is silent about this position. Only the last two can be removed by making
    // the analysis better, and one bit for all three said nothing about which a
    // reader was facing -- ADR 0005's own argument, applied a second time.
    // Measured before the split: 578 / 86 / 12 locals respectively.
    for (i = 0; i < argc; i++) {
        bool retains;
        // The default is the EVIDENCED reason, so an arm that forgets to name
        // its cause claims evidence it does not have. That is the wrong
        // direction for a default, and it is caught rather than reasoned about:
        // escape_teeth's reason-call-retain entry mutates this line, and only
        // row 41 -- the one call with a real summary -- keeps it honest.
        EscapeReasons why = ESCAPE_REASON_CALL_RETAIN;
        bool variadic_tail = call->has_spread && (i == argc - 1);
        if (whitelisted) {
            retains = false; // whitelisted external retains no argument (7a')
        } else if (variadic_tail) {
            retains = true;
            why = ESCAPE_REASON_CALL_VARIADIC;   // a spread: no summary reaches it
        } else if (is_append && i == 0) {
            retains = false;  // append does not store the slice; it returns it
        } else if (callee) {
            retains = (i < callee_count) ? callee_escapes[i] : true;
            // Past the summary's parameter count -- a variadic user function.
            // The summary exists, so this is not CALL_OPAQUE.
            if (i >= callee_count) why = ESCAPE_REASON_CALL_VARIADIC;
        } else {
            retains = true; // external/unregistered/no-summaries: pure-conservative
            why = ESCAPE_REASON_CALL_OPAQUE;
        }
        if (retains) escape_mark(ctx, &arg_taints[i], why);
    }

    TaintSet result = escape_taint_new(n);
    if (whitelisted) {
        // A whitelisted external returns no argument-derived pointer (len/cap ->
        // int, print* -> void/(int,error), Sprintf -> a fresh string), so its
        // result carries none of the arguments' taint.
    } else if (callee) {
        if (callee_return_escapes) {
            for (i = 0; i < argc; i++) escape_taint_union_into(&result, &arg_taints[i]);
        }
    } else {
        for (i = 0; i < argc; i++) escape_taint_union_into(&result, &arg_taints[i]);
    }

    // A METHOD CAN RETURN A VALUE DERIVED FROM ITS RECEIVER, and the receiver is
    // NOT a member of call->args, so every union above is blind to it. That was
    // a use-after-free, not just lost precision.
    //
    // MEASURED. goo_error_message returns `e->message` VERBATIM as the string's
    // data pointer -- no copy. So for
    //     func msgOf(e error) string { return e.Error() }
    // the returned string ALIASES the error's message buffer. `e.Error()` has a
    // non-identifier callee and ZERO arguments, so the result taint came out
    // empty, return_escapes(msgOf) read false, and the caller's `m := msgOf(e)`
    // was approved as owned. Releasing `m` freed the live error's message.
    // valgrind on 100 iterations: 55 errors from 4 contexts, "Invalid read of
    // size 1" in strlen at runtime.c:461, address "16 bytes inside a block of
    // size 67 free'd" -- header plus message, so the error's own header went
    // with it. The direct shape `m := e.Error()` was already safe, because
    // release_decision's selector_base_is_local refuses it; the hole only
    // opened through a Goo helper, which is why no probe had caught it.
    //
    // Unioned for EVERY non-identifier callee rather than only for errors: the
    // engine cannot prove a method does not return part of its receiver, and
    // method summaries resolve by BARE NAME today, so there is no reliable
    // per-method answer to consult. Over-marking costs reclamation on a
    // method-call result; under-marking dangles a pointer. An identifier callee
    // leaves callee_taint empty, so an ordinary function call is unaffected,
    // and a PACKAGE-qualified call resolves its base to nothing in ctx->env,
    // which is why strings.Split and friends are also unaffected.
    escape_taint_union_into(&result, &callee_taint);
    escape_taint_free(&callee_taint);

    for (i = 0; i < argc; i++) escape_taint_free(&arg_taints[i]);
    free(arg_taints);
    return result;
}


static void handle_go_call(EscapeCtx* ctx, ASTNode* call_node, EscapeReasons why) {
    if (!call_node || call_node->type != AST_CALL_EXPR) {
        TaintSet t = escape_expr_taint(ctx, call_node);
        escape_mark(ctx, &t, why);
        escape_taint_free(&t);
        return;
    }
    CallExprNode* call = (CallExprNode*)call_node;

    // The CALLEE's taint escapes too, not just the arguments.
    //
    // This used to compute the callee's taint and DISCARD it, and for a
    // non-identifier callee that was an under-marking hole: `go p.m()` inside
    // an arena block left `p` arena-eligible, because the call carries no
    // arguments and nothing else marks the receiver. The method body then
    // dereferences `p` after the block has freed it. Verified at the IR level
    // before the fix — the `new(T)` emitted `goo_arena_alloc`.
    //
    // This was once masked by a SEPARATE defect: `go p.m()` failed module
    // verification, so no program of this shape built. That defect is FIXED
    // (the `go` statement handed codegen_generate_method_value a signature
    // that still carried the receiver), and `examples/arena_go_method_probe.goo`
    // is now the end-to-end gate for the marking below. It was confirmed to
    // fail under valgrind with that escape_mark call removed. The old note
    // also said "on a pointer receiver", which was wrong — a value receiver
    // failed identically. Separately, it stops being masked the moment
    // closure environments become allocation sites:
    // `go f()` on a local holding a closure takes the identifier path, which
    // did not even compute the taint.
    //
    // So the taint is now computed and marked for EVERY callee shape,
    // identifier included. A top-level function name resolves to the empty
    // set, making the extra work a no-op there. Over-marking is always safe;
    // under-marking is what dangles a pointer.
    {
        TaintSet ft = escape_expr_taint(ctx, call->function);
        escape_mark(ctx, &ft, why);
        escape_taint_free(&ft);
    }

    for (ASTNode* a = call->args; a; a = a->next) {
        TaintSet t = escape_expr_taint(ctx, a);
        escape_mark(ctx, &t, why);
        escape_taint_free(&t);
    }
}


TaintSet escape_expr_taint(EscapeCtx* ctx, ASTNode* expr) {
    size_t n = ctx->slot_count;
    if (!expr) return escape_taint_new(n);

    switch (expr->type) {
        case AST_IDENTIFIER: {
            const char* name = ((IdentifierNode*)expr)->name;
            LocalVar* lv = escape_env_find(ctx->env, name);
            if (lv) return escape_taint_copy(&lv->taint);
            return escape_taint_new(n); // outer-scope/global/unknown identifier => ∅
        }
        case AST_LITERAL:
            return escape_taint_new(n);
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            TaintSet l = escape_expr_taint(ctx, b->left);
            TaintSet r = escape_expr_taint(ctx, b->right);
            escape_taint_union_into(&l, &r);
            escape_taint_free(&r);
            return l;
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* u = (UnaryExprNode*)expr;
            {
                size_t idx = source_slot(ctx, expr);
                if (idx != ESCAPE_NO_SLOT) {
                    // Own site: self-bit UNION the taint carried by any of
                    // the composite's own field values (e.g. row 13's
                    // `&Node{next: n}` — if this composite escapes, so
                    // must the site `n` embedded within it).
                    TaintSet t = escape_taint_new(n);
                    t.bits[idx] = true;
                    TaintSet fields = escape_expr_taint(ctx, u->operand);
                    escape_taint_union_into(&t, &fields);
                    escape_taint_free(&fields);
                    return t;
                }
                // Belongs to a nested unit (or, defensively, is not
                // actually one of THIS unit's registered sites): fall
                // through to the generic recursion below, which still
                // picks up any of THIS unit's own taint referenced inside
                // it without crediting the foreign site's own identity.
            }
            return escape_expr_taint(ctx, u->operand);
        }
        case AST_POSTFIX_EXPR:
            return escape_expr_taint(ctx, ((PostfixExprNode*)expr)->operand);
        case AST_INDEX_EXPR: {
            // READING `m[k]` YIELDS THE STORED VALUE, WHICH CANNOT ALIAS `k`.
            //
            // The index's taint used to be unioned into the result, so a local
            // used as a read key rode the result into whatever sink consumed
            // it. That is over-approximation and not soundness: goo_map_get
            // stores nothing, and a slice index is an integer.
            //
            // THE INDEX IS STILL WALKED, and that is the load-bearing half of
            // this arm. escape_expr_taint applies sinks as it goes -- a call in
            // key position still marks its retained arguments, and an
            // unrecognised node still hits the default arm. Only the RESULT
            // drops the index's bits.
            //
            // A KEY USED IN A **WRITE** IS UNAFFECTED. mark_lvalue_subscripts
            // marks it SUBSCRIPT_STORE from the lvalue side, which is why local
            // row 15 stays green and `m[k] = k` still marks k twice over.
            //
            // MEASURED: bench/daemon/daemon.goo's `f` goes from
            // SUBSCRIPT_STORE|CONTAINER_STORE to SUBSCRIPT_STORE alone. The
            // CONTAINER_STORE came from the read half of
            // `counts[f] = counts[f] + 1` and named a flow that does not exist:
            // the value stored is an int.
            //
            // .handoff.md records tightening this as worth "~0%", and that was
            // TRUE while one bit conflated the causes -- the write marked `f`
            // anyway, so no verdict moved. Under a reason set the same edit is
            // what makes the map-key consumer reachable at all.
            IndexExprNode* ie = (IndexExprNode*)expr;
            TaintSet base = escape_expr_taint(ctx, ie->expr);
            TaintSet idx = escape_expr_taint(ctx, ie->index);
            escape_taint_free(&idx);
            return base;
        }
        case AST_SLICE_INDEX_EXPR: {
            SliceIndexExprNode* se = (SliceIndexExprNode*)expr;
            TaintSet base = escape_expr_taint(ctx, se->expr);
            TaintSet lo = escape_expr_taint(ctx, se->low);
            escape_taint_union_into(&base, &lo);
            escape_taint_free(&lo);
            TaintSet hi = escape_expr_taint(ctx, se->high);
            escape_taint_union_into(&base, &hi);
            escape_taint_free(&hi);
            return base;
        }
        case AST_SELECTOR_EXPR:
            return escape_expr_taint(ctx, ((SelectorExprNode*)expr)->expr);
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            {
                size_t idx = source_slot(ctx, expr);
                if (idx != ESCAPE_NO_SLOT) {
                    TaintSet t = escape_taint_new(n);
                    t.bits[idx] = true;
                    return t; // new(T)'s arg is a type, nothing to recurse into
                }
                // Foreign new(...) (nested unit's own site): fall through
                // to call_taint below — harmless, "new" is never a
                // registered user function so this is treated as an
                // ordinary external call, but its type-name "argument"
                // carries no taint either way.
            }
            return call_taint(ctx, call);
        }
        case AST_FUNC_LIT: {
            // Sink #3 (closure capture) — identical to param_escape.c:
            // read captured_names[] as populated by the type checker
            // (type_check_program must run before this analysis), do NOT
            // re-walk the closure body.
            FuncLitNode* lit = (FuncLitNode*)expr;
            TaintSet t = escape_taint_new(n);
            for (size_t i = 0; i < lit->captured_count; i++) {
                LocalVar* lv = escape_env_find(ctx->env, lit->captured_names[i]);
                if (lv) escape_taint_union_into(&t, &lv->taint);
            }
            escape_mark(ctx, &t, ESCAPE_REASON_CLOSURE_CAPTURE);

            // Phase 1a: this literal's own environment is a site (registered
            // by discover_expr above, iff it captures anything). Its self-bit
            // goes in AFTER the escape_mark call, NEVER before — before, the
            // environment would mark ITSELF escaping at its own definition and
            // registering it as a site would achieve nothing at all.
            //
            // Sink #3 is deliberately NOT relaxed by this: the captured values
            // above still escape, whatever the environment's own fate turns
            // out to be. The two are independent, and only the environment's
            // fate is new here. From this point the self-bit rides the
            // ordinary taint engine, so the environment escapes exactly when
            // the closure VALUE does — assignment, return, `go`, `defer`, or a
            // retaining call argument. Calling the closure does not leak it: a
            // body reads its captured cells but cannot store the env itself.
            //
            // find_site_index misses for a nested unit's own literal, which
            // then just contributes its captured taint — same conservative
            // fall-through the &composite and new(T) cases take.
            size_t idx = source_slot(ctx, expr);
            if (idx != ESCAPE_NO_SLOT) t.bits[idx] = true;
            return t;
        }
        case AST_STRUCT_LITERAL: {
            StructLiteralNode* sl = (StructLiteralNode*)expr;
            TaintSet t = escape_taint_new(n);
            for (ASTNode* v = sl->field_values; v; v = v->next) {
                TaintSet vt = escape_expr_taint(ctx, v);
                escape_taint_union_into(&t, &vt);
                escape_taint_free(&vt);
            }
            return t;
        }
        case AST_SLICE_EXPR: { // slice literal (SliceLitNode)
            SliceLitNode* sl = (SliceLitNode*)expr;
            TaintSet t = escape_taint_new(n);
            for (ASTNode* e = sl->elements; e; e = e->next) {
                TaintSet et = escape_expr_taint(ctx, e);
                escape_taint_union_into(&t, &et);
                escape_taint_free(&et);
            }
            return t;
        }
        case AST_ARRAY_LITERAL: {
            ArrayLitNode* al = (ArrayLitNode*)expr;
            TaintSet t = escape_taint_new(n);
            for (ASTNode* e = al->elements; e; e = e->next) {
                TaintSet et = escape_expr_taint(ctx, e);
                escape_taint_union_into(&t, &et);
                escape_taint_free(&et);
            }
            return t;
        }
        case AST_KEYED_ELEMENT:
            return escape_expr_taint(ctx, ((KeyedElementNode*)expr)->value);
        case AST_PAREN_EXPR: { // map literal (MapLitNode)
            MapLitNode* ml = (MapLitNode*)expr;
            TaintSet t = escape_taint_new(n);
            for (ASTNode* k = ml->keys; k; k = k->next) {
                TaintSet kt = escape_expr_taint(ctx, k);
                escape_taint_union_into(&t, &kt);
                escape_taint_free(&kt);
            }
            for (ASTNode* v = ml->values; v; v = v->next) {
                TaintSet vt = escape_expr_taint(ctx, v);
                escape_taint_union_into(&t, &vt);
                escape_taint_free(&vt);
            }
            return t;
        }
        case AST_SLICE_CONVERSION:
            return escape_expr_taint(ctx, ((SliceConvNode*)expr)->operand);
        case AST_TYPE_ASSERT:
            return escape_expr_taint(ctx, ((TypeAssertNode*)expr)->expr);
        default:
            // Genuinely unhandled expression kind: conservative escape of
            // every site — same rationale as param_escape.c's default arm.
            {
                TaintSet t = escape_taint_all(n);
                escape_mark(ctx, &t, ESCAPE_REASON_UNCLASSIFIED);
                return t;
            }
    }
}


void escape_seed_names_from_values(EscapeCtx* ctx, char** names, size_t name_count,
                                    ASTNode* values, bool* env_changed) {
    TaintSet combined = escape_taint_new(ctx->slot_count);
    for (ASTNode* v = values; v; v = v->next) {
        TaintSet t = escape_expr_taint(ctx, v);
        escape_taint_union_into(&combined, &t);
        escape_taint_free(&t);
    }
    for (size_t i = 0; i < name_count; i++) {
        if (strcmp(names[i], "_") == 0) continue;
        if (ctx->hooks->bind(ctx, names[i], &combined)) *env_changed = true;
    }
    escape_taint_free(&combined);
}


// The body of an EXPRESSION STATEMENT, and of a select case's comm clause.
//
// SHARED ON PURPOSE, and not copied. A select case's comm is an EXPRESSION -- the
// grammar builds every case from `CASE ... expression COLON case_body`
// (src/parser/parser.y) -- so it never reaches the AST_EXPR_STMT arm. The select
// arm used to hand it to escape_walk_stmt, where an expression node fell to
// `default:` and called escape_mark_all. EVERY local in ANY function containing a
// select therefore read as escaping. A precision defect, not a soundness one,
// which is why it survived: the safe answer here is `true`.
//
// THE SEND SINK IS WHY THIS IS A HELPER AND NOT A DELETION. escape_mark_all was
// covering `case ch <- v:` BY ACCIDENT. Measured: routing comm through plain
// escape_expr_taint instead, with no sink, makes local_escape row 26 report `x`
// as NOT escaping -- an under-mark, which is the use-after-free direction. The
// sink has to travel with the change, and sharing this helper is what stops the
// two callers from drifting apart later.
static void escape_walk_expr_stmt(EscapeCtx* ctx, ASTNode* e, bool* env_changed) {
    if (!e) return;
    if (e->type == AST_BINARY_EXPR) {
        BinaryExprNode* b = (BinaryExprNode*)e;
        if (is_assign_op(b->operator)) {
            TaintSet rhs = escape_expr_taint(ctx, b->right);
            assign_to_lvalue(ctx, b->left, &rhs, env_changed);
            escape_taint_free(&rhs);
            return;
        }
        if (b->operator == TOKEN_ARROW) {
            // Channel send `ch <- v`: the sent value LEAVES this block — a
            // receiver (another goroutine, or code running after the block) reads
            // it once the arena is already freed. So taint(v) escapes the block,
            // exactly like a goroutine/defer argument (a bare send of an arena
            // value was a use-after-free before this). `<-ch` receive is a UNARY
            // ARROW (a fresh in-bound value), correctly NOT a sink — handled by
            // escape_expr_taint.
            TaintSet lt = escape_expr_taint(ctx, b->left);
            escape_taint_free(&lt);
            TaintSet rhs = escape_expr_taint(ctx, b->right);
            escape_mark(ctx, &rhs, ESCAPE_REASON_CHAN_SEND);
            escape_taint_free(&rhs);
            return;
        }
    }
    TaintSet t = escape_expr_taint(ctx, e);
    escape_taint_free(&t);
}

void escape_walk_stmt(EscapeCtx* ctx, ASTNode* stmt, bool* env_changed) {
    for (; stmt; stmt = stmt->next) {
        switch (stmt->type) {
            case AST_BLOCK_STMT:
                escape_walk_stmt(ctx, ((BlockStmtNode*)stmt)->statements, env_changed);
                break;

            case AST_EXPR_STMT:
                escape_walk_expr_stmt(ctx, ((ExprStmtNode*)stmt)->expr, env_changed);
                break;

            case AST_IF_STMT: {
                IfStmtNode* n = (IfStmtNode*)stmt;
                TaintSet t = escape_expr_taint(ctx, n->condition);
                escape_taint_free(&t);
                escape_walk_stmt(ctx, n->then_stmt, env_changed);
                escape_walk_stmt(ctx, n->else_stmt, env_changed);
                break;
            }

            case AST_IF_LET_STMT: {
                IfLetStmtNode* n = (IfLetStmtNode*)stmt;
                TaintSet t = escape_expr_taint(ctx, n->nullable_expr);
                if (n->var_name && strcmp(n->var_name, "_") != 0) {
                    if (ctx->hooks->bind(ctx, n->var_name, &t)) *env_changed = true;
                }
                escape_taint_free(&t);
                escape_walk_stmt(ctx, n->then_stmt, env_changed);
                escape_walk_stmt(ctx, n->else_stmt, env_changed);
                break;
            }

            case AST_FOR_STMT: {
                ForStmtNode* n = (ForStmtNode*)stmt;
                if (n->range_expr) {
                    TaintSet t = escape_expr_taint(ctx, n->range_expr);
                    if (n->key_name && strcmp(n->key_name, "_") != 0) {
                        TaintSet empty = escape_taint_new(ctx->slot_count);
                        if (ctx->hooks->bind(ctx, n->key_name, &empty)) *env_changed = true;
                        escape_taint_free(&empty);
                    }
                    if (n->value_name && strcmp(n->value_name, "_") != 0) {
                        if (ctx->hooks->bind(ctx, n->value_name, &t)) *env_changed = true;
                    }
                    escape_taint_free(&t);
                } else {
                    if (n->init) escape_walk_stmt(ctx, n->init, env_changed);
                    if (n->condition) {
                        TaintSet t = escape_expr_taint(ctx, n->condition);
                        escape_taint_free(&t);
                    }
                    if (n->post) escape_walk_stmt(ctx, n->post, env_changed);
                }
                escape_walk_stmt(ctx, n->body, env_changed);
                break;
            }

            case AST_RETURN_STMT: {
                // Sink #1: returning from the enclosing FUNCTION definitely
                // means this value outlives the arena block being exited.
                ReturnStmtNode* n = (ReturnStmtNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) {
                    TaintSet t = escape_expr_taint(ctx, v);
                    escape_mark(ctx, &t, ESCAPE_REASON_RETURN);
                    // param_escape records `return_escapes` here — its
                    // interprocedural signal for "F gives back a value made
                    // from one of its own parameters". The other two passes
                    // have no such notion and leave the hook NULL.
                    if (ctx->hooks->on_return) ctx->hooks->on_return(ctx, &t);
                    escape_taint_free(&t);
                }
                break;
            }

            case AST_GO_STMT:
                handle_go_call(ctx, ((GoStmtNode*)stmt)->call, ESCAPE_REASON_GO_ARG);
                break;

            case AST_DEFER_STMT:
                handle_defer_call(ctx, ((DeferStmtNode*)stmt)->call);
                break;

            case AST_BREAK_STMT:
            case AST_CONTINUE_STMT:
                break;

            case AST_VAR_DECL: {
                VarDeclNode* n = (VarDeclNode*)stmt;
                escape_seed_names_from_values(ctx, n->names, n->name_count, n->values, env_changed);
                break;
            }

            case AST_CONST_DECL: {
                ConstDeclNode* n = (ConstDeclNode*)stmt;
                escape_seed_names_from_values(ctx, n->names, n->name_count, n->values, env_changed);
                break;
            }

            case AST_MULTI_ASSIGN: {
                MultiAssignNode* n = (MultiAssignNode*)stmt;
                TaintSet combined = escape_taint_new(ctx->slot_count);
                for (ASTNode* v = n->values; v; v = v->next) {
                    TaintSet t = escape_expr_taint(ctx, v);
                    escape_taint_union_into(&combined, &t);
                    escape_taint_free(&t);
                }
                if (n->is_short_decl) {
                    for (ASTNode* tgt = n->targets; tgt; tgt = tgt->next) {
                        if (tgt->type == AST_IDENTIFIER) {
                            const char* nm = ((IdentifierNode*)tgt)->name;
                            if (strcmp(nm, "_") != 0) {
                                if (ctx->hooks->bind(ctx, nm, &combined)) *env_changed = true;
                            }
                        } else {
                            escape_mark(ctx, &combined, ESCAPE_REASON_UNCLASSIFIED);
                        }
                    }
                } else {
                    for (ASTNode* tgt = n->targets; tgt; tgt = tgt->next) {
                        assign_to_lvalue(ctx, tgt, &combined, env_changed);
                    }
                }
                escape_taint_free(&combined);
                break;
            }

            case AST_SWITCH_STMT: {
                SwitchStmtNode* n = (SwitchStmtNode*)stmt;
                if (n->tag) {
                    TaintSet t = escape_expr_taint(ctx, n->tag);
                    escape_taint_free(&t);
                }
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type == AST_CASE_CLAUSE) {
                        CaseClauseNode* cc = (CaseClauseNode*)c;
                        for (ASTNode* e = cc->exprs; e; e = e->next) {
                            TaintSet t = escape_expr_taint(ctx, e);
                            escape_taint_free(&t);
                        }
                        escape_walk_stmt(ctx, cc->body, env_changed);
                    }
                }
                break;
            }

            case AST_TYPE_SWITCH: {
                TypeSwitchNode* n = (TypeSwitchNode*)stmt;
                TaintSet t = escape_expr_taint(ctx, n->expr);
                if (n->bind_name && n->bind_name->type == AST_IDENTIFIER) {
                    const char* bn = ((IdentifierNode*)n->bind_name)->name;
                    if (strcmp(bn, "_") != 0) {
                        if (ctx->hooks->bind(ctx, bn, &t)) *env_changed = true;
                    }
                }
                escape_taint_free(&t);
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type == AST_TYPE_CASE) {
                        escape_walk_stmt(ctx, ((TypeCaseNode*)c)->body, env_changed);
                    }
                }
                break;
            }

            case AST_SELECT_STMT: {
                SelectStmtNode* n = (SelectStmtNode*)stmt;
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type == AST_SELECT_CASE) {
                        SelectCaseNode* sc = (SelectCaseNode*)c;
                        // comm is an EXPRESSION, so it goes to the shared helper
                        // and NOT to escape_walk_stmt. See that helper for what
                        // walking it as a statement used to cost.
                        escape_walk_expr_stmt(ctx, sc->comm, env_changed);
                        escape_walk_stmt(ctx, sc->body, env_changed);
                    }
                }
                break;
            }

            case AST_UNSAFE_STMT:
                escape_walk_stmt(ctx, ((UnsafeStmtNode*)stmt)->body, env_changed);
                break;

            case AST_ARENA_BLOCK:
                // Transparent pass-through — see this file's header
                // comment: ownership of any site inside was already
                // resolved in Pass 1 by AST node identity, so re-walking
                // a nested block's body here under THIS unit's ctx cannot
                // credit a foreign site to this unit's bit space (see
                // escape_expr_taint's is_new_call/is_addr_of_composite handling).
                escape_walk_stmt(ctx, ((ArenaBlockNode*)stmt)->body, env_changed);
                break;

            case AST_ASSERT_STMT: {
                AssertStmtNode* n = (AssertStmtNode*)stmt;
                TaintSet t = escape_expr_taint(ctx, n->condition);
                escape_taint_free(&t);
                if (n->message) {
                    TaintSet tm = escape_expr_taint(ctx, n->message);
                    escape_taint_free(&tm);
                }
                break;
            }

            case AST_ASSUME_STMT: {
                TaintSet t = escape_expr_taint(ctx, ((AssumeStmtNode*)stmt)->condition);
                escape_taint_free(&t);
                break;
            }

            default:
                // Genuinely unhandled statement kind: conservative escape
                // of every site in this unit.
                escape_mark_all(ctx, ESCAPE_REASON_UNCLASSIFIED);
                break;
        }
    }
}


// A defer'd call. Which treatment is correct depends on the BOUNDARY, so this
// is a hook rather than a fixed rule — see EscapeHooks.defer_is_like_go in
// include/escape_core.h for why each pass answers the way it does.
static void handle_defer_call(EscapeCtx* ctx, ASTNode* call_node) {
    if (ctx->hooks->defer_is_like_go) {
        handle_go_call(ctx, call_node, ESCAPE_REASON_DEFER_ARG);
        return;
    }
    if (!call_node) return;
    TaintSet t = escape_expr_taint(ctx, call_node);
    escape_taint_free(&t);
}
