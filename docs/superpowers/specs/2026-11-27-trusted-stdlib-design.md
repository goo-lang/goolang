# Trusted stdlib design (stdlib strategy option A)

Date: 2026-11-27
Status: design only. No code. Every number below has a command behind it
(Appendix A). Every claim about the compiler cites the code that was opened.

Inputs: the owner's decision of 2026-11-27 ("C now and A as the goal"), the
owner's clarified superset rule (all Go is valid Goo, with designed
exceptions: the reserved Goo keywords, and `unsafe`, `recover()` and `init()`
rejected in user code), the owner's instruction of 2026-11-27 ("take the most
basic go stdlib packages across first, then work on supporting the harder
ones"), ADR 0002, ADR 0005, ADR 0006, and the Go wave 1 plan
(`docs/superpowers/plans/2026-11-27-go-compat-wave-1.md`, branch of the
go-compat-plan worktree).

## 0. Summary

- Goo compiles real Go stdlib source, stage by stage. The first two stages
  need no `unsafe`, no `reflect`, no `runtime` and no `internal/*`. They take
  22 + 10 basic packages from upstream source and lift the API count from 112
  to at most 842 of 8,433 linux/amd64 symbols (1.3% to 10.0%).
- `unsafe` then becomes legal in a closed set of packages: those listed in a
  table compiled into `bin/goo` AND found under the toolchain's own `goostd/`
  directory. No flag, file, comment or environment variable grants trust.
  User code keeps the v1 rejection, with a clear diagnostic instead of today's
  accidental ones.
- ARC stays sound under `unsafe` by two rules. Every `unsafe` result is a
  VIEW of its operand (it carries the operand's taint and is never owned).
  Every conversion of a pointer to `uintptr` PINS the object for the rest of
  the program. Both rules fail toward a leak, never toward a free.
- `reflect` is the largest single item. It needs a real type descriptor
  where Goo has three fields today. It ships in four sub-stages, read-only
  first.
- The 50% goal depends on the denominator. On linux/amd64, `syscall` and
  `debug/elf` hold 44.5% of all symbols. Without them, the full plan reaches
  about 43.5%. The owner must choose the denominator (question Q1).
- A prerequisite defect, found while measuring for this design: `append`
  grows a slice with `realloc` on the old backing array. A full sub-slice
  aborts the program, and an alias of a grown slice reads freed memory
  (valgrind: invalid read). This is a memory-safety defect in SAFE user code
  today, and Go stdlib source depends on the Go semantics everywhere.
  Section 9, risk 1.

## 1. Intent, goals, non-goals, anti-goals

### Intent

Goo compiles the real Go standard library source, so that Go programs that
import it compile unchanged, and so that each new package costs a vendoring
step and a few compiler fixes instead of a hand-written rewrite.

### Goals

1. G1. Compile upstream Go source (Go 1.27.4, the pinned reference) for the
   basic packages first, with no `unsafe`, `reflect`, `runtime` or
   `internal/*` in their import closure.
2. G2. Allow `unsafe` inside a closed, compiled-in set of trusted packages
   under the toolchain's `goostd/`.
3. G3. Keep ARC sound: no `unsafe` construct in trusted code may make the
   release plan free memory that is still reachable.
4. G4. Provide `runtime`, `internal/bytealg`, the atomics and the other
   runtime hooks as C shims behind body-less Go declarations, so that the
   vendored Go source stays unchanged.
5. G5. A route to 50% of the Go stdlib API, measured by one fixed metric and
   held by a ratchet in `make verify-core`.
6. G6. Run the upstream `_test.go` files of each vendored package under
   `goo test`, as the proof that a package works.

### Non-goals

- A Go runtime. Goo keeps its own scheduler, channels and ARC. No garbage
  collector, no stack copying, no Go ABI.
- Assembly. No `.s` file is vendored. Assembly-backed functions get a C shim
  or the upstream generic Go fallback.
- `cgo`, plugins, `syscall/js`, the race detector inside the stdlib
  (`internal/race` becomes a no-op), and `runtime/pprof`.
- Go's GC-related semantics: finalizers (`runtime.SetFinalizer`,
  `runtime.AddCleanup`), `weak`, `unique`'s weak half, and `sync.Pool`
  dropping objects at GC time.
- Other operating systems. The first target is linux/amd64.

### Anti-goals (each one is a defect if it happens)

1. A1. User code gains `unsafe` power by ANY path: a direct import, a
   trusted package that re-exports an `unsafe.Pointer` value or an address as
   `uintptr`, an `internal/` import, `//go:linkname`, a body-less function, a
   cgo-like door (`import "C"`), a local directory named `goostd`, an
   environment variable, a build flag, or a comment marker.
2. A2. A trusted package that frees, through ARC, memory that it still
   holds as `unsafe.Pointer` or `uintptr`.
3. A3. A vendored file that differs from upstream in a way that no ledger
   records.
4. A4. A coverage number that counts a symbol that no test calls.

## 2. The trust boundary

### 2.1 What exists today (opened, and probed)

There is NO `unsafe` rule in the type checker today. Three accidents reject
`unsafe` in user code (probes in Appendix B):

| User code | Today | Why |
|---|---|---|
| `import "unsafe"` (any use, also `_ "unsafe"`) | `goo: error: cannot resolve import "unsafe"` (no position) | no `goostd/unsafe` directory (`src/package/import_resolver.c:266-310`) |
| `unsafe.Sizeof(x)`, `unsafe.Pointer(&x)` in an expression | `syntax error, unexpected 'unsafe'` | `unsafe` is a lexer keyword (`src/lexer/token.c:210`) |
| `unsafe.Pointer` in a type | parses (`unsafe_ptr_type`, `src/parser/parser.y:2890-2903`) | then fails at the import |
| `var unsafe Pointer` (valid Go) | `syntax error, unexpected 'unsafe'` | keyword |
| `unsafe { ... }` (Goo syntax) | `error: Unknown statement type` | parsed (`parser.y:2966`), no checker arm |
| `unsafe func f() int { ... }` (Goo syntax) | `return type mismatch` | the grammar arm drops the result type (`parser.y:556-574`) |
| `import "C"` | `goo: error: cannot resolve import "C"` (no position) | no such directory |
| `func f(x int) int` with no body | `syntax error, unexpected ';'` | grammar needs a block |

The Goo `unsafe` block and `unsafe func` syntax is therefore dead: it cannot
compile a correct program today. `codegen_generate_unsafe_stmt`
(`src/codegen/statement_codegen.c:4678-4691`) is a transparent pass-through
that no program reaches.

### 2.2 Options for "how does the compiler know a package is trusted"

| Option | Pros | Cons |
|---|---|---|
| A. Path rule: under the GOOROOT `goostd/` | no new file; matches Go's GOROOT model | `$GOOROOT` is user-settable; `./goostd` cwd fallback (`import_resolver.c:71`) lets a project ship its own `goostd/`; the six test fixtures in `goostd/` (`cpkg`, `fwdref`, `kinds`, `mypkg`, `pkgcheck`, `shapes`) become trusted |
| B. Marker in the package (comment or file) | visible in the source | any user can write the marker into their own package |
| C. Build flag (`-trusted=pkg`) | explicit | any user can pass the flag; violates A1 directly |
| D. Table compiled into `bin/goo`, plus a root check | the user cannot edit it without rebuilding the compiler; fixtures stay out; easy to gate | one more table to keep in step (a gate does that) |

**Recommendation: D.** A package is trusted when all four conditions hold:

