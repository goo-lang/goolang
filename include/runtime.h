#ifndef RUNTIME_H
#define RUNTIME_H

#define _XOPEN_SOURCE 700  // For ucontext functions

#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct goo_error goo_error_t;
typedef struct goo_string goo_string_t;
typedef struct goo_slice goo_slice_t;
typedef struct goo_goroutine goo_goroutine_t;
typedef struct goo_channel goo_channel_t;
typedef struct goo_scheduler goo_scheduler_t;
typedef struct goo_mutex goo_mutex_t;
typedef struct goo_cond goo_cond_t;
typedef struct goo_waitgroup goo_waitgroup_t;

// Error structure
struct goo_error {
    const char* message;
    int code;
    struct goo_error* cause;  // Optional chained error
};

// String structure (compatible with code generator)
struct goo_string {
    char* data;
    size_t length;
};

// Generic slice structure (compatible with code generator)
struct goo_slice {
    void* data;
    size_t length;
    size_t capacity;
};

// Program initialization and cleanup
void goo_init(int argc, char** argv);
void goo_exit(int code);

// Memory management
void* goo_alloc(size_t size);
void* goo_realloc(void* ptr, size_t size);
void goo_free(void* ptr);

// ---------------------------------------------------------------------------
// ARC object header (ADR 0002 step 1)
// ---------------------------------------------------------------------------
// Every goo_alloc'd object carries a header IMMEDIATELY BEFORE its payload,
// the way malloc itself does: goo_alloc(n) allocates GOO_OBJ_HEADER_SIZE + n
// and returns base + GOO_OBJ_HEADER_SIZE. Callers keep seeing the object
// address, so no codegen change and no C shim is affected by the layout.
//
// 16 bytes, not 8: the payload must keep max_align_t guarantees, and arena.c
// already aligns to 16 (GOO_ARENA_ALIGNMENT), so 16 is the consistent choice.
//
// TWO POINTER KINDS HAVE NO HEADER, and every operation below is a no-op on
// them rather than undefined behaviour:
//
//   - goo_zerobase. Every zero-size allocation aliases one static byte, so a
//     header read through it reads whatever precedes a static and a header
//     write CORRUPTS it.
//   - NULL.
//
// A THIRD kind has no header and is NOT checked for, because it cannot be
// detected at runtime: an arena pointer. goo_arena_alloc bump-allocates and
// returns a bare interior pointer, so goo_release on one would read a header
// out of the previous object's tail and then free an interior pointer. The
// guarantee that this never happens is STATIC, and it is the whole subject of
// docs/superpowers/specs/2026-07-28-arc-arena-coexistence.md: block_escape
// routes a site to the heap whenever a callee retains that argument, so an
// arena pointer can only ever bind to a parameter proven non-escaping, and no
// retain or release is emitted for such a parameter. If that analysis ever
// under-marks, THIS is where the damage lands.
#define GOO_OBJ_HEADER_SIZE ((size_t)16)

// IMMORTAL: an object that is never freed, whatever the traffic on it.
//
// A Goo string LITERAL is the motivating case, and it was a FOURTH headerless
// pointer kind with nothing excluding it. codegen_const_string_value emitted a
// literal as { i8* -> private constant global, i64 len }, so goo_release on one
// would compute `global - 16` and hand a .rodata address to free(). NULL and
// goo_zerobase are checked for; an arena pointer is excluded by static proof;
// a literal was excluded by nothing at all. `last := ""` in bench/daemon is
// exactly that shape, so the first release consumer would have aborted on the
// benchmark it exists to fix.
//
// The fix makes the pointer kind SELF-DESCRIBING instead of demanding another
// static proof: a literal's global now carries a real header whose count is
// this sentinel, and retain/release/free are no-ops on it. That is the Swift
// and Objective-C immortal-object trick.
//
// UINT64_MAX, not 0, and the difference matters. 0 already means "not a
// counted object" for a headerless pointer, so reusing it would make
// goo_release's underflow panic unreachable — a real caller bug would then
// pass silently. A live counted object is 1 or more and can never reach
// UINT64_MAX, so the sentinel is unambiguous.
#define GOO_RC_IMMORTAL UINT64_MAX

// Current reference count. Returns 0 for NULL and for goo_zerobase, neither of
// which has a header — 0 is therefore "not a counted object", never a real
// count, because a live object is always at least 1.
uint64_t goo_obj_refcount(const void* ptr);

// Increment. No-op on NULL and on goo_zerobase.
//
// ATOMIC. Goroutines are not cooperative coroutines on one thread —
// goo_scheduler_init spawns goo_default_thread_count() OS threads, and a
// yielded goroutine is republished to a shared ready queue that any worker may
// take, so a goroutine can also MOVE between OS threads. That settles ADR
// 0002's open "atomic counts or per-goroutine" question in favour of atomic,
// and rules out any scheme keyed on the owning OS thread. See
// docs/superpowers/specs/2026-07-28-arc-atomic-counts.md.
void goo_retain(void* ptr);

// Decrement, and free the object when the count reaches 0. No-op on NULL and
// on goo_zerobase. Releasing below 0 is a caller bug, and panics rather than
// wrapping silently.
//
// ATOMIC, and it is ONE read-modify-write, not a read then a compare then a
// decrement. The three-step form let two goroutines each observe 1 and each
// decrement, which is a double free — measured, not hypothetical.
void goo_release(void* ptr);

