# Goo Quick Reference

## Basic Syntax

### Variables and Constants

```goo
// Immutable by default
let x = 42
let y: f64 = 3.14

// Mutable variables
mut count = 0
mut name: string = "Alice"

// Constants
const PI = 3.14159
const MAX_SIZE: usize = 1024

// Global variables
shared COUNTER = AtomicInt(0)

// Thread-local
@thread_local
let mut CACHE = HashMap.new()
```

### Functions

```goo
// Basic function
func add(a: int, b: int) int {
    return a + b
}

// Multiple returns
func divmod(a: int, b: int) (int, int) {
    return a / b, a % b
}

// Named returns
func parse(s: string) (value: int, ok: bool) {
    // ... 
    return  // returns (value, ok)
}

// Generic function
func max<T: Ord>(a: T, b: T) T {
    if a > b { a } else { b }
}

// Error returns
func read_file(path: string) !string {
    // Returns string or error
}
```

### Types

```goo
// Primitives
let i: i32 = -42
let u: u64 = 42
let f: f64 = 3.14
let b: bool = true
let c: char = 'A'
let s: string = "Hello"

// Arrays and Slices
let arr: [5]int = [1, 2, 3, 4, 5]
let slice: []int = arr[1:4]

// Optionals
let maybe: ?int = Some(42)
let nothing: ?int = None

// Maps
let map: map[string]int = {
    "one": 1,
    "two": 2,
}

// Tuples
let tuple: (int, string) = (42, "answer")
```

### Structs and Enums

```goo
// Struct definition
struct Point {
    x: f64
    y: f64
}

// Struct methods
impl Point {
    func new(x: f64, y: f64) Point {
        return Point { x, y }
    }
    
    func distance(self, other: Point) f64 {
        let dx = self.x - other.x
        let dy = self.y - other.y
        return sqrt(dx*dx + dy*dy)
    }
}

// Enum definition
enum Result<T, E> {
    Ok(T)
    Err(E)
}

// Enum matching
match result {
    Ok(value) => process(value),
    Err(error) => handle_error(error)
}
```

### Interfaces

```goo
// Interface definition
interface Reader {
    read(buf: []byte) !usize
}

// Default implementation
interface Iterator<T> {
    next() ?T
    
    // Default method
    collect() []T {
        mut result = []T{}
        while let Some(item) = self.next() {
            result.append(item)
        }
        return result
    }
}

// Implementation
impl Reader for File {
    func read(self, buf: []byte) !usize {
        // Implementation
    }
}
```

## Control Flow

### If Statements

```goo
// Basic if
if x > 0 {
    positive()
} else if x < 0 {
    negative()
} else {
    zero()
}

// If expression
let abs = if x >= 0 { x } else { -x }

// If-let
if let Some(value) = optional {
    use(value)
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

// For range
for i in 0..10 {
    print(i)  // 0 to 9
}

// For iterator
for item in collection {
    process(item)
}

// For with index
for i, item in collection.enumerate() {
    print(f"{i}: {item}")
}

// Labeled break
'outer: for x in 0..10 {
    for y in 0..10 {
        if x * y > 50 {
            break 'outer
        }
    }
}
```

### Match

```goo
// Basic match
match value {
    0 => zero(),
    1..=10 => small(),
    n if n % 2 == 0 => even(n),
    _ => other()
}

// Match with binding
match shape {
    Circle(radius) => PI * radius * radius,
    Rectangle(w, h) => w * h,
    Triangle(base, height) => base * height / 2
}

// Match guards
match point {
    Point{x, y} if x == y => print("On diagonal"),
    Point{x: 0, y} => print(f"On Y axis at {y}"),
    Point{x, y: 0} => print(f"On X axis at {x}"),
    Point{x, y} => print(f"At ({x}, {y})")
}
```

## Error Handling

### Error Returns

```goo
// Function that can error
func parse_int(s: string) !int {
    // Return error
    if s.is_empty() {
        return error("Empty string")
    }
    // Return success
    return 42
}

// Try operator
func process() !void {
    let n = try parse_int("123")
    let m = try parse_int("456")
    print(n + m)
}

// Error handling
let value = try risky() else err {
    match err {
        ParseError => return default_value(),
        IoError => panic!("IO failed"),
        _ => return err  // Propagate
    }
}

// Catch
let value = risky() catch 0  // Default on error
```

