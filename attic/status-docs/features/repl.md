# Goo Interactive REPL

The Goo Interactive Read-Eval-Print Loop (REPL) provides a powerful development environment for exploring the Goo language, testing expressions, and developing code interactively.

## Overview

The REPL integrates with the compiler's type system to provide rich type information and supports hot reload for live code updates during development.

## Features

### 1. Interactive Expression Evaluation

```goo
goo> 2 + 3
int: 5

goo> name := "World"
name: string = "World"

goo> greeting := "Hello, " + name
greeting: string = "Hello, World"
```

### 2. Type Information Display

```goo
goo> :type 42
int

goo> :type [1, 2, 3]
[]int

goo> :type func(x: int) int { return x * 2 }
func(int) int
```

### 3. Multi-line Input Support

```goo
goo> if true {
...     fmt.Println("Hello")
...     fmt.Println("World")
... }
Hello
World
```

### 4. Variable Persistence

Variables defined in the REPL persist across evaluations:

```goo
goo> counter := 0
counter: int = 0

goo> counter++
void

goo> counter
int: 1
```

### 5. Command System

The REPL provides commands for introspection and control:

- `:help` - Show available commands
- `:type <expr>` - Show type information for expression
- `:scope` - Show current scope information
- `:bindings` - List all variables in scope
- `:history [n]` - Show recent evaluation history
- `:mode <mode>` - Set evaluation mode
- `:reset` - Reset REPL state
- `:reload` - Reload changed modules
- `:clear` - Clear screen
- `:exit` - Exit REPL

### 6. Hot Reload Integration

The REPL integrates with the hot reload system for live development:

```goo
goo> :reload
Checking for changed modules...
Hot reload completed successfully

goo> // Functions are automatically reloaded when files change
```

### 7. Multiple Evaluation Modes

- **Normal Mode**: Standard expression evaluation
- **Multiline Mode**: Always accept multiline input
- **Debug Mode**: Show additional debugging information
- **Type-only Mode**: Show only type information without evaluation

## Usage

### Starting the REPL

```bash
# Interactive mode
goo-repl

# Evaluate single expression
goo-repl -e "2 + 3"

# With options
goo-repl --no-color --no-timing
```

### Command Line Options

- `-h, --help` - Show help message
- `-e, --eval EXPR` - Evaluate expression and exit
- `--no-color` - Disable color output
- `--no-types` - Disable type information display
- `--no-timing` - Disable execution timing
- `--history-size N` - Set history size (default: 1000)

### Basic Session

```goo
Goo Interactive REPL
Type expressions to evaluate them, or :help for commands

goo> x := 42
x: int = 42

goo> y := x * 2
y: int = 84

goo> :type x + y
int

goo> result := x + y
result: int = 126

goo> :history 3
[1] x := 42
    => 42 (0.05ms)
[2] y := x * 2
    => 84 (0.03ms)
[3] result := x + y
    => 126 (0.02ms)

goo> :bindings
Current variable bindings:
  x: int
  y: int
  result: int

goo> :exit
Goodbye!
```

## Implementation Architecture

### Core Components

1. **REPLContext**: Main REPL state management
2. **REPLValue**: Runtime value representation
3. **REPLHistory**: Session history tracking
4. **Type Integration**: Direct integration with type checker
5. **Hot Reload Integration**: Live code updates

### Value System

The REPL supports these value types:
- Integers (`REPL_VALUE_INT`)
- Floats (`REPL_VALUE_FLOAT`) 
- Strings (`REPL_VALUE_STRING`)
- Booleans (`REPL_VALUE_BOOL`)
- Null values (`REPL_VALUE_NULL`)
- Errors (`REPL_VALUE_ERROR`)
- Complex types (`REPL_VALUE_COMPLEX`)

### Command Processing

Commands are parsed and executed through:
1. Command recognition (`:command`)
2. Argument parsing
3. Command-specific handlers
4. Result formatting and display

### Expression Evaluation Pipeline

1. **Input Parsing**: Parse input into AST
2. **Type Checking**: Verify types and infer result types
3. **Evaluation**: Execute or simulate execution
4. **Result Display**: Format and display results with type info
5. **History Update**: Add to session history

