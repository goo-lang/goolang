# Goo Standard Library

## Overview

The Goo standard library provides a comprehensive, batteries-included set of packages that are safe, performant, and ergonomic. Every API is designed to be intuitive and to prevent common mistakes.

## Core Packages

### std.io - Input/Output

#### File Operations

```goo
import std.io.{File, BufReader, BufWriter}

// Simple file reading
let content = try File.read_string("config.json")

// Buffered reading for large files
let file = try File.open("large.txt")
defer file.close()

let reader = BufReader.new(file)
for line in reader.lines() {
    process(try line)
}

// Safe file writing with atomic operations
try File.write_string("output.txt", data) // Atomic write
try File.append_string("log.txt", entry)  // Atomic append

// Advanced file operations
let metadata = try File.metadata("data.bin")
print(f"Size: {metadata.size}, Modified: {metadata.modified}")

// Directory operations
for entry in try Dir.read("./src") {
    if entry.is_file() && entry.name.ends_with(".goo") {
        print(f"Found source: {entry.path}")
    }
}
```

#### Streams

```goo
import std.io.{Read, Write, Seek}

// Generic stream processing
func copy<R: Read, W: Write>(reader: R, writer: W) !usize {
    let mut buffer = [0u8; 8192]
    let mut total = 0
    
    loop {
        let n = try reader.read(&mut buffer)
        if n == 0 { break }
        
        try writer.write(&buffer[..n])
        total += n
    }
    
    return total
}

// Memory streams
let buffer = MemoryStream.new()
try write!(buffer, "Hello, {}", name)
let result = buffer.to_string()

// Compressed streams
let file = try File.create("data.gz")
let compressed = GzipWriter.new(file)
try compressed.write_all(data)
```

### std.collections - Data Structures

#### Dynamic Arrays (Vec)

```goo
import std.collections.Vec

// Type-safe dynamic arrays
let mut vec = Vec<int>.new()
vec.push(42)
vec.extend([1, 2, 3])

// Functional operations
let squared = vec.iter()
    .map(|x| x * x)
    .filter(|x| x > 10)
    .collect<Vec<int>>()

// Efficient operations
vec.reserve(1000)  // Pre-allocate capacity
vec.retain(|x| x % 2 == 0)  // Keep only even numbers

// Safe access
if let Some(first) = vec.first() {
    print(f"First: {first}")
}

let middle = vec.get(vec.len() / 2) ?? 0
```

#### Hash Maps

```goo
import std.collections.{HashMap, BTreeMap}

// Type-safe hash maps
let mut users = HashMap<UserId, User>.new()
users.insert(id, user)

// Entry API for efficient updates
users.entry(id)
    .and_modify(|u| u.last_seen = now())
    .or_insert(User.new(id))

// Pattern matching on lookups
match users.get(id) {
    Some(user) => process(user),
    None => create_default()
}

// Ordered maps
let mut sorted = BTreeMap<string, int>.new()
for (key, value) in sorted.iter() {
    print(f"{key}: {value}")  // Always in order
}
```

#### Sets

```goo
import std.collections.{HashSet, BTreeSet}

let mut visited = HashSet<NodeId>.new()
if !visited.contains(node) {
    visited.insert(node)
    process(node)
}

// Set operations
let a = HashSet.from([1, 2, 3])
let b = HashSet.from([2, 3, 4])

let union = a.union(&b)         // {1, 2, 3, 4}
let intersection = a.intersect(&b) // {2, 3}
let difference = a.difference(&b)  // {1}
```

#### Concurrent Collections

```goo
import std.collections.concurrent.*

// Thread-safe collections
let map = ConcurrentHashMap<string, int>.new()
parallel for i in 0..1000 {
    map.insert(f"key_{i}", i)
}

// Lock-free queue
let queue = ConcurrentQueue<Task>.new()
queue.push(task)
if let Some(task) = queue.pop() {
    process(task)
}

// Atomic reference counting
let shared = Arc<Data>.new(data)
let clone = shared.clone()
```