### Defer and Cleanup

```goo
// Defer runs at scope exit
func process_file() !void {
    let file = try open("data.txt")
    defer file.close()  // Runs even if error
    
    let data = try file.read_all()
    return process(data)
}

// Multiple defers (LIFO order)
func complex() {
    defer print("3")
    defer print("2")
    defer print("1")
    print("0")
    // Prints: 0 1 2 3
}
```

## Concurrency

### Goroutines

```goo
// Spawn goroutine
go print("async")

// With closure
let x = 42
go func() {
    print(f"x = {x}")
}()

// Supervised
supervised go risky_task()

// With handle
let handle = spawn {
    long_task()
}
handle.join()  // Wait for completion
```

### Channels

```goo
// Create channel
let ch = channel<int>()
let buffered = channel<string>(100)

// Send and receive
ch <- 42        // Send
let v = <-ch    // Receive

// Close channel
close(ch)

// Range over channel
for msg in ch {
    process(msg)
}

// Select
select {
    case v = <-ch1:
        handle1(v)
    case v = <-ch2:
        handle2(v)
    case ch3 <- msg:
        print("Sent")
    case <-timeout(1s):
        print("Timeout")
    default:
        print("No channel ready")
}
```

### Channel Patterns

```goo
// Pub/Sub
let pub = channel pub<Event>()
let sub = channel sub<Event>()

// Request/Reply
let req = channel req<Request>()
let rep = channel rep<Response>()

// Push/Pull (work distribution)
let push = channel push<Task>()
let pull = channel pull<Task>()

// Fan-out/Fan-in
let work = channel<Task>()
let results = channel<Result>()

for i in 0..workers {
    go worker(work, results)
}
```

### Actors

```goo
// Actor definition
actor Counter {
    mut count: int = 0
    
    handle Inc() {
        self.count += 1
    }
    
    handle Get() -> int {
        return self.count
    }
}

// Usage
let c = Counter.spawn()
c ! Inc()              // Fire-and-forget
let n = c ? Get()      // Request-reply
```

### Parallel Execution

```goo
// Parallel for
parallel for i in 0..1000 {
    process(data[i])
}

// With configuration
parallel {
    workers: 8,
    chunk_size: 100
} for item in items {
    expensive(item)
}

// Fork-join
let (a, b, c) = parallel {
    compute_a(),
    compute_b(),
    compute_c()
}

// Map-reduce
let sum = data
    |> parallel map(process)
    |> parallel reduce(0, +)
```

## Memory Management

### Allocators

```goo
// Define allocators
allocator arena = arena { size: 1MB }
allocator pool = pool { object_size: 64 }

// Use allocator
let data = alloc(1024, arena)
defer free(data, arena)

// Scoped allocation
scope(arena) {
    let temp = alloc(100)  // Uses arena
    // Automatically freed at scope end
}
```

### Smart Pointers

```goo
// Box (unique ownership)
let boxed = Box.new(42)

// Rc (reference counting)
let shared = Rc.new(data)
let clone = shared.clone()

// Arc (atomic RC)
let atomic = Arc.new(data)

// Weak references
let weak = Rc.downgrade(&shared)
if let Some(strong) = weak.upgrade() {
    use(strong)
}
```

## Compile-Time Features

### Comptime

```goo
// Compile-time execution
const SIZE = comptime {
    calculate_optimal_size()
}

// Compile-time type
func make_array<comptime N: usize>() [N]int {
    return [0; N]
}

// Conditional compilation
comptime if target.os == "linux" {
    import linux.specific
}

// Build configuration
comptime build {
    optimization = "speed"
    target = "native"
}
```

### Attributes

```goo
// Function attributes
@inline
func hot_path() { }

@no_alloc
func allocation_free() { }

@test
func test_something() {
    assert!(2 + 2 == 4)
}

@bench
func bench_algorithm(b: Bencher) {
    b.iter(|| expensive())
}

// Safety attributes
@no_panic
func safe_function() { }

@bounds_check("eliminate")
func optimized_loop() { }
```