## Integration with IDE Features

### Hot Reload

The REPL automatically integrates with hot reload:
- Watches for file changes
- Reloads functions and types automatically
- Preserves REPL session state across reloads

### Type Information

Deep integration with the type system provides:
- Real-time type inference
- Type hierarchy display
- Constraint and concept information
- Error type analysis

### Development Workflow

The REPL supports rapid development:
1. Write code in editor
2. Test in REPL with hot reload
3. Iterate quickly with immediate feedback
4. Build confidence before committing changes

## Advanced Features

### State Preservation

The REPL maintains state across:
- Variable definitions
- Function definitions (when supported)
- Type definitions
- Module imports

### Error Handling

Comprehensive error reporting:
- Parse errors with position information
- Type checking errors
- Runtime errors (when evaluation is supported)
- Graceful recovery from errors

### Performance Monitoring

Built-in performance tracking:
- Execution time measurement
- Memory usage monitoring (planned)
- Performance statistics

### Completion and Hints

Auto-completion support for:
- Variable names
- Function names
- Type names
- Keywords
- Commands

## Configuration

### REPL Settings

Configure REPL behavior:
- Color output enable/disable
- Type information display
- Timing information
- History size limits
- Auto-completion behavior

### Integration Settings

Configure integration features:
- Hot reload watching
- File patterns to watch
- Module loading behavior

## Examples

### Type Exploration

```goo
goo> :type struct { name: string; age: int }
struct{name: string, age: int}

goo> person := struct{name: "Alice", age: 30}
person: struct{name: string, age: int} = {name: "Alice", age: 30}

goo> :type person.name
string
```

### Function Development

```goo
goo> func double(x: int) int { return x * 2 }
func double(int) int

goo> double(21)
int: 42

goo> :type double
func(int) int
```

### Error Handling

```goo
goo> func divide(a: int, b: int) !int {
...     if b == 0 {
...         return error("division by zero")
...     }
...     return a / b
... }
func divide(int, int) !int

goo> divide(10, 2)
!int: 5

goo> divide(10, 0)
!int: Error: division by zero
```

## Best Practices

### Effective REPL Usage

1. **Start Small**: Begin with simple expressions
2. **Use Type Commands**: Leverage `:type` for understanding
3. **Explore Incrementally**: Build complexity gradually
4. **Save Important Work**: Copy successful code to files
5. **Use History**: Reference previous evaluations

### Development Workflow

1. **Prototype in REPL**: Test ideas quickly
2. **Verify Types**: Use type commands to understand interfaces
3. **Hot Reload Testing**: Test changes without restarting
4. **Iterative Development**: Refine code through multiple cycles

### Debugging Tips

1. **Check Types First**: Use `:type` to verify expectations
2. **Use Debug Mode**: Enable for additional information
3. **Review History**: Check previous successful evaluations
4. **Reset When Stuck**: Use `:reset` for clean state

## Limitations

### Current Limitations

1. **Expression-Only**: Currently limited to expressions
2. **No Statement Support**: Control flow statements not yet supported
3. **Limited Evaluation**: Type checking only, no actual execution
4. **No Package System**: Module system integration incomplete

### Future Enhancements

1. **Full Statement Support**: If, for, while statements
2. **Function Definitions**: Define functions in REPL
3. **Type Definitions**: Define custom types
4. **Module System**: Import and use packages
5. **Debugger Integration**: Step-through debugging
6. **Performance Profiling**: Detailed performance analysis
7. **Code Completion**: Intelligent auto-completion
8. **Syntax Highlighting**: Color-coded input

## Troubleshooting

### Common Issues

1. **Parse Errors**: Check syntax carefully
2. **Type Errors**: Use `:type` to verify expectations
3. **Unknown Commands**: Type `:help` for command list
4. **State Issues**: Use `:reset` to clear state

### Error Recovery

The REPL is designed to recover gracefully from errors:
- Parse errors don't crash the session
- Type errors are reported clearly
- State remains consistent after errors
- History preserves successful evaluations

### Getting Help

- Use `:help` for command reference
- Check type information with `:type`
- Review session history with `:history`
- Reset state with `:reset` if needed