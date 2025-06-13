# Goo Standard Library Foundation

## Overview

The Goo Standard Library provides essential functionality for building robust applications while respecting Goo's unique language features including ownership semantics, error unions, nullable types, and memory safety.

## Implementation Status: ✅ COMPLETED

Task #16 - "Implement Standard Library Foundation" has been successfully implemented with the following components:

## Core Modules Implemented

### 1. I/O Module (`io` package)

The I/O module provides fundamental interfaces and operations for input/output operations.

#### Key Interfaces

```goo
// Reader interface for reading data
type Reader interface {
    Read(buf: []byte) -> (n: int, err: !IOError)
}

// Writer interface for writing data
type Writer interface {
    Write(data: []byte) -> (n: int, err: !IOError)
}

// Combined interfaces
type ReadWriter interface {
    Reader
    Writer
}
```

#### Core Functions

- `Copy(dst: Writer, src: Reader)` - Copy data from source to destination
- `ReadAll(reader: Reader)` - Read all data from a reader
- `Pipe()` - Create a synchronous in-memory pipe
- `MultiReader(readers: ...Reader)` - Combine multiple readers
- `TeeReader(reader: Reader, writer: Writer)` - Read and simultaneously write

#### Features

- **Error Handling**: Uses error unions (`!IOError`) for safe error propagation
- **Memory Safety**: Respects ownership semantics
- **Efficiency**: Buffered operations and optimal memory usage
- **Flexibility**: Composable interfaces for complex I/O patterns

### 2. Formatted I/O Module (`fmt` package)

Provides formatted input/output functionality similar to C's printf family.

#### Core Functions

```goo
// Print to stdout
func Print(args: ...any) -> (n: int, err: !io.IOError)
func Printf(format: string, args: ...any) -> (n: int, err: !io.IOError)
func Println(args: ...any) -> (n: int, err: !io.IOError)

// Format to string
func Sprint(args: ...any) -> string
func Sprintf(format: string, args: ...any) -> (result: string, err: !FormatError)
func Sprintln(args: ...any) -> string

// Write to custom writer
func Fprint(w: io.Writer, args: ...any) -> (n: int, err: !io.IOError)
func Fprintf(w: io.Writer, format: string, args: ...any) -> (n: int, err: !io.IOError)
```

#### Format Verbs Supported

- `%d`, `%o`, `%x`, `%X` - Integer formatting (decimal, octal, hex)
- `%f`, `%F`, `%e`, `%E`, `%g`, `%G` - Floating point formatting
- `%s` - String formatting
- `%c` - Character formatting
- `%t` - Boolean formatting
- `%v` - Default format
- `%q` - Quoted string
- `%T` - Type information

#### Usage Examples

```goo
import "fmt"

// Basic formatting
fmt.Printf("Hello %s, you are %d years old\n", "Alice", 30)

// Error handling with format errors
result, err := fmt.Sprintf("Value: %d", 42)
if err != nil {
    fmt.Printf("Format error: %s\n", err.Error())
}

// Custom formatting with interfaces
type Person struct {
    name: string
    age: int
}

func (p: Person) String() -> string {
    return fmt.Sprintf("%s (%d)", p.name, p.age)
}
```

### 3. String Utilities Module (`strings` package)

Comprehensive string manipulation functionality with UTF-8 support.

#### Core Functions

**String Searching and Testing:**
```goo
func Contains(s: string, substr: string) -> bool
func HasPrefix(s: string, prefix: string) -> bool
func HasSuffix(s: string, suffix: string) -> bool
func Index(s: string, substr: string) -> int
func LastIndex(s: string, substr: string) -> int
```

**String Transformation:**
```goo
func ToLower(s: string) -> string
func ToUpper(s: string) -> string
func Trim(s: string, cutset: string) -> string
func TrimSpace(s: string) -> string
func Replace(s: string, old: string, new: string, n: int) -> string
func ReplaceAll(s: string, old: string, new: string) -> string
```

**String Splitting and Joining:**
```goo
func Split(s: string, sep: string) -> []string
func Join(elems: []string, sep: string) -> string
func Fields(s: string) -> []string
```

#### String Builder

Efficient string building with minimal allocations:

```goo
builder := strings.NewBuilder()
builder.WriteString("Hello")
builder.WriteString(" ")
builder.WriteString("World")
result := builder.String() // "Hello World"
```

#### String Reader

Read from strings as if they were files:

```goo
reader := strings.NewReader("Hello, World!")
buffer := make([]byte, 5)
n, err := reader.Read(buffer)
// buffer now contains "Hello"
```

### 4. Dynamic Array Collection (`vec` package)

A growable array implementation with strong ownership semantics.

#### Core Type

