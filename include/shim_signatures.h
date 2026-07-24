#ifndef SHIM_SIGNATURES_H
#define SHIM_SIGNATURES_H

#include "types.h"

// P4.1: a single declarative table of every stdlib-shim CALLABLE symbol
// (fmt/os/math/strings/strconv/errors — the four hardcoded-shim packages
// plus the two source packages that still fall back to a shim per-symbol,
// see is_stdlib_shim_import's doc comment in goo.c). This replaces the old
// param-less `type_function(NULL, 0, ret)` stubs in stdlib_package_lookup
// (expression_checker.c) with REAL parameter lists, so the ordinary call
// checker (check_signature) can validate arity and argument types for a
// shim call exactly as it already does for a source-package export — see
// shim_signature_is_known_call's doc comment for the hook.
//
// Ground truth for every signature here is the CODEGEN arm that lowers the
// call (call_codegen.c, codegen_generate_stdlib_call and the bespoke
// fmt.*/strconv.Atoi/errors.* arms) and, one level further down, the
// runtime C prototype it calls (include/runtime.h) — never the OLD checker
// stubs, which carried no parameter info at all. Where a runtime C
// parameter is narrower than Goo's `int` (e.g. os.ReadByte's C `int`
// offset), the table still uses SHIM_PARAM_INT64: codegen already narrows
// via its own int-width coercion (codegen_generate_stdlib_call), and Goo's
// `int` keyword itself denotes int64 (see type_checker.c's "Default int"
// comments) — matching the language-level type keeps `os.ReadByte(path,
// n)` checkable against a plain `int` variable without forcing users to
// spell out int32 to match the C ABI.
//
// Value members (math.Pi, os.Args) are NOT calls and are not in this table
// — stdlib_package_lookup keeps handling them exactly as before.

// A parameter's checked type, expressed as a small tag rather than a Type*
// (built fresh per lookup from `checker`, since Type* construction needs a
// live TypeChecker for its builtin-type table).
typedef enum {
    SHIM_PARAM_STRING,
    SHIM_PARAM_INT64,        // Goo's `int` (int64) — see doc comment above
    SHIM_PARAM_FLOAT64,
    SHIM_PARAM_STRING_SLICE, // []string
    SHIM_PARAM_ERROR,        // the `error` type (?*int8)
} ShimParamKind;

// A call's return type. Most shims return a plain builtin/slice; a few
// need a builder beyond a bare TypeKind (error, []string, and three flavors
// of !T error-union) — SHIM_RET_ATOI_RESULT, SHIM_RET_STRING_RESULT, and
// SHIM_RET_F64_RESULT are dedicated tags for those, rather than a general
// "builder function pointer": strconv.Atoi and far.Listen/Dial (M2-B1) are
// the !int64/string entries (far's rows added without a new tag — same
// shim_ret_type() case, byte-identical construction), os.ReadFile/
// os.ReadLine (P4.8) are the !string/string entries, and far.RecvF64
// (M2-B1) is the one !float64/string entry. Entries that share a
// construction share ONE tag (not a tag per call) since building the exact
// same Type via the exact same one-line construction twice would be
// pointless duplication; see shim_ret_type()'s cases for each.
typedef enum {
    SHIM_RET_VOID,
    SHIM_RET_STRING,
    SHIM_RET_FLOAT64,
    SHIM_RET_INT32,
    SHIM_RET_BOOL,
    SHIM_RET_STRING_SLICE,
    SHIM_RET_ERROR,
    SHIM_RET_ATOI_RESULT,    // strconv.Atoi + far.Listen/Dial (all !int: int64 success / string error)
    SHIM_RET_STRING_RESULT,  // os.ReadFile / os.ReadLine: !string (string success / string error)
    SHIM_RET_F64_RESULT,     // far.RecvF64: !float64 (float64 success / string error)
} ShimRetKind;

// Returns a fresh type_function() Type* for (package, name)'s call-site
// signature, or NULL if the pair is not a known shim CALLABLE (including
// every value member — math.Pi, os.Args — which the caller must keep
// resolving through its own separate path). `is_variadic`/params on the
// built Type follow the table exactly:
//   - non-variadic entry: param_types = the table's fixed params, in order.
//   - variadic entry with 0 fixed params (Println/Print/Sprint/Sprintln):
//     param_types = NULL, param_count = 0, is_variadic = 1 — the existing
//     skip_variadic_builtin path in the call checker already leaves such a
//     callee's arity/args entirely unchecked (see type_check_call_expr),
//     so these need no new hook.
//   - variadic entry with fixed params (Printf/Sprintf/Errorf): param_types
//     = [the fixed params..., a slice-of-empty-interface "any" placeholder
//     for the variadic tail], is_variadic = 1 — the "any" slice's element
//     type is an empty TYPE_INTERFACE, which type_interface_satisfied
//     accepts for every concrete type (see its "or empty interface" return
///    site), so no variadic argument's type is ever rejected; only the
//     fixed prefix (the format string) is actually checked.
Type* shim_signature_lookup(TypeChecker* checker, const char* package, const char* name);

// True iff (package, name) names a known shim CALLABLE in the table above.
// Hook for the call checker (type_check_call_expr, mirroring the source-
// export TYPE_PACKAGE arm at expression_checker.c:~3609): when a package
// selector's export-scope lookup misses (true for every shim symbol, whose
// seeded Package carries an empty exports scope), re-check here before
// giving up on check_signature — this makes shim calls get real arity/
// arg-type checking without a marker on Type or the AST node, reusing the
// table as the single source of truth for "is this shim callee real."
int shim_signature_is_known_call(const char* package, const char* name);

#endif // SHIM_SIGNATURES_H