### std.string - String Manipulation

#### String Building

```goo
import std.string.{StringBuilder, format}

// Efficient string building
let mut sb = StringBuilder.new()
sb.append("Hello")
sb.append_char(' ')
sb.append("World")
sb.append_line("!")

// Formatting
let msg = format!("User {} logged in at {}", username, timestamp)

// Advanced formatting
let table = format!("{:<10} {:>10} {:^10}", "Left", "Right", "Center")
let hex = format!("Color: #{:06X}", rgb_value)
```

#### String Processing

```goo
import std.string.*

// Safe string operations
let trimmed = text.trim()
let upper = text.to_uppercase()
let words = text.split_whitespace().collect<Vec<string>>()

// Pattern matching
if text.starts_with("https://") {
    handle_url(text)
}

let index = text.find("needle") ?? text.len()

// Regular expressions (safe by default)
let re = Regex.compile(r"\d{4}-\d{2}-\d{2}")!
for match in re.find_all(text) {
    print(f"Found date: {match.as_str()}")
}

// Unicode-aware operations
let graphemes = text.graphemes().count()  // Correct character count
let normalized = text.normalize_nfc()      // Unicode normalization
```

### std.net - Networking

#### TCP

```goo
import std.net.{TcpListener, TcpStream}

// TCP server
let listener = try TcpListener.bind("127.0.0.1:8080")
print(f"Listening on {listener.local_addr()}")

for stream in listener.incoming() {
    go handle_client(try stream)
}

func handle_client(mut stream: TcpStream) {
    let mut buffer = [0u8; 1024]
    
    loop {
        match stream.read(&mut buffer) {
            Ok(0) => break,  // Connection closed
            Ok(n) => {
                try stream.write_all(&buffer[..n])
            },
            Err(e) => {
                print(f"Error: {e}")
                break
            }
        }
    }
}
```

#### HTTP Client

```goo
import std.net.http.{Client, Request, Method}

// Simple requests
let response = try http.get("https://api.example.com/data")
let json = try response.json<ApiResponse>()

// Advanced requests
let client = Client.builder()
    .timeout(30s)
    .max_redirects(5)
    .default_header("User-Agent", "Goo/1.0")
    .build()

let request = Request.builder()
    .method(Method.POST)
    .url("https://api.example.com/users")
    .json(&user)
    .bearer_auth(token)
    .build()

let response = try client.send(request)
```

#### HTTP Server

```goo
import std.net.http.{Server, Router, Request, Response}

// REST API server
let mut router = Router.new()

router.get("/users/:id", |req: Request| {
    let id = req.param("id")?.parse<UserId>()?
    let user = db.get_user(id)?
    Response.json(&user)
})

router.post("/users", |req: Request| {
    let user = req.json<User>()?
    let id = db.create_user(user)?
    Response.created(f"/users/{id}")
})

// Middleware
router.use(logging_middleware)
router.use(auth_middleware)
router.use(cors_middleware)

let server = Server.bind("0.0.0.0:3000")
    .router(router)
    .workers(num_cpus())
    .start()?
```

### std.json - JSON Processing

```goo
import std.json.{parse, stringify, Value}

// Automatic serialization/deserialization
#[derive(Serialize, Deserialize)]
struct User {
    id: UserId,
    name: string,
    email: string,
    #[serde(rename = "createdAt")]
    created_at: DateTime,
}

// Parse JSON
let user = try json.parse<User>(json_string)

// Generate JSON
let json_string = try json.stringify(&user)
let pretty = try json.stringify_pretty(&user, 2)

// Dynamic JSON
let value = try json.parse_value(input)
if let Some(name) = value.get("name")?.as_string() {
    print(f"Name: {name}")
}

// JSON streaming
let file = try File.open("large.json")
let stream = JsonStreamReader.new(file)
for result in stream {
    let record = try result
    process(record)
}
```

### std.time - Time and Date

