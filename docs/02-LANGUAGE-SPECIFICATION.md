# Goo Language Specification v1.0

## Table of Contents

1. [Lexical Elements](#lexical-elements)
2. [Types](#types)
3. [Variables and Constants](#variables-and-constants)
4. [Functions](#functions)
5. [Control Flow](#control-flow)
6. [Error Handling](#error-handling)
7. [Concurrency](#concurrency)
8. [Memory Management](#memory-management)
9. [Modules and Packages](#modules-and-packages)
10. [Compile-Time Features](#compile-time-features)
11. [Attributes](#attributes)
12. [Grammar](#grammar)

## Lexical Elements

### Comments

```goo
// Line comment
/* Block comment */
/// Documentation comment
```

### Identifiers

- Must start with letter or underscore
- Can contain letters, numbers, underscores
- Case sensitive

### Keywords

```
actor       comptime    for         match       shared
as          const       func        module      struct
async       defer       go          mut         super
await       else        if          package     trait
break       enum        impl        pub         try
capability  error       import      return      type
case        export      interface   select      unsafe
channel     extern      let         self        use
```

## Types

### Primitive Types

```goo
// Integers
i8, i16, i32, i64, i128, isize
u8, u16, u32, u64, u128, usize

// Floating point
f32, f64

// Boolean
bool

// Character and String
char    // Unicode scalar value
string  // UTF-8 encoded string

// Unit type
void
```

### Composite Types

```goo
// Arrays (fixed size)
[10]int
[N]T where N: comptime int

// Slices (dynamic size)
[]int
[]T

// Maps
map[string]int
map[K]V where K: Hash + Eq
// Nil-map semantics (Go parity, decided + shipped 2026-07-10, locked by
// examples/nil_map_write_abort_probe.goo and nil_map_read_probe.goo):
// writing to a nil map panics "assignment to entry in nil map" (exit 2,
// including compound assignment m[k]+=1); reads yield the zero value,
// comma-ok reports false, len is 0, delete is a no-op, and range iterates
// zero times.

// Tuples
(int, string, bool)

// Structs
struct Point {
    x: f64
    y: f64
}

// Enums
enum Result<T, E> {
    Ok(T)
    Err(E)
}
```

### Type Aliases

```goo
type UserId = u64
type Distance = f64 @unit("meters")
```

### Interfaces

```goo
interface Reader {
    read(buf: []byte) !usize
}

// With default implementations
interface Iterator<T> {
    next() ?T
    
    // Default implementation
    collect() []T {
        result := []T{}
        while let Some(item) = self.next() {
            result.append(item)
        }
        return result
    }
}
```

### Nullable Types

> The syntax below reflects the v1 shipped compiler (Phase 2 sub-project C,
> 2026-07-09); the rest of this section (interfaces above, generics,
> `Iterator<T>`) is aspirational/pre-implementation vision syntax that has
> not been built (see the note at the top of [Error Handling](#error-handling)).

`?T` marks a value that may be absent (`nil`) — a tagged optional over any
base type, including value types like `?int` (`{ present bool; value T }`
under the hood, not Go's untyped nil). It composes with struct fields and
function parameters/returns:

```goo
var a ?int = 42
var b ?int = nil

type Holder struct {
    val ?int
    tag int
}

func useNullable(p ?int) int {
    if let v = p {
        return v
    }
    return -1
}
```

The only way to read a `?T`'s payload is `if let`, which binds the
unwrapped value in the `then` branch and takes `else` on `nil`:

```goo
if let v = a {
    fmt.Println(v)
} else {
    fmt.Println("absent")
}

if a == nil {
    fmt.Println("a is absent")
}
```

`?T == ?T` is defined: true iff both operands are `nil`, or both are
present with equal payloads; `T == ?T` / `?T == T` (either operand order)
is true iff the `?T` operand is present and its payload equals `T`. `!=` is
the negation.

**Not implemented in v1** (tracked as v2+ candidates): a force-unwrap
operator (`x!`), a presence-test (`x?`), and a coalesce operator
(`x ?? default`). `if let` is the only unwrap path.

**Shift semantics** (Go parity, shipped 2026-07-10 with real `-O` passes):
a shift count `>=` the operand's width yields 0 (or the sign fill, for a
signed right shift), and a runtime-negative count panics
`runtime error: negative shift amount` — both locked by probes and
identical at `-O0` and `-O2`. Known v1 edge (pre-existing, documented not
fixed): a runtime count that is an exact multiple of 256/the operand width
beyond the coercion width (e.g. `x << k` with `k == 256` for an 8-bit
operand) truncates before the guard and wraps instead of saturating.

### Nil Semantics (Go Parity)

**Goo adopts Go's nil semantics** for `*T`, `[]T`, `map[K]V`, `chan T`,
`func(...)`, interfaces, and `error`: all seven are nilable, `nil` is
assignable and comparable for each, and dereferencing/dispatching through
one when it is actually nil panics instead of silently misbehaving. `?T`
(and its pointer form `?*T`) is Goo's separate, opt-in **non-nullable**
differentiator — a tagged optional that is never Go's untyped nil under the
hood (see [Nullable Types](#nullable-types) above) — not the default for
ordinary Go-shaped code. This closes the fork the roadmap's P2.2 left open
(ADR 0001, `docs/adr/0001-nil-semantics-go-parity-with-emitted-deref-checks.md`).

The table below is the full 24-operation matrix (empirically probed
2026-07-23, reconstructed here from ADR 0001's Context paragraph plus the
cells this arc closed). Every row marked "fixed this arc" is pinned by
`scripts/nil_deref_probe.sh` (11 cases, wired into `make verify-core` as
`nil-deref-probe`); every other row was already Go-parity before this arc
and is re-verified by the same probe run or an existing locked fixture.

| # | Operation | Behavior | Status |
|---|---|---|---|
| 1 | `*T` assign `nil` / compare `== nil` | typechecks, compiles, compares correctly | works |
| 2 | `[]T` assign `nil` / compare `== nil` | typechecks, compiles, compares correctly | works |
| 3 | `map[K]V` assign `nil` / compare `== nil` | typechecks, compiles, compares correctly | works |
| 4 | `chan T` assign `nil` / compare `== nil` | typechecks, compiles, compares correctly | works |
| 5 | `func(...)...` assign `nil` / compare `== nil` | typechecks, compiles, compares correctly | works |
| 6 | nil-map read (`m[k]`) | zero value; comma-ok reports `false` | works |
| 7 | nil-map write (`m[k] = v`, incl. `m[k] += 1`) | panics `assignment to entry in nil map` (P3.9 decision, 2026-07-10) | works |
| 8 | nil-map delete (`delete(m, k)`) | no-op | works |
| 9 | nil-map range (`for k, v := range m`) | zero iterations | works |
| 10 | nil-slice `len(s)` | `0` | works |
| 11 | nil-slice `append(s, ...)` | allocates, returns a non-nil result | works |
| 12 | nil-channel send/recv (`ch <- v`, `<-ch`) | blocks forever — never a silent zero-value success; a nil-channel `select` case is never ready. In a main-only program the deadlock detector reports it near-instantly (Go parity: "all goroutines are asleep"-class abort; `src/runtime/channels.c` nil-block path) | works |
| 13 | `close(nil)` (nil channel) | panics `close of nil channel` | works |
| 14 | pointer deref read (`*p`) | panics (canonical message below) | **fixed this arc** (Task 2) |
| 15 | pointer deref write (`*p = v`) | panics (canonical message below) | **fixed this arc** (Task 2) |
| 16 | field read via nil pointer (`p.x`) | panics (canonical message below) | **fixed this arc** (Task 2) |
| 17 | field write via nil pointer (`p.x = v`) | panics (canonical message below) | **fixed this arc** (Task 2) |
| 18 | nil-receiver method NOT touching fields (e.g. `p.Tag()` where `Tag` never reads/writes a field) | runs normally — legal Go | works (pre-existing) |
| 19 | nil-receiver method touching fields (e.g. `p.Get()` where `Get` reads/writes a field) | panics at the field access *inside* the method body — same site as row 16, matching Go's panic location | **fixed this arc** (Task 2) |
| 20 | nil user-interface dispatch (`var s I; s.M()`) | panics; the call's *arguments* are evaluated before the panic fires (Go's evaluation order) | **fixed this arc** (Task 3) |
| 21 | typed-nil value in interface dispatch (`var p *T; var s I = p; s.M()`) | runs normally — only a field access inside `M` would panic (row 19) | works (pre-existing) |
| 22 | `error(nil).Error()` | panics (previously a silent `""` return via a deliberate codegen guard) | **fixed this arc** (Task 4) |
| 23 | nil-slice index (`s[0]` where `s` is nil) | panics, exit 2 — Go-parity *behavior* | divergent (wording only, see below) |
| 24 | nil func-value call (`var f func(); f()`) | panics, exit 2 — Go-parity *behavior* | divergent (wording only, see below) |

**Argument-evaluation order on nil-interface dispatch (row 20).** Go
evaluates a call's arguments, including their side effects, before the call
itself — and a nil-interface-dispatch panic happens *as part of* the call,
not before it. Goo matches this: `s.Speak(sideEffect())` on a nil `s` still
runs `sideEffect()` (and any of its own output) before panicking, even
though `Speak` never executes. Verified against real `go1.26.1` and pinned
by the probe's `nil_interface_dispatch_arg_order` case, which asserts the
side-effecting argument's stdout survives the panic. The check itself lives
inside `codegen_interface_dispatch`'s dispatch choke point, after argument
codegen, specifically so the ordering falls out of normal control flow
rather than needing a separate early check.

**Correctness fix, ride-along with row 15 (Task 2).** Writing through a
pointer stored in a struct field — `*h.p = v` where `h.p` is itself a
non-identifier pointer-typed lvalue (as opposed to `*p = v` on a plain
pointer variable) — was previously a **silent no-op store**: the value was
written into the field's own slot instead of through the pointer it holds,
losing the write with no diagnostic. Reconciling the lvalue path to add
row 15's nil check surfaced and fixed this pre-existing bug as a side
effect (no behavior change for the plain-identifier case). The reconciled
path is exercised by the probe's `star_write` and `non_nil_paths` cases.

**The canonical panic.** Rows 14-17, 19, 20, and 22 — the sites this arc
closed — all route through the same cold, `noreturn` `goo_nil_deref_fail(file,
line)`, reached via an inline `icmp eq null` + conditional branch at each
site (arc-17's bounds-check shape, reused verbatim-shaped) — never a
SIGSEGV/exit-139 crash. Each produces, on stderr, a `nil dereference at
<file>:<line>` diagnostic line followed by:

```
panic: runtime error: invalid memory address or nil pointer dereference
```

and exits with code **2**, matching Go's message text exactly. This is one
family among several Go-parity panics in the matrix above, not the only
one: nil-map write (row 7) panics its own `assignment to entry in nil map`
and `close(nil)` (row 13) panics its own `close of nil channel` — both
pre-existing, both following the same exit-2 convention, neither routed
through `goo_nil_deref_fail`.

**Known v1 divergences — message wording only, deliberately not changed
this arc** (rows 23-24 above; both pre-date ADR 0001 and are out of this
arc's scope):

- **Nil-slice index** (`s[0]` on a nil slice): Goo panics `bounds check
  failed` (via the same arc-17 inline bounds-check path every slice index
  uses — a nil slice has length 0, so any index fails it); Go panics
  `runtime error: index out of range [0] with length 0`. Exit code 2 and
  panic-on-any-index behavior match; only the text differs.
- **Nil func-value call** (`var f func(); f()`): Goo panics `call of nil
  function` (a pre-existing guard, `codegen_emit_funcnil_check` in
  `src/codegen/call_codegen.c`); Go panics `runtime error: invalid memory
  address or nil pointer dereference` (the same message as the pointer/
  field/interface sites above — Go treats a nil func call as an ordinary
  nil dereference). Exit code 2 and panic-on-call behavior match; only the
  text (and the absence of a `nil dereference at file:line` line) differs.
- **`&*p` on a nil pointer**: RESOLVED (SCB arc) — Goo now folds `&*p`
  to `p` at emission time, matching Go: no nil check at the `&*` site,
  `p` evaluated exactly once, and the check fires at whatever later
  dereferences the result (pinned by `nil_deref_probe.sh`'s
  `addr_of_deref_fold` / `addr_of_deref_then_deref` cases).

## Variables and Constants

### Variables

```goo
// Immutable by default
let x = 42
let y: f64 = 3.14

// Mutable
mut count = 0
mut buffer: [1024]byte = [0; 1024]

// Type inference
let data = load_file("data.txt") // Compiler infers type
```

### Constants

```goo
const PI = 3.14159
const MAX_SIZE: usize = 1024
const VERSION = comptime {
    read_file("VERSION").trim()
}
```

### Global Variables

```goo
// Thread-local
@thread_local
let mut COUNTER = 0

// Shared (automatically synchronized)
shared STATE = State.new()
```

## Functions

### Basic Functions

```goo
func add(a: int, b: int) int {
    return a + b
}

// Multiple return values
func divmod(a: int, b: int) (int, int) {
    return a / b, a % b
}

// Named return values
func parse(s: string) (value: int, ok: bool) {
    // Can use 'value' and 'ok' as variables
    if let Some(v) = parse_int(s) {
        value = v
        ok = true
    }
    return // returns (value, ok)
}
```

### Methods

```goo
impl Point {
    // Constructor
    func new(x: f64, y: f64) Point {
        return Point { x, y }
    }
    
    // Method
    func distance(self, other: Point) f64 {
        let dx = self.x - other.x
        let dy = self.y - other.y
        return sqrt(dx*dx + dy*dy)
    }
    
    // Mutable method
    func translate(mut self, dx: f64, dy: f64) {
        self.x += dx
        self.y += dy
    }
}
```

### Generic Functions

```goo
func max<T: Ord>(a: T, b: T) T {
    if a > b { return a }
    return b
}

// With constraints
func sum<T>(items: []T) T 
    where T: Add<Output=T> + Default {
    mut result = T.default()
    for item in items {
        result = result + item
    }
    return result
}
```

## Control Flow

### If Expressions

```goo
// Traditional if
if x > 0 {
    positive()
} else if x < 0 {
    negative()
} else {
    zero()
}

// If expression
let result = if condition { value1 } else { value2 }

// If-let pattern matching
if let Some(value) = optional {
    use_value(value)
}
```

### Loops

```goo
// Infinite loop
loop {
    if done() { break }
}

// While loop
while condition {
    process()
}

// For loop with range
for i in 0..10 {
    print(i)
}

// For loop with iterator
for value in collection {
    process(value)
}

// For loop with index
for i, value in collection.enumerate() {
    print(f"{i}: {value}")
}
```

### Match Expressions

```goo
match value {
    0 => zero(),
    1..=10 => small(),
    n if n % 2 == 0 => even(n),
    _ => other()
}

// Exhaustive matching enforced
match result {
    Ok(value) => process(value),
    Err(error) => handle_error(error)
    // Compiler error if any case missed
}
```

## Error Handling

> This section was rewritten 2026-07-09 (Phase 2 sub-project C, roadmap
> P2.6-P2.9/P2.11) to match the actual shipped v1 compiler — the previous
> revision showed a Rust-flavored `let`/`match`/`else err` syntax that was
> never implemented. Goo's real error handling is Go-compatible-first:
> ordinary `(T, error)` returns work exactly as in Go, and `!T`/`try`/`catch`
> are Goo's additive sugar over that same shape. The rest of this document
> (composite types, generics, actors, etc.) remains the pre-implementation
> vision and is NOT updated here — see individual roadmap docs under
> `docs/superpowers/specs/` for what has actually shipped.

### Error Type

```goo
// Go-compatible: an ordinary (T, error) return works unchanged.
func classic(bad bool) (int, error) {
    if bad {
        return 0, errors.New("boom")
    }
    return 41, nil
}

// Goo's error union !T: the function returns either a T or an error, with
// no separate error return value.
func parseInt(bad bool) !int {
    if bad {
        return error("boom")
    }
    return 41
}
```

### Try Operator

`try` unwraps a call's success value inline; on error it propagates out of
the *enclosing* function, converting into that function's own `!T` error
arm. `try` accepts both an `!T`-returning callee and a user-declared
`(T, error)`-returning callee (mirroring Go's own `(T, error)` idiom) — in
either case, the enclosing function must itself return an error union
(`!T`); a `(T, error)`-returning enclosing function gets a compile error
naming the requirement (v1 does not support try-propagation into a plain
`(T, error)` return).

```goo
func outer(bad bool) !int {
    v := try mightFail(bad)   // !int callee: unwraps 20, or propagates the error
    return v + 1
}

func outer2(bad bool) !int {
    v := try classic(bad)     // (T, error) callee: same unwrap/propagate contract
    return v + 1
}
```

### Catch

`catch` recovers from a failing `!T` or `(T, error)` call. The bound
variable is the real `error` interface (`e.Error()` dispatches). A block
`catch` may end in a trailing expression, which supplies the recovered
value on the error path; the success path skips the block and passes the
unwrapped value through unchanged:

```goo
result := mightFail(true) catch e {
    fmt.Println("failed:", e.Error())
    -1   // trailing expression: the recovered value
}
```

`catch => expr` is sugar for a fallback with no bound error variable:

```goo
result := mightFail(true) catch => -1
```

Binding a raw error union with no `try`/`catch`/destructure (`x := f()`
where `f()` returns `!T`) is a compile error naming the three ways to
handle it — an error union must always be handled at the binding.

### Defer and Cleanup

```goo
func process() error {
    file, err := openFile("data.txt")
    if err != nil {
        return err
    }
    defer file.Close() // runs at scope exit, Go-compatible
    return file.ReadAll()
}
```

Deferred calls run at function exit in LIFO order; arguments (and the callee
function value, for indirect calls) are evaluated when the `defer` statement
executes, per Go. Defers inside loops push once per iteration (P3.4).

**Known v1 divergence from Go** (pre-existing, found 2026-07-10): a deferred
function that mutates a NAMED RESULT does not affect the returned value —
`func f() (r int) { defer func(){ r = r + 1 }(); return 5 }` returns 6 in Go
but 5 in Goo, because `return` snapshots the value before defers run. Applies
to both defer mechanisms (static and loop/runtime-stack).

### Goroutines

```goo
// Spawn a goroutine
go process_async()

// With parameters
go func() {
    do_work()
}()

// Supervised goroutine
supervised go handle_connection(conn)
```

**Program exit** (Go parity, locked by `examples/main_exit_probe.goo`): when
`main` returns, the process exits immediately — running goroutines are
abandoned, exactly as in Go. A program that needs goroutine side effects to
complete must synchronize before returning (channel handshake, or
`sync.WaitGroup` once available).

**Deadlock detection** (partial in v1): when `main` blocks and no goroutine
exists that could ever wake it, the runtime aborts with Go's `fatal error:
all goroutines are asleep - deadlock!` (exit code 2), locked by
`examples/deadlock_probe.goo`. **Known v1 divergence from Go**: if `main`
blocks while spawned goroutines are themselves all permanently blocked, the
deadlock is NOT detected and the program hangs — Go aborts here because its
`main` is a goroutine, while Goo's `main` is an OS thread (structural
limitation, documented at the detector in `src/runtime/concurrency.c`).

**Nil channels** (Go parity, locked by `examples/nil_chan_deadlock_probe.goo`):
send and receive on a nil channel block forever (never a silent zero-value
success); in a select, a nil-channel case is never ready. `close(nil)` panics
`close of nil channel`. A main-only program blocking on a nil channel gets
the deadlock abort above.

**sync package** (P4.7, locked by `examples/sync_probe.goo`):
`sync.Mutex` (Lock/Unlock) and `sync.WaitGroup` (Add/Done/Wait) are usable as
zero values (`var mu sync.Mutex` — no construction), matching Go. Unlocking
an unlocked mutex panics `sync: unlock of unlocked mutex` (exit 2). **Known
v1 divergence from Go**: copying a Mutex or WaitGroup copies an internal
handle, so copies ALIAS the same primitive after first use — Go instead
gives copies independent (broken) state and forbids the copy via `go vet`.
Don't copy them after use in either language. Method values on sync types
(`f := mu.Lock`) are rejected in v1. Embedding works (`type Counter struct
{ sync.Mutex; n int }` with promoted `c.Lock()` — locked by
`examples/embed_pkg_probe.goo`).

### Channels

```goo
// Create channels
let ch = channel<int>()
let buffered = channel<string>(100)

// Send and receive
ch <- 42           // Send
let value = <-ch   // Receive

// Select statement
select {
    case value = <-ch1:
        process1(value)
    case value = <-ch2:
        process2(value)
    case ch3 <- result:
        sent()
    default:
        nothing_ready()
}
```

### Advanced Channels

```goo
// Pub/sub channel
let events = channel pub<Event>()
let subscriber = channel sub<Event>()

// Request/reply
let requests = channel req<Request>()
let replies = channel rep<Reply>()

// Work distribution
let tasks = channel push<Task>()
let workers = channel pull<Task>()
```

### Actors

```goo
actor Counter {
    mut value: int = 0
    
    handle Increment() {
        self.value += 1
    }
    
    handle Get() -> int {
        return self.value
    }
    
    handle Reset() {
        self.value = 0
    }
}

// Usage
let counter = Counter.new()
counter ! Increment()
let value = counter ? Get()
```

### Parallel Execution

```goo
// Parallel for loop
parallel for i in 0..1000 {
    process(data[i])
}

// With options
parallel {
    workers: 8,
    chunk_size: 100
} for item in items {
    expensive_computation(item)
}

// Map-reduce pattern
let results = parallel map(items, process)
    |> parallel reduce(sum)
```

## Memory Management

### Allocators

```goo
// Define allocators
allocator heap_alloc heap {}
allocator arena_alloc arena { size: 1_000_000 }
allocator pool_alloc pool { object_size: 64 }

// Use specific allocator
let data = alloc(1024, heap_alloc)
defer free(data, heap_alloc)

// Scoped allocation
scope(arena_alloc) {
    let temp1 = alloc(100) // Uses arena_alloc
    let temp2 = alloc(200) // Uses arena_alloc
} // All freed automatically
```

> The `allocator`/`scope()` forms above are a forward-looking design sketch.
> The mechanism actually implemented today is the **`arena { }` block** described
> next.

### Arena Regions (`arena { }`)

Goo follows a **hybrid memory model**. The default is ordinary heap allocation
(a managed/GC default is a planned future leg — see below). An `arena { }` block
opts a lexical region into a **bump allocator** whose entire backing is freed in
one shot when the block exits:

```goo
arena {
    // new(T) / &T{} allocations that stay inside the block are bump-allocated
    // from this arena and reclaimed together when the block ends.
    node := &Node{value: 42}
    tmp  := new(int)
    use(node, tmp)
} // the whole arena is freed here
```

**Escaping values are promoted to the heap automatically — the model is safe by
construction, with no user annotations and no dangling pointers.** The compiler
runs an interprocedural escape analysis: any allocation whose value can outlive
the block — returned, assigned to a variable declared outside the block, assigned
through a pointer or into a global, captured by a closure, passed to a goroutine,
passed to a function that retains it, or *embedded in* any such value — is emitted
on the persistent heap instead of the arena. Only allocations proven to die with
the block use the arena. When the analysis is unsure, it heap-allocates (it never
puts an escaping value in the arena):

```goo
var kept *Node
func build() *Node {
    arena {
        scratch := &Node{value: 1} // stays local -> arena, freed at block exit
        result  := &Node{value: 2}
        kept = result              // escapes to an outer variable -> heap
        return &Node{value: 3}     // escapes via return -> heap, survives
    }
}
```

Semantics and current limits:

- The arena is freed on **every** exit from the block: the normal fall-through,
  a `return` (which frees every arena it unwinds through), and a `break`/`continue`
  (which frees the arenas inside the loop it exits, while leaving any arena that
  *encloses* the loop alive). Each exit path frees the arena exactly once.
- Arena blocks nest, and an arena block inside a loop allocates and frees a fresh
  arena per iteration — the concrete win: a loop that builds many temporary
  objects inside an `arena { }` keeps resident memory flat instead of growing.
- Because escaping allocations are heap-promoted, code outside the block is never
  affected by the arena, and no arena-allocated value is ever reachable after the
  block is freed.

**Hybrid model roadmap.** Arena regions are leg 1 (manual, opt-in reclamation).
Two further legs are planned and not yet implemented: a **GC-managed default** (the
"true to Go" default allocation strategy), and **comptime-selected strategies** (let
compile-time code choose heap vs. arena vs. managed per context).

### Smart Pointers

```goo
// Unique ownership
let unique = Box.new(value)

// Reference counting
let shared = Rc.new(value)
let copy = shared.clone()

// Thread-safe reference counting
let atomic = Arc.new(value)
```

### Unsafe Blocks

```goo
unsafe {
    // Direct memory manipulation
    let ptr = data.as_ptr()
    *ptr = 42
}
```

## Modules and Packages

### Module Declaration

```goo
module math {
    pub func abs(x: f64) f64 {
        if x < 0 { return -x }
        return x
    }
    
    // Private function
    func helper() { }
}
```

### Imports

```goo
import std.io
import math.{sin, cos, tan}
import graphics.render as r

// Conditional imports
comptime if target.os == "linux" {
    import linux.specific
}
```

### Visibility

```goo
pub struct PublicStruct { }
pub(crate) struct CrateOnly { }
pub(super) struct ParentModule { }
struct Private { } // Default is private
```

## Compile-Time Features

### Comptime Execution

```goo
const TABLE = comptime {
    mut table = [256]u32{}
    for i in 0..256 {
        table[i] = calculate_crc(i)
    }
    return table
}
```

### Comptime Parameters

```goo
func create_array<comptime N: usize>() [N]int {
    return [0; N]
}

let array = create_array<100>() // Size known at compile time
```

### Build Configuration

```goo
comptime {
    if option("debug") {
        const LOG_LEVEL = "debug"
    } else {
        const LOG_LEVEL = "info"
    }
}
```

### Code Generation

```goo
comptime {
    for field in struct_fields(MyStruct) {
        @generate(f"
            func get_{field.name}(s: MyStruct) {field.type} {{
                return s.{field.name}
            }}
        ")
    }
}
```

## Attributes

### Common Attributes

```goo
@inline
func hot_function() { }

@no_inline
func cold_function() { }

@test
func test_addition() {
    assert(add(2, 2) == 4)
}

@bench
func bench_sort(b: Bencher) {
    let data = random_array(1000)
    b.iter(|| sort(data.clone()))
}
```

### Safety Attributes

```goo
@no_panic
func safe_function() { }

@no_alloc
func allocation_free() { }

@pure
func no_side_effects(x: int) int {
    return x * 2
}
```

### Optimization Hints

```goo
@optimize("speed")
func fast_path() { }

@optimize("size")
func cold_path() { }

@likely
if common_case() { }

@unlikely
if rare_case() { }
```

## Grammar

### EBNF Notation

```ebnf
program = { package_decl } { import_decl } { top_level_decl } ;

package_decl = "package" identifier ;

import_decl = "import" ( string_lit | import_spec ) ;
import_spec = identifier "." ( identifier | "{" identifier { "," identifier } "}" ) ;

top_level_decl = func_decl | type_decl | const_decl | var_decl | impl_decl ;

func_decl = { attribute } "func" identifier [ generic_params ] "(" [ param_list ] ")" [ return_type ] block ;

type_decl = "type" identifier [ generic_params ] "=" type ;

const_decl = "const" identifier [ ":" type ] "=" expression ;

var_decl = [ "mut" ] "let" identifier [ ":" type ] "=" expression ;

impl_decl = "impl" [ generic_params ] type "{" { method_decl } "}" ;

type = primitive_type | array_type | slice_type | map_type | struct_type | enum_type | interface_type | identifier ;

expression = literal | identifier | binary_expr | unary_expr | call_expr | field_expr | index_expr | match_expr | if_expr | block ;

statement = expression | var_decl | assignment | return_stmt | break_stmt | continue_stmt | defer_stmt ;

block = "{" { statement } "}" ;
```