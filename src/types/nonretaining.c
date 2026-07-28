// Arena leg — Task 7a': non-retaining external-call whitelist. See
// include/nonretaining.h for the contract and the soundness/status notes.

#include "nonretaining.h"
#include "shim_signatures.h"
#include <string.h>

// Plain-identifier builtins that provably do not retain a pointer argument:
//   len(x), cap(x)  — read the length/capacity of x; never store x.
//   print(x), println(x) — write x synchronously to stderr and return; the
//                          argument is not referenced after the call returns.
// A user function of the same name is never routed here (callers check the
// user-function registry first), so shadowing is analysed by its real body.
static bool is_nonretaining_builtin(const char* name) {
    if (!name) return false;
    return strcmp(name, "len") == 0
        || strcmp(name, "cap") == 0
        || strcmp(name, "print") == 0
        || strcmp(name, "println") == 0;
}

bool goo_callee_is_non_retaining(const ASTNode* call_function) {
    if (!call_function) return false;

    if (call_function->type == AST_IDENTIFIER) {
        return is_nonretaining_builtin(((const IdentifierNode*)call_function)->name);
    }

    if (call_function->type == AST_SELECTOR_EXPR) {
        const SelectorExprNode* sel = (const SelectorExprNode*)call_function;
        // A package selector call. The answer comes from the shim table's
        // `non_retaining` column, which is the single declarative home for
        // every shim fact and carries the per-symbol proof beside the row.
        //
        // This used to be a hardcoded list of six `fmt` selectors here. Those
        // six moved into the table unchanged, so the fact and its signature
        // can no longer drift apart. The change also widened the answer past
        // `fmt`: measured on bench/daemon, EVERY local passed to any stdlib
        // call was marked escaping purely because a C shim has no Goo body,
        // which made the whole ARC leg unable to reclaim anything.
        //
        // Matching is on BOTH the package identifier and the selector, so
        // `other.Println` is not whitelisted.
        if (sel->expr && sel->expr->type == AST_IDENTIFIER) {
            const char* pkg = ((const IdentifierNode*)sel->expr)->name;
            return shim_signature_is_non_retaining(pkg, sel->selector) != 0;
        }
        return false;
    }

    return false;
}
