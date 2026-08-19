# 🚀 Goo Programming Language

Welcome to **Goo** - a modern, safe, and expressive programming language that combines the best of Go's simplicity with advanced type system features for enhanced safety and expressiveness.

## 🌟 Key Features

### 1. 🚫 **Nullable Types** with `?T` syntax
```goo
var user ?User = findUser(id)
if user? {
    fmt.Printf("Found user: %s", user!.name)
} else {
    fmt.Println("User not found")
}
```

### 2. ❗ **Error Unions** with `!T` syntax
```goo
func divide(a, b int) (!int, !Error) {
    if b == 0 {
        return nil, Error("division by zero")
    }
    return a / b, nil
}

// Usage
result, err := divide(10, 2)
if err! {
    fmt.Printf("Error: %s", err!)
} else {
    fmt.Printf("Result: %d", result!)
}
```

### 3. 📡 **Channels & Concurrency** (Go-compatible)
```goo
ch := make(chan int, 10)

go func() {
    for i := 0; i < 5; i++ {
        ch <- i * 2
    }
    close(ch)
}()

for value := range ch {
    fmt.Printf("Received: %d\n", value)
}
```

### 4. 🎯 **Advanced Pattern Matching**
```goo
type Result<T, E> enum {
    Ok(T)
    Err(E)
}

result.Match(
    func(value int) string { return fmt.Sprintf("Success: %d", value) },
    func(error string) string { return fmt.Sprintf("Error: %s", error) }
)
```

### 5. 🧬 **Generic Interfaces**
```goo
interface Comparable<T> {
    Compare(other T) int
    Equal(other T) bool
}

func findMax<T Comparable<T>>(items []T) ?T {
    // Implementation with null safety
}
```

### 6. 📋 **Contract Programming**
```goo
contract SafeArray<T> {
    require len(arr) > 0
    require index >= 0 && index < len(arr)
    
    func Get(arr []T, index int) T {
        ensure result == arr[index]
        return arr[index]
    }
}
```

### 7. 🔢 **Dependent Types**
```goo
type BoundedInt<Min: int, Max: int> struct {
    value int where value >= Min && value <= Max
}

percentage, err := NewBoundedInt<0, 100>(75)
```

### 8. 🔒 **Memory Safety & Ownership**
```goo
var owned String = "Hello, ownership!"
var moved String = move(owned)  // owned is now invalid

func borrowString(s &String) {  // Borrowed reference
    fmt.Printf("Borrowed: %s", s)
}
```

## 🛠️ Getting Started

### Building Goo Programs

1. **Enhanced REPL with Syntax Highlighting:**
   ```bash
   make repl-enhanced
   ./bin/goo-repl-enhanced
   ```

2. **Main Compiler:**
   ```bash
   make lexer
   ./bin/goo program.goo
   ```

3. **Performance Dashboard:**
   ```bash
   make dashboard
   ./bin/goo-dashboard --demo
   ```

### Example Programs

- **`hello_world.goo`** - Basic introduction
- **`error_unions_demo.goo`** - Error handling patterns
- **`nullable_types_demo.goo`** - Null safety features
- **`channels_concurrency_demo.goo`** - Concurrent programming
- **`advanced_features_demo.goo`** - Advanced type system features
- **`interactive_demo.goo`** - Simple REPL examples

## 🎨 Development Tools

### Enhanced REPL Features:
- ✅ **Real-time syntax highlighting** with color-coded elements
- ✅ **Intelligent code completion** with context awareness
- ✅ **Goo-specific syntax support** (!, ?, <-)
- ✅ **Multiple themes** (default, dark, light)
- ✅ **Interactive commands** (help, syntax, completion)

### IDE Integration:
- ✅ **Language Server Protocol (LSP)** implementation
- ✅ **Debug Adapter Protocol (DAP)** for debugging
- ✅ **VS Code extension** with full language support
- ✅ **Syntax highlighting** and **semantic tokens**
- ✅ **Go-to-definition** and **find references**

### Debugging & Monitoring:
- ✅ **Time-travel debugging** with state snapshots
- ✅ **Performance monitoring** with real-time metrics
- ✅ **Hot reload** capabilities for rapid development
- ✅ **Enhanced error reporting** with detailed explanations

## 📖 Language Syntax Reference

### Variable Declarations
```goo
var x int = 42
var message string = "Hello, Goo!"
var user ?User = nil              // Nullable
var result !int = computeValue()  // Error union
```

### Function Definitions
```goo
// Basic function
func add(a, b int) int {
    return a + b
}

// Function with error union return
func safeDivide(a, b int) (!int, !Error) {
    if b == 0 {
        return nil, Error("division by zero")
    }
    return a / b, nil
}

// Generic function
func swap<T>(a, b T) (T, T) {
    return b, a
}
```

### Type Definitions
```goo
// Struct
type User struct {
    ID    int
    Name  string
    Email ?string
}

// Enum
type Status enum {
    Pending
    Active
    Inactive
}

// Generic type
type Container<T> struct {
    value T
    count int
}
```

### Control Flow
```goo
// If with null check
if user? {
    fmt.Printf("User: %s", user!.Name)
}

// If with error check
if err! {
    fmt.Printf("Error: %s", err!)
    return
}

// For loop
for i := 0; i < 10; i++ {
    fmt.Printf("%d ", i)
}

// Range over channel
for value := range ch {
    process(value)
}
```

## 🚀 Why Goo?

1. **🛡️ Safety First**: Null safety and error handling built into the type system
2. **⚡ Performance**: Zero-cost abstractions and compile-time optimizations  
3. **🧠 Expressiveness**: Advanced type features without complexity overhead
4. **🔄 Go Compatibility**: Familiar syntax with modern enhancements
5. **🛠️ Great Tooling**: Professional development environment out of the box
6. **📈 Scalability**: From prototypes to production systems

## 📚 Learn More

- **Examples Directory**: `examples/` - Comprehensive code samples
- **Documentation**: `docs/features/` - Detailed feature explanations
- **Test Suite**: `tests/` - Extensive test coverage demonstrating features
- **IDE Support**: `ide/vscode/` - VS Code extension for development

---

**Happy coding with Goo! 🎉**

*Build safer, more expressive programs with modern language features while maintaining the simplicity and performance you love.*