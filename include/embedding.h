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
                         // function's TYPE_FUNCTION (receiver = params[0]).
    Type* owner;         // the embedded type that directly owns the member
                         // (pointer already unwrapped) — Task 6 re-mangles
                         // against type_receiver_name(owner).
    char ambig_a[128];   // AMBIGUOUS only: two dotted paths for diagnostics,
    char ambig_b[128];   // e.g. "Base.X" / "Other.X".
    int via_pointer;     // 1 if any hop from the outer struct to the member's
                         // owner traversed a pointer field (*T) — receiver-kind
                         // soundness: a value outer still holds a pointer-recv
                         // promoted method iff the path went through a pointer.
} EmbedResult;

// BFS the embedding graph of `struct_type` for member `name` (field OR
// method), Go promotion rules: shallowest depth wins, outer shadows inner,
// >=2 hits at the winning depth => AMBIGUOUS. Direct (depth-0) members are
// NOT reported — callers handle those on their existing fast paths.
// Pointer-embedding cycles terminate via a visited set.
EmbedResult embedding_resolve(TypeChecker* checker, Type* struct_type,
                              const char* name);

#endif // EMBEDDING_H