// A destructor for the CONTENTS of an object, run by goo_release_with at the
// moment the count reaches 0 and immediately before the payload is freed.
//
// WHY THIS EXISTS. goo_release ends in goo_free(ptr) on ONE block, and the ARC
// header carries no type tag (see `reserved` above), so nothing in the release
// path can walk what an object CONTAINS. A map is the motivating case: freeing
// GooMapSV leaves every GooMapEntrySV hanging off it unreachable. Measured on
// bench/daemon -- 822,000 of the map's 902,000 bytes per 2,000 requests are
// those entry nodes (docs/adr/0002-measurements/element_scan_spike.md).
typedef void (*GooObjDtor)(void* obj);

// goo_release, with a destructor. `dtor` runs ONLY on the release that takes the
// count to 0, so an object someone else still holds keeps its contents. NULL is
// permitted and means "free the block and nothing else" -- goo_release(p) is
// exactly goo_release_with(p, NULL).
//
// The decrement is not duplicated here: goo_release delegates to this function,
// so the read-modify-write whose three-step form caused a measured double free
// exists in exactly one place.
void goo_release_with(void* ptr, GooObjDtor dtor);

// Mark an object as never-to-be-freed. A PACKAGE-LEVEL GLOBAL is live for the
// whole program, so nothing may ever free it -- and the escape analysis has
// no way to say so, because ParamEscapeSummary.return_escapes tracks
// parameters only: a function returning a global is indistinguishable from
// one returning a fresh allocation. Called once, from the global initializer
// function, on the heap part of each global's value right after it is
// stored (codegen_generate_global_init_function).
//
// Same trick a string literal's header has carried since PR #264: goo_retain
// and goo_release_with both check GOO_RC_IMMORTAL before touching the count,
// so this makes every later retain and release on this object a no-op.
// No-op on NULL and on goo_zerobase, exactly as goo_retain/goo_release_with
// are.
//
// READS the count before it writes: a string literal's header is ALREADY
// GOO_RC_IMMORTAL and lives in a constant .rodata global, so an unconditional
// store there is a write to read-only memory (see the fix-round-1 comment on
// the definition in runtime.c for the crash this caused and the invariant the
// guard relies on).
void goo_make_immortal(void* ptr);

// Release the first `len` elements of a slice buffer. The caller releases the
// buffer itself afterwards.
//
// NOT a GooObjDtor, and it cannot be one: a destructor gets only the object
// pointer, and this needs the LENGTH. The header carries no size, and a size
// would be the wrong number anyway because it covers CAP -- the elements
// between len and cap are uninitialised. Codegen reads len from field 1 of the
// slice value and passes it in.
//
// `stride` is sizeof(element), and the releasable pointer must be at element
// offset 0. True for a bare pointer (stride 8) and for a string, whose
// {i8*, i64} puts the data pointer first (stride 16). Codegen refuses any
// other element shape.
void goo_slice_release_elems(void* buf, int64_t len, int64_t stride);

// GooObjDtor for a map. Frees the entry nodes and NOT the keys or the values:
// goo_map_set_sv stores both verbatim, so the map owns neither -- a key can be
// a string literal's constant data. Wraps goo_map_clear_sv, which already
// applies exactly that rule; an adapter rather than a cast, because calling
// goo_map_clear_sv through GooObjDtor's type would be undefined behaviour.
void goo_map_dtor(void* obj);

// Bump/arena allocator: a growable block-list bump allocator that the
// arena-region memory model routes allocations into. Opaque; see
// src/runtime/arena.c for the block-list layout.
typedef struct GooArena GooArena;
GooArena* goo_arena_new(size_t initial_size);
void* goo_arena_alloc(GooArena* a, size_t size);
void goo_arena_reset(GooArena* a);
void goo_arena_free(GooArena* a);

// Runtime defer stack (P3.4): backs a "stack-mode" function's defer
// registrations — a function with at least one loop-nested `defer` routes
// ALL of its defers through this instead of the static per-lexical-defer
// active-flag machinery (statement_codegen.c), because a single loop
// iteration can register more than one dynamic defer and only a growable
// runtime stack can hold an unbounded, per-iteration count of them.
//
// A function's frame is a single goo_defer_frame_t, entry-block-allocated
// and zero-initialized by codegen (function_codegen.c). Each `defer`
// statement reached at runtime evaluates its args/receiver on the spot
// (Go's defer-time evaluation), heap-allocates an env cell holding that
// snapshot, and pushes {thunk, env} — so "executing the statement IS the
// registration" and per-iteration snapshots fall out for free. Every
// function-exit path calls goo_defer_run exactly once (LIFO unwind);
// goo_defer_run is safe on a never-pushed (zeroed) frame.
typedef struct goo_defer_entry {
    void (*fn)(void* env);  // thunk: unpacks env and makes the deferred call
    void* env;              // goo_alloc'd snapshot cell, or NULL for a no-arg defer
} goo_defer_entry_t;

typedef struct goo_defer_frame {
    goo_defer_entry_t* entries;  // goo_realloc'd growable array
    size_t len;
    size_t cap;
} goo_defer_frame_t;

// Push one deferred call onto `f` (grows `entries` via goo_realloc; panics
// via goo_realloc's own out-of-memory handling — never returns NULL to a
// caller that then dereferences it).
void goo_defer_push(goo_defer_frame_t* f, void (*fn)(void* env), void* env);

// Run every entry in `f` in LIFO (last-pushed-first) order, freeing each
// env right after its call and freeing the entries array afterward. Leaves
// `f` zeroed (len=0, cap=0, entries=NULL) — safe to call again (a no-op) or
// on a frame that was never pushed to at all.
void goo_defer_run(goo_defer_frame_t* f);

