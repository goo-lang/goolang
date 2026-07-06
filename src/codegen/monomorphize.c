#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Task 7: type-argument mangling for monomorphized generic instances. These
// helpers are pure name-computation (no LLVM/codegen state touched) so they
// can be unit-probed standalone; Tasks 9-10 call them from the monomorphization
// worklist to name each concrete instantiation (e.g. `Id[int]` -> `Id__int`).

// Per-file str_dup idiom (see src/types/types.c and friends) rather than a
// project-wide symbol: no header in this codebase declares a shared
// non-static str_dup, so every translation unit that wants one defines its
// own static copy. NULL-safe (mirrors the other copies), even though every
// call site below already guards against a NULL argument.
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) memcpy(dup, str, len + 1);
    return dup;
}

// A nameable, unique-enough token for a concrete type, suitable for splicing
// into an LLVM symbol name. Recurses through pointer/slice wrappers so
// `*[]int` -> "ptr_slice_int"; every other kind falls to the scalar/struct
// name via type_receiver_name, with type_to_string as a fallback.
//
// Both `type_receiver_name` and `type_to_string` are read from
// src/types/types.c: type_receiver_name returns `type->name` for non-struct
// kinds, which is NULL only if some Type was built without going through the
// standard type_int/type_bool/type_float/type_string_type/type_char
// constructors (those all populate `name`); type_to_string never returns
// NULL (it falls back to "?"/"struct"/"null"). So the `n ? n : type_to_string(t)`
// fallback below already guarantees a non-NULL token for every scalar this
// compiler can construct — the literal "T" is a last-resort belt-and-braces
// default in case a future Type kind reaches this path with both unset.
char* codegen_type_mangle_token(const Type* t) {
    if (!t) return str_dup("void");
    switch (t->kind) {
        case TYPE_POINTER: {
            char* inner = codegen_type_mangle_token(t->data.pointer.pointee_type);
            char* out = malloc(strlen(inner) + 5);
            sprintf(out, "ptr_%s", inner); free(inner); return out;
        }
        case TYPE_SLICE: {
            char* inner = codegen_type_mangle_token(t->data.slice.element_type);
            char* out = malloc(strlen(inner) + 7);
            sprintf(out, "slice_%s", inner); free(inner); return out;
        }
        default: {
            const char* n = type_receiver_name(t);
            if (!n) n = type_to_string(t); // fallback for scalars (never NULL)
            return str_dup(n ? n : "T");
        }
    }
}

// `base` + `{args[0..n)}` -> `base__tok0__tok1...`, e.g. `Map` + {int,string}
// -> `Map__int__string`. Caller frees the result.
char* codegen_mangle_instance(const char* base, Type* const* args, size_t n) {
    size_t cap = strlen(base) + 1;
    char** toks = calloc(n ? n : 1, sizeof(char*));
    for (size_t i = 0; i < n; i++) { toks[i] = codegen_type_mangle_token(args[i]); cap += strlen(toks[i]) + 2; }
    char* out = malloc(cap); strcpy(out, base);
    for (size_t i = 0; i < n; i++) { strcat(out, "__"); strcat(out, toks[i]); free(toks[i]); }
    free(toks); return out;
}