```goo
type Vec[T any] struct {
    // Internal implementation with ownership tracking
}
```

#### Creation and Basic Operations

```goo
// Create new Vec
v := vec.New[int]()
v_with_cap := vec.NewWithCapacity[int](100)
v_with_values := vec.WithValues(1, 2, 3, 4, 5)

// Basic operations
v.Push(42)                    // Add element
value, err := v.Pop()         // Remove last element
length := v.Len()             // Get length
is_empty := v.IsEmpty()       // Check if empty
```

#### Element Access

```goo
// Safe element access
elem, err := v.Get(index)           // Borrowed reference
mut_elem, err := v.GetMut(index)    // Mutable reference
err := v.Set(index, value)          // Set value

// Bounds checking
first, err := v.First()             // First element
last, err := v.Last()               // Last element
```

#### Advanced Operations

```goo
// Insertion and removal
v.Insert(index, value)              // Insert at index
removed, err := v.Remove(index)     // Remove at index
v.SwapRemove(index)                 // Remove by swapping with last

// Collection operations
v.Extend(&other_vec)                // Extend with another Vec
v.ExtendFromSlice(slice)            // Extend with slice
clone := v.Clone()                  // Deep copy

// Iteration
v.ForEach(func(elem: &T) {
    // Process each element
})
```

#### Ownership and Memory Safety

```goo
// Ownership transfer
owned_vec := vec.WithValues(1, 2, 3)
assert(owned_vec.IsOwned())

// Borrowing
slice := owned_vec.AsSlice()        // Borrowed view
mut_slice, err := owned_vec.AsMutSlice()  // Mutable borrowed view

// Memory management
v.Reserve(additional_capacity)      // Ensure capacity
v.Shrink()                         // Reduce capacity to fit
v.Clear()                          // Remove all elements
```

### 5. Hash Map Collection (`map` package)

A hash table implementation with ownership semantics and type safety.

#### Core Type

```goo
type Map[K comparable, V any] struct {
    // Internal hash table implementation
}
```

#### Creation and Basic Operations

```goo
// Create new Map
m := map.New[string, int]()
m_with_cap := map.NewWithCapacity[string, int](100)

// From key-value pairs
pairs := []map.Pair[string, int]{
    {Key: "a", Value: 1},
    {Key: "b", Value: 2},
}
m_from_pairs := map.FromPairs(pairs)
```

#### Element Operations

```goo
// Insertion and retrieval
old_val, err := m.Insert("key", 42)  // Returns old value if key existed
value, err := m.Get("key")           // Get value
exists := m.Contains("key")          // Check existence

// References
val_ref, err := m.GetRef("key")      // Borrowed reference
mut_ref, err := m.GetMutRef("key")   // Mutable reference

// Removal
removed, err := m.Remove("key")      // Remove and return value
```

#### Advanced Operations

```goo
// Collection operations
keys := m.Keys()                     // Vec of all keys
values := m.Values()                 // Vec of all values
pairs := m.Pairs()                   // Vec of key-value pairs

// Iteration
m.ForEach(func(key: &K, value: &V) {
    // Process each key-value pair
})

// Bulk operations
m.Extend(&other_map)                 // Add all from another map
m.Retain(func(k: K, v: V) -> bool {  // Keep only matching pairs
    return some_condition
})
```

#### Entry API

Advanced entry-based operations:

```goo
entry := m.Entry("key")
if entry.Exists() {
    value := entry.Get()
} else {
    entry.Set(default_value)
}

// Or use convenience methods
old_val, err := entry.OrInsert(default_value)
removed, err := entry.Remove()
```

#### Performance Features

```goo
// Load factor and capacity management
load_factor := m.CurrentLoadFactor()
capacity := m.Capacity()

// Automatic resizing based on load factor
// Efficient hash function for different key types
```

### 6. Operating System Interface (`os` package)

Platform-independent interface to operating system functionality.

#### File Operations

```goo
// File creation and opening
file, err := os.Open("filename.txt")           // Read-only
file, err := os.Create("filename.txt")         // Create/truncate
file, err := os.OpenFile("file", flags, perm)  // Custom flags

// File I/O
n, err := file.Read(buffer)
n, err := file.Write(data)
new_pos, err := file.Seek(offset, whence)
err := file.Close()

// File information
info, err := os.Stat("filename.txt")
info, err := file.Stat()
```

#### Directory Operations

```goo
// Directory management
err := os.Mkdir("dirname", 0755)
err := os.MkdirAll("path/to/dir", 0755)
entries, err := os.ReadDir("dirname")

// Working directory
wd, err := os.Getwd()
err := os.Chdir("new/directory")
```

#### Process Management