// Error handling
void goo_panic(const char* message) __attribute__((noreturn));
goo_error_t* goo_new_error(const char* message);
goo_error_t* goo_new_error_with_code(const char* message, int code);
goo_error_t* goo_error_from_string(goo_string_t msg);
goo_string_t goo_error_message(goo_error_t* e);
goo_error_t* goo_error_wrap(goo_string_t msg, goo_error_t* cause);
goo_error_t* goo_error_unwrap(goo_error_t* e);
// errors.Is — walk the wrap chain comparing IDENTITY. Returns int rather than
// bool to match goo_strings_contains and the other SHIM_RET_BOOL runtime
// entries, which predate any stdbool dependency in this header.
int goo_error_is(goo_error_t* err, goo_error_t* target);
void goo_error_free(goo_error_t* error);

// GooObjDtor for an error. goo_error_wrap goo_allocs the struct AND a copy of
// the message, so one goo_free from goo_release_with reaches only the struct
// -- the same shape goo_map_dtor exists for. Leaves `cause` alone: it is
// stored verbatim and the caller's own local may still name it, so freeing it
// here would free a value with another owner.
void goo_error_dtor(void* obj);

// I/O functions
void goo_print(const char* message);
void goo_println(const char* message);
void goo_print_string(goo_string_t str);
void goo_println_string(goo_string_t str);
// fmt.Fprint/Fprintln/Fprintf lower to this: the string is already formatted by
// the reused Sprint lowering, so this only picks the stream. fd is 1 (stdout)
// or 2 (stderr) — os.Stdout / os.Stderr, which the checker restricts the
// fmt.Fprint family to.
void goo_fwrite_string(int64_t fd, goo_string_t str);

// `goo test` runtime (src/runtime/testing.c). testing.Run takes the test
// FUNCTION as a value so the frame belongs to the runtime — that is what makes
// t.Fatal implementable, since nothing in Goo can unwind another function's
// frame. `file`/`line` on goo_testing_log are the CALL SITE's position,
// threaded down from codegen.
void goo_testing_run(goo_string_t name, void (*fn)(void*, void*), void* env);
void goo_testing_fail(void* t);
// Marks failed AND stops the test (longjmp back into goo_testing_run's frame).
void goo_testing_failnow(void* t);
void goo_testing_log(void* t, goo_string_t file, int64_t line, goo_string_t msg);
void goo_testing_summary(void);
void goo_println_int(int64_t value);
void goo_println_bool(int value);
void goo_println_float(double value);
void goo_print_int(int64_t value);
void goo_print_bool(int value);
void goo_print_float(double value);
void goo_println_uint(uint64_t value);
void goo_print_uint(uint64_t value);

// String operations
goo_string_t goo_string_new(const char* data);
goo_string_t goo_string_new_with_length(const char* data, size_t length);
void goo_string_free(goo_string_t str);
goo_string_t goo_string_concat(goo_string_t a, goo_string_t b);
// P1-1: value equality for `==`/`!=` on strings. Returns 1 iff equal
// (same length and bytes), 0 otherwise. nil/empty are equal to each other.
int goo_string_eq(goo_string_t a, goo_string_t b);
// P1-2: lexicographic comparison for `< <= > >=`. Returns -1, 0, or 1
// (a<b, a==b, a>b) — bytes compared up to the shorter length, then by length.
int goo_string_cmp(goo_string_t a, goo_string_t b);
// Scalar-to-string conversions — heap-allocate the result; used by fmt.Sprintf
// and reusable by a later strconv milestone.
goo_string_t goo_int_to_string(int64_t value);
goo_string_t goo_uint_to_string(uint64_t value);
goo_string_t goo_float_to_string(double value);
goo_string_t goo_bool_to_string(int value);
int goo_string_to_int(goo_string_t s, int64_t* out);
// string(rune) / string(byte) conversion (Go: any integer type -> string
// yields its value's Unicode code point UTF-8-encoded). An invalid code
// point (negative, a UTF-16 surrogate half, or beyond U+10FFFF) encodes as
// U+FFFD, matching Go. Returns by the same by-value {ptr,len} convention as
// goo_string_new above.
goo_string_t goo_string_from_rune(int32_t r);

// Stdlib package backings (used by codegen to lower fmt.*, os.*,
// strings.*, math.* calls into runtime symbols)
int goo_strings_contains(const char* haystack, const char* needle);
goo_string_t goo_strings_to_upper(const char* s);
goo_string_t goo_strings_to_lower(const char* s);
goo_string_t goo_strings_trim_space(const char* s);

// strings.Split / Join speak the canonical 3-field goo_slice_t (a
// []string whose `data` points at a goo_string_t array). The slice
// crosses the C<->codegen boundary BY POINTER, never by value: a
// 24-byte aggregate is SysV class MEMORY, and hand-emitted LLVM IR does
// not reproduce gcc's sret/byval lowering for it (by-value passing
// silently corrupts — only <=16-byte structs survive in registers).
// Split writes its result through `out`; Join reads `parts` in place.
void goo_strings_split(goo_slice_t* out, const char* s, const char* sep);
goo_string_t goo_strings_join(const goo_slice_t* parts, const char* sep);
goo_string_t goo_os_getenv(const char* name);

// os.ReadFile(path) -> !string / os.ReadLine() -> !string (P4.8). Same
// ok-flag + out-param shape as goo_string_to_int above (mirrored deliberately
// — see call_codegen.c's codegen_generate_string_result_call): return 1 with
// the success value written to *out, or 0 with a human-readable error message
// written to *out. goo_string_t is 16 bytes (ptr+len), safely by-value per
// goo_os_getenv above; only the file content itself needs the out-param, to
// stay byte-length honest (embedded NULs survive) the same way Split/Args use
// an out-param for their own >16-byte aggregate, not because goo_string_t
// itself needs one.
int goo_os_read_file(const char* path, goo_string_t* out);
int goo_os_read_line(goo_string_t* out);

