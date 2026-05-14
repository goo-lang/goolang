// CompCert compatibility shim.
//
// CompCert (the verified C compiler) is C99 + a subset of C11 + no
// GNU extensions. It rejects C11 atomics (`_Atomic` keyword,
// <stdatomic.h>) outright. The headers in this repo that need
// atomics for the *gcc/clang* build pull this shim in when
// __COMPCERT__ is defined, so the same source tree can be parsed
// by either compiler.
//
// The atomic operations themselves degrade to plain non-atomic
// equivalents under CompCert. That's correct *only* for builds
// that don't actually run concurrent code. The V1 verification
// path targets the compiler binary's translation correctness, not
// the runtime's concurrency guarantees — so this trade-off is
// scoped: ccomp-built binaries from this tree are not safe to use
// with the concurrent runtime modules. The audit (docs/
// COMPCERT_AUDIT.md) flags this.
#ifndef GOO_CCOMP_SHIM_H
#define GOO_CCOMP_SHIM_H

#ifdef __COMPCERT__

// C23 made `bool`/`true`/`false` keywords; CompCert is C99 and only
// has `_Bool`. Pull in stdbool.h's C99 typedef/macros so the source
// tree can use `bool` everywhere without C23.
#include <stdbool.h>

// Strip the _Atomic keyword. Variables become plain storage.
#define _Atomic
#define _Thread_local

// GCC/clang thread-local extension. Same trade-off as _Atomic — the
// ccomp build doesn't intend to run concurrent code, so per-thread
// storage degrades to plain static.
#define __thread

// _Atomic(T) parametrized form — used in 2 sites in shared_variables.h
// to atomic-qualify void* / size_t. Under the shim we drop the
// qualifier (single-thread runtime). Defined as a macro so the
// gcc/clang build (non-CompCert) still sees real C11 atomics.
#define GOO_ATOMIC_PTR    void*
#define GOO_ATOMIC_SIZE_T size_t

// Common atomic typedefs that headers expect.
typedef int                atomic_int;
typedef unsigned int       atomic_uint;
typedef long               atomic_long;
typedef unsigned long      atomic_ulong;
typedef long long          atomic_llong;
typedef unsigned long long atomic_ullong;
typedef _Bool              atomic_bool;
typedef unsigned char      atomic_uchar;
typedef signed char        atomic_schar;
typedef short              atomic_short;
typedef unsigned short     atomic_ushort;

#include <stdint.h>
#include <stddef.h>
typedef int8_t   atomic_int_least8_t;
typedef int16_t  atomic_int_least16_t;
typedef int32_t  atomic_int_least32_t;
typedef int64_t  atomic_int_least64_t;
typedef uint8_t  atomic_uint_least8_t;
typedef uint16_t atomic_uint_least16_t;
typedef uint32_t atomic_uint_least32_t;
typedef uint64_t atomic_uint_least64_t;
typedef int_fast8_t   atomic_int_fast8_t;
typedef int_fast16_t  atomic_int_fast16_t;
typedef int_fast32_t  atomic_int_fast32_t;
typedef int_fast64_t  atomic_int_fast64_t;
typedef uint_fast8_t  atomic_uint_fast8_t;
typedef uint_fast16_t atomic_uint_fast16_t;
typedef uint_fast32_t atomic_uint_fast32_t;
typedef uint_fast64_t atomic_uint_fast64_t;
typedef uint64_t atomic_uint64_t;        // non-standard but used in this codebase
typedef size_t   atomic_size_t;
typedef intptr_t atomic_intptr_t;
typedef uintptr_t atomic_uintptr_t;

// memory_order — define the constants but the ordering itself
// has no effect under the shim.
typedef enum {
    memory_order_relaxed,
    memory_order_consume,
    memory_order_acquire,
    memory_order_release,
    memory_order_acq_rel,
    memory_order_seq_cst
} memory_order;

// Operations degrade to plain reads/writes/arithmetic.
#define atomic_init(obj, val)             ((void)((*(obj)) = (val)))
#define atomic_store(obj, val)            ((void)((*(obj)) = (val)))
#define atomic_store_explicit(obj, v, mo) ((void)((*(obj)) = (v)))
#define atomic_load(obj)                  (*(obj))
#define atomic_load_explicit(obj, mo)     (*(obj))
#define atomic_exchange(obj, val) \
    (__extension_unsupported_in_compcert_shim)
#define atomic_compare_exchange_strong(obj, expected, desired) \
    (__extension_unsupported_in_compcert_shim)
#define atomic_fetch_add(obj, val)        ((*(obj)) += (val), (*(obj)) - (val))
#define atomic_fetch_sub(obj, val)        ((*(obj)) -= (val), (*(obj)) + (val))
#define atomic_fetch_or(obj, val)         ((*(obj)) |= (val), (*(obj)) & ~(val))
#define atomic_fetch_and(obj, val)        ((*(obj)) &= (val), (*(obj)) | (val))
#define atomic_fetch_xor(obj, val)        ((*(obj)) ^= (val), (*(obj)) ^ (val))
#define atomic_flag_test_and_set(obj)     (0)
#define atomic_flag_clear(obj)            ((void)0)

#define ATOMIC_VAR_INIT(value)            (value)
#define ATOMIC_FLAG_INIT                  {0}

typedef struct { _Atomic _Bool _flag; } atomic_flag;

#else

#include <stdatomic.h>
#define GOO_ATOMIC_PTR    _Atomic(void*)
#define GOO_ATOMIC_SIZE_T _Atomic(size_t)

#endif  // __COMPCERT__

#endif  // GOO_CCOMP_SHIM_H