```goo
// Process information
pid := os.Getpid()
ppid := os.Getppid()

// Process creation
process, err := os.StartProcess("program", args, &proc_attr)
state, err := process.Wait()
err := process.Kill()

// Signal handling
err := process.Signal(os.SIGTERM)
```

#### Environment Variables

```goo
// Environment access
value := os.Getenv("PATH")              // Returns ?string
value, ok := os.LookupEnv("HOME")       // Returns (string, bool)
err := os.Setenv("VAR", "value")
err := os.Unsetenv("VAR")
all_env := os.Environ()
```

#### System Information

```goo
// User and system info
uid := os.Getuid()
gid := os.Getgid()
hostname, err := os.Hostname()
home_dir, err := os.UserHomeDir()
temp_dir := os.TempDir()
```

## Language Integration Features

### 1. Ownership Semantics

All standard library components respect Goo's ownership model:

```goo
// Ownership transfer
owned_vec := vec.WithValues(1, 2, 3)
// vec_data ownership moved to map
m.Insert("data", owned_vec)

// Borrowing
borrowed_ref, err := m.GetRef("data")
// borrowed_ref cannot outlive m and cannot be modified

// Mutable borrowing
mut_ref, err := m.GetMutRef("data")
// mut_ref allows modification but prevents other access
```

### 2. Error Handling with Error Unions

All fallible operations use error unions:

```goo
// Error propagation
func process_file(path: string) -> !io.IOError {
    file, err := try os.Open(path)
    defer file.Close()
    
    data, err := try io.ReadAll(file)
    return process_data(data)
}

// Error handling
result := process_file("data.txt") catch |err| {
    fmt.Printf("Error: %s\n", err.Error())
    return default_result
}
```

### 3. Nullable Types

Optional values are handled with nullable types:

```goo
// Environment variables return nullable strings
home := os.Getenv("HOME")
if home != nil {
    fmt.Printf("Home directory: %s\n", *home)
} else {
    fmt.Println("HOME not set")
}

// Safe unwrapping with if-let
if let home_dir = os.Getenv("HOME") {
    fmt.Printf("Home: %s\n", home_dir)
}
```

### 4. Memory Safety

Automatic memory management with ownership tracking:

```goo
// No manual memory management needed
large_data := vec.NewWithCapacity[byte](1024 * 1024)
large_data.ExtendFromSlice(file_data)
// Memory automatically freed when large_data goes out of scope

// Safe iteration without iterator invalidation
for item := large_data.Iter().Next(); item != nil; item = large_data.Iter().Next() {
    process(*item)
}
```

## Performance Characteristics

### Vec Performance
- **Amortized O(1)** push operations
- **O(1)** access by index
- **O(n)** insertion/deletion in middle
- **Automatic capacity management** with configurable growth

### Map Performance
- **Average O(1)** insertion, lookup, deletion
- **Automatic load factor management** (default 75%)
- **Efficient hash functions** for built-in types
- **Separate chaining** for collision resolution

### String Operations
- **UTF-8 aware** string processing
- **Efficient builder pattern** for concatenation
- **Zero-copy operations** where possible
- **Optimized search algorithms**

## Usage Examples

### File Processing Pipeline

```goo
import "os"
import "io"
import "strings"
import "fmt"

func process_log_file(filename: string) -> !os.OSError {
    // Open file
    file, err := try os.Open(filename)
    defer file.Close()
    
    // Read all content
    content, err := try io.ReadAll(file)
    
    // Process lines
    lines := strings.Split(string(content), "\n")
    error_lines := vec.New[string]()
    
    for line := range lines {
        if strings.Contains(line, "ERROR") {
            error_lines.Push(line)
        }
    }
    
    // Output results
    fmt.Printf("Found %d error lines:\n", error_lines.Len())
    error_lines.ForEach(func(line: &string) {
        fmt.Printf("  %s\n", *line)
    })
    
    return nil
}
```

### Data Analysis

```goo
import "strings"
import "map"
import "vec"
import "fmt"

func analyze_word_frequency(text: string) -> map.Map[string, int] {
    word_count := map.New[string, int]()
    
    // Split into words
    words := strings.Fields(strings.ToLower(text))
    
    // Count frequency
    for word := range words {
        // Clean word (remove punctuation)
        clean_word := strings.Trim(word, ".,!?;:")
        
        // Update count
        current, _ := word_count.Get(clean_word)
        if current != nil {
            word_count.Insert(clean_word, *current + 1)
        } else {
            word_count.Insert(clean_word, 1)
        }
    }
    
    return word_count
}

func print_top_words(word_count: &map.Map[string, int], top_n: int) {
    // Convert to sorted pairs
    pairs := word_count.Pairs()
    
    // Sort by count (simplified - would use proper sorting)
    // ... sorting implementation ...
    
    fmt.Printf("Top %d words:\n", top_n)
    for i := 0; i < min(top_n, pairs.Len()); i++ {
        pair, _ := try pairs.Get(i)
        fmt.Printf("%s: %d\n", pair.Key, pair.Value)
    }
}
```