// os.File — the write side (io arc). One opaque word holding the descriptor;
// the compiler seeds a matching one-field struct for the TYPE, and the two
// agree on exactly one thing: the fd sits at offset 0.
//
// os.Stdout / os.Stderr are *os.File, so they need storage with a STABLE
// address. Codegen emits a reference to these globals rather than building a
// file object per use, which is what makes pointer identity hold and lets a
// boxed io.Writer carry the same address.
struct goo_os_file;
extern struct goo_os_file goo_os_stdout_file;
extern struct goo_os_file goo_os_stderr_file;

// Scalar in, scalar out, per this header's file-I/O convention. Go's
// `Write(p []byte) (int, error)` returns a 24-byte tuple; rather than
// ABI-match it in C, the compiler emits an adapter carrying the Goo method
// ABI and that adapter calls this. Returns the byte count, or -errno.
int64_t goo_os_file_write(void* file, const void* buf, int64_t n);

// os.Args ([]string): argc/argv captured ONCE from the generated
// executable's entry point (see the is_entry_main prologue in
// src/codegen/function_codegen.c, the only caller of goo_os_args_init).
// goo_os_args() lazily builds and caches a []string from the raw argv on
// first read — argv storage is stable for the process lifetime, so
// caching is safe and avoids re-walking/re-allocating on every read.
// Same by-pointer ABI as goo_strings_split above: the 24-byte goo_slice_t
// is SysV class MEMORY and cannot cross the codegen<->C boundary by value.
void goo_os_args_init(int argc, char** argv);
void goo_os_args(goo_slice_t* out);
double goo_math_sqrt(double x);
double goo_math_pow(double x, double y);
double goo_math_abs(double x);
double goo_math_min(double x, double y);
double goo_math_max(double x, double y);

// General map `map[K]V` (M2-general-maps, extended for non-string keys).
// Both key and value travel as 8-byte slots (`int64_t`): the key slot holds
// either a char* (STRING) or the key's raw bits (INLINE — int/uint/bool/
// rune/byte/pointer); the value slot holds an integer or any pointer.
// Codegen casts each slot to/from the declared K/V types. Linear-scan
// linked list — performance is not the point; correctness is. Richer key
// kinds (struct/float) and comma-ok presence reads beyond what's below are
// future work; on a miss `get` returns 0.
//
// Map key kind: how goo_map_key_eq compares two int64 key slots. STRING = the
// slot holds a char*, compared by strcmp; INLINE = the slot holds the key's
// bits (int/uint/bool/rune/byte/pointer), compared by ==; STRUCT = the slot
// holds a pointer to a heap copy of the struct, compared via the per-map
// key_eq comparator. IFACE = the slot holds a pointer to a heap-copied boxed
// interface value `{void* vtable; void* data}`; compared via goo_iface_key_eq
// below (also reached through the per-map key_eq comparator). New kinds
// (float) append here later.
typedef int (*GooKeyEqFn)(int64_t a, int64_t b);
enum { GOO_MAPKEY_STRING = 0, GOO_MAPKEY_INLINE = 1, GOO_MAPKEY_STRUCT = 2, GOO_MAPKEY_IFACE = 3 };

// Interface-typed map keys (Task 2): compare two boxed interface key slots.
// Each slot is a pointer to a heap-copied `{void* vtable; void* data}` pair
// (mirrors a struct key's "pointer to a heap copy" — codegen_map_key_to_slot's
// TYPE_INTERFACE arm). Equality is Go's interface equality: same dynamic type
// (vtable identity — see codegen_interface_vtable's per-(concrete,iface)
// symbol-name caching, codegen/interface_codegen.c) AND equal dynamic value
// (dispatched to vtable slot 0, the concrete's per-type value-equality
// comparator synthesized by codegen_get_or_emit_type_eq). A NULL vtable is a
// nil interface; two nils compare equal, nil vs. non-nil never does (falls
// out of the `vta != vtb` check — NULL != any real vtable pointer).
int goo_iface_key_eq(int64_t a, int64_t b);

// Format an interface value {vtable,data} as its %v string (fmt.Println/
// fmt.Sprintf's interface-argument path). nil vtable -> "<nil>"; otherwise
// hops vtable slot 0 -> descriptor -> descriptor field 2 (fmt_fn) and calls
// it with `data`. See codegen_get_or_emit_type_fmt (interface_codegen.c) for
// how fmt_fn thunks are synthesized.
goo_string_t goo_iface_format(void* vtable, void* data);

// Panic for a failed type assertion `x.(T)`, naming the DYNAMIC (actually
// held) type instead of a compile-time-only static message (Task 4 of the
// interface-type-descriptor plan). `iface_name` and `target_name` are the
// STATIC names codegen already had (the interface's own name, and the
// assertion's target type); `vtable` is the asserted interface value's
// runtime vtable — slot 0 is the per-concrete-type descriptor (see
// goo_iface_format above), whose field 1 is the dynamic type_name. A NULL
// vtable (nil interface) renders as "<nil>", mirroring goo_iface_format's own
// nil handling. Formats "interface conversion: %s is %s, not %s" and calls
// goo_panic — never returns.
void goo_panic_iface_conversion(const char* iface_name, void* vtable,
                                 const char* target_name) __attribute__((noreturn));