## Standard Library

### I/O

```goo
import std.io

// Print
print("Hello")
println("World")

// Files
let content = try File.read_string("file.txt")
try File.write_string("out.txt", data)

// Stdin/Stdout
let line = try stdin.read_line()
stdout.write_all(bytes)
```

### Collections

```goo
import std.collections.*

// Vec
let mut v = vec[1, 2, 3]
v.push(4)
v.pop()

// HashMap
let mut m = map[string]int{}
m.insert("key", 42)
let value = m.get("key") ?? 0

// Set
let s = set[1, 2, 3]
if s.contains(2) { }
```

### Strings

```goo
import std.string

// String operations
let s = "hello"
let upper = s.to_uppercase()
let parts = s.split(",")

// Formatting
let msg = format!("x = {}, y = {}", x, y)
let hex = format!("{:x}", 255)  // "ff"
```

### Time

```goo
import std.time

// Duration
let timeout = Duration.seconds(30)

// Timing
let start = Instant.now()
expensive_operation()
let elapsed = start.elapsed()

// Date/Time
let now = DateTime.now()
let tomorrow = now + Duration.days(1)
```

## Common Patterns

### Builder Pattern

```goo
let server = Server.builder()
    .port(8080)
    .workers(4)
    .timeout(30s)
    .build()
```

### Result Chaining

```goo
let result = try load_file(path)
    |> try parse_json
    |> try validate
    |> try process
```

### Resource Management

```goo
// RAII pattern
using file = try File.open("data.txt") {
    let data = try file.read_all()
    process(data)
}  // File closed automatically
```

### Option Combinators

```goo
let name = user
    .map(|u| u.name)
    .filter(|n| !n.is_empty())
    .unwrap_or("Anonymous")
```

## Operators

### Arithmetic

```goo
+   // Addition
-   // Subtraction
*   // Multiplication
/   // Division
%   // Remainder
**  // Power
```

### Comparison

```goo
==  // Equal
!=  // Not equal
<   // Less than
<=  // Less or equal
>   // Greater than
>=  // Greater or equal
```

### Logical

```goo
&&  // AND
||  // OR
!   // NOT
```

### Bitwise

```goo
&   // AND
|   // OR
^   // XOR
~   // NOT
<<  // Left shift
>>  // Right shift
```

### Assignment

```goo
=   // Assign
+=  // Add assign
-=  // Subtract assign
*=  // Multiply assign
/=  // Divide assign
%=  // Remainder assign
```

### Other

```goo
..   // Range exclusive
..=  // Range inclusive
?    // Try operator
!    // Error return
<-   // Channel receive
->   // Return type
=>   // Match arm
::   // Path separator
.    // Member access
?.   // Optional chaining
??   // Null coalescing
```

## Keywords

```goo
actor      comptime   for        match      shared
as         const      func       module     struct
async      defer      go         mut        super
await      else       if         package    trait
break      enum       impl       pub        try
case       error      import     return     type
channel    export     interface  select     unsafe
continue   extern     let        self       use
```

## Type Inference

```goo
// Let compiler infer
let x = 42              // int
let y = 3.14            // f64
let z = "hello"         // string
let items = vec[1,2,3]  // vec<int>

// But can be explicit
let x: i32 = 42
let y: f64 = 3.14

// Generic inference
let v = Vec.new()       // Need type
let v = vec<string>[]   // Explicit
let v = vec["a", "b"]   // Inferred
```

## Safety

### Safe by Default

```goo
// All these are safe:
let arr = [1, 2, 3]
let x = arr[0]          // Bounds checked
let p = &arr[0]         // Lifetime tracked
shared data = Data{}    // Thread-safe

// Opt into unsafe
unsafe {
    let ptr = 0x1234 as *mut u8
    *ptr = 42
}
```

### Capabilities

```goo
// Restrict capabilities
@capabilities[FileRead, Network]
func limited_function() {
    let data = read_file("data.txt")  // OK
    send_network(data)                 // OK
    // delete_file("data.txt")         // ERROR: No FileWrite
}
```

This quick reference covers the essential Goo syntax and features. For detailed information, see the full language specification.
