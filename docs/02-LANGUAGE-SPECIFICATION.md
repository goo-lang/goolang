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

### Error Type

```goo
// Built-in error trait
interface Error {
    message() string
    source() ?Error
}

// Error unions
func parse_int(s: string) !int {
    // Returns either int or error
}
```

### Try Operator

```goo
func process_file(path: string) !Result {
    let content = try read_file(path)
    let data = try parse_json(content)
    let result = try process_data(data)
    return result
}
```

### Error Handling

```goo
// Handle specific errors
let value = try risky_operation() else err {
    match err {
        IoError.NotFound => return default_value(),
        IoError.PermissionDenied => panic("No permission"),
        _ => return err // Propagate other errors
    }
}

// Catch-all
let value = risky_operation() catch default_value()
```

### Defer and Cleanup

```goo
func process() !void {
    let file = try open_file("data.txt")
    defer file.close() // Runs at scope exit
    
    let data = try file.read_all()
    return process_data(data)
}
```

## Concurrency

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