// Panic for a failed type assertion `x.(I)` to an INTERFACE target I
// (interface-target RTTI, Task 1) — Go's own wording for this shape differs
// from goo_panic_iface_conversion's concrete-target "X is Y, not Z": it's
// "<dynamic> is not <I>" (real Go appends ": missing method M"; v1 omits it
// since the miss is decided by closed-world descriptor identity across
// every implementer, not a single missing-method check). Reads the dynamic
// type name the same way goo_panic_iface_conversion does (vtable slot 0 ->
// descriptor field 1); a NULL vtable (nil interface) renders as "<nil>".
void goo_panic_iface_notimpl(void* vtable, const char* target_name) __attribute__((noreturn));

struct GooMapEntrySV;
typedef struct GooMapSV {
    struct GooMapEntrySV* head;
    int32_t key_kind;
    GooKeyEqFn key_eq;   // per-map struct-key comparator; NULL for string/inline maps
} GooMapSV;
GooMapSV* goo_map_new_sv(int32_t key_kind, GooKeyEqFn key_eq);
void goo_map_set_sv(GooMapSV* m, int64_t k, int64_t v);
// goo_map_set_sv, and the map TAKES ownership of the key.
//
// Codegen emits this in place of goo_map_set_sv when release_decision proves
// the key expression is a fresh temporary that no other name ever held. After
// it returns, the map is the key's only owner: goo_map_clear_sv,
// goo_map_delete_sv and therefore goo_map_dtor will release it, and nothing
// else may.
//
// Ownership is recorded PER ENTRY, so one map can mix owned and borrowed keys.
// That is the common case, not a corner: bench/daemon writes `counts[f]` with a
// borrowed local and `counts[strings.ToUpper(f)]` with a fresh allocation.
//
// CALLER CONTRACT: do not free the key afterwards, and do not pass a key that
// a live local also releases. A key slot that is not a pointer (an INLINE map)
// must never reach here — codegen refuses that, and the runtime checks the key
// kind as a second layer.
void goo_map_set_sv_owning(GooMapSV* m, int64_t k, int64_t v);
int64_t goo_map_get_sv(GooMapSV* m, int64_t k);
// Presence-returning read: *found=1 and *out=value if k is present, else
// *found=0 and *out=0. Backs comma-ok map reads (v, ok := m[k]).
void goo_map_get_sv_ok(GooMapSV* m, int64_t k, int64_t* out, int* found);
// Entry count. Backs len(m); linear scan of the linked list.
int64_t goo_map_len_sv(GooMapSV* m);
// Map iteration: cursor-based walk of the entry list. Init the cursor to
// m->head (NULL map ⇒ NULL cursor ⇒ immediate end). Iteration order is the
// list order — HEAD insertion makes that REVERSE INSERTION ORDER, which is
// DETERMINISTIC. Documented deviation: Go deliberately randomizes map
// iteration order; Goo does not (recorded in the decl-surface-breadth
// spec). Entries deleted mid-iteration: unlinking a not-yet-visited entry
// skips it (Go-consistent); deleting the CURRENT entry frees the node the
// cursor points through — callers of the codegen'd loop cannot do that
// today (no delete inside range bodies in generated code paths), recorded
// as a limitation, not defended.
//
// GooMapEntrySV stays opaque here (only forward-declared above, as before):
// `cursor` is typed `struct GooMapEntrySV**`, which is a legal, fully-typed
// parameter even though the pointee is incomplete in this translation unit
// (pointer types are always complete regardless of pointee completeness).
// That is preferred over moving the entry struct's definition into the
// header (would leak runtime.c-private layout to every includer) and over
// typing the cursor as `void**` (would erase the type distinction between
// "a map entry cursor" and "any pointer-to-pointer", inviting a mismatched
// call at the LLVM IR call-site to go unnoticed).
int goo_map_iter_next_sv(struct GooMapEntrySV** cursor, int64_t* key_out, int64_t* val_out);
// Cursor init for the walk above: returns m->head, or NULL when m itself
// is NULL. Exists precisely for the nil-map case — Go's zero-value map
// (`var m map[string]int`, never made) is legal to range over and yields
// zero iterations, so the "NULL map ⇒ NULL cursor" caller obligation
// documented on goo_map_iter_next_sv is discharged HERE in the runtime,
// not by a null-check every code generator must remember to emit. Also
// keeps GooMapSV's field layout out of generated IR entirely (no
// GEP-to-head at the range-loop site).
struct GooMapEntrySV* goo_map_iter_init_sv(GooMapSV* m);
// Deletes the entry for key k, if present. Backs delete(m, k). Does not
// free k: the map never owns key storage (see goo_map_set_sv above).
void goo_map_delete_sv(GooMapSV* m, int64_t k);
// Removes every entry (no-op on a NULL/empty map). Backs clear(m) (Go
// 1.21). Same key-ownership contract as goo_map_delete_sv.
void goo_map_clear_sv(GooMapSV* m);

// Slice operations
// Zero-initialized backing store for make([]T, n[, cap]). Returns a bare
// pointer (never NULL, even for count 0) rather than a goo_slice_t: the
// 24-byte header cannot cross the codegen<->C boundary by value (SysV
// MEMORY class — see the goo_slice_new warning in runtime_integration.c),
// so codegen builds the {ptr,len,cap} aggregate itself in IR.
void* goo_slice_alloc(int64_t count, int64_t elem_size);
goo_slice_t goo_slice_new(size_t element_size, size_t capacity);
void goo_slice_free(goo_slice_t slice);
void* goo_slice_get(goo_slice_t slice, size_t index, size_t element_size);
int goo_slice_append(goo_slice_t* slice, void* element, size_t element_size);

// copy(dst, src) core (Go-exact): moves min(dst_len, src_len) elements and
// returns the count. memmove — overlapping ranges are legal (the copy_probe
// golden's copy(src[1:4], src[0:3]) case pins this). Raw-pointer ABI (no
// goo_slice_t* — the caller already has each side's {data,len}, whether the
// source is a slice or, when the destination's element is byte, a string).
int64_t goo_slice_copy_raw(void* dst, int64_t dst_len,
                           const void* src, int64_t src_len, int64_t elem_size);
