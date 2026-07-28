#include "shim_signatures.h"
#include <stdlib.h>
#include <string.h>

// See shim_signatures.h for the full design rationale. This file is the
// single declarative table (plus the two lookups over it) — no control
// flow beyond "find the row, build the Type(s)".

typedef struct {
    const char* pkg;
    const char* name;
    ShimRetKind ret;
    const ShimParamKind* params; // the FIXED-prefix params, never including
                                  // the variadic tail (see is_variadic below)
    size_t param_count;
    int is_variadic;

    // ARC / escape analysis: does this shim provably NOT retain a pointer
    // argument past the call, AND provably NOT return a value that aliases
    // one? Both halves are required, because escape_core.c's `whitelisted`
    // branch uses this one bit for both — it skips marking the arguments
    // escaping AND gives the call result an empty taint.
    //
    // SOUNDNESS: 1 is a TRUST assertion. If a row marked 1 actually retained
    // an argument, an arena value passed to it would be freed while still
    // referenced, and under ARC a released value would be freed while the
    // callee still held it. This bit only ever REDUCES escaping, so a wrong 1
    // is the one direction that can dangle a pointer. 0 is always safe.
    //
    // Every 1 below is justified against the runtime C body that implements
    // it, named in the row's comment. Adding a 1 needs the same proof.
    int non_retaining;
} ShimSignature;

static const ShimParamKind PARAMS_STRING[]              = { SHIM_PARAM_STRING };
static const ShimParamKind PARAMS_STRING_STRING[]        = { SHIM_PARAM_STRING, SHIM_PARAM_STRING };
static const ShimParamKind PARAMS_STRING_INT64[]         = { SHIM_PARAM_STRING, SHIM_PARAM_INT64 };
static const ShimParamKind PARAMS_STRING_SLICE_STRING[]  = { SHIM_PARAM_STRING_SLICE, SHIM_PARAM_STRING };
static const ShimParamKind PARAMS_INT64[]                = { SHIM_PARAM_INT64 };
static const ShimParamKind PARAMS_FLOAT64[]              = { SHIM_PARAM_FLOAT64 };
static const ShimParamKind PARAMS_FLOAT64_FLOAT64[]      = { SHIM_PARAM_FLOAT64, SHIM_PARAM_FLOAT64 };
static const ShimParamKind PARAMS_ERROR[]                = { SHIM_PARAM_ERROR };
// errors.Is(err, target): both operands are errors.
static const ShimParamKind PARAMS_ERROR_ERROR[]          = { SHIM_PARAM_ERROR, SHIM_PARAM_ERROR };
static const ShimParamKind PARAMS_INT64_FLOAT64[]        = { SHIM_PARAM_INT64, SHIM_PARAM_FLOAT64 };
// fmt.Fprintf(w, format, ...): the writer, then the format string. The reverse
// order of PARAMS_STRING_INT64, which os.ReadByte uses.
static const ShimParamKind PARAMS_INT64_STRING[]         = { SHIM_PARAM_INT64, SHIM_PARAM_STRING };

#define NPARAMS(arr) (sizeof(arr) / sizeof((arr)[0]))

