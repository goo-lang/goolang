// Arena leg — Task 7a': non-retaining external-call whitelist. See
// include/nonretaining.h for the contract and the soundness/status notes.

#include "nonretaining.h"
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

// `fmt` selector calls that provably do not retain a pointer argument:
//   fmt.Print / Println / Printf — format the arguments and write them to
//     stdout synchronously; nothing outlives the call.
//   fmt.Sprintf — formats the arguments into a NEW string and returns it; the
//     argument pointers are not retained, and the returned string is a fresh
//     copy that does not alias them.
// Deliberately EXCLUDED (they retain / may retain, so they stay conservative):
//   fmt.Errorf — with the %w verb the returned error wraps and retains an
//     argument, so it is NOT safe to whitelist.
//   append — stores its element arguments into a slice that may outlive the
//     call; conservative retain is required.
static bool is_nonretaining_fmt_selector(const char* selector) {
    if (!selector) return false;
    return strcmp(selector, "Print") == 0
        || strcmp(selector, "Println") == 0
        || strcmp(selector, "Printf") == 0
        || strcmp(selector, "Sprintf") == 0;
}

bool goo_callee_is_non_retaining(const ASTNode* call_function) {
    if (!call_function) return false;

    if (call_function->type == AST_IDENTIFIER) {
        return is_nonretaining_builtin(((const IdentifierNode*)call_function)->name);
    }

    if (call_function->type == AST_SELECTOR_EXPR) {
        const SelectorExprNode* sel = (const SelectorExprNode*)call_function;
        // Only the `fmt` package's listed functions — match the package
        // identifier AND the selector, so `other.Println` is not whitelisted.
        if (sel->expr && sel->expr->type == AST_IDENTIFIER
            && strcmp(((const IdentifierNode*)sel->expr)->name, "fmt") == 0) {
            return is_nonretaining_fmt_selector(sel->selector);
        }
        return false;
    }

    return false;
}