// append(dst, s...) bulk arm: snapshot src, grow dst by src_len (same
// amortized-doubling policy as goo_slice_append), then move the snapshot in.
// The snapshot is what makes self-append (append(b, b...)) safe across the
// grow — a realloc of dst may free/move the very block src still aliases.
void goo_slice_append_bulk(goo_slice_t* dst, const void* src,
                           int64_t src_len, int64_t elem_size);

// []byte(s) / string(b) conversion cores. Copy semantics (Go-exact): the
// result never aliases the source. Bare-pointer ABI (goo_slice_t never
// crosses by value); lengths explicit.
void* goo_bytes_from_string(const char* p, int64_t len);
char* goo_cstr_from_bytes(void* data, int64_t len);

// Runtime type information (for future use)
typedef enum {
    GOO_TYPE_VOID,
    GOO_TYPE_BOOL,
    GOO_TYPE_INT8,
    GOO_TYPE_INT16,
    GOO_TYPE_INT32,
    GOO_TYPE_INT64,
    GOO_TYPE_UINT8,
    GOO_TYPE_UINT16,
    GOO_TYPE_UINT32,
    GOO_TYPE_UINT64,
    GOO_TYPE_FLOAT32,
    GOO_TYPE_FLOAT64,
    GOO_TYPE_STRING,
    GOO_TYPE_POINTER,
    GOO_TYPE_ARRAY,
    GOO_TYPE_SLICE,
    GOO_TYPE_STRUCT,
    GOO_TYPE_ERROR_UNION,
    GOO_TYPE_NULLABLE
} goo_type_kind_t;

typedef struct goo_type_info {
    goo_type_kind_t kind;
    size_t size;
    size_t align;
    const char* name;
} goo_type_info_t;

// Bounds checking (for safe mode)
//
// arc-17: goo_bounds_fail/goo_slice_bounds_fail are UNCONDITIONAL-fail,
// noreturn functions — codegen inlines the `index >= length` compare (resp.
// the slice-bounds OR-chain) directly at the call site and only branches
// into these on the cold failure edge (codegen_emit_bounds_check /
// codegen_generate_slice_index_expr in src/codegen/composite_codegen.c).
// `noreturn` matters both for codegen (the fail block ends in
// LLVMBuildUnreachable right after the call) and for the plain C build.
// goo_bounds_check/goo_slice_bounds_check below are kept as thin
// conditional wrappers for source/ABI compatibility with pre-arc-17
// callers — codegen itself no longer emits calls to them.
void goo_bounds_fail(size_t index, size_t length, const char* file, int line) __attribute__((noreturn));
void goo_slice_bounds_fail(int64_t low, int64_t high, int64_t max, const char* file, int line) __attribute__((noreturn));

// ADR 0001: cold noreturn target of the inline nil checks
// (codegen_emit_nil_check) at pointer-deref/field/interface-dispatch
// sites — same shape as goo_bounds_fail above. The message text is Go's
// canonical nil-panic wording and is pinned by scripts/nil_deref_probe.sh;
// changing it is a contract change, not a wording tweak.
void goo_nil_deref_fail(const char* file, int line) __attribute__((noreturn));

void goo_bounds_check(size_t index, size_t length, const char* file, int line);

// Bounds check for slice/substring EXPRESSIONS `base[low:high]` — the
// sibling of goo_bounds_check for single-element index reads/writes. Panics
// if low < 0 || high < low || high > max, where max is cap(base) for a
// slice and len(base) for a string. See goo_slice_bounds_check in runtime.c.
void goo_slice_bounds_check(int64_t low, int64_t high, int64_t max, const char* file, int line);

// Decode the UTF-8 rune at data[i] (i < len); writes *rune_out, returns byte
// width (1..4). Used by rune-aware for-range-over-string. See runtime.c.
int32_t goo_utf8_decode(const char* data, int64_t len, int64_t i, int32_t* rune_out);

// (GOO_BOUNDS_CHECK was here. It had ZERO call sites, and its GOO_DEBUG guard
// was unreachable because no target defined GOO_DEBUG -- so it always expanded
// to (void)0 and had never been compiled in its live form. Now that `make
// debug` really defines GOO_DEBUG, keeping it would silently switch on a macro
// nobody calls, wrapping goo_bounds_check(), which is a RUNTIME entry point
// that codegen emits for compiled Goo programs and not a check on the
// compiler's own C. The function stays; the unused C-side macro is gone. Use
// GOO_ASSERT from include/goo_assert.h for an invariant in compiler code.)

// Concurrency support

// Goroutine function type
typedef void (*goo_goroutine_func_t)(void* arg);

// Goroutine creation and management
int goo_default_thread_count(void);
void goo_scheduler_init(int num_threads);
void goo_scheduler_shutdown(void);
// Block the caller (typically generated main) until every spawned goroutine has
// finished, so goroutine side effects are observable before the program exits.
void goo_scheduler_wait(void);
goo_goroutine_t* goo_go(goo_goroutine_func_t func, void* arg);
void goo_yield(void);
void goo_goroutine_exit(void);
// Returns the goroutine currently running on the calling worker thread, or NULL
// if the caller is not inside a goroutine. (Per-thread; supersedes the old
// shared g_scheduler->current_goroutine field for channel-wait bookkeeping.)
goo_goroutine_t* goo_current_goroutine(void);

// Channel operations
goo_channel_t* goo_make_chan(size_t elem_size, size_t buffer_size);
void goo_chan_close(goo_channel_t* ch);
void goo_chan_free(goo_channel_t* ch);

