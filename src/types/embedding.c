// Struct embedding promotion resolver (spec:
// docs/superpowers/specs/2026-07-04-struct-embedding-design.md).
// Single owner of Go's promotion rules: BFS by depth over embedded members,
// shallowest unique hit wins, ties at the winning depth are ambiguous.
#include "embedding.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define EMBED_MAX_QUEUE 64
#define EMBED_MAX_VISITED 64

typedef struct {
    Type* type;                        // struct type at this node
    const char* path[EMBED_MAX_DEPTH]; // field-name hops to reach it
    size_t len;
    int via_ptr;                       // 1 if any hop so far crossed a pointer field
} QueueEntry;

// The nameable identity of a type for the visited set / method mangling.
static const char* embed_type_name(Type* t) {
    if (!t) return NULL;
    if (t->kind == TYPE_STRUCT) return t->data.struct_type.name;
    return t->name;
}

// Unwrap *T to T for descending into an embedded pointer member.
static Type* embed_deref(Type* t) {
    if (t && t->kind == TYPE_POINTER) return t->data.pointer.pointee_type;
    return t;
}

static void embed_format_path(char* out, size_t cap,
                              const char* const* path, size_t len,
                              const char* leaf) {
    size_t off = 0;
    out[0] = '\0';
    for (size_t i = 0; i < len; i++) {
        int n = snprintf(out + off, cap - off, "%s.", path[i]);
        if (n < 0 || (size_t)n >= cap - off) return;
        off += (size_t)n;
    }
    snprintf(out + off, cap - off, "%s", leaf);
}

// Look for `name` DIRECTLY on `t` (fields first, then method T__name).
// Returns 1 and fills member_type/is_method on a hit.
static int embed_direct_member(TypeChecker* checker, Type* t, const char* name,
                               Type** member_type, int* is_method) {
    if (t->kind == TYPE_STRUCT) {
        for (size_t i = 0; i < t->data.struct_type.field_count; i++) {
            StructField* f = &t->data.struct_type.fields[i];
            if (f->name && strcmp(f->name, name) == 0) {
                *member_type = f->type;
                *is_method = 0;
                return 1;
            }
        }
    }
    const char* tn = embed_type_name(t);
    if (tn) {
        char* mangled = type_method_mangled_name(tn, name);
        // P4.3 review-fix (MAJOR): owner-routed lookup so a package-owned
        // embedded member's methods (declared in ITS package, visible only
        // through its exports scope) resolve during promotion and
        // interface-satisfaction-via-embedding — same routing as every
        // other method-existence site (see type_checker_lookup_method).
        Variable* m = mangled
            ? type_checker_lookup_method(checker, t, name, mangled)
            : NULL;
        free(mangled);
        if (m && m->type && m->type->kind == TYPE_FUNCTION) {
            *member_type = m->type;
            *is_method = 1;
            return 1;
        }
    }
    return 0;
}

EmbedResult embedding_resolve(TypeChecker* checker, Type* struct_type,
                              const char* name) {
    EmbedResult res;
    memset(&res, 0, sizeof(res));
    res.kind = EMBED_NOT_FOUND;
    if (!checker || !struct_type || struct_type->kind != TYPE_STRUCT || !name)
        return res;

    QueueEntry queue[EMBED_MAX_QUEUE];
    size_t head = 0, tail = 0;
    // Visited set WITH depths: a type may be reached again at the SAME depth
    // (diamond embedding — Go counts that as two members at that depth, i.e.
    // ambiguous), but never at a deeper one (that's a cycle, or shadowed by
    // the shallower occurrence either way).
    const char* visited[EMBED_MAX_VISITED];
    size_t visited_depth[EMBED_MAX_VISITED];
    size_t visited_count = 0;

    queue[tail].type = struct_type;
    queue[tail].len = 0;
    queue[tail].via_ptr = 0;
    tail++;
    const char* rootname = embed_type_name(struct_type);
    if (rootname && visited_count < EMBED_MAX_VISITED) {
        visited[visited_count] = rootname;
        visited_depth[visited_count] = 0;
        visited_count++;
    }

    size_t hit_depth = 0;
    int hits = 0;

    while (head < tail) {
        // Process one full BFS LEVEL at a time so same-depth ties are seen
        // together before any deeper level is explored.
        size_t level_end = tail;
        size_t depth = queue[head].len; // all entries in [head, level_end) share it
        for (size_t q = head; q < level_end; q++) {
            Type* t = queue[q].type;
            // Depth 0 is the outer struct itself: its direct members are the
            // caller's fast path — only descend, don't match.
            if (queue[q].len > 0) {
                Type* mt = NULL;
                int ism = 0;
                if (embed_direct_member(checker, t, name, &mt, &ism)) {
                    hits++;
                    if (hits == 1) {
                        hit_depth = queue[q].len;
                        res.kind = ism ? EMBED_METHOD : EMBED_FIELD;
                        res.type = mt;
                        res.owner = t;
                        res.via_pointer = queue[q].via_ptr;
                        // Path to the OWNER: all hops (the member itself is
                        // resolved at the last hop's type).
                        res.len = queue[q].len;
                        for (size_t i = 0; i < res.len; i++)
                            res.path[i] = queue[q].path[i];
                        embed_format_path(res.ambig_a, sizeof(res.ambig_a),
                                          queue[q].path, queue[q].len, name);
                    } else if (queue[q].len == hit_depth) {
                        res.kind = EMBED_AMBIGUOUS;
                        embed_format_path(res.ambig_b, sizeof(res.ambig_b),
                                          queue[q].path, queue[q].len, name);
                        return res;
                    }
                    continue; // a hit shadows everything below it — don't descend
                }
            }
            if (hits > 0 && queue[q].len >= hit_depth)
                continue; // deeper than the winning depth — irrelevant
            if (t->kind != TYPE_STRUCT || queue[q].len >= EMBED_MAX_DEPTH)
                continue;
            for (size_t i = 0; i < t->data.struct_type.field_count; i++) {
                StructField* f = &t->data.struct_type.fields[i];
                if (!f->is_embedded) continue;
                Type* child = embed_deref(f->type);
                if (!child) continue;
                const char* cn = embed_type_name(child);
                size_t child_depth = queue[q].len + 1;
                int seen_shallower = 0;
                for (size_t v = 0; v < visited_count; v++) {
                    if (cn && visited[v] && strcmp(visited[v], cn) == 0 &&
                        visited_depth[v] < child_depth) {
                        seen_shallower = 1;
                        break;
                    }
                }
                if (seen_shallower) continue; // cycle or shadowed — don't descend
                if (cn && visited_count < EMBED_MAX_VISITED) {
                    visited[visited_count] = cn;
                    visited_depth[visited_count] = child_depth;
                    visited_count++;
                }
                if (tail >= EMBED_MAX_QUEUE) continue; // bounded; silently deep
                queue[tail].type = child;
                queue[tail].len = queue[q].len + 1;
                queue[tail].via_ptr = queue[q].via_ptr || (f->type->kind == TYPE_POINTER);
                for (size_t p = 0; p < queue[q].len; p++)
                    queue[tail].path[p] = queue[q].path[p];
                queue[tail].path[queue[q].len] = f->name;
                tail++;
            }
        }
        head = level_end;
        if (hits > 0 && depth >= hit_depth) break; // winning level fully scanned
    }
    return res;
}