// Ground truth for every row: the codegen arm that lowers the call
// (call_codegen.c) and, beneath it, the runtime C prototype (runtime.h) —
// see shim_signatures.h's doc comment. Value members (math.Pi, os.Args)
// are deliberately absent; stdlib_package_lookup keeps handling those.
static const ShimSignature SHIM_TABLE[] = {
    // fmt: Println/Print/Sprint/Sprintln are variadic-any with ZERO fixed
    // params — param_count=0, params=NULL below builds a Type with
    // param_types=NULL, which the call checker's existing
    // skip_variadic_builtin path already leaves fully unchecked (no new
    // hook needed for these four). Printf/Sprintf/Errorf require a real
    // first string param, checked, then an unchecked "any" tail.
    { "fmt", "Println",  SHIM_RET_VOID,   NULL,          0, 1, 1 },
    { "fmt", "Print",    SHIM_RET_VOID,   NULL,          0, 1, 1 },
    { "fmt", "Sprint",   SHIM_RET_STRING, NULL,          0, 1, 1 },
    { "fmt", "Sprintln", SHIM_RET_STRING, NULL,          0, 1, 1 },
    { "fmt", "Printf",   SHIM_RET_VOID,   PARAMS_STRING, NPARAMS(PARAMS_STRING), 1, 1 },
    { "fmt", "Sprintf",  SHIM_RET_STRING, PARAMS_STRING, NPARAMS(PARAMS_STRING), 1, 1 },
    { "fmt", "Errorf",   SHIM_RET_ERROR,  PARAMS_STRING, NPARAMS(PARAMS_STRING), 1, 0 },
    // The Fprint family: the same formatting as their unprefixed siblings,
    // written to a chosen stream. The writer is an io.Writer, so ANY value
    // implementing Write is accepted — this used to be an int64 file
    // descriptor with a CHECKER rule restricting the argument to the two
    // os selectors, because no io.Writer existed to type the slot.
    //
    // ZERO fixed params here, exactly like Println above, and for the same
    // reason: ShimParamKind is a closed enum of scalar kinds with no way to
    // spell an interface. The writer is therefore validated by the checker's
    // own arm (check_fprint_writer_arg, expression_checker.c), which runs a
    // real io.Writer satisfaction test rather than a syntactic name match.
    // Fprintf's format string is checked there too, since dropping the fixed
    // params drops the generic check that used to cover it.
    { "fmt", "Fprint",   SHIM_RET_VOID,   NULL, 0, 1, 0 },
    { "fmt", "Fprintln", SHIM_RET_VOID,   NULL, 0, 1, 0 },
    { "fmt", "Fprintf",  SHIM_RET_VOID,   NULL, 0, 1, 0 },
    // os. Args is a value member (skip, per doc comment above).
    //
    // NON-RETAINING: Exit only. It takes an int64 and does not return, so no
    // pointer argument exists to retain. Every other os row stays 0: their
    // runtime bodies were not audited for retention in this cut, and 0 is the
    // safe answer.
    { "os", "Exit",      SHIM_RET_VOID,  PARAMS_INT64,        NPARAMS(PARAMS_INT64), 0, 1 },
    { "os", "Getenv",    SHIM_RET_STRING, PARAMS_STRING,      NPARAMS(PARAMS_STRING), 0, 0 },
    { "os", "WriteFile", SHIM_RET_INT32, PARAMS_STRING_STRING, NPARAMS(PARAMS_STRING_STRING), 0, 0 },
    { "os", "ReadByte",  SHIM_RET_INT32, PARAMS_STRING_INT64, NPARAMS(PARAMS_STRING_INT64), 0, 0 },
    { "os", "FileSize",  SHIM_RET_INT32, PARAMS_STRING,       NPARAMS(PARAMS_STRING), 0, 0 },
    // P4.8: os.ReadFile(string) !string, os.ReadLine() !string — see
    // SHIM_RET_STRING_RESULT's doc comment in shim_signatures.h.
    { "os", "ReadFile",  SHIM_RET_STRING_RESULT, PARAMS_STRING, NPARAMS(PARAMS_STRING), 0, 0 },
    { "os", "ReadLine",  SHIM_RET_STRING_RESULT, NULL,          0, 0, 0 },
    // math. Pi is a value member (skip, per doc comment above).
    //
    // NON-RETAINING, all five, trivially: every parameter and every result is
    // a float64. There is no pointer for the callee to keep.
    { "math", "Sqrt", SHIM_RET_FLOAT64, PARAMS_FLOAT64,         NPARAMS(PARAMS_FLOAT64), 0, 1 },
    { "math", "Pow",  SHIM_RET_FLOAT64, PARAMS_FLOAT64_FLOAT64, NPARAMS(PARAMS_FLOAT64_FLOAT64), 0, 1 },
    { "math", "Abs",  SHIM_RET_FLOAT64, PARAMS_FLOAT64,         NPARAMS(PARAMS_FLOAT64), 0, 1 },
    { "math", "Min",  SHIM_RET_FLOAT64, PARAMS_FLOAT64_FLOAT64, NPARAMS(PARAMS_FLOAT64_FLOAT64), 0, 1 },
    { "math", "Max",  SHIM_RET_FLOAT64, PARAMS_FLOAT64_FLOAT64, NPARAMS(PARAMS_FLOAT64_FLOAT64), 0, 1 },
    // strings: Contains/ToUpper/ToLower/Split/Join stay in this table as
    // the FALLBACK below source exports (goostd/strings) — see
    // is_stdlib_shim_import's doc comment in goo.c for why `strings` walks
    // as a source package while still routing some symbols through here.
    //
    // NON-RETAINING, all six, and each one COPIES into a fresh goo_alloc
    // buffer, so no result aliases an argument either (src/runtime/runtime.c):
    //   Contains  -> goo_strings_contains: strstr, returns bool, stores nothing
    //   ToUpper   -> goo_strings_map_case: goo_alloc + per-byte write
    //   ToLower   -> goo_strings_map_case, same body
    //   TrimSpace -> goo_strings_trim_space: goo_alloc + memcpy. NOTE this is
    //                a COPY where Go's strings.TrimSpace returns a SLICE of
    //                its argument. The shim's behaviour is what is asserted
    //                here, not Go's.
    //   Split     -> goo_strings_split: goo_alloc for the goo_string_t array,
    //                then goo_string_new_with_length per part, which copies
    //   Join      -> goo_strings_join: goo_alloc + memcpy. Reads the parts
    //                slice, stores neither it nor its elements.
    { "strings", "Contains",   SHIM_RET_BOOL,        PARAMS_STRING_STRING,       NPARAMS(PARAMS_STRING_STRING), 0, 1 },
    { "strings", "ToUpper",    SHIM_RET_STRING,       PARAMS_STRING,             NPARAMS(PARAMS_STRING), 0, 1 },
    { "strings", "ToLower",    SHIM_RET_STRING,       PARAMS_STRING,             NPARAMS(PARAMS_STRING), 0, 1 },
    { "strings", "TrimSpace",  SHIM_RET_STRING,       PARAMS_STRING,             NPARAMS(PARAMS_STRING), 0, 1 },
    { "strings", "Split",      SHIM_RET_STRING_SLICE, PARAMS_STRING_STRING,      NPARAMS(PARAMS_STRING_STRING), 0, 1 },
    { "strings", "Join",       SHIM_RET_STRING,       PARAMS_STRING_SLICE_STRING, NPARAMS(PARAMS_STRING_SLICE_STRING), 0, 1 },
    // NON-RETAINING, both:
    //   Itoa -> goo_int_to_string: goo_alloc + memcpy of a stack buffer. The
    //           argument is an int64 anyway.
    //   Atoi -> reads the string and writes an int64 out-param. Its ERROR
    //           branch builds the COMPILE-TIME literal "strconv.Atoi: invalid
    //           syntax" (call_codegen.c, LLVMBuildGlobalStringPtr), so the
    //           returned error does not alias the argument.
    { "strconv", "Itoa", SHIM_RET_STRING,       PARAMS_INT64,  NPARAMS(PARAMS_INT64), 0, 1 },
    { "strconv", "Atoi", SHIM_RET_ATOI_RESULT,  PARAMS_STRING, NPARAMS(PARAMS_STRING), 0, 1 },
    // NON-RETAINING: New and Is only.
    //   New -> goo_error_from_string -> goo_error_wrap, which does
    //          goo_alloc(len+1) + memcpy of the message. The returned error
    //          owns a COPY and does not alias the argument.
    //   Is  -> walks the %w chain comparing pointer identity. Stores nothing.
    //
    // Unwrap is 0 and MUST stay 0: goo_error_unwrap returns `e->cause`
    // (runtime.c), a pointer INTO the argument's own structure. Its result
    // aliases its argument, which is exactly what this bit must not claim.
    { "errors", "New",    SHIM_RET_ERROR, PARAMS_STRING, NPARAMS(PARAMS_STRING), 0, 1 },
    { "errors", "Unwrap", SHIM_RET_ERROR, PARAMS_ERROR,  NPARAMS(PARAMS_ERROR), 0, 0 },
    // errors.Is walks the %w chain comparing identity. errors.As is
    // deliberately absent and is VACUOUS rather than merely hard: `error` is a
    // concrete type in v1 (pinned by custom_error_type_reject), so there are
    // no distinct concrete error types for it to recover.
    { "errors", "Is",     SHIM_RET_BOOL,  PARAMS_ERROR_ERROR, NPARAMS(PARAMS_ERROR_ERROR), 0, 1 },
    // far (M2-B1): far-transport shims, runtime side src/runtime/far_transport.c.
    // Listen/Dial reuse the !int construction ATOI_RESULT already builds;
    // RecvF64 needs the new !float64 tag. Error spellings are API (see
    // far_transport.h).
    { "far", "Listen",  SHIM_RET_ATOI_RESULT, PARAMS_STRING,        NPARAMS(PARAMS_STRING), 0, 0 },
    { "far", "Dial",    SHIM_RET_ATOI_RESULT, PARAMS_STRING,        NPARAMS(PARAMS_STRING), 0, 0 },
    { "far", "SendF64", SHIM_RET_VOID,        PARAMS_INT64_FLOAT64, NPARAMS(PARAMS_INT64_FLOAT64), 0, 0 },
    { "far", "RecvF64", SHIM_RET_F64_RESULT,  PARAMS_INT64,         NPARAMS(PARAMS_INT64), 0, 0 },
    { "far", "Close",   SHIM_RET_VOID,        PARAMS_INT64,         NPARAMS(PARAMS_INT64), 0, 0 },
};
#define SHIM_TABLE_COUNT (sizeof(SHIM_TABLE) / sizeof(SHIM_TABLE[0]))