// Channel send/receive
int goo_chan_send(goo_channel_t* ch, void* data);
int goo_chan_recv(goo_channel_t* ch, void* data);
int goo_chan_send_timeout(goo_channel_t* ch, void* data, uint64_t timeout_ns);
int goo_chan_recv_timeout(goo_channel_t* ch, void* data, uint64_t timeout_ns);

// Non-blocking operations
int goo_chan_try_send(goo_channel_t* ch, void* data);
int goo_chan_try_recv(goo_channel_t* ch, void* data);

// Channel state queries
int goo_chan_is_closed(goo_channel_t* ch);
size_t goo_chan_len(goo_channel_t* ch);
size_t goo_chan_cap(goo_channel_t* ch);

// Select operation support
typedef struct goo_select_case {
    goo_channel_t* channel;
    void* data;
    int is_send;  // 1 for send, 0 for receive
    int ready;    // Set by select operation
} goo_select_case_t;

int goo_select(goo_select_case_t* cases, size_t num_cases, int64_t timeout_ns);

// Deadlock detection
typedef struct goo_deadlock_detector {
    int enabled;
    uint64_t last_check_time;
    uint64_t check_interval_ns;
    int detected_deadlock;
    int blocked_goroutines;   // M9: goroutines currently parked in a channel cond_wait
    int main_in_wait;         // M9: main has entered goo_scheduler_wait (body done)
} goo_deadlock_detector_t;

// Deadlock detection functions
int goo_deadlock_init(void);
void goo_deadlock_shutdown(void);
void goo_deadlock_enable(int enable);
int goo_deadlock_detected(void);
void goo_sched_block_begin(void);
void goo_sched_block_end(void);
void goo_deadlock_abort(void) __attribute__((noreturn));

// Channel pattern operations
int goo_chan_subscribe(goo_channel_t* publisher, goo_channel_t* subscriber);
int goo_chan_unsubscribe(goo_channel_t* publisher, goo_channel_t* subscriber);
int goo_chan_pair_req_rep(goo_channel_t* req_chan, goo_channel_t* rep_chan);
int goo_chan_add_worker(goo_channel_t* push_chan, goo_channel_t* pull_chan);

// Goroutine states
typedef enum {
    GOO_GOROUTINE_READY,
    GOO_GOROUTINE_RUNNING,
    GOO_GOROUTINE_BLOCKED,
    GOO_GOROUTINE_DONE
} goo_goroutine_state_t;

// Channel patterns (from AST)
typedef enum {
    GOO_CHANNEL_BASIC,
    GOO_CHANNEL_PUB,
    GOO_CHANNEL_SUB,
    GOO_CHANNEL_REQ,
    GOO_CHANNEL_REP,
    GOO_CHANNEL_PUSH,
    GOO_CHANNEL_PULL
} goo_channel_pattern_t;

// Pattern channel function (defined after enum)
goo_channel_t* goo_make_pattern_chan(goo_channel_pattern_t pattern, size_t elem_size, const char* endpoint);

// Platform-specific includes for structure definitions
#if defined(__APPLE__) && defined(__MACH__)
    #define GOO_PLATFORM_MACOS 1
    #define GOO_PLATFORM_UNIX 1
#elif defined(__linux__)
    #define GOO_PLATFORM_LINUX 1
    #define GOO_PLATFORM_UNIX 1
#elif defined(_WIN32) || defined(_WIN64)
    #define GOO_PLATFORM_WINDOWS 1
#else
    #define GOO_PLATFORM_UNKNOWN 1
    #define GOO_PLATFORM_UNIX 1  // Assume Unix-like
#endif

#ifdef GOO_PLATFORM_UNIX
    #include <pthread.h>
    #include <ucontext.h>
#endif

// Mutex structure
struct goo_mutex {
#ifdef GOO_PLATFORM_UNIX
    pthread_mutex_t mutex;
#endif
    int locked;
};

// Condition variable structure
struct goo_cond {
#ifdef GOO_PLATFORM_UNIX
    pthread_cond_t cond;
#endif
    int dummy;  // Prevent empty struct
};

// Wait group structure
struct goo_waitgroup {
    goo_mutex_t* mutex;
    goo_cond_t* cond;
    int counter;
};

// Goroutine structure (simplified for header)
// Note: Uses ucontext on Unix systems (deprecated on macOS but functional)
struct goo_goroutine {
    goo_goroutine_state_t state;
    void (*function)(void*);
    void* arg;
    void* stack;
    size_t stack_size;
    
#ifdef GOO_PLATFORM_UNIX
    ucontext_t context;
#endif
    
    struct goo_goroutine* next;
    uint64_t id;
    uint64_t creation_time;
    
    // Deadlock detection - what this goroutine is waiting for
    goo_channel_t* waiting_on_channel;
    int waiting_for_send;  // 1 for send, 0 for receive
};

// Subscriber node for pub/sub pattern
struct goo_subscriber {
    goo_channel_t* channel;
    struct goo_subscriber* next;
    int active;
};

// Channel structure
struct goo_channel {
    void* buffer;
    size_t elem_size;
    size_t capacity;
    size_t length;
    size_t head;
    size_t tail;
    
    goo_channel_pattern_t pattern;
    char* endpoint;
    
    goo_mutex_t* mutex;
    goo_cond_t* not_empty;
    goo_cond_t* not_full;
    
    int closed;
    uint64_t id;

    struct goo_goroutine* send_waiters;
    struct goo_goroutine* recv_waiters;