```goo
import std.time.{Duration, Instant, DateTime}

// Durations
let timeout = Duration.seconds(30)
let precise = Duration.nanoseconds(123_456_789)

// Timing operations
let start = Instant.now()
expensive_operation()
let elapsed = start.elapsed()
print(f"Operation took {elapsed.as_millis()}ms")

// Date and time
let now = DateTime.now()
let tomorrow = now + Duration.days(1)
let formatted = now.format("%Y-%m-%d %H:%M:%S")

// Time zones
let utc = DateTime.now_utc()
let tokyo = utc.with_timezone("Asia/Tokyo")
let offset = tokyo.offset()

// Scheduling
timer.schedule_repeating(Duration.minutes(5), || {
    cleanup_task()
})
```

### std.sync - Synchronization

```goo
import std.sync.*

// Once - one-time initialization
static INIT: Once = Once.new()
static mut CONFIG: Option<Config> = None

INIT.call_once(|| {
    unsafe { CONFIG = Some(load_config()) }
})

// WaitGroup - coordinate goroutines
let wg = WaitGroup.new()

for i in 0..10 {
    wg.add(1)
    go func(id: int) {
        defer wg.done()
        process_item(id)
    }(i)
}

wg.wait()  // Wait for all to complete

// Channels with select
let (tx1, rx1) = channel<int>()
let (tx2, rx2) = channel<string>()

select {
    recv(rx1) -> msg => print(f"Got int: {msg}"),
    recv(rx2) -> msg => print(f"Got string: {msg}"),
    default => print("Nothing ready"),
}
```

### std.crypto - Cryptography

```goo
import std.crypto.{hash, hmac, aes, rsa}

// Hashing
let digest = hash.sha256(data)
let hex = digest.to_hex()

// HMAC
let mac = hmac.sha256(key, message)
if !hmac.verify(key, message, mac) {
    panic!("Invalid MAC")
}

// Symmetric encryption
let key = aes.generate_key()
let encrypted = try aes.encrypt(key, plaintext)
let decrypted = try aes.decrypt(key, encrypted)

// Asymmetric encryption
let (public, private) = rsa.generate_keypair(2048)
let signature = try rsa.sign(private, message)
let valid = try rsa.verify(public, message, signature)

// Secure random
let token = crypto.random_bytes(32)
let nonce = crypto.random_u64()
```

### std.os - Operating System Interface

```goo
import std.os.*

// Environment variables
let path = env.get("PATH") ?? "/usr/bin"
env.set("MY_VAR", "value")

// Process management
let output = try Command.new("ls")
    .args(["-la", "/tmp"])
    .output()

print(f"Status: {output.status}")
print(f"Stdout: {output.stdout}")

// Async process execution
let child = try Command.new("long_running")
    .stdin(Stdio.piped())
    .stdout(Stdio.piped())
    .spawn()

child.stdin.write_all(input)?
let output = child.wait_with_output()?

// System information
let info = SystemInfo.get()
print(f"OS: {info.os}")
print(f"Arch: {info.arch}")
print(f"CPUs: {info.cpu_count}")
print(f"Memory: {info.total_memory}")
```

### std.test - Testing Framework

```goo
import std.test.*

#[test]
func test_addition() {
    assert_eq!(2 + 2, 4)
    assert_ne!(2 + 2, 5)
}

#[test]
func test_with_context() !void {
    let ctx = TestContext.new()
    ctx.set_timeout(5s)
    
    let result = try async_operation()
    ctx.assert_true(result.is_valid())
    ctx.assert_contains(result.data, "expected")
}

#[bench]
func bench_sort(b: Bencher) {
    let data = generate_random_array(1000)
    
    b.iter(|| {
        let mut copy = data.clone()
        sort(&mut copy)
    })
}

// Property-based testing
#[property]
func prop_reverse_twice_is_identity(vec: Vec<int>) {
    let once = vec.clone().reverse()
    let twice = once.reverse()
    assert_eq!(vec, twice)
}

// Fuzzing
#[fuzz]
func fuzz_parser(data: []byte) {
    // Parser shouldn't crash on any input
    let _ = parse_document(data)
}
```