static const ShimSignature* shim_signature_find(const char* package, const char* name) {
    if (!package || !name) return NULL;
    for (size_t i = 0; i < SHIM_TABLE_COUNT; i++) {
        if (strcmp(SHIM_TABLE[i].pkg, package) == 0 && strcmp(SHIM_TABLE[i].name, name) == 0) {
            return &SHIM_TABLE[i];
        }
    }
    return NULL;
}

int shim_signature_is_known_call(const char* package, const char* name) {
    return shim_signature_find(package, name) != NULL;
}

int shim_signature_is_non_retaining(const char* package, const char* name) {
    const ShimSignature* row = shim_signature_find(package, name);
    // An unknown pair is not a shim at all, so it gets the conservative answer
    // — the same answer every unaudited row carries.
    return row ? row->non_retaining : 0;
}

// Fresh "any" = the empty interface (Go 1.18+ predeclared `any`). Mirrors
// type_checker.c's static type_checker_any_type() exactly (same
// construction, same "fresh Type built on every call is safe" rationale —
// codegen never keys anything off this Type's pointer identity, only its
// empty method list and the vtable's string name) — duplicated rather than
// exposed from type_checker.c because that helper is itself a private,
// file-scoped piece of the AST_INTERFACE_TYPE evaluator, not a general
// public constructor.
static Type* shim_any_type(void) {
    Type* result = type_new(TYPE_INTERFACE);
    if (!result) return NULL;
    result->data.interface.methods = NULL;
    result->data.interface.method_count = 0;
    result->data.interface.name = NULL;
    result->data.interface.is_synthesized = 0;
    result->data.interface.source_concept = NULL;
    return result;
}