    // Unbuffered (capacity == 0) rendezvous handoff: a sender parks one value in
    // rv_slot (rv_full = 1) and blocks until a receiver copies it out and clears
    // rv_full. Reuses not_empty (receivers wait) and not_full (senders wait).
    void* rv_slot;
    int rv_full;
    
    // Pattern-specific data
    union {
        struct {
            struct goo_subscriber* subscribers;
            size_t subscriber_count;
        } pub_data;
        
        struct {
            goo_channel_t* publisher;
            struct goo_subscriber* sub_node;
        } sub_data;
        
        struct {
            goo_channel_t* paired_channel;
            uint64_t request_id_counter;
        } req_rep_data;
        
        struct {
            goo_channel_t** workers;
            size_t worker_count;
            size_t next_worker_index;
        } push_pull_data;
    } pattern_data;
};

// Runtime statistics structure
typedef struct {
    size_t num_goroutines;
    size_t num_channels;
    size_t scheduler_cycles;
    size_t context_switches;
} goo_runtime_stats_t;

// Scheduler structure (simplified for header)
struct goo_scheduler {
    goo_goroutine_t* ready_queue;
    goo_goroutine_t* current_goroutine;
    
#ifdef GOO_PLATFORM_UNIX
    pthread_t* os_threads;
    ucontext_t main_context;
#endif
    
    int num_threads;
    int running;
    
    goo_mutex_t* scheduler_mutex;
    uint64_t next_goroutine_id;
    uint64_t next_channel_id;
    
    goo_runtime_stats_t stats;
    goo_deadlock_detector_t deadlock_detector;
};

// Synchronization primitives (now just function declarations)
goo_mutex_t* goo_mutex_new(void);
void goo_mutex_free(goo_mutex_t* mutex);
void goo_mutex_lock(goo_mutex_t* mutex);
void goo_mutex_unlock(goo_mutex_t* mutex);
int goo_mutex_try_lock(goo_mutex_t* mutex);

goo_waitgroup_t* goo_waitgroup_new(void);
void goo_waitgroup_free(goo_waitgroup_t* wg);
void goo_waitgroup_add(goo_waitgroup_t* wg, int delta);
void goo_waitgroup_done(goo_waitgroup_t* wg);
void goo_waitgroup_wait(goo_waitgroup_t* wg);

// P4.7 (packages-B, B3): sync.Mutex / sync.WaitGroup shim wrappers
// (src/runtime/sync_shim.c). `slot` is the ADDRESS of the single
// opaque-pointer field of the Goo-visible sync.Mutex / sync.WaitGroup
// struct — codegen passes &receiver (bitcast to void**), NOT a
// goo_mutex_t*/goo_waitgroup_t* itself. Each wrapper lazily allocates the
// real runtime primitive on first use, satisfying Go's zero-value contract
// (`var mu sync.Mutex` is usable immediately, no make/new anywhere) despite
// goo_mutex_t/goo_waitgroup_t requiring real pthread init — see the design
// doc (2026-07-10-p4-packages-b-design.md, section B3) for the full
// rationale and the race-safety argument for the lazy-init scheme.
void goo_sync_mutex_lock(void** slot);
// Go parity: panics "sync: unlock of unlocked mutex" (matching Go's own
// message) when called on a Mutex that isn't currently locked, INCLUDING a
// never-locked zero-value Mutex — see sync_shim.c's doc comment for why the
// check lives here rather than in goo_mutex_unlock itself.
void goo_sync_mutex_unlock(void** slot);
void goo_sync_wg_add(void** slot, int64_t delta);
void goo_sync_wg_done(void** slot);
void goo_sync_wg_wait(void** slot);

// P4.6 (packages-C, C1): time.Sleep / time.Now runtime shim
// (src/runtime/time_shim.c), wrapping the platform primitives (platform.h).
// goo_time_sleep_ns clamps a negative Duration to a no-op sleep (Go: a
// negative or zero Sleep duration returns immediately). goo_time_unix_ns
// reads the WALL clock (CLOCK_REALTIME via goo_platform_wall_time_ns), not
// the monotonic one goo_platform_time_ns exposes internally — see that
// function's doc comment for why UnixNano needs the distinction.
void goo_time_sleep_ns(int64_t ns);
int64_t goo_time_unix_ns(void);
// time.After(d): a capacity-1 channel of time.Time (one int64 of wall-clock
// nanoseconds) that a timer goroutine sends to once d has passed. Capacity 1
// so an abandoned timer never blocks — the usual case is that another select
// arm wins and nobody ever receives. Returns NULL if the goroutine cannot
// start. The channel is not freed, exactly as make(chan T)'s is not.
goo_channel_t* goo_time_after(int64_t ns);

goo_runtime_stats_t goo_get_runtime_stats(void);

// Zero-size allocation sentinel (Go's "zerobase" pattern): a single shared,
// process-lifetime byte whose ADDRESS stands in for the backing pointer of
// any zero-size heap allocation. goo_alloc/goo_arena_alloc return &goo_zerobase
// instead of NULL when size==0, so a zero-size allocation (most visibly the
// backing pointer of an empty-but-non-nil slice literal, `[]T{}`) is never
// mistaken for Go nil. It is never written through (nothing has anywhere to
// write — every consumer's length/cap is 0 too) and never handed to a real
// free()/realloc(): goo_free treats it as a no-op and goo_realloc treats it
// as NULL before calling the real allocator, so it is safe to share across
// every zero-size allocation site without a real per-site allocation. Also
// referenced directly (as an external symbol) by codegen's global-scope
// empty-slice-literal constant path (composite_codegen.c), which builds an
// LLVM constant with no runtime call to route through goo_alloc.
extern unsigned char goo_zerobase;

#endif // RUNTIME_H