### std.math - Mathematics

```goo
import std.math.*

// Basic operations
let x = abs(-42)
let y = pow(2.0, 10.0)
let z = sqrt(16.0)

// Trigonometry
let angle = PI / 4
let s = sin(angle)
let c = cos(angle)

// Advanced math
let result = exp(log(x) + log(y))  // x * y via logarithms
let factorial = gamma(n + 1)        // n!

// Vector math
let v1 = Vec3.new(1.0, 2.0, 3.0)
let v2 = Vec3.new(4.0, 5.0, 6.0)
let dot = v1.dot(v2)
let cross = v1.cross(v2)
let normalized = v1.normalize()

// Matrix operations
let m1 = Matrix4.identity()
let m2 = Matrix4.rotation(angle, axis)
let m3 = m1 * m2

// Statistics
let data = [1.0, 2.0, 3.0, 4.0, 5.0]
let mean = stats.mean(data)
let stddev = stats.std_dev(data)
let median = stats.median(data)
```

## Advanced Packages

### std.async - Asynchronous Programming

```goo
import std.async.{Future, Stream, Task}

// Async functions
async func fetch_user(id: UserId) !User {
    let response = await http.get(f"/api/users/{id}")
    let user = await response.json<User>()
    return user
}

// Concurrent execution
async func fetch_users(ids: []UserId) ![]User {
    let futures = ids.map(|id| fetch_user(id))
    let results = await Future.all(futures)
    return results
}

// Streams
async func process_stream() {
    let stream = data_source().stream()
    
    await stream
        .map(|item| transform(item))
        .filter(|item| item.is_valid())
        .for_each(|item| save(item))
}

// Task spawning
let task = Task.spawn(async {
    let result = await expensive_computation()
    return result
})

let result = await task
```

### std.db - Database Interface

```goo
import std.db.{Connection, Query, Transaction}

// Connection management
let db = try Connection.connect("postgresql://localhost/mydb")
defer db.close()

// Simple queries
let users = try db.query<User>("SELECT * FROM users WHERE age > ?", 18)

// Prepared statements
let stmt = try db.prepare("INSERT INTO users (name, email) VALUES (?, ?)")
for user in new_users {
    try stmt.execute(user.name, user.email)
}

// Transactions
let tx = try db.begin_transaction()
try {
    try tx.execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", amount, from_id)
    try tx.execute("UPDATE accounts SET balance = balance + ? WHERE id = ?", amount, to_id)
    try tx.commit()
} catch error {
    try tx.rollback()
    return error
}

// Connection pooling
let pool = ConnectionPool.new()
    .max_connections(10)
    .connect_timeout(5s)
    .idle_timeout(30s)
    .build()

let conn = try pool.get_connection()
defer pool.return_connection(conn)
```

### std.graphics - Graphics and UI

```goo
import std.graphics.{Canvas, Color, Image}

// 2D Graphics
let canvas = Canvas.new(800, 600)

canvas.set_color(Color.RGB(255, 0, 0))
canvas.fill_rect(10, 10, 100, 50)

canvas.set_color(Color.RGBA(0, 255, 0, 128))
canvas.draw_circle(200, 200, 50)

// Image processing
let image = try Image.load("photo.jpg")
let thumbnail = image
    .resize(200, 200)
    .apply_filter(Filter.Gaussian(2.0))
    .crop(50, 50, 100, 100)

try thumbnail.save("thumb.jpg")

// Vector graphics
let svg = SvgBuilder.new()
    .rect(0, 0, 100, 100)
    .fill(Color.BLUE)
    .circle(50, 50, 25)
    .fill(Color.RED)
    .build()

try svg.save("graphic.svg")
```

### std.ai - AI and Machine Learning