static Type* shim_param_type(TypeChecker* checker, ShimParamKind kind) {
    switch (kind) {
        case SHIM_PARAM_STRING:       return type_checker_get_builtin(checker, TYPE_STRING);
        case SHIM_PARAM_INT64:        return type_checker_get_builtin(checker, TYPE_INT64);
        case SHIM_PARAM_FLOAT64:      return type_checker_get_builtin(checker, TYPE_FLOAT64);
        case SHIM_PARAM_STRING_SLICE: return type_slice(type_checker_get_builtin(checker, TYPE_STRING));
        case SHIM_PARAM_ERROR:        return type_checker_error_type(checker);
    }
    return NULL;
}

static Type* shim_ret_type(TypeChecker* checker, ShimRetKind kind) {
    switch (kind) {
        case SHIM_RET_VOID:         return type_checker_get_builtin(checker, TYPE_VOID);
        case SHIM_RET_STRING:       return type_checker_get_builtin(checker, TYPE_STRING);
        case SHIM_RET_FLOAT64:      return type_checker_get_builtin(checker, TYPE_FLOAT64);
        case SHIM_RET_INT32:        return type_checker_get_builtin(checker, TYPE_INT32);
        case SHIM_RET_BOOL:         return type_checker_get_builtin(checker, TYPE_BOOL);
        case SHIM_RET_STRING_SLICE: return type_slice(type_checker_get_builtin(checker, TYPE_STRING));
        case SHIM_RET_ERROR:        return type_checker_error_type(checker);
        case SHIM_RET_ATOI_RESULT: {
            // strconv.Atoi(string) -> !int: success=int64 ("int"), error=string
            // — identical to the pre-existing checker stub this replaces
            // (expression_checker.c's old strconv.Atoi arm), byte-for-byte.
            Type* int_t = type_checker_get_builtin(checker, TYPE_INT64);
            Type* err_t = type_checker_get_builtin(checker, TYPE_STRING);
            return type_error_union(int_t, err_t);
        }
        case SHIM_RET_STRING_RESULT: {
            // os.ReadFile / os.ReadLine (P4.8) -> !string: success=string,
            // error=string. Shared by both rows — see the tag's doc comment
            // in shim_signatures.h for why this is one case, not two.
            Type* str_t = type_checker_get_builtin(checker, TYPE_STRING);
            Type* err_t = type_checker_get_builtin(checker, TYPE_STRING);
            return type_error_union(str_t, err_t);
        }
        case SHIM_RET_F64_RESULT: {
            // far.RecvF64 (M2-B1) -> !float64: success=float64, error=string
            // — same shape as SHIM_RET_ATOI_RESULT above, float64 in place
            // of int64.
            Type* f64_t = type_checker_get_builtin(checker, TYPE_FLOAT64);
            Type* err_t = type_checker_get_builtin(checker, TYPE_STRING);
            return type_error_union(f64_t, err_t);
        }
    }
    return NULL;
}

