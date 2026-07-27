#ifndef EMBEDDING_H
#define EMBEDDING_H

#include "types.h"

#define EMBED_MAX_DEPTH 8

typedef enum {
    EMBED_NOT_FOUND = 0,
    EMBED_FIELD,
    EMBED_METHOD,
    EMBED_AMBIGUOUS
} EmbedResultKind;

typedef struct {
    EmbedResultKind kind;
    // Field-name hops from the outer struct to the OWNER of the found member,
    // outermost first. Empty (len==0) never happens: direct members are the
    // caller's fast path, the resolver only reports promoted ones.
    const char* path[EMBED_MAX_DEPTH];
    size_t len;
    Type* type;          // FIELD: the field's type. METHOD: the mangled
                         // function's TYPE_FUNCTION (receiver = params[0]),
                         // EXCEPT when via_interface — see below.
    Type* owner;         // the embedded type that directly owns the member
                         // (pointer already unwrapped) — Task 6 re-mangles
                         // against type_receiver_name(owner).
    char ambig_a[128];   // AMBIGUOUS only: two dotted paths for diagnostics,
    char ambig_b[128];   // e.g. "Base.X" / "Other.X".
    int via_pointer;     // 1 if any hop from the outer struct to the member's
                         // owner traversed a pointer field (*T) — receiver-kind
                         // soundness: a value outer still holds a pointer-recv
                         // promoted method iff the path went through a pointer.
    int via_interface;   // METHOD only: the owner is an embedded INTERFACE
                         // (Go's `struct { io.Reader }`), so the member came
                         // from owner->data.interface.methods rather than from
                         // a declared method Variable. TWO consequences, and
                         // every EMBED_METHOD consumer must handle both:
                         //   1. `type` has NO receiver at params[0]. An
                         //      interface method type is receiver-less by
                         //      construction, so the usual
                         //      "param_count == want + 1" arity convention does
                         //      NOT hold and the receiver-kind rule is moot (an
                         //      embedded interface's methods are in the method
                         //      set of the outer VALUE — whatever concrete sits
                         //      in the field had its own receiver kind checked
                         //      when it was assigned there).
                         //   2. There is no function to call. Dispatch is
                         //      DYNAMIC, through the vtable of the value stored
                         //      in the interface field. A static call built
                         //      from a re-mangled owner name resolves to
                         //      nothing — the same "missing method
                         //      implementation" failure recorded in
                         //      docs/superpowers/specs/
                         //      2026-07-28-seeded-shim-vtable-spike.md.
                         // A synthesized receiver-spliced type would hide (1)
                         // and leave (2) silently broken, which is why this is
                         // a flag and not a fabricated signature.
} EmbedResult;

// BFS the embedding graph of `struct_type` for member `name` (field OR
// method), Go promotion rules: shallowest depth wins, outer shadows inner,
// >=2 hits at the winning depth => AMBIGUOUS. Direct (depth-0) members are
// NOT reported — callers handle those on their existing fast paths.
// Pointer-embedding cycles terminate via a visited set.
EmbedResult embedding_resolve(TypeChecker* checker, Type* struct_type,
                              const char* name);

#endif // EMBEDDING_H