```goo
import std.ai.{Model, Tensor, NeuralNetwork}

// Tensor operations
let a = Tensor.from([[1.0, 2.0], [3.0, 4.0]])
let b = Tensor.from([[5.0, 6.0], [7.0, 8.0]])
let c = a.matmul(b)

// Neural networks
let model = NeuralNetwork.builder()
    .layer(Dense.new(784, 128, Activation.ReLU))
    .layer(Dropout.new(0.2))
    .layer(Dense.new(128, 10, Activation.Softmax))
    .build()

// Training
let optimizer = Adam.new(learning_rate: 0.001)
let loss_fn = CrossEntropy.new()

for epoch in 0..100 {
    for (inputs, targets) in training_data {
        let predictions = model.forward(inputs)
        let loss = loss_fn.compute(predictions, targets)
        
        model.backward(loss)
        optimizer.step(model)
    }
}

// Inference
let prediction = model.predict(test_input)
```

## Design Principles

### 1. Safety First

- No unsafe operations in safe APIs
- Impossible to misuse APIs
- Clear error handling

### 2. Performance by Default

- Zero-cost abstractions
- Optimized implementations
- Minimal allocations

### 3. Ergonomic APIs

- Intuitive naming
- Consistent patterns
- Rich functionality

### 4. Comprehensive Documentation

- Every function documented
- Examples for common use cases
- Performance characteristics noted

### 5. Platform Abstraction

- Same API across platforms
- Platform-specific functionality clearly marked
- Graceful degradation

## Extension Model

```goo
// Standard library can be extended
extension Vec<T> {
    func second() ?T {
        self.get(1)
    }
    
    func shuffle(mut self, rng: RandomGen) {
        // Fisher-Yates shuffle
        for i in (0..self.len()).rev() {
            let j = rng.range(0..=i)
            self.swap(i, j)
        }
    }
}

// Now available on all Vec instances
let vec = vec![1, 2, 3, 4, 5]
if let Some(second) = vec.second() {
    print(f"Second element: {second}")
}
```

## Performance Guarantees

All standard library functions specify their performance characteristics:

```goo
/// Sorts the slice in-place using a stable sort algorithm.
/// 
/// # Performance
/// - Time: O(n log n) worst case
/// - Space: O(n) auxiliary space
/// - Stable: Equal elements maintain their relative order
/// 
/// For unstable sort with O(1) space, use `sort_unstable()`
func sort<T: Ord>(slice: []T) {
    // Implementation
}
```

## Package Organization

### Core Packages (Always Available)

- `std.collections` - Data structures
- `std.string` - String manipulation
- `std.io` - Input/output operations
- `std.os` - Operating system interface

### Standard Packages (Import Required)

- `std.net` - Networking
- `std.json` - JSON processing
- `std.time` - Time and date
- `std.crypto` - Cryptography
- `std.test` - Testing framework
- `std.math` - Mathematics

### Extended Packages (Optional)

- `std.async` - Asynchronous programming
- `std.db` - Database interface
- `std.graphics` - Graphics and UI
- `std.ai` - AI and machine learning

## Cross-Platform Support

The standard library provides consistent APIs across platforms:

```goo
// Same API on all platforms
let info = SystemInfo.get()

// Platform-specific features clearly marked
#[cfg(unix)]
func get_uid() uid_t {
    // Unix-specific implementation
}

#[cfg(windows)]
func get_user_sid() string {
    // Windows-specific implementation
}

// Graceful fallbacks
func get_user_home() string {
    #[cfg(unix)]
    return env.get("HOME") ?? "/tmp"
    
    #[cfg(windows)]
    return env.get("USERPROFILE") ?? "C:\\temp"
}
```

## Summary

The Goo standard library provides:

1. **Comprehensive**: Everything needed for modern applications
2. **Safe**: Impossible to misuse, clear error handling
3. **Fast**: Optimized implementations, zero-cost abstractions
4. **Ergonomic**: Intuitive APIs, consistent patterns
5. **Well-documented**: Examples, performance notes, platform details
6. **Extensible**: Easy to add functionality through extensions
7. **Cross-platform**: Consistent behavior across operating systems

The goal is to make the common case easy and the complex case possible, all while maintaining safety and performance.