Type* shim_signature_lookup(TypeChecker* checker, const char* package, const char* name) {
    if (!checker) return NULL;
    const ShimSignature* sig = shim_signature_find(package, name);
    if (!sig) return NULL;

    Type* ret = shim_ret_type(checker, sig->ret);
    if (!ret) return NULL;

    if (sig->param_count == 0 && sig->is_variadic) {
        // Println/Print/Sprint/Sprintln: param_types=NULL, param_count=0,
        // is_variadic=1 — see this function's doc comment in the header for
        // why this needs no separate checking hook.
        Type* fn = type_function(NULL, 0, ret);
        if (fn) fn->data.function.is_variadic = 1;
        return fn;
    }

    // Fixed prefix params, built from the table; a variadic entry appends
    // one more slot: a slice of the empty-interface "any" type, standing in
    // for the unchecked variadic tail (Printf/Sprintf/Errorf).
    size_t total = sig->param_count + (sig->is_variadic ? 1 : 0);
    Type** params = total ? malloc(total * sizeof(Type*)) : NULL;
    if (total && !params) return NULL;
    for (size_t i = 0; i < sig->param_count; i++) {
        params[i] = shim_param_type(checker, sig->params[i]);
        if (!params[i]) { free(params); return NULL; }
    }
    if (sig->is_variadic) {
        Type* any_t = shim_any_type();
        Type* any_slice = any_t ? type_slice(any_t) : NULL;
        if (!any_slice) { free(params); return NULL; }
        params[sig->param_count] = any_slice;
    }

    Type* fn = type_function(params, total, ret);
    free(params); // type_function copies the array (types.c: malloc+memcpy)
    if (fn) fn->data.function.is_variadic = sig->is_variadic;
    return fn;
}