1. T1. The package was found by the GOOROOT tier of `resolve_import`
   (`import_resolver.c:296-299`), or it is the entry directory of `goo test`
   or `goo build`. The `./name` tier, the source-directory fallback
   (`:303-307`) and any future fetched-module tier (ADR 0006, "What
   sub-project B will cost", item 1) never confer trust.
2. T2. The trusted ROOT is `realpath` of the EXE-RELATIVE `goostd`: the
   installed layout `<exe-dir>/../lib/goostd` or the dev layout
   `<exe-dir>/../goostd` (`import_resolver.c:61-64`). It is never `$GOOROOT`
   by itself and never the cwd fallback `./goostd`. `$GOOROOT` still selects
   where packages RESOLVE. A package resolved under it is trusted only when
   `realpath(GOOROOT/goostd)` equals the trusted root. So the dev workflow
   `GOOROOT=$PWD ./bin/goo` keeps working, and a project that sets
   `GOOROOT=./myproject` gets no trust.
3. T3. `realpath(package dir)` equals `realpath(root + "/" + import path)`.
   This refuses a symbolic link that leaves the root.
4. T4. The import path is in the compiled-in table
   (`src/package/trusted_packages.c`, new). The six fixture packages are not
   in it. A gate holds the table equal to the set of `goostd/` packages that
   import `unsafe` (section 7, gate T-2).

Trust is a property of the whole package, all of its files, its `_test`
files under `goo test` included.

The threat model is user CODE, not the person at the keyboard. A person who
edits the toolchain's own `goostd/` edits the compiler, exactly as a person
who edits `$GOROOT/src` edits Go. Question Q3 asks the owner to confirm this.

### 2.3 What an untrusted package may not do

Each rule names the anti-goal path it closes.

| Rule | Closes |
|---|---|
| U-import: no `import "unsafe"` | direct import |
| U-surface: no reference to an exported symbol of a trusted package whose type names `unsafe.Pointer`, or which takes a raw address as `uintptr` | re-export. Measured on Go 1.27.4: 9 symbols name `unsafe.Pointer` (`reflect.NewAt`, `reflect.SliceAt`, `reflect.Value.SetPointer`, `reflect.Value.UnsafePointer`, `runtime.SetCgoTraceback`, `sync/atomic.{CompareAndSwap,Load,Store,Swap}Pointer`), and 4 take raw addresses (`syscall.Syscall`, `Syscall6`, `RawSyscall`, `RawSyscall6`). The checker computes the first set from types, so a new trusted package cannot leak by omission. The second set is a hand list. |
| U-convert: no conversion from or to `unsafe.Pointer`, by TYPE, whatever the spelling | a value of type `unsafe.Pointer` that arrives by inference (`p := v.UnsafePointer()` then `(*int)(p)`). With U-surface no such value can arrive; this rule is the second wall. |
| U-internal: Go's rule. A path with an `internal` element resolves only for a package rooted at the parent of `internal`. A bare `internal/...` path resolves ONLY in the trusted root, never in the source-directory fallback. | `internal/bytealg.MakeNoZero` (uninitialised memory), `internal/abi.NoEscape` |
| U-cgo: no `import "C"` | cgo-like door |
| U-body: no body-less function declaration | a declaration that would bind to a runtime C symbol |
| U-linkname: no `//go:linkname` | Go's own rule: linkname needs `import "unsafe"` |

`reflect.Value.Pointer()` and `reflect.Value.UnsafeAddr()` return `uintptr`.
They stay allowed: an integer gives no power without a way back to a pointer,
and U-convert, U-surface and U-cgo close every way back.

### 2.4 The `unsafe` keyword

Trusted Go source writes `unsafe.Pointer(p)` and `unsafe.Sizeof(x)` in
expressions, and 466 of the 2,019 public stdlib files use the identifier
`unsafe` (9,293 uses). The lexer keyword must therefore stop eating it.

| Option | Pros | Cons |
|---|---|---|
| R1. Retire the `unsafe` keyword. `unsafe` becomes an ordinary identifier; `unsafe.Pointer` parses as a qualified type and `unsafe.Sizeof(x)` as a call on a package selector | Go spells it this way; `var unsafe Pointer` becomes valid Goo (a superset gain); the rejection moves to ONE checker rule with a positioned diagnostic; deletes dead syntax (2.1) | removes ADR 0002's idea of a named `unsafe { }` block. The `@` attribute decision (vision decision 5b) gives a better spelling later, for example `@unsafe { }` |
| R2. Keep the keyword; add a grammar arm `UNSAFE DOT identifier` as an operand | keeps the Goo block syntax | keeps a keyword that Go does not have; `var unsafe T` stays a superset violation; one more workaround arm |
| R3. Lexer lookahead: emit an identifier when `.` follows | no grammar change | a context-sensitive lexer rule, the class of hack the grammar skill warns about |

**Recommendation: R1.** It is a grammar change, so it follows the
goo-grammar skill: `scripts/grammar-tripwire.sh` before and after (baseline
31 S/R, 0 R/R), and the ledger procedure for any delta. Removing three
productions is expected to remove conflicts, not add them; the bison run
decides. Question Q2.

### 2.5 Diagnostics (project format, `diag_emit`)

Each message is a new row in `catalogue/diagnostics.tsv`. None has a
`help:` line, because no paste-ready fix exists (the project rule).

```
main.go:5:2: error: package unsafe is reserved for the trusted standard library
    5 | 	"unsafe"
      | 	^
```

```
main.go:9:7: error: reflect.Value.UnsafePointer uses unsafe.Pointer, which is reserved for the trusted standard library
    9 | 	p := v.UnsafePointer()
      | 	     ^
```

```
main.go:9:2: error: syscall.Syscall takes a raw address as uintptr and is reserved for the trusted standard library
```

```
main.go:12:9: error: conversion from unsafe.Pointer to *int is reserved for the trusted standard library
```

```
main.go:4:2: error: use of internal package internal/bytealg not allowed
```
(Go's wording, verbatim.)

```
main.go:3:8: error: import "C" (cgo) is not supported
```

```
main.go:5:6: error: missing function body
```
(Go's wording, verbatim.)

```
main.go:7:1: error: //go:linkname only allowed in Go files that import "unsafe"
```
(Go's wording, verbatim. Goo ignores comments today, so a user
`//go:linkname` has no effect; the diagnostic makes the rule visible.)

A trusted-path package found outside the trusted root (T2 fails) is an
untrusted package. It gets the first diagnostic at its own `import "unsafe"`
line, with one more sentence:

```
/home/u/proj/goostd/strings/builder.go:9:2: error: package unsafe is reserved for the trusted standard library; this copy of strings is outside the toolchain's goostd (/opt/goo/lib/goostd)
```

With R1, `unsafe { ... }` in user code is an ordinary syntax error at the
`{` (Go also rejects it), and `unsafe func` is a syntax error at `func`.

## 3. `unsafe` semantics under ARC

### 3.1 What ARC is today (opened)

- Every `goo_alloc` object has a 16-byte header `{ rc, reserved }` in front
  of the payload (`src/runtime/runtime.c:53-97`, `include/runtime.h:58-108`).
  NULL, `goo_zerobase` and IMMORTAL literals are headerless or never freed.
  An arena pointer has no header and is excluded only by static proof.
- **Codegen emits no `goo_retain` anywhere.** The only mention in
  `src/codegen/` is a comment (`composite_codegen.c:1524`). So the count of a
  live object stays 1, and an object is freed by ONE static release that the
  release plan places at function exit, iteration end or reassignment.
- The release plan frees a local only when it is non-escaping, OWNED by the
  binding-site table, not arena-routed, and not held by a longer-lived local
  (`include/release_decision.h`, conditions 1-6). "Anything not listed" in
  the binding table is not owned.
- `returns_fresh` needs a POSITIVE proof that every return path gives fresh
  memory. Every shape that it does not recognise is borrowed
  (`src/types/returns_fresh.c:33-52`).
- Taint flows through binary expressions (`escape_core.c:685-691`), so
  `uintptr(p) + 8` keeps the taint of `p` if the conversion passes it on.
- The escape passes run on ONE FILE at a time. `src/compiler/goo.c:1147`
  calls `codegen_generate_program` for each file of a package, and
  `codegen.c:380` runs `param_escape_analyze` on that file. A call to a
  function of another file or another package is opaque: its arguments get
  `ESCAPE_REASON_CALL_OPAQUE` and its result is borrowed. This is sound, and
  it is the reason that most cross-package `unsafe` cases are already safe.

So the danger is not "a count falls to zero too early". The danger is "the
static release frees a local whose memory `unsafe` code still reaches
through a path that the taint analysis cannot see".

### 3.2 The rules

Each rule applies to trusted code. Untrusted code cannot reach the
constructs (section 2.3).

- **V (view).** The result of `unsafe.Pointer(p)` for a pointer `p`,
  `(*T)(u)` and `uintptr`-free pointer conversions, `unsafe.Add`,
  `unsafe.Slice`, `unsafe.String`, `unsafe.SliceData` and `unsafe.StringData`
  carries the taint of its pointer operand, exactly as `s[1:]` does today. At
  a binding site it is NOT owned. In `returns_fresh` it is borrowed. The
  contrast is the point: `string(b)` COPIES in Goo and is fresh;
  `unsafe.String(&b[0], len(b))` ALIASES and is never fresh.
- **P (pin).** A conversion `uintptr(u)` where `u` has type `unsafe.Pointer`
  marks every slot in the taint of `u` as escaping with a new reason bit,
  `ESCAPE_REASON_UNSAFE_PIN` (bit 13, which moves `ESCAPE_REASON_ALL` in
  `include/escape_core.h:211`). It is a no-evidence reason in ADR 0005's
  sense, like `CALL_OPAQUE`: it records that the analysis stops looking. No
  later pass may narrow it. The object lives until the program ends.
- **I (integer origin).** `unsafe.Pointer(x)` for an integer `x` has an
  EMPTY taint (origin unknown) and is never owned. Codegen emits no
  `goo_release` for any local whose type is `unsafe.Pointer` or `uintptr`,
  nor for a local bound to a value that rule V or rule I produced.
- **L (layout).** `unsafe.Sizeof`, `Offsetof` and `Alignof` are constants
  from Goo's own LLVM layout, not from Go's. Goo's layouts: string
  `{ptr, len}` (16 bytes), slice `{ptr, len, cap}` (24), interface
  `{vtable, data}` (16, `interface_codegen.c:10-12`), `?T` `{i1, T}`
  (`nullable_codegen.c:15`). Go code that reads an interface's data word as
  `(*[2]unsafe.Pointer)(unsafe.Pointer(&x))[1]` (`sync/pool.go:94`) works,
  because the data word is second in both layouts. Go code that reads
  `internal/abi.Type` does not work, and is replaced (section 5).
- **S (shim retention).** A C shim parameter of type `unsafe.Pointer`,
  `*unsafe.Pointer` or any pointer is retaining unless an audited row says
  otherwise. The default for an unknown callee is already `CALL_OPAQUE`;
  this rule forbids a "non-retaining" audit for a parameter that the C body
  stores. A shim result of pointer type is borrowed unless audited as fresh.
- **H (headerless memory).** Memory that a C shim gets from outside
  `goo_alloc` (mmap, `environ`, a C string) is copied into `goo_alloc`
  memory before it becomes a Goo value, or it stays behind rule I. It must
  never reach `goo_release` or `goo_realloc`.
- **X (cross-package stays opaque).** A call into a trusted package keeps
  `CALL_OPAQUE` for its arguments and a borrowed result, and `block_escape`
  keeps routing an arena site that such a call receives to the heap. If a
  later change adds cross-file or cross-package summaries, the summary of a
  trusted function must be computed with rules V, P, I and S.
- **O (override the escape-analysis lies).** Where Go source uses `unsafe`
  ONLY to hide an escape from Go's compiler, the vendored package carries a
  Goo override file without the trick (the `*_goo.go` convention of
  `docs/superpowers/specs/2026-07-01-stdlib-imports-design.md`, Phase 2). In
  the 11 core packages this is one site: `strings/builder.go:39`,
  `b.addr = (*Builder)(abi.NoEscape(unsafe.Pointer(b)))`. `internal/abi.NoEscape`
  is `unsafe.Pointer(uintptr(p) ^ 0)` (`internal/abi/escape.go:19-21`):
  under rule P it pins the Builder for life.

### 3.3 Each case where ARC can free memory that trusted code still uses

| # | Case (concrete stdlib site) | Without the rules | Rule |
|---|---|---|---|
| C1 | A reference held only as an integer: `uintptr` in a field, a global, or the kernel (an epoll `Data` word in `internal/poll`) | the integer carries no taint into a sink; the release frees the object at scope end | P |
| C2 | A zero-copy view treated as a copy: `stringslite.Clone` and `errors.Join(...).Error()` both end in `return unsafe.String(&b[0], len(b))` (`internal/stringslite/strings.go:121`, `errors/join.go:58`); `strings.Builder.String` returns `unsafe.String(unsafe.SliceData(b.buf), len(b.buf))` (`strings/builder.go:47`) | if `unsafe.String` were modelled like `string(b)` (fresh), the caller frees the bytes while the Builder still appends to them, and the callee also frees `b` | V |
| C3 | An interior pointer from `unsafe.Add` or `uintptr` arithmetic (`sync/pool.go:302`, `indexLocal`) reaches `goo_release` | `free(ptr - 16)` on a pointer that `malloc` never returned | V, I |
| C4 | Publication through an atomic: `atomic.StorePointer(&p.local, unsafe.Pointer(&local[0]))` (`sync/pool.go:243`) | a "non-retaining" audit of the shim lets `local` be released at function exit while `p.local` points at it | S |
| C5 | The escape-analysis lie `abi.NoEscape` (C1 in disguise) | hides the self-pointer from any flow analysis | P, O |
| C6 | `unsafe.Slice((*byte)(unsafe.Pointer(&x)), n)` over a stack local, returned | the stack slot is reused after return | V: the taint reaches the return, `local_escape` marks `x`, and the address-taken promotion (`function_codegen.c:13-30`) moves `x` to the heap |
| C7 | Headerless memory (C, arena, literal) wrapped as a Goo slice and later grown by `append` | `goo_slice_append` calls `goo_realloc(slice->data, ...)` (`runtime.c:1573`), which computes `data - 16` | H, I, and the stage 0 `append` fix (risk 1) |
| C8 | An arena object passed to trusted code that stores its address | the arena frees the block at `}` | X |
| C9 | A finalizer or cleanup that runs on release while a `uintptr` holder exists (`os` calls `runtime.SetFinalizer` and `runtime.AddCleanup`) | the finalizer closes a file that is still in use | finalizers are no-ops (Q7) |
| C10 | A `reflect.Value` that points into a local (`reflect.ValueOf(&x)`) | the local is released while the Value lives | X: the call to `reflect.ValueOf` is cross-package, so `x` gets `CALL_OPAQUE` |

### 3.4 What the rules cost

Rules V, P and I fail toward a LEAK. The measured cost is unknown until
stage 3. The predicted costs: every `strings.Builder` escapes under the
upstream source (the self-pointer is a field store, a sink), so the
override in rule O is necessary, not optional; `strings.Clone` and
`errors.Join().Error()` results are borrowed, so each call leaks its bytes.
Gate T-7 (section 7) measures this on `bench/daemon/daemon.goo` by block
count, the method of ADR 0005's measurements. A later precision step can
prove `unsafe.String(&b[0], n)` fresh when `b` is a fresh local and the
pointer is its base. That step needs its own measurement first.

## 4. The runtime boundary

### 4.1 What the core packages import (measured, linux/amd64 file selection, no tests)

Tool: `go/build` with `GOOS=linux GOARCH=amd64`, every selector on an
imported low-level package, every body-less function and every
`//go:linkname` (Appendix A, item 5).

| Package (files) | `runtime`, `internal/*`, `sync/atomic`, `unsafe`, `syscall` symbols used | Body-less functions |
|---|---|---|
| `strings` (8) | `internal/abi`: NoEscape. `internal/bytealg`: CompareString CountString Cutover IndexString LastIndexByteString LastIndexRabinKarp MakeNoZero MaxBruteForce. `internal/stringslite`: Clone Cut CutPrefix CutSuffix HasPrefix HasSuffix Index IndexByte TrimPrefix TrimSuffix. `iter`: Seq. `unsafe`: Pointer SliceData String | none |
| `strconv` (5) | `internal/bytealg`: IndexByteString. `internal/strconv`: the whole implementation (AppendBool ... Itoa, 21 symbols). `internal/stringslite`: Clone | none |
| `bytes` (4) | `internal/bytealg`: Compare Count Cutover Index IndexByte IndexByteString IndexRabinKarp IndexString LastIndexByte LastIndexRabinKarp MakeNoZero MaxBruteForce MaxLen. `iter`: Seq. `unsafe` (blank import for linkname) | none; `//go:linkname Repeat` |
| `sort` (5) | `internal/reflectlite`: Swapper ValueOf | none |
| `fmt` (5) | `internal/fmtsort`: Sort. `internal/stringslite`: IndexByte. `reflect`: 32 symbols (Kind constants, TypeOf, ValueOf, MakeSlice, Value) | none |
| `os` (45) | `internal/poll` (9), `internal/syscall/unix` (24), `internal/byteorder` (6), `internal/filepathlite` (4), `internal/testlog` (5), `internal/bytealg` (2), `internal/strconv` (2), `internal/stringslite` (2), `internal/goarch`, `internal/syscall/execenv`; `runtime`: AddCleanup Cleanup GOOS KeepAlive SetFinalizer; `sync/atomic`: Int32 Pointer Uint32; `unsafe`: Offsetof Sizeof Slice StringData; `syscall`: 124 symbols | checkClonePidfd ignoreSIGSYS restoreSIGSYS runtime_args runtime_beforeExit runtime_rand sigpipe |
| `time` (10) | `internal/bytealg`: IndexByte. `internal/stringslite`: Clone. `runtime`: GOOS GOROOT. `syscall`: 10 symbols. `unsafe`: Pointer | Sleep newTimer now resetTimer runtimeIsBubbled runtimeNano runtimeNow stopTimer |
| `sync` (11) | `internal/race` (8), `internal/runtime/atomic`: LoadAcquintptr StoreReluintptr, `internal/sync`: HashTrieMap Mutex, `internal/synctest` (7); `runtime`: GOMAXPROCS; `sync/atomic`: Bool CompareAndSwapUintptr Int32 LoadPointer LoadUintptr Pointer StorePointer StoreUintptr Uint64; `unsafe`: Pointer Sizeof | fatal runtime_Semacquire runtime_SemacquireRWMutex runtime_SemacquireRWMutexR runtime_SemacquireWaitGroup runtime_Semrelease runtime_notifyListAdd runtime_notifyListCheck runtime_notifyListNotifyAll runtime_notifyListNotifyOne runtime_notifyListWait runtime_procPin runtime_procUnpin runtime_randn runtime_registerPoolCleanup throw |
| `errors` (3) | `internal/reflectlite`: Interface Ptr Type TypeOf Value ValueOf. `unsafe`: String | none |
| `io` (3) | none (imports `errors` and `sync` only) | none |
| `unicode/utf8` (1) | none | none |

The internal packages that these need, and their own imports:

| Package | Files (Go, asm) | Imports |
|---|---|---|
| `internal/bytealg` | 9, 5 | `internal/cpu`, `unsafe` |
| `internal/stringslite` | 1, 0 | `internal/bytealg`, `unsafe` |
| `internal/strconv` | 11, 0 | `math/bits`, `unsafe` (float bit casts, `internal/strconv/deps.go:13-16`) |
| `internal/abi` | 14, 2 | `internal/goarch`, `unsafe` |
| `internal/byteorder` | 1, 0 | none |
| `internal/goarch` | 3, 0 | none |
| `internal/cpu` | 4, 2 | `unsafe` |
| `internal/fmtsort` | 1, 0 | `cmp`, `reflect`, `slices` |
| `internal/reflectlite` | 3, 1 | `internal/abi`, `internal/goarch`, `internal/unsafeheader`, `runtime`, `unsafe` |
| `internal/sync` | 3, 0 | `internal/abi`, `internal/goarch`, `internal/race`, `sync/atomic`, `unsafe` |
| `internal/race` | 2, 0 | `internal/abi`, `unsafe` |
| `internal/synctest` | 1, 0 | `internal/abi`, `unsafe` |
| `internal/runtime/atomic` | 8, 1 | `internal/goarch`, `unsafe` |
| `internal/filepathlite` | 3, 0 | `errors`, `internal/bytealg`, `internal/stringslite`, `io/fs`, `slices` |
| `internal/poll` | 24, 0 | `errors`, `internal/strconv`, `internal/syscall/unix`, `io`, `runtime`, `sync`, `sync/atomic`, `syscall`, `time`, `unsafe` |
| `internal/testlog` | 2, 0 | `sync`, `sync/atomic`, `unsafe` |
| `internal/syscall/unix` | 26, 0 | `internal/strconv`, `runtime`, `sync/atomic`, `syscall`, `unsafe` |
| `iter` | 1, 0 | `internal/race`, `runtime`, `unsafe` (only `Pull` needs them; `Seq` and `Seq2` are plain generic types) |

### 4.2 One binding mechanism: a body-less declaration is a C symbol

Today the shim table (`include/shim_signatures.h`) describes parameters with
seven scalar tags (`string`, `int64`, `float64`, `[]string`, `error`, two QIR
types). It cannot describe `*uint32`, `[]byte`, `unsafe.Pointer` or a
generic. The Go source already declares every runtime hook as a body-less
function with its full Go type.

| Option | Pros | Cons |
|---|---|---|
| E1. A body-less function in a TRUSTED package binds to the C symbol `goo_ts_<pkg>_<name>` (package path mangled, for example `goo_ts_sync_runtime_Semacquire`), implemented under `src/runtime/trusted/` | the Go declaration is the signature, so nothing is written twice; the vendored file stays unchanged; escape treatment is the existing opaque-callee rule | needs a link-time gate that every body-less function has a C definition |
| E2. Grow the shim table with pointer and slice parameter tags | one table for all shims | every signature is written twice (Go source and C table); the table grows with each shape |
| E3. Rewrite each hook package in Goo source | no C | the hooks ARE the runtime (semaphores, timers); Goo source cannot reach the scheduler |

**Recommendation: E1.** It is a deep module: one rule, any number of hooks.
`//go:linkname` in a trusted file is accepted and has no effect, because the
binding comes from the rule, not from the comment. Gate T-6 checks the
symbols. An audited list in C can mark a hook as non-retaining or as
returning fresh memory (`bytealg.MakeNoZero` allocates), under the same
trust rule as the shim table's `non_retaining` column.

### 4.3 Which symbols are C shims and which are Goo source

| Symbol group | Provided as | Reason |
|---|---|---|
| `internal/bytealg` IndexByte, IndexByteString, Count, CountString, Compare, CompareString, Equal | C shim over `memchr`/`memcmp` | Go uses assembly here; libc is as fast and is already linked |
| `internal/bytealg` Index, IndexString, IndexRabinKarp, LastIndexRabinKarp, Cutover, MaxLen, MaxBruteForce | upstream generic Go files (`*_generic.go`) | pure Go fallbacks exist for arches without assembly |
| `internal/bytealg.MakeNoZero` | C shim: `goo_alloc` without the zero fill, audited as fresh | Go has no Go fallback; callers overwrite before they read |
| `sync/atomic`, `internal/runtime/atomic` | first C shims over `__atomic` builtins, later compiler intrinsics | C first: correct, small, testable. Intrinsics later remove one call per operation, when a measurement asks for it |
| `sync` runtime hooks (Semacquire, Semrelease, notifyList*, procPin, randn, registerPoolCleanup, throw, fatal) | C shims over the existing scheduler (`src/runtime/concurrency.c`, `channels.c`) | they park goroutines; see the parking plan for the waiter model |
| `time` hooks (now, runtimeNano, runtimeNow, Sleep, newTimer, stopTimer, resetTimer, runtimeIsBubbled) | C shims; timers over the existing scheduler | runtime-owned |
| `os` hooks (runtime_args, runtime_beforeExit, runtime_rand, sigpipe, the SIGSYS pair, checkClonePidfd) | C shims | process state lives in the C runtime |
| `runtime.GOOS`, `GOROOT`, `GOMAXPROCS`, `KeepAlive` | Goo source in a small trusted `runtime` package; `KeepAlive` is a use with no escape | constants and one no-op |
| `runtime.SetFinalizer`, `runtime.AddCleanup` | no-op (Q7) | no GC; section 3.3 C9 |
| `internal/race`, `internal/synctest`, `internal/testlog` | Goo source stubs: race `Enabled = false`, no bubbles, no test log | features out of scope |
| `internal/stringslite`, `internal/strconv`, `internal/byteorder`, `internal/goarch`, `internal/filepathlite` | upstream Go source | pure Go plus `unsafe` |
| `internal/cpu` | Goo source: every feature flag false | selects the generic paths |
| `internal/abi.NoEscape` | upstream source, with rule O overrides at its callers | section 3.2 |
| `internal/reflectlite`, `reflect` | Goo source with Go's exported API | upstream reads `internal/abi.Type` and calls into the Go runtime (section 5) |
| `internal/sync.HashTrieMap` (for `sync.Map`) | upstream source, late | needs generic types, `atomic.Pointer[T]` and a hash function per type |
| `syscall`, `internal/syscall/unix`, `internal/poll` | upstream Go source over a C shim for the raw `Syscall`/`Syscall6`/`RawSyscall` entry points (libc `syscall(2)`); `internal/poll` needs a netpoll hook into the scheduler | stage 5 |
| `sync.Pool` | Goo override: a mutex-protected free list | the upstream per-P design relies on `procPin` and GC-time cleanup |

## 5. `reflect`

### 5.1 What `reflect` needs, against what Goo has

| Needed | Goo today (opened) |
|---|---|
| One descriptor per type: kind, size, align, name, package path, element, key, length, fields (name, offset, type, tag, embedded, exported), methods (name, type, function), comparable flag, hash | `goo.typedesc.<pkg>__<T>`: three fields `{eq_fn, type_name, fmt_fn}` behind vtable slot 0 (`interface_codegen.c:658-700`), name-deduped, so it is unique per type |
| The concrete type of an interface value | reachable: interface `{vtable, data}`, slot 0 is the descriptor |
| Type identity independent of the interface | vtables are per (concrete, interface) pair and asserts compare vtable pointers (`interface_codegen.c:733`); `reflect.TypeOf` must compare DESCRIPTORS instead |
| Struct tags | not parsed: 3 of the measured files fail first on a tag (`crypto/x509/pkix`, `net/rpc/jsonrpc`) |
| Method values callable from a `Value` | thunks exist per (type, interface) pair only; no per-type method table |
| A dynamic call with any signature (`Value.Call`, `MakeFunc`) | nothing |

The upstream `reflect` (8,798 lines, linux/amd64) and `internal/reflectlite`
(1,187 lines) read Go's `internal/abi.Type` (1,670 lines) and call into the
Go runtime by linkname (map access, `unsafe_New`, `typedmemmove`, channel
operations). Neither can be vendored as-is. The plan is Goo source that
implements Go's exported API (76 top-level symbols and 90 methods in
Go 1.27.4's API files) over a widened Goo descriptor, with the upstream pure
parts (`Kind.String`, `StructTag.Get`/`Lookup`, `DeepEqual`) vendored and
the upstream tests run where they compile.

### 5.2 `reflect` and ARC

- A `Value` never owns. Its data pointer is a view (rule V) of the
  interface box or of the address that it came from.
- `reflect.New`, `MakeSlice`, `MakeMap` and `Append` allocate with
  `goo_alloc` (header present) and return a borrowed result, because the
  call is cross-package (rule X). They leak, never dangle.
- `Value.Set` stores through a pointer. The stored value arrives as an
  interface argument of a cross-package call, so it already escapes with
  `CALL_OPAQUE`.
- Interface boxes are heap copies today (`interface_codegen.c:10-12`) and
  mostly never released (ADR 0002: 11 of 13 alloc sites pass a NULL site).
  If a later change releases boxes, every `Value` made from a box keeps the
  box alive by rule V, and that change must prove it.

### 5.3 `!T` and `?T`

Measured today: a program can box `!int` and `?int` into `interface{}`, and
`fmt.Println` then prints the type name (`!int64`, `?int64`) for a present
AND an absent value. `%T` is not supported. That is a Goo defect regardless
of this design.

| Option for `reflect` | Pros | Cons |
|---|---|---|
| K1. `!T` cannot be converted to an interface (a compile error that says to use `try` or `catch` first). `?T` boxes as its `T` when present and as a nil interface when absent | Go code sees only Go kinds; `encoding/json` gives `null` or the value; no new Kind | loses the difference between "absent `?T`" and "nil interface" inside the interface |
| K2. New kinds `ErrorUnion` and `Nullable` after Go's last Kind | exact | Go code with an exhaustive Kind switch (`fmt`, `encoding/json`) falls to its default arm; every vendored switch needs review |
| K3. `?T` as Kind `Pointer` with `Elem() == T` | close to JSON semantics | the representation `{i1, T}` is not a pointer, so `Elem().Set` and `Pointer()` lie |

**Recommendation: K1** (question Q6). It keeps `reflect` a Go API.

### 5.4 Staged plan, and how large it is

| Sub-stage | Scope | Unblocks |
|---|---|---|
| R1 reflectlite | TypeOf, ValueOf, Kind, Elem, Len, Index, Swapper, Interface, Implements/AssignableTo; descriptor gains kind, size, element, length and an interface method-name table | `errors.As`/`Is` (real source), `sort.Slice`/`SliceStable` |
| R2 read-only reflect | Type: Name, String, PkgPath, Elem, Key, Len, NumField, Field (with Tag), NumMethod; Value: Bool, Int, Uint, Float, String, Len, Index, MapRange, MapKeys, Field, Elem, IsNil, IsZero, Interface, CanInterface; DeepEqual | real `fmt` (with `internal/fmtsort`), `encoding/json` Marshal, `encoding/binary` Write and Size, `text/template` reads, `flag` |
| R3 writes | Set*, New, MakeSlice, MakeMap, Append, Addr, settability rules | `encoding/json` Unmarshal, `encoding/binary` Read, `encoding/gob` |
| R4 calls | Method, Call, MakeFunc, FuncOf, StructOf | `text/template` method calls, `net/rpc` |

Honest size: R1 is the size of one arc of the wave 1 kind. R2 is larger
than the ARC 1 arc (PRs #258 to #286), because the descriptor change
touches every type that codegen emits, and every vendored consumer brings
its own gaps. R4 needs a call trampoline for any signature with no libffi
(no new install), so codegen must emit a call stub per method signature.
R4 is the largest item in this design, and it is last on purpose.

## 6. The order of packages (owner instruction: the basic packages first)

All counts are on the linux/amd64 denominator (8,433 symbols, Q1). Each
"after" figure is an UPPER bound: it counts every symbol of each listed
package as if the whole package compiles. Today's base is 112 symbols
(`scripts/stdlib-coverage.py` on Go 1.27.4).

| Stage | What it adds | Packages | +symbols | After (bound) |
|---|---|---|---|---|
| 0 | prerequisites: the `append` fix (risk 1); one coverage metric (Q1); the vendoring tool and ledger (7, T-3) | none | 0 | 112 (1.3%) |
| 1 | basic packages from upstream source. Import closure inside {each other, the packages Goo provides today}. No trust boundary. | bufio, cmp, compress/lzw, container/heap, container/list, container/ring, crypto, encoding, encoding/ascii85, encoding/csv, go/build/constraint, hash, html, image/color, image/color/palette, math/cmplx, mime/quotedprintable, structs, text/scanner, text/tabwriter, unicode, unicode/utf16 (22) | +532 | 644 (7.6%) |
| 2 | generics with type sets, then `slices`, `maps`, `iter` (Seq only) and what they unblock. `slices` imports `unsafe` for one function (`overlaps`); it gets a rule O override until stage 3. | compress/bzip2, compress/flate, encoding/base32, encoding/hex, go/doc/comment, iter, maps, regexp, regexp/syntax, slices (10) | +198 | 842 (10.0%) |
| 3 | the trust boundary (2), the ARC rules (3), body-less hooks (4.2), `internal/bytealg`, `stringslite`, `strconv`, `byteorder`, `cpu`, `goarch`. Real source replaces today's rewrites. | strings, bytes, strconv, math, math/bits, unicode/utf8, io, path, path/filepath, encoding/base64, hash/crc32, hash/adler32, hash/crc64, hash/fnv, hash/maphash, sync/atomic, math/rand, math/rand/v2, unique | +478 | 1,320 (15.7%) |
| 4 | reflect R1 to R3 (5.4) and its consumers | errors, sort, fmt, encoding/binary, debug/dwarf, encoding/json, encoding/json/jsontext, encoding/xml, encoding/gob, flag, log, go/token, go/scanner, go/ast, text/template, text/template/parse, html/template, encoding/pem, encoding/asn1, mime, testing/iotest, compress/gzip, compress/zlib, io/ioutil, math/big | +956 | 2,276 (27.0%) |
| 5 | runtime hooks, `syscall`, `internal/poll`, `os` | sync, time, context, os, os/exec, os/signal, os/user, io/fs, syscall, runtime, runtime/debug, log/slog, log/syslog, net, net/netip, net/url, net/textproto, net/http, net/mail, net/smtp, debug/elf, debug/macho, debug/pe, archive/tar, archive/zip, crypto/tls, crypto/x509, crypto/sha256, crypto/rand, testing | +5,148 | 7,424 (88.0%) |

The measured facts that the stages rest on:

- 176 public packages on linux/amd64 (`go list std`, without `unsafe`,
  `runtime`, `reflect`, `sync/atomic`, `internal`, `vendor`). 54 import none
  of `unsafe`, `reflect`, `runtime`, `sync/atomic` or any `internal` path
  DIRECTLY (1,227 symbols). Only 10 are clean in the TRANSITIVE closure
  (387 symbols): cmp, container/list, container/ring, encoding, image/color,
  image/color/palette, structs, unicode, unicode/utf16, unicode/utf8.
- Stage 1 is the fixpoint "admit a direct-clean package when all its
  imports are admitted", seeded with the 14 packages that Goo provides
  today (fmt, os, math, errors, sync, time as shims; strings, strconv,
  bytes, sort, io, unicode/utf8, math/bits, path/filepath as goostd source).
  Stage 2 adds `slices`, `maps` and `iter` to the seed.
- In the stage 2 remainder, `slices` blocks 14 packages, `bufio` 11 (it
  joins in stage 1), `encoding/binary` 7 (it needs `reflect`, so stage 4),
  `net` 5.
- Parse state today, linux/amd64 file selection of the core, stage 1,
  stage 2 and support packages: 155 of 318 files parse (48.7%). Of the
  stage 1 packages, only container/list, container/ring, encoding, structs,
  crypto and encoding/csv parse fully. The first error of each other file is
  in section 8. A separate agent measures which stage 1 packages COMPILE
  today and what blocks the rest; its result refines this stage.
- Stage 5 crosses 50% (4,217 symbols) only because of `syscall` (2,186)
  and `debug/elf` (1,567), 44.5% of the denominator together. Without them
  the plan ends near 3,671 (43.5%). The "application" subset (everything
  except `syscall`, `debug/*`, `go/*`, `image/*`, `runtime/*`) is 3,513
  symbols.

## 7. The gates (each in `make verify-core` unless stated)

Each new probe has a `--self-test` that proves it can fail, and a
parity-count bump in the same commit (project memory: "Adding a goolang
gate").

| Gate | Proves | Teeth (self-test) |
|---|---|---|
| T-1 `unsafe-boundary-probe` | every path of section 2.3 is refused with the exact text of 2.5: `import "unsafe"` in `main`, in `./p`, in a source-dir bare-name package, in a `./goostd/x` cwd-fallback package, under `GOOROOT=<copy of goostd outside the root>`, a symlink out of the root; each U-surface symbol class; U-convert; `internal/...` import; `import "C"`; a body-less function; `//go:linkname` | a positive control: a trusted package (the first real one, `math` with `Float64bits`) compiles `unsafe`; the self-test runs the probe against a build with the trust check forced open and must see FAIL |
| T-2 `trusted-manifest-probe` | the compiled-in table equals the set of `goostd/` packages whose files import `unsafe`; no fixture package is in it; every entry exists | mutate the table (add a fixture, drop an entry): FAIL |
| T-3 `vendor-drift-probe` | every vendored file equals its pristine copy in `third_party/go1.27.4-src/` (about 2.9 MB for the 69 packages measured here) modulo the ledger `goostd/VENDOR.tsv` (renames of Goo keywords, override files, dropped asm files). No Go toolchain is needed in CI | edit one vendored byte with no ledger row: FAIL |
| T-4 `stdlib-api-ratchet` | the symbol count on the chosen denominator (Q1) never falls below `tests/stdlib/api-floor.txt`; a symbol counts only if `check_stdlib_coverage.sh` finds a test or golden that calls it (anti-goal A4) | lower the count by deleting one fixture: FAIL |
| T-5 `upstream-tests-probe` | for each vendored package, the upstream `_test.go` files that compile run under `goo test`; the list of excluded tests per package, each with a reason, is a ledger; the passing-test count ratchets | delete one passing upstream test: FAIL |
| T-6 `trusted-extern-probe` | every body-less function in a trusted package has a C definition `goo_ts_*` in `lib/libgoo_runtime.a` (by `nm`) | remove one definition: FAIL |
| T-7 `arc-unsafe-probe` | one fixture per case C1 to C10 (section 3.3) in a trusted test package runs clean under valgrind and under `GOO_ARC_POISON=1`; `GOO_ARC_DEBUG=1` shows `UNSAFE_PIN` on the C1 and C5 fixtures; `bench/daemon/daemon.goo` block count at exit stays within a committed budget after each stage-3 replacement | a build that treats `unsafe.String` as fresh must make the C2 fixture fail under valgrind |
| T-8 `keyword-rename-probe` (part of T-3) | each rename of a Goo keyword in vendored source is scope-correct (the renamed file type-checks with `go vet` at vendor time) and listed in the ledger | a ledger row with no matching rename: FAIL |

Gate T-3 replaces "re-vendor and diff" with a committed pristine copy,
because CI has no Go toolchain and a skip would read as a pass.

The existing gates stay: `check_stdlib_coverage.sh` (it learns nested
`goostd/` paths), `stdlib-coverage-drift`, `diag-format-probe`,
`diagnostics-drift-probe`, `grammar-tripwire`, the superset probe.

## 8. Relation to the Go-compatibility work

### 8.1 Measured: the first parse error of each failing file

Population: the linux/amd64 `GoFiles` of the 11 core packages, the 54
direct-clean packages, and `slices`, `maps`, `cmp`, `iter`, `math`,
`math/bits`, `bufio`, `encoding/binary`, `unicode/utf16`. 318 files, 155
parse, 163 fail. Each failing file counts once, by its FIRST error. A later
error stays hidden, so each row is an upper bound on what its fix unblocks.

| Construct | Files | Planned in wave 1 |
|---|---|---|
| tuple assignment: three or more names, or a selector target (`n, b.err = ...`) | 40 | Task 6 |
| case lists (`case a, b:`) | 25 | Task 10 |
| qualified composite literal (`sync.Pool{...}`, `&ast.File{...}`) | 15 | Task 11 |
| `[...]T{...}` array literal | 14 | Task 15 |
| body-less function (assembly or linkname) | 10 | no: this design, 4.2 |
| `unsafe` keyword | 8 | no: this design, 2.4 |
| `&^=` | 8 | Task 6 (ruling 4) |
| type sets in constraints (`~T`, `\|`) | 7 | no |
| generic types and instantiated types (`type Seq[V any]`, `iter.Seq[string]`, `atomic.Pointer[T]`) | 5 | no |
| type alias (`type A = B`) | 4 | no |
| `*p++` on a dereference | 4 | no |
| number literal forms (hex float `0x1p127`, `.5`, digit separator `0x8000_0000`) | 4 | no |
| embedded qualified type (`io.Reader` inside an interface) | 4 | no |
| struct tags | 3 | no |
| local type declaration | 2 | Task 12 |
| directional channel type in a field (`chan<- T`, `<-chan T`) | 2 | no |
| anonymous struct literal | 2 | Task 13 |
| three-index slice (`s[0:size:size]`) | 1 | no |
| `var` with three or more names | 1 | Task 9 |
| grouped type declaration (`type ( ... )`) | 1 | no |
| Goo keyword as an identifier (`regexp`: method `match`) | 1 | no: 8.2 |
| `for x = range` | 1 | Task 7 |
| other (`go/format/internal.go`) | 1 | no |

Wave 1 plans the first-error construct of 108 of the 163 files (66%). The
55 others need wave 2 work or this design. The probes `c1` to `c5`
(Appendix B) confirm the top five rows on today's `bin/goo`.

### 8.2 Goo keywords inside Go stdlib source

Measured over the 2,019 public files (go/scanner, identifiers only): `match`
23 files (112 uses), `arena` 9 (43), `enum` 2 (23), `try` 2 (8). In the core
packages plus `regexp`: `match` 5 files, `try` 2. Every such name is
lowercase, so it is never exported, and a rename is API-neutral.

| Option | Pros | Cons |
|---|---|---|
| W1. Rename at vendor time (`match` to `match_`), scope-aware, listed in the ledger, gated by T-8 | keeps the owner's rule (keywords stay reserved); no compiler change | a rename ledger per Go release |
| W2. A keyword-free lexer mode for trusted files | no rename | the keyword set depends on trust, a hidden coupling between the lexer and the package resolver |
| W3. Contextual keywords | helps users too | the owner ruled the keywords stay reserved (wave 1 ruling 9) |

**Recommendation: W1** (question Q4).

### 8.3 Other compatibility facts that the stdlib needs

- Symbol mangling uses the SHORT package name
  (`src/codegen/codegen.c:52`, `codegen_pkg_mangled_symbol(...->name, ...)`).
  Five short names collide among public packages: `rand` (crypto/rand,
  math/rand, math/rand/v2), `json` (encoding/json, encoding/json/v2),
  `template`, `scanner`, `pprof`. One program that imports `text/scanner`
  (stage 1) and `go/scanner` (stage 4) gets duplicate symbols. The mangling
  must key on the full import path before stage 4. This is from the code,
  not from a probe.
- `goostd/` is flat today, with an alias table (`import_resolver.c:150-158`).
  Vendored packages go in nested directories (`goostd/container/list/`);
  `resolve_import` already joins the full path, so the alias table shrinks
  over time.
- File selection: the vendoring tool selects `GoFiles` with
  `GOOS=linux GOARCH=amd64` and the `purego` tag, drops every `.s` file, and
  swaps an assembly-backed file for its generic twin when one exists
  (`math/*_noasm.go`, `internal/bytealg/*_generic.go`). The ledger records
  each swap.
- `goo test` limits (CLAUDE.md "goo test"): no `t.Run`, no benchmarks, no
  examples, `TestMain` rejected. Upstream tests use all of these, so gate
  T-5 starts with a large exclusion ledger, and wave 2 should add `t.Run`.

## 9. Risks, ranked

1. **`append` reallocates the old backing array (existing defect, safe
   code).** `goo_slice_append` calls `goo_realloc(slice->data, ...)`
   (`src/runtime/runtime.c:1573`). Measured: a full sub-slice
   (`t := s[1:]; t = append(t, 5)`) aborts with `realloc(): invalid pointer`
   (exit 134; Go prints `10 11 4 99 5`). An alias of a slice that grows
   (`u := s; s = append(s, 1); s[0] = 8`) sees the write (Goo `8 8`, Go
   `7 8`). When the block moves, valgrind reports an invalid read of the
   alias. Go semantics (a new array on growth, the old one intact) are what
   every vendored package assumes. Fix it first, as its own defect task,
   with Go-derived goldens. It also closes case C7.
2. **A use-after-free through `unsafe` in trusted code.** Rules V, P, I, S,
   H, X and O close the ten named cases. The residual risk is an
   implementation slip in the rules themselves. Gate T-7 holds one valgrind
   fixture per case.
3. **Leak growth.** The rules fail toward leaks, and real upstream code
   leaks more than today's rewrites (3.4). Gate T-7's daemon budget decides
   whether a real package replaces a rewrite.
4. **Trust boundary bypass.** Section 2.3 lists every known path, and gate
   T-1 has a fixture for each one. A new path appears whenever the resolver
   gains a tier (ADR 0006 sub-project B). That change must extend T-1.
5. **Language gaps.** 55 of 163 first errors are in no plan. Generics with
   type sets and generic types are a project of their own, and stage 2
   waits on them.
6. **`reflect` size** (5.4). R2 is larger than the ARC 1 arc. R4 has no
   design yet.
7. **The metric.** A symbol-count goal pulls work toward `syscall`
   constants and `debug/elf`, which QPU and server work do not need. Q1.
8. **Upstream drift.** File selection, overrides and keyword renames are
   per Go release. The pristine copy and the ledger (T-3) make each bump a
   reviewed diff.
9. **Compile time and scale, unmeasured.** `unicode/tables.go` alone is
   257 KB of composite literals. No measurement of Goo's compile time on it
   exists. Stage 1 measures it first.
10. **Short-name mangling collisions** (8.3) before stage 4.

## 10. Open questions for the owner

- **Q1. Which denominator is the 50% goal?** Measured on Go 1.27.4:
  11,453 symbols for all operating systems (`scripts/stdlib-coverage.py`),
  8,433 for linux/amd64, 3,513 for the application subset. The figure 6,436
  from 2026-11-27 drops every OS- or arch-qualified line (the `sed` pattern
  expects `, (linux-386)` where the files write ` (linux-386),`).
  Recommendation: linux/amd64 for the ratchet, and the application subset
  reported beside it.
- **Q2. Retire the `unsafe` keyword (2.4, R1)?** Recommendation: yes. The
  Goo `unsafe { }` and `unsafe func` syntax compiles no correct program
  today.
- **Q3. Is a person who points `$GOOROOT` at their own `goostd/` inside the
  threat model?** Recommendation: no for the person (they edit the
  toolchain), yes for a PROJECT: only the exe-relative root confers trust.
- **Q4. Rename Goo keywords in vendored source at vendor time (8.2, W1)?**
  Recommendation: yes.
- **Q5. Accept the leak cost of rules V and P, with override files for hot
  paths and a daemon budget as the gate?** Recommendation: yes.
- **Q6. `!T` and `?T` under `reflect` and in interfaces (5.3)?**
  Recommendation: K1. `!T` is a compile error in an interface conversion,
  and `?T` boxes as `T` or nil. This changes today's behaviour, which prints
  the type name.
- **Q7. Make finalizers and cleanups no-ops, as a recorded by-design
  divergence?** Recommendation: yes (Go does not promise that they run).
- **Q8. When does real source REPLACE a goostd rewrite** (`strings`,
  `bytes`, `strconv`, `sort`, `io`)? Recommendation: when the real package
  passes its upstream tests (T-5) and the daemon budget (T-7), not before.

## 11. Order of work

1. Stage 0: the `append` defect (risk 1) as its own task, with goldens from
   `go run` and a valgrind fixture.
2. Stage 0: the owner chooses the metric (Q1). Fix the measurement, make
   `stdlib-coverage.py` and gate T-4 use it, and commit the floor.
3. Stage 0: the vendoring tool (file selection, pristine copy, ledger) and
   gates T-3 and T-8. No package moves yet.
4. Stage 1: vendor the transitively clean packages one at a time, smallest
   first (container/list, container/ring, encoding, structs, cmp,
   unicode/utf16, image/color, image/color/palette, unicode), then the rest
   of stage 1. Each package is one task: vendor, fix the compiler gaps it
   shows (most are wave 1 tasks), run its upstream tests (gate T-5), raise
   the floor (T-4). Measure the compile time of `unicode` first (risk 9).
5. Stage 2: wave 2 generics (type sets, generic types), then `slices`,
   `maps`, `iter.Seq`, then the stage 2 packages.
6. Stage 3, part 1: retire the keyword (Q2), the trust table and checks,
   the diagnostics, and gate T-1. No package uses `unsafe` yet, except the
   positive control.
7. Stage 3, part 2: ARC rules V, P, I, S, H, the new reason bit, and gate
   T-7 with its ten fixtures, BEFORE any real package relies on them.
8. Stage 3, part 3: body-less hooks (4.2) with gate T-6; `internal/bytealg`,
   `stringslite`, `strconv`, `cpu`, `goarch`, `byteorder`; then `math`,
   `math/bits`, `unicode/utf8`, `strconv`, `strings` (with the rule O
   override), `bytes`, `io`, `path`, the hash packages. Each replacement
   passes T-5 and T-7.
9. Fix the short-name mangling (8.3).
10. Stage 4: descriptor widening, then reflect R1 (errors, sort), R2 (fmt,
    encoding/json Marshal, encoding/binary), R3 (Unmarshal).
11. Stage 5: the `sync` and `time` hooks over the parking plan's waiter
    model, then `syscall`, `internal/poll`, `os`, `net`. Reflect R4 last.

## Appendix A. Measurements (2026-11-27, Go 1.27.4, `bin/goo` built from bb99dfe3)

Scratch scripts live in the session scratchpad; each command below is
repeatable from a clean checkout plus Go 1.27.4 at `/usr/local/go`.

1. Exported top-level symbols. `cat $GOROOT/api/go1*.txt`, drop `#`
   lines, match `^pkg P( (qualifier))?, (func|type|const|var) NAME`, sort
   unique by (P, NAME): 11,453. The same with only unqualified lines plus
   `(linux-amd64)` and `(linux-amd64-cgo)` lines: 8,433. The 2026-11-27
   pattern without the qualifier: 6,436 (re-measured, same).
2. Goo coverage. `GOO_STDLIB_COVERAGE_OUT=<scratch> python3
   scripts/stdlib-coverage.py`: 112 supported, 11,453 total, 12 packages.
   `scripts/check_stdlib_coverage.sh`: 202 symbols (61 shim rows, 4 value
   members, 4 time constants, 5 time functions, 15 seeded methods, 87 goostd
   functions, 26 goostd methods; it also counts Goo-only `lanes`, `far` and
   `qir`), PASS. The committed `docs/stdlib-coverage.json` is stale
   (go1.26.1).
3. Parse share. The 2026-11-27 script with this worktree's `bin/goo`: 852 of
   2,019 public files (no tests, cmd, internal, vendor, testdata) parse;
   positive control rc 0. Re-measured, same.
4. Per-file imports, `go/parser` imports-only over the same 2,019 files:
   `unsafe` 568, `internal/*` 519 (and 113 more import a nested
   `.../internal/...` path), `runtime` 137, `reflect` 60, `sync/atomic` 43.
   The 2026-11-27 figures (574, 522, 138, 64, 44) came from a different
   method; the difference does not change any conclusion.
5. Core symbol use (4.1): a `go/build` + `go/ast` tool, `GOOS=linux
   GOARCH=amd64`, `GoFiles` only.
6. Package sets (6): `GOOS=linux GOARCH=amd64 go list -f
   '{{.ImportPath}}|{{.Imports}}|{{.Deps}}' std`; 176 public packages; 54
   direct-clean, 1,227 symbols; 10 transitive-clean, 387 symbols. The
   2026-11-27 list of 50 clean packages used a per-file test over all
   build variants and kept `crypto/*` packages that import
   `crypto/internal/*`; it also listed non-packages (`builtin`, `_asm`,
   `_gen`).
7. Stages (6): the fixpoint script; cumulative sums against the per-package
   coverage of measurement 2.
8. First parse errors (8.1): `bin/goo --emit-ast-json` on each selected
   file, the first `error` line, the source line at its position, classified
   by construct.
9. Goo keywords as identifiers (8.2): a `go/scanner` tool over the 2,019
   files.
10. `unsafe` in API signatures (2.3): `grep 'unsafe\.Pointer'` over the API
    files: 4 `reflect`, 4 `sync/atomic`, 1 `runtime`.
11. Short-name collisions (8.3): last path element of the 176 public
    packages, duplicates listed.

## Appendix B. Probes (scratch programs, `bin/goo` from bb99dfe3)

| Probe | Goo | Go |
|---|---|---|
| `u1`: `import "unsafe"`; `unsafe.Sizeof(x)` | `syntax error, unexpected 'unsafe'` | prints 8 |
| `u2`: `unsafe.Pointer(&x)` in an expression | `syntax error, unexpected 'unsafe'` | works |
| `u8`: `import _ "unsafe"` alone | `goo: error: cannot resolve import "unsafe"` | works |
| `u9`: a struct field of type `unsafe.Pointer` | `goo: error: cannot resolve import "unsafe"` | works |
| `u7`: `var unsafe Pointer = 3` | `syntax error, unexpected 'unsafe'` | works |
| `u3`: `unsafe { ... }` (Goo) | `error: Unknown statement type` | n/a |
| `u6`: `unsafe func raw() int { return 7 }` (Goo) | `return type mismatch: cannot return a value from a function with no return type` | n/a |
| `u4`: `import "C"` | `goo: error: cannot resolve import "C"` | cgo |
| `u5`: body-less `func ext(x int) int` | `syntax error, unexpected ';'` | `missing function body` |
| `c1`: `case 1, 2:` | `syntax error, unexpected ','` | works |
| `c2`: `a, b, c := three()` | `syntax error, unexpected ','` | works |
| `c3`: `x &^= 2` | `syntax error, unexpected '='` | works |
| `c4`: `[...]int{1, 2, 3}` | `syntax error, unexpected '...'` | works |
| `c5`: `n, r.err = two()` | `syntax error, unexpected '.'` | works |
| `r1`: `!int` boxed in `interface{}`, printed | `!int64` | n/a |
| `r2`: `?int` present and absent, boxed, printed | `?int64` twice | n/a |
| `a1`: `append` to a full sub-slice | abort, `realloc(): invalid pointer`, exit 134 | `10 11 4 99 5` |
| `a2`: alias of a grown slice | `8 8` | `7 8` |
| `a3`: alias of a slice grown 600 times, under valgrind | `Invalid read of size 8`, 1 error | `7 7 1` |