### Network Data Processing

```goo
import "fmt"
import "strings"
import "vec"
import "map"

func process_server_logs(log_data: string) {
    lines := strings.Split(log_data, "\n")
    ip_counts := map.New[string, int]()
    status_codes := map.New[int, int]()
    
    for line := range lines {
        // Parse log line: IP - - [date] "request" status size
        fields := strings.Fields(line)
        if len(fields) >= 9 {
            ip := fields[0]
            status_str := fields[8]
            
            // Count IPs
            current_ip, _ := ip_counts.Get(ip)
            if current_ip != nil {
                ip_counts.Insert(ip, *current_ip + 1)
            } else {
                ip_counts.Insert(ip, 1)
            }
            
            // Count status codes
            if status, ok := parse_int(status_str); ok {
                current_status, _ := status_codes.Get(status)
                if current_status != nil {
                    status_codes.Insert(status, *current_status + 1)
                } else {
                    status_codes.Insert(status, 1)
                }
            }
        }
    }
    
    // Report results
    fmt.Printf("Unique IPs: %d\n", ip_counts.Size())
    fmt.Printf("Status code distribution:\n")
    status_codes.ForEach(func(code: &int, count: &int) {
        fmt.Printf("  %d: %d requests\n", *code, *count)
    })
}
```

## Best Practices

### 1. Error Handling

```goo
// Always handle errors explicitly
file, err := os.Open("config.txt")
if err != nil {
    match err.kind {
        case os.OSErrorKind.NotFound:
            // Create default config
            return create_default_config()
        case os.OSErrorKind.PermissionDenied:
            return fmt.Errorf("Permission denied: %s", err.message)
        default:
            return err
    }
}

// Use try for error propagation
func load_config() -> !ConfigError {
    file, err := try os.Open("config.txt")
    defer file.Close()
    
    data, err := try io.ReadAll(file)
    return parse_config(data)
}
```

### 2. Memory Management

```goo
// Pre-allocate when size is known
data := vec.NewWithCapacity[Record](expected_count)

// Use borrowing for read-only access
func process_data(records: &vec.Vec[Record]) {
    records.ForEach(func(record: &Record) {
        // Process without taking ownership
    })
}

// Clear collections when done
large_map.Clear()  // Free memory early
```

### 3. Performance Optimization

```goo
// Use string builder for concatenation
builder := strings.NewBuilder()
for item := range items {
    builder.WriteString(item.ToString())
    builder.WriteString("\n")
}
result := builder.String()

// Batch operations when possible
file_data := vec.New[byte]()
for chunk := range data_chunks {
    file_data.ExtendFromSlice(chunk)
}
// Write once instead of multiple small writes
file.Write(file_data.AsSlice())
```

### 4. Type Safety

```goo
// Use specific types for different concepts
type UserID int
type ProductID int

user_map := map.New[UserID, User]()
product_map := map.New[ProductID, Product]()

// Type system prevents mixing up IDs
// user_map.Get(ProductID(123))  // Compile error!
```

## Testing and Validation

The standard library includes comprehensive test coverage:

- **Unit Tests**: Each module thoroughly tested
- **Integration Tests**: Cross-module functionality verified
- **Performance Tests**: Scalability and efficiency validated
- **Memory Safety Tests**: Ownership semantics verified
- **Error Handling Tests**: Error propagation and recovery tested

Run tests with:
```bash
goo test tests/test_stdlib_comprehensive.goo
```

## Future Enhancements

Planned additions to the standard library:

1. **Network Programming**: HTTP client/server, WebSocket support
2. **Cryptography**: Hashing, encryption, digital signatures
3. **Time and Date**: Advanced time manipulation and formatting
4. **Regular Expressions**: Pattern matching and text processing
5. **JSON/XML**: Serialization and deserialization
6. **Compression**: Data compression algorithms
7. **Database**: SQL database connectivity
8. **Logging**: Structured logging framework

## Conclusion

The Goo Standard Library Foundation provides a solid base for application development with:

- **Type Safety**: Strong typing with ownership semantics
- **Memory Safety**: Automatic memory management without garbage collection
- **Error Safety**: Explicit error handling with error unions
- **Performance**: Efficient algorithms and data structures
- **Usability**: Clean, intuitive APIs
- **Reliability**: Comprehensive testing and validation

The implementation demonstrates how modern system programming languages can combine safety, performance, and usability in a standard library that feels familiar to developers coming from other languages while providing unique benefits through Goo's advanced type system.