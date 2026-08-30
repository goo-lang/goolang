// The TypeChecker accessors, extracted so they stop closing a dependency cycle.
//
// WHY THIS FILE EXISTS. Measured with nm on the built objects, 2026-08-30:
// src/types held one seven-file cycle -- embedding, expression_checker,
// expression_helpers, lane_ownership, ownership_checker, shim_signatures,
// type_checker -- and Bazel cannot express a cycle, so all seven had to share
// one coarse target and none of them could be tested on its own.
//
// Four of the edges into type_checker.c carried one or two symbols each, and
// every one of those symbols was an accessor rather than a decision:
//
//   shim_signatures -> type_checker   type_checker_error_type, ..._get_builtin
//   embedding       -> type_checker   type_checker_lookup_method
//   lane_ownership  -> type_checker   type_checker_lookup_variable
//   ownership_checker -> type_checker type_checker_lookup_variable
//
// Moving the four accessors here cuts all four edges. embedding.c and
// shim_signatures.c leave the cycle entirely and become their own targets.
//
// WHAT MAY LIVE HERE. Reads of TypeChecker state, and nothing that decides
// anything. Every function below is a lookup or a constructor over the
// checker's own tables. If a function here ever needs to report a type error,
// consult a statement, or walk an expression, it belongs back in
// type_checker.c -- and putting it here would re-close the cycle it exists to
// cut, which the link would report as a dependency loop rather than silently.
//
// The whole file depends on src/types/types.c and on no other file in this
// package. scope_lookup_variable, type_receiver_owner_package, type_nullable
// and type_pointer are all defined there.
#include "types.h"
#include <stdlib.h>

Type* type_checker_get_builtin(TypeChecker* checker, TypeKind kind) {
    if (!checker || !checker->builtin_types || kind >= TYPE_COUNT) {
        return NULL;
    }
    return checker->builtin_types[kind];
}

// v1 `error` = `?*int8` (a nullable pointer). Single source of truth so the
// `error` keyword, the n,err destructure, and errors.New stay in lockstep —
// Phase 6's real error struct / `.Error()` changes only this.
Type* type_checker_error_type(TypeChecker* checker) {
    Type* t = type_nullable(type_pointer(type_checker_get_builtin(checker, TYPE_INT8)));
    // Phase 6 Task 3: tag the nullable so `.Error()` dispatch (type checker +
    // codegen) can recognize "this is the error type" without re-deriving its
    // shape. type_nullable() always auto-derives a name (e.g. "?*int8" here),
    // so it is never NULL at this point — overwrite it unconditionally rather
    // than guarding on !t->name (which would never fire).
    if (t) {
        free(t->name);
        t->name = xstrdup("error");
    }
    return t;
}

Variable* type_checker_lookup_variable(TypeChecker* checker, const char* name) {
    if (!checker || !checker->current_scope) return NULL;
    return scope_lookup_variable(checker->current_scope, name);
}

// P4.3 (packages-B): see the doc comment on the declaration (types.h) for the
// full rationale. Dispatch is decided by the receiver type's OWNER, never by
// which scope happens to resolve the bare mangled name first (review-fix,
// CRITICAL): a main-package method with the same receiver-type name AND
// method name ("Point__Scale") used to hijack cross-package dispatch because
// the bare current-scope lookup ran first.
//
//   - owner set, owner != current_package (a cross-package receiver): the
//     owning package's exports are the ONLY legitimate source — Go's rule
//     is that methods on a package's type can only be defined in that
//     package, so NO fallback to the current scope exists (a bare hit there
//     is by construction a different, same-named type's method, or an
//     out-of-package method declaration Go itself would reject). Gated on
//     the METHOD name being exported (see the declaration comment for why
//     the mangled name's own leading case is not sufficient).
//   - owner == current_package (a package's own body checking/codegen'ing
//     calls on its own types): the method Variable lives in the still-pushed
//     package scope under the bare name — exports aren't even populated
//     until the whole body has been checked (package_export_filter runs
//     last) — so the current-scope lookup is the correct source, and
//     unexported methods are correctly callable intra-package.
//   - owner NULL (a main-declared or anonymous/builtin receiver): current
//     scope, today's behavior.
Variable* type_checker_lookup_method(TypeChecker* checker, Type* recv_type,
                                      const char* method_name, const char* mangled_name) {
    if (!checker || !mangled_name) return NULL;
    struct Package* owner = type_receiver_owner_package(recv_type);
    if (owner && owner != checker->current_package) {
        if (!method_name || method_name[0] < 'A' || method_name[0] > 'Z') return NULL;
        return scope_lookup_variable(owner->exports, mangled_name);
    }
    return type_checker_lookup_variable(checker, mangled_name);
}
